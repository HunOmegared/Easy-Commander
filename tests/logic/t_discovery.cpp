// Logic test: the host search (UDP broadcast), the host chooser and the
// search at program start, against stubs and the fake host.
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
static void check(const char *what, int ok) { printf("%-62s %s\n", what, ok ? "ok" : "*** FAILED ***"); if (!ok) failures++; }
static void addHost(int a, int b, int c, int d, unsigned port, const char *name) {
  FakeHost h; h.ip[0]=a; h.ip[1]=b; h.ip[2]=c; h.ip[3]=d; h.port=port; h.name=name; FD_HOSTS.push_back(h);
}
static void feedStdin(const char *text) { FILE *f = fopen("/tmp/ec_stdin.txt", "w"); fputs(text, f); fclose(f); freopen("/tmp/ec_stdin.txt", "r", stdin); }

int main() {
  system("rm -rf /tmp/fakehost && mkdir -p /tmp/fakehost/SUB /tmp/fakehost/EMPTYD && echo hello > /tmp/fakehost/A.TXT && echo x > /tmp/fakehost/B.TXT && echo y > /tmp/fakehost/SUB/C.TXT");
  Rx = (uint8_t *)malloc(RX_SIZE);
  Left.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  Right.items = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  TmpItems = (Item *)malloc(MAX_ITEMS * sizeof(Item));
  Vid = fakeVid;
  Sock = TcpSocketMgr::getSocket();
  Connected = 1;
  strcpy(HostName, "fake");
  Left.remote = 1; strcpy(Left.path, "\\");

  // 1. two hosts answer; the search is sent three times, answers must not be doubled
  addHost(192,168,50,54, 6502, "RETRO-PC");
  addHost(192,168,50,60, 6600, "LAPTOP");
  FD_JUNK.push_back("HELLO\r\n");
  FD_JUNK.push_back("ECHOST 1 0 BAD\r\n");
  FD_JUNK.push_back("ECHOST 1 70000 TOOBIG\r\n");
  FD_JUNK.push_back("ECHOST 2 6502 FUTURE\r\n");
  FD_JUNK.push_back("ECHOST 1 6502");
  g_keyval = 13;
  FD_SENDS = 0;
  int n = findHosts();
  check("two hosts found, duplicates and junk ignored", n == 2 && FoundCount == 2);
  check("search was sent three times", FD_SENDS == 3);
  check("first host: name, address, port", !strcmp(Found[0].name,"RETRO-PC") && Found[0].ip[3]==54 && Found[0].port==6502);
  check("second host keeps its own port", !strcmp(Found[1].name,"LAPTOP") && Found[1].ip[3]==60 && Found[1].port==6600);
  check("callback slot is free again afterwards", g_cb == 0);

  // 2. odd names are made printable
  FD_HOSTS.clear(); FD_JUNK.clear();
  { FakeHost h; h.ip[0]=10;h.ip[1]=0;h.ip[2]=0;h.ip[3]=5; h.port=6502; h.name="NA\x01ME"; FD_HOSTS.push_back(h); }
  n = findHosts();
  check("control characters in a name become '?'", n == 1 && !strcmp(Found[0].name, "NA?ME"));

  // 3. more hosts than the list holds
  FD_HOSTS.clear();
  for (int i = 0; i < 12; i++) addHost(10,0,1,i+1, 6502, "H");
  n = findHosts();
  check("at most 8 hosts are kept", n == MAX_FOUND);

  // 4. nobody answers
  FD_HOSTS.clear();
  n = findHosts();
  check("no answer gives 0", n == 0);

  // 5. UDP cannot be used (no free callback slot)
  g_cb = (UdpCallback)1; g_cbPort = 1;
  n = findHosts();
  check("no free UDP callback gives -1", n == -1);
  g_cb = 0;

  // 6. Esc stops the search after the first send
  addHost(192,168,50,54, 6502, "RETRO-PC");
  g_keyval = 27; FD_SENDS = 0;
  n = findHosts();
  check("Esc stops after the first search", FD_SENDS == 1 && n == 1);
  g_keyval = 13;

  // 7. applyFound
  applyFound(&Found[0]);
  check("applyFound sets name, address and port", !strcmp(HostName,"192.168.50.54") && ServerAddr[3]==54 && ServerPort==6502);

  // 8. Alt+F1 with one host: Enter picks it and the left panel shows that host
  FD_HOSTS.clear(); addHost(192,168,50,54, 6502, "RETRO-PC");
  strcpy(HostName, "fake"); Left.cur = 2;
  cmdChooseHost();
  check("Alt+F1: chosen host is used", !strcmp(HostName,"192.168.50.54") && Left.count > 0 && Left.cur == 0);

  // 9. search at program start
  FD_HOSTS.clear(); addHost(192,168,50,54, 6502, "RETRO-PC");
  strcpy(HostName, "");
  int r = searchAtStart();
  check("start: one host is taken automatically", r == 1 && !strcmp(HostName,"192.168.50.54"));

  addHost(192,168,50,60, 6600, "LAPTOP");
  feedStdin("2\n");
  r = searchAtStart();
  check("start: several hosts, the second is chosen by number", r == 1 && ServerAddr[3]==60 && ServerPort==6600);

  feedStdin("7\n1\n");
  r = searchAtStart();
  check("start: a wrong number is asked again", r == 1 && ServerAddr[3]==54);

  FD_HOSTS.clear();
  feedStdin("retro-pc\n");
  r = searchAtStart();
  check("start: nobody answers, a typed address is used", r == 0 && !strcmp(HostName,"retro-pc"));

  feedStdin("\n");
  r = searchAtStart();
  check("start: nothing typed gives -1", r == -1);

  printf("%d failure(s)\n", failures);
  return failures;
}
