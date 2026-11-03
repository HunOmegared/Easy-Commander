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
started. Options: `-dir <folder>`, `-port <number>` (default 6502).

## DOS client (Open Watcom 2.0 + mTCP source)

Used so far: Open Watcom 2.0 (beta, October 2026 build) and the mTCP source from
<http://www.brutman.com/mTCP/> (download the *source* archive, not only the
binaries). The paths below assume mTCP is in `C:\mtcp`, with no spaces in the path.

1. Install Open Watcom, open a `cmd` window and set up the environment:

       C:\WATCOM\owsetenv.bat

2. Create the application folder next to the other apps and copy the sources
   (run this from the root of this repository):

       mkdir C:\mtcp\src\APPS\EC
       copy client\ec\EC.CPP      C:\mtcp\src\APPS\EC
       copy client\ec\MAKEFILE    C:\mtcp\src\APPS\EC
       copy C:\mtcp\src\APPS\NC\NC.CFG  C:\mtcp\src\APPS\EC\EC.CFG

   `EC.CFG` is the configuration header of mTCP Netcat, copied unchanged.

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
