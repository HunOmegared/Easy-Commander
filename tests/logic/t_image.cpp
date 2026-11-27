// Logic test: the disk image engine of ECU.EXE (INT 13h replaced by a fake disk).
// Checks CHS and LBA reading, unaligned offsets, a bad sector (becomes zeros) and the CRC.
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#define stricmp strcasecmp
#define main ec_main
#include "ec_check.cpp"
#undef main

static int failures = 0;
static void check(const char *what, int ok) { printf("%-64s %s\n", what, ok ? "ok" : "*** FAILED ***"); if (!ok) failures++; }

static const u32 SECTORS = 2880;                    // 1.44 MB, 18 spt, 2 heads
static std::vector<uint8_t> media;
static u32 badSector = 0xFFFFFFFFUL;
static int resets = 0;
static int useExt = 0;

static uint8_t pat(u32 i) { return (uint8_t)((i * 31 + (i >> 9) * 7 + 5) & 0xFF); }

static int readSectors(u32 lba, u32 n, void *buf) {
  for (u32 i = 0; i < n; i++) if (lba + i == badSector) return 0x10;
  if (lba + n > SECTORS) return 0x04;
  memcpy(buf, &media[lba * 512], n * 512);
  return 0;
}
static int bios13ReadChs(u8, u16 cyl, u8 head, u8 sect, u8 count, void *buf) {
  if (sect < 1 || sect > 18) return 0x01;
  if ((unsigned)sect - 1 + count > 18) return 0x09;     // may not cross a track
  return readSectors(((u32)cyl * 2 + head) * 18 + (sect - 1), count, buf);
}
static int bios13ReadLba(u8, u32 lba, u16 count, void *buf) { return readSectors(lba, count, buf); }
static void bios13Reset(u8) { resets++; }
static int bios13Params(DiskInfo *) { return 0; }

static uint32_t refCrc(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0 - (c & 1))); }
  return c ^ 0xFFFFFFFFUL;
}

int main() {
  initCrcTable();
  media.resize(SECTORS * 512);
  for (u32 i = 0; i < media.size(); i++) media[i] = pat(i);

  for (int mode = 0; mode < 2; mode++) {
    useExt = mode;
    DiskInfo d; memset(&d, 0, sizeof d);
    d.drive = 0; d.cyls = 80; d.heads = 2; d.spt = 18; d.sectors = SECTORS; d.ext = mode;
    printf("--- %s ---\n", mode ? "LBA (INT 13h ext)" : "CHS");
    ImgDisk = &d; ImgBytes = SECTORS * 512UL; ImgBad = 0; badSector = 0xFFFFFFFFUL;
    check("imgAlloc", imgAlloc());
    ImgDisk = &d;

    static char buf[16384];
    imgReadBytes(0, 16384, buf);
    check("first 16 KB equal", memcmp(buf, &media[0], 16384) == 0);
    imgReadBytes(1000001UL, 12345, buf);
    check("unaligned 12345 bytes at 1000001 equal", memcmp(buf, &media[1000001], 12345) == 0);
    imgReadBytes(SECTORS * 512UL - 100, 100, buf);
    check("last 100 bytes equal", memcmp(buf, &media[SECTORS * 512UL - 100], 100) == 0);

    uint32_t c; imgCrc(&c);
    check("CRC of the whole image", c == refCrc(&media[0], media.size()));

    badSector = 500; ImgBad = 0; resets = 0;
    std::vector<uint8_t> want = media; memset(&want[500 * 512], 0, 512);
    imgReadBytes(499 * 512UL, 4096, buf);
    check("bad sector: zeros, neighbours intact", memcmp(buf, &want[499 * 512], 4096) == 0);
    check("bad sector counted once and remembered", ImgBad == 1 && ImgBadFirst[0] == 500);
    check("bad sector: BIOS reset was tried", resets >= 3);
    imgCrc(&c);
    check("CRC with a bad sector = CRC of the zero-filled image", c == refCrc(&want[0], want.size()));
    check("CRC pass does not change the bad count", ImgBad == 1);
    imgFree();
  }
  printf("%d failure(s)\n", failures);
  return failures;
}
