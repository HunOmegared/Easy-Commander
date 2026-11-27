//go:build readonly

package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The read-only build shares, but cannot receive or write anything for a client.
func TestReadOnlyBuild(t *testing.T) {
	share := t.TempDir()
	os.WriteFile(filepath.Join(share, "A.TXT"), []byte("hello"), 0o644)
	ln, _ := net.Listen("tcp4", "127.0.0.1:0")
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()

	h := NewHost()
	if err := h.Start(share, port, "T"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(h.Stop)

	if err := h.StartReceiving(port, "T", InboxConfig{Dir: t.TempDir()}, nil); err == nil {
		t.Error("receiving must not start in the read-only build")
	}

	conn, err := net.Dial("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	r := bufio.NewReader(conn)
	ask := func(cmd string) string {
		conn.SetDeadline(time.Now().Add(5 * time.Second))
		fmt.Fprintf(conn, "%s\r\n", cmd)
		l, err := r.ReadString('\n')
		if err != nil {
			t.Fatalf("%s: %v", cmd, err)
		}
		return strings.TrimSpace(l)
	}

	if l := ask("HELLO"); l != "OK EASYCOMMANDER 1" {
		t.Errorf("HELLO must not offer UPLOAD: %q", l)
	}
	for _, c := range []string{`OFFER 1 5 \`, `IMKDIR \X`, `ILIST \`, `IGET \A.TXT`} {
		if l := ask(c); !strings.HasPrefix(l, "ERR 403") {
			t.Errorf("%s: %q", c, l)
		}
	}
	if l := ask(`LIST \`); l != "OK 1" {
		t.Errorf("sharing still works: %q", l)
	}
	if _, err := os.Stat(filepath.Join(share, "X")); err == nil {
		t.Error("a folder was made")
	}
}
