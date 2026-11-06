// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace E5SystemOS {
// Independent raw APFS image, r257 served from the eMMC userdata tail.
// Host preparation verifies the complete pinned image before any card write;
// the identical image bytes moved from the SD span, so the superblock CRC is
// unchanged. The bases now sit past the SD card's capacity on purpose: the
// SD host's fits() self-refuses and the eMMC kext owns both windows.
static constexpr uint64_t base = 40103936; // eMMC userdata tail, 512-byte sectors.
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

namespace E5Data {
// r245: the Data window is served media-direct from the card — reads pull
// CMD18 chunks under the host lock and writes land as single-sector CMD24
// writes before their block I/O completes, so there is no RAM image and every
// completed write is already durable. The container on the card must be grown
// to the window size (macOS-side apfs resizeContainer keeps the volume UUID);
// the NXSB block count is cross-checked against the window at publication.
// r257: the window moved to the eMMC userdata tail, after the SystemOS image.
static constexpr uint64_t base = 46555136; // in 512-byte sectors.
static constexpr uint64_t sectors = 8388608; // 4 GiB media-direct window.
// Verified against the installed container's checksummed APSB objects and
// diskutil's read-only attachment. The earlier B5A22774 UUID was stale.
static constexpr const char *uuid = "30ADC749-351F-4FFF-998A-20E1B683859F";

inline bool contains(uint64_t block, uint64_t count) {
  return count && block < sectors && count <= sectors - block;
}
inline bool fits(uint64_t cardSectors) {
  return base <= cardSectors && sectors <= cardSectors - base;
}
}
