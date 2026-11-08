#!/bin/sh
# Logic tests for the DOS client.  They compile client/ec/EC.CPP with stub
# headers that imitate just enough of DOS, Open Watcom and mTCP, and run it
# against a fake Easy Commander host.  No DOS, Watcom or mTCP is needed, only
# a Linux (or WSL) shell with g++.  The files below /tmp are used and deleted.
#
# This is a safety net for the program logic.  It is NOT a replacement for
# testing on real DOS: video memory, the keyboard, the packet driver, disk
# behaviour and the real mTCP library are not exercised.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
B=${TMPDIR:-/tmp}/ec-logic-build
mkdir -p "$B"
{
  cat <<HEAD
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#define stricmp strcasecmp
HEAD
  tr -d "\r" < "$HERE/../../client/ec/EC.CPP"
} > "$B/ec_check.cpp"

status=0
for t in t_download t_local t_discovery t_crc t_view t_mask; do
  echo "=== $t ==="
  g++ -std=gnu++11 -w -I"$HERE/stubs" -I"$B" -o "$B/$t" "$HERE/$t.cpp"
  timeout 60 "$B/$t" || status=1
done
exit $status
