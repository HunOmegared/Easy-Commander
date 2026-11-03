// Easy Commander host - read-only file host for DOS clients over plain TCP.
//
// Protocol (text commands, CRLF-terminated lines):
//
//	HELLO            -> OK EASYCOMMANDER 1
//	LIST <path>      -> OK <n>, then n lines: <D|F>\t<NAME>\t<size>\t<YYYYMMDD>\t<HHMM>
//	GET <path> [offset [length]]
//	                 -> OK <n> <filesize>, then exactly <n> raw bytes, where n is
//	                    the part of the file from offset (at most length bytes)
//	TREE <path>      -> OK <n> <totalbytes>, then n lines like LIST, but with
//	                    paths relative to <path> (backslash-separated), parents
//	                    listed before their contents; empty folders included
//	CRC <path>       -> OK <size> <crc32 as 8 hex digits>   (files only)
//	QUIT             -> OK Bye
//	errors           -> ERR <code> <message>
//
// The client only ever sees DOS 8.3 names. The host maps long names to
// 8.3 names deterministically, so no state has to be stored.
package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"hash/crc32"
	"io"
	"io/fs"
	"log"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	protoVersion = 1
	maxLine      = 2048
	maxClients   = 8
	idleTimeout  = 10 * time.Minute
	writeTimeout = 60 * time.Second
)

// ---------------------------------------------------------------------
// Protocol errors

type protoErr struct {
	code int
	msg  string
}

func (e protoErr) Error() string { return e.msg }

var (
	errBad      = protoErr{400, "Bad request"}
	errNotFound = protoErr{404, "Not found"}
	errNotDir   = protoErr{405, "Not a directory"}
	errIsDir    = protoErr{406, "Is a folder, use TREE"}
	errDenied   = protoErr{403, "Cannot open file"}
	errInternal = protoErr{500, "Internal error"}
	errBusy     = protoErr{503, "Busy"}
)

func writeErr(w *bufio.Writer, err error) {
	var pe protoErr
	if !errors.As(err, &pe) {
		pe = errInternal
	}
	fmt.Fprintf(w, "ERR %d %s\r\n", pe.code, pe.msg)
	w.Flush()
}

// ---------------------------------------------------------------------
// DOS 8.3 name mapping

type entry struct {
	Real string
	DOS  string
	Dir  bool
	Size int64
	Mod  time.Time
}

const dosPunct = "!#$%&'()-@^_`{}~"

func isDOSChar(c byte) bool {
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strings.IndexByte(dosPunct, c) >= 0
}

func allDOS(s string) bool {
	for i := 0; i < len(s); i++ {
		if !isDOSChar(s[i]) {
			return false
		}
	}
	return true
}

// nativeDOS reports whether name is already a valid 8.3 name
// (case-insensitively) and returns its upper-case form.
func nativeDOS(name string) (string, bool) {
	up := strings.ToUpper(name)
	base, ext := up, ""
	if i := strings.IndexByte(up, '.'); i >= 0 {
		base, ext = up[:i], up[i+1:]
		if ext == "" || strings.Contains(ext, ".") {
			return "", false
		}
	}
	if len(base) < 1 || len(base) > 8 || len(ext) > 3 || !allDOS(base) || !allDOS(ext) {
		return "", false
	}
	return up, true
}

// sanitize upper-cases s, drops spaces and dots, replaces anything that is
// not a legal DOS character with '_' and truncates to max characters.
func sanitize(s string, max int) string {
	var b strings.Builder
	for _, r := range strings.ToUpper(s) {
		if r == ' ' || r == '.' {
			continue
		}
		if r < 128 && isDOSChar(byte(r)) {
			b.WriteByte(byte(r))
		} else {
			b.WriteByte('_')
		}
		if b.Len() >= max {
			break
		}
	}
	return b.String()
}

// mangle builds a BASE~N.EXT name that is not yet in used.
func mangle(name string, used map[string]bool) string {
	base, ext := name, ""
	if i := strings.LastIndexByte(name, '.'); i > 0 {
		base, ext = name[:i], name[i+1:]
	}
	b := sanitize(base, 8)
	if b == "" {
		b = "_"
	}
	e := sanitize(ext, 3)
	for n := 1; ; n++ {
		suf := "~" + strconv.Itoa(n)
		keep := 8 - len(suf)
		if keep < 1 {
			keep = 1
		}
		bb := b
		if len(bb) > keep {
			bb = bb[:keep]
		}
		cand := bb + suf
		if e != "" {
			cand += "." + e
		}
		if !used[cand] {
			return cand
		}
	}
}

// assignNames gives every entry a unique DOS name. The result only depends
// on the set of real names, so repeated calls always agree.
// Names that already are valid 8.3 names win over generated ones.
func assignNames(es []entry) {
	sort.Slice(es, func(i, j int) bool { return es[i].Real < es[j].Real })
	used := map[string]bool{}
	done := make([]bool, len(es))
	for i := range es {
		if n, ok := nativeDOS(es[i].Real); ok && !used[n] {
			es[i].DOS = n
			used[n] = true
			done[i] = true
		}
	}
	for i := range es {
		if done[i] {
			continue
		}
		es[i].DOS = mangle(es[i].Real, used)
		used[es[i].DOS] = true
	}
}

// listDir lists a real directory. Hidden (dot) files, symlinks and special
// files are not shown.
func listDir(dir string) ([]entry, error) {
	des, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	es := make([]entry, 0, len(des))
	for _, de := range des {
		name := de.Name()
		if strings.HasPrefix(name, ".") || de.Type()&fs.ModeSymlink != 0 {
			continue
		}
		info, err := de.Info()
		if err != nil {
			continue
		}
		if !info.IsDir() && !info.Mode().IsRegular() {
			continue
		}
		es = append(es, entry{Real: name, Dir: info.IsDir(), Size: info.Size(), Mod: info.ModTime()})
	}
	assignNames(es)
	return es, nil
}

func dosStamp(t time.Time) (string, string) {
	t = t.Local()
	if t.Year() < 1980 || t.Year() > 2099 {
		return "19800101", "0000"
	}
	return t.Format("20060102"), t.Format("1504")
}

// ---------------------------------------------------------------------
// Server

type server struct {
	root string
}

// resolve turns a client (DOS-name) path into a real path below root.
// Every component must match a listed DOS name, so the client can never
// leave the shared folder.
func (s *server) resolve(p string) (string, *entry, error) {
	cur := s.root
	var last *entry
	parts := strings.FieldsFunc(p, func(r rune) bool { return r == '/' || r == '\\' })
	for _, part := range parts {
		if part == ".." {
			return "", nil, errBad
		}
		if part == "." {
			continue
		}
		if last != nil && !last.Dir {
			return "", nil, errNotFound
		}
		es, err := listDir(cur)
		if err != nil {
			return "", nil, errNotFound
		}
		want := strings.ToUpper(part)
		var found *entry
		for i := range es {
			if es[i].DOS == want {
				found = &es[i]
				break
			}
		}
		if found == nil {
			return "", nil, errNotFound
		}
		cur = filepath.Join(cur, found.Real)
		last = found
	}
	return cur, last, nil
}

func (s *server) cmdList(w *bufio.Writer, arg string) {
	path, ent, err := s.resolve(arg)
	if err != nil {
		writeErr(w, err)
		return
	}
	if ent != nil && !ent.Dir {
		writeErr(w, errNotDir)
		return
	}
	es, err := listDir(path)
	if err != nil {
		writeErr(w, errInternal)
		return
	}
	sort.Slice(es, func(i, j int) bool {
		if es[i].Dir != es[j].Dir {
			return es[i].Dir
		}
		return es[i].DOS < es[j].DOS
	})
	fmt.Fprintf(w, "OK %d\r\n", len(es))
	for _, e := range es {
		kind, size := "F", e.Size
		if e.Dir {
			kind, size = "D", 0
		}
		d, t := dosStamp(e.Mod)
		fmt.Fprintf(w, "%s\t%s\t%d\t%s\t%s\r\n", kind, e.DOS, size, d, t)
	}
	w.Flush()
}

const (
	maxTreeDepth   = 32
	maxTreeEntries = 50000
)

var (
	errTooBig     = protoErr{413, "Folder tree too large"}
	errUnreadable = protoErr{403, "Cannot read a folder"}
)

type treeItem struct {
	Dir  bool
	Rel  string // DOS path relative to the requested folder, backslash-separated
	Size int64
	Mod  time.Time
}

// walkTree collects every folder and file below dir, depth first. A folder
// is always added before its contents, so a client can create folders in
// the order received. Unreadable folders fail the whole request instead of
// silently producing an incomplete copy.
func walkTree(dir, rel string, depth int, items *[]treeItem, total *int64) error {
	if depth > maxTreeDepth {
		return errTooBig
	}
	es, err := listDir(dir)
	if err != nil {
		return errUnreadable
	}
	sort.Slice(es, func(i, j int) bool {
		if es[i].Dir != es[j].Dir {
			return es[i].Dir
		}
		return es[i].DOS < es[j].DOS
	})
	for _, e := range es {
		if len(*items) >= maxTreeEntries {
			return errTooBig
		}
		r := e.DOS
		if rel != "" {
			r = rel + `\` + e.DOS
		}
		*items = append(*items, treeItem{Dir: e.Dir, Rel: r, Size: e.Size, Mod: e.Mod})
		if e.Dir {
			if err := walkTree(filepath.Join(dir, e.Real), r, depth+1, items, total); err != nil {
				return err
			}
		} else {
			*total += e.Size
		}
	}
	return nil
}

func (s *server) cmdTree(w *bufio.Writer, arg string) {
	path, ent, err := s.resolve(arg)
	if err != nil {
		writeErr(w, err)
		return
	}
	if ent != nil && !ent.Dir {
		writeErr(w, errNotDir)
		return
	}
	var items []treeItem
	var total int64
	if err := walkTree(path, "", 1, &items, &total); err != nil {
		writeErr(w, err)
		return
	}
	fmt.Fprintf(w, "OK %d %d\r\n", len(items), total)
	for _, it := range items {
		kind, size := "F", it.Size
		if it.Dir {
			kind, size = "D", 0
		}
		d, t := dosStamp(it.Mod)
		fmt.Fprintf(w, "%s\t%s\t%d\t%s\t%s\r\n", kind, it.Rel, size, d, t)
	}
	w.Flush()
}

// cmdCRC answers with the size and the CRC32 (IEEE) of a file, so a client
// can verify a download after it has been written to disk.
func (s *server) cmdCRC(w *bufio.Writer, arg string) {
	path, ent, err := s.resolve(arg)
	if err != nil {
		writeErr(w, err)
		return
	}
	if ent == nil || ent.Dir {
		writeErr(w, errIsDir)
		return
	}
	f, err := os.Open(path)
	if err != nil {
		writeErr(w, errDenied)
		return
	}
	defer f.Close()
	h := crc32.NewIEEE()
	n, err := io.Copy(h, f)
	if err != nil {
		writeErr(w, errInternal)
		return
	}
	fmt.Fprintf(w, "OK %d %08X\r\n", n, h.Sum32())
	w.Flush()
}

// cmdGet returns false if the connection must be closed afterwards.
func (s *server) cmdGet(conn net.Conn, w *bufio.Writer, arg string) bool {
	// "GET <path> [offset [length]]". DOS names never contain spaces, so
	// the arguments can simply be split at spaces.
	fields := strings.Fields(arg)
	if len(fields) == 0 || len(fields) > 3 {
		writeErr(w, errBad)
		return true
	}
	var offset int64
	length := int64(-1)
	if len(fields) >= 2 {
		v, perr := strconv.ParseInt(fields[1], 10, 64)
		if perr != nil || v < 0 {
			writeErr(w, errBad)
			return true
		}
		offset = v
	}
	if len(fields) == 3 {
		v, perr := strconv.ParseInt(fields[2], 10, 64)
		if perr != nil || v < 0 {
			writeErr(w, errBad)
			return true
		}
		length = v
	}

	path, ent, err := s.resolve(fields[0])
	if err != nil {
		writeErr(w, err)
		return true
	}
	if ent == nil || ent.Dir {
		writeErr(w, errIsDir)
		return true
	}
	f, err := os.Open(path)
	if err != nil {
		writeErr(w, errDenied)
		return true
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		writeErr(w, errInternal)
		return true
	}
	total := st.Size()
	if offset > total {
		writeErr(w, errBad)
		return true
	}
	size := total - offset
	if length >= 0 && length < size {
		size = length
	}
	if offset > 0 {
		if _, serr := f.Seek(offset, io.SeekStart); serr != nil {
			writeErr(w, errInternal)
			return true
		}
	}
	fmt.Fprintf(w, "OK %d %d\r\n", size, total)
	if w.Flush() != nil {
		return false
	}

	lr := io.LimitReader(f, size)
	buf := make([]byte, 32*1024)
	var sent int64
	for sent < size {
		n, rerr := lr.Read(buf)
		if n > 0 {
			conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if _, werr := conn.Write(buf[:n]); werr != nil {
				return false
			}
			sent += int64(n)
		}
		if rerr != nil {
			break
		}
	}
	conn.SetWriteDeadline(time.Time{})
	// If the file shrank while sending, the client got fewer bytes than
	// announced; closing the connection is the only honest signal.
	return sent == size
}

func (s *server) handle(conn net.Conn) {
	defer conn.Close()
	remote := conn.RemoteAddr().String()
	r := bufio.NewReaderSize(conn, maxLine)
	w := bufio.NewWriter(conn)
	for {
		conn.SetReadDeadline(time.Now().Add(idleTimeout))
		raw, err := r.ReadSlice('\n')
		if err != nil {
			if errors.Is(err, bufio.ErrBufferFull) {
				writeErr(w, errBad)
			}
			return
		}
		line := strings.TrimSpace(string(raw))
		if line == "" {
			continue
		}
		cmd, arg := line, ""
		if i := strings.IndexByte(line, ' '); i >= 0 {
			cmd, arg = line[:i], strings.TrimSpace(line[i+1:])
		}
		cmd = strings.ToUpper(cmd)
		log.Printf("%s %s %s", remote, cmd, arg)

		switch cmd {
		case "HELLO":
			fmt.Fprintf(w, "OK EASYCOMMANDER %d\r\n", protoVersion)
			w.Flush()
		case "LIST":
			s.cmdList(w, arg)
		case "TREE":
			s.cmdTree(w, arg)
		case "CRC":
			s.cmdCRC(w, arg)
		case "GET":
			if !s.cmdGet(conn, w, arg) {
				return
			}
		case "QUIT":
			fmt.Fprint(w, "OK Bye\r\n")
			w.Flush()
			return
		default:
			writeErr(w, protoErr{400, "Unknown command"})
		}
	}
}

func allowedRemote(addr net.Addr) bool {
	ta, ok := addr.(*net.TCPAddr)
	if !ok {
		return false
	}
	ip := ta.IP
	return ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast()
}

func localIPs() []string {
	var out []string
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok {
			if ip4 := ipn.IP.To4(); ip4 != nil && ip4.IsPrivate() {
				out = append(out, ip4.String())
			}
		}
	}
	return out
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "\nERROR: "+format+"\n", args...)
	fmt.Print("Press Enter to exit...")
	bufio.NewReader(os.Stdin).ReadString('\n')
	os.Exit(1)
}

func main() {
	dirFlag := flag.String("dir", "", "folder to share (or give it as the first argument / drop it on the program)")
	port := flag.Int("port", 6502, "TCP port to listen on")
	flag.Parse()

	dir := *dirFlag
	if dir == "" && flag.NArg() > 0 {
		dir = flag.Arg(0)
	}
	if dir == "" {
		cwd, err := os.Getwd()
		if err != nil {
			fatal("cannot determine current folder: %v", err)
		}
		dir = filepath.Join(cwd, "share")
		if err := os.MkdirAll(dir, 0o755); err != nil {
			fatal("cannot create %s: %v", dir, err)
		}
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		fatal("bad folder: %v", err)
	}
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		fatal("%s is not a folder", abs)
	}

	ln, err := net.Listen("tcp4", fmt.Sprintf(":%d", *port))
	if err != nil {
		fatal("cannot listen on port %d: %v", *port, err)
	}

	fmt.Printf("Easy Commander host (read-only)\n")
	fmt.Printf("Sharing folder: %s\n", abs)
	fmt.Printf("Port: %d\n", *port)
	if ips := localIPs(); len(ips) > 0 {
		fmt.Println("Enter one of these addresses in the DOS client:")
		for _, ip := range ips {
			fmt.Printf("  %s\n", ip)
		}
	}
	fmt.Println("Only local network (private) addresses are accepted. Ctrl+C to stop.")
	fmt.Println()

	srv := &server{root: abs}
	sem := make(chan struct{}, maxClients)
	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			log.Printf("accept: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		if !allowedRemote(conn.RemoteAddr()) {
			log.Printf("rejected non-local connection from %s", conn.RemoteAddr())
			conn.Close()
			continue
		}
		select {
		case sem <- struct{}{}:
			go func() {
				defer func() { <-sem }()
				srv.handle(conn)
			}()
		default:
			w := bufio.NewWriter(conn)
			writeErr(w, errBusy)
			conn.Close()
		}
	}
}
