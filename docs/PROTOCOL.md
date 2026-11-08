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
| 403 | file cannot be opened / folder cannot be read |
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

The host is read-only, accepts connections only from private, loopback and
link-local addresses, and a client can never leave the shared folder: every
path component must match a name the host listed. There is no encryption and no
login; use it on a trusted local network only.
