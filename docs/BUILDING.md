# Building

## Host (Go 1.21 or newer)

    cd host
    go test
    go build -o easycommander-host.exe        (Windows)
    go build -o easycommander-host            (Linux)

Cross compiling from a Linux shell:

    GOOS=windows GOARCH=amd64 go build -o easycommander-host.exe
    GOOS=linux   GOARCH=amd64 go build -o easycommander-host

From a Windows `cmd` window:

    set GOOS=linux
    set GOARCH=amd64
    go build -o easycommander-host

Run it with the folder to share (or drop the folder onto the executable).
Without an argument it shares a folder named `share` in the folder where it was
started. Options: `-dir <folder>`, `-port <number>` (default 6502, TCP and UDP) and
`-name <text>` (the name shown to clients that search for hosts; default: the
computer name).

## DOS client (Open Watcom 1.9 + mTCP source)

Use **Open Watcom 1.9** (recommended; it is what the mTCP author uses). The Open
Watcom 2.0 beta (V2 fork) also compiles the code, but the program then prints
`End: heap is corrupted!` at exit. Install 1.9 in its own folder (for example
`C:\WATCOM19`), and check that `wmake -?` reports version 1.9. The mTCP source is from
<http://www.brutman.com/mTCP/> (download the *source* archive, not only the
binaries). The paths below assume mTCP is in `C:\mtcp`, with no spaces in the path.

1. Install Open Watcom, open a `cmd` window and set up the environment:

       C:\WATCOM19\owsetenv.bat

2. Create the application folder next to the other apps and copy the sources
   (run this from the root of this repository):

       mkdir C:\mtcp\src\APPS\EC
       copy client\ec\EC.CPP      C:\mtcp\src\APPS\EC
       copy client\ec\MAKEFILE    C:\mtcp\src\APPS\EC
       copy client\ec\EC.CFG      C:\mtcp\src\APPS\EC

   `EC.CFG` is a modified copy of the configuration header of mTCP Netcat. The
   only change is `UDP_MAX_CALLBACKS`, raised from 1 to 2: the DNS code uses the
   only callback the default allows, and the host search needs one more. (The
   change is marked in the file's header.) It includes mTCP's own `Global.Cfg`.

3. Build:

       cd C:\mtcp\src\APPS\EC
       wmake

   The result is `EC.EXE`. `EC` is built in the **large** memory model (`-ml`):
   in the compact model (`-mc`) the code does not fit into one 64 KB segment.

The command line test client is built the same way from `client/ectest/`
(folder `ECTEST`, `ECTEST.CFG` copied from `NC.CFG`); it fits in the compact
model.

The user interface prototype needs no mTCP:

    wcl -bt=dos -ms -0 ecui.cpp

On the DOS machine you need a packet driver for your network card and an mTCP
configuration (the `MTCPCFG` environment variable, see the mTCP documentation).

## Tests

* Host: `cd host && go test`
* DOS client logic (no DOS needed, Linux or WSL with g++): `sh tests/logic/run.sh`

## Line endings

The DOS sources use CRLF, the Go and shell files use LF. `.gitattributes` takes
care of that; do not convert the DOS sources to LF.
