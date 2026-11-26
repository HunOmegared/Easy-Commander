package main

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// ---- a pretend DOS client --------------------------------------------

type fakeFile struct {
	rel    string // backslash separated, parents first
	dir    bool
	data   []byte
	mod    time.Time
	failAt int64 // read error from this offset on (-1 = never)
}

func ff(rel string, data string) fakeFile {
	return fakeFile{rel: rel, data: []byte(data), mod: time.Date(2020, 3, 4, 12, 34, 0, 0, time.Local), failAt: -1}
}

func fd(rel string) fakeFile {
	return fakeFile{rel: rel, dir: true, mod: time.Date(2020, 3, 4, 12, 34, 0, 0, time.Local), failAt: -1}
}

type fakeClient struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

func dial(t *testing.T, port int) *fakeClient {
	t.Helper()
	conn, err := net.Dial("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &fakeClient{t: t, conn: conn, r: bufio.NewReader(conn)}
}

func (c *fakeClient) send(s string) {
	c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	fmt.Fprintf(c.conn, "%s\r\n", s)
}

func (c *fakeClient) line() string {
	c.conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	s, err := c.r.ReadString('\n')
	if err != nil {
		c.t.Fatalf("read: %v (got %q)", err, s)
	}
	return strings.TrimRight(s, "\r\n")
}

// answer to a command, skipping WAIT lines.
func (c *fakeClient) status() string {
	for {
		l := c.line()
		if l != "WAIT" {
			return l
		}
	}
}

func counts(files []fakeFile) (n int, total int64) {
	for _, f := range files {
		if !f.dir {
			n++
			total += int64(len(f.data))
		}
	}
	return
}

func (c *fakeClient) offer(files []fakeFile, label string) string {
	n, total := counts(files)
	cmd := fmt.Sprintf("OFFER %d %d", n, total)
	if label != "" {
		cmd += " " + label
	}
	c.send(cmd)
	return c.status()
}

// serve answers the host's TREE and GET until END or ABORT comes.
func (c *fakeClient) serve(files []fakeFile) string {
	for {
		l := c.line()
		switch {
		case strings.HasPrefix(l, "END") || strings.HasPrefix(l, "ABORT"):
			return l
		case strings.HasPrefix(l, "TREE"):
			_, total := counts(files)
			n, _ := counts(files)
			_ = n
			fmt.Fprintf(c.conn, "OK %d %d\r\n", len(files), total)
			for _, f := range files {
				kind, size := "F", len(f.data)
				if f.dir {
					kind, size = "D", 0
				}
				fmt.Fprintf(c.conn, "%s\t%s\t%d\t%s\t%s\r\n", kind, f.rel, size, f.mod.Format("20060102"), f.mod.Format("1504"))
			}
		case strings.HasPrefix(l, "GET "):
			p := strings.Fields(l)
			off, _ := strconv.ParseInt(p[2], 10, 64)
			ln, _ := strconv.ParseInt(p[3], 10, 64)
			var f *fakeFile
			for i := range files {
				if `\`+files[i].rel == p[1] {
					f = &files[i]
				}
			}
			if f == nil {
				fmt.Fprintf(c.conn, "ERR 404 Not found\r\n")
				continue
			}
			if f.failAt >= 0 && off >= f.failAt {
				fmt.Fprintf(c.conn, "ERR 500 Read error\r\n")
				continue
			}
			end := off + ln
			if end > int64(len(f.data)) {
				end = int64(len(f.data))
			}
			fmt.Fprintf(c.conn, "OK %d %d\r\n", end-off, len(f.data))
			c.conn.Write(f.data[off:end])
		default:
			c.t.Fatalf("host sent something unexpected: %q", l)
		}
	}
}

// ---- helpers ----------------------------------------------------------

func startRx(t *testing.T, cfg InboxConfig, confirm func(context.Context, OfferInfo) bool) (*Host, int) {
	t.Helper()
	if cfg.Dir == "" {
		cfg.Dir = filepath.Join(t.TempDir(), "inbox")
	}
	port := freePort(t)
	h := NewHost()
	if err := h.StartReceiving(port, "Test", cfg, confirm); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(h.Stop)
	return h, port
}

func onlyChild(t *testing.T, dir string) string {
	t.Helper()
	es, err := os.ReadDir(dir)
	if err != nil || len(es) != 1 {
		t.Fatalf("expected exactly one folder in %s, got %v (err %v)", dir, es, err)
	}
	return filepath.Join(dir, es[0].Name())
}

func readFile(t *testing.T, p string) string {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// ---- tests ------------------------------------------------------------

func TestReceiveBasic(t *testing.T) {
	big := strings.Repeat("0123456789ABCDEF", 5000) // 80000 bytes: several blocks
	files := []fakeFile{
		ff("README.TXT", "hello"),
		fd("SUB"),
		ff(`SUB\ONE.TXT`, "one"),
		fd(`SUB\DEEP`),
		ff(`SUB\DEEP\BIG.BIN`, big),
		ff("EMPTY.DAT", ""),
	}
	h, port := startRx(t, InboxConfig{Reserve: 0}, nil)
	c := dial(t, port)
	if st := c.offer(files, "486_C"); st != "OK" {
		t.Fatalf("offer: %s", st)
	}
	if end := c.serve(files); end != "END 4 0" {
		t.Fatalf("end: %s", end)
	}

	sess := onlyChild(t, h.rx.cfg.Dir)
	if !strings.HasPrefix(filepath.Base(sess), "486_C_") {
		t.Errorf("folder name %q", filepath.Base(sess))
	}
	if readFile(t, filepath.Join(sess, "README.TXT")) != "hello" ||
		readFile(t, filepath.Join(sess, "SUB", "ONE.TXT")) != "one" ||
		readFile(t, filepath.Join(sess, "SUB", "DEEP", "BIG.BIN")) != big {
		t.Error("file contents differ")
	}
	if st, err := os.Stat(filepath.Join(sess, "EMPTY.DAT")); err != nil || st.Size() != 0 {
		t.Error("EMPTY.DAT")
	}
	st, _ := os.Stat(filepath.Join(sess, "README.TXT"))
	if !st.ModTime().Equal(files[0].mod) {
		t.Errorf("date not kept: %v", st.ModTime())
	}
	// no .part left over
	filepath.Walk(sess, func(p string, i os.FileInfo, err error) error {
		if strings.HasSuffix(p, ".part") {
			t.Errorf("left over %s", p)
		}
		return nil
	})
	if !strings.Contains(readFile(t, filepath.Join(sess, reportName)), "Files received: 4, failed: 0") {
		t.Error("report")
	}

	// the connection is a normal one again
	c.send("HELLO")
	if l := c.line(); !strings.HasPrefix(l, "OK EASYCOMMANDER 1") {
		t.Errorf("after the transfer: %s", l)
	}
}

func TestReceiveSecondSessionNeverOverwrites(t *testing.T) {
	files := []fakeFile{ff("A.TXT", "first")}
	h, port := startRx(t, InboxConfig{}, nil)
	for i := 0; i < 2; i++ {
		c := dial(t, port)
		if st := c.offer(files, "X"); st != "OK" {
			t.Fatal(st)
		}
		c.serve(files)
	}
	es, _ := os.ReadDir(h.rx.cfg.Dir)
	if len(es) != 2 {
		t.Fatalf("expected two session folders, got %d", len(es))
	}
}

func TestReceiveClientErrorFailsOnlyThatFile(t *testing.T) {
	bad := ff("BAD.DAT", strings.Repeat("x", 40000))
	bad.failAt = recvBlock // the second block cannot be read
	files := []fakeFile{ff("A.TXT", "a"), bad, ff("Z.TXT", "z")}
	h, port := startRx(t, InboxConfig{}, nil)
	c := dial(t, port)
	c.offer(files, "OLD")
	if end := c.serve(files); end != "END 2 1" {
		t.Fatalf("end: %s", end)
	}
	sess := onlyChild(t, h.rx.cfg.Dir)
	if readFile(t, filepath.Join(sess, "Z.TXT")) != "z" {
		t.Error("the file after the failed one was not received")
	}
	if _, err := os.Stat(filepath.Join(sess, "BAD.DAT")); err == nil {
		t.Error("an incomplete file got its final name")
	}
	if st, err := os.Stat(filepath.Join(sess, "BAD.DAT.part")); err != nil || st.Size() != recvBlock {
		t.Errorf("the data read so far should stay as BAD.DAT.part: %v", err)
	}
	if !strings.Contains(readFile(t, filepath.Join(sess, reportName)), "FAILED BAD.DAT") {
		t.Error("report does not name the failed file")
	}
}

func TestReceiveRefusals(t *testing.T) {
	files := []fakeFile{ff("A.TXT", strings.Repeat("a", 1000))}

	// receiving off: no UPLOAD in HELLO, OFFER refused
	h := NewHost()
	dir := t.TempDir()
	port := freePort(t)
	if err := h.Start(dir, port, ""); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(h.Stop)
	c := dial(t, port)
	c.send("HELLO")
	if l := c.line(); l != "OK EASYCOMMANDER 1" {
		t.Errorf("HELLO with receiving off: %q", l)
	}
	if st := c.offer(files, ""); !strings.HasPrefix(st, "ERR 403") {
		t.Errorf("offer with receiving off: %s", st)
	}

	// quota
	_, p2 := startRx(t, InboxConfig{Quota: 500}, nil)
	if st := dial(t, p2).offer(files, ""); !strings.HasPrefix(st, "ERR 507") {
		t.Errorf("over quota: %s", st)
	}

	// free space that must remain: far more than any disk has
	_, p3 := startRx(t, InboxConfig{Reserve: 1 << 60}, nil)
	if st := dial(t, p3).offer(files, ""); !strings.HasPrefix(st, "ERR 507") {
		t.Errorf("no space: %s", st)
	}

	// bad numbers
	_, p4 := startRx(t, InboxConfig{}, nil)
	c4 := dial(t, p4)
	for _, bad := range []string{"OFFER", "OFFER x y", "OFFER -1 5", "OFFER 1 -5", "OFFER 1 2 3 4"} {
		c4.send(bad)
		if l := c4.line(); !strings.HasPrefix(l, "ERR 400") {
			t.Errorf("%q -> %s", bad, l)
		}
	}
}

func TestReceiveQuotaCountsWhatIsThere(t *testing.T) {
	cfg := InboxConfig{Dir: filepath.Join(t.TempDir(), "inbox"), Quota: 1500}
	os.MkdirAll(cfg.Dir, 0o755)
	os.WriteFile(filepath.Join(cfg.Dir, "OLD.BIN"), make([]byte, 1000), 0o644)
	_, port := startRx(t, cfg, nil)
	files := []fakeFile{ff("A.TXT", strings.Repeat("a", 600))} // 1000 + 600 > 1500
	if st := dial(t, port).offer(files, ""); !strings.HasPrefix(st, "ERR 507") {
		t.Errorf("%s", st)
	}
}

func TestReceiveHelloAnnouncesUpload(t *testing.T) {
	_, port := startRx(t, InboxConfig{}, nil)
	c := dial(t, port)
	c.send("HELLO")
	if l := c.line(); l != "OK EASYCOMMANDER 1 UPLOAD" {
		t.Errorf("HELLO: %q", l)
	}
	// sharing is off: reading is refused
	c.send("LIST \\")
	if l := c.line(); !strings.HasPrefix(l, "ERR 403") {
		t.Errorf("LIST with sharing off: %q", l)
	}
}

func TestSharingAndReceivingAreIndependent(t *testing.T) {
	share := t.TempDir()
	os.WriteFile(filepath.Join(share, "S.TXT"), []byte("shared"), 0o644)
	port := freePort(t)
	h := NewHost()
	cfg := InboxConfig{Dir: filepath.Join(t.TempDir(), "in")}
	if err := h.StartReceiving(port, "", cfg, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(h.Stop)
	if err := h.Start(share, port, ""); err != nil {
		t.Fatalf("sharing next to receiving: %v", err)
	}
	if !h.Running() || !h.Receiving() {
		t.Fatal("both should be on")
	}
	c := dial(t, port)
	c.send("LIST \\")
	if l := c.line(); l != "OK 1" {
		t.Fatalf("LIST: %q", l)
	}
	c.line()

	h.StopSharing()
	if h.Running() || !h.Receiving() {
		t.Fatal("only sharing should be off")
	}
	c.send("LIST \\")
	if l := c.line(); !strings.HasPrefix(l, "ERR 403") {
		t.Errorf("LIST after StopSharing: %q", l)
	}
	files := []fakeFile{ff("A.TXT", "a")}
	if st := c.offer(files, "X"); st != "OK" {
		t.Errorf("receiving should still work: %s", st)
	}
	c.serve(files)

	h.StopReceiving()
	if h.Receiving() {
		t.Fatal("still receiving")
	}
	// the inbox must not be the shared folder, and the other way round
	h2 := NewHost()
	if err := h2.Start(share, freePort(t), ""); err != nil {
		t.Fatal(err)
	}
	defer h2.Stop()
	if err := h2.StartReceiving(h2.Port, "", InboxConfig{Dir: share}, nil); err == nil {
		t.Error("the inbox may not be the shared folder")
	}
}

func TestReceiveAsk(t *testing.T) {
	files := []fakeFile{ff("A.TXT", "asked")}
	var mu sync.Mutex
	var seen OfferInfo
	answer := true
	confirm := func(ctx context.Context, info OfferInfo) bool {
		mu.Lock()
		defer mu.Unlock()
		seen = info
		return answer
	}
	h, port := startRx(t, InboxConfig{Ask: true}, confirm)

	c := dial(t, port)
	if st := c.offer(files, "PC"); st != "OK" {
		t.Fatal(st)
	}
	c.serve(files)
	mu.Lock()
	if seen.Files != 1 || seen.Bytes != 5 || seen.Label != "PC" || seen.Remote != "127.0.0.1" || !strings.HasPrefix(seen.Dest, "PC_") {
		t.Errorf("what the person was shown: %+v", seen)
	}
	answer = false
	mu.Unlock()

	if st := dial(t, port).offer(files, "PC"); !strings.HasPrefix(st, "ERR 401") {
		t.Errorf("declined: %s", st)
	}
	es, _ := os.ReadDir(h.rx.cfg.Dir)
	if len(es) != 1 {
		t.Errorf("a declined offer must not create a folder, got %d", len(es))
	}

	// Ask on, but nobody to ask: refuse
	_, p2 := startRx(t, InboxConfig{Ask: true}, nil)
	if st := dial(t, p2).offer(files, ""); !strings.HasPrefix(st, "ERR 401") {
		t.Errorf("no confirm function: %s", st)
	}
}

func TestReceiveAskTimeoutSendsWait(t *testing.T) {
	files := []fakeFile{ff("A.TXT", "x")}
	released := make(chan struct{})
	confirm := func(ctx context.Context, info OfferInfo) bool {
		select {
		case <-ctx.Done():
		case <-released:
		}
		return true // too late: must be ignored
	}
	h, port := startRx(t, InboxConfig{Ask: true}, confirm)
	defer close(released)
	h.rx.askWait = 300 * time.Millisecond

	c := dial(t, port)
	n, total := counts(files)
	c.send(fmt.Sprintf("OFFER %d %d", n, total))
	if st := c.status(); !strings.HasPrefix(st, "ERR 408") {
		t.Errorf("no answer: %s", st)
	}
}

func TestReceiveOnlyOneAtATime(t *testing.T) {
	files := []fakeFile{ff("A.TXT", "x")}
	h, port := startRx(t, InboxConfig{}, nil)
	c1 := dial(t, port)
	if st := c1.offer(files, "ONE"); st != "OK" {
		t.Fatal(st)
	}
	// c1 now has the host asking it for TREE; it does not answer yet
	waitFor(t, "first transfer to start", func() bool { return h.rx.busy.Load() })
	if st := dial(t, port).offer(files, "TWO"); !strings.HasPrefix(st, "ERR 409") {
		t.Errorf("second transfer: %s", st)
	}
	c1.serve(files)
}

func TestReceiveRefusesBadNames(t *testing.T) {
	bad := []string{`..\EVIL.TXT`, `CON`, `NUL.TXT`, `COM1`, `LPT9.DAT`, "LONGFILENAME.TXT", "A B.TXT", `SUB\..\X`, "AUX", `C:\X`, "/ETC", `A.B.C`, "ÉÁ.TXT", ""}
	for _, name := range bad {
		files := []fakeFile{ff("OK.TXT", "x")}
		files[0].rel = name
		h, port := startRx(t, InboxConfig{}, nil)
		c := dial(t, port)
		n, total := 1, int64(1)
		c.send(fmt.Sprintf("OFFER %d %d", n, total))
		if st := c.status(); st != "OK" {
			t.Fatalf("%q: %s", name, st)
		}
		end := c.serve(files)
		if !strings.HasPrefix(end, "ABORT") {
			t.Errorf("%q should be refused, got %q", name, end)
		}
		sess := onlyChild(t, h.rx.cfg.Dir)
		var found []string
		filepath.Walk(h.rx.cfg.Dir, func(p string, i os.FileInfo, err error) error {
			if err == nil && !i.IsDir() && !strings.HasSuffix(p, reportName) {
				found = append(found, p)
			}
			return nil
		})
		if len(found) != 0 {
			t.Errorf("%q: files were written: %v (session %s)", name, found, sess)
		}
		// nothing outside the session folder either
		if es, _ := os.ReadDir(filepath.Dir(h.rx.cfg.Dir)); len(es) != 1 {
			t.Errorf("%q: something appeared next to the inbox", name)
		}
	}
}

func TestReceiveRefusesBrokenLists(t *testing.T) {
	cases := map[string][]fakeFile{
		"parent missing":  {ff(`SUB\A.TXT`, "x")},
		"duplicate":       {ff("A.TXT", "x"), ff("a.txt", "x")},
		"more than offer": {ff("A.TXT", "x"), ff("B.TXT", "y")},
	}
	for name, list := range cases {
		h, port := startRx(t, InboxConfig{}, nil)
		c := dial(t, port)
		c.send("OFFER 1 1") // one file of one byte
		if st := c.status(); st != "OK" {
			t.Fatal(st)
		}
		if end := c.serve(list); !strings.HasPrefix(end, "ABORT") {
			t.Errorf("%s: %q", name, end)
		}
		_ = h
	}
	// a file bigger than offered
	_, port := startRx(t, InboxConfig{}, nil)
	c := dial(t, port)
	c.send("OFFER 1 3")
	c.status()
	if end := c.serve([]fakeFile{ff("A.TXT", "12345")}); !strings.HasPrefix(end, "ABORT") {
		t.Errorf("bigger than offered: %q", end)
	}
}

func TestReceiveClientLiesAboutSize(t *testing.T) {
	// The list says 5000 bytes but the file has only 100: the client answers
	// GET with too little data. The host must not hang or accept it as complete.
	_, port := startRx(t, InboxConfig{}, nil)
	c := dial(t, port)
	c.send("OFFER 1 5000")
	c.status()
	c.line() // TREE
	fmt.Fprintf(c.conn, "OK 1 5000\r\nF\tA.DAT\t5000\t20200304\t1234\r\n")
	c.line() // GET \A.DAT 0 16384 -> only 5000 wanted
	fmt.Fprintf(c.conn, "OK 100 5000\r\n")
	c.conn.Write(make([]byte, 100))
	c.line() // next GET at offset 100
	fmt.Fprintf(c.conn, "OK 0 5000\r\n")
	end := c.line()
	if !strings.HasPrefix(end, "ABORT") {
		t.Errorf("end: %q", end)
	}
}

func TestReceiveSwitchOffStopsTransfer(t *testing.T) {
	files := []fakeFile{ff("A.TXT", "x")}
	h, port := startRx(t, InboxConfig{}, nil)
	c := dial(t, port)
	if st := c.offer(files, "X"); st != "OK" {
		t.Fatal(st)
	}
	c.line() // TREE arrives, we do not answer
	h.StopReceiving()
	c.conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	if _, err := io.Copy(io.Discard, c.conn); err != nil && !strings.Contains(err.Error(), "reset") {
		t.Errorf("connection should be closed: %v", err)
	}
}

func TestInLocalNets(t *testing.T) {
	_, n22, _ := net.ParseCIDR("192.168.0.22/24")
	n22.IP = net.ParseIP("192.168.0.22") // an interface address keeps its own host part
	wide := &net.IPNet{IP: net.ParseIP("10.0.4.7"), Mask: net.CIDRMask(16, 32)}
	addrs := []net.Addr{&net.IPNet{IP: net.ParseIP("192.168.0.22"), Mask: net.CIDRMask(24, 32)}, wide}
	yes := []string{"192.168.0.1", "192.168.0.254", "10.0.200.9"}
	no := []string{"192.168.1.1", "192.168.0.256", "10.1.0.1", "8.8.8.8", "172.16.0.5"}
	for _, s := range yes {
		if ip := net.ParseIP(s); ip == nil || !inLocalNets(ip, addrs) {
			t.Errorf("%s should be accepted", s)
		}
	}
	for _, s := range no {
		if ip := net.ParseIP(s); ip != nil && inLocalNets(ip, addrs) {
			t.Errorf("%s should be refused", s)
		}
	}
	if inLocalNets(net.ParseIP("fe80::1"), addrs) {
		t.Error("IPv6 should be refused")
	}
	_ = bytes.MinRead
}

func TestInboxAllowedUsesNetworksAndAllowList(t *testing.T) {
	h := NewHost()
	h.ifaceAddrs = func() ([]net.Addr, error) {
		return []net.Addr{&net.IPNet{IP: net.ParseIP("192.168.0.22"), Mask: net.CIDRMask(23, 32)}}, nil
	}
	_, extra, _ := net.ParseCIDR("172.20.0.0/16")
	rx := &Receiver{cfg: InboxConfig{Allow: []*net.IPNet{extra}}}
	for ip, want := range map[string]bool{
		"127.0.0.1": true, "192.168.0.5": true, "192.168.1.9": true, "192.168.2.1": false,
		"172.20.3.4": true, "172.21.0.1": false, "10.0.0.1": false,
	} {
		if got := h.inboxAllowed(rx, net.ParseIP(ip)); got != want {
			t.Errorf("%s: got %v want %v", ip, got, want)
		}
	}
	h.ifaceAddrs = func() ([]net.Addr, error) { return nil, fmt.Errorf("no interfaces") }
	if h.inboxAllowed(&Receiver{}, net.ParseIP("192.168.0.5")) {
		t.Error("with no interface information only loopback may pass")
	}
}

func TestCleanDOSComponent(t *testing.T) {
	ok := []string{"A.TXT", "readme.txt", "FILE", "A~1.EXE", "X-Y_Z.1", "COMMAND.COM", "COM10", "CONFIG.SYS"}
	bad := []string{"", "CON", "con.txt", "NUL", "PRN.X", "AUX", "COM1", "LPT3.TXT", "CLOCK$", "A B", "..", ".", "TOOLONGNAME.TXT", "A.TOOL", `A\B`, "A/B", "A:B", "A*"}
	for _, s := range ok {
		if _, good := cleanDOSComponent(s); !good {
			t.Errorf("%q should be accepted", s)
		}
	}
	for _, s := range bad {
		if _, good := cleanDOSComponent(s); good {
			t.Errorf("%q should be refused", s)
		}
	}
}

func TestCleanLabel(t *testing.T) {
	for in, want := range map[string]string{"486_C": "486_C", "../../x": "x", "a b": "ab", "": "", "ÉÁ": "", "0123456789012345678901234": "01234567890123456789"} {
		if got := cleanLabel(in); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
}
