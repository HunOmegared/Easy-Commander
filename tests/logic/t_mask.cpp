// Logic test: select / unselect by mask (+ and -), DOS style wildcards.
// Exit code = number of failed checks.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#define stricmp strcasecmp
#define main ec_main
#include "ec_check.cpp"
#undef main

static int failures = 0;
static void check( const char *what, int ok ) {
  printf( "%-62s %s\n", what, ok ? "ok" : "*** FAILED ***" );
  if ( !ok ) failures++;
}

static void typeKeys( const char *s ) {
  while ( *s ) g_script.push_back( (unsigned char)*s++ );
  g_script.push_back( 13 );
}

static void addItem( Panel *p, const char *name, int dir ) {
  Item *it = &p->items[p->count++];
  memset( it, 0, sizeof( *it ) );
  strcpy( it->name, name );
  it->isDir = dir;
}

static int selCount( Panel *p ) {
  int n = 0;
  for ( int i = 0; i < p->count; i++ ) if ( p->items[i].sel ) n++;
  return n;
}

static int isSel( Panel *p, const char *name ) {
  for ( int i = 0; i < p->count; i++ ) if ( strcmp( p->items[i].name, name ) == 0 ) return p->items[i].sel;
  return -1;
}

static u16 FakeScreen[80*25];

int main() {
  Vid = FakeScreen;
  check( "*.TXT matches README.TXT",        wildMatch( "*.TXT", "README.TXT" ) );
  check( "*.txt is not case sensitive",     wildMatch( "*.txt", "README.TXT" ) );
  check( "*.TXT does not match A.DOC",      !wildMatch( "*.TXT", "A.DOC" ) );
  check( "*.* matches a name without dot",  wildMatch( "*.*", "SETUP" ) );
  check( "*.TXT does not match SETUP",      !wildMatch( "*.TXT", "SETUP" ) );
  check( "READ* means READ*.*",             wildMatch( "READ*", "README.TXT" ) && wildMatch( "READ*", "READ" ) );
  check( "READ* does not match XREAD.TXT",  !wildMatch( "READ*", "XREAD.TXT" ) );
  check( "A?C.* matches ABC.X, not ABBC.X", wildMatch( "A?C.*", "ABC.X" ) && !wildMatch( "A?C.*", "ABBC.X" ) );
  check( "*.??? matches DRIVER.SYS",        wildMatch( "*.???", "DRIVER.SYS" ) );
  check( "R*E.TXT matches README.TXT",      wildMatch( "R*E.TXT", "README.TXT" ) );
  check( "SETUP. matches SETUP only",       wildMatch( "SETUP.", "SETUP" ) && !wildMatch( "SETUP.", "SETUP.EXE" ) );
  check( "* matches everything",            wildMatch( "*", "A.B" ) && wildMatch( "*", "NOEXT" ) );

  Left.items = (Item *)malloc( MAX_ITEMS * sizeof( Item ) );
  Left.count = 0;
  addItem( &Left, "..", 1 );
  addItem( &Left, "DRIVERS", 1 );
  addItem( &Left, "README.TXT", 0 );
  addItem( &Left, "INSTALL.TXT", 0 );
  addItem( &Left, "SETUP.EXE", 0 );
  addItem( &Left, "LICENSE", 0 );
  Left.cur = 0;
  Active = 0;
  g_scriptMode = 1;

  typeKeys( "*.TXT" );
  handleKey( '+' );
  check( "+ *.TXT selects the two text files",
         selCount( &Left ) == 2 && isSel( &Left, "README.TXT" ) == 1 && isSel( &Left, "INSTALL.TXT" ) == 1 );

  typeKeys( "INSTALL.*" );
  handleKey( '-' );
  check( "- INSTALL.* unselects that one",
         selCount( &Left ) == 1 && isSel( &Left, "README.TXT" ) == 1 );

  g_script.push_back( 13 );
  handleKey( '+' );
  check( "+ Enter (mask *.*) selects all files, not folders",
         selCount( &Left ) == 4 && isSel( &Left, "DRIVERS" ) == 0 && isSel( &Left, ".." ) == 0 );

  Left.items[1].sel = 1;                      // a folder selected with Insert
  g_script.push_back( 13 );
  handleKey( '-' );
  check( "- Enter (mask *.*) clears everything, folders too", selCount( &Left ) == 0 );

  typeKeys( "*.TXT" );
  handleKey( '+' );
  g_script.push_back( 27 );
  handleKey( '-' );
  check( "Esc in the mask box changes nothing", selCount( &Left ) == 2 );

  handleKey( '*' );
  check( "* still inverts the selection (files only)",
         isSel( &Left, "README.TXT" ) == 0 && isSel( &Left, "SETUP.EXE" ) == 1 && isSel( &Left, "DRIVERS" ) == 0 );

  // typing replaces the proposed *.* ; Backspace edits it
  g_script.push_back( 8 ); g_script.push_back( 8 ); g_script.push_back( 8 );
  typeKeys( "SETUP.*" );
  handleKey( '-' );
  check( "Backspace edits the proposed mask", isSel( &Left, "SETUP.EXE" ) == 0 );

  typeKeys( "*.ZZZ" );
  g_script.push_back( 27 );                   // the "No file matches" message waits for a key
  handleKey( '+' );
  check( "no match: message shown, selection unchanged", selCount( &Left ) == 1 && isSel( &Left, "LICENSE" ) == 1 );

  printf( "%d failure(s)\n", failures );
  return failures;
}
