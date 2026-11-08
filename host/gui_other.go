//go:build !windows

package main

// Only Windows has the window version; everywhere else the host runs in the
// console.

const guiAvailable = false

func runGUI(o options) {}

func attachConsole() {}
