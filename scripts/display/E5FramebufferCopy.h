// SPDX-License-Identifier: BSD-2-Clause-Patent
// Present premultiplied BGRA or two-plane XR10 surfaces over black as RGB565.
#pragma once
#include <stdint.h>

struct E5PixelSummary {
  uint32_t pixels=0, rgb=0, alpha=0, opaque=0, rgbWithZeroAlpha=0;
  uint32_t hash=2166136261u;
};

// The scanout is RAM, not a register window. Use aligned scalar pairs;
// volatile and may_alias keep device accesses exact and avoid wider/SIMD loads.
typedef uint32_t E5PixelWord __attribute__((__may_alias__));
static inline void e5_store_rgb565_pair(volatile uint16_t *p, uint16_t first, uint16_t second) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  const uint32_t word = ((uint32_t)first << 16) | second;
#else
  const uint32_t word = first | ((uint32_t)second << 16);
#endif
  *(volatile E5PixelWord *)p = word;
}
static inline uint16_t e5_bgra_rgb565(uint32_t pixel) {
  return (uint16_t)(((pixel >> 8) & 0xf800) | ((pixel >> 5) & 0x07e0) | ((pixel >> 3) & 0x001f));
}

static inline uint32_t e5_read_pixel_word(const uint8_t *p, bool aligned) {
  if (aligned) {
    uint32_t value = *(const volatile E5PixelWord *)p;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    return value;
  }
  // Device mappings cannot use unaligned word accesses. Keep this fallback
  // byte-wide even when the compiler recognizes a packed load pattern.
  const volatile uint8_t *bytes = p;
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
static inline void e5_count_pixel(E5PixelSummary &s, uint8_t b,
    uint8_t g, uint8_t r, uint8_t a) {
  const bool colored = (b | g | r) != 0;
  ++s.pixels; s.rgb += colored; s.alpha += a != 0;
  s.opaque += a == 255; s.rgbWithZeroAlpha += colored && !a;
  s.hash = (s.hash ^ b) * 16777619u;
  s.hash = (s.hash ^ g) * 16777619u;
  s.hash = (s.hash ^ r) * 16777619u;
  s.hash = (s.hash ^ a) * 16777619u;
}

static inline bool e5_framebuffer_layout(uintptr_t base, uint64_t bytes,
    uint64_t stride, uint32_t width, uint32_t height, unsigned pixelBytes) {
  return base && width && height && width <= 16384 && height <= 16384 &&
      stride >= (uint64_t)width * pixelBytes && stride <= UINT64_MAX / height &&
      bytes >= stride * height && bytes <= UINTPTR_MAX &&
      base <= UINTPTR_MAX - bytes;
}

static inline bool e5_copy_bgra_rgb565(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height,
    E5PixelSummary *summary = nullptr) {
  const uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  if ((dst & 1) || (destinationStride & 1) ||
      !e5_framebuffer_layout(dst, destinationBytes, destinationStride, width, height, 2) ||
      !e5_framebuffer_layout(src, sourceBytes, sourceStride, width, height, 4)) return false;
  E5PixelSummary result{};
  const bool aligned = ((src | sourceStride) & 3) == 0;
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint16_t *out = (volatile uint16_t *)(dst + y * destinationStride);
    const uint8_t *in = (const uint8_t *)(src + y * sourceStride);
    for (uint32_t x = 0; x < width;) {
      const uint8_t *p = in + (uint64_t)x * 4;
      const uint32_t pixel = e5_read_pixel_word(p, aligned);
      if (summary) e5_count_pixel(result, (uint8_t)pixel, (uint8_t)(pixel >> 8),
          (uint8_t)(pixel >> 16), (uint8_t)(pixel >> 24));
      if (((uintptr_t)(out + x) & 3) == 0 && x + 1 < width) {
        const uint32_t next = e5_read_pixel_word(p + 4, aligned);
        if (summary) e5_count_pixel(result, (uint8_t)next, (uint8_t)(next >> 8),
            (uint8_t)(next >> 16), (uint8_t)(next >> 24));
        e5_store_rgb565_pair(out + x, e5_bgra_rgb565(pixel), e5_bgra_rgb565(next));
        x += 2;
      } else { out[x++] = e5_bgra_rgb565(pixel); }
    }
  }
  if (summary) *summary = result;
  return true;
}

static inline bool e5_blank_rgb565(volatile void *destination,
    uint64_t bytes, uint64_t stride, uint32_t width, uint32_t height) {
  const uintptr_t dst = (uintptr_t)destination;
  if ((dst & 1) || (stride & 1) ||
      !e5_framebuffer_layout(dst, bytes, stride, width, height, 2)) return false;
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint16_t *out = (volatile uint16_t *)(dst + y * stride);
    for (uint32_t x = 0; x < width;) {
      if (((uintptr_t)(out + x) & 3) == 0 && x + 1 < width) {
        e5_store_rgb565_pair(out + x, 0, 0); x += 2;
      } else out[x++] = 0;
    }
  }
  return true;
}

struct E5B3a8Layout {
  uint64_t colorOffset, colorStride, alphaOffset, alphaStride;
};
static inline bool e5_b3a8_layout(uint64_t allocation, uint32_t width,
    uint32_t height, uint64_t colorOffset, uint64_t colorStride,
    uint64_t alphaOffset, uint64_t alphaStride, E5B3a8Layout &layout) {
  if (!width || !height || width > 16384 || height > 16384) return false;
  // Match NativeBridgeMetalFrontend::b3a8Planes when IOSurface omits both
  // row pitches: linear BGR10_XR followed by A8, each row aligned to 64 bytes.
  if (!colorStride && !alphaStride) {
    colorOffset = 0;
    colorStride = ((uint64_t)width * 4 + 63) & ~63ULL;
    alphaOffset = colorStride * height;
    alphaStride = ((uint64_t)width + 63) & ~63ULL;
  }
  if (colorStride < (uint64_t)width * 4 || alphaStride < width ||
      colorOffset > allocation || alphaOffset > allocation ||
      colorStride > (allocation - colorOffset) / height ||
      alphaStride > (allocation - alphaOffset) / height) return false;
  const uint64_t colorEnd = colorOffset + colorStride * height;
  const uint64_t alphaEnd = alphaOffset + alphaStride * height;
  if (colorOffset < alphaEnd && alphaOffset < colorEnd) return false;
  layout = {colorOffset, colorStride, alphaOffset, alphaStride};
  return true;
}

static inline uint8_t e5_xr10_u8(uint32_t code) {
  // XR10 black is 384, white is 894; clamp to the panel's SDR range.
  return code <= 384 ? 0 : code >= 894 ? 255 : (uint8_t)((code - 384 + 1) / 2);
}
static inline bool e5_copy_b3a8_rgb565(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, const E5B3a8Layout &layout, uint32_t width,
    uint32_t height, E5PixelSummary *summary = nullptr) {
  const uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  E5B3a8Layout checked{};
  if ((dst & 1) || (destinationStride & 1) || !src ||
      sourceBytes > UINTPTR_MAX || src > UINTPTR_MAX - sourceBytes ||
      !layout.colorStride || !layout.alphaStride ||
      !e5_framebuffer_layout(dst, destinationBytes, destinationStride, width, height, 2) ||
      !e5_b3a8_layout(sourceBytes, width, height, layout.colorOffset,
          layout.colorStride, layout.alphaOffset, layout.alphaStride, checked)) return false;
  E5PixelSummary result{};
  const bool aligned = (((src + layout.colorOffset) | layout.colorStride) & 3) == 0;
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint16_t *out = (volatile uint16_t *)(dst + y * destinationStride);
    const uint8_t *color = (const uint8_t *)(src + layout.colorOffset + y * layout.colorStride);
    const volatile uint8_t *alpha = (const volatile uint8_t *)(src + layout.alphaOffset + y * layout.alphaStride);
    for (uint32_t x = 0; x < width;) {
      auto pixel = [&](uint32_t at) -> uint16_t {
        const uint32_t word = e5_read_pixel_word(color + (uint64_t)at * 4, aligned);
        const uint8_t b = e5_xr10_u8(word & 1023), g = e5_xr10_u8((word >> 10) & 1023);
        const uint8_t r = e5_xr10_u8((word >> 20) & 1023);
        if (summary) e5_count_pixel(result, b, g, r, alpha[at]);
        return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      };
      const uint16_t first = pixel(x);
      if (((uintptr_t)(out + x) & 3) == 0 && x + 1 < width) {
        const uint16_t second = pixel(x + 1);
        e5_store_rgb565_pair(out + x, first, second); x += 2;
      } else out[x++] = first;
    }
  }
  if (summary) *summary = result;
  return true;
}
static inline bool e5_summarize_bgra(const void *source,uint64_t bytes,
    uint64_t stride,uint32_t width,uint32_t height,E5PixelSummary &summary) {
  if(!e5_framebuffer_layout((uintptr_t)source,bytes,stride,width,height,4))return false;
  E5PixelSummary result{};
  for(uint32_t y=0;y<height;++y) {
    const uint8_t *row=(const uint8_t *)source+y*stride;
    for(uint32_t x=0;x<width;++x) {
      const uint8_t *p=row+(uint64_t)x*4;const bool colored=(p[0]|p[1]|p[2])!=0;
      ++result.pixels;result.rgb+=colored;result.alpha+=p[3]!=0;
      result.opaque+=p[3]==255;result.rgbWithZeroAlpha+=colored && !p[3];
      for(unsigned c=0;c<4;++c)result.hash=(result.hash^p[c])*16777619u;
    }
  }
  summary=result;return true;
}
