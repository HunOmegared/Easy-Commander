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
#define TIMER_GET_CURRENT() ((clockTicks_t)0)
#define TIMER_MS_TO_TICKS(x) ((x)/55)
static inline uint32_t Timer_diff(uint32_t a,uint32_t b){return b-a;}
#define MyMTU 1500
#define TCP_SOCKET_RING_SIZE 16
#define _heapchk() 2
