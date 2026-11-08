#pragma once
#include <deque>
// Keyboard stub.  Normally every key is g_keyval.  In script mode (used by
// t_view) the keys come from g_script, and Esc is returned when it is empty.
static int g_keys=1000000; static int g_keyval=13;
static std::deque<int> g_script; static int g_scriptMode=0;
static inline int getch(){
  if(g_scriptMode){ if(g_script.empty()) return 27; int c=g_script.front(); g_script.pop_front(); return c; }
  return g_keyval; }
static inline int kbhit(){ return g_scriptMode ? (g_script.empty()?0:1) : (g_keys>0); }
// In script mode kbhit is false when the script is empty: a dialog that waits
// for a key then needs its key in the script (the tests push it).
