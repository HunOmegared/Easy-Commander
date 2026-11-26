package main

// The settings of the window version, kept in a small text file next to the
// program (echost.ini):  dir=...  port=...  name=...  hidden=0|1
// and for the inbox:  inbox=...  quota=<megabytes>  ask=0|1

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

const (
	defaultPort    = 6502
	defaultQuotaMB = 4096
)

type config struct {
	Dir    string // shared folder ("" = not chosen yet)
	Port   int
	Name   string // "" = the computer name
	Hidden bool   // start in the tray, without the window

	InDir   string // the inbox folder ("" = not chosen yet)
	QuotaMB int    // most the inbox may hold, in megabytes (0 = only the free space limits)
	Ask     bool   // ask before every incoming transfer
}

func defaultConfig() config {
	return config{Port: defaultPort, QuotaMB: defaultQuotaMB, Ask: true}
}

func parseConfig(text string) config {
	c := defaultConfig()
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
		case "inbox":
			c.InDir = val
		case "quota":
			if n, err := strconv.Atoi(val); err == nil && n >= 0 && n <= 100000000 {
				c.QuotaMB = n
			}
		case "ask":
			c.Ask = val != "0"
		}
	}
	return c
}

func (c config) String() string {
	h := 0
	if c.Hidden {
		h = 1
	}
	a := 0
	if c.Ask {
		a = 1
	}
	return fmt.Sprintf("dir=%s\nport=%d\nname=%s\nhidden=%d\ninbox=%s\nquota=%d\nask=%d\n",
		c.Dir, c.Port, c.Name, h, c.InDir, c.QuotaMB, a)
}

// loadConfig returns the defaults when the file is missing.
func loadConfig(path string) config {
	b, err := os.ReadFile(path)
	if err != nil {
		return defaultConfig()
	}
	return parseConfig(string(b))
}

func saveConfig(path string, c config) error {
	return os.WriteFile(path, []byte(c.String()), 0o644)
}

// quotaText shows a quota in megabytes the way a person writes it.
func quotaText(mb int) string {
	switch {
	case mb == 0:
		return "0"
	case mb%(1024*1024) == 0:
		return fmt.Sprintf("%d TB", mb/(1024*1024))
	case mb%1024 == 0:
		return fmt.Sprintf("%d GB", mb/1024)
	}
	return fmt.Sprintf("%d MB", mb)
}

// parseQuotaText reads "4 GB", "500MB", "2T", "4096" (a plain number means
// megabytes) and returns megabytes. 0 means no quota.
func parseQuotaText(s string) (int, error) {
	t := strings.ToUpper(strings.ReplaceAll(strings.TrimSpace(s), " ", ""))
	t = strings.TrimSuffix(t, "B")
	mult := int64(1)
	if t != "" {
		switch t[len(t)-1] {
		case 'K':
			return 0, fmt.Errorf("use MB, GB or TB")
		case 'M':
			t = t[:len(t)-1]
		case 'G':
			mult, t = 1024, t[:len(t)-1]
		case 'T':
			mult, t = 1024*1024, t[:len(t)-1]
		}
	}
	n, err := strconv.ParseInt(t, 10, 64)
	if err != nil || n < 0 || n*mult > 100000000 {
		return 0, fmt.Errorf("bad quota")
	}
	return int(n * mult), nil
}
