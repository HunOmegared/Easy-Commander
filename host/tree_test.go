package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWalkTreeStructure(t *testing.T) {
	root := t.TempDir()
	mk := func(p string) { os.MkdirAll(filepath.Join(root, p), 0o755) }
	wf := func(p, data string) { os.WriteFile(filepath.Join(root, p), []byte(data), 0o644) }
	mk("Long Folder Name/sub")
	mk("empty")
	wf("Long Folder Name/sub/file one.txt", "abc")
	wf("Long Folder Name/top.txt", "hello")
	wf("a.txt", "x")

	var items []treeItem
	var total int64
	if err := walkTree(root, "", 1, &items, &total); err != nil {
		t.Fatal(err)
	}

	want := []string{
		`D EMPTY`,
		`D LONGFO~1`,
		`D LONGFO~1\SUB`,
		`F LONGFO~1\SUB\FILEON~1.TXT`,
		`F LONGFO~1\TOP.TXT`,
		`F A.TXT`,
	}
	var got []string
	for _, it := range items {
		k := "F"
		if it.Dir {
			k = "D"
		}
		got = append(got, k+" "+it.Rel)
	}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Fatalf("got  %v\nwant %v", got, want)
	}
	if total != 9 {
		t.Fatalf("total = %d, want 9", total)
	}
}

func TestResolveTreePathsAndTraversal(t *testing.T) {
	root := t.TempDir()
	os.MkdirAll(filepath.Join(root, "Long Folder Name", "sub"), 0o755)
	os.WriteFile(filepath.Join(root, "Long Folder Name", "sub", "file one.txt"), []byte("abc"), 0o644)
	s := &server{root: root}

	p, ent, err := s.resolve(`LONGFO~1\SUB\FILEON~1.TXT`)
	if err != nil || ent == nil || ent.Dir {
		t.Fatalf("resolve failed: %v %v", ent, err)
	}
	if filepath.Base(p) != "file one.txt" {
		t.Fatalf("resolved to %q", p)
	}
	if _, _, err := s.resolve(`..\x`); err != errBad {
		t.Fatalf("traversal not rejected: %v", err)
	}
	if _, _, err := s.resolve(`LONGFO~1\..\..\x`); err != errBad {
		t.Fatalf("traversal not rejected: %v", err)
	}
}
