// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace E5SystemOS {
// Independent raw APFS image after the unchanged 6,077,580,800-byte OS disk.
// Host preparation verifies the complete pinned image before any card write.
static constexpr uint64_t base = 12582912; // 6 GiB, in 512-byte sectors.
static constexpr uint64_t sectors = 6451200;
static constexpr uint32_t superblockCrc = 0xc034a6f0u;
static constexpr const char *uuid = "8846FE78-2910-4297-AF97-D59BB6F8FC16";

inline bool contains(uint64_t block, uint64_t count) {
  return count && block < sectors && count <= sectors - block;
}
inline bool fits(uint64_t cardSectors) {
  return base <= cardSectors && sectors <= cardSectors - base;
}
}
