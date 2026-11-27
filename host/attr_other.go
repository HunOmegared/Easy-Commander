//go:build !windows

package main

import "os"

// setAttrs: only "read-only" has a meaning outside Windows (hidden, system and
// archive are not kept).
func setAttrs(path string, attr uint8) {
	if attr&1 != 0 {
		if st, err := os.Stat(path); err == nil && !st.IsDir() {
			os.Chmod(path, 0o444)
		}
	}
}
