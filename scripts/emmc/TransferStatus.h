// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace E5Emmc {
// Live GPT and Android sysfs agree: cache is 64 MiB, not 1 GiB.
constexpr uint64_t cacheFirstLba = 0xd0a800ull;
constexpr uint64_t cacheSectorCount = 0x20000ull;

constexpr bool windowFitsCard(uint64_t sectors, uint64_t first, uint64_t count) {
  return count != 0 && first < sectors && count <= sectors - first;
}

// SDHCI error summary is bit 15 and detailed errors occupy bits 16..31.
// A failed transfer can also set both completion bits: errors always win.
constexpr bool transferOk(uint32_t status) {
  return (status & 0xFFFF8000u) == 0 && (status & 0x03u) == 0x03u;
}
}
