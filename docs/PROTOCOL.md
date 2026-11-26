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

### `OFFER <files> <bytes> [label]`

The other direction (the DOS client sends, the host receives). Only works while
the person at the host has switched receiving on, and only for clients in the
host's own network. `HELLO` tells a client that it may try: the answer ends with
` UPLOAD`:

    OK EASYCOMMANDER 1 UPLOAD

`<files>` and `<bytes>` are the number of files and their total size, `label`
(letters, digits, `_`, `-`, up to 20) names the folder the files arrive in.

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
    ...
    host:    END <files received> <files failed>        or   ABORT <message>

`END` or `ABORT` is the last line of the host: the connection is a normal one
again and the client may send the next command. A client answers `ERR` to a
request for anything it did not offer. `ERR` for one `GET` (for example a read
error on the client's disk) fails only that file; the host keeps what it
received as `NAME.EXT.part` and goes on with the next file.

Everything is decided on the host, and nothing the client says is trusted:

* The files go into a new folder `<label>_<YYYYMMDD>_<HHMM>` below the inbox
  folder (never into the shared folder, never over an existing file or folder).
* Every name must be a plain 8.3 name: no `..`, no separators, no device names
  (`CON`, `NUL`, `COM1` ...). A folder must be listed before its contents. A
  list that breaks a rule ends the transfer with `ABORT`.
* A file is written as `NAME.EXT.part` and renamed when it is complete; the date
  and time from the list are set on it. A report `ECINBOX.LOG` is written into
  the folder.
* The number of files and bytes may not exceed what was offered.
* The offer is refused before anyone is asked when the inbox would exceed its
  quota, or when less than a reserve (1 GB) of free disk space would remain.
* One transfer at a time; every wait has a time limit.

Errors of `OFFER`:

| Code | Meaning |
|---|---|
| 400 | bad numbers |
| 401 | the person at the host said no |
| 403 | receiving is off, or the client is not in the host's network |
| 408 | nobody at the host answered in time (60 seconds) |
| 409 | another transfer is running |
| 500 | the folder could not be created |
| 507 | over the quota, or not enough free disk space |

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
writes only into its inbox folder, as described above. What arrives there is
untrusted data: the host never opens or runs it. There is no encryption and no
login (a person has to confirm incoming transfers unless that was switched
off); use it on a trusted local network only.
