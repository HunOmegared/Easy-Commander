package main

// Receiving: the other direction.
//
// The DOS client offers files with "OFFER <files> <bytes> [label]". If the host
// agrees (receiving is on, the client is on the local network, the quota and
// the free disk space allow it, and the person at the host said yes when the
// host asks), the roles are swapped on the same TCP connection: the HOST
// becomes the downloader and sends TREE and GET to the client, which answers
// exactly like a host would, but only for the files it offered. Everything
// that decides what lands on the disk is therefore checked here, on the host,
// and nothing the client says is trusted:
//
//   - files go into a new folder of their own below the inbox folder, never
//     into the shared folder and never over an existing file
//   - every name must be a plain DOS 8.3 name (no "..", no device names)
//   - a file is written as NAME.EXT.part and renamed when it is complete
//   - the number of bytes is limited by what was offered, by the quota of the
//     inbox and by the free disk space that must remain
//   - one transfer at a time, with time limits on every step
//
// Protocol (see docs/PROTOCOL.md):
//
//	client: OFFER <files> <bytes> [label]
//	host:   WAIT                 (now and then, while somebody is asked)
//	host:   OK | ERR <code> <message>
//	then, after OK, the host sends TREE \ and GET \path offset length and the
//	client answers with the normal replies; at the end the host sends
//	        END <files received> <files failed>     or    ABORT <message>
//	and the connection is a normal one again.

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"io/fs"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	recvBlock       = 16 * 1024 // bytes asked from the client at a time (small: it is a DOS machine)
	askTimeout      = 60 * time.Second
	reverseTimeout  = 2 * time.Minute // longest wait for one answer of the client
	defaultReserve  = int64(1) << 30  // free space that must remain on the disk
	maxOfferEntries = 100000
	reportName      = "ECINBOX.LOG"
)

// waitTick is how often a waiting client hears "WAIT" (a variable for the tests).
var waitTick = 5 * time.Second

var (
	errNoUpload   = protoErr{403, "Receiving is off"}
	errNoShare    = protoErr{403, "Sharing is off"}
	errNotHere    = protoErr{403, "Not allowed from this address"}
	errDeclined   = protoErr{401, "Declined by the host"}
	errNoAnswer   = protoErr{408, "No answer at the host"}
	errNoSpace    = protoErr{507, "Not enough disk space"}
	errOverQuota  = protoErr{507, "Over the quota of the inbox"}
	errBusyRecv   = protoErr{409, "Another transfer is running"}
	errCantCreate = protoErr{500, "Cannot create the folder"}
)

// InboxConfig is what the person at the host decides.
type InboxConfig struct {
	Dir     string       // the inbox folder
	Quota   int64        // bytes the inbox may hold in total; 0 = only the free space limits
	Reserve int64        // free space that must remain on the disk
	Ask     bool         // ask before every transfer
	Allow   []*net.IPNet // networks accepted besides the ones the computer is in
}

// OfferInfo is shown to the person who is asked.
type OfferInfo struct {
	Remote string // address of the client
	Label  string
	Files  int
	Bytes  int64
	Dest   string // name of the folder that will be created
	Free   int64  // free disk space (-1 = unknown)
	Used   int64  // bytes already in the inbox
	Quota  int64
}

// Receiver holds the state of the receiving mode.
type Receiver struct {
	cfg     InboxConfig
	confirm func(ctx context.Context, info OfferInfo) bool
	busy    atomic.Bool
	done    chan struct{} // closed when receiving is switched off
	askWait time.Duration

	mu     sync.Mutex
	active net.Conn
}

func (rx *Receiver) setActive(c net.Conn) {
	rx.mu.Lock()
	rx.active = c
	rx.mu.Unlock()
}

func (rx *Receiver) stopped() bool {
	select {
	case <-rx.done:
		return true
	default:
		return false
	}
}

// ---------------------------------------------------------------------
// Host side switches

// StartReceiving switches the inbox on. The host starts listening if it does
// not already (sharing may be off). confirm is only used when cfg.Ask is set.
func (h *Host) StartReceiving(port int, name string, cfg InboxConfig, confirm func(context.Context, OfferInfo) bool) error {
	h.mu.Lock()
	defer h.mu.Unlock()

	if h.rx != nil {
		return fmt.Errorf("already receiving")
	}
	if cfg.Dir == "" {
		return fmt.Errorf("no inbox folder chosen")
	}
	abs, err := filepath.Abs(cfg.Dir)
	if err != nil {
		return fmt.Errorf("bad folder: %v", err)
	}
	if err := os.MkdirAll(abs, 0o755); err != nil {
		return fmt.Errorf("cannot create %s: %v", abs, err)
	}
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		return fmt.Errorf("%s is not a folder", abs)
	}
	if h.sharing && h.shareRoot != "" && samePath(abs, h.shareRoot) {
		return fmt.Errorf("the inbox must not be the shared folder")
	}
	if cfg.Quota < 0 || cfg.Reserve < 0 {
		return fmt.Errorf("bad quota")
	}
	if err := h.listenLocked(port, name); err != nil {
		return err
	}
	cfg.Dir = abs
	h.rx = &Receiver{cfg: cfg, confirm: confirm, done: make(chan struct{}), askWait: askTimeout}
	h.InboxDir = abs
	return nil
}

// StopReceiving switches the inbox off and ends a transfer that is running.
func (h *Host) StopReceiving() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.stopReceivingLocked()
	if !h.sharing {
		h.closeListenerLocked()
	}
}

func (h *Host) stopReceivingLocked() {
	rx := h.rx
	if rx == nil {
		return
	}
	h.rx = nil
	close(rx.done)
	rx.mu.Lock()
	if rx.active != nil {
		rx.active.Close()
	}
	rx.mu.Unlock()
}

func (h *Host) Receiving() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.rx != nil
}

// ReceiveBusy is true while a transfer is running.
func (h *Host) ReceiveBusy() bool {
	rx := h.receiver()
	return rx != nil && rx.busy.Load()
}

// ReceiveAsks is true if every transfer must be confirmed.
func (h *Host) ReceiveAsks() bool {
	rx := h.receiver()
	return rx != nil && rx.cfg.Ask
}

func (h *Host) receiver() *Receiver {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.rx
}

func (h *Host) shareDir() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.sharing {
		return ""
	}
	return h.shareRoot
}

func samePath(a, b string) bool {
	return strings.EqualFold(filepath.Clean(a), filepath.Clean(b))
}

// ---------------------------------------------------------------------
// Who may send

// inLocalNets reports whether ip is in one of the networks the computer itself
// is in (address and netmask of every interface).
func inLocalNets(ip net.IP, addrs []net.Addr) bool {
	ip4 := ip.To4()
	if ip4 == nil {
		return false
	}
	for _, a := range addrs {
		n, ok := a.(*net.IPNet)
		if !ok || n.IP.To4() == nil {
			continue
		}
		if n.Contains(ip4) {
			return true
		}
	}
	return false
}

func (h *Host) inboxAllowed(rx *Receiver, ip net.IP) bool {
	if ip.IsLoopback() {
		return true
	}
	for _, n := range rx.cfg.Allow {
		if n.Contains(ip) {
			return true
		}
	}
	get := h.ifaceAddrs
	if get == nil {
		get = net.InterfaceAddrs
	}
	addrs, err := get()
	if err != nil {
		return false
	}
	return inLocalNets(ip, addrs)
}

// uploadFor says whether HELLO may announce the receiving mode to this client.
func (h *Host) uploadFor(addr net.Addr) bool {
	rx := h.receiver()
	ta, ok := addr.(*net.TCPAddr)
	if rx == nil || !ok {
		return false
	}
	return h.inboxAllowed(rx, ta.IP)
}

// ---------------------------------------------------------------------
// Helpers

func cleanLabel(s string) string {
	var b strings.Builder
	for _, r := range s {
		if (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' || r == '-' {
			b.WriteRune(r)
		}
		if b.Len() >= 20 {
			break
		}
	}
	return b.String()
}

// dirSize adds up the size of every file below dir.
func dirSize(dir string) int64 {
	var n int64
	filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.Type().IsRegular() {
			if info, err := d.Info(); err == nil {
				n += info.Size()
			}
		}
		return nil
	})
	return n
}

// makeSessionDir creates base/name, or base/name_2, name_3 ... when it exists.
func makeSessionDir(base, name string) (string, error) {
	for i := 1; i < 100; i++ {
		n := name
		if i > 1 {
			n = fmt.Sprintf("%s_%d", name, i)
		}
		p := filepath.Join(base, n)
		err := os.Mkdir(p, 0o755)
		if err == nil {
			return p, nil
		}
		if !os.IsExist(err) {
			return "", err
		}
	}
	return "", fmt.Errorf("too many folders named %s", name)
}

var reservedDOS = map[string]bool{"CON": true, "PRN": true, "AUX": true, "NUL": true, "CLOCK$": true}

// cleanDOSComponent accepts a plain DOS 8.3 name and returns it in upper
// case. Names of devices (CON, NUL, COM1 ...) are refused: Windows would open
// the device instead of making a file.
func cleanDOSComponent(s string) (string, bool) {
	up, ok := nativeDOS(s)
	if !ok {
		return "", false
	}
	base := up
	if i := strings.IndexByte(up, '.'); i >= 0 {
		base = up[:i]
	}
	if reservedDOS[base] {
		return "", false
	}
	if len(base) == 4 && (strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT")) && base[3] >= '1' && base[3] <= '9' {
		return "", false
	}
	return up, true
}

type offered struct {
	isDir bool
	parts []string // the checked, upper case components
	size  int64
	mod   time.Time // zero = unknown
}

func (o offered) rel() string { return strings.Join(o.parts, `\`) }

// parseOfferedLine reads a TREE style line: <D|F>\t<REL>\t<size>\t<YYYYMMDD>\t<HHMM>
func parseOfferedLine(line string) (offered, error) {
	f := strings.Split(line, "\t")
	if len(f) < 5 || (f[0] != "D" && f[0] != "F") {
		return offered{}, fmt.Errorf("bad line")
	}
	o := offered{isDir: f[0] == "D"}
	for _, c := range strings.Split(f[1], `\`) {
		n, ok := cleanDOSComponent(c)
		if !ok {
			return offered{}, fmt.Errorf("bad name %q", c)
		}
		o.parts = append(o.parts, n)
	}
	if len(o.parts) > maxTreeDepth {
		return offered{}, fmt.Errorf("path too deep")
	}
	size, err := strconv.ParseInt(f[2], 10, 64)
	if err != nil || size < 0 || (o.isDir && size != 0) {
		return offered{}, fmt.Errorf("bad size")
	}
	o.size = size
	if len(f[3]) == 8 && len(f[4]) == 4 {
		y, e1 := strconv.Atoi(f[3][:4])
		m, e2 := strconv.Atoi(f[3][4:6])
		d, e3 := strconv.Atoi(f[3][6:])
		hh, e4 := strconv.Atoi(f[4][:2])
		mm, e5 := strconv.Atoi(f[4][2:])
		if e1 == nil && e2 == nil && e3 == nil && e4 == nil && e5 == nil &&
			y >= 1980 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= 31 && hh < 24 && mm < 60 {
			o.mod = time.Date(y, time.Month(m), d, hh, mm, 0, 0, time.Local)
		}
	}
	return o, nil
}

// ---------------------------------------------------------------------
// The transfer

// clientError is an ERR answer of the client (for example a read error of its
// disk): the file fails, the transfer goes on.
type clientError struct{ msg string }

func (e clientError) Error() string { return e.msg }

type receiveSession struct {
	conn net.Conn
	r    *bufio.Reader
	w    *bufio.Writer
	rx   *Receiver
}

// ask sends one command to the client and returns the status line it answers
// ("OK ..."), or a clientError for "ERR ...". Any other problem is a plain
// error and ends the session.
func (s *receiveSession) ask(cmd string) (string, error) {
	s.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	if _, err := fmt.Fprintf(s.w, "%s\r\n", cmd); err != nil {
		return "", err
	}
	if err := s.w.Flush(); err != nil {
		return "", err
	}
	s.conn.SetWriteDeadline(time.Time{})
	return s.readStatus()
}

func (s *receiveSession) readStatus() (string, error) {
	s.conn.SetReadDeadline(time.Now().Add(reverseTimeout))
	raw, err := s.r.ReadSlice('\n')
	if err != nil {
		return "", err
	}
	line := strings.TrimSpace(string(raw))
	switch {
	case line == "OK" || strings.HasPrefix(line, "OK "):
		return strings.TrimSpace(line[2:]), nil
	case strings.HasPrefix(line, "ERR"):
		msg := strings.TrimSpace(line[3:])
		if len(msg) > 80 {
			msg = msg[:80]
		}
		return "", clientError{msg}
	}
	return "", fmt.Errorf("unexpected answer from the client")
}

// readItems asks for the list of everything that was offered.
func (s *receiveSession) readItems(files int, total int64) ([]offered, error) {
	st, err := s.ask(`TREE \`)
	if err != nil {
		return nil, err
	}
	var n int
	var sum int64
	if _, err := fmt.Sscanf(st, "%d %d", &n, &sum); err != nil || n < 0 || n > maxOfferEntries {
		return nil, fmt.Errorf("bad list header")
	}
	items := make([]offered, 0, n)
	dirs := map[string]bool{}
	seen := map[string]bool{}
	var nfiles int
	var bytes int64
	for i := 0; i < n; i++ {
		s.conn.SetReadDeadline(time.Now().Add(reverseTimeout))
		raw, err := s.r.ReadSlice('\n')
		if err != nil {
			return nil, err
		}
		o, err := parseOfferedLine(strings.TrimRight(string(raw), "\r\n"))
		if err != nil {
			return nil, fmt.Errorf("the client's list: %v", err)
		}
		key := o.rel()
		if seen[key] {
			return nil, fmt.Errorf("the client's list has %s twice", key)
		}
		seen[key] = true
		if len(o.parts) > 1 {
			parent := strings.Join(o.parts[:len(o.parts)-1], `\`)
			if !dirs[parent] {
				return nil, fmt.Errorf("the client's list: folder of %s is missing", key)
			}
		}
		if o.isDir {
			dirs[key] = true
		} else {
			nfiles++
			bytes += o.size
			if nfiles > files || bytes > total {
				return nil, fmt.Errorf("the client sent more than it offered")
			}
		}
		items = append(items, o)
	}
	return items, nil
}

// receiveFile fetches one file with GET blocks. A returned clientError means
// only this file failed.
func (s *receiveSession) receiveFile(local string, o offered, received *int64, limit int64) error {
	tmp := local + ".part"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}
	closed := false
	defer func() {
		if !closed {
			f.Close()
		}
	}()

	buf := make([]byte, recvBlock)
	var off int64
	for off < o.size {
		if s.rx.stopped() {
			return fmt.Errorf("receiving was switched off")
		}
		want := o.size - off
		if want > recvBlock {
			want = recvBlock
		}
		st, err := s.ask(fmt.Sprintf(`GET \%s %d %d`, o.rel(), off, want))
		if err != nil {
			return err
		}
		var n, total int64
		if _, err := fmt.Sscanf(st, "%d %d", &n, &total); err != nil || n < 1 || n > want {
			return fmt.Errorf("bad answer to GET")
		}
		if *received+n > limit {
			return fmt.Errorf("the client sent more than it offered")
		}
		s.conn.SetReadDeadline(time.Now().Add(reverseTimeout))
		if _, err := io.ReadFull(s.r, buf[:n]); err != nil {
			return err
		}
		if _, err := f.Write(buf[:n]); err != nil {
			return fileError{err}
		}
		off += n
		*received += n
	}
	if err := f.Close(); err != nil {
		closed = true
		return fileError{err}
	}
	closed = true
	if err := os.Rename(tmp, local); err != nil {
		return fileError{err}
	}
	if !o.mod.IsZero() {
		os.Chtimes(local, o.mod, o.mod)
	}
	return nil
}

// fileError is a problem with the host's own disk: the transfer stops.
type fileError struct{ err error }

func (e fileError) Error() string { return "cannot write: " + e.err.Error() }

// handleOffer returns false if the connection must be closed.
func (h *Host) handleOffer(conn net.Conn, r *bufio.Reader, w *bufio.Writer, arg string) bool {
	rx := h.receiver()
	if rx == nil {
		writeErr(w, errNoUpload)
		return true
	}
	ta, _ := conn.RemoteAddr().(*net.TCPAddr)
	if ta == nil || !h.inboxAllowed(rx, ta.IP) {
		logf("%s: offer refused (not in the local network)", conn.RemoteAddr())
		writeErr(w, errNotHere)
		return true
	}

	f := strings.Fields(arg)
	if len(f) < 2 || len(f) > 3 {
		writeErr(w, errBad)
		return true
	}
	files, err1 := strconv.Atoi(f[0])
	total, err2 := strconv.ParseInt(f[1], 10, 64)
	if err1 != nil || err2 != nil || files < 0 || files > maxOfferEntries || total < 0 {
		writeErr(w, errBad)
		return true
	}
	label := ""
	if len(f) == 3 {
		label = cleanLabel(f[2])
	}
	if label == "" {
		label = strings.ReplaceAll(ta.IP.String(), ".", "_")
		label = strings.ReplaceAll(label, ":", "_")
	}

	if !rx.busy.CompareAndSwap(false, true) {
		writeErr(w, errBusyRecv)
		return true
	}
	defer rx.busy.Store(false)
	rx.setActive(conn)
	defer rx.setActive(nil)

	// Quota and free space, checked before anybody is asked.
	cfg := rx.cfg
	used := dirSize(cfg.Dir)
	if cfg.Quota > 0 && used+total > cfg.Quota {
		logf("%s: offer of %d bytes refused (over the quota)", ta.IP, total)
		writeErr(w, errOverQuota)
		return true
	}
	free, ferr := freeSpace(cfg.Dir)
	if ferr != nil {
		free = -1
	} else if free < total+cfg.Reserve {
		logf("%s: offer of %d bytes refused (not enough disk space)", ta.IP, total)
		writeErr(w, errNoSpace)
		return true
	}

	dest := fmt.Sprintf("%s_%s", label, time.Now().Format("20060102_1504"))
	info := OfferInfo{Remote: ta.IP.String(), Label: label, Files: files, Bytes: total,
		Dest: dest, Free: free, Used: used, Quota: cfg.Quota}

	if cfg.Ask {
		if rx.confirm == nil {
			writeErr(w, errDeclined)
			return true
		}
		logf("%s offers %d files (%d bytes): waiting for your answer", ta.IP, files, total)
		ctx, cancel := context.WithTimeout(context.Background(), rx.askWait)
		answer := make(chan bool, 1)
		go func() { answer <- rx.confirm(ctx, info) }()
		tick := time.NewTicker(waitTick)
		yes, answered := false, false
	wait:
		for {
			select {
			case yes = <-answer:
				answered = true
				break wait
			case <-ctx.Done():
				break wait
			case <-rx.done:
				break wait
			case <-tick.C:
				conn.SetWriteDeadline(time.Now().Add(writeTimeout))
				fmt.Fprint(w, "WAIT\r\n")
				if w.Flush() != nil {
					tick.Stop()
					cancel()
					return false
				}
			}
		}
		tick.Stop()
		cancel()
		if rx.stopped() {
			writeErr(w, errNoUpload)
			return true
		}
		if !answered {
			logf("%s: no answer, offer refused", ta.IP)
			writeErr(w, errNoAnswer)
			return true
		}
		if !yes {
			logf("%s: offer declined", ta.IP)
			writeErr(w, errDeclined)
			return true
		}
	}

	sdir, err := makeSessionDir(cfg.Dir, dest)
	if err != nil {
		logf("cannot create the folder for %s: %v", ta.IP, err)
		writeErr(w, errCantCreate)
		return true
	}
	conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	fmt.Fprint(w, "OK\r\n")
	if w.Flush() != nil {
		return false
	}
	conn.SetWriteDeadline(time.Time{})
	logf("%s: receiving %d files (%d bytes) into %s", ta.IP, files, total, sdir)

	s := &receiveSession{conn: conn, r: r, w: w, rx: rx}
	return s.run(sdir, ta.IP.String(), label, files, total)
}

// run does the transfer. It returns false if the connection must be closed.
func (s *receiveSession) run(sdir, remote, label string, files int, total int64) bool {
	started := time.Now()
	abort := func(format string, a ...any) bool {
		msg := fmt.Sprintf(format, a...)
		logf("%s: transfer stopped: %s", remote, msg)
		s.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
		fmt.Fprintf(s.w, "ABORT %s\r\n", strings.NewReplacer("\r", " ", "\n", " ").Replace(msg))
		return s.w.Flush() == nil
	}

	items, err := s.readItems(files, total)
	if err != nil {
		if _, isClient := err.(clientError); isClient {
			return abort("the client could not list what it offered: %v", err)
		}
		if isConnErr(err) {
			logf("%s: connection lost while listing: %v", remote, err)
			return false
		}
		return abort("%v", err)
	}

	var report []string
	ok, failed := 0, 0
	var received int64
	for _, o := range items {
		if s.rx.stopped() {
			abort("receiving was switched off")
			return false
		}
		local := filepath.Join(append([]string{sdir}, o.parts...)...)
		if o.isDir {
			if err := os.Mkdir(local, 0o755); err != nil && !os.IsExist(err) {
				return abort("cannot create folder %s: %v", o.rel(), err)
			}
			continue
		}
		var ferr error
		if o.size == 0 {
			f, err := os.OpenFile(local, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
			if err != nil {
				ferr = fileError{err}
			} else {
				f.Close()
				if !o.mod.IsZero() {
					os.Chtimes(local, o.mod, o.mod)
				}
			}
		} else {
			ferr = s.receiveFile(local, o, &received, total)
		}
		switch e := ferr.(type) {
		case nil:
			ok++
		case clientError:
			failed++
			logf("%s: %s failed: the client says: %s", remote, o.rel(), e.msg)
			report = append(report, fmt.Sprintf("FAILED %s: %s (partial data, if any, is in %s.part)", o.rel(), e.msg, filepath.Base(local)))
		case fileError:
			return abort("%s", e.Error())
		default:
			if isConnErr(ferr) {
				logf("%s: connection lost during %s: %v", remote, o.rel(), ferr)
				return false
			}
			return abort("%s: %v", o.rel(), ferr)
		}
	}

	s.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	fmt.Fprintf(s.w, "END %d %d\r\n", ok, failed)
	if s.w.Flush() != nil {
		return false
	}
	logf("%s: received %d files (%d bytes), %d failed, in %s", remote, ok, received, failed, time.Since(started).Round(time.Second))
	writeReport(sdir, remote, label, started, ok, failed, received, report)
	return true
}

func isConnErr(err error) bool {
	if err == nil {
		return false
	}
	if ne, ok := err.(net.Error); ok && ne != nil {
		return true
	}
	return err == io.EOF || err == io.ErrUnexpectedEOF || err == bufio.ErrBufferFull
}

func writeReport(sdir, remote, label string, started time.Time, ok, failed int, bytes int64, lines []string) {
	var b strings.Builder
	fmt.Fprintf(&b, "Easy Commander inbox report\r\n")
	fmt.Fprintf(&b, "From: %s (%s)\r\n", remote, label)
	fmt.Fprintf(&b, "Started: %s\r\n", started.Format("2006-01-02 15:04:05"))
	fmt.Fprintf(&b, "Files received: %d, failed: %d, bytes: %d\r\n", ok, failed, bytes)
	for _, l := range lines {
		fmt.Fprintf(&b, "%s\r\n", l)
	}
	os.WriteFile(filepath.Join(sdir, reportName), []byte(b.String()), 0o644)
}
