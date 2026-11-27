package main

// Program start and the console version. On Windows the program opens a
// window (gui_windows.go) unless it is started with -console.

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type options struct {
	dir     string
	port    int
	name    string
	dirSet  bool // given on the command line (overrides the saved settings)
	portSet bool
	nameSet bool

	// receiving (the other direction), console version only
	inbox   string
	quota   int64
	reserve int64
	noAsk   bool
	yes     bool
	allow   []*net.IPNet
	noShare bool
}

// parseSize reads 500, 100K, 500M, 4G, 1T (powers of 1024) into bytes.
func parseSize(s string) (int64, error) {
	s = strings.TrimSpace(strings.ToUpper(s))
	mult := int64(1)
	if s != "" {
		switch s[len(s)-1] {
		case 'K':
			mult = 1 << 10
		case 'M':
			mult = 1 << 20
		case 'G':
			mult = 1 << 30
		case 'T':
			mult = 1 << 40
		}
		if mult != 1 {
			s = s[:len(s)-1]
		}
	}
	n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if err != nil || n < 0 || n > (int64(1)<<62)/mult {
		return 0, fmt.Errorf("bad size")
	}
	return n * mult, nil
}

// parseAllow reads a comma separated list of networks like 172.20.0.0/16.
func parseAllow(s string) ([]*net.IPNet, error) {
	var out []*net.IPNet
	for _, f := range strings.Split(s, ",") {
		f = strings.TrimSpace(f)
		if f == "" {
			continue
		}
		_, n, err := net.ParseCIDR(f)
		if err != nil || n.IP.To4() == nil {
			return nil, fmt.Errorf("%q is not an IPv4 network like 192.168.0.0/24", f)
		}
		out = append(out, n)
	}
	return out, nil
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "\nERROR: "+format+"\n", args...)
	fmt.Print("Press Enter to exit...")
	bufio.NewReader(os.Stdin).ReadString('\n')
	os.Exit(1)
}

func main() {
	dirFlag := flag.String("dir", "", "folder to share (or give it as the first argument / drop it on the program)")
	port := flag.Int("port", defaultPort, "TCP port to listen on (and UDP port for host searches)")
	name := flag.String("name", "", "name shown to clients that search for hosts (default: the computer name)")
	console := flag.Bool("console", false, "no window: print to the console (Windows; other systems always do)")
	inbox := flag.String("inbox", "", "console only: folder that receives what DOS clients send (switches receiving on)")
	quota := flag.String("quota", "4G", "most the inbox may hold in total, like 500M or 4G (0 = only the free disk space limits)")
	reserve := flag.String("reserve", "1G", "free disk space that must remain after receiving")
	noAsk := flag.Bool("no-ask", false, "do not ask before every incoming transfer (you confirm once at the start, or give -yes)")
	yes := flag.Bool("yes", false, "with -no-ask: do not ask even at the start")
	allow := flag.String("allow", "", "networks that may send besides the ones this computer is in, like 172.20.0.0/16 (comma separated)")
	noShare := flag.Bool("no-share", false, "only receive, do not share a folder")
	flag.Parse()

	o := options{dir: *dirFlag, port: *port, name: *name}
	flag.Visit(func(f *flag.Flag) {
		switch f.Name {
		case "dir":
			o.dirSet = true
		case "port":
			o.portSet = true
		case "name":
			o.nameSet = true
		}
	})
	if o.dir == "" && flag.NArg() > 0 {
		o.dir = flag.Arg(0)
		o.dirSet = true
	}

	o.inbox, o.noAsk, o.yes, o.noShare = *inbox, *noAsk, *yes, *noShare
	var err error
	if o.quota, err = parseSize(*quota); err != nil {
		fatal("-quota: %v", err)
	}
	if o.reserve, err = parseSize(*reserve); err != nil {
		fatal("-reserve: %v", err)
	}
	if o.allow, err = parseAllow(*allow); err != nil {
		fatal("-allow: %v", err)
	}
	if o.noShare && o.inbox == "" {
		fatal("-no-share needs -inbox")
	}

	if guiAvailable && !*console && o.inbox == "" {
		runGUI(o)
		return
	}
	attachConsole()
	runConsole(o)
}

func interactive() bool {
	fi, err := os.Stdin.Stat()
	return err == nil && fi.Mode()&os.ModeCharDevice != 0
}

// consoleAsker asks the person at the keyboard. Lines are read by one
// goroutine only, so an answer that comes too late is never taken for the
// answer to the next question.
func consoleAsker() func(context.Context, OfferInfo) bool {
	lines := make(chan string, 4)
	go func() {
		in := bufio.NewReader(os.Stdin)
		for {
			l, err := in.ReadString('\n')
			if err != nil {
				close(lines)
				return
			}
			lines <- strings.TrimSpace(l)
		}
	}()
	return func(ctx context.Context, info OfferInfo) bool {
		if !interactive() {
			return false
		}
		for len(lines) > 0 { // forget stale input
			<-lines
		}
		fmt.Printf("\n%s wants to send %d files, %s.\n", info.Remote, info.Files, humanBytes(info.Bytes))
		fmt.Printf("They would go into the inbox folder \\%s\n", info.Dest)
		if info.Free >= 0 {
			fmt.Printf("Free disk space: %s\n", humanBytes(info.Free))
		}
		fmt.Print("Accept? [y/N] ")
		select {
		case l, ok := <-lines:
			return ok && (l == "y" || l == "Y" || strings.EqualFold(l, "yes"))
		case <-ctx.Done():
			fmt.Println("(no answer, declined)")
			return false
		}
	}
}

func humanBytes(n int64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KB", float64(n)/(1<<10))
	}
	return fmt.Sprintf("%d bytes", n)
}

func runConsole(o options) {
	h := NewHost()

	if !o.noShare {
		dir := o.dir
		if dir == "" {
			cwd, err := os.Getwd()
			if err != nil {
				fatal("cannot determine current folder: %v", err)
			}
			dir = filepath.Join(cwd, "share")
			if err := os.MkdirAll(dir, 0o755); err != nil {
				fatal("cannot create %s: %v", dir, err)
			}
		}
		if err := h.Start(dir, o.port, o.name); err != nil {
			fatal("%v", err)
		}
	}

	if o.inbox != "" {
		if o.noAsk && !o.yes {
			if !interactive() {
				fatal("-no-ask without a person at the keyboard needs -yes")
			}
			fmt.Printf("Without asking, ANY computer in the allowed networks can fill %s\n", o.inbox)
			fmt.Print("(up to the quota) until you stop the program. Type YES to start: ")
			l, _ := bufio.NewReader(os.Stdin).ReadString('\n')
			if strings.TrimSpace(l) != "YES" {
				fatal("cancelled")
			}
		}
		cfg := InboxConfig{Dir: o.inbox, Quota: o.quota, Reserve: o.reserve, Ask: !o.noAsk, Allow: o.allow}
		var ask func(context.Context, OfferInfo) bool
		if cfg.Ask {
			ask = consoleAsker()
		}
		if err := h.StartReceiving(o.port, o.name, cfg, ask); err != nil {
			fatal("%v", err)
		}
	}

	switch {
	case h.Running() && h.Receiving():
		fmt.Printf("Easy Commander host (sharing, and receiving into the inbox)\n")
	case h.Receiving():
		fmt.Printf("Easy Commander host (receiving only)\n")
	default:
		fmt.Printf("Easy Commander host (read-only)\n")
	}
	if h.Running() {
		fmt.Printf("Sharing folder: %s\n", h.Dir)
	}
	if h.Receiving() {
		q := "no quota (the free disk space limits)"
		if o.quota > 0 {
			q = "quota " + humanBytes(o.quota)
		}
		fmt.Printf("Inbox folder: %s, %s, ", h.InboxDir, q)
		if o.noAsk {
			fmt.Println("no question before a transfer")
		} else {
			fmt.Println("asks before every transfer")
		}
		fmt.Println("Only computers in the networks of this computer may send (more with -allow).")
	}
	fmt.Printf("Port: %d\n", h.Port)
	if ips := localIPs(); len(ips) > 0 {
		fmt.Println("Enter one of these addresses in the DOS client:")
		for _, ip := range ips {
			fmt.Printf("  %s\n", ip)
		}
	}
	fmt.Println("Only local network (private) addresses are accepted. Ctrl+C to stop.")

	if h.DiscoveryErr != nil {
		fmt.Printf("Host search is off (UDP port %d: %v)\n", h.Port, h.DiscoveryErr)
	} else {
		fmt.Printf("Answering host searches on UDP port %d as %q\n", h.Port, h.Label)
	}
	fmt.Println()

	select {} // serve until the program is stopped (Ctrl+C)
}
