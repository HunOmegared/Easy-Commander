/*

   Easy Commander (step 4)

   A two panel, Norton Commander style text mode client for the Easy
   Commander host.

     - LEFT panel : the files of the host (read through the network)
     - RIGHT panel: real local DOS folders (the download target)

   Browse the host's folders on the left, pick the target folder on the
   right, select with Insert and press F5 to download files and whole
   folders (with their subfolders).

   Usage:  EC [host] [port]       (port defaults to 6502; without a host
                                   the program asks for the address)

   The networking code follows mTCP Netcat:
   mTCP Netcat.cpp, Copyright (C) 2006-2026 Michael B. Brutman
   (mbbrutman@gmail.com), http://www.brutman.com/mTCP

   Because it uses the mTCP library this program is distributed under the
   GNU General Public License version 3 or (at your option) any later
   version, like mTCP itself.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <bios.h>
#include <dos.h>
#include <i86.h>
#include <direct.h>
#include <time.h>
#include <malloc.h>

#include "types.h"

#include "timer.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "udp.h"
#include "dns.h"


typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;


#define SCR_W       80
#define LIST_ROWS   20            // file rows inside a panel
#define MAX_ITEMS   300           // per folder

// Colours (background in the high nibble)
#define A_BORDER    0x1B
#define A_NORMAL    0x1B          // light cyan on blue
#define A_DIR       0x1F          // white on blue
#define A_SEL       0x1E          // yellow on blue
#define A_CURSOR    0x30          // black on cyan
#define A_CURSEL    0x3E          // yellow on cyan
#define A_TITLE_ON  0x70          // black on grey (active panel path)
#define A_TITLE_OFF 0x1B
#define A_KEYNUM    0x07
#define A_KEYTXT    0x30
#define A_DLG       0x70
#define A_DLGB      0x7F
#define A_DLGHINT   0x78
#define A_FIELD     0x1F

// Box drawing characters (code page 437)
#define B_TL 0xC9
#define B_TR 0xBB
#define B_BL 0xC8
#define B_BR 0xBC
#define B_H  0xCD
#define B_V  0xBA
#define B_LT 0xC7
#define B_RT 0xB6
#define B_HS 0xC4
#define B_FULL  0xDB
#define B_LIGHT 0xB0

// Keys: plain characters are 0..255, extended keys are 0x100 + scan code.
#define K_UP    0x148
#define K_DOWN  0x150
#define K_PGUP  0x149
#define K_PGDN  0x151
#define K_HOME  0x147
#define K_END   0x14F
#define K_INS   0x152
#define K_F1    0x13B
#define K_F2    0x13C
#define K_F3    0x13D
#define K_F4    0x13E
#define K_F5    0x13F
#define K_F7    0x141
#define K_F8    0x142
#define K_F9    0x143
#define K_F10   0x144
#define K_LEFT  0x14B
#define K_RIGHT 0x14D
#define K_ALTF1 0x168
#define K_CTRL_O 0x0F      // Ctrl+O: look at the DOS screen
#define K_ALTF2 0x169


// ---------------------------------------------------------------------
// Data model

typedef struct {
  char name[13];
  u32  size;
  u16  date;                 // DOS packed date
  u16  time;                 // DOS packed time
  u8   isDir;
  u8   sel;
} Item;

typedef struct {
  Item *items;
  int   count;
  int   cur;
  int   top;
  int   remote;              // 1: sample (host) data, 0: local disk
  int   truncated;
  char  path[80];
} Panel;

static Panel Left;
static Panel Right;
static int   Active = 0;     // 0 = left, 1 = right
static int   Running = 1;

static void uiMessage( const char *title, const char *l1, const char *l2 );
static void pump( void );
static void selectAll( Panel *p, int mode );
static void saveSettings( void );


// ---------------------------------------------------------------------
// Screen

static u16 __far *Vid;
static u8  SavedMode;
static u16 SavedCursor;
static u16 SavedPos;                // cursor position of the DOS screen (DH row, DL column)
static u16 *SavedScreen;            // the DOS screen as it was at the start (NULL: not kept)
#define SCR_H       25

static void putCh( int x, int y, u8 ch, u8 attr ) {
  Vid[ y * SCR_W + x ] = ((u16)attr << 8) | ch;
}

static void putStr( int x, int y, const char *s, u8 attr ) {
  while ( *s && (x < SCR_W) ) {
    putCh( x, y, (u8)*s, attr );
    x++;
    s++;
  }
}

// Draw s in a field exactly w characters wide (cut or padded with spaces).
static void putStrPad( int x, int y, const char *s, int w, u8 attr ) {
  int i = 0;
  while ( (i < w) && s[i] ) { putCh( x + i, y, (u8)s[i], attr ); i++; }
  while ( i < w )           { putCh( x + i, y, ' ', attr ); i++; }
}

static void uiFill( int x, int y, int w, int h, u8 ch, u8 attr ) {
  for ( int j = 0; j < h; j++ ) {
    for ( int i = 0; i < w; i++ ) {
      putCh( x + i, y + j, ch, attr );
    }
  }
}

static void drawBox( int x, int y, int w, int h, u8 attr ) {
  putCh( x,       y,       B_TL, attr );
  putCh( x+w-1,   y,       B_TR, attr );
  putCh( x,       y+h-1,   B_BL, attr );
  putCh( x+w-1,   y+h-1,   B_BR, attr );
  uiFill( x+1,   y,     w-2, 1,   B_H, attr );
  uiFill( x+1,   y+h-1, w-2, 1,   B_H, attr );
  uiFill( x,     y+1,   1,   h-2, B_V, attr );
  uiFill( x+w-1, y+1,   1,   h-2, B_V, attr );
}

static void screenInit( void ) {

  union REGS r;

  r.h.ah = 0x0F;
  int86( 0x10, &r, &r );
  SavedMode = r.h.al & 0x7F;
  u8 page = r.h.bh;

  r.h.ah = 0x03;
  r.h.bh = page;
  int86( 0x10, &r, &r );
  SavedCursor = r.x.cx;
  SavedPos    = r.x.dx;

  if ( SavedMode == 7 ) {
    printf( "Easy Commander needs a colour display.\n" );
    exit( 1 );
  }

  Vid = (u16 __far *)MK_FP( 0xB800, 0 );

  // Like Norton Commander: keep the DOS screen (the start text of the
  // program, the commands before it) and give it back at the end.  This
  // works for the normal 80x25 text mode only.
  u8 rows = *(volatile u8 __far *)MK_FP( 0x40, 0x84 );     // 0 on old BIOS: 25 rows
  if ( ((SavedMode == 2) || (SavedMode == 3)) && (page == 0) && ((rows == 0) || (rows == SCR_H - 1)) ) {
    SavedScreen = (u16 *)malloc( SCR_W * SCR_H * 2 );
  }

  if ( SavedScreen ) {
    for ( int i = 0; i < SCR_W * SCR_H; i++ ) {
      SavedScreen[i] = Vid[i];
      Vid[i] = 0x0720;                          // blank, grey on black
    }
  }
  else {
    // 80x25 colour text; this also clears the screen.
    r.x.ax = 0x0003;
    int86( 0x10, &r, &r );
  }

  // Hide the hardware cursor.
  r.h.ah = 0x01;
  r.x.cx = 0x2000;
  int86( 0x10, &r, &r );
}

// Put the cursor back where the DOS screen had it.
static void dosCursor( void ) {

  union REGS r;

  r.h.ah = 0x01;
  r.x.cx = SavedCursor;
  int86( 0x10, &r, &r );

  r.h.ah = 0x02;
  r.h.bh = 0;
  r.x.dx = SavedPos;
  int86( 0x10, &r, &r );
}

static void screenDone( void ) {

  union REGS r;

  if ( SavedScreen ) {
    for ( int i = 0; i < SCR_W * SCR_H; i++ ) Vid[i] = SavedScreen[i];
    dosCursor( );
  }
  else {
    r.h.ah = 0x00;
    r.h.al = SavedMode;
    int86( 0x10, &r, &r );

    r.h.ah = 0x01;
    r.x.cx = SavedCursor;
    int86( 0x10, &r, &r );
  }
}

// ---------------------------------------------------------------------
// Keyboard and time

static int uiReadKey( void ) {
  while ( !kbhit( ) ) pump( );
  int c = getch( );
  if ( (c == 0) || (c == 0xE0) ) {
    c = getch( );
    return 0x100 + c;
  }
  return c;
}

static u32 uiTicks( void ) {
  return *(volatile u32 __far *)MK_FP( 0x40, 0x6C );
}

static void uiWaitTicks( u32 n ) {
  u32 t = uiTicks( );
  while ( (u32)(uiTicks( ) - t) < n ) { }
}


// ---------------------------------------------------------------------
// Panel contents

static int itemLess( const Item *a, const Item *b ) {
  if ( a->isDir != b->isDir ) return a->isDir > b->isDir;
  return strcmp( a->name, b->name ) < 0;
}

// Folders first, then by name.  A leading ".." stays on top.
static void sortItems( Panel *p ) {

  int start = 0;
  if ( (p->count > 0) && (strcmp( p->items[0].name, ".." ) == 0) ) start = 1;

  for ( int i = start + 1; i < p->count; i++ ) {
    Item t = p->items[i];
    int j = i - 1;
    while ( (j >= start) && itemLess( &t, &p->items[j] ) ) {
      p->items[j+1] = p->items[j];
      j--;
    }
    p->items[j+1] = t;
  }
}

static void addItem( Panel *p, const char *name, u8 isDir, u32 size, u16 date, u16 time ) {

  if ( p->count >= MAX_ITEMS ) {
    p->truncated = 1;
    return;
  }

  Item *it = &p->items[p->count];
  strncpy( it->name, name, 12 );
  it->name[12] = 0;
  it->isDir = isDir;
  it->size  = size;
  it->date  = date;
  it->time  = time;
  it->sel   = 0;
  p->count++;
}

// ---------------------------------------------------------------------
// Network: one TCP connection to the host, one command at a time.

#define RX_SIZE        8192
#define IO_TIMEOUT_MS  15000ul

typedef struct {
  TcpBuffer b;
  uint8_t data[1460];
} DataBuf;

volatile uint8_t CtrlBreakDetected = 0;

void __interrupt __far ctrlBreakHandler( ) {
  CtrlBreakDetected = 1;
}

void __interrupt __far ctrlCHandler( ) {
  // Do nothing
}

static TcpSocket  *Sock = NULL;
static uint8_t    *Rx;
static uint16_t    RxPos = 0;
static uint16_t    RxLen = 0;
static const char *ErrMsg = "";
static char        HostMsg[100];
static int         Connected = 0;
static IpAddr_t    ServerAddr;
static uint16_t    ServerPort = 6502;
static char        HostName[80];
static Item       *TmpItems;

// Keeps the TCP/IP stack alive; called whenever we wait for something.
static void pump( void ) {
  if ( Sock ) Utils::processPackets( );
}


// Pull whatever the socket has into our buffer. This uses the same
// raw packet path that Netcat uses by default (no TCP receive buffer).
// Returns: >0 number of new bytes, 0 nothing yet,
//          -1 remote side closed and nothing is left, -2 Ctrl-Break

static int16_t fillRx( void ) {

  Utils::processPackets( );

  if ( CtrlBreakDetected ) return -2;

  // Move unread bytes to the front to make room.
  if ( RxPos > 0 ) {
    if ( RxPos < RxLen ) {
      memmove( Rx, Rx + RxPos, RxLen - RxPos );
    }
    RxLen = RxLen - RxPos;
    RxPos = 0;
  }

  int16_t total = 0;

  // Only take a packet if its payload is guaranteed to fit.
  while ( (RX_SIZE - RxLen) >= MyMTU ) {

    uint8_t *packet = (uint8_t *)Sock->incoming.dequeue( );
    if ( packet == NULL ) break;

    IpHeader *ip = (IpHeader *)(packet + sizeof(EthHeader) );
    TcpHeader *tcp = (TcpHeader *)(ip->payloadPtr( ));
    uint8_t *userData = ((uint8_t *)tcp)+tcp->getTcpHlen( );
    uint16_t len = ip->payloadLen( ) - tcp->getTcpHlen( );

    memcpy( Rx + RxLen, userData, len );
    RxLen += len;
    total += len;

    // Give the buffer back to the TCP stack right away.
    Buffer_free( packet );
  }

  if ( total > 0 ) return total;

  if ( Sock->isRemoteClosed( ) ) return -1;

  return 0;
}


// Wait until at least one unread byte is in the buffer.
// Returns the number of unread bytes, or -1 (see ErrMsg).

static int16_t waitData( void ) {

  if ( RxPos < RxLen ) return RxLen - RxPos;

  clockTicks_t start = TIMER_GET_CURRENT( );

  while ( 1 ) {

    int16_t n = fillRx( );

    if ( n > 0 ) return RxLen - RxPos;

    if ( n == -2 ) { ErrMsg = "Ctrl-Break"; return -1; }
    if ( n == -1 ) { ErrMsg = "connection closed by host"; return -1; }

    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( IO_TIMEOUT_MS ) ) {
      ErrMsg = "timeout";
      return -1;
    }

  }

}


// Read one line (CR/LF or LF terminated) into dst without the line end.
// Returns the length, or -1 on error (see ErrMsg).

static int readLine( char *dst, int max ) {

  int n = 0;

  while ( 1 ) {

    if ( waitData( ) < 0 ) return -1;

    uint8_t c = Rx[RxPos++];

    if ( c == '\n' ) break;
    if ( c == '\r' ) continue;

    if ( n < (max - 1) ) {
      dst[n] = c;
      n++;
    }

  }

  dst[n] = 0;
  return n;
}


// ---------------------------------------------------------------------
// Sending (the same low level path Netcat uses)

static int sendCommand( const char *cmd ) {

  uint16_t len = strlen( cmd );

  if ( len > 1000 ) {
    ErrMsg = "command too long";
    return -1;
  }

  DataBuf *buf = (DataBuf *)TcpBuffer::getXmitBuf( );

  if ( buf == NULL ) {
    ErrMsg = "no transmit buffers available";
    return -1;
  }

  memcpy( buf->data, cmd, len );
  buf->data[len]   = 13;
  buf->data[len+1] = 10;
  buf->b.setDataLen( len + 2 );

  int16_t rc = Sock->enqueue( &buf->b );

  if ( rc ) {
    ErrMsg = "error sending command";
    return -1;
  }

  return 0;
}



// Text after "OK" in a response line.

static const char *afterOk( const char *line ) {
  if ( line[2] == ' ' ) return line + 3;
  return line + 2;
}



// Open (or re-open) the connection and check that the other end really is
// an Easy Commander host.  Returns 1 on success, 0 on failure (see ErrMsg).
static int connectHost( void ) {

  char line[100];

  if ( Sock ) {
    Sock->close( );
    TcpSocketMgr::freeSocket( Sock );
    Sock = NULL;
  }

  Connected = 0;
  RxPos = 0;
  RxLen = 0;
  CtrlBreakDetected = 0;

  Sock = TcpSocketMgr::getSocket( );

  uint16_t localPort = rand( ) + 1024;

  int8_t rc = Sock->connect( localPort, ServerAddr, ServerPort, 10000 );

  if ( rc != 0 ) {
    Sock->close( );
    TcpSocketMgr::freeSocket( Sock );
    Sock = NULL;
    ErrMsg = "could not connect to the host";
    return 0;
  }

  if ( sendCommand( "HELLO" ) || (readLine( line, sizeof( line ) ) < 0) ) {
    return 0;
  }

  if ( strncmp( line, "OK EASYCOMMANDER", 16 ) != 0 ) {
    ErrMsg = "that is not an Easy Commander host";
    return 0;
  }

  Connected = 1;
  return 1;
}

static int ensureConnected( void ) {
  if ( Connected ) return 1;
  return connectHost( );
}

static int parseNumber( const char *s, int len ) {
  int v = 0;
  while ( len-- && (*s >= '0') && (*s <= '9') ) v = v * 10 + (*s++ - '0');
  return v;
}

// Splits "D<tab>NAME<tab>size<tab>YYYYMMDD<tab>HHMM" (the line is changed).
static int parseListLine( char *line, u8 *isDir, char **name, u32 *size, u16 *date, u16 *time ) {

  char *t1 = strchr( line, '\t' );
  if ( ((line[0] != 'D') && (line[0] != 'F')) || (t1 == NULL) ) return 0;

  *isDir = (line[0] == 'D');
  *name = t1 + 1;

  char *t2 = strchr( *name, '\t' );
  if ( t2 == NULL ) return 0;
  *t2 = 0;

  char *t3 = strchr( t2 + 1, '\t' );
  if ( t3 == NULL ) return 0;
  *t3 = 0;

  char *t4 = strchr( t3 + 1, '\t' );
  if ( t4 == NULL ) return 0;
  *t4 = 0;

  *size = strtoul( t2 + 1, NULL, 10 );

  const char *d = t3 + 1;
  const char *tm = t4 + 1;

  if ( (strlen( d ) < 8) || (strlen( tm ) < 4) ) {
    *date = 0;
    *time = 0;
  }
  else {
    int y = parseNumber( d, 4 ) - 1980;
    if ( y < 0 ) y = 0;
    if ( y > 127 ) y = 127;
    *date = (u16)((y << 9) | (parseNumber( d + 4, 2 ) << 5) | parseNumber( d + 6, 2 ));
    *time = (u16)((parseNumber( tm, 2 ) << 11) | (parseNumber( tm + 2, 2 ) << 5));
  }

  return 1;
}

static int isRemoteRoot( const char *path ) {
  const char *p = path;
  while ( (*p == '\\') || (*p == '/') ) p++;
  return *p == 0;
}

// Reads a folder listing from the host into dst.
// Returns 0 on success, -1 if the connection failed, -2 if the host
// refused (the reason is in ErrMsg).
static int fetchRemoteList( const char *path, Panel *dst ) {

  char cmd[300];
  char line[300];

  dst->count = 0;
  dst->truncated = 0;

  if ( strlen( path ) > 250 ) {
    ErrMsg = "path too long";
    return -2;
  }

  if ( !ensureConnected( ) ) return -1;

  CtrlBreakDetected = 0;

  sprintf( cmd, "LIST %s", path );

  if ( sendCommand( cmd ) ) { Connected = 0; return -1; }

  if ( readLine( line, sizeof( line ) ) < 0 ) { Connected = 0; return -1; }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    strncpy( HostMsg, line, sizeof( HostMsg ) - 1 );
    HostMsg[sizeof( HostMsg ) - 1] = 0;
    ErrMsg = HostMsg;
    return -2;
  }

  unsigned long n = strtoul( afterOk( line ), NULL, 10 );

  if ( !isRemoteRoot( path ) ) addItem( dst, "..", 1, 0, 0, 0 );

  for ( unsigned long i = 0; i < n; i++ ) {

    if ( readLine( line, sizeof( line ) ) < 0 ) { Connected = 0; return -1; }

    u8 isDir;
    char *name;
    u32 size;
    u16 date, time;

    if ( parseListLine( line, &isDir, &name, &size, &date, &time ) ) {
      addItem( dst, name, isDir, size, date, time );
    }
  }

  sortItems( dst );
  return 0;
}

// Loads the host folder named by p->path.  The panel is only changed if
// that works; otherwise a message is shown and the old list stays.
static int reloadRemote( Panel *p ) {

  Panel tmp = *p;
  tmp.items = TmpItems;

  int rc = fetchRemoteList( p->path, &tmp );

  if ( rc == 0 ) {
    TmpItems = p->items;
    p->items = tmp.items;
    p->count = tmp.count;
    p->truncated = tmp.truncated;
    return 1;
  }

  uiMessage( "Host", (rc == -1) ? "Connection problem:" : "The host says:", ErrMsg );
  return 0;
}

// End the session the way a normal client would: ask the host to quit and
// wait (briefly) until it has closed its side before we close ours.
// Anything the host still sends is read and thrown away.

static void quitHost( void ) {

  if ( sendCommand( "QUIT" ) ) return;

  clockTicks_t start = TIMER_GET_CURRENT( );

  while ( 1 ) {

    int16_t n = fillRx( );

    if ( (n == -1) || (n == -2) ) break;    // closed by host, or Ctrl-Break

    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( 3000ul ) ) break;

  }

}



static int loadLocal( Panel *p ) {

  struct find_t fb;
  char pattern[100];

  p->count = 0;
  p->truncated = 0;

  int len = (int)strlen( p->path );

  if ( len > 3 ) addItem( p, "..", 1, 0, 0, 0 );

  sprintf( pattern, "%s%s*.*", p->path, (p->path[len-1] == '\\') ? "" : "\\" );

  unsigned rc = _dos_findfirst( pattern, _A_SUBDIR | _A_RDONLY | _A_ARCH, &fb );

  // 2 and 18 only mean "no files here"; anything else is a real problem.
  if ( (rc != 0) && (rc != 2) && (rc != 18) ) {
    return 0;
  }

  while ( rc == 0 ) {

    if ( strcmp( fb.name, "." ) && strcmp( fb.name, ".." ) ) {
      addItem( p, fb.name, (fb.attrib & _A_SUBDIR) ? 1 : 0, fb.size, fb.wr_date, fb.wr_time );
    }

    rc = _dos_findnext( &fb );
  }

  sortItems( p );
  return 1;
}

static void fixView( Panel *p ) {

  if ( p->cur >= p->count ) p->cur = p->count - 1;
  if ( p->cur < 0 ) p->cur = 0;

  if ( p->cur < p->top ) p->top = p->cur;
  if ( p->cur >= (p->top + LIST_ROWS) ) p->top = p->cur - LIST_ROWS + 1;
  if ( p->top < 0 ) p->top = 0;
}

static int reload( Panel *p ) {
  if ( p->remote ) {
    if ( !reloadRemote( p ) ) return 0;
  }
  else {
    if ( !loadLocal( p ) ) return 0;
  }
  fixView( p );
  return 1;
}

// Remove the last path component.  "\A\B" -> "\A", "\A" -> "\",
// "C:\A\B" -> "C:\A", "C:\A" -> "C:\".
static void pathUp( char *path ) {

  char *last = strrchr( path, '\\' );
  if ( last == NULL ) return;

  if ( last == path )          last[1] = 0;
  else if ( last[-1] == ':' )  last[1] = 0;
  else                         *last = 0;
}

static void pathDown( char *path, const char *name ) {

  int len = (int)strlen( path );

  if ( (len + (int)strlen( name ) + 2) >= 79 ) return;

  if ( path[len-1] != '\\' ) strcat( path, "\\" );
  strcat( path, name );
}

static void enterItem( Panel *p ) {

  if ( p->count == 0 ) return;

  Item *it = &p->items[p->cur];
  if ( !it->isDir ) return;

  char oldPath[80];
  strcpy( oldPath, p->path );

  char leaving[13];
  leaving[0] = 0;

  if ( strcmp( it->name, ".." ) == 0 ) {
    const char *last = strrchr( p->path, '\\' );
    if ( last ) { strncpy( leaving, last + 1, 12 ); leaving[12] = 0; }
    pathUp( p->path );
  }
  else {
    pathDown( p->path, it->name );
  }

  int oldCur = p->cur;
  int oldTop = p->top;

  p->cur = 0;
  p->top = 0;

  if ( !reload( p ) ) {
    // The folder could not be read: stay where we were.
    strcpy( p->path, oldPath );
    if ( !p->remote ) {
      loadLocal( p );
      uiMessage( "Folder", "Cannot read that folder.", NULL );
    }
    p->cur = oldCur;
    p->top = oldTop;
    return;
  }

  // When going up, put the cursor on the folder we just left.
  if ( leaving[0] ) {
    for ( int i = 0; i < p->count; i++ ) {
      if ( stricmp( p->items[i].name, leaving ) == 0 ) { p->cur = i; break; }
    }
    fixView( p );
  }
}

static int countSelected( Panel *p, u32 *bytes ) {

  int n = 0;
  *bytes = 0;

  for ( int i = 0; i < p->count; i++ ) {
    if ( p->items[i].sel ) {
      n++;
      *bytes += p->items[i].size;
    }
  }

  return n;
}


// ---------------------------------------------------------------------
// Drawing

static void formatItem( const Item *it, char *buf ) {

  if ( strcmp( it->name, ".." ) == 0 ) {
    sprintf( buf, "%-12s %9s", "..", "<UP-DIR>" );
    return;
  }

  char sz[16];
  if ( it->isDir ) strcpy( sz, "<DIR>" );
  else             sprintf( sz, "%lu", it->size );

  sprintf( buf, "%-12s %9s %02d-%02d-%02d %02d:%02d",
           it->name, sz,
           (int)((1980 + ((it->date >> 9) & 0x7F)) % 100),
           (int)((it->date >> 5) & 0x0F),
           (int)(it->date & 0x1F),
           (int)((it->time >> 11) & 0x1F),
           (int)((it->time >> 5) & 0x3F) );
}

static void drawPanel( Panel *p, int x0, int active ) {

  char buf[100];

  drawBox( x0, 0, 40, 24, A_BORDER );

  putCh( x0, 21, B_LT, A_BORDER );
  uiFill( x0 + 1, 21, 38, 1, B_HS, A_BORDER );
  putCh( x0 + 39, 21, B_RT, A_BORDER );

  // Path in the top border.
  if ( strlen( p->path ) <= 34 ) sprintf( buf, " %s ", p->path );
  else                           sprintf( buf, " ...%s ", p->path + strlen( p->path ) - 31 );
  putStr( x0 + (40 - (int)strlen( buf )) / 2, 0, buf, active ? A_TITLE_ON : A_TITLE_OFF );

  // File list.
  for ( int row = 0; row < LIST_ROWS; row++ ) {

    int idx = p->top + row;

    if ( idx >= p->count ) {
      uiFill( x0 + 1, 1 + row, 38, 1, ' ', A_NORMAL );
      continue;
    }

    Item *it = &p->items[idx];
    u8 attr = A_NORMAL;

    if ( it->isDir ) attr = A_DIR;
    if ( it->sel )   attr = A_SEL;
    if ( active && (idx == p->cur) ) attr = it->sel ? A_CURSEL : A_CURSOR;

    formatItem( it, buf );
    putStrPad( x0 + 1, 1 + row, buf, 38, attr );
  }

  // Info line: selection summary or the item under the cursor.
  u32 bytes;
  int n = countSelected( p, &bytes );

  if ( n > 0 ) {
    sprintf( buf, "Selected: %d item(s), %lu bytes", n, bytes );
  }
  else if ( p->truncated ) {
    sprintf( buf, "Too many items, first %d shown", MAX_ITEMS );
  }
  else if ( p->count > 0 ) {
    Item *it = &p->items[p->cur];
    if ( it->isDir ) sprintf( buf, "%s  <DIR>", it->name );
    else             sprintf( buf, "%s  %lu bytes", it->name, it->size );
  }
  else {
    strcpy( buf, "(empty)" );
  }
  putStrPad( x0 + 1, 22, buf, 38, A_NORMAL );

  // Which side this is.
  if ( p->remote ) sprintf( buf, " Host %.24s ", HostName );
  else             strcpy( buf, " Local " );
  putStr( x0 + 2, 23, buf, A_BORDER );
}

// The BIOS keeps the state of Shift, Ctrl and Alt at 0040:0017
// (bit 0 right Shift, 1 left Shift, 2 Ctrl, 3 Alt).
static u8 shiftFlags( void ) {
  return *(volatile u8 __far *)MK_FP( 0x40, 0x17 );
}

#define SHIFT_ALT 0x08

static int KeyBarAlt = 0;      // which labels are on the screen right now

// The bottom line shows what the function keys do.  While Alt is held down
// it shows what the Alt+function keys do, like Norton Commander.
static void drawKeyBar( void ) {

  static const char *normal[10] = {
    "Help", "Reconn", "View", "", "Get", "", "MkDir", "Delete", "Opts", "Quit"
  };

  static const char *withAlt[10] = {
    "Host", "Drive", "", "", "", "", "", "", "", ""
  };

  KeyBarAlt = (shiftFlags( ) & SHIFT_ALT) ? 1 : 0;

  const char **labels = KeyBarAlt ? withAlt : normal;

  for ( int i = 0; i < 10; i++ ) {

    char num[4];
    sprintf( num, "%d", i + 1 );

    int x = i * 8;
    putStr( x, 24, num, A_KEYNUM );
    putStrPad( x + (int)strlen( num ), 24, labels[i], 8 - (int)strlen( num ), A_KEYTXT );
  }
}

static void drawAll( void ) {
  drawPanel( &Left,  0,  Active == 0 );
  drawPanel( &Right, 40, Active == 1 );
  drawKeyBar( );
}


// ---------------------------------------------------------------------
// Dialogs.  The caller redraws the screen afterwards.

static void dialogFrame( int x, int y, int w, int h, const char *title ) {

  char t[60];

  uiFill( x, y, w, h, ' ', A_DLG );
  drawBox( x, y, w, h, A_DLGB );

  sprintf( t, " %s ", title );
  putStr( x + (w - (int)strlen( t )) / 2, y, t, A_DLG );
}

// Shows lines of text.  yesno: Enter/Y gives 1, Esc/N gives 0.
// Otherwise any key closes it and 1 is returned.
static int uiDialog( const char *title, const char **lines, int n, int yesno ) {

  const char *hint = (yesno == 2) ? "Y = Yes     Esc = No" : yesno ? "Enter = Yes     Esc = No" : "Press any key";

  int w = (int)strlen( title ) + 6;
  int i;

  for ( i = 0; i < n; i++ ) {
    if ( (int)strlen( lines[i] ) + 6 > w ) w = (int)strlen( lines[i] ) + 6;
  }
  if ( (int)strlen( hint ) + 6 > w ) w = (int)strlen( hint ) + 6;
  if ( w > 76 ) w = 76;

  int h = n + 5;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;

  dialogFrame( x, y, w, h, title );

  for ( i = 0; i < n; i++ ) {
    putStrPad( x + 3, y + 2 + i, lines[i], w - 6, A_DLG );
  }

  putStr( x + (w - (int)strlen( hint )) / 2, y + 3 + n, hint, A_DLGHINT );

  while ( 1 ) {
    int k = uiReadKey( );
    if ( !yesno ) return 1;
    if ( ((k == 13) && (yesno != 2)) || (k == 'y') || (k == 'Y') ) return 1;
    if ( (k == 27) || (k == 'n') || (k == 'N') ) return 0;
  }
}

static void uiMessage( const char *title, const char *l1, const char *l2 ) {
  const char *lines[2];
  lines[0] = l1;
  lines[1] = l2;
  uiDialog( title, lines, l2 ? 2 : 1, 0 );
}

// Asks for a short text.  Returns 1 on Enter (with text), 0 on Esc.
static int inputBox( const char *title, const char *prompt, char *buf, int maxlen ) {

  int w = 44, h = 7;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;
  int len = (int)strlen( buf );
  int fresh = (len > 0);              // text that was there at the start: the first typed character replaces it

  while ( 1 ) {

    dialogFrame( x, y, w, h, title );
    putStr( x + 3, y + 2, prompt, A_DLG );
    uiFill( x + 3, y + 3, w - 6, 1, ' ', A_FIELD );
    putStr( x + 3, y + 3, buf, A_FIELD );
    putCh( x + 3 + len, y + 3, '_', A_FIELD );
    putStr( x + 3, y + 5, "Enter = OK     Esc = Cancel", A_DLGHINT );

    int k = uiReadKey( );

    if ( k == 13 ) return len > 0;
    if ( k == 27 ) return 0;

    if ( (k == 8) && (len > 0) ) {
      len--;
      buf[len] = 0;
    }
    else if ( (k >= 32) && (k < 127) ) {
      if ( fresh ) { len = 0; buf[0] = 0; }
      if ( len < maxlen ) {
        buf[len++] = (char)k;
        buf[len] = 0;
      }
    }
    fresh = 0;
  }
}

// ---------------------------------------------------------------------
// Downloading

#define TREE_BUF_SIZE  60000u         // room for the list of files to fetch
#define BLOCK_SIZE     65536UL        // the host sends files in blocks of this size

#define FETCH_OK     0
#define FETCH_FATAL  4                // connection problem: re-open the session
#define FETCH_SKIP   5                // this file failed, the session is fine
#define FETCH_BADCRC 6                // file is complete but the CRC32 differs
#define FETCH_CANCEL 7
#define FETCH_DISK   8

static uint8_t      *TreeBuf;
static uint16_t      TreeUsed = 0;
static unsigned long TreeEntries = 0;
static unsigned long TreeFiles = 0;
static unsigned long TreeDirs = 0;
static unsigned long TreeBad = 0;
static u32           TreeBytes = 0;
static int           TreeTooBig = 0;

static uint8_t      *CrcBuf;
#define DISK_BUF_SIZE 16384u
static char         *DiskBuf;         // bigger output buffer: fewer, larger disk writes
static uint32_t      CrcTable[256];
static int           Verify = 0;          // F9: check every file with CRC32
static int           FastCrc = 1;         // F9: faster CRC32 loop (same result, for slow CPUs)
static int           FastCrcOk = 0;       // set by the self-test in initCrcTable
static int           OverwriteMode = 0;   // 0 ask, 1 overwrite all, 2 skip all
static char          LastError[120];

// ---- CRC32 (same code as the test client) ----

// The classic loop: one 32 bit shift and one 32 bit XOR per byte.  On a
// 16 bit compiler every 32 bit operation takes several instructions.
static uint32_t crcStandard( uint32_t crc, const uint8_t *buf, uint16_t n ) {
  for ( uint16_t i = 0; i < n; i++ ) {
    crc = CrcTable[ (crc ^ buf[i]) & 0xFF ] ^ (crc >> 8);
  }
  return crc;
}

// The same calculation with the CRC kept as two 16 bit halves (little endian:
// the table entry's low word comes first).  No 32 bit shifts are needed, which
// is much faster on 8086..386 class CPUs.  The result is identical.
static uint32_t crcFast( uint32_t crc, const uint8_t *buf, uint16_t n ) {

  uint16_t lo = (uint16_t)crc;
  uint16_t hi = (uint16_t)(crc >> 16);

  for ( uint16_t i = 0; i < n; i++ ) {
    const uint16_t *t = (const uint16_t *)&CrcTable[ (uint8_t)(lo ^ buf[i]) ];
    lo = (uint16_t)((lo >> 8) | (hi << 8)) ^ t[0];
    hi = (uint16_t)(hi >> 8) ^ t[1];
  }

  return ((uint32_t)hi << 16) | lo;
}

static void initCrcTable( void ) {

  for ( uint16_t n = 0; n < 256; n++ ) {
    uint32_t c = n;
    for ( int k = 0; k < 8; k++ ) {
      if ( c & 1 ) c = 0xEDB88320UL ^ (c >> 1);
      else         c = c >> 1;
    }
    CrcTable[n] = c;
  }

  // Self-test of the fast loop with the standard CRC32 check value.  If it
  // ever gives another answer, the standard loop is used instead.
  FastCrcOk = ( (crcFast( 0xFFFFFFFFUL, (const uint8_t *)"123456789", 9 ) ^ 0xFFFFFFFFUL) == 0xCBF43926UL );

}


// Read a file back from disk and compute its size and CRC32.
// Returns 0 on success, -1 if the file could not be read.

static int crcOfFile( const char *name, uint32_t *crcOut, uint32_t *sizeOut ) {

  FILE *f = fopen( name, "rb" );
  if ( f == NULL ) return -1;

  uint32_t crc  = 0xFFFFFFFFUL;
  uint32_t size = 0;

  while ( 1 ) {

    uint16_t n = fread( CrcBuf, 1, 4096, f );
    if ( n == 0 ) break;

    size += n;

    if ( FastCrc && FastCrcOk ) crc = crcFast( crc, CrcBuf, n );
    else           crc = crcStandard( crc, CrcBuf, n );

  }

  if ( ferror( f ) ) {
    fclose( f );
    return -1;
  }

  fclose( f );

  *crcOut  = crc ^ 0xFFFFFFFFUL;
  *sizeOut = size;
  return 0;
}



// Ask the host for the CRC32 of a file and compare it with the copy on
// disk.  The file is read back from the disk, so disk errors are caught.
static int verifyFile( const char *remote, const char *localName ) {

  char line[300];
  char cmd[300];

  sprintf( cmd, "CRC %s", remote );

  if ( sendCommand( cmd ) || (readLine( line, sizeof( line ) ) < 0) ) {
    Connected = 0;
    return FETCH_FATAL;
  }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    strncpy( HostMsg, line, sizeof( HostMsg ) - 1 );
    HostMsg[sizeof( HostMsg ) - 1] = 0;
    ErrMsg = HostMsg;
    return FETCH_SKIP;
  }

  char *endp;
  uint32_t hostSize = strtoul( afterOk( line ), &endp, 10 );
  uint32_t hostCrc  = strtoul( endp, NULL, 16 );

  uint32_t diskCrc, diskSize;

  if ( crcOfFile( localName, &diskCrc, &diskSize ) ) {
    ErrMsg = "cannot read the file back from the disk";
    return FETCH_BADCRC;
  }

  if ( (diskCrc != hostCrc) || (diskSize != hostSize) ) {
    ErrMsg = "CRC32 mismatch";
    return FETCH_BADCRC;
  }

  return FETCH_OK;
}

// ---- local helpers ----

static void joinPath( char *out, const char *dir, const char *rel ) {
  strcpy( out, dir );
  uint16_t len = strlen( out );
  if ( (len > 0) && (out[len-1] != '\\') && (out[len-1] != '/') && (out[len-1] != ':') ) {
    out[len] = '\\';
    out[len+1] = 0;
  }
  strcat( out, rel );
}


// Create a folder and any missing parents.  Errors are ignored here: an
// existing folder also makes mkdir fail, and a real problem shows up later
// when a file cannot be created.

static void makeDirs( const char *path ) {

  char tmp[260];

  if ( strlen( path ) >= sizeof( tmp ) ) return;
  strcpy( tmp, path );

  for ( uint16_t i = 0; tmp[i]; i++ ) {
    if ( ((tmp[i] == '\\') || (tmp[i] == '/')) && (i > 0) && (tmp[i-1] != ':') ) {
      char c = tmp[i];
      tmp[i] = 0;
      mkdir( tmp );
      tmp[i] = c;
    }
  }

  mkdir( tmp );
}



// Does a file exist on the local disk?  Returns its size if so.
static int localInfo( const char *path, u32 *size ) {

  struct find_t fb;

  if ( _dos_findfirst( path, _A_RDONLY | _A_ARCH, &fb ) == 0 ) {
    *size = fb.size;
    return 1;
  }

  return 0;
}

// ---- the list of things to download ----

// An entry in the list: type (1 byte), size (4), DOS date (2), DOS time (2), name.
#define ENTRY_HEAD 9

static void addEntry( char type, const char *rel, u32 size, u16 date, u16 time ) {

  uint16_t relLen = (uint16_t)strlen( rel );

  if ( (relLen == 0) || (relLen > 120) ) {
    TreeBad++;
    return;
  }

  if ( ((u32)TreeUsed + ENTRY_HEAD + relLen + 1) > TREE_BUF_SIZE ) {
    TreeTooBig = 1;
    return;
  }

  TreeBuf[TreeUsed] = type;
  memcpy( TreeBuf + TreeUsed + 1, &size, 4 );
  memcpy( TreeBuf + TreeUsed + 5, &date, 2 );
  memcpy( TreeBuf + TreeUsed + 7, &time, 2 );
  memcpy( TreeBuf + TreeUsed + ENTRY_HEAD, rel, relLen + 1 );
  TreeUsed += ENTRY_HEAD + relLen + 1;
  TreeEntries++;

  if ( type == 'D' ) {
    TreeDirs++;
  }
  else {
    TreeFiles++;
    TreeBytes += size;
  }
}

// Adds everything below a host folder (prefix is its name relative to the
// folder shown in the panel).  Returns 0, -1 (connection) or -2 (host).
static int appendTree( const char *remoteDir, const char *prefix ) {

  char cmd[300];
  char line[300];
  char rel[200];

  if ( strlen( remoteDir ) > 250 ) {
    ErrMsg = "path too long";
    return -2;
  }

  if ( !ensureConnected( ) ) return -1;

  CtrlBreakDetected = 0;

  sprintf( cmd, "TREE %s", remoteDir );

  if ( sendCommand( cmd ) ) { Connected = 0; return -1; }
  if ( readLine( line, sizeof( line ) ) < 0 ) { Connected = 0; return -1; }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    strncpy( HostMsg, line, sizeof( HostMsg ) - 1 );
    HostMsg[sizeof( HostMsg ) - 1] = 0;
    ErrMsg = HostMsg;
    return -2;
  }

  unsigned long n = strtoul( afterOk( line ), NULL, 10 );

  for ( unsigned long i = 0; i < n; i++ ) {

    if ( readLine( line, sizeof( line ) ) < 0 ) { Connected = 0; return -1; }

    u8 isDir;
    char *name;
    u32 size;
    u16 date, time;

    if ( !parseListLine( line, &isDir, &name, &size, &date, &time ) ) {
      TreeBad++;
      continue;
    }

    if ( (strlen( prefix ) + 1 + strlen( name )) >= sizeof( rel ) ) {
      TreeBad++;
      continue;
    }

    sprintf( rel, "%s\\%s", prefix, name );
    addEntry( isDir ? 'D' : 'F', rel, size, date, time );
  }

  return 0;
}

// Collects the selected items (or the one under the cursor) of a host
// panel.  Folders are expanded through the host's TREE command.
// Returns 0 on success; on failure a message has been shown.
static int buildList( Panel *p ) {

  TreeUsed = 0;
  TreeEntries = TreeFiles = TreeDirs = TreeBad = 0;
  TreeBytes = 0;
  TreeTooBig = 0;

  u32 bytes;
  int n = countSelected( p, &bytes );

  for ( int i = 0; i < p->count; i++ ) {

    Item *it = &p->items[i];

    if ( strcmp( it->name, ".." ) == 0 ) continue;
    if ( (n > 0) ? !it->sel : (i != p->cur) ) continue;

    if ( it->isDir ) {

      char remote[300];

      addEntry( 'D', it->name, 0, it->date, it->time );
      joinPath( remote, p->path, it->name );

      int rc = appendTree( remote, it->name );

      if ( rc ) {
        uiMessage( "Host", (rc == -1) ? "Connection problem:" : "The host says:", ErrMsg );
        return -1;
      }
    }
    else {
      addEntry( 'F', it->name, it->size, it->date, it->time );
    }
  }

  return 0;
}

// ---- progress dialog ----

static u32           ProgTotal, ProgDone, ProgFileSize, ProgFileDone;
static u32           ProgStart, ProgLast;
static unsigned long ProgFileNo, ProgFileCount;
static char          ProgName[16];
static int           ProgCancel, ProgPhase;      // phase 1: checking the CRC

#define PW 62
#define PH 12

static int percent( u32 done, u32 total ) {

  if ( (total == 0) || (done >= total) ) return 100;

  u32 t = total >> 10;
  if ( t == 0 ) return 0;

  return (int)(((done >> 10) * 100UL) / t);
}

static void drawBar( int x, int y, int w, int pct ) {

  int filled = (w * pct) / 100;

  uiFill( x, y, filled, 1, B_FULL, A_DLG );
  uiFill( x + filled, y, w - filled, 1, B_LIGHT, A_DLG );
}

static void progressFrame( void ) {

  int x = (SCR_W - PW) / 2;
  int y = (25 - PH) / 2;

  dialogFrame( x, y, PW, PH, "Downloading" );
  putStr( x + 3, y + 10, "Esc = cancel (takes effect after the current block)", A_DLGHINT );
}

static void progressUpdate( int force ) {

  u32 now = uiTicks( );

  if ( !force && ((u32)(now - ProgLast) < 2) ) return;
  ProgLast = now;

  if ( kbhit( ) ) {
    if ( uiReadKey( ) == 27 ) ProgCancel = 1;
  }

  int x = (SCR_W - PW) / 2;
  int y = (25 - PH) / 2;
  char buf[100];

  sprintf( buf, "%s %lu of %lu: %s", ProgPhase ? "Checking file" : "File",
           ProgFileNo, ProgFileCount, ProgName );
  putStrPad( x + 3, y + 2, buf, PW - 6, A_DLG );

  int pct = percent( ProgFileDone, ProgFileSize );
  drawBar( x + 3, y + 3, 50, pct );
  sprintf( buf, "%3d %%", pct );
  putStr( x + 54, y + 3, buf, A_DLG );

  sprintf( buf, "Total: %lu of %lu bytes", ProgDone, ProgTotal );
  putStrPad( x + 3, y + 5, buf, PW - 6, A_DLG );

  pct = percent( ProgDone, ProgTotal );
  drawBar( x + 3, y + 6, 50, pct );
  sprintf( buf, "%3d %%", pct );
  putStr( x + 54, y + 6, buf, A_DLG );

  u32 ms = (u32)(now - ProgStart) * 55;
  sprintf( buf, "Speed: %lu KB/s", ms ? ((ProgDone >> 10) * 1000UL) / ms : 0UL );
  putStrPad( x + 3, y + 8, buf, PW - 6, A_DLG );
}

// ---- transfer of one file ----

// The file is requested in blocks.  After every block the session is in
// a clean state, so Esc (and a full disk) can stop the transfer without
// breaking the connection.
static int downloadFile( const char *remote, const char *localName, u32 *gotOut, u16 date, u16 time ) {

  char line[300];
  char cmd[300];

  *gotOut = 0;

  if ( strlen( remote ) > 230 ) {
    ErrMsg = "path too long";
    return FETCH_SKIP;
  }

  FILE *f = fopen( localName, "wb" );
  if ( f == NULL ) {
    ErrMsg = "cannot create the file";
    return FETCH_SKIP;
  }

  // Write to the disk in big blocks instead of one network packet at a time.
  if ( DiskBuf ) setvbuf( f, DiskBuf, _IOFBF, DISK_BUF_SIZE );

  CtrlBreakDetected = 0;

  u32 offset = 0;
  u32 total  = 0;
  int first  = 1;
  int rc     = FETCH_OK;
  int aborting = 0;

  while ( 1 ) {

    sprintf( cmd, "GET %s %lu %lu", remote, (unsigned long)offset, (unsigned long)BLOCK_SIZE );

    if ( sendCommand( cmd ) || (readLine( line, sizeof( line ) ) < 0) ) {
      Connected = 0;
      rc = FETCH_FATAL;
      break;
    }

    if ( (line[0] != 'O') || (line[1] != 'K') ) {
      strncpy( HostMsg, line, sizeof( HostMsg ) - 1 );
      HostMsg[sizeof( HostMsg ) - 1] = 0;
      ErrMsg = HostMsg;
      rc = FETCH_SKIP;
      break;
    }

    char *endp;
    u32 n = strtoul( afterOk( line ), &endp, 10 );
    u32 fileTotal = strtoul( endp, NULL, 10 );

    if ( first ) {
      total = fileTotal;
      ProgFileSize = fileTotal;
      first = 0;
    }

    u32 remaining = n;

    while ( remaining ) {

      if ( waitData( ) < 0 ) {
        Connected = 0;
        rc = FETCH_FATAL;
        break;
      }

      uint16_t avail = RxLen - RxPos;
      uint16_t chunk = avail;
      if ( remaining < avail ) chunk = (uint16_t)remaining;

      // After a disk error or Esc the rest of this block is read and thrown away.
      if ( !aborting ) {
        if ( fwrite( Rx + RxPos, 1, chunk, f ) != chunk ) {
          aborting = 1;
          rc = FETCH_DISK;
          ErrMsg = "cannot write to the disk (full?)";
        }
        else {
          ProgFileDone += chunk;
          ProgDone += chunk;
        }
      }

      RxPos += chunk;
      remaining -= chunk;

      progressUpdate( 0 );

      if ( ProgCancel && !aborting ) {
        aborting = 1;
        rc = FETCH_CANCEL;
        ErrMsg = "cancelled";
      }
    }

    if ( rc == FETCH_FATAL ) break;
    if ( aborting ) break;

    offset += n;

    if ( (n == 0) || (offset >= total) ) break;
  }

  // Write the last buffer, then give the file the date and time it has on the
  // host (the file is flushed first: a later write would change them again).
  int flushRc = 0;

  if ( rc == FETCH_OK ) {
    flushRc = fflush( f );
    if ( (flushRc == 0) && (date != 0) ) _dos_setftime( fileno( f ), date, time );
  }

  // Closing can fail too (disk full).
  int closeRc = fclose( f );
  if ( flushRc != 0 ) closeRc = flushRc;

  if ( (rc == FETCH_OK) && (closeRc != 0) ) {
    rc = FETCH_DISK;
    ErrMsg = "cannot write to the disk (full?)";
  }

  if ( (rc == FETCH_OK) && (offset < total) ) {
    rc = FETCH_SKIP;
    ErrMsg = "the file changed on the host";
  }

  if ( rc == FETCH_OK ) {

    *gotOut = offset;

    if ( Verify ) {
      ProgPhase = 1;
      progressUpdate( 1 );
      rc = verifyFile( remote, localName );   // the file stays, whatever the result
      ProgPhase = 0;
    }
  }
  else {
    remove( localName );                      // no half files left behind
  }

  return rc;
}

// ---- questions ----

// Returns 'Y' overwrite, 'N' skip, 'A' overwrite all, 'S' skip all, 'C' cancel.
static int askOverwrite( const char *rel, u32 newSize, u32 oldSize ) {

  char l1[80], l2[80];
  int w = 64, h = 9;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;

  sprintf( l1, "File exists: %.48s", rel );
  sprintf( l2, "on disk %lu bytes, new %lu bytes", (unsigned long)oldSize, (unsigned long)newSize );

  dialogFrame( x, y, w, h, "Overwrite?" );
  putStrPad( x + 3, y + 2, l1, w - 6, A_DLG );
  putStrPad( x + 3, y + 3, l2, w - 6, A_DLG );
  putStrPad( x + 3, y + 5, "Y = Overwrite   N = Skip   A = Overwrite all", w - 6, A_DLGHINT );
  putStrPad( x + 3, y + 6, "S = Skip all    Esc = Cancel", w - 6, A_DLGHINT );

  while ( 1 ) {
    int k = uiReadKey( );
    if ( (k == 'y') || (k == 'Y') || (k == 13) ) return 'Y';
    if ( (k == 'n') || (k == 'N') ) return 'N';
    if ( (k == 'a') || (k == 'A') ) return 'A';
    if ( (k == 's') || (k == 'S') ) return 'S';
    if ( k == 27 ) return 'C';
  }
}

static void cmdOptions( void ) {

  int w = 62, h = 12;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;

  while ( 1 ) {

    char l1[80], l2[80];

    dialogFrame( x, y, w, h, "Options" );

    sprintf( l1, "[%c]  V = check every download with CRC32", Verify ? 'X' : ' ' );
    putStrPad( x + 3, y + 2, l1, w - 6, A_DLG );
    putStrPad( x + 3, y + 3, "     (slower, but proves the copy on the disk is exact)", w - 6, A_DLG );
    sprintf( l2, "[%c]  F = fast CRC32 code (for slow CPUs, same result)", FastCrc ? 'X' : ' ' );
    putStrPad( x + 3, y + 5, l2, w - 6, A_DLG );
    putStrPad( x + 3, y + 6, "     (switch it off only if you suspect a problem)", w - 6, A_DLG );
    putStr( x + 3, y + 9, "V, F = switch      Enter or Esc = close", A_DLGHINT );

    int k = uiReadKey( );

    if ( (k == 'v') || (k == 'V') || (k == ' ') ) {
      Verify = !Verify;
      saveSettings( );
    }
    if ( (k == 'f') || (k == 'F') ) {
      FastCrc = !FastCrc;
      saveSettings( );
    }
    if ( (k == 13) || (k == 27) ) break;
  }
}

// ---------------------------------------------------------------------
// File viewer (F3).  Works on a file of the host (read in pieces with GET
// offset length, so only what is looked at is transferred) or on a local file.
// Only a window of the file is kept in memory.  Text mode shows lines (cut
// into pieces of VIEW_MAXLINE bytes when there is no line end); hex mode
// shows 16 bytes per row.

#define VIEW_WIN      40960u    // bytes kept in memory
#define VIEW_PRE      12288u    // a new window starts this far before the top line
#define VIEW_BACK      4096u    // at least this much must be kept before the top line
#define VIEW_AHEAD    12288u    // at least this much must be kept after the top line
#define VIEW_MAXLINE    512u    // a text line is never longer than this
#define VIEW_SBUF      8192u    // search block
#define VIEW_ROWS        23

#define A_VIEWHEAD  0x30
#define A_VIEWTEXT  0x1B

static int       VRemote;
static char      VPath[300];
static FILE     *VFile;
static u32       VSize;
static u8       *VBuf;            // the window
static u8       *VSbuf;           // search block
static u32       VBase;           // file offset of VBuf[0]
static uint16_t  VLen;            // bytes in the window
static int       VLoaded;
static u32       VPos;            // file offset of the first line on the screen
static int       VHex;
static uint16_t  VCol;
static char      VSearch[40];
static u32       VNext;           // where a repeated search continues
static int       VFound;
static int       VError;          // reading failed while viewing: the viewer closes

// Read len bytes at offset off into dst.  Returns the number of bytes (less
// than len only at the end of the file), or -1 (see ErrMsg).  This is a long:
// an int has only 16 bits in this compiler and a window is bigger than 32767.
static long viewRead( u32 off, uint16_t len, u8 *dst ) {

  if ( off >= VSize ) return 0;
  if ( (u32)len > VSize - off ) len = (uint16_t)(VSize - off);

  if ( !VRemote ) {
    if ( fseek( VFile, (long)off, SEEK_SET ) ) {
      ErrMsg = "cannot read the file";
      return -1;
    }
    uint16_t n = (uint16_t)fread( dst, 1, len, VFile );
    if ( (n != len) && ferror( VFile ) ) {
      ErrMsg = "cannot read the file";
      return -1;
    }
    return (long)n;
  }

  char cmd[340];
  char line[300];

  if ( !ensureConnected( ) ) return -1;

  sprintf( cmd, "GET %s %lu %u", VPath, (unsigned long)off, (unsigned)len );

  if ( sendCommand( cmd ) || (readLine( line, sizeof( line ) ) < 0) ) {
    Connected = 0;
    return -1;
  }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    strncpy( HostMsg, line, sizeof( HostMsg ) - 1 );
    HostMsg[sizeof( HostMsg ) - 1] = 0;
    ErrMsg = HostMsg;
    return -1;
  }

  char *endp;
  u32 n = strtoul( afterOk( line ), &endp, 10 );
  u32 total = strtoul( endp, NULL, 10 );

  if ( (n > (u32)len) || (total != VSize) ) {
    ErrMsg = (n > (u32)len) ? "unexpected answer from the host" : "the file changed on the host";
    Connected = 0;                 // the answer is not read: start the session again
    return -1;
  }

  uint16_t got = 0;
  u32 left = n;

  while ( left ) {

    if ( waitData( ) < 0 ) {
      Connected = 0;
      return -1;
    }

    uint16_t avail = RxLen - RxPos;
    uint16_t chunk = avail;
    if ( left < avail ) chunk = (uint16_t)left;

    memcpy( dst + got, Rx + RxPos, chunk );
    RxPos += chunk;
    got += chunk;
    left -= chunk;
  }

  return (long)got;
}

// Make sure that the window holds the bytes around pos: at least VIEW_BACK
// before it (unless that is the start of the file) and VIEW_AHEAD after it
// (unless that is the end of the file).  A new window starts VIEW_PRE before
// pos, which leaves room to scroll in both directions without reading again.
// Returns 0 or -1.
static int viewEnsure( u32 pos ) {

  if ( VLoaded && (pos >= VBase) && (pos <= VBase + VLen) &&
       ((VBase == 0) || ((pos - VBase) >= VIEW_BACK)) &&
       (((VBase + VLen) >= VSize) || ((VBase + VLen - pos) >= VIEW_AHEAD)) ) {
    return 0;
  }

  u32 start = (pos > VIEW_PRE) ? (pos - VIEW_PRE) : 0;

  long n = viewRead( start, VIEW_WIN, VBuf );
  if ( n < 0 ) {
    VError = 1;
    return -1;
  }

  VBase = start;
  VLen = (uint16_t)n;
  VLoaded = 1;
  return 0;
}

// Start of the line (piece) that contains the byte at pos.
static u32 viewLineStart( u32 pos ) {

  u32 s0 = pos;

  while ( s0 > VBase ) {
    if ( VBuf[(uint16_t)(s0 - 1 - VBase)] == '\n' ) break;
    s0--;
  }

  return s0 + ((pos - s0) / VIEW_MAXLINE) * VIEW_MAXLINE;
}

// Start of the line after the one that starts at p.
static u32 viewNextLine( u32 p ) {

  u32 lim = p + VIEW_MAXLINE;
  if ( lim > VSize ) lim = VSize;
  if ( lim > VBase + VLen ) lim = VBase + VLen;

  while ( p < lim ) {
    if ( VBuf[(uint16_t)(p - VBase)] == '\n' ) return p + 1;
    p++;
  }

  return p;
}

// n lines back from p (p is a line start; stops at the start of the file).
static u32 viewBackLines( u32 p, int n ) {

  while ( (n > 0) && (p > 0) ) {
    if ( viewEnsure( p ) ) return p;
    p = viewLineStart( p - 1 );
    n--;
  }

  return p;
}

// n lines forward from p; stops at the last line.  *hitEnd is set if it did.
static u32 viewForwardLines( u32 p, int n, int *hitEnd ) {

  *hitEnd = 0;

  while ( n > 0 ) {
    if ( viewEnsure( p ) ) return p;
    u32 q = viewNextLine( p );
    if ( q >= VSize ) { *hitEnd = 1; break; }
    p = q;
    n--;
  }

  return p;
}

// The top line of the last full page.
static u32 viewLastPage( void ) {

  if ( VSize == 0 ) return 0;

  if ( VHex ) {
    u32 rows = (VSize + 15) / 16;
    return (rows > VIEW_ROWS) ? ((rows - VIEW_ROWS) * 16) : 0;
  }

  if ( viewEnsure( VSize ) ) return 0;
  return viewBackLines( VSize, VIEW_ROWS );
}

static const char *viewErr( void ) {
  return (ErrMsg && ErrMsg[0]) ? ErrMsg : "unknown error";
}

static void viewDrawKeyBar( void ) {

  const char *labels[10] = { "", "", "", "", "", "", "Search", "", "", "Quit" };
  labels[3] = VHex ? "Text" : "Hex";

  for ( int i = 0; i < 10; i++ ) {
    char num[4];
    sprintf( num, "%d", i + 1 );
    int x = i * 8;
    putStr( x, 24, num, A_KEYNUM );
    putStrPad( x + (int)strlen( num ), 24, labels[i], 8 - (int)strlen( num ), A_KEYTXT );
  }
}

// Draws the screen.  Returns the file offset after the last byte shown.
static u32 viewDraw( void ) {

  char head[100];
  u32 p = VPos;
  u32 shownEnd = VPos;

  uiFill( 0, 1, SCR_W, VIEW_ROWS, ' ', A_VIEWTEXT );

  for ( int row = 0; row < VIEW_ROWS; row++ ) {

    if ( p >= VSize ) break;
    if ( viewEnsure( p ) ) break;

    int y = 1 + row;

    if ( VHex ) {

      char line[100];
      int n = 16;
      if ( (VSize - p) < 16 ) n = (int)(VSize - p);

      int c = sprintf( line, "%08lX: ", (unsigned long)p );
      for ( int i = 0; i < 16; i++ ) {
        if ( i == 8 ) line[c++] = ' ';
        if ( i < n ) c += sprintf( line + c, "%02X ", (unsigned)VBuf[(uint16_t)(p - VBase) + i] );
        else         c += sprintf( line + c, "   " );
      }
      line[c++] = ' ';
      for ( int i = 0; i < n; i++ ) {
        u8 b = VBuf[(uint16_t)(p - VBase) + i];
        line[c++] = (b < 32) ? '.' : (char)b;
      }
      line[c] = 0;
      putStr( 0, y, line, A_VIEWTEXT );

      p += n;
    }
    else {

      u32 e = viewNextLine( p );
      uint16_t vx = 0;

      for ( u32 o = p; o < e; o++ ) {

        u8 b = VBuf[(uint16_t)(o - VBase)];

        if ( (b == '\n') || (b == '\r') ) continue;

        if ( b == '\t' ) {
          uint16_t stop = (uint16_t)((vx + 8) & ~7u);
          while ( vx < stop ) {
            if ( (vx >= VCol) && ((vx - VCol) < SCR_W) ) putCh( vx - VCol, y, ' ', A_VIEWTEXT );
            vx++;
          }
        }
        else {
          if ( (vx >= VCol) && ((vx - VCol) < SCR_W) ) putCh( vx - VCol, y, (b < 32) ? '.' : b, A_VIEWTEXT );
          vx++;
        }

        if ( (vx - VCol) >= SCR_W ) break;
      }

      p = e;
    }

    shownEnd = p;
  }

  // Header: name, column, size, how far we are.
  uiFill( 0, 0, SCR_W, 1, ' ', A_VIEWHEAD );

  {
    int len = (int)strlen( VPath );
    sprintf( head, "View: %s", (len > 34) ? (VPath + len - 34) : VPath );
    putStr( 0, 0, head, A_VIEWHEAD );
  }

  if ( VHex ) strcpy( head, "Hex" );
  else        sprintf( head, "Col %u", (unsigned)VCol );
  putStr( 42, 0, head, A_VIEWHEAD );

  sprintf( head, "%lu Bytes", (unsigned long)VSize );
  putStr( 56, 0, head, A_VIEWHEAD );

  sprintf( head, "%3d%%", percent( shownEnd, VSize ) );
  putStr( 76, 0, head, A_VIEWHEAD );

  viewDrawKeyBar( );

  return shownEnd;
}

// True if Esc was pressed.  Any other key stays in the keyboard buffer.
static int viewEscPressed( void ) {
  if ( _bios_keybrd( _KEYBRD_READY ) != 0x011B ) return 0;
  _bios_keybrd( _KEYBRD_READ );
  return 1;
}

// Looks for the nl bytes of needle from offset from on.  With icase the
// letters A-Z match in both cases.  Returns 1 (found, *at set), 0 (not found),
// 2 (cancelled with Esc) or -1 (error, see ErrMsg).
static int viewSearch( u32 from, const u8 *needle, uint16_t nl, int icase, u32 *at ) {

  u8 nd[40];

  if ( (nl == 0) || (nl > 38) ) return 0;

  for ( uint16_t i = 0; i < nl; i++ ) nd[i] = icase ? (u8)toupper( needle[i] ) : needle[i];

  u32 off = from;

  while ( off < VSize ) {

    char msg[40];
    sprintf( msg, "Searching %3d%%  (Esc cancels)", percent( off, VSize ) );
    putStrPad( 0, 0, msg, SCR_W, A_VIEWHEAD );

    long n = viewRead( off, VIEW_SBUF, VSbuf );
    if ( n < 0 ) return -1;
    if ( n == 0 ) break;

    for ( int i = 0; (i + nl) <= n; i++ ) {
      uint16_t k = 0;
      while ( (k < nl) && ((icase ? (u8)toupper( VSbuf[i + k] ) : VSbuf[i + k]) == nd[k]) ) k++;
      if ( k == nl ) {
        *at = off + (u32)i;
        return 1;
      }
    }

    if ( (off + (u32)n) >= VSize ) break;

    off += (u32)n - (u32)(nl - 1);

    if ( viewEscPressed( ) ) return 2;
  }

  return 0;
}

// Turns what was typed into the bytes to look for.  In text mode that is the
// text itself.  In hex mode it is hexadecimal bytes ("6E 50" or "6e50"), or a
// text in double quotes ("needle").  Returns the length, or -1 if it makes no sense.
static int viewParseNeedle( const char *in, int hexMode, u8 *out, int *icase ) {

  int n = 0;
  int len = (int)strlen( in );

  *icase = 1;

  if ( hexMode && (in[0] == '"') ) {
    in++;
    len--;
    if ( (len > 0) && (in[len-1] == '"') ) len--;
  }
  else if ( hexMode ) {

    int half = -1;

    *icase = 0;

    for ( int i = 0; i < len; i++ ) {

      char c = in[i];
      int v;

      if ( c == ' ' ) {
        if ( half >= 0 ) return -1;           // a space inside a byte
        continue;
      }

      if ( (c >= '0') && (c <= '9') )      v = c - '0';
      else if ( (c >= 'a') && (c <= 'f') ) v = c - 'a' + 10;
      else if ( (c >= 'A') && (c <= 'F') ) v = c - 'A' + 10;
      else return -1;

      if ( half < 0 ) {
        half = v;
      }
      else {
        if ( n >= 38 ) return -1;
        out[n++] = (u8)(half * 16 + v);
        half = -1;
      }
    }

    if ( half >= 0 ) return -1;               // odd number of digits
    return n;
  }

  if ( len > 38 ) return -1;
  memcpy( out, in, len );
  return len;
}

static void viewFindText( void ) {

  char text[40];
  u8 needle[40];
  int icase;

  strcpy( text, VSearch );

  if ( !inputBox( "Search", VHex ? "Hex bytes (6E 50) or \"text\":" : "Search for:", text, 38 ) ) return;

  int nl = viewParseNeedle( text, VHex, needle, &icase );

  if ( nl <= 0 ) {
    uiMessage( "Search", "Type hex bytes in pairs (6E 50)", "or a text in double quotes." );
    return;
  }

  u32 from = VPos;
  if ( VFound && (strcmp( text, VSearch ) == 0) ) from = VNext;
  strcpy( VSearch, text );

  u32 at = 0;
  int rc = viewSearch( from, needle, (uint16_t)nl, icase, &at );

  if ( rc == 1 ) {
    VFound = 1;
    VNext = at + 1;
    if ( VHex ) {
      VPos = at & ~15UL;
    }
    else if ( viewEnsure( at ) == 0 ) {
      VPos = viewLineStart( at );
    }
  }
  else if ( rc == 0 ) {
    VFound = 0;
    uiMessage( "Search", "Not found.", NULL );
  }
  else if ( rc == -1 ) {
    VFound = 0;
    uiMessage( "Search", "Reading the file failed:", viewErr( ) );
  }
}

// One key in the viewer.  Returns 0 when the viewer should close.
static int viewKey( int k ) {

  switch ( k ) {

    case K_F3:
    case K_F10:
    case 27:
      return 0;

    case K_UP:
      if ( VPos > 0 ) {
        if ( VHex ) VPos = (VPos >= 16) ? (VPos - 16) : 0;
        else if ( viewEnsure( VPos ) == 0 ) VPos = viewLineStart( VPos - 1 );
      }
      break;

    case K_DOWN:
      if ( VHex ) {
        if ( (VPos + 16) < VSize ) VPos += 16;
      }
      else if ( viewEnsure( VPos ) == 0 ) {
        u32 q = viewNextLine( VPos );
        if ( q < VSize ) VPos = q;
      }
      break;

    case K_PGUP:
      if ( VHex ) VPos = (VPos >= 16UL * VIEW_ROWS) ? (VPos - 16UL * VIEW_ROWS) : 0;
      else VPos = viewBackLines( VPos, VIEW_ROWS );
      break;

    case K_PGDN:
      if ( VHex ) {
        u32 last = viewLastPage( );
        VPos += 16UL * VIEW_ROWS;
        if ( VPos > last ) VPos = last;
      }
      else {
        int hit, hit2 = 0;
        u32 q = viewForwardLines( VPos, VIEW_ROWS, &hit );

        // Is the page at q full?  A line is at most VIEW_MAXLINE bytes, so
        // that much text after q holds enough lines without looking.
        if ( !hit && ((VSize - q) <= (u32)(VIEW_ROWS - 1) * VIEW_MAXLINE) ) {
          viewForwardLines( q, VIEW_ROWS - 1, &hit2 );
        }

        if ( hit || hit2 ) {
          u32 last = viewLastPage( );
          if ( last > VPos ) VPos = last;
        }
        else {
          VPos = q;
        }
      }
      break;

    case K_HOME:
      VPos = 0;
      break;

    case K_END:
      VPos = viewLastPage( );
      break;

    case K_LEFT:
      if ( !VHex ) VCol = (VCol >= 8) ? (VCol - 8) : 0;
      break;

    case K_RIGHT:
      if ( !VHex && (VCol < 480) ) VCol += 8;
      break;

    case K_F4:
      if ( VHex ) {
        VHex = 0;
        if ( viewEnsure( VPos ) == 0 ) VPos = viewLineStart( VPos );
      }
      else {
        VHex = 1;
        VPos &= ~15UL;
      }
      VCol = 0;
      break;

    case K_F7:
      viewFindText( );
      break;

    default:
      break;
  }

  return 1;
}

// Opens a file for viewing.  Returns 0 or -1 (the user has been told why).
static int viewOpen( int remote, const char *path, u32 size ) {

  strcpy( VPath, path );

  VRemote = remote;
  VSize = size;
  VFile = NULL;
  VLoaded = 0;
  VBase = 0;
  VLen = 0;
  VPos = 0;
  VHex = 0;
  VCol = 0;
  VFound = 0;
  VError = 0;

  VBuf = (u8 *)malloc( VIEW_WIN );
  VSbuf = (u8 *)malloc( VIEW_SBUF );

  if ( !VBuf || !VSbuf ) {
    uiMessage( "View", "Not enough memory.", NULL );
    free( VBuf );
    free( VSbuf );
    VBuf = VSbuf = NULL;
    return -1;
  }

  if ( !VRemote ) {
    VFile = fopen( VPath, "rb" );
    if ( VFile == NULL ) {
      uiMessage( "View", "Cannot open the file.", NULL );
      free( VBuf );
      free( VSbuf );
      VBuf = VSbuf = NULL;
      return -1;
    }
  }

  if ( (VSize > 0) && viewEnsure( 0 ) ) {
    uiMessage( "View", "Cannot read the file:", viewErr( ) );
    if ( VFile ) fclose( VFile );
    VFile = NULL;
    free( VBuf );
    free( VSbuf );
    VBuf = VSbuf = NULL;
    return -1;
  }

  return 0;
}

static void viewClose( void ) {

  if ( VFile ) fclose( VFile );
  VFile = NULL;
  free( VBuf );
  free( VSbuf );
  VBuf = NULL;
  VSbuf = NULL;
}

static void cmdView( void ) {

  Panel *p = Active ? &Right : &Left;

  if ( p->count == 0 ) return;

  Item *it = &p->items[p->cur];

  if ( it->isDir ) return;

  char path[300];
  joinPath( path, p->path, it->name );

  if ( strlen( path ) > 230 ) {
    uiMessage( "View", "The path is too long.", NULL );
    return;
  }

  if ( viewOpen( p->remote, path, it->size ) ) return;

  while ( 1 ) {
    viewDraw( );
    if ( VError ) {
      uiMessage( "View", "Reading the file failed:", viewErr( ) );
      break;
    }
    if ( !viewKey( uiReadKey( ) ) ) break;
    if ( VError ) {
      uiMessage( "View", "Reading the file failed:", viewErr( ) );
      break;
    }
  }

  viewClose( );
}

// ---------------------------------------------------------------------
// Commands

static const char *baseOf( const char *rel ) {
  const char *p = rel;
  const char *last = rel;
  while ( *p ) {
    if ( *p == '\\' ) last = p + 1;
    p++;
  }
  return last;
}

static void cmdDownload( void ) {

  if ( Active != 0 ) {
    uiMessage( "Download", "Select files in the left (host) panel,", "then press F5." );
    return;
  }

  if ( buildList( &Left ) ) return;

  if ( TreeTooBig ) {
    uiMessage( "Download", "That is too many files for one go.", "Select fewer folders at a time." );
    return;
  }

  if ( TreeEntries == 0 ) {
    uiMessage( "Download", "Nothing to download.", NULL );
    return;
  }

  // Confirm.
  {
    char l1[80], l2[80], l3[80], l4[80];
    const char *lines[4];
    int nl = 0;

    sprintf( l1, "%lu file(s) in %lu folder(s), %lu bytes", TreeFiles, TreeDirs, (unsigned long)TreeBytes );
    sprintf( l2, "to %.50s ?", Right.path );
    sprintf( l3, "CRC32 check: %s   (change with F9)", Verify ? "on" : "off" );

    lines[nl++] = l1;
    lines[nl++] = l2;
    lines[nl++] = l3;

    if ( TreeBad ) {
      sprintf( l4, "%lu entries will be skipped (name or path too long)", TreeBad );
      lines[nl++] = l4;
    }

    if ( !uiDialog( "Download", lines, nl, 1 ) ) return;
  }

  // Transfer.
  ProgTotal = TreeBytes;
  ProgDone = 0;
  ProgFileDone = 0;
  ProgFileSize = 0;
  ProgFileNo = 0;
  ProgFileCount = TreeFiles;
  ProgCancel = 0;
  ProgPhase = 0;
  ProgName[0] = 0;
  ProgStart = uiTicks( );
  ProgLast = 0;
  OverwriteMode = 0;
  LastError[0] = 0;

  unsigned long okFiles = 0, skipped = 0, failed = 0, badCrc = 0;
  u32 bytesCopied = 0;
  int stop = 0;                       // 1 cancelled, 2 connection lost, 3 disk problem

  progressFrame( );
  progressUpdate( 1 );

  uint16_t pos = 0;
  char local[300];
  char remote[300];

  while ( (pos < TreeUsed) && !stop ) {

    char type = TreeBuf[pos];
    u32 sz;
    u16 fdate, ftime;
    memcpy( &sz, TreeBuf + pos + 1, 4 );
    memcpy( &fdate, TreeBuf + pos + 5, 2 );
    memcpy( &ftime, TreeBuf + pos + 7, 2 );
    const char *rel = (const char *)(TreeBuf + pos + ENTRY_HEAD);
    pos += ENTRY_HEAD + (uint16_t)strlen( rel ) + 1;

    joinPath( local, Right.path, rel );

    if ( type == 'D' ) {
      makeDirs( local );
      continue;
    }

    ProgFileNo++;
    ProgFileDone = 0;
    ProgFileSize = sz;
    strncpy( ProgName, baseOf( rel ), 12 );
    ProgName[12] = 0;
    progressUpdate( 1 );

    if ( ProgCancel ) { stop = 1; break; }

    u32 oldSize;

    if ( localInfo( local, &oldSize ) ) {

      int act = 'Y';

      if ( OverwriteMode == 2 ) {
        act = 'N';
      }
      else if ( OverwriteMode == 0 ) {
        act = askOverwrite( rel, sz, oldSize );
        progressFrame( );
        progressUpdate( 1 );
      }

      if ( act == 'C' ) { stop = 1; break; }
      if ( act == 'A' ) OverwriteMode = 1;
      if ( act == 'S' ) { OverwriteMode = 2; act = 'N'; }

      if ( act == 'N' ) {
        skipped++;
        ProgDone += sz;
        continue;
      }
    }

    joinPath( remote, Left.path, rel );

    u32 got = 0;
    int r = downloadFile( remote, local, &got, fdate, ftime );

    if ( r == FETCH_OK ) {
      okFiles++;
      bytesCopied += got;
    }
    else {

      sprintf( LastError, "%.30s: %.60s", baseOf( rel ), ErrMsg );

      if      ( r == FETCH_CANCEL ) stop = 1;
      else if ( r == FETCH_FATAL )  stop = 2;
      else if ( r == FETCH_DISK )   stop = 3;
      else if ( r == FETCH_BADCRC ) { badCrc++; okFiles++; bytesCopied += got; }
      else                          failed++;
    }
  }

  // Summary.
  {
    u32 ms = (u32)(uiTicks( ) - ProgStart) * 55;

    char s1[80], s2[80], s3[80], s4[120], s5[120];
    const char *lines[6];
    int nl = 0;

    sprintf( s1, "Copied: %lu   Skipped: %lu   Failed: %lu", okFiles, skipped, failed );
    lines[nl++] = s1;

    sprintf( s2, "%lu bytes in %lu.%01lu s", (unsigned long)bytesCopied,
             (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100) );
    lines[nl++] = s2;

    if ( ms > 0 ) {
      sprintf( s3, "about %lu KB/s", (unsigned long)(((bytesCopied >> 10) * 1000UL) / ms) );
      lines[nl++] = s3;
    }

    if ( badCrc ) {
      sprintf( s4, "CRC32 PROBLEMS: %lu file(s) may be damaged", badCrc );
      lines[nl++] = s4;
    }

    if ( stop >= 2 ) {
      sprintf( s5, "%s: %.70s", (stop == 2) ? "Connection lost" : "Disk problem", ErrMsg );
      lines[nl++] = s5;
    }
    else if ( LastError[0] && (failed || badCrc) ) {
      sprintf( s5, "Last problem: %.80s", LastError );
      lines[nl++] = s5;
    }

    uiDialog( (stop == 0) ? "Done" : (stop == 1) ? "Cancelled" : "Stopped", lines, nl, 0 );
  }

  selectAll( &Left, 0 );
  reload( &Right );
}

// ---------------------------------------------------------------------
// Deleting (only in the local panel: the host is read-only)

#define ALL_ATTRS      ( _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH | _A_SUBDIR )
#define DEL_MAX_DEPTH  20

static unsigned long DelFiles, DelDirs, DelFailed;
static char          DelLast[40];
static int           DelX, DelY;

// DOS refuses to delete read-only files, so clear the attributes first.
static void clearAttr( const char *path ) {
  _dos_setfileattr( path, _A_NORMAL );
}

static void delShow( const char *name ) {
  char buf[60];
  sprintf( buf, "Deleting %.40s", name );
  putStrPad( DelX + 3, DelY + 2, buf, 44, A_DLG );
}

static void delFailure( const char *name ) {
  DelFailed++;
  strncpy( DelLast, name, sizeof( DelLast ) - 1 );
  DelLast[sizeof( DelLast ) - 1] = 0;
}

// Counts what is below a folder (used for the question before deleting).
static void countTree( const char *dir, unsigned long *files, unsigned long *dirs, int depth ) {

  struct find_t fb;
  char pattern[100];
  char child[100];

  if ( (depth > DEL_MAX_DEPTH) || (strlen( dir ) > 70) ) return;

  joinPath( pattern, dir, "*.*" );

  unsigned rc = _dos_findfirst( pattern, ALL_ATTRS, &fb );

  while ( rc == 0 ) {

    if ( strcmp( fb.name, "." ) && strcmp( fb.name, ".." ) ) {

      if ( fb.attrib & _A_SUBDIR ) {
        (*dirs)++;
        joinPath( child, dir, fb.name );
        countTree( child, files, dirs, depth + 1 );
      }
      else {
        (*files)++;
      }
    }

    rc = _dos_findnext( &fb );
  }
}

// Deletes a folder and everything in it.  Returns 1 if the folder is gone.
// On the first thing that cannot be deleted it gives up on that folder
// (and so on the ones above it) instead of looping.
static int deleteTree( const char *dir, int depth ) {

  struct find_t fb;
  char pattern[100];
  char child[100];

  if ( (depth > DEL_MAX_DEPTH) || (strlen( dir ) > 70) ) {
    delFailure( "path too deep" );
    return 0;
  }

  joinPath( pattern, dir, "*.*" );

  while ( 1 ) {

    // Always start the search again: deleting while walking through a
    // directory is not reliable in DOS.
    unsigned rc = _dos_findfirst( pattern, ALL_ATTRS, &fb );

    while ( (rc == 0) && (!strcmp( fb.name, "." ) || !strcmp( fb.name, ".." )) ) {
      rc = _dos_findnext( &fb );
    }

    if ( rc != 0 ) break;                 // nothing left in this folder

    joinPath( child, dir, fb.name );
    delShow( fb.name );

    if ( fb.attrib & _A_SUBDIR ) {
      if ( !deleteTree( child, depth + 1 ) ) return 0;
    }
    else {
      clearAttr( child );

      if ( remove( child ) != 0 ) {
        delFailure( fb.name );
        return 0;
      }

      DelFiles++;
    }
  }

  clearAttr( dir );

  if ( rmdir( dir ) != 0 ) {
    delFailure( dir );
    return 0;
  }

  DelDirs++;
  return 1;
}

static void cmdDelete( void ) {

  if ( Active != 1 ) {
    uiMessage( "Delete", "The host is read-only.", "Files can only be deleted in the right (local) panel." );
    return;
  }

  Panel *p = &Right;
  u32 bytes;
  int sel = countSelected( p, &bytes );
  int i;

  unsigned long files = 0, dirs = 0, subFiles = 0, subDirs = 0;
  char path[100];

  for ( i = 0; i < p->count; i++ ) {

    Item *it = &p->items[i];

    if ( strcmp( it->name, ".." ) == 0 ) continue;
    if ( (sel > 0) ? !it->sel : (i != p->cur) ) continue;

    if ( it->isDir ) {
      dirs++;
      joinPath( path, p->path, it->name );
      countTree( path, &subFiles, &subDirs, 1 );
    }
    else {
      files++;
    }
  }

  if ( (files + dirs) == 0 ) {
    uiMessage( "Delete", "Nothing to delete.", NULL );
    return;
  }

  // Ask.  Only the Y key confirms, so a stray Enter cannot delete anything.
  {
    char l1[80], l2[80], l3[80], l4[80];
    const char *lines[4];
    int nl = 0;

    sprintf( l1, "Delete %lu file(s) and %lu folder(s)?", files, dirs );
    lines[nl++] = l1;

    if ( dirs ) {
      strcpy( l2, "Folders are deleted WITH their contents:" );
      sprintf( l3, "%lu file(s) and %lu subfolder(s) inside.", subFiles, subDirs );
      lines[nl++] = l2;
      lines[nl++] = l3;
    }

    sprintf( l4, "In %.60s", p->path );
    lines[nl++] = l4;

    if ( !uiDialog( "Delete", lines, nl, 2 ) ) return;
  }

  // Do it.
  DelFiles = DelDirs = DelFailed = 0;
  DelLast[0] = 0;

  {
    int w = 50, h = 5;
    DelX = (SCR_W - w) / 2;
    DelY = (25 - h) / 2;
    dialogFrame( DelX, DelY, w, h, "Deleting" );
  }

  for ( i = 0; i < p->count; i++ ) {

    Item *it = &p->items[i];

    if ( strcmp( it->name, ".." ) == 0 ) continue;
    if ( (sel > 0) ? !it->sel : (i != p->cur) ) continue;

    joinPath( path, p->path, it->name );

    if ( it->isDir ) {
      deleteTree( path, 1 );
    }
    else {
      delShow( it->name );
      clearAttr( path );

      if ( remove( path ) == 0 ) DelFiles++;
      else                       delFailure( it->name );
    }
  }

  int oldCur = p->cur;
  reload( p );
  p->cur = oldCur;
  fixView( p );

  if ( DelFailed ) {
    char l1[80], l2[80];
    sprintf( l1, "Could not delete everything (%lu problem(s)).", DelFailed );
    sprintf( l2, "Last: %.30s - read-only disk, in use or protected?", DelLast );
    uiMessage( "Delete", l1, l2 );
  }
}

// ---------------------------------------------------------------------
// Drives (right panel)

// DOS would ask "Abort, Retry, Fail?" on an empty floppy and wreck the
// screen.  Always answer Fail; the caller sees an ordinary error.
static int __far criticalHandler( unsigned deverr, unsigned errcode, unsigned __far *devhdr ) {
  return _HARDERR_FAIL;
}

// Finds the drives that exist.  Returns how many, letters are in the array.
static int listDrives( char *letters ) {

  unsigned orig, cur, total, dummy;
  int n = 0;

  _dos_getdrive( &orig );
  _dos_setdrive( orig, &total );          // learn how many drives there are

  if ( total > 26 ) total = 26;

  for ( unsigned d = 1; d <= total; d++ ) {
    _dos_setdrive( d, &dummy );
    _dos_getdrive( &cur );
    if ( cur == d ) letters[n++] = (char)('A' + d - 1);
  }

  _dos_setdrive( orig, &dummy );

  letters[n] = 0;
  return n;
}

// Lets the user pick a drive.  Returns the letter, or 0 for Esc.
static int chooseDrive( char current ) {

  char letters[28];
  int n = listDrives( letters );

  if ( n == 0 ) return 0;

  int sel = 0;
  int i;

  for ( i = 0; i < n; i++ ) {
    if ( letters[i] == current ) sel = i;
  }

  const int perRow = 16;
  int rows = (n + perRow - 1) / perRow;
  int cols = (n < perRow) ? n : perRow;
  int w = cols * 4 + 8;
  if ( w < 44 ) w = 44;
  int h = rows + 5;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;

  const char *hint = "Arrows + Enter, or press a drive letter";

  while ( 1 ) {

    dialogFrame( x, y, w, h, "Drive" );

    for ( i = 0; i < n; i++ ) {
      char cell[8];
      sprintf( cell, " %c: ", letters[i] );
      putStr( x + 4 + (i % perRow) * 4, y + 2 + (i / perRow), cell, (i == sel) ? A_CURSOR : A_DLG );
    }

    putStr( x + (w - (int)strlen( hint )) / 2, y + 3 + rows, hint, A_DLGHINT );

    int k = uiReadKey( );

    if ( k == K_LEFT )  sel = (sel + n - 1) % n;
    if ( k == K_RIGHT ) sel = (sel + 1) % n;
    if ( k == 13 )      return letters[sel];
    if ( k == 27 )      return 0;

    if ( (k > 32) && (k < 127) ) {
      int c = toupper( k );
      for ( i = 0; i < n; i++ ) {
        if ( letters[i] == c ) return c;
      }
    }
  }
}

static void cmdDrive( void ) {

  int letter = chooseDrive( Right.path[0] );
  if ( letter == 0 ) return;

  char newPath[80];

  if ( _getdcwd( letter - 'A' + 1, newPath, sizeof( newPath ) ) == NULL ) {
    sprintf( newPath, "%c:\\", letter );
  }

  char oldPath[80];
  strcpy( oldPath, Right.path );
  int oldCur = Right.cur;
  int oldTop = Right.top;

  strcpy( Right.path, newPath );
  Right.cur = 0;
  Right.top = 0;

  if ( !reload( &Right ) ) {
    strcpy( Right.path, oldPath );
    Right.cur = oldCur;
    Right.top = oldTop;
    loadLocal( &Right );
    fixView( &Right );
    uiMessage( "Drive", "That drive is not ready.", "(no disk in the drive?)" );
  }

  Active = 1;
}

// ---------------------------------------------------------------------
// Choosing another host (Alt+F1)

// ---------------------------------------------------------------------
// Finding hosts on the local network.
//
// The client sends a UDP broadcast "ECDISC 1"; every host answers the sender
// with "ECHOST 1 <tcp port> <name>".  The packet handling follows mTCP's
// DHCP client: a packet structure that starts with the Ethernet/IP/UDP
// headers is sent to IpBroadcastNonRoutable, and the receive handler gets the
// whole packet and must hand it back with Buffer_free.

#define DISCOVERY_PORT  6502          // UDP port the hosts listen on
#define MAX_FOUND       8

typedef struct {
  IpAddr_t ip;
  uint16_t port;
  char     name[28];
} FoundHost;

typedef struct {
  UdpPacket_t hdr;
  uint8_t     data[16];
} DiscoverPacket_t;

static FoundHost Found[MAX_FOUND];
static int       FoundCount = 0;

// Handles the text of one answer.  Anything that is not a valid answer is ignored.
static void parseHostReply( char *text, const uint8_t *ipSrc ) {

  if ( strncmp( text, "ECHOST 1 ", 9 ) != 0 ) return;

  char *endp;
  unsigned long port = strtoul( text + 9, &endp, 10 );

  if ( (port == 0) || (port > 65535UL) || (*endp != ' ') ) return;

  char *name = endp + 1;

  for ( char *q = name; *q; q++ ) {
    if ( (*q == '\r') || (*q == '\n') ) { *q = 0; break; }
  }

  int i;

  // The search is sent several times, so the same host answers several times.
  for ( i = 0; i < FoundCount; i++ ) {
    if ( (memcmp( Found[i].ip, ipSrc, 4 ) == 0) && (Found[i].port == (uint16_t)port) ) return;
  }

  if ( FoundCount >= MAX_FOUND ) return;

  FoundHost *h = &Found[FoundCount++];

  memcpy( h->ip, ipSrc, 4 );
  h->port = (uint16_t)port;

  strncpy( h->name, name, sizeof( h->name ) - 1 );
  h->name[sizeof( h->name ) - 1] = 0;

  // Only printable characters go on the screen.
  for ( i = 0; h->name[i]; i++ ) {
    if ( ((u8)h->name[i] < 32) || ((u8)h->name[i] > 126) ) h->name[i] = '?';
  }
}

// All UDP packets for our port come through here.
static void discoveryHandler( const unsigned char *packet, const UdpHeader *udp ) {

  const IpHeader *ip = (const IpHeader *)(packet + sizeof( EthHeader ));
  uint16_t len = ntohs( udp->len );

  if ( len > 8 ) {

    char text[100];

    len -= 8;                                   // the UDP header itself
    if ( len > (sizeof( text ) - 1) ) len = sizeof( text ) - 1;

    memcpy( text, ((const unsigned char *)udp) + 8, len );
    text[len] = 0;

    parseHostReply( text, ip->ip_src );
  }

  // Always give the packet back, whatever it was.
  Buffer_free( packet );
}

// Sends the search three times and collects the answers for about two
// seconds.  Esc stops waiting.  Returns the number of hosts in Found[],
// or -1 if UDP could not be used.
static int findHosts( void ) {

  DiscoverPacket_t pkt;

  FoundCount = 0;
  CtrlBreakDetected = 0;

  uint16_t port = Udp::getUnusedCallbackPort( );
  if ( port == 0 ) port = 6503;

  if ( Udp::registerCallback( port, &discoveryHandler ) ) {
    ErrMsg = "UDP is not available";
    return -1;
  }

  memcpy( pkt.data, "ECDISC 1\r\n", 10 );

  int stop = 0;

  for ( int attempt = 0; (attempt < 3) && !stop; attempt++ ) {

    if ( Udp::sendUdp( IpBroadcastNonRoutable, port, DISCOVERY_PORT, 10, (unsigned char *)&pkt, 1 ) < 0 ) {
      ErrMsg = "could not send the search";
      break;
    }

    clockTicks_t start = TIMER_GET_CURRENT( );

    while ( Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 700 ) ) {

      Utils::processPackets( );

      if ( CtrlBreakDetected ) { stop = 1; break; }

      if ( kbhit( ) ) {
        int k = getch( );
        if ( (k == 0) || (k == 0xE0) ) getch( );     // an extended key: skip its second code
        else if ( k == 27 )            { stop = 1; break; }
      }
    }
  }

  Udp::unregisterCallback( port );

  return FoundCount;
}

// Makes Found[i] the host to connect to.
static void applyFound( const FoundHost *h ) {
  memcpy( ServerAddr, h->ip, 4 );
  ServerPort = h->port;
  sprintf( HostName, "%d.%d.%d.%d", h->ip[0], h->ip[1], h->ip[2], h->ip[3] );
}

// At program start, before the screen is switched: no host was given and
// none is saved.  Returns 1 if a host was found and chosen (name, address and
// port are set), 0 if the user typed an address (to be resolved), -1 if
// nothing was entered.
static int searchAtStart( void ) {

  printf( "Looking for hosts on the network (Esc skips) ...\n" );

  int n = findHosts( );
  int i;

  if ( n == 1 ) {
    printf( "Found: %s  %d.%d.%d.%d\n", Found[0].name,
            Found[0].ip[0], Found[0].ip[1], Found[0].ip[2], Found[0].ip[3] );
    applyFound( &Found[0] );
    return 1;
  }

  if ( n > 1 ) {

    printf( "\n" );
    for ( i = 0; i < n; i++ ) {
      printf( "  %d  %-24s %d.%d.%d.%d\n", i + 1, Found[i].name,
              Found[i].ip[0], Found[i].ip[1], Found[i].ip[2], Found[i].ip[3] );
    }

    while ( 1 ) {

      char line[20];

      printf( "\nNumber of the host (1-%d, or just Enter to type an address): ", n );
      if ( fgets( line, sizeof( line ), stdin ) == NULL ) return -1;

      if ( (line[0] == '\n') || (line[0] == '\r') || (line[0] == 0) ) break;

      int pick = atoi( line );
      if ( (pick >= 1) && (pick <= n) ) {
        applyFound( &Found[pick - 1] );
        return 1;
      }
    }
  }
  else {
    printf( "No host answered.\n" );
  }

  printf( "Address of the host (for example 192.168.1.10): " );
  if ( fgets( HostName, sizeof( HostName ), stdin ) == NULL ) return -1;

  int l = (int)strlen( HostName );
  while ( (l > 0) && ((HostName[l-1] == '\n') || (HostName[l-1] == '\r') || (HostName[l-1] == ' ')) ) {
    HostName[--l] = 0;
  }

  return (l == 0) ? -1 : 0;
}

static int resolveHost( const char *name, uint8_t *addrOut ) {

  IpAddr_t addr;

  CtrlBreakDetected = 0;

  int8_t rc = Dns::resolve( name, addr, 1 );
  if ( rc < 0 ) return 0;

  while ( 1 ) {
    if ( CtrlBreakDetected ) break;
    if ( !Dns::isQueryPending( ) ) break;
    Utils::processPackets( );
    Dns::drivePendingQuery( );
  }

  rc = Dns::resolve( name, addr, 0 );
  if ( rc != 0 ) return 0;

  memcpy( addrOut, addr, 4 );
  return 1;
}

// Switches the left panel to another host.  If it cannot be read, everything
// stays as it was.
static void switchToHost( const char *name, const uint8_t *addr, uint16_t port ) {

  IpAddr_t oldAddr;
  memcpy( oldAddr, ServerAddr, 4 );
  uint16_t oldPort = ServerPort;
  char oldName[80];
  char oldPath[80];
  strcpy( oldName, HostName );
  strcpy( oldPath, Left.path );

  memcpy( ServerAddr, addr, 4 );
  ServerPort = port;
  strcpy( HostName, name );
  strcpy( Left.path, "\\" );
  Connected = 0;

  if ( !reload( &Left ) ) {
    memcpy( ServerAddr, oldAddr, 4 );
    ServerPort = oldPort;
    strcpy( HostName, oldName );
    strcpy( Left.path, oldPath );
    Connected = 0;                        // the old host is reached again on the next action
    return;
  }

  Left.cur = 0;
  Left.top = 0;
  fixView( &Left );
  Active = 0;

  saveSettings( );
}

// Shows the hosts that answered and lets the user pick one.  Returns the
// index in Found[], n for "type an address", or -1 for Esc.
static int chooseFound( int n ) {

  int sel = 0;
  int w = 56;
  int h = n + 6;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;
  const char *hint = "Up/Down + Enter, Esc = cancel";

  while ( 1 ) {

    dialogFrame( x, y, w, h, "Hosts found" );

    for ( int i = 0; i <= n; i++ ) {

      char line[70];

      if ( i < n ) {
        sprintf( line, " %-24.24s  %d.%d.%d.%d", Found[i].name,
                 Found[i].ip[0], Found[i].ip[1], Found[i].ip[2], Found[i].ip[3] );
      }
      else {
        strcpy( line, " (type an address ...)" );
      }

      putStrPad( x + 3, y + 2 + i, line, w - 6, (i == sel) ? A_CURSOR : A_DLG );
    }

    putStr( x + (w - (int)strlen( hint )) / 2, y + 4 + n, hint, A_DLGHINT );

    int k = uiReadKey( );

    if ( k == K_UP )   sel = (sel + n) % (n + 1);
    if ( k == K_DOWN ) sel = (sel + 1) % (n + 1);
    if ( k == 13 )     return sel;
    if ( k == 27 )     return -1;
  }
}

static void cmdChooseHost( void ) {

  // First look for hosts on the network.
  {
    int w = 44, h = 5;
    int x = (SCR_W - w) / 2;
    int y = (25 - h) / 2;
    dialogFrame( x, y, w, h, "Searching" );
    putStrPad( x + 3, y + 2, "Looking for hosts ... (Esc stops)", w - 6, A_DLG );
  }

  int n = findHosts( );

  if ( n > 0 ) {

    int pick = chooseFound( n );

    if ( pick < 0 ) return;

    if ( pick < n ) {
      char name[20];
      sprintf( name, "%d.%d.%d.%d", Found[pick].ip[0], Found[pick].ip[1], Found[pick].ip[2], Found[pick].ip[3] );
      switchToHost( name, Found[pick].ip, Found[pick].port );
      return;
    }
  }
  else {
    uiMessage( "Hosts", (n < 0) ? "The search could not be sent." : "No host answered.", "You can type an address instead." );
  }

  // Nothing found (or the user wants to type): ask for an address.
  char buf[60];

  strncpy( buf, HostName, 36 );
  buf[36] = 0;

  if ( !inputBox( "Connect to host", "Host name or IP address[:port]:", buf, 36 ) ) return;

  uint16_t port = ServerPort;
  char *colon = strchr( buf, ':' );

  if ( colon ) {
    *colon = 0;
    port = atoi( colon + 1 );
    if ( port == 0 ) {
      uiMessage( "Connect", "That is not a valid port number.", NULL );
      return;
    }
  }

  {
    int w = 40, h = 5;
    int x = (SCR_W - w) / 2;
    int y = (25 - h) / 2;
    dialogFrame( x, y, w, h, "Connecting" );
    putStrPad( x + 3, y + 2, "Looking for the host ...", w - 6, A_DLG );
  }

  IpAddr_t addr;

  if ( !resolveHost( buf, addr ) ) {
    uiMessage( "Connect", "Cannot find that host:", buf );
    return;
  }

  switchToHost( buf, addr, port );
}

static void cmdMakeDir( void ) {

  if ( Active != 1 ) {
    uiMessage( "Make folder", "Folders can only be made in the", "right (local) panel." );
    return;
  }

  char name[16];
  name[0] = 0;

  if ( !inputBox( "Make folder", "Name of the new folder:", name, 12 ) ) return;

  char full[100];
  strcpy( full, Right.path );
  pathDown( full, name );

  if ( mkdir( full ) != 0 ) {
    uiMessage( "Make folder", "Could not create that folder.", NULL );
    return;
  }

  reload( &Right );

  for ( int i = 0; i < Right.count; i++ ) {
    if ( stricmp( Right.items[i].name, name ) == 0 ) { Right.cur = i; break; }
  }
  fixView( &Right );
}

static void cmdReconnect( void ) {

  const char *lines[2];
  lines[0] = "Reconnect to the host and";
  lines[1] = "read the left panel again?";

  if ( uiDialog( "Reconnect", lines, 2, 1 ) ) {
    Connected = 0;
    reload( &Left );
  }
}

// Free conventional memory in KB: the largest block DOS could still give
// us right now (DOS function 48h asked for too much returns that size).
// A request for 1 MB can never succeed in conventional memory, so the
// "success" case does not happen and nothing has to be given back.
static unsigned freeMemKB( void ) {

  union REGS r;

  r.h.ah = 0x48;
  r.x.bx = 0xFFFF;
  r.x.cflag = 0;
  int86( 0x21, &r, &r );

  if ( r.x.cflag ) return r.x.bx / 64;      // paragraphs of 16 bytes -> KB

  return 0;
}

static void cmdHelp( void ) {

  const char *lines[15];
  char memLine[80];

  lines[0]  = "Up/Down/PgUp/PgDn/Home/End   move the cursor";
  lines[1]  = "Enter       open a folder (.. goes up)";
  lines[2]  = "Tab         switch panel";
  lines[3]  = "Insert      select / unselect and move down";
  lines[4]  = "+  -  *     select / unselect by mask, invert";
  lines[5]  = "F5          download selected items to the right panel";
  lines[6]  = "F7          make a folder (right panel)";
  lines[7]  = "F8          delete (right panel only, asks first)";
  lines[8]  = "F9          options (CRC32 check of every download)";
  lines[9]  = "F2 reconnect   Alt+F1 find/choose host   Alt+F2 drive";
  lines[10] = "F3          view a file (F4 hex/text, F7 search, Esc close)";
  lines[11] = "F10 or Esc  quit";
  lines[12] = "Esc during a download cancels it.";
  lines[13] = "Ctrl+O      look at the DOS screen (any key returns)";

  sprintf( memLine, "Free memory now: %u KB (compare with MEM before EC)", freeMemKB( ) );
  lines[14] = memLine;

  uiDialog( "Easy Commander - help", lines, 15, 0 );
}

// Ctrl+O: show the DOS screen (with the start text) until a key is pressed.
static void cmdDosScreen( void ) {

  if ( !SavedScreen ) return;

  union REGS r;

  for ( int i = 0; i < SCR_W * SCR_H; i++ ) Vid[i] = SavedScreen[i];
  dosCursor( );

  uiReadKey( );                    // the connection is kept alive meanwhile

  r.h.ah = 0x01;                   // hide the cursor again; the caller redraws
  r.x.cx = 0x2000;
  int86( 0x10, &r, &r );
}

// DOS style wildcards: * (any number of characters) and ? (one character),
// no difference between upper and lower case.
static int wildPart( const char *m, const char *s ) {
  while ( *m ) {
    if ( *m == '*' ) {
      m++;
      if ( !*m ) return 1;
      while ( 1 ) {
        if ( wildPart( m, s ) ) return 1;
        if ( !*s ) return 0;
        s++;
      }
    }
    if ( !*s ) return 0;
    if ( (*m != '?') && (toupper( (unsigned char)*m ) != toupper( (unsigned char)*s )) ) return 0;
    m++;
    s++;
  }
  return *s == 0;
}

// The name and the extension are compared separately, as DOS does: "*.*"
// also matches a name without an extension, "READ*" means "READ*.*".
static int wildMatch( const char *mask, const char *name ) {

  char mn[24], me[24], nn[16], ne[16];
  const char *d;

  d = strrchr( mask, '.' );
  if ( d ) { strncpy( mn, mask, d - mask ); mn[d - mask] = 0; strcpy( me, d + 1 ); }
  else     { strcpy( mn, mask ); strcpy( me, "*" ); }

  d = strrchr( name, '.' );
  if ( d ) { strncpy( nn, name, d - name ); nn[d - name] = 0; strcpy( ne, d + 1 ); }
  else     { strcpy( nn, name ); ne[0] = 0; }

  return wildPart( mn, nn ) && wildPart( me, ne );
}

// + and -: ask for a mask and select (or unselect) the files that match.
// Folders are never selected this way (as with "select all"), but - also
// unselects them, so that - with *.* clears everything.
static void selectMask( Panel *p, int on ) {

  char mask[24];
  strcpy( mask, "*.*" );

  if ( !inputBox( on ? "Select files" : "Unselect files", "Mask (for example *.TXT):", mask, 20 ) ) return;

  int n = 0;

  for ( int i = 0; i < p->count; i++ ) {
    Item *it = &p->items[i];
    if ( strcmp( it->name, ".." ) == 0 ) continue;
    if ( on && it->isDir ) continue;
    if ( wildMatch( mask, it->name ) ) {
      it->sel = on ? 1 : 0;
      n++;
    }
  }

  if ( (n == 0) && on ) uiMessage( "Select files", "No file matches that mask.", NULL );
}

static void selectAll( Panel *p, int mode ) {   // 0 none, 1 all files, 2 invert files

  for ( int i = 0; i < p->count; i++ ) {
    Item *it = &p->items[i];
    if ( strcmp( it->name, ".." ) == 0 ) continue;

    if ( mode == 0 ) it->sel = 0;
    else if ( !it->isDir ) it->sel = (mode == 1) ? 1 : !it->sel;
  }
}

static void handleKey( int k ) {

  Panel *p = Active ? &Right : &Left;

  switch ( k ) {

    case K_UP:   p->cur--; break;
    case K_DOWN: p->cur++; break;
    case K_PGUP: p->cur -= LIST_ROWS; break;
    case K_PGDN: p->cur += LIST_ROWS; break;
    case K_HOME: p->cur = 0; break;
    case K_END:  p->cur = p->count - 1; break;

    case 9:      Active = !Active; break;

    case 13:     enterItem( p ); break;

    case K_INS:
      if ( (p->count > 0) && (strcmp( p->items[p->cur].name, ".." ) != 0) ) {
        p->items[p->cur].sel = !p->items[p->cur].sel;
      }
      p->cur++;
      break;

    case '+':    selectMask( p, 1 ); break;
    case '-':    selectMask( p, 0 ); break;
    case '*':    selectAll( p, 2 ); break;

    case K_F3:   cmdView( ); break;
    case K_CTRL_O: cmdDosScreen( ); break;
    case K_F5:   cmdDownload( ); break;
    case K_F7:   cmdMakeDir( ); break;
    case K_F8:   cmdDelete( ); break;

    case K_ALTF1: cmdChooseHost( ); break;
    case K_ALTF2: cmdDrive( ); break;
    case K_F1:   cmdHelp( ); break;

    case K_F2:   cmdReconnect( ); break;

    case K_F9:   cmdOptions( ); break;

    case K_F10:
    case 27: {
      const char *lines[1];
      lines[0] = "Leave Easy Commander?";
      if ( uiDialog( "Quit", lines, 1, 1 ) ) Running = 0;
      break;
    }

    default:
      break;
  }

  fixView( p );
}


// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Settings: the last host and the CRC option are remembered in EC.INI,
// in the folder of EC.EXE.  Problems with the file are ignored: the program
// simply works without it.

static char     IniPath[100];
static char     SavedHost[40];
static uint16_t SavedPort = 6502;
static int      UsedSavedHost = 0;
static int      NeedSearch = 0;

static void setIniPath( const char *argv0 ) {

  strncpy( IniPath, argv0, 80 );
  IniPath[80] = 0;

  // Keep everything up to the last '\\' or ':' (the folder of the program).
  int cut = -1;
  for ( int i = 0; IniPath[i]; i++ ) {
    if ( (IniPath[i] == '\\') || (IniPath[i] == ':') ) cut = i;
  }

  if ( cut >= 0 ) IniPath[cut + 1] = 0;
  else            IniPath[0] = 0;

  strcat( IniPath, "EC.INI" );
}

static void loadSettings( void ) {

  FILE *f = fopen( IniPath, "r" );
  if ( f == NULL ) return;

  char line[100];

  while ( fgets( line, sizeof( line ), f ) ) {

    int l = (int)strlen( line );
    while ( (l > 0) && ((line[l-1] == '\n') || (line[l-1] == '\r') || (line[l-1] == ' ')) ) {
      line[--l] = 0;
    }

    if ( strncmp( line, "host=", 5 ) == 0 ) {
      strncpy( SavedHost, line + 5, sizeof( SavedHost ) - 1 );
      SavedHost[sizeof( SavedHost ) - 1] = 0;
    }
    else if ( strncmp( line, "port=", 5 ) == 0 ) {
      unsigned long p = strtoul( line + 5, NULL, 10 );
      if ( (p > 0) && (p < 65536UL) ) SavedPort = (uint16_t)p;
    }
    else if ( strncmp( line, "verify=", 7 ) == 0 ) {
      Verify = (line[7] == '1');
    }
    else if ( strncmp( line, "fastcrc=", 8 ) == 0 ) {
      FastCrc = (line[8] != '0');
    }
  }

  fclose( f );
}

static void saveSettings( void ) {

  FILE *f = fopen( IniPath, "w" );
  if ( f == NULL ) return;

  fprintf( f, "host=%s\n", HostName );
  fprintf( f, "port=%u\n", (unsigned)ServerPort );
  fprintf( f, "verify=%d\n", Verify ? 1 : 0 );
  fprintf( f, "fastcrc=%d\n", FastCrc ? 1 : 0 );

  fclose( f );
}

static void endProgram( int rc ) {
  Utils::endStack( );
  Utils::dumpStats( stderr );
  exit( rc );
}

int main( int argc, char *argv[] ) {

  printf( "Easy Commander 0.4, Copyright (C) 2026 Daniel Szentmiklosi.\n" );
  printf( "Free software, GPLv3 or later. Built " __DATE__ ".\n\n" );

  setIniPath( argv[0] );
  loadSettings( );

  if ( argc > 1 ) {
    if ( strlen( argv[1] ) >= sizeof( HostName ) ) {
      fprintf( stderr, "Host name too long\n" );
      return 1;
    }
    strcpy( HostName, argv[1] );
  }
  else if ( SavedHost[0] ) {
    strcpy( HostName, SavedHost );
    ServerPort = SavedPort;
    UsedSavedHost = 1;
    printf( "Using the saved host %s (to use another one: EC <host>)\n", HostName );
  }
  else {
    NeedSearch = 1;            // no host given and none saved: look for one once TCP/IP runs
  }

  if ( argc > 2 ) {
    ServerPort = atoi( argv[2] );
    if ( ServerPort == 0 ) {
      fprintf( stderr, "Usage: EC [host] [port]\n" );
      return 1;
    }
  }

  Rx          = (uint8_t *)malloc( RX_SIZE );
  Left.items  = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  Right.items = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  TmpItems    = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  TreeBuf     = (uint8_t *)malloc( TREE_BUF_SIZE );
  CrcBuf      = (uint8_t *)malloc( 4096 );
  DiskBuf     = (char *)malloc( DISK_BUF_SIZE );     // optional: without it the default buffering is used

  if ( !Rx || !Left.items || !Right.items || !TmpItems || !TreeBuf || !CrcBuf ) {
    fprintf( stderr, "Not enough memory.\n" );
    return 1;
  }

  initCrcTable( );
  _harderr( criticalHandler );

  // TCP/IP (same calls as Netcat)

  if ( Utils::parseEnv( ) != 0 ) {
    exit( 2 );
  }

  if ( Utils::initStack( 2, TCP_SOCKET_RING_SIZE, ctrlBreakHandler, ctrlCHandler ) ) {
    fprintf( stderr, "\nFailed to initialize TCP/IP - exiting\n" );
    exit( 2 );
  }

  // From here on leave through endProgram( ).

  int addressKnown = 0;

  if ( NeedSearch ) {
    int r = searchAtStart( );
    if ( r < 0 ) endProgram( 1 );
    addressKnown = r;          // 1: the search already gave the address
  }

  if ( !addressKnown ) {

    printf( "Resolving %s - press Ctrl-Break to abort\n", HostName );

    if ( !resolveHost( HostName, ServerAddr ) ) {
      fprintf( stderr, "Error resolving host\n" );
      if ( UsedSavedHost ) fprintf( stderr, "That was the saved host. Try: EC <host>\n" );
      endProgram( 3 );
    }
  }

  printf( "Connecting to %d.%d.%d.%d port %u\n",
          ServerAddr[0], ServerAddr[1], ServerAddr[2], ServerAddr[3], ServerPort );

  if ( !connectHost( ) ) {
    fprintf( stderr, "Could not connect: %s\n", ErrMsg );
    if ( UsedSavedHost ) fprintf( stderr, "That was the saved host. Try: EC <host>\n" );
    endProgram( 3 );
  }

  Left.remote = 1;
  strcpy( Left.path, "\\" );

  if ( fetchRemoteList( Left.path, &Left ) != 0 ) {
    fprintf( stderr, "Cannot read the host's folder: %s\n", ErrMsg );
    endProgram( 3 );
  }

  saveSettings( );              // the connection works: remember this host

  Right.remote = 0;
  if ( getcwd( Right.path, 79 ) == NULL ) strcpy( Right.path, "C:\\" );

  // The user interface

  screenInit( );

  reload( &Right );
  fixView( &Left );
  drawAll( );

  while ( Running ) {

    if ( !kbhit( ) ) {
      pump( );

      // Alt pressed or released: switch the labels in the bottom line.
      if ( ((shiftFlags( ) & SHIFT_ALT) != 0) != (KeyBarAlt != 0) ) drawKeyBar( );

      continue;
    }

    handleKey( uiReadKey( ) );

    if ( Running ) drawAll( );
  }

  screenDone( );

  if ( Connected ) quitHost( );

  if ( Sock ) {
    Sock->close( );
    TcpSocketMgr::freeSocket( Sock );
  }

  endProgram( 0 );

  return 0;
}
