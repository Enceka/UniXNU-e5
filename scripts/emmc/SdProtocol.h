// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace E5Sd {
constexpr uint32_t r1Errors = 0xfdffe008u;

inline void response136(const uint32_t raw[4], uint32_t result[4]) {
  for (unsigned i = 0; i < 4; i++)
    result[i] = (raw[3 - i] << 8) | (i == 3 ? 0u : raw[2 - i] >> 24);
}

inline uint64_t sdhcSectors(const uint32_t csd[4]) {
  if ((csd[0] >> 30) != 1) return 0; // SDHC/SDXC CSD v2 only.
  uint32_t size = ((csd[1] & 0x3fu) << 16) | (csd[2] >> 16);
  return ((uint64_t)size + 1) << 10;
}

// Unisoc supplies the 2x source. For nonzero N, SDCLK = source/(4*N).
// Zero bypasses the divider, giving source/2. Reject unattainable ceilings.
constexpr uint16_t clockDivider(uint32_t source, uint32_t ceiling) {
  if (source == 0 || ceiling == 0) return 0xffff;
  uint64_t twice = (uint64_t)ceiling * 2;
  uint64_t div = ((uint64_t)source + twice - 1) / twice;
  if (div <= 1) return 0;
  uint64_t n = (div + 1) / 2;
  return n > 1023 ? 0xffff : (uint16_t)(((n & 0xff) << 8) | ((n & 0x300) >> 2));
}

constexpr bool readyTran(uint32_t response) {
  return (response & r1Errors) == 0 && (response & 0x1f00u) == 0x0900u;
}

// AON 0x520 fast-hotplug value from the vendor power-up sequence:
// protect-en bit20 and debounce-en bit0 set, debounce count bits1-8 = 0,
// detect-polar bit21 cleared (E5 cd-gpios flags 0x23 -> polar clear).
constexpr uint32_t hotplugValue(uint32_t cur) {
  return (cur | 0x00100001u) & ~0x002001feu;
}
}
