// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copy a CPU-rendered premultiplied BGRA surface over black to E5 RGB565.
#pragma once
#include <stdint.h>

static inline bool e5_framebuffer_layout(uintptr_t base, uint64_t bytes,
    uint64_t stride, uint32_t width, uint32_t height, unsigned pixelBytes) {
  return base && width && height && width <= 16384 && height <= 16384 &&
      stride >= (uint64_t)width * pixelBytes && stride <= UINT64_MAX / height &&
      bytes >= stride * height && bytes <= UINTPTR_MAX &&
      base <= UINTPTR_MAX - bytes;
}

static inline bool e5_copy_bgra_rgb565(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height) {
  const uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  if ((dst & 1) || (destinationStride & 1) ||
      !e5_framebuffer_layout(dst, destinationBytes, destinationStride, width, height, 2) ||
      !e5_framebuffer_layout(src, sourceBytes, sourceStride, width, height, 4)) return false;
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint16_t *out = (volatile uint16_t *)(dst + y * destinationStride);
    const uint8_t *in = (const uint8_t *)(src + y * sourceStride);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t *p = in + (uint64_t)x * 4;
      out[x] = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
    }
  }
  return true;
}

static inline bool e5_blank_rgb565(volatile void *destination,
    uint64_t bytes, uint64_t stride, uint32_t width, uint32_t height) {
  const uintptr_t dst = (uintptr_t)destination;
  if ((dst & 1) || (stride & 1) ||
      !e5_framebuffer_layout(dst, bytes, stride, width, height, 2)) return false;
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint16_t *out = (volatile uint16_t *)(dst + y * stride);
    for (uint32_t x = 0; x < width; ++x) out[x] = 0;
  }
  return true;
}
