// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../TransferStatus.h"
#include <stdio.h>

int main() {
  struct Case { uint32_t status; bool accepted; const char *reason; };
  const Case cases[] = {
    {0x00000003, true,  "command and data complete"},
    {0x00000103, true,  "completion with unrelated card interrupt"},
    {0x00000000, false, "no completion / timeout"},
    {0x00000001, false, "command complete without data"},
    {0x00000002, false, "data complete without command"},
    {0x00008003, false, "error summary with both completion bits"},
    {0x00010003, false, "command timeout even without summary"},
    {0x00020003, false, "command CRC with completion"},
    {0x00100003, false, "data timeout with completion"},
    {0x00208003, false, "data CRC: previous false-success case"},
    {0x00408003, false, "data end-bit error with completion"},
    {0x02008003, false, "ADMA error with completion"},
    {0x80000003, false, "vendor error with completion"},
  };
  for (const auto &test : cases) {
    if (E5Emmc::transferOk(test.status) != test.accepted) {
      fprintf(stderr, "FAIL %08x: %s\n", test.status, test.reason);
      return 1;
    }
  }
  struct Window { uint64_t sectors, first, count; bool accepted; };
  const Window windows[] = {
    {61079552, E5Emmc::cacheFirstLba, E5Emmc::cacheSectorCount, true},
    {10, 5, 5, true},
    {10, 5, 6, false},
    {10, 10, 1, false},
    {10, 5, 0, false},
    {UINT64_MAX, UINT64_MAX - 5, 6, false},
  };
  for (const auto &test : windows) {
    if (E5Emmc::windowFitsCard(test.sectors, test.first, test.count) != test.accepted) {
      fprintf(stderr, "FAIL: cache window boundary/overflow case\n");
      return 1;
    }
  }
  printf("PASS: %zu SDHCI completion/error cases; %zu window boundary cases\n",
         sizeof(cases) / sizeof(cases[0]), sizeof(windows) / sizeof(windows[0]));
}
