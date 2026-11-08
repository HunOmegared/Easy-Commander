package main

// Program start and the console version. On Windows the program opens a
// window (gui_windows.go) unless it is started with -console.

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"path/filepath"
)

type options struct {
	dir     string
	port    int
	name    string
	dirSet  bool // given on the command line (overrides the saved settings)
	portSet bool
	nameSet bool
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

	if guiAvailable && !*console {
		runGUI(o)
		return
	}
	attachConsole()
	runConsole(o)
}

func runConsole(o options) {
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

	h := NewHost()
	if err := h.Start(dir, o.port, o.name); err != nil {
		fatal("%v", err)
	}

	fmt.Printf("Easy Commander host (read-only)\n")
	fmt.Printf("Sharing folder: %s\n", h.Dir)
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
