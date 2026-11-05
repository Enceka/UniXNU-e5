// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

namespace E5BootLog {
constexpr uint64_t partitionSectors = 131072;
constexpr uint64_t partitionFirst = 1566720;
constexpr uint64_t firstSector = 124928; // 61 MiB, partition relative.
constexpr uint64_t sectorCount = 4096;   // 2 MiB; last MiB is protected.
constexpr uint32_t r1Errors = 0xffffa080u;
constexpr uint32_t slots = 8, slotSectors = 512, recordPayload = 444;
inline uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
inline uint64_t be64(const uint8_t *p) {
  uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n = n << 8 | p[i]; return n;
}
inline void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
inline void put64(uint8_t *p, uint64_t n) {
  put32(p, (uint32_t)n); put32(p + 4, (uint32_t)(n >> 32));
}
inline uint32_t crcUpdate(uint32_t c, const uint8_t *p, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    c ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
  }
  return c;
}
inline uint32_t crc(const uint8_t *p, unsigned n) { return ~crcUpdate(0xffffffffu, p, n); }
inline bool contains(uint64_t relative, uint64_t count) {
  return count && relative < sectorCount && count <= sectorCount - relative;
}
inline bool ready(uint32_t r1) {
  return !(r1 & r1Errors) && (r1 & 0x1f00u) == 0x0900u;
}
struct Gpt {
  uint64_t entriesLba;
  uint32_t count, entriesCrc;
};
inline bool header(const uint8_t *p, uint64_t cardSectors, Gpt &g) {
  if (memcmp(p, "EFI PART", 8) || le32(p + 8) != 0x10000 || cardSectors < 34) return false;
  uint32_t bytes = le32(p + 12);
  if (bytes < 92 || bytes > 512 || le64(p + 24) != 1 || le64(p + 32) != cardSectors - 1) return false;
  uint8_t copy[512]; memcpy(copy, p, bytes); put32(copy + 16, 0);
  if (crc(copy, bytes) != le32(p + 16)) return false;
  g.entriesLba = le64(p + 72); g.count = le32(p + 80); g.entriesCrc = le32(p + 88);
  uint64_t usable = le64(p + 40), last = le64(p + 48);
  if (le32(p + 84) != 128 || !g.count || g.count > 128 || g.entriesLba < 2 ||
      usable >= cardSectors || last >= cardSectors - 1 || usable > last) return false;
  return g.entriesLba < usable && (g.count * 128u + 511u) / 512u <= usable - g.entriesLba;
}
inline bool bootEntry(const uint8_t *p, uint64_t cardSectors) {
  static const char name[] = "boot_b";
  for (unsigned i = 0; i < sizeof(name); ++i)
    if (p[56 + i * 2] != (uint8_t)name[i] || p[57 + i * 2]) return false;
  unsigned type = 0; for (unsigned i = 0; i < 16; ++i) type |= p[i];
  uint64_t first = le64(p + 32), last = le64(p + 40);
  return type && first == partitionFirst && last < cardSectors &&
      last >= first && last - first + 1 == partitionSectors;
}
inline bool bootHeader(const uint8_t *p) {
  return !memcmp(p, "ANDROID!", 8) && le32(p + 40) == 4 &&
      !le32(p + 12) && !le32(p + 16) && le32(p + 8) &&
      (uint64_t)le32(p + 8) + 4096 <= firstSector * 512;
}
inline bool footer(const uint8_t *sector) {
  const uint8_t *p = sector + 448;
  if (memcmp(p, "AVBf", 4) || be64(p + 4) != 0x100000000ull) return false;
  uint64_t total = partitionSectors * 512, original = be64(p + 12);
  uint64_t start = be64(p + 20), bytes = be64(p + 28);
  if (!original || original > total || !bytes || start > total - 64 || bytes > total - 64 - start)
    return false;
  return start >= (firstSector + sectorCount) * 512 || start + bytes <= firstSector * 512;
}
inline void probe(uint8_t *p, uint32_t nonce, uint64_t lba, uint32_t payloadBytes) {
  for (unsigned i = 0; i < 508; ++i) p[i] = (uint8_t)((i * 37u) ^ (nonce >> ((i & 3) * 8)) ^ 0xa5u);
  memcpy(p, "E5LOGT1", 8); put32(p + 8, 1); put32(p + 12, nonce);
  put64(p + 16, lba); put32(p + 24, payloadBytes); put32(p + 508, crc(p, 508));
}
inline bool validProbe(const uint8_t *p) {
  return !memcmp(p, "E5LOGT1", 8) && le32(p + 8) == 1 && le32(p + 508) == crc(p, 508);
}
inline void record(uint8_t *p, uint32_t nonce, uint64_t generation, uint64_t seq,
                   uint64_t uptimeNs, uint64_t stream, uint32_t flags,
                   uint64_t dropped, const uint8_t *bytes, uint32_t length) {
  memset(p, 0, 512);
  memcpy(p, "E5BLOG1", 8); put32(p + 8, 1); put32(p + 12, nonce);
  put64(p + 16, generation); put64(p + 24, seq); put64(p + 32, uptimeNs);
  put64(p + 40, stream); put32(p + 48, length); put32(p + 52, flags);
  put64(p + 56, dropped);
  if (length && bytes && length <= recordPayload) memcpy(p + 64, bytes, length);
  put32(p + 508, crc(p, 508));
}
inline bool validRecord(const uint8_t *p) {
  return !memcmp(p, "E5BLOG1", 8) && le32(p + 8) == 1 && le32(p + 48) <= recordPayload &&
      le64(p + 16) && le32(p + 508) == crc(p, 508);
}
}
