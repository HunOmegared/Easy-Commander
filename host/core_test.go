package main

import (
	"bufio"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func freePort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

func waitFor(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for i := 0; i < 100; i++ {
		if ok() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timeout: %s", what)
}

func TestHostStartStop(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "A.TXT"), []byte("hello"), 0o644)
	port := freePort(t)

	h := NewHost()
	if h.Running() {
		t.Fatal("running before Start")
	}
	if err := h.Start(dir, port, "My PC"); err != nil {
		t.Fatal(err)
	}
	if !h.Running() || h.Port != port || h.Label != "My PC" {
		t.Fatalf("state after Start: running=%v port=%d label=%q", h.Running(), h.Port, h.Label)
	}
	if err := h.Start(dir, port, ""); err == nil {
		t.Fatal("second Start must fail")
	}

	c, err := net.Dial("tcp4", net.JoinHostPort("127.0.0.1", itoa(port)))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	r := bufio.NewReader(c)
	c.Write([]byte("HELLO\r\n"))
	line, _ := r.ReadString('\n')
	if strings.TrimSpace(line) != "OK EASYCOMMANDER 1" {
		t.Fatalf("HELLO answer %q", line)
	}
	waitFor(t, "one client", func() bool { return h.Clients() == 1 })

	// A busy port is reported, not hidden.
	if err := NewHost().Start(dir, port, ""); err == nil {
		t.Fatal("Start on a busy port must fail")
	}

	h.Stop()
	if h.Running() {
		t.Fatal("still running after Stop")
	}
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := r.ReadString('\n'); err == nil {
		t.Fatal("the client connection must be closed by Stop")
	}
	waitFor(t, "no clients", func() bool { return h.Clients() == 0 })

	// It can be started again on the same port.
	if err := h.Start(dir, port, ""); err != nil {
		t.Fatalf("restart: %v", err)
	}
	h.Stop()
}

func itoa(n int) string { return strconv.Itoa(n) }

func TestHostStartErrors(t *testing.T) {
	h := NewHost()
	if err := h.Start(filepath.Join(t.TempDir(), "nope"), 6502, ""); err == nil {
		t.Error("missing folder must fail")
	}
	if err := h.Start(t.TempDir(), 0, ""); err == nil {
		t.Error("port 0 must fail")
	}
	if err := h.Start(t.TempDir(), 70000, ""); err == nil {
		t.Error("port 70000 must fail")
	}
}

func TestLogRing(t *testing.T) {
	var r logRing
	lines, next := r.since(0)
	if len(lines) != 0 || next != 0 {
		t.Fatal("empty ring")
	}
	r.add("a")
	r.add("b")
	lines, next = r.since(0)
	if strings.Join(lines, ",") != "a,b" || next != 2 {
		t.Fatalf("got %v %d", lines, next)
	}
	r.add("c")
	lines, next = r.since(next)
	if strings.Join(lines, ",") != "c" || next != 3 {
		t.Fatalf("got %v %d", lines, next)
	}
	for i := 0; i < logRingMax+10; i++ {
		r.add("x")
	}
	lines, next = r.since(0) // asks for lines that were dropped: gets what is left
	if len(lines) != logRingMax || next != 3+logRingMax+10 {
		t.Fatalf("after overflow: %d lines, next %d", len(lines), next)
	}
}

func TestQuietCommand(t *testing.T) {
	if quietCommand("GET", "\\A.TXT") || quietCommand("GET", "\\A.TXT 0 65536") || quietCommand("LIST", "\\") {
		t.Error("these must be logged")
	}
	if !quietCommand("GET", "\\A.TXT 65536 65536") {
		t.Error("later blocks must not be logged")
	}
}

func TestConfig(t *testing.T) {
	c := parseConfig("")
	if c.Port != 6502 || c.Dir != "" || c.Hidden {
		t.Fatalf("defaults: %+v", c)
	}
	c = parseConfig("dir=C:\\Share Me\r\nport=7000\r\nname=Workshop PC\r\nhidden=1\r\n")
	if c.Dir != "C:\\Share Me" || c.Port != 7000 || c.Name != "Workshop PC" || !c.Hidden {
		t.Fatalf("parsed: %+v", c)
	}
	if parseConfig("port=99999").Port != 6502 || parseConfig("port=abc").Port != 6502 {
		t.Error("bad ports fall back to the default")
	}
	if got := parseConfig(c.String()); got != c {
		t.Fatalf("round trip: %+v != %+v", got, c)
	}
	path := filepath.Join(t.TempDir(), "echost.ini")
	if loadConfig(path).Port != 6502 {
		t.Error("missing file gives defaults")
	}
	if err := saveConfig(path, c); err != nil {
		t.Fatal(err)
	}
	if loadConfig(path) != c {
		t.Error("save and load")
	}
}
