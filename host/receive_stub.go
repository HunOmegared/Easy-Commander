//go:build readonly

package main

// The read-only build: everything that receives or writes files for a client is not
// compiled in at all (see receive.go). The host can only share a folder. This file
// holds the few names the rest of the program still refers to.

import (
	"bufio"
	"context"
	"errors"
	"net"
	"time"
)

const attrMask = 0x27
const defaultReserve = 0
const maxMkdirsPerConn = 100

var (
	errNoUpload = protoErr{403, "Receiving is off"}
	errNoShare  = protoErr{403, "Sharing is off"}
)

// readOnlyBuild is true in this build (shown in the window title and in -version).
const (
	readOnlyBuild = true
	titleSuffix   = " (read-only)"
)

var waitTick = 5 * time.Second

type InboxConfig struct {
	Dir     string
	Quota   int64
	Reserve int64
	Ask     bool
	Allow   []*net.IPNet
}

type OfferInfo struct {
	Remote string
	Label  string
	Files  int
	Bytes  int64
	Dest   string
	Free   int64
	Used   int64
	Quota  int64
}

type Receiver struct{ cfg InboxConfig }

func samePath(a, b string) bool { return false }

func (h *Host) StartReceiving(port int, name string, cfg InboxConfig, confirm func(context.Context, OfferInfo) bool) error {
	return errors.New("this is the read-only build: it can share a folder but cannot receive files")
}
func (h *Host) StopReceiving()    {}
func (h *Host) Receiving() bool   { return false }
func (h *Host) ReceiveBusy() bool { return false }
func (h *Host) ReceiveAsks() bool { return false }
func (h *Host) handleOffer(conn net.Conn, r *bufio.Reader, w *bufio.Writer, arg string) bool {
	writeErr(w, protoErr{403, "Receiving is off"})
	return true
}

func (h *Host) stopReceivingLocked()                   {}
func (h *Host) uploadFor(addr net.Addr) bool           { return false }
func (h *Host) inboxFor(addr net.Addr) string          { return "" }
func (s *server) cmdMkdir(w *bufio.Writer, arg string) { writeErr(w, errNoUpload) }

func (h *Host) shareDir() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.sharing {
		return ""
	}
	return h.shareRoot
}
