#pragma once
// Stand-in for the mTCP UDP layer.  A call to Udp::sendUdp with the search
// text makes the fake hosts in FD_HOSTS answer through the registered
// callback, exactly like Udp::process would deliver real answers.
#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>
struct UdpHeader { uint16_t src, dst, len, chksum; };
typedef struct { EthHeader eh; IpHeader ip; UdpHeader udp; } UdpPacket_t;
typedef void (*UdpCallback)(const unsigned char *, const UdpHeader *);
static const IpAddr_t IpBroadcastNonRoutable = {255,255,255,255};

struct FakeHost { uint8_t ip[4]; unsigned short port; std::string name; };
static std::vector<FakeHost> FD_HOSTS;
static std::vector<std::string> FD_JUNK;      // extra answers that must be ignored
static int FD_SENDS = 0;
static UdpCallback g_cb = 0;
static uint16_t g_cbPort = 0;

static inline void fd_deliver(const uint8_t *ip, const std::string &payload) {
  size_t n = sizeof(UdpPacket_t) + payload.size();
  uint8_t *pk = (uint8_t*)calloc(1, n);
  IpHeader *iph = (IpHeader*)(pk + sizeof(EthHeader));
  memcpy(iph->ip_src, ip, 4);
  iph->plen = (uint16_t)(20 + 8 + payload.size());
  UdpHeader *u = (UdpHeader*)(pk + sizeof(EthHeader) + sizeof(IpHeader));
  u->src = htons(6502); u->dst = htons(g_cbPort); u->len = htons((uint16_t)(8 + payload.size()));
  memcpy(pk + sizeof(UdpPacket_t), payload.data(), payload.size());
  if (g_cb) g_cb(pk, u); else free(pk);
}

struct Udp {
  static int8_t registerCallback(uint16_t port, UdpCallback f){ if (g_cb) return -1; g_cb=f; g_cbPort=port; return 0; }
  static int8_t unregisterCallback(uint16_t port){ if (g_cb && port==g_cbPort){ g_cb=0; return 0; } return -1; }
  static uint16_t getUnusedCallbackPort(void){ return 4000; }
  static int8_t sendUdp(const IpAddr_t, uint16_t, uint16_t dstPort, uint16_t payloadLen, unsigned char *data, uint8_t){
    std::string p((char*)data + sizeof(UdpPacket_t), payloadLen);
    FD_SENDS++;
    if (dstPort != 6502 || p.compare(0, 8, "ECDISC 1") != 0) return 0;
    for (size_t i=0;i<FD_HOSTS.size();i++){
      char buf[100]; snprintf(buf,sizeof buf,"ECHOST 1 %u %s\r\n",(unsigned)FD_HOSTS[i].port,FD_HOSTS[i].name.c_str());
      fd_deliver(FD_HOSTS[i].ip, buf);
    }
    uint8_t other[4]={10,0,0,99};
    for (size_t i=0;i<FD_JUNK.size();i++) fd_deliver(other, FD_JUNK[i]);
    return 0;
  }
};
