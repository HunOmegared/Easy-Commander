/*

   Easy Commander test client (ECTEST)

   A small command line client for the Easy Commander host. It only exists
   to prove the network part (connect, HELLO, LIST, TREE, GET) before a
   user interface is built on top of it.

   The structure and every mTCP call used here follow mTCP Netcat:
   mTCP Netcat.cpp, Copyright (C) 2006-2026 Michael B. Brutman
   (mbbrutman@gmail.com), http://www.brutman.com/mTCP

   This program uses the mTCP library, so it is distributed under the GNU
   General Public License version 3 or (at your option) any later version,
   like mTCP itself.

   Usage:
     ectest <host> <port> HELLO
     ectest <host> <port> LIST [path]
     ectest <host> <port> TREE [path]
     ectest <host> <port> GET <path> [localfile]

   Return codes:
     0 - All good
     1 - Args or setup error
     2 - TCP/IP initialization failed
     3 - Could not resolve or connect
     4 - Protocol or transfer error

*/

#include <dos.h>
#include <conio.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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


#define RX_SIZE        8192       // Our own receive buffer
#define TREE_BUF_SIZE  60000u     // Room for the folder list used by GETDIR
#define IO_TIMEOUT_MS  15000ul    // Give up if nothing arrives for this long

#define CMD_HELLO 1
#define CMD_LIST  2
#define CMD_TREE  3
#define CMD_GET   4
#define CMD_NOP   5       // Diagnostics: start and stop the stack only (no buffer allocated)
#define CMD_GETDIR 9      // Download a whole folder tree
#define CMD_NOP2  8       // Diagnostics: like NOP, but allocate two 8 KB buffers like Netcat
#define CMD_CONNECT 6     // Diagnostics: connect, then close at once
#define CMD_CONNECTQ 7    // Diagnostics: connect, QUIT politely, then close


// Used for outgoing data (same layout Netcat uses)

typedef struct {
  TcpBuffer b;
  uint8_t data[1460];
} DataBuf;


// Ctrl-Break and Ctrl-C handlers.  (Same as Netcat.)

volatile uint8_t CtrlBreakDetected = 0;

void __interrupt __far ctrlBreakHandler( ) {
  CtrlBreakDetected = 1;
}

void __interrupt __far ctrlCHandler( ) {
  // Do nothing
}


static TcpSocket  *Sock;
static uint8_t    *Rx;
static uint8_t    *TreeBuf;
static uint8_t    *CrcBuf;
static uint32_t    CrcTable[256];
static uint8_t     Verify = 0;      // /V given: check downloads with CRC32
static uint8_t    *DummyA;
static uint8_t    *DummyB;
static uint16_t    RxPos = 0;
static uint16_t    RxLen = 0;
static const char *ErrMsg = "";


static void usage( void );
static void endProgram( int rc );


static void usage( void ) {
  fprintf( stderr,
    "ectest <host> <port> HELLO\n"
    "ectest <host> <port> LIST [path]\n"
    "ectest <host> <port> TREE [path]\n"
    "ectest <host> <port> GET <path> [localfile]\n"
    "ectest <host> <port> GETDIR <path> <destfolder>\n"
    "  (add /V to GET or GETDIR to verify every download with a CRC32 check)\n"
    "ectest <host> <port> NOP | NOP2 | CONNECT | CONNECTQ   (diagnostics)\n\n"
    "Example: ectest 10.0.2.2 6502 LIST /\n" );
  exit( 1 );
}


// Only call this after Utils::initStack succeeded.

static uint8_t HeapDebug = 0;

// Diagnostics: print the raw result code of the C library heap check.
static void showHeap( const char *when ) {
  if ( HeapDebug ) fprintf( stderr, "_heapchk %s: %d\n", when, (int)_heapchk( ) );
}


static void endProgram( int rc ) {
  Utils::endStack( );
  showHeap( "after endStack" );
  Utils::dumpStats( stderr );
  exit( rc );
}


// ---------------------------------------------------------------------
// Receiving

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
    fprintf( stderr, "Command too long\n" );
    return -1;
  }

  DataBuf *buf = (DataBuf *)TcpBuffer::getXmitBuf( );

  if ( buf == NULL ) {
    fprintf( stderr, "No transmit buffers available\n" );
    return -1;
  }

  memcpy( buf->data, cmd, len );
  buf->data[len]   = 13;
  buf->data[len+1] = 10;
  buf->b.setDataLen( len + 2 );

  int16_t rc = Sock->enqueue( &buf->b );

  if ( rc ) {
    fprintf( stderr, "Error sending command: %d\n", rc );
    return -1;
  }

  return 0;
}


// ---------------------------------------------------------------------
// Protocol helpers

// Reads the first response line. Returns 0 for "OK ...", -1 otherwise.

static int expectOk( char *line, int max ) {

  if ( readLine( line, max ) < 0 ) {
    fprintf( stderr, "No response from host: %s\n", ErrMsg );
    return -1;
  }

  if ( line[0] == 'O' && line[1] == 'K' ) return 0;

  fprintf( stderr, "Host answered: %s\n", line );
  return -1;
}


// Text after "OK" in a response line.

static const char *afterOk( const char *line ) {
  if ( line[2] == ' ' ) return line + 3;
  return line + 2;
}


static const char *baseName( const char *path ) {
  const char *p = path;
  const char *last = path;
  while ( *p ) {
    if ( (*p == '\\') || (*p == '/') ) last = p + 1;
    p++;
  }
  return last;
}


static int doHello( void ) {

  char line[300];

  if ( sendCommand( "HELLO" ) ) return 4;
  if ( expectOk( line, sizeof( line ) ) ) return 4;

  printf( "%s\n", line );

  if ( strncmp( line, "OK EASYCOMMANDER", 16 ) != 0 ) {
    fprintf( stderr, "Warning: that does not look like an Easy Commander host.\n" );
    return 4;
  }

  return 0;
}


// LIST and TREE both answer with "OK <count> ..." followed by <count> lines.

static int doListing( const char *cmdName, const char *path ) {

  char line[300];
  char cmd[300];

  sprintf( cmd, "%s %s", cmdName, path );

  if ( sendCommand( cmd ) ) return 4;
  if ( expectOk( line, sizeof( line ) ) ) return 4;

  unsigned long count = strtoul( afterOk( line ), NULL, 10 );

  printf( "%s\n", line );

  unsigned long i;
  for ( i = 0; i < count; i++ ) {

    if ( readLine( line, sizeof( line ) ) < 0 ) {
      fprintf( stderr, "\nList ended early after %lu of %lu lines: %s\n", i, count, ErrMsg );
      return 4;
    }

    printf( "%s\n", line );
  }

  return 0;
}


#define FETCH_OK    0
#define FETCH_FATAL 4     // connection or protocol is no longer usable
#define FETCH_SKIP  5     // this file failed, but the session is still in sync
#define FETCH_BADCRC 6    // file received, but the CRC check failed (session still in sync)


static void initCrcTable( void ) {

  for ( uint16_t n = 0; n < 256; n++ ) {
    uint32_t c = n;
    for ( int k = 0; k < 8; k++ ) {
      if ( c & 1 ) c = 0xEDB88320UL ^ (c >> 1);
      else         c = c >> 1;
    }
    CrcTable[n] = c;
  }

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

    for ( uint16_t i = 0; i < n; i++ ) {
      crc = CrcTable[ (crc ^ CrcBuf[i]) & 0xFF ] ^ (crc >> 8);
    }

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
// disk.  Returns FETCH_OK if they match, FETCH_BADCRC if not,
// FETCH_SKIP if the host could not answer, FETCH_FATAL if the connection
// is gone.

static int verifyFile( const char *remote, const char *localName ) {

  char line[300];
  char cmd[300];

  sprintf( cmd, "CRC %s", remote );

  if ( sendCommand( cmd ) ) return FETCH_FATAL;

  if ( readLine( line, sizeof( line ) ) < 0 ) {
    fprintf( stderr, "No response from host: %s\n", ErrMsg );
    return FETCH_FATAL;
  }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    fprintf( stderr, "\nHost could not give a CRC: %s\n", line );
    return FETCH_SKIP;
  }

  const char *args = afterOk( line );
  char *endp;
  uint32_t hostSize = strtoul( args, &endp, 10 );
  uint32_t hostCrc  = strtoul( endp, NULL, 16 );

  uint32_t diskCrc, diskSize;

  if ( crcOfFile( localName, &diskCrc, &diskSize ) ) {
    fprintf( stderr, "\nCannot read %s back from disk\n", localName );
    return FETCH_BADCRC;
  }

  if ( (diskCrc != hostCrc) || (diskSize != hostSize) ) {
    fprintf( stderr, "\nCRC mismatch: host %08lX (%lu bytes), disk %08lX (%lu bytes)\n",
             (unsigned long)hostCrc, (unsigned long)hostSize,
             (unsigned long)diskCrc, (unsigned long)diskSize );
    return FETCH_BADCRC;
  }

  return FETCH_OK;
}


// Download one file.
// mode 0: stand-alone GET with progress output
// mode 1: part of GETDIR, prints only a dot for every 256 KB

static int fetchFile( const char *remote, const char *localName, int mode, uint32_t *gotOut ) {

  char line[300];
  char cmd[300];

  if ( gotOut ) *gotOut = 0;

  if ( strlen( remote ) > 280 ) {
    fprintf( stderr, "Path is too long\n" );
    return FETCH_SKIP;
  }

  // Open the local file first.  If that fails nothing has been sent yet,
  // so the session stays in sync and the caller can go on.

  FILE *f = fopen( localName, "wb" );
  if ( f == NULL ) {
    fprintf( stderr, "Cannot create %s\n", localName );
    return FETCH_SKIP;
  }

  sprintf( cmd, "GET %s", remote );

  if ( sendCommand( cmd ) ) {
    fclose( f );
    return FETCH_FATAL;
  }

  if ( readLine( line, sizeof( line ) ) < 0 ) {
    fprintf( stderr, "No response from host: %s\n", ErrMsg );
    fclose( f );
    return FETCH_FATAL;
  }

  if ( (line[0] != 'O') || (line[1] != 'K') ) {
    // The host refused (for example: file is gone).  Still in sync.
    fprintf( stderr, "Host answered: %s\n", line );
    fclose( f );
    remove( localName );
    return FETCH_SKIP;
  }

  uint32_t size = strtoul( afterOk( line ), NULL, 10 );

  if ( mode == 0 ) {
    printf( "Receiving %s (%lu bytes)\n", localName, (unsigned long)size );
  }

  clockTicks_t startTime = TIMER_GET_CURRENT( );

  uint32_t step       = (mode == 0) ? 16384ul : 262144ul;
  uint32_t nextReport = step;
  uint32_t remaining  = size;
  int      rc = FETCH_OK;

  while ( remaining ) {

    if ( waitData( ) < 0 ) {
      fprintf( stderr, "\nTransfer failed: %s\n", ErrMsg );
      rc = FETCH_FATAL;
      break;
    }

    uint16_t avail = RxLen - RxPos;
    uint16_t chunk = avail;
    if ( remaining < avail ) chunk = (uint16_t)remaining;

    if ( fwrite( Rx + RxPos, 1, chunk, f ) != chunk ) {
      fprintf( stderr, "\nError writing to disk (disk full?)\n" );
      rc = FETCH_FATAL;      // the rest of the file is still on its way
      break;
    }

    RxPos += chunk;
    remaining -= chunk;

    if ( (size - remaining) >= nextReport ) {
      if ( mode == 0 ) {
        printf( "\r%lu / %lu bytes", (unsigned long)(size - remaining), (unsigned long)size );
      }
      else {
        putchar( '.' );
        fflush( stdout );
      }
      nextReport += step;
    }

  }

  fclose( f );

  uint32_t received = size - remaining;
  if ( gotOut ) *gotOut = received;

  if ( mode == 0 ) {

    uint32_t elapsedMs = uint32_t(TIMER_GET_CURRENT( ) - startTime) * 55;

    printf( "\r%lu / %lu bytes\n", (unsigned long)received, (unsigned long)size );

    if ( rc == FETCH_OK ) {
      printf( "Done." );
      if ( elapsedMs > 0 ) {
        printf( " %lu.%03lu s, about %lu KB/s",
                (unsigned long)(elapsedMs / 1000), (unsigned long)(elapsedMs % 1000),
                (unsigned long)(((received / 1024ul) * 1000ul) / elapsedMs) );
      }
      printf( "\n" );
    }
    else {
      printf( "Incomplete file: %s\n", localName );
    }

  }

  if ( (rc == FETCH_OK) && Verify ) {

    if ( mode == 0 ) {
      printf( "Verifying CRC32 ... " );
      fflush( stdout );
    }

    rc = verifyFile( remote, localName );

    if ( mode == 0 ) {
      printf( (rc == FETCH_OK) ? "OK\n" : "FAILED\n" );
    }

  }

  return rc;
}


static int doGet( const char *path, const char *localName ) {
  return ( fetchFile( path, localName, 0, NULL ) == FETCH_OK ) ? 0 : 4;
}


// dir + "\" + rel, without doubling a separator that is already there.

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


// Download a folder with all subfolders into destDir.  The contents of
// <path> land directly in destDir.  The whole TREE list is read first (the
// host answers one command at a time), then folders are created and files
// are fetched in the order the host sent them.

static int doGetDir( const char *path, const char *destDir ) {

  char line[300];
  char cmd[300];
  char remote[300];
  char local[300];

  sprintf( cmd, "TREE %s", path );

  if ( sendCommand( cmd ) ) return 4;
  if ( expectOk( line, sizeof( line ) ) ) return 4;

  const char *args = afterOk( line );
  char *endp;
  unsigned long count = strtoul( args, &endp, 10 );
  unsigned long total = strtoul( endp, NULL, 10 );

  // Read the complete list.  Every line has to be read to stay in sync,
  // even if we run out of room to keep them.

  uint16_t used = 0;
  unsigned long stored = 0;
  unsigned long bad = 0;
  int tooBig = 0;

  unsigned long i;
  for ( i = 0; i < count; i++ ) {

    if ( readLine( line, sizeof( line ) ) < 0 ) {
      fprintf( stderr, "Folder list ended early after %lu of %lu lines: %s\n", i, count, ErrMsg );
      return 4;
    }

    char *tab1 = strchr( line, '\t' );
    if ( ((line[0] != 'D') && (line[0] != 'F')) || (tab1 == NULL) ) { bad++; continue; }

    char *rel = tab1 + 1;
    char *tab2 = strchr( rel, '\t' );
    if ( tab2 == NULL ) { bad++; continue; }

    *tab2 = 0;
    uint32_t sz = strtoul( tab2 + 1, NULL, 10 );

    uint16_t relLen = strlen( rel );
    if ( (relLen == 0) || (relLen > 120) ) { bad++; continue; }

    if ( ((uint32_t)used + 5 + relLen + 1) > TREE_BUF_SIZE ) { tooBig = 1; continue; }

    TreeBuf[used] = line[0];
    memcpy( TreeBuf + used + 1, &sz, 4 );
    memcpy( TreeBuf + used + 5, rel, relLen + 1 );
    used += 5 + relLen + 1;
    stored++;
  }

  if ( tooBig ) {
    fprintf( stderr, "This folder tree is too big for the list buffer (%u bytes).\n"
                     "Download its subfolders one at a time.\n", (unsigned)TREE_BUF_SIZE );
    return 4;
  }

  if ( bad ) {
    fprintf( stderr, "Warning: %lu list entries were ignored (bad format or path too long).\n", bad );
  }

  makeDirs( destDir );

  printf( "%lu entries, %lu bytes to download into %s\n\n", stored, total, destDir );

  clockTicks_t startTime = TIMER_GET_CURRENT( );

  unsigned long n = 0;
  unsigned long dirs = 0;
  unsigned long okFiles = 0;
  unsigned long skipped = 0;
  unsigned long badCrc = 0;
  uint32_t bytesDone = 0;
  int rc = 0;

  uint16_t pos = 0;

  while ( pos < used ) {

    char type = TreeBuf[pos];
    uint32_t sz;
    memcpy( &sz, TreeBuf + pos + 1, 4 );
    const char *rel = (const char *)(TreeBuf + pos + 5);
    pos += 5 + strlen( rel ) + 1;
    n++;

    joinPath( local, destDir, rel );

    if ( type == 'D' ) {
      printf( "[%lu/%lu] %s\\\n", n, stored, rel );
      makeDirs( local );
      dirs++;
      continue;
    }

    sprintf( remote, "%s\\%s", path, rel );

    printf( "[%lu/%lu] %s (%lu bytes) ", n, stored, rel, (unsigned long)sz );
    fflush( stdout );

    uint32_t got = 0;
    int r = fetchFile( remote, local, 1, &got );

    if ( r == FETCH_OK ) {
      printf( Verify ? " ok, CRC ok\n" : " ok\n" );
      okFiles++;
      bytesDone += got;
    }
    else if ( r == FETCH_SKIP ) {
      printf( " SKIPPED\n" );
      skipped++;
    }
    else if ( r == FETCH_BADCRC ) {
      printf( " CRC MISMATCH\n" );
      badCrc++;
    }
    else {
      printf( " FAILED\n" );
      rc = 4;
      break;
    }

  }

  uint32_t elapsedMs = uint32_t(TIMER_GET_CURRENT( ) - startTime) * 55;

  printf( "\nFolders: %lu  Files: %lu  Skipped: %lu  Bytes: %lu of %lu\n",
          dirs, okFiles, skipped, (unsigned long)bytesDone, total );

  if ( Verify ) {
    printf( "CRC32 check: %lu file(s) with errors\n", badCrc );
  }

  if ( elapsedMs > 0 ) {
    printf( "Time: %lu.%03lu s, about %lu KB/s\n",
            (unsigned long)(elapsedMs / 1000), (unsigned long)(elapsedMs % 1000),
            (unsigned long)(((bytesDone / 1024ul) * 1000ul) / elapsedMs) );
  }

  if ( rc ) {
    printf( "Stopped early: the connection or the disk failed.\n" );
  }
  else if ( skipped || badCrc || (bytesDone != total) ) {
    printf( "Not everything was copied - see the messages above.\n" );
    rc = 4;
  }

  return rc;
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


// ---------------------------------------------------------------------

int main( int argc, char *argv[] ) {

  fprintf( stderr, "Easy Commander test client (ECTEST) v8 - built " __DATE__ "\n\n" );

  // /V may appear anywhere: take it out of the argument list.
  for ( int a = 1; a < argc; a++ ) {
    if ( stricmp( argv[a], "/V" ) == 0 ) {
      Verify = 1;
      for ( int b = a; b < argc - 1; b++ ) argv[b] = argv[b+1];
      argc--;
      a--;
    }
  }

  if ( argc < 4 ) usage( );

  char hostName[80];
  if ( strlen( argv[1] ) >= sizeof( hostName ) ) usage( );
  strcpy( hostName, argv[1] );

  uint16_t port = atoi( argv[2] );
  if ( port == 0 ) usage( );

  int cmdId = 0;
  if      ( stricmp( argv[3], "HELLO" ) == 0 ) cmdId = CMD_HELLO;
  else if ( stricmp( argv[3], "LIST"  ) == 0 ) cmdId = CMD_LIST;
  else if ( stricmp( argv[3], "TREE"  ) == 0 ) cmdId = CMD_TREE;
  else if ( stricmp( argv[3], "GET"   ) == 0 ) cmdId = CMD_GET;
  else if ( stricmp( argv[3], "GETDIR" ) == 0 ) cmdId = CMD_GETDIR;
  else if ( stricmp( argv[3], "NOP"   ) == 0 ) cmdId = CMD_NOP;
  else if ( stricmp( argv[3], "NOP2"  ) == 0 ) cmdId = CMD_NOP2;
  else if ( stricmp( argv[3], "CONNECT" ) == 0 ) cmdId = CMD_CONNECT;
  else if ( stricmp( argv[3], "CONNECTQ" ) == 0 ) cmdId = CMD_CONNECTQ;
  else usage( );

  const char *path = "/";
  if ( argc > 4 ) path = argv[4];

  if ( strlen( path ) > 250 ) {
    fprintf( stderr, "Path is too long\n" );
    exit( 1 );
  }

  const char *localName = NULL;

  if ( cmdId == CMD_GET ) {
    if ( argc < 5 ) usage( );
    localName = baseName( path );
    if ( argc > 5 ) localName = argv[5];
    if ( *localName == 0 ) {
      fprintf( stderr, "GET needs a file path, not a folder\n" );
      exit( 1 );
    }
  }


  if ( (cmdId == CMD_NOP) || (cmdId == CMD_NOP2) ) HeapDebug = 1;
  showHeap( "at start" );

  const char *destDir = NULL;

  if ( cmdId == CMD_GETDIR ) {
    if ( argc < 6 ) usage( );
    destDir = argv[5];
    if ( (strlen( path ) > 120) || (strlen( destDir ) > 100) ) {
      fprintf( stderr, "Path is too long\n" );
      exit( 1 );
    }
  }

  // Allocate our buffer early so we fail before the stack is hooked.

  if ( cmdId == CMD_NOP2 ) {

    // Same allocation pattern as Netcat (two 8 KB buffers, never freed).
    DummyA = (uint8_t *)malloc( 8192 );
    DummyB = (uint8_t *)malloc( 8192 );
    if ( (DummyA == NULL) || (DummyB == NULL) ) {
      fprintf( stderr, "Not enough memory\n" );
      exit( 1 );
    }

  }
  else if ( cmdId != CMD_NOP ) {

    Rx = (uint8_t *)malloc( RX_SIZE );
    if ( Rx == NULL ) {
      fprintf( stderr, "Not enough memory\n" );
      exit( 1 );
    }

    if ( cmdId == CMD_GETDIR ) {
      TreeBuf = (uint8_t *)malloc( TREE_BUF_SIZE );
      if ( TreeBuf == NULL ) {
        fprintf( stderr, "Not enough memory\n" );
        exit( 1 );
      }
    }

  }


  if ( Verify ) {
    CrcBuf = (uint8_t *)malloc( 4096 );
    if ( CrcBuf == NULL ) {
      fprintf( stderr, "Not enough memory\n" );
      exit( 1 );
    }
    initCrcTable( );
  }

  showHeap( "before stack" );

  // Initialize TCP/IP.  (Same calls as Netcat.)

  if ( Utils::parseEnv( ) != 0 ) {
    exit( 2 );
  }

  if ( Utils::initStack( 2, TCP_SOCKET_RING_SIZE, ctrlBreakHandler, ctrlCHandler ) ) {
    fprintf( stderr, "\nFailed to initialize TCP/IP - exiting\n" );
    exit( 2 );
  }

  // From this point on exit through endProgram( ).

  if ( (cmdId == CMD_NOP) || (cmdId == CMD_NOP2) ) {
    fprintf( stderr, "NOP: stack started, shutting it down again\n" );
    endProgram( 0 );
  }


  uint16_t localPort = rand( ) + 1024;


  // Resolve the host name (or IP address).

  fprintf( stderr, "Resolving %s - press Ctrl-Break to abort\n", hostName );

  IpAddr_t serverAddr;

  int8_t rc2 = Dns::resolve( hostName, serverAddr, 1 );
  if ( rc2 < 0 ) {
    fprintf( stderr, "Error resolving host\n" );
    endProgram( 3 );
  }

  while ( 1 ) {
    if ( CtrlBreakDetected ) break;
    if ( !Dns::isQueryPending( ) ) break;
    Utils::processPackets( );
    Dns::drivePendingQuery( );
  }

  rc2 = Dns::resolve( hostName, serverAddr, 0 );
  if ( rc2 != 0 ) {
    fprintf( stderr, "Error resolving host\n" );
    endProgram( 3 );
  }


  // Connect.

  Sock = TcpSocketMgr::getSocket( );

  fprintf( stderr, "Connecting to %d.%d.%d.%d port %u\n\n",
           serverAddr[0], serverAddr[1], serverAddr[2], serverAddr[3], port );

  int8_t rc = Sock->connect( localPort, serverAddr, port, 10000 );

  if ( rc != 0 ) {
    fprintf( stderr, "Could not connect to the host\n" );
    endProgram( 3 );
  }


  // Run the command.

  int result = 0;

  switch ( cmdId ) {
    case CMD_HELLO: result = doHello( ); break;
    case CMD_LIST:  result = doListing( "LIST", path ); break;
    case CMD_TREE:  result = doListing( "TREE", path ); break;
    case CMD_GET:   result = doGet( path, localName ); break;
    case CMD_GETDIR: result = doGetDir( path, destDir ); break;
    case CMD_CONNECT:  printf( "Connected.\n" ); break;
    case CMD_CONNECTQ: printf( "Connected.\n" ); break;
  }


  // Close politely and leave.  (CONNECT deliberately skips the QUIT.)

  if ( cmdId != CMD_CONNECT ) quitHost( );

  Sock->close( );
  TcpSocketMgr::freeSocket( Sock );

  endProgram( result );

  return 0;
}
