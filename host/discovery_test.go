package main

import (
	"net"
	"testing"
	"time"
)

func TestDiscoveryReply(t *testing.T) {
	pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Skip("no UDP on the loopback interface")
	}
	defer pc.Close()
	go serveDiscovery(pc, 6502, "TESTHOST")

	c, err := net.Dial("udp4", pc.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	c.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := c.Write([]byte("ECDISC 1\r\n")); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 100)
	n, err := c.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(buf[:n]), "ECHOST 1 6502 TESTHOST\r\n"; got != want {
		t.Fatalf("got %q, want %q", got, want)
	}

	// Anything else gets no answer.
	if _, err := c.Write([]byte("HELLO\r\n")); err != nil {
		t.Fatal(err)
	}
	c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if n, err := c.Read(buf); err == nil {
		t.Fatalf("unexpected answer %q", buf[:n])
	}
}

func TestSanitizeName(t *testing.T) {
	cases := []struct{ in, want string }{
		{"RETRO-PC", "RETRO-PC"},
		{"Gépház", "G?ph?z"},
		{"", "easycommander"},
		{"   ", "easycommander"},
		{"abcdefghijklmnopqrstuvwxyz0123456789", "abcdefghijklmnopqrstuvwx"},
	}
	for _, c := range cases {
		if got := sanitizeName(c.in); got != c.want {
			t.Errorf("sanitizeName(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}
