#pragma once
#include <stdint.h>
#include <stddef.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define stricmp strcasecmp
#define __interrupt
#define __far
typedef uint32_t clockTicks_t;
typedef uint8_t IpAddr_t[4];
static inline clockTicks_t fake_ticks(){ static clockTicks_t t=0; t+=3; return t; }
#define TIMER_GET_CURRENT() fake_ticks()
#define TIMER_MS_TO_TICKS(x) ((x)/55)
static inline uint32_t Timer_diff(uint32_t a,uint32_t b){return b-a;}
#define MyMTU 1500
#define TCP_SOCKET_RING_SIZE 16
#define _heapchk() 2

static inline uint16_t htons(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t ntohs(uint16_t v){ return htons(v); }
