# Easy Commander protocol (version 1)

Plain text commands over one TCP connection (default port **6502**), one command
at a time. Every command and every response line ends with CR LF (the host also
accepts a bare LF). Command names are case-insensitive. The longest command line
is 2048 bytes.

The client only ever sees **DOS 8.3 names**. Paths use `\` or `/` as separator
(both are accepted), `\` is the root of the shared folder. A path component must
be a name the host listed; `..` is rejected.

## Commands

### `HELLO`

    OK EASYCOMMANDER 1

### `LIST <path>`

Lists one folder. An empty path means the root.

    OK <count>
    <D|F> TAB <NAME> TAB <size> TAB <YYYYMMDD> TAB <HHMM>        (count lines)

Folders first, then files, each sorted by name. A folder has size 0. Date and
time are the host's local time; dates before 1980 or after 2099 become
`19800101 0000`.

### `TREE <path>`

Lists a folder and everything below it.

    OK <count> <total size of all files>
    <D|F> TAB <relative path> TAB <size> TAB <YYYYMMDD> TAB <HHMM>

Paths are relative to `<path>` and use `\`. A folder is always listed before its
contents; empty folders are included. Limits: 32 levels, 50 000 entries.

### `GET <path> [offset [length]]`

    OK <n> <filesize>
    <exactly n raw bytes>

`n` is the part of the file from `offset` (default 0) with at most `length`
bytes (default: up to the end). An `offset` equal to the file size gives
`n = 0`; a larger offset is an error. The DOS client requests files in 64 KB
blocks, so that cancelling and a full disk leave the connection in a clean
state.

### `CRC <path>`

    OK <size> <CRC32 as 8 hexadecimal digits>

CRC-32 (IEEE 802.3, the one used by zip) of the whole file.

### `OFFER <files> <bytes> <path> [label]`

The other direction (the DOS client sends, the host receives). Only works while
the person at the host has switched receiving on, and only for clients in the
host's own network. `HELLO` tells a client that it may try: the answer ends with
` UPLOAD`:

    OK EASYCOMMANDER 1 UPLOAD

`<files>` and `<bytes>` are the number of files and their total size. `<path>` is
the folder of the inbox the files should go into, as the client sees it (a DOS
path, `\` is the root of the inbox; the folders can be made with `IMKDIR`). In the
root the host makes a new folder `<label>_<YYYYMMDD>_<HHMM>` for them (`label`:
letters, digits, `_`, `-`, up to 20, default: the client's address); in any other
folder the files go straight into it.

    WAIT                         (every 5 seconds while somebody at the host is asked)
    OK                           the host agrees
    ERR <code> <message>         it does not (see below)

After `OK` the **roles are swapped on the same connection**: the host is the
downloader and sends commands, the client answers exactly like a host would, but
only for what it offered, and the offered files are the root (`\`):

    host:    TREE \
    client:  OK <entries> <total size>, then the entries (as for TREE)
    host:    GET \<path> <offset> <length>        (blocks of 16 KB)
    client:  OK <n> <filesize>, then n bytes       or   ERR <code> <message>
    host:    CRC \<path>                           (only to check a resumed file)
    client:  OK <size> <CRC32 as 8 hex digits>     or   ERR <code> <message>
    ...
    host:    END <received> <failed> <unchanged>        or   ABORT <message>

The client's list may carry two more fields on every line, after `HHMM`:
`TAB <seconds> TAB <attributes>` (seconds 0-59; attributes = the DOS attribute
byte: read-only 1, hidden 2, system 4, archive 32, other bits are ignored). The host
sets the time with the seconds and, on Windows, the attributes (elsewhere only
read-only). Lines without the two fields are fine.

`END` or `ABORT` is the last line of the host: the connection is a normal one
again and the client may send the next command. A client answers `ERR` to a
request for anything it did not offer. `ERR` for one `GET` (for example a read
error on the client's disk) fails only that file; the host keeps what it
received as `NAME.EXT.part` and goes on with the next file.

Everything is decided on the host, and nothing the client says is trusted:

* The files go into the folder named by `<path>` below the inbox folder, or, in the
  root, into a new folder `<label>_<YYYYMMDD>_<HHMM>` (never into the shared
  folder). A file that already exists is never overwritten: it is skipped, counts
  as failed and is named in the report.
* Every name must be a plain 8.3 name: no `..`, no separators, no device names
  (`CON`, `NUL`, `COM1` ...). A folder must be listed before its contents. A
  list that breaks a rule ends the transfer with `ABORT`.
* A file that exists already with the same size and time (2 seconds apart at most)
  is not fetched again: it is counted as *unchanged*. A different one is never
  overwritten (see above).
* An interrupted file (`NAME.EXT.part`, smaller than the offered size) is
  continued where it stopped: the host asks for the rest with `GET`, then asks
  `CRC` for the whole file and compares it with its own file. If they differ, or
  the client cannot answer, the partial file is deleted and the file counts as
  failed (the next try starts from the beginning). A file that was fetched in one
  go is not checked again.
* A file is written as `NAME.EXT.part` and renamed when it is complete; the date
  and time from the list are set on it. A report `ECINBOX.LOG` is added to the
  folder (the reports of several transfers follow each other).
* The number of files and bytes may not exceed what was offered.
* The offer is refused before anyone is asked when the inbox would exceed its
  quota, or when less than a reserve (1 GB) of free disk space would remain.
* One transfer at a time; every wait has a time limit.

Errors of `OFFER`:

| Code | Meaning |
|---|---|
| 400 | bad numbers, bad path |
| 404 | the destination folder does not exist |
| 405 | the destination is not a folder |
| 401 | the person at the host said no |
| 403 | receiving is off, or the client is not in the host's network |
| 408 | nobody at the host answered in time (60 seconds) |
| 409 | another transfer is running |
| 500 | the folder could not be created |
| 507 | over the quota, or not enough free disk space |

### `ILIST`, `ITREE`, `ICRC`, `IGET`

The same four commands as `LIST`, `TREE`, `CRC` and `GET`, but they work on the
**inbox** (the folder the received files go to) instead of the shared folder:
the same DOS names, the same answers, read-only. They work only while receiving
is switched on at the host and only for clients that may send (the host's own
network); otherwise the answer is `ERR 403 Receiving is off`. The DOS client in
upload mode shows the inbox in its left panel with them, so what was sent can be
seen, and fetched again.

### `IMKDIR <path>`

Makes a folder in the inbox: the last component of `<path>` is the new name (a plain
8.3 name, no device names), the folder above it must exist. `OK`, or `ERR 400` (bad
name or path), `404` (no such parent), `405`, `409` (already exists), `403` (receiving
is off, the client may not, or more than 100 folders in one connection).

### `QUIT`

    OK Bye

The host then closes the connection.

## Discovery (UDP)

A client that does not know the host's address sends one UDP datagram with the
text `ECDISC 1` (optionally followed by CR LF) to the broadcast address, on the
same port number as the TCP port (default 6502). Every host answers directly to
the sender:

    ECHOST 1 <tcp port> <name>

`<name>` is printable ASCII, at most 24 characters (the computer name, or the
`-name` option of the host). The client takes the host's address from the
sender of the reply. A host never announces itself; it only answers requests
from private, loopback and link-local addresses and ignores everything else.
Broadcasts do not cross routers. The DOS client sends its search to UDP port
6502 only, so a host that was started with another `-port` has to be addressed
by hand.

## Errors

    ERR <code> <message>

| Code | Meaning |
|---|---|
| 400 | bad request, unknown command, bad number, `..` in a path |
| 403 | file cannot be opened / folder cannot be read / sharing is off |
| 404 | no such file or folder |
| 405 | not a folder (LIST/TREE on a file) |
| 406 | is a folder (GET/CRC on a folder; use TREE) |
| 413 | folder tree too large (TREE) |
| 500 | internal error |
| 503 | too many clients (the limit is 8) |

An `ERR` response never leaves the connection out of sync: the client can send
the next command. An idle connection is closed by the host after 10 minutes.

## Names and visibility

* Hidden files (names starting with `.`), symbolic links and special files are
  not listed and cannot be fetched.
* Names that already are valid 8.3 names are shown in upper case. The others
  become `BASE~N.EXT`: spaces and dots are removed, illegal characters become
  `_`, the base is shortened so that `~N` fits in 8 characters, the extension
  is cut to 3. Entries are numbered in the order of their real names, and valid
  8.3 names win over generated ones, so the mapping depends only on the folder's
  content (no state is kept between requests).

## Safety

The shared folder is read-only, the host accepts connections only from private,
loopback and link-local addresses, and a client can never leave the shared
folder: every path component must match a name the host listed. Receiving
(`OFFER`) is off until it is switched on at the host, accepts only clients in
the networks of the host's own interfaces (and any that were added by hand), and
writes only into its inbox folder, as described above (and lets the same clients read the inbox back, see `ILIST`). What arrives there is
untrusted data: the host never opens or runs it. There is no encryption and no
login (a person has to confirm incoming transfers unless that was switched
off); use it on a trusted local network only.
