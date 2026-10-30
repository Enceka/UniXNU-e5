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
  // Fast-hotplug AON 0x520 value: preserve unrelated bits, set protect-en
  // bit20 and debounce-en bit0, clear detect-polar bit21 and count bits1-8.
  if (E5Sd::hotplugValue(0) != 0x00100001u) return 12;
  if (E5Sd::hotplugValue(0x002001feu) != 0x00100001u) return 13;
  if (E5Sd::hotplugValue(0xffc00000u) != 0xffd00001u) return 14;
  // UMP9620 linear LDO VOL: 1.2V floor, 10mV step -> 3.0V = 0xb4, 3.3V = 0xd2.
  if (E5Sd::ldoVolUv(3000000) != 0xb4u) return 15;
  if (E5Sd::ldoVolUv(3300000) != 0xd2u) return 16;
  if (E5Sd::ldoVolUv(1200000) != 0u) return 17;
  if (E5Sd::ldoVolUv(3750000) != 0xffu) return 18;
  // sdio0 aonapb chain source: Android SDR104 residue rpll/÷1/1x-half.
  if (E5Sd::sdio0SourceHz(4, 0, 1) != 195000000u) return 19;
  if (E5Sd::sdio0SourceHz(1, 0, 1) != 13000000u) return 20;
  if (E5Sd::sdio0SourceHz(1, 0, 0) != 26000000u) return 21;
  if (E5Sd::sdio0SourceHz(0, 0, 0) != 1000000u) return 22;
  if (E5Sd::sdio0SourceHz(2, 3, 1) != 38400000u) return 23;
  if (E5Sd::sdio0SourceHz(6, 0, 1) != 0u) return 24;
  // Host divider from a real 195 MHz 1x residue: 400k needs N=122 (0x7a).
  if (E5Sd::clockDivider(195000000, 400000) != (((122 & 0xff) << 8) | ((122 & 0x300) >> 2))) return 25;
  puts("PASS: SD response ordering, card capacity, divider ceilings, R1 state, hotplug value, LDO VOL, sdio0 chain source");
}
