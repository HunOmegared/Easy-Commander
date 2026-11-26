//go:build !windows

package main

import "syscall"

// freeSpace returns the number of bytes a normal user can still write to the
// volume that holds dir.
func freeSpace(dir string) (int64, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(dir, &st); err != nil {
		return 0, err
	}
	return int64(uint64(st.Bavail) * uint64(st.Bsize)), nil
}
