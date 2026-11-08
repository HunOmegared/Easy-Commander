#pragma once
#include "conio.h"
// _bios_keybrd for the tests: looks at (READY) or takes (READ) the next key of
// the script; Esc is 0x011B like the BIOS returns it.
#define _KEYBRD_READ  0
#define _KEYBRD_READY 1
static inline unsigned _bios_keybrd( unsigned cmd ) {
  if ( !g_scriptMode || g_script.empty() ) return 0;
  int c = g_script.front();
  unsigned code = (c == 27) ? 0x011B : (0x1E00u | (unsigned)(c & 0xFF));
  if ( cmd == _KEYBRD_READ ) g_script.pop_front();
  return code;
}
