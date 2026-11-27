# Changelog

## 1.1

Two-way transfer, security first. The download side works as before.

* **Upload edition `ECU.EXE`** (the same `EC.CPP` built with `-dEC_UPLOAD`): F4 switches the
  left panel to the host's **inbox**, F5 sends files and folders there (or gets them back),
  F7 makes folders in the inbox. `EC.EXE` stays download-only.
* **Disk images:** F6 sends a whole floppy or hard disk as `.IMG` (BIOS INT 13h, LBA when
  available, bad sectors become zeros and are counted). Disks above 4 GB are refused for now.
* **Host receiving** (off by default): Start receiving, inbox folder, quota, "ask before every
  transfer". Never overwrites ("already exists"), plain 8.3 names only, no device names, local
  networks only, free-space reserve, one transfer at a time, report in `ECINBOX.LOG`.
* **Resume and skip:** an interrupted transfer continues from the `.part` file and is verified
  with CRC32; identical files (same size and time) are skipped ("Unchanged").
* **Dates with seconds and DOS attributes** (read-only, hidden, system, archive) are kept in
  both directions. The list lines of the protocol got two optional fields; older clients and
  hosts ignore them.
* **Read-only host build** (`go build -tags readonly`, `echost-readonly.exe`): the receiving code
  is not compiled in at all.
* The local panel now shows hidden and system files.
* New logic tests for the DOS client (upload, fake disk) and host tests (`go test -race`).
* [Protocol](docs/PROTOCOL.md) and [build](docs/BUILDING.md) documents updated.

## 1.0

First release: download-only client and host.
