#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <string>
#define __far
static unsigned char g_scratch[65536];
#define MK_FP(s,o) ((void*)(g_scratch + (o)))
union REGS { struct { unsigned short ax,bx,cx,dx,si,di,cflag; } x; struct { unsigned char al,ah,bl,bh,cl,ch,dl,dh; } h; };
static inline int int86(int,union REGS*,union REGS*){return 0;}
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
#define _HARDERR_FAIL 3
struct find_t { char reserved[21]; char attrib; unsigned short wr_time, wr_date; unsigned long size; char name[13]; };
static inline void fill_stat(struct find_t *fb, const std::string &full, const char *name){
  struct stat st; stat(full.c_str(),&st);
  strncpy(fb->name,name,12); fb->name[12]=0;
  fb->attrib = S_ISDIR(st.st_mode)?0x10:0x20; fb->size = st.st_size;
  fb->wr_date = ((2026-1980)<<9)|(11<<5)|1; fb->wr_time=(10<<11);
}
static inline unsigned fill_ft(struct find_t *fb){
  DIR *d; std::string *dir; memcpy(&d,fb->reserved,sizeof d); memcpy(&dir,fb->reserved+8,sizeof dir);
  struct dirent *e;
  while((e=readdir(d))){ fill_stat(fb,*dir+"/"+e->d_name,e->d_name); return 0; }
  closedir(d); delete dir; return 1;
}
static inline unsigned _dos_findfirst(const char *pat, unsigned, struct find_t *fb){
  std::string p=pat; for(auto &c:p) if(c=='\\') c='/';
  if ( p.size()>4 && p.substr(p.size()-4)=="/*.*" ) {
    std::string *dir=new std::string(p.substr(0,p.size()-4)); DIR *d=opendir(dir->c_str());
    if(!d){ delete dir; return 3; }
    memcpy(fb->reserved,&d,sizeof d); memcpy(fb->reserved+8,&dir,sizeof dir); return fill_ft(fb); }
  struct stat st; if(stat(p.c_str(),&st)) return 2;
  fill_stat(fb,p,"X"); return 0;
}
static inline unsigned _dos_findnext(struct find_t *fb){ return fill_ft(fb);}
static inline unsigned _dos_setfileattr(const char *p, unsigned){ std::string s=p; for(auto &c:s) if(c=='\\') c='/'; chmod(s.c_str(),0644); return 0; }
// drives: A, C and D exist; D: is "not ready"
static unsigned g_drive=3;
static inline void _dos_getdrive(unsigned *d){ *d=g_drive; }
static inline void _dos_setdrive(unsigned d, unsigned *total){ if(d==1||d==3||d==4) g_drive=d; *total=5; }
static inline void _harderr(int (*)(unsigned,unsigned,unsigned*)){}
