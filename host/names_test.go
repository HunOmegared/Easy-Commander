package main

import "testing"

func names(real ...string) map[string]string {
	es := make([]entry, len(real))
	for i, r := range real {
		es[i] = entry{Real: r}
	}
	assignNames(es)
	m := map[string]string{}
	for _, e := range es {
		m[e.Real] = e.DOS
	}
	return m
}

func TestNativeNamesUppercased(t *testing.T) {
	m := names("doom.exe", "README.TXT")
	if m["doom.exe"] != "DOOM.EXE" || m["README.TXT"] != "README.TXT" {
		t.Fatalf("got %v", m)
	}
}

func TestLongNameMangled(t *testing.T) {
	m := names("README_long.txt")
	if m["README_long.txt"] != "README~1.TXT" {
		t.Fatalf("got %v", m)
	}
}

func TestCollisionsAreNumberedDeterministically(t *testing.T) {
	m := names("Program Files", "Program Files (x86)", "PROGRA~1")
	if m["PROGRA~1"] != "PROGRA~1" {
		t.Fatalf("native name must win: %v", m)
	}
	if m["Program Files"] != "PROGRA~2" || m["Program Files (x86)"] != "PROGRA~3" {
		t.Fatalf("got %v", m)
	}
}

func TestCaseCollision(t *testing.T) {
	m := names("a.txt", "A.TXT")
	if m["A.TXT"] != "A.TXT" || m["a.txt"] != "A~1.TXT" {
		t.Fatalf("got %v", m)
	}
}

func TestIllegalCharsAndAccents(t *testing.T) {
	m := names("árvíztűrő tükörfúrógép.txt")
	got := m["árvíztűrő tükörfúrógép.txt"]
	if len(got) > 12 || got[len(got)-4:] != ".TXT" {
		t.Fatalf("bad 8.3 name %q", got)
	}
}
