# Logic tests for the DOS client

`run.sh` compiles `client/ec/EC.CPP` with small stub headers (in `stubs/`) that
imitate just enough of DOS, Open Watcom and mTCP, and runs the program logic
against a **fake Easy Commander host** (`stubs/tcpsockm.h`).

What is covered: folder listing, recursive download with blocks, overwrite
handling, cancelling, CRC32 checks (standard and fast loop), deleting (read-only and hidden files, a file
that cannot be deleted), the file viewer (paging, hex mode, search, local and host files), select by mask, the drive chooser, switching the host, the settings file and the
host search (UDP broadcast, with answers from several fake hosts).

What is **not** covered: video memory, the keyboard, the packet driver, real
disk behaviour, timing and the real mTCP library. Always test on real DOS too.

Needs a Linux (or WSL) shell with `g++`. Run: `sh tests/logic/run.sh`
The tests create and delete folders below `/tmp` (`fakehost`, `fakedl`,
`fakeloc`, `fakeloc2`).
