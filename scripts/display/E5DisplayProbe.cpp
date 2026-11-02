// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

// E5TW only: printf would change the framebuffer being measured.
#include <stdarg.h>
extern "C" void e5_kputc(char);
extern "C" void e5_iomfb_trace_dump(void);
static void e5_display_tracef(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
}

extern kmod_info_t g_kernel_kmod_info;
extern "C" void e5_display_static_init(void) __asm__("__GLOBAL__sub_I_E5Display.cpp");
extern "C" unsigned e5_display_compiled_size(void);

// Sampled by NativeSd after the early boot log has wrapped.
int e5_display_construction = 0; // v: entered,parent,ctors,post,meta,alloc,ancestry,init,release
int e5_display_original_size = 0; // s: actual registered Legacy class size (hex)
int e5_display_registration_result = -1; // r: full postModLoad OSReturn (hex)

static const OSMetaClass *findClass(const char *name) {
  const OSSymbol *key = OSSymbol::withCString(name);
  const OSMetaClass *meta = key ? OSMetaClass::getMetaClassWithName(key) : nullptr;
  if (key) key->release();
  return meta;
}

extern "C" unsigned e5_display_backend_total = 0;
extern "C" unsigned e5_display_backend_slots = 0;
extern "C" unsigned e5_display_backend_last_slot = 0;
extern "C" unsigned e5_display_backend_last_sel = 0;

extern "C" IOReturn e5_display_backend_soft(unsigned slot, uint32_t selector,
                                            void *b, void *c, void *d) {
  // r88 walk-research round: backboardd reached slot 213 (r87). The remaining
  // abstract backend slots return the SAME tolerated error the pitch path
  // proved safe (kIOReturnUnsupported) so the client walk continues past
  // every backend call. r89: counters ride the always-captured rw-done echo
  // (printf alone wraps out of the ring), mask bits follow PURE order
  // 211,213,245,246,306,307,337,338,345 -> bits 0..8.
  static const unsigned kPureSlots[9] = {211, 213, 245, 246, 306, 307, 337, 338, 345};
  unsigned mask = 0;
  for (unsigned i = 0; i < 9; ++i)
    if (kPureSlots[i] == slot) { mask = 1u << i; break; }
  __atomic_fetch_add(&e5_display_backend_total, 1U, __ATOMIC_RELAXED);
  __atomic_fetch_or(&e5_display_backend_slots, mask, __ATOMIC_RELAXED);
  __atomic_store_n(&e5_display_backend_last_slot, slot, __ATOMIC_RELAXED);
  __atomic_store_n(&e5_display_backend_last_sel, selector, __ATOMIC_RELAXED);
  e5_display_tracef("E5Display: soft backend slot %u sel=%x b=%llx c=%llx d=%llx -> e00002f0\n",
         slot, selector, (unsigned long long)b, (unsigned long long)c,
         (unsigned long long)d);
  return kIOReturnUnsupported;
}

// r97 frame-acceptance probe: crc32 the panel scanout window the loader
// handed the kernel (0xBE000000, 320x480 RGB565, 0x4B000 bytes) at a slow
// cadence. The UEFI menu leaves a static image; any later change is a
// kernel-console redraw or a real SpringBoard frame. Count + last crc ride
// the rw-done echo (F=) because standalone printf lines wrap out of the
// persistent ring; the print only fires on transitions.
extern "C" unsigned e5_display_frame_changes = 0;
extern "C" unsigned e5_display_frame_crc = 0;
static IODeviceMemory *sPanelMemory = nullptr;
static IOMemoryMap *sPanelMap = nullptr;
static unsigned sPanelKicks = 0;

static uint32_t e5_display_crc32(const volatile uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

extern "C" void e5_display_frame_kick(void) {
  static unsigned traceKicks;
  if ((traceKicks++ % 16) == 0) e5_iomfb_trace_dump();
  if (sPanelKicks == 0) {
    sPanelMemory = IODeviceMemory::withRange(0xBE000000ull, 0x4B000ull);
    if (sPanelMemory) sPanelMap = sPanelMemory->map();
    if (!sPanelMap) {
      e5_display_tracef("E5Display: panel map failed - frame probe disabled\n");
      sPanelKicks = 1; // never try again; crc stays 0
      return;
    }
    e5_display_tracef("E5Display: panel mapped at %p (0xBE000000+0x4B000)\n",
           (void *)(uintptr_t)sPanelMap->getVirtualAddress());
  }
  if (!sPanelMap) return;
  sPanelKicks++;
  if ((sPanelKicks % 16) != 2) return; // ~every 16th big-read echo batch
  const volatile uint16_t *px = (const volatile uint16_t *)sPanelMap->getVirtualAddress();
  uint32_t crc = e5_display_crc32((const volatile uint8_t *)px, 0x4B000);
  if (crc != e5_display_frame_crc) {
    if (e5_display_frame_changes || e5_display_frame_crc)
      e5_display_tracef("E5Display: panel crc %08x (was %08x) change #%u\n",
             crc, e5_display_frame_crc, e5_display_frame_changes + 1);
    __atomic_store_n(&e5_display_frame_crc, crc, __ATOMIC_RELAXED);
    unsigned n = __atomic_fetch_add(&e5_display_frame_changes, 1U, __ATOMIC_RELAXED) + 1;
    // r102: render a FULL-WIDTH 40x60 block thumbnail (8x8 px blocks over
    // the 320x480 panel, 1 = any non-black pixel) into a 300-byte buffer
    // (5 bytes per row, 40 bits LSB-first). Printed every 16th change and
    // at the first; the persistent ring tail keeps the last couple so
    // host-side ASCII rendering can diff SB-enabled vs headless content
    // (status bar / dock structure vs uniform compositor fill).
    static uint8_t thumb[300];
    for (unsigned row = 0; row < 60; ++row) {
      uint64_t bits = 0;
      for (unsigned col = 0; col < 40; ++col) {
        unsigned any = 0;
        for (unsigned y = 0; y < 8 && !any; ++y) {
          const volatile uint16_t *p = px + (row * 8 + y) * 320 + col * 8;
          for (unsigned x = 0; x < 8 && !any; ++x)
            if (p[x]) any = 1;
        }
        if (any) bits |= 1ull << col;
      }
      thumb[row * 5 + 0] = (uint8_t)(bits & 0xff);
      thumb[row * 5 + 1] = (uint8_t)((bits >> 8) & 0xff);
      thumb[row * 5 + 2] = (uint8_t)((bits >> 16) & 0xff);
      thumb[row * 5 + 3] = (uint8_t)((bits >> 24) & 0xff);
      thumb[row * 5 + 4] = (uint8_t)((bits >> 32) & 0xff);
    }
    if ((n % 4) == 0 || n == 1) {
      e5_display_tracef("E5PANELSNAP %u %08x ", n, crc);
      for (unsigned i = 0; i < 300; ++i) e5_display_tracef("%02x", thumb[i]);
      e5_display_tracef("\n");
    }
    // r106: 4-bit-per-channel color histogram over a 4px grid (80x60 =
    // 4800 samples). Text = two dominant values (bg/fg); a wallpaper =
    // many distinct values with smooth counts; icons/UI = few saturated
    // clusters. Encoded as the top 8 buckets "rrrgggbb:count" in 4800.
    if ((n % 8) == 0 || n == 1) {
      static unsigned hist[16][16][16];  // 16 KiB: must NOT live on the
      // echo-thread stack (r106 kernel data abort, stack overflow).
      for (unsigned a = 0; a < 16; ++a)
        for (unsigned b = 0; b < 16; ++b)
          for (unsigned c = 0; c < 16; ++c) hist[a][b][c] = 0;
      for (unsigned y = 0; y < 480; y += 8)
        for (unsigned x = 0; x < 320; x += 4) {
          uint16_t v = px[y * 320 + x];
          unsigned r = (v >> 12) & 0xf, g = (v >> 7) & 0xf, b = (v >> 3) & 0xf;
          hist[r][g][b]++;
        }
      struct Top { unsigned rgb, cnt; } top[8] = {};
      for (unsigned a = 0; a < 16; ++a)
        for (unsigned b = 0; b < 16; ++b)
          for (unsigned c = 0; c < 16; ++c) {
            unsigned cnt = hist[a][b][c];
            for (unsigned k = 0; k < 8; ++k)
              if (cnt > top[k].cnt) {
                for (unsigned m = 7; m > k; --m) top[m] = top[m - 1];
                top[k].rgb = (a << 8) | (b << 4) | c;
                top[k].cnt = cnt;
                break;
              }
          }
      e5_display_tracef("E5PANELHIST %u", n);
      for (unsigned k = 0; k < 8; ++k)
        e5_display_tracef(" %03x:%u", top[k].rgb, top[k].cnt);
      e5_display_tracef("\n");
    }
    // r108: read the actual glyphs. Three 8x8 tiles at fixed anchors plus
    // a 16px-tall band across the full width at 4px/2px sampling; large UI
    // text (24-32px glyphs) shows letter silhouettes -> identify the
    // drawing client by WHAT the panel says.
    if ((n % 8) == 1 || (n % 32) == 0) {
      const unsigned anchors[3][2] = {{16, 240}, {160, 240}, {280, 240}};
      for (unsigned t = 0; t < 3; ++t) {
        unsigned ax = anchors[t][0], ay = anchors[t][1];
        e5_display_tracef("E5TILE %u a%u,%u ", n, ax, ay);
        for (unsigned y = 0; y < 8; ++y) {
          unsigned bits = 0;
          for (unsigned x = 0; x < 8; ++x)
            if (px[(ay + y) * 320 + ax + x]) bits |= 1u << x;
          e5_display_tracef("%02x", bits);
        }
        e5_display_tracef("\n");
      }
      e5_display_tracef("E5BAND %u y238", n);
      for (unsigned y = 238; y < 254; y += 2) {
        unsigned char bits[40];
        for (unsigned xb = 0; xb < 40; ++xb) {
          unsigned b = 0;
          for (unsigned x = 0; x < 8; ++x)
            if (px[y * 320 + xb * 8 + x]) b |= 1u << x;
          bits[xb] = (unsigned char)b;
        }
        for (unsigned xq = 0; xq < 40; ++xq) e5_display_tracef("%02x", bits[xq]);
        e5_display_tracef(" ");
      }
      e5_display_tracef("\n");
    }
  }
}

extern "C" int e5_concrete_display_probe(void) {
  int expected = 0;
  if (!__atomic_compare_exchange_n(&e5_display_construction, &expected, 1, false,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 0;
  const OSMetaClass *legacy = findClass("IOMobileFramebufferLegacy");
  const OSMetaClass *service = findClass("IOMobileFramebufferService");
  const OSMetaClass *framebuffer = findClass("IOMobileFramebuffer");
  if (!legacy) return 0;
  e5_display_original_size = (int)legacy->getClassSize();
  if (legacy != &IOMobileFramebufferLegacy::gMetaClass ||
      legacy->getClassSize() != 0x18a0 || !service || !framebuffer ||
      legacy->getSuperClass() != service || service->getSuperClass() != framebuffer ||
      framebuffer->getSuperClass() != IOService::metaClass) return 0;
  e5_display_construction |= 0x002;

  // The build removes only this owned class's automatic initializer entry.
  // Run it once, after the real family's registration, under normal locking.
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) return 0;
  e5_display_static_init();
  e5_display_construction |= 0x004;
  OSReturn result = OSMetaClass::postModLoad(handle);
  e5_display_registration_result = (int)result;
  if (result != kOSReturnSuccess) return 0;
  e5_display_construction |= 0x008;

  const OSMetaClass *owned = findClass("E5Display");
  if (!owned || owned->getSuperClass() != legacy ||
      owned->getClassSize() != e5_display_compiled_size()) return 0;
  e5_display_construction |= 0x010;
  const unsigned instances = owned->getInstanceCount();
  OSObject *object = OSMetaClass::allocClassWithName("E5Display");
  if (!object) return 0;
  e5_display_construction |= 0x020;
  IOService *display = OSDynamicCast(IOService, object);
  if (display && object->getMetaClass() == owned &&
      object->metaCast("IOMobileFramebufferLegacy") &&
      object->metaCast("IOMobileFramebufferService") &&
      object->metaCast("IOMobileFramebuffer")) {
    e5_display_construction |= 0x040;
    if (display->init()) e5_display_construction |= 0x080;
  }
  object->release();
  if (owned->getInstanceCount() == instances) e5_display_construction |= 0x100;
  e5_display_tracef("E5Display: construction v=%x size=%x registration=%x instances=%u/%u\n",
         (unsigned)e5_display_construction, (unsigned)e5_display_original_size,
         (unsigned)e5_display_registration_result, instances, owned->getInstanceCount());
  return e5_display_construction == 0x1ff;
}
