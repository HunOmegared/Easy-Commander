// Logic test: the upload code of ECU.EXE (EC.CPP built with -DEC_UPLOAD) against a
// fake host that plays the receiving side of the OFFER protocol: it sends WAIT, OK,
// then asks TREE \ and GET \path off len like the real host, and finally END.
// Exit code = number of failed checks.
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
static void check(const char *what, int ok) { printf("%-64s %s\n", what, ok ? "ok" : "*** FAILED ***"); if (!ok) failures++; }
static int run(const char *cmd) { return system(cmd); }

// ---- the fake receiving host ----

struct Job { std::string rel; long size; long off; };
static std::vector<uint8_t> in;          // bytes from the client, not yet used
static int phase = 0;                    // see step()
static long treeLeft = 0;
static std::vector<std::string> dirs;
static std::vector<Job> jobs;
static size_t cur = 0;
static long bodyLeft = 0;
static FILE *out = 0;
static int probes = 0, probeErr = 0;
static int endSent = 0;
static int failFile = 0;                 // 1: the fake host answers a GET with garbage once (not used)
static std::string inbox = "/tmp/fakeinbox";
static std::string lastOffer;
static int g_ok = 0, g_failed = 0;
static int reqCount = 0;
static int offerMode = 0;                // 0 accept, 1 refuse (401), 2 accept after WAIT
static int cancelAfterBlocks = -1;       // >=0: set ProgCancel (Esc) after that many GET blocks
static int blocks = 0;
static int deleteOnOffer = 0;

static void sendTxt(const std::string &s) { std::vector<uint8_t> none; fh_send(s, none); }

static bool takeLine(std::string &line) {
  for (size_t i = 0; i < in.size(); i++) if (in[i] == '\n') {
    line.assign((char *)&in[0], i);
    if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
    in.erase(in.begin(), in.begin() + i + 1);
    return true;
  }
  return false;
}

static std::vector<std::string> probeList;

static void nextRequest() {
  if (probes < (int)probeList.size()) { sendTxt(probeList[probes] + "\r\n"); phase = 5; return; }
  while (cur < jobs.size() && jobs[cur].size == 0) {            // empty files: no GET
    std::string p = inbox + "/" + jobs[cur].rel; for (auto &c : p) if (c == '\\') c = '/';
    FILE *f = fopen(p.c_str(), "wb"); fclose(f); g_ok++; cur++;
  }
  if (cur >= jobs.size()) {
    char b[64]; snprintf(b, sizeof b, "END %d %d\r\n", g_ok, g_failed); sendTxt(b); endSent = 1; phase = 9; FH_RAW = 0; return;   // normal commands again
  }
  Job &j = jobs[cur];
  long want = j.size - j.off; if (want > 16384) want = 16384;
  char b[300]; snprintf(b, sizeof b, "GET \\%s %ld %ld\r\n", j.rel.c_str(), j.off, want);
  reqCount++;
  sendTxt(b); phase = 3;
}

static void step() {
  while (1) {
    std::string line;
    if (phase == 0) {                                  // answer to TREE: header
      if (!takeLine(line)) return;
      long n = 0, tot = 0; if (sscanf(line.c_str(), "OK %ld %ld", &n, &tot) != 2) { phase = 9; return; }
      treeLeft = n; phase = 1; continue;
    }
    if (phase == 1) {                                  // tree lines
      if (treeLeft == 0) { cur = 0; nextRequest(); continue; }
      if (!takeLine(line)) return;
      treeLeft--;
      char t; char rel[200]; long size; char d[16], tm[16];
      for (auto &c : line) if (c == '\t') c = ' ';
      if (sscanf(line.c_str(), "%c %199s %ld %15s %15s", &t, rel, &size, d, tm) == 5) {
        std::string p = inbox + "/" + rel; for (auto &c : p) if (c == '\\') c = '/';
        if (t == 'D') run(("mkdir -p " + p).c_str());
        else { Job j; j.rel = rel; j.size = size; j.off = 0; jobs.push_back(j); }
      }
      continue;
    }
    if (phase == 3) {                                  // answer to GET: header
      if (!takeLine(line)) return;
      long n = 0, tot = 0;
      if (sscanf(line.c_str(), "OK %ld %ld", &n, &tot) == 2) {
        bodyLeft = n;
        if (!out) { std::string p = inbox + "/" + jobs[cur].rel; for (auto &c : p) if (c == '\\') c = '/'; out = fopen(p.c_str(), "ab"); }
        phase = 4; continue;
      }
      g_failed++; cur++; nextRequest(); continue;     // ERR: this file fails, go on
    }
    if (phase == 4) {                                  // body
      if (in.size() < (size_t)bodyLeft) return;
      fwrite(&in[0], 1, bodyLeft, out); in.erase(in.begin(), in.begin() + bodyLeft);
      jobs[cur].off += bodyLeft; blocks++;
      if (cancelAfterBlocks >= 0 && blocks >= cancelAfterBlocks) { ProgCancel = 1; FH_RAW = 0; phase = 9; return; }   // the client will drop the connection; the host asks nothing more
      if (jobs[cur].off >= jobs[cur].size) { fclose(out); out = 0; g_ok++; cur++; }
      nextRequest(); continue;
    }
    if (phase == 5) {                                  // answer to a probe: must be ERR
      if (!takeLine(line)) return;
      probes++; if (line.compare(0, 4, "ERR ") == 0) probeErr++;
      nextRequest(); continue;
    }
    return;
  }
}

static void rawFromClient(const uint8_t *p, size_t n) { in.insert(in.end(), p, p + n); step(); }

static void onOffer(const std::string &arg) {
  lastOffer = arg;
  if (out) { fclose(out); out = 0; }
  if (deleteOnOffer) { run("rm -f /tmp/fakeup/BIG.BIN"); deleteOnOffer = 0; }
  in.clear(); phase = 0; jobs.clear(); cur = 0; probes = 0; probeErr = 0; g_ok = g_failed = 0; endSent = 0; blocks = 0;
  if (offerMode == 1) { sendTxt("ERR 401 The person at the host said no\r\n"); return; }
  if (offerMode == 2) sendTxt("WAIT\r\nWAIT\r\n");
  sendTxt("OK\r\n");
  FH_RAW = rawFromClient;
  sendTxt("TREE \\\r\n");
}

int main() {
  run("rm -rf /tmp/fakeup /tmp/fakeinbox && mkdir -p /tmp/fakeup/SUB/DEEP /tmp/fakeup/EMPTYD /tmp/fakeinbox");
  run("echo hello > /tmp/fakeup/A.TXT; : > /tmp/fakeup/ZERO.TXT");
  run("head -c 200000 /dev/urandom > /tmp/fakeup/BIG.BIN");
  run("echo csub > /tmp/fakeup/SUB/C.TXT; head -c 70000 /dev/urandom > /tmp/fakeup/SUB/DEEP/D.BIN");

  Rx = (uint8_t *)malloc(RX_SIZE);
  Left.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  Right.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  TmpItems = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  TreeBuf = (uint8_t *)malloc(TREE_BUF_SIZE);
  CrcBuf = (uint8_t *)malloc(4096);
  DiskBuf = (char *)malloc(DISK_BUF_SIZE);
  initCrcTable();
  Vid = fakeVid;
  Sock = TcpSocketMgr::getSocket();
  Connected = 1;
  strcpy(HostName, "fake");
  FH_OFFER = onOffer;

  Left.remote = 1; strcpy(Left.path, "\\");
  Right.remote = 0; strcpy(Right.path, "/tmp/fakeup");
  loadLocal(&Right);

  // F4: the host says UPLOAD in HELLO
  FH_UPLOAD = 0;
  UploadMode = 0;
  cmdUploadMode();
  check("F4 without UPLOAD in HELLO: stays off", UploadMode == 0 && UploadCap == 0);
  FH_UPLOAD = 1;
  cmdUploadMode();
  check("F4 with UPLOAD in HELLO: upload mode on, right panel active", UploadMode == 1 && UploadCap == 1 && Active == 1);

  check("F4: the left panel was read with ILIST (the host's inbox)", FH_INBOXCMDS >= 1 && Left.path[0] == '\\' && Left.path[1] == 0);
  {
    int before = FH_INBOXCMDS;
    Panel q = Left; q.items = TmpItems;
    fetchRemoteList("\\", &q);
    check("in upload mode every listing goes to the inbox", FH_INBOXCMDS == before + 1);
  }

  // F5 = Send, everything selected
  probeList.clear();
  probeList.push_back("GET \\NOTOFFER.TXT 0 10");        // not offered
  probeList.push_back("CRC \\A.TXT");                    // not served at all
  probeList.push_back("GET \\..\\..\\ETC\\PASSWD 0 10");  // path trick
  probeList.push_back("LIST \\");
  for (int i = 0; i < Right.count; i++) if (strcmp(Right.items[i].name, "..")) Right.items[i].sel = 1;
  Active = 1;
  cmdSend();
  check("OFFER line: 5 files, 270011 bytes, label DOS", lastOffer == "5 270011 DOS");
  check("host got every file, byte for byte (incl. empty file and folder)", run("diff -r /tmp/fakeup /tmp/fakeinbox") == 0);
  check("host got END with 5 ok / 0 failed", endSent && g_ok == 5 && g_failed == 0);
  check("all 4 probes (not offered / CRC / .. / LIST) were answered with ERR", probes == 4 && probeErr == 4);
  check("transfer progress counted all bytes", ProgDone == 270011);
  check("session is in sync afterwards (HELLO works)", Connected == 1);
  FH_RAW = 0;
  {
    Panel t = Left; t.items = TmpItems;
    check("normal commands work again after the transfer", fetchRemoteList("\\", &t) == 0);
  }
  {
    int before = FH_INBOXCMDS;
    cmdUploadMode();                                    // F4 again: back to the share
    check("F4 again: upload mode off, share listed with LIST", UploadMode == 0 && FH_INBOXCMDS == before && Active == 0);
    cmdUploadMode();                                    // and on again for the next tests
    check("F4 once more: inbox again", UploadMode == 1 && FH_INBOXCMDS > before);
    FH_RAW = 0;
  }

  // WAIT lines before OK
  run("rm -rf /tmp/fakeinbox && mkdir -p /tmp/fakeinbox");
  probeList.clear();
  offerMode = 2;
  for (int i = 0; i < Right.count; i++) if (strcmp(Right.items[i].name, "..")) Right.items[i].sel = 0;
  for (int i = 0; i < Right.count; i++) if (!strcmp(Right.items[i].name, "A.TXT")) Right.items[i].sel = 1;
  cmdSend();
  check("WAIT, WAIT, OK: the transfer still works (one file)", run("test -f /tmp/fakeinbox/A.TXT && cmp /tmp/fakeup/A.TXT /tmp/fakeinbox/A.TXT") == 0 && g_ok == 1);
  FH_RAW = 0;

  // refused
  offerMode = 1;
  for (int i = 0; i < Right.count; i++) if (!strcmp(Right.items[i].name, "A.TXT")) Right.items[i].sel = 1;
  cmdSend();
  check("ERR 401 from the host: nothing is served, connection stays usable", Connected == 1 && FH_RAW == 0);
  {
    Panel t = Left; t.items = TmpItems;
    check("... and LIST works after the refusal", fetchRemoteList("\\", &t) == 0);
  }

  // Esc during the transfer: the connection is dropped
  offerMode = 0;
  run("rm -rf /tmp/fakeinbox && mkdir -p /tmp/fakeinbox");
  probeList.clear();
  for (int i = 0; i < Right.count; i++) if (!strcmp(Right.items[i].name, "BIG.BIN")) Right.items[i].sel = 1;
  cancelAfterBlocks = 3;
  cmdSend();
  check("Esc: the host asked for nothing more (the client dropped the connection)", phase == 9 && !endSent);
  cancelAfterBlocks = -1;
  FH_RAW = 0;

  // a file that disappears after it was offered: ERR for that one, the other goes on
  check("after Esc the client reconnected by itself and listed the inbox again", Connected == 1);
  run("rm -rf /tmp/fakeinbox && mkdir -p /tmp/fakeinbox");
  probeList.clear();
  offerMode = 0;
  for (int i = 0; i < Right.count; i++) Right.items[i].sel = 0;
  for (int i = 0; i < Right.count; i++) if (!strcmp(Right.items[i].name, "A.TXT") || !strcmp(Right.items[i].name, "BIG.BIN")) Right.items[i].sel = 1;
  UploadMode = 1; UploadCap = 1; Active = 1;
  deleteOnOffer = 1;
  cmdSend();
  FH_RAW = 0;
  check("vanished file fails alone, the other file arrives", g_failed == 1 && g_ok == 1 && run("cmp /tmp/fakeup/A.TXT /tmp/fakeinbox/A.TXT") == 0);
  check("a failed file leaves nothing half-written on the fake host", run("test ! -e /tmp/fakeinbox/BIG.BIN") == 0);

  printf("\n%d failure(s)\n", failures);
  return failures;
}

