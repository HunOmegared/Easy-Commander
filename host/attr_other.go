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

// fileAttrs: only "read-only" exists outside Windows.
func fileAttrs(info os.FileInfo) uint8 {
	if info.Mode().IsRegular() && info.Mode().Perm()&0o200 == 0 {
		return 1
	}
	return 0
}
