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

// sdio0 aonapb chain (live-verified under Android; the fetched realme
// ums9620-clk.c table is offset -0xC and does not match this silicon):
// mux 0x64920280 bits[2:0] parent rates, 2x divider 0x64920284 bits[10:0]
// with value+1 encoding, 1x selector 0x64920288 bit0 (set = 1x is 2x/2).
// Android SDR104 idle: mux=4 rpll, d2x=0, d1x=1 -> 195 MHz to the host.
constexpr uint32_t sdio0SourceHz(uint32_t mux, uint32_t d2x, uint32_t d1x) {
  const uint32_t parent[6] = {1000000, 26000000, 307200000, 384000000, 390000000, 409600000};
  if (mux >= 6) return 0;
  const uint32_t twoX = parent[mux] / (d2x + 1);
  return d1x ? twoX / 2 : twoX;
}

constexpr bool readyTran(uint32_t response) {
  return (response & r1Errors) == 0 && (response & 0x1f00u) == 0x0900u;
}

// AON 0x520 fast-hotplug value from the vendor power-up sequence:
// protect-en bit20 and debounce-en bit0 set, debounce count bits1-8 = 0,
// detect-polar bit21 SET. E5 cd-gpios = <eic-sync 35 0>: the flags cell is 0
// (the 0x23 cell is the pin NUMBER), so of_get_named_gpio_flags() reports
// detect_gpio_polar = false and the vendor fast_hotplug_enable() takes the
// else branch, SETTING the polarity bit. Round 13 proved it: with bit21 set
// the card echoes CMD8 immediately; with it clear every response command
// CRC-fails with a zero response register (fast-hotplug gates the response
// path while it believes no card is present).
constexpr uint32_t hotplugValue(uint32_t cur) {
  return (cur | 0x00210001u) & ~0x000001feu;
}

// UMP9620 linear LDO VOL encoding (vendor ump9620-regulator.c): 10mV steps
// from 1.2V, 8-bit field -> 3.0V = 0xb4, 3.3V = 0xd2.
constexpr uint32_t ldoVolUv(uint32_t microvolts) {
  return (microvolts - 1200000u) / 10000u;
}
}
