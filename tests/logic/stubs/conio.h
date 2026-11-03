#pragma once
static int g_keys=1000000; static int g_keyval=13;
static inline int getch(){ return g_keyval; }
static inline int kbhit(){ return g_keys>0; }
