package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParseSize(t *testing.T) {
	good := map[string]int64{"0": 0, "500": 500, "100K": 100 << 10, "500m": 500 << 20, "4G": 4 << 30, " 2 T ": 2 << 40, "1g": 1 << 30}
	for in, want := range good {
		if got, err := parseSize(in); err != nil || got != want {
			t.Errorf("%q -> %d, %v (want %d)", in, got, err, want)
		}
	}
	for _, in := range []string{"", "G", "-1", "1.5G", "abc", "4X", "99999999999999T"} {
		if _, err := parseSize(in); err == nil {
			t.Errorf("%q should be refused", in)
		}
	}
}

func TestParseAllow(t *testing.T) {
	n, err := parseAllow("172.20.0.0/16, 10.1.2.0/24,")
	if err != nil || len(n) != 2 || n[0].String() != "172.20.0.0/16" {
		t.Fatalf("%v %v", n, err)
	}
	for _, bad := range []string{"172.20.0.0", "x/8", "fe80::/10", "1.2.3.4/33"} {
		if _, err := parseAllow(bad); err == nil {
			t.Errorf("%q should be refused", bad)
		}
	}
	if n, err := parseAllow(""); err != nil || len(n) != 0 {
		t.Error("empty list")
	}
}

func TestConfigInboxSettings(t *testing.T) {
	c := parseConfig("")
	if c.Ask != true || c.QuotaMB != defaultQuotaMB || c.InDir != "" {
		t.Errorf("defaults: %+v", c)
	}
	c = parseConfig("inbox=D:\\Backups\nquota=51200\nask=0\n")
	if c.InDir != "D:\\Backups" || c.QuotaMB != 51200 || c.Ask {
		t.Errorf("read: %+v", c)
	}
	if parseConfig("quota=-5").QuotaMB != defaultQuotaMB || parseConfig("quota=abc").QuotaMB != defaultQuotaMB {
		t.Error("bad quota must keep the default")
	}
	if parseConfig("quota=0").QuotaMB != 0 {
		t.Error("quota 0 means no quota")
	}
	path := filepath.Join(t.TempDir(), "echost.ini")
	want := config{Dir: "C:\\Share", Port: 7001, Name: "PC", Hidden: true, InDir: "E:\\In", QuotaMB: 123, Ask: false}
	if err := saveConfig(path, want); err != nil {
		t.Fatal(err)
	}
	if got := loadConfig(path); got != want {
		t.Errorf("round trip: %+v != %+v", got, want)
	}
	// an old file without the inbox lines keeps working with the defaults
	os.WriteFile(path, []byte("dir=C:\\Old\nport=6502\nname=\nhidden=0\n"), 0o644)
	if got := loadConfig(path); got.Dir != "C:\\Old" || !got.Ask || got.QuotaMB != defaultQuotaMB {
		t.Errorf("old file: %+v", got)
	}
}

func TestReceiveWaitKeepAlive(t *testing.T) {
	old := waitTick
	waitTick = 40 * time.Millisecond
	defer func() { waitTick = old }()

	files := []fakeFile{ff("A.TXT", "x")}
	release := make(chan struct{})
	confirm := func(ctx context.Context, info OfferInfo) bool {
		<-release
		return true
	}
	_, port := startRx(t, InboxConfig{Ask: true}, confirm)
	c := dial(t, port)
	n, total := counts(files)
	c.send(fmt.Sprintf("OFFER %d %d", n, total))
	waits := 0
	go func() { time.Sleep(250 * time.Millisecond); close(release) }()
	for {
		l := c.line()
		if l == "WAIT" {
			waits++
			continue
		}
		if l != "OK" {
			t.Fatalf("got %q", l)
		}
		break
	}
	if waits < 2 {
		t.Errorf("the client should hear WAIT while the host waits for the answer, heard %d", waits)
	}
	if end := c.serve(files); !strings.HasPrefix(end, "END 1 0") {
		t.Errorf("end: %s", end)
	}
}

func TestQuotaText(t *testing.T) {
	for in, want := range map[string]int{"0": 0, "4096": 4096, "4 GB": 4096, "4g": 4096, "500MB": 500, "500 mb": 500, "2T": 2 * 1024 * 1024, " 1 gb ": 1024, "": 0} {
		got, err := parseQuotaText(in)
		if in == "" {
			if err == nil {
				t.Error("empty text is not a quota")
			}
			continue
		}
		if err != nil || got != want {
			t.Errorf("%q -> %d, %v (want %d)", in, got, err, want)
		}
	}
	for _, bad := range []string{"abc", "-1", "1.5 GB", "100K", "GB", "99999999999 GB"} {
		if _, err := parseQuotaText(bad); err == nil {
			t.Errorf("%q should be refused", bad)
		}
	}
	for mb, want := range map[int]string{0: "0", 4096: "4 GB", 500: "500 MB", 1536: "1536 MB", 1024 * 1024: "1 TB"} {
		if got := quotaText(mb); got != want {
			t.Errorf("quotaText(%d) = %q, want %q", mb, got, want)
		}
		if back, err := parseQuotaText(quotaText(mb)); err != nil || back != mb {
			t.Errorf("round trip %d -> %q -> %d, %v", mb, quotaText(mb), back, err)
		}
	}
}
