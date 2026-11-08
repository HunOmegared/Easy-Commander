// Logic test: delete, drive chooser and host chooser of EC.CPP against stubs.
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
static int ex(const char *p){ char c[300]; snprintf(c,sizeof c,"test -e '%s'",p); return system(c)==0; }
static void selectNames(Panel *p, const char *names){ for(int i=0;i<p->count;i++){ p->items[i].sel = strstr(names, p->items[i].name)!=0 && strcmp(p->items[i].name,"..")!=0; } }
static int failures=0;
static void check(const char *what,int ok){ printf("%-60s %s\n",what,ok?"ok":"*** FAILED ***"); if(!ok) failures++; }

int main(){
  system("rm -rf /tmp/fakeloc /tmp/fakeloc2 && mkdir -p /tmp/fakeloc/DIRA/SUB /tmp/fakeloc/EMPTYD /tmp/fakeloc/STUCKD /tmp/fakeloc2");
  system("echo a > /tmp/fakeloc/FILE1.TXT; chmod 444 /tmp/fakeloc/FILE1.TXT; echo k > /tmp/fakeloc/KEEP.TXT");
  system("echo x > /tmp/fakeloc/DIRA/SUB/X.TXT; echo h > /tmp/fakeloc/DIRA/SUB/.HID; echo a > /tmp/fakeloc/DIRA/A.TXT");
  system("echo s > /tmp/fakeloc/STUCKD/STUCK.TXT; echo o > /tmp/fakeloc/STUCKD/OTHER.TXT; echo z > /tmp/fakeloc2/Z.TXT");

  Rx=(uint8_t*)malloc(RX_SIZE); Left.items=(Item*)malloc(MAX_ITEMS*sizeof(Item)); Right.items=(Item*)malloc(MAX_ITEMS*sizeof(Item));
  TmpItems=(Item*)malloc(MAX_ITEMS*sizeof(Item)); TreeBuf=(uint8_t*)malloc(TREE_BUF_SIZE); CrcBuf=(uint8_t*)malloc(4096);
  initCrcTable(); Vid=fakeVid; Sock=TcpSocketMgr::getSocket(); Connected=1; strcpy(HostName,"fake");
  Left.remote=1; strcpy(Left.path,"\\"); fetchRemoteList(Left.path,&Left);
  Right.remote=0; strcpy(Right.path,"/tmp/fakeloc"); reload(&Right);
  printf("right panel: %d items\n",Right.count);

  // 1. wrong panel: nothing may happen
  Active=0; selectNames(&Right,"FILE1.TXT"); g_keyval='y'; cmdDelete();
  check("left panel active: F8 does nothing",ex("/tmp/fakeloc/FILE1.TXT"));

  // 2. answer N: nothing deleted
  Active=1; selectNames(&Right,"FILE1.TXT DIRA EMPTYD"); g_keyval='n'; cmdDelete();
  check("answering N keeps everything",ex("/tmp/fakeloc/FILE1.TXT")&&ex("/tmp/fakeloc/DIRA/SUB/X.TXT"));
  // Enter must NOT confirm

  // 3. Y: read-only file, folder with hidden file and subfolders, empty folder
  selectNames(&Right,"FILE1.TXT DIRA EMPTYD"); g_keyval='y'; cmdDelete();
  check("read-only file deleted",!ex("/tmp/fakeloc/FILE1.TXT"));
  check("folder with subfolder + hidden file deleted",!ex("/tmp/fakeloc/DIRA"));
  check("empty folder deleted",!ex("/tmp/fakeloc/EMPTYD"));
  check("things not selected are kept (KEEP.TXT, STUCKD)",ex("/tmp/fakeloc/KEEP.TXT")&&ex("/tmp/fakeloc/STUCKD/STUCK.TXT"));
  printf("   counters: files=%lu dirs=%lu failed=%lu\n",DelFiles,DelDirs,DelFailed);
  check("panel was refreshed (no DIRA entry)",[&]{ for(int i=0;i<Right.count;i++) if(!strcmp(Right.items[i].name,"DIRA")) return false; return true; }());

  // 4. cursor only (nothing selected)
  for(int i=0;i<Right.count;i++){ Right.items[i].sel=0; if(!strcmp(Right.items[i].name,"KEEP.TXT")) Right.cur=i; }
  g_keyval='y'; cmdDelete();
  check("cursor item deleted when nothing is selected",!ex("/tmp/fakeloc/KEEP.TXT")&&ex("/tmp/fakeloc/STUCKD"));

  // 5. something cannot be deleted: must stop, report, and not loop
  g_failremove="STUCK.TXT"; selectNames(&Right,"STUCKD"); g_keyval='y'; cmdDelete(); g_failremove=0;
  check("undeletable file: terminates, file and folder remain",ex("/tmp/fakeloc/STUCKD/STUCK.TXT"));
  printf("   failed=%lu last=%s\n",DelFailed,DelLast);

  // 6. drives
  { char l[28]; int n=listDrives(l); printf("drives: %d (%s)\n",n,l); check("drive list is A C D",n==3&&!strcmp(l,"ACD")); }
  strcpy(Right.path,"/tmp/fakeloc"); reload(&Right);
  g_keyval='a'; cmdDrive();      // A: "not ready" -> must restore
  check("not-ready drive: path restored",!strcmp(Right.path,"/tmp/fakeloc"));
  g_keyval='d'; cmdDrive();      // D: works
  check("D: selected -> right panel shows its folder",!strcmp(Right.path,"/tmp/fakeloc2")&&Right.count==2&&!strcmp(Right.items[0].name,"..")&&!strcmp(Right.items[1].name,"Z.TXT"));
  g_keyval=27;  cmdDrive();
  check("Esc in the drive chooser changes nothing",!strcmp(Right.path,"/tmp/fakeloc2"));

  // 7. host chooser (Enter keeps the proposed name and reconnects)
  g_keyval=13; Left.cur=2; cmdChooseHost();
  check("host chooser: reconnects and reloads the host root",Left.count==5&&!strcmp(HostName,"fake")&&Left.cur==0);
  // 8. bottom line follows the Alt key (BIOS flag byte at 0040:0017)
  {
    auto bar=[&](){ static char row[81]; for(int x=0;x<80;x++) row[x]=(char)(fakeVid[24*80+x]&0xFF); row[80]=0; return std::string(row); };
    g_scratch[0x17]=0;    drawKeyBar(); std::string a=bar();
    g_scratch[0x17]=0x08; drawKeyBar(); std::string b=bar();
    g_scratch[0x17]=0x04; drawKeyBar(); std::string c=bar();   // Ctrl only: still the normal labels
    g_scratch[0x17]=0;    drawKeyBar(); std::string d=bar();
    check("bottom line normally shows Delete and Quit",a.find("Delete")!=std::string::npos&&a.find("Quit")!=std::string::npos);
    check("with Alt held it shows Host and Drive instead",b.find("Host")!=std::string::npos&&b.find("Drive")!=std::string::npos&&b.find("Delete")==std::string::npos);
    check("Ctrl alone does not change the bottom line",c==a);
    check("after releasing Alt the normal labels are back",d==a);
  }

  // 9. settings (EC.INI)
  {
    setIniPath("C:\\EC\\EC.EXE"); check("INI path next to the program",!strcmp(IniPath,"C:\\EC\\EC.INI"));
    setIniPath("EC.EXE");          check("INI path without a folder",!strcmp(IniPath,"EC.INI"));
    setIniPath("C:EC.EXE");        check("INI path with a drive only",!strcmp(IniPath,"C:EC.INI"));

    strcpy(IniPath,"/tmp/ec_test.ini");
    strcpy(HostName,"192.168.50.54"); ServerPort=7000; Verify=1;
    saveSettings();
    SavedHost[0]=0; SavedPort=6502; Verify=0;
    loadSettings();
    check("settings survive a save and load",!strcmp(SavedHost,"192.168.50.54")&&SavedPort==7000&&Verify==1);

    system("printf 'host=pc\\r\\nport=99999\\nverify=0\\njunk\\n' > /tmp/ec_test.ini");
    SavedHost[0]=0; SavedPort=6502; Verify=1;
    loadSettings();
    check("bad port and unknown lines are ignored, CR LF is fine",!strcmp(SavedHost,"pc")&&SavedPort==6502&&Verify==0);

    strcpy(IniPath,"/tmp/does-not-exist/EC.INI"); SavedHost[0]=0;
    loadSettings(); saveSettings();   // neither may fail or crash
    check("missing or unwritable INI file is harmless",SavedHost[0]==0);
  }

  printf("%d failure(s)\n",failures);
  return failures;
}
