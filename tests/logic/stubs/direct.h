#pragma once
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
static inline std::string t_conv(const char *p){ std::string s=p; for(auto &c:s) if(c=='\\') c='/'; return s; }
static inline FILE *t_fopen(const char *p,const char *m){ return fopen(t_conv(p).c_str(),m); }
static const char *g_failremove = 0;
static inline int t_remove(const char *p){ if(g_failremove && strstr(p,g_failremove)) return -1; return remove(t_conv(p).c_str()); }
static inline int t_mkdir(const char *p){ return mkdir(t_conv(p).c_str(),0755); }
static inline int t_rmdir(const char *p){ return rmdir(t_conv(p).c_str()); }
#define fopen(p,m) t_fopen(p,m)
#define remove(p) t_remove(p)
#define mkdir(p) t_mkdir(p)
#define rmdir(p) t_rmdir(p)
static char g_dcwd_c[]="C:\\DL", g_dcwd_a[]="A:\\";
static inline char *_getdcwd(int d,char *b,int){ if(d==1){ strcpy(b,g_dcwd_a); return b;} if(d==3){ strcpy(b,g_dcwd_c); return b;} if(d==4){ strcpy(b,"/tmp/fakeloc2"); return b;} return NULL; }
