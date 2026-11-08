#pragma once
#include <deque>
#include <string>
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
struct TcpBuffer { uint16_t len; void setDataLen(uint16_t n){ len=n; } static TcpBuffer *getXmitBuf(){ return (TcpBuffer*)calloc(1,sizeof(TcpBuffer)+1500); } };
struct EthHeader{ uint8_t b[14]; };
struct TcpHeader{ uint8_t hlen; uint8_t rest[19]; int getTcpHlen(){return hlen;} };
struct IpHeader{ uint16_t plen; uint8_t pad[10]; uint8_t ip_src[4]; uint8_t ip_dest[4]; void *payloadPtr(){return (uint8_t*)this+20;} uint16_t payloadLen(){return plen;} };
inline void Buffer_free(const void *p){ free((void*)p); }

static std::string FH_ROOT="/tmp/fakehost";
static int FH_BADCRC=0;
static std::deque<std::vector<uint8_t> > FH_PKTS;
static int FH_COMMANDS=0;

static uint32_t fh_crc(const std::vector<uint8_t>&d){ uint32_t c=0xFFFFFFFFu; for(size_t i=0;i<d.size();i++){ c^=d[i]; for(int k=0;k<8;k++) c=(c&1)?(c>>1)^0xEDB88320u:(c>>1);} return c^0xFFFFFFFFu; }
static void fh_send(const std::string &hdr,const std::vector<uint8_t>&body){
  std::vector<uint8_t> all(hdr.begin(),hdr.end()); all.insert(all.end(),body.begin(),body.end());
  size_t pos=0; if(all.empty()) return;
  while(pos<all.size()){ size_t n=std::min<size_t>(1400,all.size()-pos);
    std::vector<uint8_t> pk(14+20+20+n,0); IpHeader *ip=(IpHeader*)&pk[14]; ip->plen=20+n; ((TcpHeader*)&pk[34])->hlen=20;
    memcpy(&pk[54],&all[pos],n); FH_PKTS.push_back(pk); pos+=n; }
}
static std::string fh_conv(std::string s){ for(auto&c:s) if(c=='\\') c='/'; return s; }
static bool fh_isdir(const std::string&p){ struct stat st; return stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode); }
static bool fh_read(const std::string&p,std::vector<uint8_t>&out){ FILE*f=fopen(p.c_str(),"rb"); if(!f) return false; uint8_t b[4096]; size_t n; while((n=fread(b,1,sizeof b,f))>0) out.insert(out.end(),b,b+n); fclose(f); return true; }
static std::vector<std::string> fh_names(const std::string&dir){ std::vector<std::string> v; DIR*d=opendir(dir.c_str()); if(!d) return v; struct dirent*e; while((e=readdir(d))){ std::string n=e->d_name; if(n=="."||n=="..") continue; v.push_back(n);} closedir(d); std::sort(v.begin(),v.end()); return v; }
static void fh_tree(const std::string&dir,const std::string&rel,std::vector<std::string>&out){
  std::vector<std::string> names=fh_names(dir);
  for(int pass=0;pass<2;pass++) for(size_t i=0;i<names.size();i++){ bool isd=fh_isdir(dir+"/"+names[i]); if((pass==0)!=isd) continue;
    std::string r=rel.empty()?names[i]:rel+"\\"+names[i]; struct stat st; stat((dir+"/"+names[i]).c_str(),&st);
    char buf[300]; snprintf(buf,sizeof buf,"%c\t%s\t%ld\t20261101\t1048\r\n",isd?'D':'F',r.c_str(),isd?0L:(long)st.st_size); out.push_back(buf);
    if(isd) fh_tree(dir+"/"+names[i],r,out); }
}
static void fh_command(const std::string &cmd){
  FH_COMMANDS++;
  size_t sp=cmd.find(' '); std::string verb=cmd.substr(0,sp), arg=(sp==std::string::npos)?"":cmd.substr(sp+1);
  std::vector<uint8_t> none;
  if(verb=="HELLO"){ fh_send("OK EASYCOMMANDER 1\r\n",none); return; }
  if(verb=="LIST"||verb=="TREE"||verb=="GET"||verb=="CRC"){
    std::vector<std::string> f; { size_t p=0; while(p<arg.size()){ size_t q=arg.find(' ',p); if(q==std::string::npos) q=arg.size(); if(q>p) f.push_back(arg.substr(p,q-p)); p=q+1; } }
    std::string path=f.empty()?"":fh_conv(f[0]); if(path.find("..")!=std::string::npos){ fh_send("ERR 400 Bad request\r\n",none); return; }
    std::string full=FH_ROOT+(path.empty()||path[0]!='/'?"/":"")+path;
    if(verb=="LIST"||verb=="TREE"){
      if(!fh_isdir(full)){ fh_send("ERR 404 Not found\r\n",none); return; }
      std::vector<std::string> lines;
      if(verb=="TREE") fh_tree(full,"",lines);
      else { std::vector<std::string> names=fh_names(full); for(int pass=0;pass<2;pass++) for(size_t i=0;i<names.size();i++){ bool isd=fh_isdir(full+"/"+names[i]); if((pass==0)!=isd) continue; struct stat st; stat((full+"/"+names[i]).c_str(),&st); char buf[300]; snprintf(buf,sizeof buf,"%c\t%s\t%ld\t20261101\t1048\r\n",isd?'D':'F',names[i].c_str(),isd?0L:(long)st.st_size); lines.push_back(buf);} }
      char h[64]; if(verb=="TREE"){ long tot=0; for(size_t i=0;i<lines.size();i++){ if(lines[i][0]=='F'){ size_t a=lines[i].find('\t',2); tot+=atol(lines[i].c_str()+a+1);} } snprintf(h,sizeof h,"OK %zu %ld\r\n",lines.size(),tot);} else snprintf(h,sizeof h,"OK %zu\r\n",lines.size());
      std::string all=h; for(size_t i=0;i<lines.size();i++) all+=lines[i]; fh_send(all,none); return; }
    std::vector<uint8_t> data; if(fh_isdir(full)||!fh_read(full,data)){ fh_send(fh_isdir(full)?"ERR 406 Is a folder, use TREE\r\n":"ERR 404 Not found\r\n",none); return; }
    if(verb=="CRC"){ char h[64]; uint32_t c=fh_crc(data); if(FH_BADCRC) c^=1; snprintf(h,sizeof h,"OK %zu %08X\r\n",data.size(),c); fh_send(h,none); return; }
    long off=f.size()>1?atol(f[1].c_str()):0; long len=f.size()>2?atol(f[2].c_str()):-1; long total=data.size();
    if(off>total){ fh_send("ERR 400 Bad request\r\n",none); return; }
    long n=total-off; if(len>=0&&len<n) n=len; char h[64]; snprintf(h,sizeof h,"OK %ld %ld\r\n",n,total);
    fh_send(h,std::vector<uint8_t>(data.begin()+off,data.begin()+off+n)); return; }
  fh_send("ERR 400 Unknown command\r\n",none);
}
struct Q { void *dequeue(){ if(FH_PKTS.empty()) return 0; std::vector<uint8_t> v=FH_PKTS.front(); FH_PKTS.pop_front(); uint8_t*p=(uint8_t*)malloc(v.size()); memcpy(p,&v[0],v.size()); return p; } };
struct TcpSocket { Q incoming;
  int16_t enqueue(TcpBuffer*b){ std::string c((char*)b+sizeof(TcpBuffer),b->len); while(!c.empty()&&(c.back()=='\r'||c.back()=='\n')) c.erase(c.size()-1); fh_command(c); free(b); return 0; }
  int8_t connect(uint16_t,IpAddr_t,uint16_t,uint32_t){return 0;} void close(){} bool isRemoteClosed(){return false;} };
struct TcpSocketMgr { static TcpSocket *getSocket(){ static TcpSocket s; return &s; } static void freeSocket(TcpSocket*){} };
struct Utils { static int parseEnv(){return 0;} static int initStack(int,int,void(*)(),void(*)()){return 0;}
  static void endStack(){} static void dumpStats(FILE*){} static void processPackets(){} };
struct Dns { static int8_t resolve(const char*,IpAddr_t,int){return 0;} static bool isQueryPending(){return false;} static void drivePendingQuery(){} };
