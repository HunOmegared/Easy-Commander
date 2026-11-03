package main

import (
	"bufio"
	"bytes"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCmdCRC(t *testing.T) {
	root := t.TempDir()
	data := []byte("hello")
	os.WriteFile(filepath.Join(root, "Hello World.txt"), data, 0o644)
	s := &server{root: root}

	var out bytes.Buffer
	w := bufio.NewWriter(&out)

	s.cmdCRC(w, `HELLOW~1.TXT`)
	want := fmt.Sprintf("OK %d %08X\r\n", len(data), crc32.ChecksumIEEE(data))
	if out.String() != want {
		t.Fatalf("got %q, want %q", out.String(), want)
	}

	out.Reset()
	s.cmdCRC(w, `/NOPE.TXT`)
	if !strings.HasPrefix(out.String(), "ERR 404") {
		t.Fatalf("got %q", out.String())
	}
}
