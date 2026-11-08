# Easy Commander

A Norton Commander style file manager for **MS-DOS** that fetches files over the
network from a modern PC. A small **host program** (Windows / Linux) shares a
folder read-only; the **DOS client** shows the host on the left and a local DOS
folder on the right, and downloads files and whole folders with F5.

It exists because getting files onto a retro PC over Ethernet usually means
setting up FTP, SMB1 or serial transfers, which are all awkward. Here the host
is a single executable and the client is a two-panel program you already know
how to use.

```
+== \ ===============================++== C:\DL ============================+
| EMPTY        <DIR>  26-11-01 10:59 || ..          <UP-DIR>                |
| LONGFO~1     <DIR>  26-11-01 11:00 ||                                    |
| TESTFO~1     <DIR>  26-11-01 11:05 ||                                    |
| NCT.IMA     1474560 26-11-01 01:25 ||                                    |
+== Host 192.168.1.10 ===============++== Local ============================+
1Help 2Reconn 3 4 5Get 6 7MkDir 8Delete 9Opts 10Quit
```

## Status

Early, but working. Tested in an emulated PC (86Box, MS-DOS 6.22, NE2000
compatible network card) and on two real machines, both with a CompactFlash card
instead of a hard disk and an RTL8019AS ISA network card with the Realtek packet
driver: an AMD 5x86-133 with 8 MB RAM, and a Cyrix 486DLC with 4 MB RAM (with an
8 bit XT-IDE card and with a 16 bit IDE controller). Downloads, CRC32 checks,
host search and switching between two hosts worked on all of them.
**Older CPUs (286 and below) have not been tested yet.**

* Browse the host's folders, select with Insert, download files and folders
  with their subfolders, keeping the structure.
* Optional CRC32 check of every downloaded file (read back from disk). A faster
  CRC32 loop for slow CPUs is on by default (same result; F9 switches it off).
* View a file with F3, on the host or on the local disk: text or hex (F4), search
  (F7; in hex mode type hex bytes such as `6E 50`, or a text in double quotes),
  scrolling with the cursor keys. A file on the host is read in pieces,
  so only what you look at is transferred.
* Cancel a download with Esc; existing files are never overwritten without asking.
* **Download only, by design:** the host shares a folder read-only and the client
  has no upload. Nothing on the host can be changed, renamed or deleted from the
  DOS side, and the host only accepts connections from local network addresses.
* Delete files and folders on the *local* side (F8).
* Switch drives (Alt+F2) and host (Alt+F1).
* Remembers the last host and the CRC option in `EC.INI` next to `EC.EXE`; started
  without arguments, `EC` connects to the saved host.
* Finds hosts on the local network by itself (UDP broadcast): at the first start
  without arguments, and with Alt+F1 at any time; several hosts are shown in a list.

## Quick start

**Host (modern PC)** - see [docs/BUILDING.md](docs/BUILDING.md):

    easycommander-host.exe C:\path\to\shared\folder

It prints the addresses it can be reached at and listens on TCP port 6502.
Only connections from private (local network) addresses are accepted.
The first start on Windows asks whether the firewall may allow it.

**DOS client** (needs a packet driver and an mTCP configuration, see the mTCP
documentation):

    EC 192.168.1.10          (or: EC host-name [port])

## Keys

| Key | Action |
|---|---|
| Up, Down, PgUp, PgDn, Home, End | move the cursor |
| Enter | open a folder (`..` goes up) |
| Tab | switch panel |
| Insert | select / unselect, move down |
| `+` `-` `*` | select all files / none / invert |
| F3 | view the file under the cursor (in the viewer: F4 hex/text, F7 search, Left/Right sideways, Esc or F10 close) |
| F5 | download the selected items (or the one under the cursor) to the right panel's folder |
| F7 | make a folder (right panel) |
| F8 | delete (right panel only; only `Y` confirms) |
| F9 | options (CRC32 check of every download, fast CRC32 code) |
| F2 | reconnect and refresh the host panel |
| Alt+F1 | connect to another host |
| Alt+F2 | change drive (right panel) |
| Ctrl+O | look at the DOS screen (the start text and earlier commands); any key returns |
| F10, Esc | quit |

## Repository layout

| Folder | Content |
|---|---|
| `host/` | the host program (Go) and its tests |
| `client/ec/` | the DOS client (`EC.CPP`, `MAKEFILE`, `EC.CFG`), built inside the mTCP source tree |
| `client/ectest/` | a small command line test client (HELLO, LIST, TREE, GET, GETDIR, CRC) |
| `client/ecui/` | the user interface prototype (builds without mTCP, no network) |
| `docs/` | [protocol](docs/PROTOCOL.md) and [build instructions](docs/BUILDING.md) |
| `tests/logic/` | logic tests that run without DOS, see its README |

## Performance

Measured on real machines, always downloading the same 12 MB file. All of them
use a CompactFlash card instead of a hard disk, an RTL8019AS ISA card (10 Mbit/s)
and the Realtek packet driver. The first test series, on the AMD 5x86-133 (8 MB
RAM, CF card from InnoDisk):

| Test | Speed |
|---|---|
| network only (`ectest ... GET file NUL`) | about 600 KB/s |
| network + disk (`ectest ... GET file C:\file`) | about 387 KB/s |
| Easy Commander, F5 | about 361 KB/s |

The speed is even over the whole file. Receiving and writing to the disk do
not overlap, so their times add up; the disk side is the slower part on this
machine. The test client's `/T` switch prints the speed of every megabyte.

The same file with Easy Commander (F5), on the 5x86-133 and a Cyrix 486DLC (4 MB RAM):

| Machine and disk | CRC32 check off | CRC32 check on |
|---|---|---|
| AMD 5x86-133, CF (InnoDisk) | 361 KB/s | 212 KB/s |
| AMD 5x86-133, CF (SiliconSystems) | 394 KB/s | - |
| Cyrix 486DLC, 8 bit XT-IDE card, CF | 105 KB/s | 51 KB/s |
| Cyrix 486DLC, 16 bit IDE, CF | 425 KB/s | 225 KB/s (126 KB/s with the old CRC loop) |

On the slow machines the disk is what limits the speed (a 486DLC with an 8 bit
XT-IDE card writes only about 130 KB/s; its network-only speed is 651 KB/s), and
the CRC32 check, which reads the file back, takes about as long as the
download itself unless the fast CRC32 loop (F9) is used.

## Known issues

* Build with **Open Watcom 1.9**. With the Open Watcom 2.0 beta (the V2
  GitHub fork, October 2026 build) the program prints `End: heap is
  corrupted!` at exit. The message comes from mTCP's `UTILS.CPP`
  (`_heapchk()` after the stack is shut down); the unmodified mTCP Netcat
  prints it too with that compiler, and it disappears when EC and ECTEST are
  built with Open Watcom 1.9. Every CRC32 check passed with both compilers.
* The client only sees DOS 8.3 names; the host maps long names to
  `BASE~1.EXT` style names. File dates are not restored (copies get the
  current date).
* The host search is a broadcast: it does not cross routers, and hosts started
  with another `-port` are not found by it (type their address instead). In the
  86Box "SLIRP" network mode the host is found but listed as 127.0.0.1; type
  10.0.2.2 instead.
* Memory use: while the program runs, F1 shows the free conventional memory;
  compare it with the value `MEM` reports before starting `EC`. An earlier build
  used about 286 KB; later builds use roughly 16 KB more (bigger write buffer).

## Roadmap

* Test on more hardware (other network cards and packet drivers, 286 and 386 machines, less RAM).
* Select by mask (`+` with `*.TXT`), keep file dates.

## License

GPL-3.0-or-later. The DOS client links the mTCP library by Michael B. Brutman
(GPLv3) and its structure follows mTCP Netcat, so the DOS programs must be GPLv3.
To keep things simple the whole project uses the same license. The mTCP source
is **not** part of this repository; get it from <http://www.brutman.com/mTCP/>.

Copyright (C) 2026 Daniel Szentmiklosi.
