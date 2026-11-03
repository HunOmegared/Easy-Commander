// Logic test: the download code of EC.CPP against a fake Easy Commander host
// (see stubs/tcpsockm.h).  Exit code = number of failed checks.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#define stricmp strcasecmp
#define main ec_main
#include "ec_check.cpp"
#undef main
#undef fopen
#undef remove
#undef mkdir
#undef rmdir

static u16 fakeVid[80*25];
static int failures = 0;
static void check(const char *what, int ok) { printf("%-60s %s\n", what, ok ? "ok" : "*** FAILED ***"); if (!ok) failures++; }
static int run(const char *cmd) { return system(cmd); }

int main() {
  run("rm -rf /tmp/fakehost /tmp/fakedl && mkdir -p /tmp/fakehost/SUB/DEEP /tmp/fakehost/EMPTYD /tmp/fakedl");
  run("echo hello > /tmp/fakehost/A.TXT; : > /tmp/fakehost/ZERO.TXT");
  run("head -c 200000 /dev/urandom > /tmp/fakehost/BIG.BIN");
  run("echo csub > /tmp/fakehost/SUB/C.TXT; head -c 70000 /dev/urandom > /tmp/fakehost/SUB/DEEP/D.BIN");

  Rx = (uint8_t *)malloc(RX_SIZE);
  Left.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  Right.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  TmpItems = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  TreeBuf = (uint8_t *)malloc(TREE_BUF_SIZE);
  CrcBuf = (uint8_t *)malloc(4096);
  initCrcTable();
  Vid = fakeVid;
  Sock = TcpSocketMgr::getSocket();
  Connected = 1;
  strcpy(HostName, "fake");

  Left.remote = 1;
  strcpy(Left.path, "\\");
  int rc = fetchRemoteList(Left.path, &Left);
  check("LIST of the host root works", rc == 0 && Left.count == 5);

  Right.remote = 0;
  strcpy(Right.path, "/tmp/fakedl");

  for (int i = 0; i < Left.count; i++) if (strcmp(Left.items[i].name, "..")) Left.items[i].sel = 1;
  Active = 0;
  Verify = 1;
  cmdDownload();
  check("all bytes counted (270011)", ProgDone == 270011 && ProgTotal == 270011);
  check("copy identical to the host folder (incl. empty file/folder)", run("diff -r /tmp/fakehost /tmp/fakedl") == 0);

  for (int i = 0; i < Left.count; i++) if (strcmp(Left.items[i].name, "..")) Left.items[i].sel = 1;
  cmdDownload();   // files exist now: the overwrite question is answered with Y
  check("second run (overwrite) still identical", run("diff -r /tmp/fakehost /tmp/fakedl") == 0);

  ProgCancel = 1;
  u32 got = 0;
  ProgFileDone = 0; ProgDone = 0; ProgFileSize = 0;
  int r = downloadFile("\\BIG.BIN", "/tmp/fakedl/CANCEL.BIN", &got);
  check("Esc: transfer reports cancel", r == FETCH_CANCEL);
  check("Esc: partial file removed", run("test ! -e /tmp/fakedl/CANCEL.BIN") == 0);
  ProgCancel = 0;
  Panel t = Left; t.items = TmpItems;
  check("Esc: session still in sync (LIST works afterwards)", fetchRemoteList("\\SUB", &t) == 0 && t.count == 3);

  FH_BADCRC = 1; ProgFileDone = 0;
  r = downloadFile("\\A.TXT", "/tmp/fakedl/BADCRC.TXT", &got);
  check("wrong CRC is detected, file is kept", r == FETCH_BADCRC && run("test -e /tmp/fakedl/BADCRC.TXT") == 0);
  FH_BADCRC = 0;

  r = downloadFile("\\NOPE.TXT", "/tmp/fakedl/NOPE.TXT", &got);
  check("missing file: skipped, nothing left behind", r == FETCH_SKIP && run("test ! -e /tmp/fakedl/NOPE.TXT") == 0);

  printf("%d failure(s)\n", failures);
  return failures;
}
