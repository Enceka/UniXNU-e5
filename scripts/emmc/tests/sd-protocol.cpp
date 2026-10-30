// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SdProtocol.h"
#include <stdio.h>

int main() {
  // Raw controller words reconstructed from this card's Android CSD.
  const uint32_t raw[] = {0x800a4000, 0x00735f7f, 0x325b5900, 0x00400e00};
  const uint32_t expected[] = {0x400e0032, 0x5b590000, 0x735f7f80, 0x0a400000};
  uint32_t csd[4]; E5Sd::response136(raw, csd);
  for (unsigned i = 0; i < 4; i++) if (csd[i] != expected[i]) return 1;
  if (E5Sd::sdhcSectors(csd) != 30244864) return 2;
  uint32_t largest[] = {0x40000000, 0x3f, 0xffff0000, 0};
  if (E5Sd::sdhcSectors(largest) != 0x100000000ull) return 3;
  largest[0] = 0; if (E5Sd::sdhcSectors(largest) != 0) return 4;
  largest[0] = 0x80000000; if (E5Sd::sdhcSectors(largest) != 0) return 5;
  if (E5Sd::clockDivider(26000000, 400000) != 0x1100) return 6;
  if (E5Sd::clockDivider(26000000, 13000000) != 0) return 7;
  if (E5Sd::clockDivider(390000000, 208000000) != 0) return 8;
  if (E5Sd::clockDivider(26000000, 0) != 0xffff) return 9;
  if (E5Sd::clockDivider(26000000, 1) != 0xffff) return 10;
  if (!E5Sd::readyTran(0x900) || E5Sd::readyTran(0x800) ||
      E5Sd::readyTran(0x00400900)) return 11;
  puts("PASS: SD response ordering, card capacity, divider ceilings and R1 state");
}
