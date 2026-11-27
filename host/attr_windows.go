//go:build windows

package main

import "syscall"

// setAttrs gives a file the DOS attributes the client reported (read-only 1,
// hidden 2, system 4, archive 32: the same bits Windows uses).
func setAttrs(path string, attr uint8) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return
	}
	a := uint32(attr & attrMask)
	if a == 0 {
		a = syscall.FILE_ATTRIBUTE_NORMAL
	}
	syscall.SetFileAttributes(p, a)
}
