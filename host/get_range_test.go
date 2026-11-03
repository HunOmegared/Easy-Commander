package main

import (
	"bufio"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
)

// getRange sends one GET command through an in-memory connection and
// returns the header line and the bytes that followed it.
func getRange(t *testing.T, s *server, arg string, want int) (string, string) {
	t.Helper()
	srv, cli := net.Pipe()
	defer cli.Close()
	go func() {
		w := bufio.NewWriter(srv)
		s.cmdGet(srv, w, arg)
		srv.Close()
	}()
	r := bufio.NewReader(cli)
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	body := make([]byte, want)
	if want > 0 {
		if _, err := io.ReadFull(r, body); err != nil {
			t.Fatal(err)
		}
	}
	return line, string(body)
}

func TestGetRanges(t *testing.T) {
	root := t.TempDir()
	os.WriteFile(filepath.Join(root, "DIGITS.TXT"), []byte("0123456789"), 0o644)
	s := &server{root: root}

	cases := []struct {
		arg, header, body string
	}{
		{`DIGITS.TXT`, "OK 10 10\r\n", "0123456789"},
		{`DIGITS.TXT 2 5`, "OK 5 10\r\n", "23456"},
		{`DIGITS.TXT 8 100`, "OK 2 10\r\n", "89"},
		{`DIGITS.TXT 10 5`, "OK 0 10\r\n", ""},
		{`DIGITS.TXT 0 0`, "OK 0 10\r\n", ""},
	}
	for _, c := range cases {
		h, b := getRange(t, s, c.arg, len(c.body))
		if h != c.header || b != c.body {
			t.Errorf("%q: got %q %q, want %q %q", c.arg, h, b, c.header, c.body)
		}
	}
}
