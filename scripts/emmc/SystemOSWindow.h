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

namespace E5Data {
// r245: the Data window is served media-direct from the card — reads pull
// CMD18 chunks under the host lock and writes land as CMD25 chunks before
// their block I/O completes, so there is no RAM image and every completed
// write is already durable. The container on the card must be grown to the
// window size (macOS-side apfs resizeContainer keeps the volume UUID); the
// NXSB block count is cross-checked against the window at publication.
// 4 GiB fits the 14.4 GiB card at the same base with ~1.3 GiB spare; the
// r239 128 MiB RAM image and its whole-window boot load are gone.
static constexpr uint64_t base = 19034112; // in 512-byte sectors.
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
