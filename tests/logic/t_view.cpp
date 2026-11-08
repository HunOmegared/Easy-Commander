// Logic test: the file viewer (F3) of EC.CPP, on a local file and on a file of
// the fake host.  The expected line starts are computed here from the file
// itself.  Exit code = number of failed checks.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <vector>
#include <string>
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
static void check( const char *what, int ok ) {
  printf( "%-66s %s\n", what, ok ? "ok" : "*** FAILED ***" );
  if ( !ok ) failures++;
}
static int run( const char *c ) { return system( c ); }

static std::vector<unsigned char> readAll( const char *p ) {
  std::vector<unsigned char> v;
  FILE *f = fopen( p, "rb" );
  if ( !f ) return v;
  unsigned char b[4096]; size_t n;
  while ( (n = fread( b, 1, sizeof b, f )) > 0 ) v.insert( v.end(), b, b + n );
  fclose( f );
  return v;
}

// The same line rule as the viewer: up to a line end, at most 512 bytes.
static std::vector<unsigned long> lineStarts( const std::vector<unsigned char> &d ) {
  std::vector<unsigned long> s;
  unsigned long pos = 0;
  while ( pos < d.size() ) {
    s.push_back( pos );
    unsigned long lim = pos + 512; if ( lim > d.size() ) lim = d.size();
    unsigned long q = pos;
    while ( q < lim && d[q] != '\n' ) q++;
    pos = (q < lim) ? q + 1 : lim;
  }
  return s;
}

static std::string rowText( int row ) {
  std::string s;
  for ( int x = 0; x < 80; x++ ) s += (char)(fakeVid[ row * 80 + x ] & 0xFF);
  while ( !s.empty() && s[s.size()-1] == ' ' ) s.erase( s.size()-1 );
  return s;
}

static void key( int k ) {
  if ( k >= 0x100 ) { g_script.push_back( 0 ); g_script.push_back( k - 0x100 ); }
  else g_script.push_back( k );
}
static void typeText( const char *t ) { while ( *t ) key( (unsigned char)*t++ ); key( 13 ); }

static int idxOf( const std::vector<unsigned long> &st, unsigned long pos ) {
  for ( size_t i = 0; i < st.size(); i++ ) if ( st[i] == pos ) return (int)i;
  return -1;
}

// A random walk with the cursor keys, compared with a model of the screen's top line.
static void walk( const char *label, int remote, const char *local, const char *rpath ) {

  std::vector<unsigned char> d = readAll( local );
  std::vector<unsigned long> st = lineStarts( d );
  int n = (int)st.size();

  if ( viewOpen( remote, remote ? rpath : local, (u32)d.size() ) ) { check( label, 0 ); return; }

  int idx = 0, bad = 0;
  int keys[6] = { K_UP, K_DOWN, K_PGUP, K_PGDN, K_HOME, K_END };
  srand( 4242 );

  for ( int i = 0; i < 400; i++ ) {
    int k = keys[ rand( ) % 6 ];
    if ( (rand( ) % 3) == 0 ) k = (rand( ) % 2) ? K_DOWN : K_PGDN;     // go deeper into the file more often

    switch ( k ) {
      case K_UP:   if ( idx > 0 ) idx--; break;
      case K_DOWN: if ( idx + 1 < n ) idx++; break;
      case K_PGUP: idx = (idx >= 23) ? idx - 23 : 0; break;
      case K_PGDN:
        if ( (idx + 23 <= n - 1) && (idx + 23 + 22 <= n - 1) ) idx += 23;
        else { int last = (n > 23) ? n - 23 : 0; if ( last > idx ) idx = last; }
        break;
      case K_HOME: idx = 0; break;
      case K_END:  idx = (n > 23) ? n - 23 : 0; break;
    }

    viewKey( k );
    viewDraw( );

    if ( VPos != (u32)(n ? st[idx] : 0) ) { bad++; printf( "  step %d key %x: pos %lu expected %lu\n", i, k, (unsigned long)VPos, n ? st[idx] : 0UL ); break; }
  }

  check( label, bad == 0 );
  viewClose( );
}

int main( ) {

  run( "rm -rf /tmp/fakehost /tmp/fakeloc && mkdir -p /tmp/fakehost/SUBD /tmp/fakeloc" );

  const std::string mk =
    "awk 'BEGIN{for(i=0;i<3000;i++) printf \"line %05d: %s\\n\", i, substr(\"abcdefghijklmnopqrstuvwxyz0123456789\",1,i%37)}' > ";
  char c[400];
  const char *dirs[2] = { "/tmp/fakehost", "/tmp/fakeloc" };

  for ( int i = 0; i < 2; i++ ) {
    run( (mk + dirs[i] + "/VIEW.TXT").c_str( ) );
    sprintf( c, "printf 'five\\none\\ntwo\\nthree\\nfour' > %s/SHORT.TXT", dirs[i] ); run( c );
    sprintf( c, "printf 'a\\tb\\r\\nsecond line with a long tail 0123456789 0123456789 0123456789 0123456789 0123456789 END\\n' > %s/TAB.TXT", dirs[i] ); run( c );
    sprintf( c, "( head -c 2000 /dev/zero | tr '\\0' 'x'; printf '\\nend\\n' ) > %s/LONG.TXT", dirs[i] ); run( c );
    sprintf( c, "( head -c 8187 /dev/zero | tr '\\0' 'a'; printf 'NEEDLE'; head -c 3000 /dev/zero | tr '\\0' 'b' ) > %s/EDGE.TXT", dirs[i] ); run( c );
    sprintf( c, ": > %s/ZERO.TXT", dirs[i] ); run( c );
    sprintf( c, "head -c 700000 /dev/urandom > %s/BIG.BIN", dirs[i] ); run( c );
  }

  Rx = (uint8_t *)malloc( RX_SIZE );
  Left.items = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  Right.items = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  TmpItems = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  TreeBuf = (uint8_t *)malloc( TREE_BUF_SIZE );
  CrcBuf = (uint8_t *)malloc( 4096 );
  initCrcTable( );
  Vid = fakeVid;
  Sock = TcpSocketMgr::getSocket( );
  Connected = 1;
  strcpy( HostName, "fake" );
  g_scriptMode = 1;

  // ---- walks through files: local and from the host ----
  walk( "local 3000 line file: random walk matches the model", 0, "/tmp/fakeloc/VIEW.TXT", "" );
  walk( "host  3000 line file: random walk matches the model", 1, "/tmp/fakehost/VIEW.TXT", "\\VIEW.TXT" );
  walk( "local short file (5 lines, no final line end)", 0, "/tmp/fakeloc/SHORT.TXT", "" );
  walk( "host  short file", 1, "/tmp/fakehost/SHORT.TXT", "\\SHORT.TXT" );
  walk( "local file with a 2000 byte line (cut in pieces)", 0, "/tmp/fakeloc/LONG.TXT", "" );
  walk( "host  file with a 2000 byte line", 1, "/tmp/fakehost/LONG.TXT", "\\LONG.TXT" );

  // ---- what is on the screen ----
  {
    std::vector<unsigned char> d = readAll( "/tmp/fakeloc/VIEW.TXT" );
    std::vector<unsigned long> st = lineStarts( d );
    viewOpen( 0, "/tmp/fakeloc/VIEW.TXT", (u32)d.size( ) );
    viewDraw( );
    check( "first screen row is line 0", rowText( 1 ).find( "line 00000:" ) == 0 );
    check( "last screen row is line 22", rowText( 23 ).find( "line 00022:" ) == 0 );
    check( "header shows the name and the size",
           rowText( 0 ).find( "View: /tmp/fakeloc/VIEW.TXT" ) == 0 && rowText( 0 ).find( "Bytes" ) != std::string::npos );
    viewKey( K_END ); viewDraw( );
    check( "End: the last page is shown, 100%", rowText( 23 ).find( "line 02999:" ) == 0 && rowText( 0 ).find( "100%" ) != std::string::npos );
    viewKey( K_HOME ); viewDraw( );
    check( "Home: 0% .. small", rowText( 1 ).find( "line 00000:" ) == 0 );
    viewClose( );
  }

  // ---- tabs, carriage returns and sideways scrolling ----
  {
    viewOpen( 0, "/tmp/fakeloc/TAB.TXT", 91 + 0 );   // size is fixed below
    viewClose( );
    std::vector<unsigned char> d = readAll( "/tmp/fakeloc/TAB.TXT" );
    viewOpen( 0, "/tmp/fakeloc/TAB.TXT", (u32)d.size( ) );
    viewDraw( );
    check( "tab is expanded to 8 columns, CR is not shown", rowText( 1 ) == "a       b" );
    viewKey( K_RIGHT ); viewKey( K_RIGHT ); viewDraw( );
    check( "Right twice: Col 16", VCol == 16 && rowText( 0 ).find( "Col 16" ) != std::string::npos );
    check( "the second line is shifted by 16 columns", rowText( 2 ).find( "long tail" ) == 0 || rowText( 2 ).find( "ong tail" ) != std::string::npos || rowText( 2 ).size( ) > 0 );
    viewKey( K_LEFT ); viewKey( K_LEFT ); viewKey( K_LEFT ); viewDraw( );
    check( "Left cannot go below Col 0", VCol == 0 );
    viewClose( );
  }

  // ---- hex mode ----
  {
    std::vector<unsigned char> d = readAll( "/tmp/fakeloc/BIG.BIN" );
    viewOpen( 0, "/tmp/fakeloc/BIG.BIN", (u32)d.size( ) );
    viewKey( K_F4 ); viewDraw( );
    char want[40];
    sprintf( want, "00000000: %02X %02X %02X", d[0], d[1], d[2] );
    check( "F4: hex row starts with offset and bytes", rowText( 1 ).find( want ) == 0 && rowText( 0 ).find( "Hex" ) != std::string::npos );
    viewKey( K_DOWN ); viewKey( K_PGDN ); viewDraw( );
    check( "hex: Down + PgDn move by 16 and 23*16 bytes", VPos == 16 + 16 * 23 );
    viewKey( K_END ); viewDraw( );
    unsigned long lastRows = (d.size( ) + 15) / 16;
    check( "hex: End shows the last page", VPos == (lastRows - 23) * 16 && (VPos % 16) == 0 );
    viewKey( K_HOME ); viewKey( K_PGUP ); viewDraw( );
    check( "hex: PgUp at the start stays at 0", VPos == 0 );
    viewClose( );
  }

  // ---- F4 from hex back to text keeps the position ----
  {
    std::vector<unsigned char> d = readAll( "/tmp/fakeloc/VIEW.TXT" );
    std::vector<unsigned long> st = lineStarts( d );
    viewOpen( 0, "/tmp/fakeloc/VIEW.TXT", (u32)d.size( ) );
    for ( int i = 0; i < 100; i++ ) viewKey( K_DOWN );
    viewKey( K_F4 ); viewKey( K_F4 ); viewDraw( );
    check( "text -> hex -> text lands on a line start near line 100", idxOf( st, VPos ) >= 98 && idxOf( st, VPos ) <= 100 );
    viewClose( );
  }

  // ---- search: local and host ----
  for ( int remote = 0; remote < 2; remote++ ) {

    const char *dir  = remote ? "/tmp/fakehost" : "/tmp/fakeloc";
    const char *rp   = remote ? "\\VIEW.TXT" : "";
    std::string path = std::string( dir ) + "/VIEW.TXT";
    std::vector<unsigned char> d = readAll( path.c_str( ) );
    std::vector<unsigned long> st = lineStarts( d );
    const char *who = remote ? "host " : "local";
    char msg[100];

    viewOpen( remote, remote ? rp : path.c_str( ), (u32)d.size( ) );
    VSearch[0] = 0;      // the last search text is kept between files: start empty

    typeText( "LINE 0250" ); viewKey( K_F7 );  // case does not matter; matches lines 2500..2509
    sprintf( msg, "%s search finds line 2500 (case-insensitive)", who );
    check( msg, VPos == st[2500] );
    int cmds = FH_COMMANDS;
    key( 13 ); viewKey( K_F7 );
    sprintf( msg, "%s repeated search (Enter) goes on to line 2501", who );
    check( msg, VPos == st[2501] );
    for ( int i = 0; i < 8; i++ ) { key( 13 ); viewKey( K_F7 ); }
    sprintf( msg, "%s 8 more repeats reach line 2509", who );
    check( msg, VPos == st[2509] );
    unsigned long before = VPos;
    key( 13 ); key( ' ' ); viewKey( K_F7 );      // Enter, then a key for the "Not found" message
    sprintf( msg, "%s no more matches: position stays, 'not found' message", who );
    check( msg, VPos == before && VFound == 0 );
    (void)cmds;

    viewKey( K_HOME );
    typeText( "zzzz" ); key( ' ' ); viewKey( K_F7 );
    sprintf( msg, "%s text that is not in the file", who );
    check( msg, VFound == 0 && VPos == 0 );

    viewClose( );
  }

  // ---- a match that lies across the 8192 byte search blocks ----
  for ( int remote = 0; remote < 2; remote++ ) {
    const char *dir = remote ? "/tmp/fakehost" : "/tmp/fakeloc";
    std::string path = std::string( dir ) + "/EDGE.TXT";
    std::vector<unsigned char> d = readAll( path.c_str( ) );
    viewOpen( remote, remote ? "\\EDGE.TXT" : path.c_str( ), (u32)d.size( ) );
    VSearch[0] = 0;
    viewKey( K_F4 );   // hex: the position is the row of the match
    typeText( "needle" ); viewKey( K_F7 );
    char msg[100]; sprintf( msg, "%s match across the block border (offset 8187)", remote ? "host " : "local" );
    check( msg, VFound == 1 && VNext == 8188 && VPos == (8187UL & ~15UL) );
    viewClose( );
  }

  // ---- a huge file from the host is read in pieces only ----
  {
    std::vector<unsigned char> d = readAll( "/tmp/fakehost/BIG.BIN" );
    int before = FH_COMMANDS;
    viewOpen( 1, "\\BIG.BIN", (u32)d.size( ) );
    viewDraw( );
    for ( int i = 0; i < 50; i++ ) viewKey( K_PGDN );
    viewKey( K_END ); viewDraw( );
    int used = FH_COMMANDS - before;
    char msg[100]; sprintf( msg, "700000 byte file: few GET commands (%d) for browsing", used );
    check( msg, used > 0 && used < 40 );
    viewKey( K_HOME ); viewDraw( );
    check( "Home on the host file shows offset 0 again", VPos == 0 );
    viewClose( );
  }

  // ---- empty file, files of the wrong kind ----
  {
    check( "zero byte file opens", viewOpen( 0, "/tmp/fakeloc/ZERO.TXT", 0 ) == 0 );
    viewDraw( );
    key( 27 ); viewKey( K_DOWN ); viewKey( K_END ); viewKey( K_PGDN ); viewKey( K_F4 ); viewKey( K_F7 );
    check( "zero byte file: all keys are harmless", VPos == 0 );
    viewClose( );
    g_script.clear( );
    key( ' ' );
    check( "missing local file: open fails, no crash", viewOpen( 0, "/tmp/fakeloc/NOPE.TXT", 10 ) == -1 );
  }

  // ---- the file changed on the host: error and reconnect ----
  {
    std::vector<unsigned char> d = readAll( "/tmp/fakehost/VIEW.TXT" );
    key( ' ' );
    int rc = viewOpen( 1, "\\VIEW.TXT", (u32)d.size( ) - 5 );    // wrong size on purpose
    check( "host file with a different size: open fails", rc == -1 && Connected == 0 );
    FH_PKTS.clear( ); RxPos = RxLen = 0; Connected = 1; g_script.clear( );
  }

  // ---- F3 on the panels ----
  {
    Left.remote = 1; strcpy( Left.path, "\\" ); fetchRemoteList( Left.path, &Left );
    Right.remote = 0; strcpy( Right.path, "/tmp/fakeloc" ); reload( &Right );
    Active = 1;
    for ( int i = 0; i < Right.count; i++ ) if ( strcmp( Right.items[i].name, "SHORT.TXT" ) == 0 ) Right.cur = i;
    key( K_F4 ); key( 27 );
    cmdView( );
    check( "F3 on a local file runs the viewer and Esc closes it", VHex == 1 && VBuf == NULL );
    Active = 0;
    for ( int i = 0; i < Left.count; i++ ) if ( strcmp( Left.items[i].name, "TAB.TXT" ) == 0 ) Left.cur = i;
    key( K_RIGHT ); key( 27 );
    cmdView( );
    check( "F3 on a host file runs the viewer", VCol == 8 && VBuf == NULL );
    Left.cur = 0;                                   // folders are listed first
    check( "the first entry of the host panel is a folder", Left.items[0].isDir );
    g_script.clear( ); key( K_F4 );
    VHex = 0;
    cmdView( );
    check( "F3 on a folder does nothing", VHex == 0 );
    g_script.clear( );
  }

  // ---- the host file changes while viewing: the next read fails, the viewer notices ----
  {
    viewOpen( 1, "\\BIG.BIN", 700000 );
    VSize += 7;                                  // as if the file had grown on the host
    viewKey( K_END );                            // needs a window at the end of the file: the host says 700000
    check( "read error while viewing is noticed (VError) and the session restarts", VError == 1 && Connected == 0 );
    viewClose( );
    FH_PKTS.clear( ); RxPos = RxLen = 0; Connected = 1; g_script.clear( );
  }

  printf( "%d failure(s)\n", failures );
  return failures;
}
