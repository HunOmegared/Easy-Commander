package main

// The settings of the window version, kept in a small text file next to the
// program (echost.ini):  dir=...  port=...  name=...  hidden=0|1

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

const defaultPort = 6502

type config struct {
	Dir    string // shared folder ("" = not chosen yet)
	Port   int
	Name   string // "" = the computer name
	Hidden bool   // start in the tray, without the window
}

func parseConfig(text string) config {
	c := config{Port: defaultPort}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		i := strings.IndexByte(line, '=')
		if i < 0 {
			continue
		}
		key, val := strings.ToLower(strings.TrimSpace(line[:i])), strings.TrimSpace(line[i+1:])
		switch key {
		case "dir":
			c.Dir = val
		case "port":
			if n, err := strconv.Atoi(val); err == nil && n >= 1 && n <= 65535 {
				c.Port = n
			}
		case "name":
			c.Name = val
		case "hidden":
			c.Hidden = val == "1"
		}
	}
	return c
}

func (c config) String() string {
	h := 0
	if c.Hidden {
		h = 1
	}
	return fmt.Sprintf("dir=%s\nport=%d\nname=%s\nhidden=%d\n", c.Dir, c.Port, c.Name, h)
}

// loadConfig returns the defaults when the file is missing.
func loadConfig(path string) config {
	b, err := os.ReadFile(path)
	if err != nil {
		return config{Port: defaultPort}
	}
	return parseConfig(string(b))
}

func saveConfig(path string, c config) error {
	return os.WriteFile(path, []byte(c.String()), 0o644)
}
