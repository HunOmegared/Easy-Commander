// Logic test: the fast CRC32 loop of EC.CPP must give exactly the same
// answers as the standard loop.  Exit code = number of failed checks.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#define stricmp strcasecmp
#define main ec_main
#include "ec_check.cpp"
#undef main

static int failures = 0;
static void check( const char *what, int ok ) {
  printf( "%-60s %s\n", what, ok ? "ok" : "*** FAILED ***" );
  if ( !ok ) failures++;
}

int main() {
  initCrcTable( );
  check( "self-test of the fast loop passed", FastCrcOk == 1 );

  const uint8_t *v = (const uint8_t *)"123456789";
  check( "standard loop: check value CBF43926",
         (crcStandard( 0xFFFFFFFFUL, v, 9 ) ^ 0xFFFFFFFFUL) == 0xCBF43926UL );
  check( "fast loop: check value CBF43926",
         (crcFast( 0xFFFFFFFFUL, v, 9 ) ^ 0xFFFFFFFFUL) == 0xCBF43926UL );
  check( "empty input", crcFast( 0x12345678UL, v, 0 ) == 0x12345678UL );

  static uint8_t buf[4096];
  srand( 12345 );
  int same = 1;
  for ( int round = 0; round < 300; round++ ) {
    uint16_t n = (round < 40) ? (uint16_t)round : (uint16_t)(rand( ) % 4097);
    for ( int i = 0; i < n; i++ ) buf[i] = (uint8_t)rand( );
    uint32_t start = ((uint32_t)rand( ) << 16) ^ (uint32_t)rand( );
    if ( crcFast( start, buf, n ) != crcStandard( start, buf, n ) ) same = 0;
  }
  check( "300 random blocks: fast == standard", same );

  // Chaining: two calls must equal one call (the file is read in blocks).
  for ( int i = 0; i < 4096; i++ ) buf[i] = (uint8_t)(i * 7 + 3);
  uint32_t one = crcFast( 0xFFFFFFFFUL, buf, 4096 );
  uint32_t two = crcFast( crcFast( 0xFFFFFFFFUL, buf, 1000 ), buf + 1000, 3096 );
  check( "chained blocks give the same CRC", one == two );

  printf( "%d failure(s)\n", failures );
  return failures;
}
