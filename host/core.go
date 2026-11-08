package main

// The host without any user interface: start and stop the sharing, count the
// clients, collect a short activity log. The console version (cli.go) and the
// window version (gui_windows.go) both use it.

import (
	"bufio"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// logf writes one line of the activity log. The window version replaces it
// with a function that feeds the list in the window.
var logf = log.Printf

// ---------------------------------------------------------------------
// A short log that the window can read in pieces

type logRing struct {
	mu    sync.Mutex
	lines []string
	first int // number of the first line kept (lines are numbered from 0)
}

const logRingMax = 1000

func (r *logRing) add(s string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.lines = append(r.lines, s)
	if len(r.lines) > logRingMax {
		drop := len(r.lines) - logRingMax
		r.lines = append([]string(nil), r.lines[drop:]...)
		r.first += drop
	}
}

// since returns the lines numbered n and higher, and the number to ask for
// next time.
func (r *logRing) since(n int) ([]string, int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if n < r.first {
		n = r.first
	}
	next := r.first + len(r.lines)
	if n >= next {
		return nil, next
	}
	return append([]string(nil), r.lines[n-r.first:]...), next
}

// ---------------------------------------------------------------------
// Host

type Host struct {
	mu      sync.Mutex
	running bool
	ln      net.Listener
	pc      net.PacketConn
	conns   map[net.Conn]struct{}

	// Valid while running (and after the last run).
	Dir          string
	Port         int
	Label        string // the name announced to searching clients
	DiscoveryErr error  // why host search is off, if it is
}

func NewHost() *Host {
	return &Host{conns: map[net.Conn]struct{}{}}
}

// Start begins sharing dir on the TCP and UDP port. An empty name means the
// computer name.
func (h *Host) Start(dir string, port int, name string) error {
	h.mu.Lock()
	defer h.mu.Unlock()

	if h.running {
		return errors.New("already sharing")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return fmt.Errorf("bad folder: %v", err)
	}
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		return fmt.Errorf("%s is not a folder", abs)
	}
	if port < 1 || port > 65535 {
		return fmt.Errorf("bad port number %d", port)
	}

	ln, err := net.Listen("tcp4", fmt.Sprintf(":%d", port))
	if err != nil {
		return fmt.Errorf("cannot listen on port %d: %v", port, err)
	}

	if name == "" {
		name, _ = os.Hostname()
	}
	label := sanitizeName(name)

	h.DiscoveryErr = nil
	h.pc = nil
	if pc, err := net.ListenPacket("udp4", fmt.Sprintf(":%d", port)); err != nil {
		h.DiscoveryErr = err
	} else {
		h.pc = pc
		go serveDiscovery(pc, port, label)
	}

	h.ln = ln
	h.conns = map[net.Conn]struct{}{}
	h.Dir, h.Port, h.Label = abs, port, label
	h.running = true

	go h.accept(ln, &server{root: abs})
	return nil
}

// Stop ends the sharing and closes every client connection.
func (h *Host) Stop() {
	h.mu.Lock()
	defer h.mu.Unlock()

	if !h.running {
		return
	}
	h.running = false
	h.ln.Close()
	if h.pc != nil {
		h.pc.Close()
	}
	for c := range h.conns {
		c.Close()
	}
}

func (h *Host) Running() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.running
}

// Clients is the number of connected DOS clients.
func (h *Host) Clients() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.conns)
}

func (h *Host) addConn(c net.Conn) {
	h.mu.Lock()
	h.conns[c] = struct{}{}
	h.mu.Unlock()
}

func (h *Host) delConn(c net.Conn) {
	h.mu.Lock()
	delete(h.conns, c)
	h.mu.Unlock()
}

func (h *Host) accept(ln net.Listener, srv *server) {
	sem := make(chan struct{}, maxClients)
	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			logf("accept: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		if !allowedRemote(conn.RemoteAddr()) {
			logf("rejected non-local connection from %s", conn.RemoteAddr())
			conn.Close()
			continue
		}
		select {
		case sem <- struct{}{}:
			h.addConn(conn)
			logf("%s connected", conn.RemoteAddr())
			go func() {
				defer func() {
					<-sem
					h.delConn(conn)
					logf("%s disconnected", conn.RemoteAddr())
				}()
				srv.handle(conn)
			}()
		default:
			w := bufio.NewWriter(conn)
			writeErr(w, errBusy)
			conn.Close()
		}
	}
}
