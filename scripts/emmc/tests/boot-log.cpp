// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../BootLog.h"
#include <assert.h>
#include <limits.h>
using namespace E5BootLog;
int main() {
  assert(contains(0, 1) && contains(sectorCount - 1, 1));
  assert(!contains(sectorCount, 1) && !contains(0, 0));
  assert(!contains(1, UINT64_MAX) && !contains(UINT64_MAX, 1));
  assert(ready(0x900) && !ready(0x800) && !ready(0x700));
  assert(!ready(0x900 | (1u << 26)) && !ready(0x900 | 0x80));
  uint8_t p[512] = {};
  probe(p, 123456, partitionFirst + firstSector, 62926860);
  assert(validProbe(p)); p[100] ^= 1; assert(!validProbe(p));
  uint8_t entry[128] = {}; entry[0] = 1;
  const char *name = "boot_b";
  for (unsigned i = 0; i < 6; ++i) entry[56 + 2 * i] = name[i];
  put64(entry + 32, partitionFirst); put64(entry + 40, partitionFirst + partitionSectors - 1);
  assert(bootEntry(entry, 61079552)); entry[68] = 'x'; assert(!bootEntry(entry, 61079552));
  memset(p, 0, sizeof(p)); memcpy(p, "EFI PART", 8); put32(p + 8, 0x10000);
  put32(p + 12, 92); put64(p + 24, 1); put64(p + 32, 61079551);
  put64(p + 40, 34); put64(p + 48, 61079518); put64(p + 72, 2);
  put32(p + 80, 70); put32(p + 84, 128); put32(p + 16, crc(p, 92));
  Gpt g; assert(header(p, 61079552, g)); assert(g.count == 70);
  p[90] ^= 1; assert(!header(p, 61079552, g));
  memset(p, 0, sizeof(p)); memcpy(p, "ANDROID!", 8);
  put32(p + 8, 62926860); put32(p + 40, 4); assert(bootHeader(p));
  put32(p + 8, (uint32_t)(firstSector * 512)); assert(!bootHeader(p));
  memset(p, 0, sizeof(p)); memcpy(p + 448, "AVBf", 4);
  auto putBig = [](uint8_t *dst, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) dst[7 - i] = (uint8_t)(n >> (i * 8));
  };
  putBig(p + 452, 0x100000000ull); putBig(p + 460, 46776320);
  putBig(p + 468, 46776320); putBig(p + 476, 2304); assert(footer(p));
  putBig(p + 468, firstSector * 512); assert(!footer(p));
  return 0;
}
