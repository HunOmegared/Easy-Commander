/*

   Easy Commander - user interface prototype (step 1)

   A two panel, Norton Commander style text mode interface.

   This step has NO network code yet:
     - the LEFT panel shows built-in sample data that mirrors the test
       folder of the host (a stand-in for the host's files),
     - the RIGHT panel shows real local DOS folders (the download target).
   F5 only runs a fake progress bar. Nothing is copied.

   Build (plain Open Watcom, 16 bit DOS, no mTCP needed):
       wcl -bt=dos -ms -0 ecui.cpp

   Keys:
       Up/Down/PgUp/PgDn/Home/End   move        Tab      other panel
       Enter                        open folder Insert   select / unselect
       + (all files)  - (none)  * (invert)      F5       download (fake)
       F7  make folder (right panel)            F1       help
       F10 or Esc  quit

   License: GPLv3 or later, like the rest of Easy Commander.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <direct.h>


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
#define K_F5    0x13F
#define K_F7    0x141
#define K_F9    0x143
#define K_F10   0x144


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


// ---------------------------------------------------------------------
// Sample data for the left panel (mirrors the test folder from the tests)

#define SDATE ( ((2026-1980) << 9) | (11 << 5) | 1 )
#define STIME ( (10 << 11) | (48 << 5) )

typedef struct {
  const char *dir;
  const char *name;
  u8          isDir;
  u32         size;
} Sample;

static const Sample Samples[] = {
  { "",                         "EMPTY",         1, 0 },
  { "",                         "LONGFO~1",      1, 0 },
  { "",                         "TESTFO~1",      1, 0 },
  { "",                         "LONGFI~1.TXT",  0, 0 },
  { "",                         "LONGFI~2.TXT",  0, 0 },
  { "",                         "_RV_ZT~1.TXT",  0, 0 },
  { "LONGFO~1",                 "FILEON~1.TXT",  0, 9 },
  { "TESTFO~1",                 "SUBFOLD",       1, 0 },
  { "TESTFO~1",                 "NCT.IMA",       0, 1474560UL },
  { "TESTFO~1\\SUBFOLD",        "SUBSUB",        1, 0 },
  { "TESTFO~1\\SUBFOLD",        "NE2000.COM",    0, 8693 },
  { "TESTFO~1\\SUBFOLD",        "NOVELL.DOC",    0, 1733 },
  { "TESTFO~1\\SUBFOLD\\SUBSUB", "DX81ENG.EXE",  0, 12144784UL },
  { "TESTFO~1\\SUBFOLD\\SUBSUB", "NOVELL.NOT",   0, 4143 }
};

#define SAMPLE_COUNT ( sizeof( Samples ) / sizeof( Samples[0] ) )


// ---------------------------------------------------------------------
// Screen

static u16 __far *Vid;
static u8  SavedMode;
static u16 SavedCursor;

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

static void fill( int x, int y, int w, int h, u8 ch, u8 attr ) {
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
  fill( x+1,   y,     w-2, 1,   B_H, attr );
  fill( x+1,   y+h-1, w-2, 1,   B_H, attr );
  fill( x,     y+1,   1,   h-2, B_V, attr );
  fill( x+w-1, y+1,   1,   h-2, B_V, attr );
}

static void screenInit( void ) {

  union REGS r;

  r.h.ah = 0x0F;
  int86( 0x10, &r, &r );
  SavedMode = r.h.al & 0x7F;

  r.h.ah = 0x03;
  r.h.bh = 0;
  int86( 0x10, &r, &r );
  SavedCursor = r.x.cx;

  if ( SavedMode == 7 ) {
    printf( "Easy Commander needs a colour display.\n" );
    exit( 1 );
  }

  // 80x25 colour text; this also clears the screen.
  r.x.ax = 0x0003;
  int86( 0x10, &r, &r );

  Vid = (u16 __far *)MK_FP( 0xB800, 0 );

  // Hide the hardware cursor.
  r.h.ah = 0x01;
  r.x.cx = 0x2000;
  int86( 0x10, &r, &r );
}

static void screenDone( void ) {

  union REGS r;

  r.h.ah = 0x00;
  r.h.al = SavedMode;
  int86( 0x10, &r, &r );

  r.h.ah = 0x01;
  r.x.cx = SavedCursor;
  int86( 0x10, &r, &r );
}


// ---------------------------------------------------------------------
// Keyboard and time

static int readKey( void ) {
  int c = getch( );
  if ( (c == 0) || (c == 0xE0) ) {
    c = getch( );
    return 0x100 + c;
  }
  return c;
}

static u32 ticks( void ) {
  return *(volatile u32 __far *)MK_FP( 0x40, 0x6C );
}

static void waitTicks( u32 n ) {
  u32 t = ticks( );
  while ( (u32)(ticks( ) - t) < n ) { }
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

static void loadSample( Panel *p ) {

  const char *dir = p->path;
  if ( *dir == '\\' ) dir++;

  p->count = 0;
  p->truncated = 0;

  if ( *dir ) addItem( p, "..", 1, 0, 0, 0 );

  for ( unsigned i = 0; i < SAMPLE_COUNT; i++ ) {
    if ( stricmp( Samples[i].dir, dir ) == 0 ) {
      addItem( p, Samples[i].name, Samples[i].isDir, Samples[i].size, SDATE, STIME );
    }
  }

  sortItems( p );
}

static void loadLocal( Panel *p ) {

  struct find_t fb;
  char pattern[100];

  p->count = 0;
  p->truncated = 0;

  int len = (int)strlen( p->path );

  if ( len > 3 ) addItem( p, "..", 1, 0, 0, 0 );

  sprintf( pattern, "%s%s*.*", p->path, (p->path[len-1] == '\\') ? "" : "\\" );

  unsigned rc = _dos_findfirst( pattern, _A_SUBDIR | _A_RDONLY | _A_ARCH, &fb );

  while ( rc == 0 ) {

    if ( strcmp( fb.name, "." ) && strcmp( fb.name, ".." ) ) {
      addItem( p, fb.name, (fb.attrib & _A_SUBDIR) ? 1 : 0, fb.size, fb.wr_date, fb.wr_time );
    }

    rc = _dos_findnext( &fb );
  }

  sortItems( p );
}

static void fixView( Panel *p ) {

  if ( p->cur >= p->count ) p->cur = p->count - 1;
  if ( p->cur < 0 ) p->cur = 0;

  if ( p->cur < p->top ) p->top = p->cur;
  if ( p->cur >= (p->top + LIST_ROWS) ) p->top = p->cur - LIST_ROWS + 1;
  if ( p->top < 0 ) p->top = 0;
}

static void reload( Panel *p ) {
  if ( p->remote ) loadSample( p ); else loadLocal( p );
  fixView( p );
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

  p->cur = 0;
  p->top = 0;
  reload( p );

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
  fill( x0 + 1, 21, 38, 1, B_HS, A_BORDER );
  putCh( x0 + 39, 21, B_RT, A_BORDER );

  // Path in the top border.
  if ( strlen( p->path ) <= 34 ) sprintf( buf, " %s ", p->path );
  else                           sprintf( buf, " ...%s ", p->path + strlen( p->path ) - 31 );
  putStr( x0 + (40 - (int)strlen( buf )) / 2, 0, buf, active ? A_TITLE_ON : A_TITLE_OFF );

  // File list.
  for ( int row = 0; row < LIST_ROWS; row++ ) {

    int idx = p->top + row;

    if ( idx >= p->count ) {
      fill( x0 + 1, 1 + row, 38, 1, ' ', A_NORMAL );
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
  putStr( x0 + 2, 23, p->remote ? " Host " : " Local ", A_BORDER );
}

static void drawKeyBar( void ) {

  static const char *labels[10] = {
    "Help", "Conn", "", "", "Get", "", "MkDir", "", "Opts", "Quit"
  };

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

  fill( x, y, w, h, ' ', A_DLG );
  drawBox( x, y, w, h, A_DLGB );

  sprintf( t, " %s ", title );
  putStr( x + (w - (int)strlen( t )) / 2, y, t, A_DLG );
}

// Shows lines of text.  yesno: Enter/Y gives 1, Esc/N gives 0.
// Otherwise any key closes it and 1 is returned.
static int dialog( const char *title, const char **lines, int n, int yesno ) {

  const char *hint = yesno ? "Enter = Yes     Esc = No" : "Press any key";

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
    int k = readKey( );
    if ( !yesno ) return 1;
    if ( (k == 13) || (k == 'y') || (k == 'Y') ) return 1;
    if ( (k == 27) || (k == 'n') || (k == 'N') ) return 0;
  }
}

static void message( const char *title, const char *l1, const char *l2 ) {
  const char *lines[2];
  lines[0] = l1;
  lines[1] = l2;
  dialog( title, lines, l2 ? 2 : 1, 0 );
}

// Asks for a short text.  Returns 1 on Enter (with text), 0 on Esc.
static int inputBox( const char *title, const char *prompt, char *buf, int maxlen ) {

  int w = 44, h = 7;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;
  int len = (int)strlen( buf );

  while ( 1 ) {

    dialogFrame( x, y, w, h, title );
    putStr( x + 3, y + 2, prompt, A_DLG );
    fill( x + 3, y + 3, w - 6, 1, ' ', A_FIELD );
    putStr( x + 3, y + 3, buf, A_FIELD );
    putCh( x + 3 + len, y + 3, '_', A_FIELD );
    putStr( x + 3, y + 5, "Enter = OK     Esc = Cancel", A_DLGHINT );

    int k = readKey( );

    if ( k == 13 ) return len > 0;
    if ( k == 27 ) return 0;

    if ( (k == 8) && (len > 0) ) {
      len--;
      buf[len] = 0;
    }
    else if ( (k >= 32) && (k < 127) && (len < maxlen) ) {
      buf[len++] = (char)k;
      buf[len] = 0;
    }
  }
}

// A stand-in for the real transfer: only shows how the progress dialog
// will look and that Esc cancels.
static void fakeDownload( int n, u32 bytes, const char *target ) {

  char l1[70], l2[70];
  int w = 50, h = 9;
  int x = (SCR_W - w) / 2;
  int y = (25 - h) / 2;
  int barW = 36;
  int cancelled = 0;

  sprintf( l1, "%d item(s), %lu bytes", n, bytes );
  sprintf( l2, "to %.40s", target );

  dialogFrame( x, y, w, h, "Downloading (fake)" );
  putStrPad( x + 3, y + 2, l1, w - 6, A_DLG );
  putStrPad( x + 3, y + 3, l2, w - 6, A_DLG );
  putStr( x + 3, y + 7, "Esc = Cancel", A_DLGHINT );

  for ( int step = 0; step <= barW; step++ ) {

    char pct[12];
    sprintf( pct, "%3d %%", (step * 100) / barW );

    fill( x + 7, y + 5, step, 1, B_FULL, A_DLG );
    fill( x + 7 + step, y + 5, barW - step, 1, B_LIGHT, A_DLG );
    putStr( x + 3, y + 5, pct, A_DLG );

    waitTicks( 3 );

    if ( kbhit( ) && (readKey( ) == 27) ) {
      cancelled = 1;
      break;
    }
  }

  message( cancelled ? "Cancelled" : "Done",
           cancelled ? "The fake download was cancelled." : "Nothing was copied: this is only a demo.",
           NULL );
}


// ---------------------------------------------------------------------
// Commands

static void cmdDownload( void ) {

  if ( Active != 0 ) {
    message( "Download", "Select files in the left (host) panel,", "then press F5." );
    return;
  }

  Panel *p = &Left;
  u32 bytes;
  int n = countSelected( p, &bytes );

  // Nothing selected: take the item under the cursor.
  if ( (n == 0) && (p->count > 0) && (strcmp( p->items[p->cur].name, ".." ) != 0) ) {
    n = 1;
    bytes = p->items[p->cur].size;
  }

  if ( n == 0 ) {
    message( "Download", "Nothing to download.", NULL );
    return;
  }

  char l1[70], l2[70];
  const char *lines[3];

  sprintf( l1, "Download %d item(s), %lu bytes", n, bytes );
  sprintf( l2, "to %.50s ?", Right.path );
  lines[0] = l1;
  lines[1] = l2;

  if ( dialog( "Download", lines, 2, 1 ) ) {
    fakeDownload( n, bytes, Right.path );
  }
}

static void cmdMakeDir( void ) {

  if ( Active != 1 ) {
    message( "Make folder", "Folders can only be made in the", "right (local) panel." );
    return;
  }

  char name[16];
  name[0] = 0;

  if ( !inputBox( "Make folder", "Name of the new folder:", name, 12 ) ) return;

  char full[100];
  strcpy( full, Right.path );
  pathDown( full, name );

  if ( mkdir( full ) != 0 ) {
    message( "Make folder", "Could not create that folder.", NULL );
    return;
  }

  reload( &Right );

  for ( int i = 0; i < Right.count; i++ ) {
    if ( stricmp( Right.items[i].name, name ) == 0 ) { Right.cur = i; break; }
  }
  fixView( &Right );
}

static void cmdHelp( void ) {

  const char *lines[10];

  lines[0] = "Up/Down/PgUp/PgDn/Home/End   move the cursor";
  lines[1] = "Enter       open a folder (.. goes up)";
  lines[2] = "Tab         switch panel";
  lines[3] = "Insert      select / unselect and move down";
  lines[4] = "+  -  *     select all files / none / invert";
  lines[5] = "F5          download selected items to the right panel";
  lines[6] = "F7          make a folder (right panel)";
  lines[7] = "F10 or Esc  quit";
  lines[8] = "";
  lines[9] = "Step 1: no network yet, the left panel shows sample data.";

  dialog( "Easy Commander - help", lines, 10, 0 );
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

    case '+':    selectAll( p, 1 ); break;
    case '-':    selectAll( p, 0 ); break;
    case '*':    selectAll( p, 2 ); break;

    case K_F5:   cmdDownload( ); break;
    case K_F7:   cmdMakeDir( ); break;
    case K_F1:   cmdHelp( ); break;

    case K_F2:
    case K_F9:
      message( "Not yet", "This comes in a later step.", NULL );
      break;

    case K_F10:
    case 27: {
      const char *lines[1];
      lines[0] = "Leave Easy Commander?";
      if ( dialog( "Quit", lines, 1, 1 ) ) Running = 0;
      break;
    }

    default:
      break;
  }

  fixView( p );
}


// ---------------------------------------------------------------------

#ifndef HOSTTEST

int main( void ) {

  Left.items  = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  Right.items = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );

  if ( (Left.items == NULL) || (Right.items == NULL) ) {
    printf( "Not enough memory.\n" );
    return 1;
  }

  Left.remote = 1;
  strcpy( Left.path, "\\" );

  Right.remote = 0;
  if ( getcwd( Right.path, 79 ) == NULL ) strcpy( Right.path, "C:\\" );

  screenInit( );

  reload( &Left );
  reload( &Right );
  drawAll( );

  while ( Running ) {

    if ( !kbhit( ) ) continue;      // later: the network is polled here

    handleKey( readKey( ) );

    if ( Running ) drawAll( );
  }

  screenDone( );

  return 0;
}

#endif
