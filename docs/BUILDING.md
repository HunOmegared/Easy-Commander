# Building

## Host (Go 1.21 or newer)

    cd host
    go test
    go build -ldflags "-H=windowsgui -s -w" -o echost.exe        (Windows)
    go build -o echost                                          (Linux)

`host\build-windows.bat` runs the two Windows commands. `-H=windowsgui` makes a
program without a console window; the window version needs nothing but Go (no
extra packages). Without that flag it works too, but a console window is left
open next to the program window.

The Linux files of a release are built like this (static, no libraries needed;
`GOARCH` is `amd64`, `arm64`, `arm` with `GOARM=7`, or `386`):

    GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o echost

Cross compiling from a Linux shell:

    GOOS=windows GOARCH=amd64 go build -ldflags "-H=windowsgui -s -w" -o echost.exe
    GOOS=linux   GOARCH=amd64 go build -o echost

From a Windows `cmd` window:

    set GOOS=linux
    set GOARCH=amd64
    go build -o echost

On Windows the program opens a window (see the README); on Linux it runs in the
console. In the console it shares the folder given as the argument (or dropped
onto the executable), or a folder named `share` in the folder where it was
started. Options: `-dir <folder>`, `-port <number>` (default 6502, TCP and UDP),
`-name <text>` (the name shown to clients that search for hosts; default: the
computer name) and, on Windows, `-console` (no window; output goes to the
console the program was started from). In the window version the same options
override the saved settings (`echost.ini` next to the program).

Receiving (the other direction, see `docs/PROTOCOL.md`, command `OFFER`) is off
until it is switched on: in the window with "Start receiving" (inbox folder,
quota, "Ask me before every transfer"), in the console with `-inbox <folder>`.
Console options for it: `-quota 4G` (most the inbox may hold; `0` = only the free
disk space limits), `-reserve 1G` (free space that must remain), `-no-ask` (do
not ask before every transfer; you confirm once at the start, or give `-yes`),
`-allow 172.20.0.0/16` (more networks that may send; by default only the networks
of the computer's own interfaces and loopback), `-no-share` (receive only).

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

**ECU.EXE, the upload edition.** The same `EC.CPP` built with `-dEC_UPLOAD` can
also send files to a host that has receiving switched on (F4 = upload mode, F5 =
Send). It has its own makefile; build it in its own run (the makefile deletes
the object files first, so the two programs do not mix):

       cd C:\mtcp\src\APPS\EC
       wmake -f makefile.ecu

The result is `ECU.EXE`. `EC.EXE` stays download-only.

The command line test client is built the same way from `client/ectest/`
(folder `ECTEST`, `ECTEST.CFG` copied from `NC.CFG`); it fits in the compact
model.

The user interface prototype needs no mTCP:

    wcl -bt=dos -ms -0 ecui.cpp

On the DOS machine you need a packet driver for your network card and an mTCP
configuration (the `MTCPCFG` environment variable, see the mTCP documentation).

## Tests

* Host: `cd host && go test` (add `-race` for the data race check)
* DOS client logic, download and upload edition (no DOS needed, Linux or WSL with g++): `sh tests/logic/run.sh`

## Line endings

The DOS sources use CRLF, the Go and shell files use LF. `.gitattributes` takes
care of that; do not convert the DOS sources to LF.
