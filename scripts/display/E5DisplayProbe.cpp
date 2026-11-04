// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
// v21: fake vsync via the family completion entry. Fixed runtime VA
// (KC slide is a build constant): notify(svc, expected=[svc+0x1810]).
extern "C" {
  unsigned e5_display_vsync_ticks = 0;
  unsigned e5_display_vsync_delivered = 0;
}
using e5_notify_fn = void (*)(void *, void *);

#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <sys/proc.h>
#include <kern/task.h>

// E5TW only: printf would change the framebuffer being measured.
#include <stdarg.h>
extern "C" void e5_kputc(char);
extern "C" void *e5_display_async_reg3(void);
extern "C" unsigned e5_pmic_peek(unsigned off); // NativeSd.cpp: PMIC ADI read for wdt probe
extern "C" void e5_iomfb_trace_dump(void);
extern "C" void e5_display_state_dump(void);
extern "C" void e5_display_vsync_fire(void); // v27
extern "C" void e5_display_async_kick(void); // v29.1: arm the delivery timer
static void e5_display_tracef(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
}

// R128: a bounded process/registry snapshot after native readiness completed
// without any surface calls. No task_info/map locks, user-memory reads,
// suspension or signals from this media-read checkpoint. A proc reference
// protects each process while the pinned task's scalar state is sampled.
static void e5_display_runtime_census() {
  static unsigned sequence;
  const unsigned sample = ++sequence;
  static const char *const targets[] = {
      "SpringBoard", "backboardd", "runningboardd", "frontboardd",
      "liveactivitiesd", "launchd", "WindowServer"};
  unsigned seen = 0;
  for (int pid = 1; pid < 512; ++pid) {
    proc_t process = proc_find(pid);
    if (!process) continue;
    char name[32] = {};
    proc_name(pid, name, sizeof(name));
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
      if (strcmp(name, targets[i]) != 0) continue;
      seen |= 1u << i;
      const int exiting = proc_exiting(process);
      task_t task = exiting ? TASK_NULL : proc_task(process);
      e5_display_tracef("E5PROC seq=%u pid=%d name=%s exit=%d active=%u suspended=%u\n",
          sample, pid, name, exiting,
          task ? (unsigned)task_is_active(task) : 0,
          task ? (unsigned)task_is_app_suspended(task) : 0);
      break;
    }
    proc_rele(process);
  }
  static const char *const services[] = {
      "IOGPU", "IOAccelerator", "AppleKeyStore", "IOSurfaceRoot",
      "IOMobileFramebuffer", "IOCoreSurfaceRoot"};
  unsigned published = 0;
  for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); ++i) {
    OSDictionary *matching = i == 5 ? IOService::nameMatching(services[i])
                                   : IOService::serviceMatching(services[i]);
    if (!matching) continue;
    auto service = IOService::copyMatchingService(matching);
    if (service) published |= 1u << i;
#if !defined(IOKIT_ENABLE_SHARED_PTR)
    OSSafeReleaseNULL(service);
#endif
    matching->release();
  }
  { extern unsigned e5_quiet; if (!e5_quiet)
      e5_display_tracef("E5LIVE seq=%u pid_limit=512 seen=%02x services=%02x\n",
      sample, seen, published); }
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
extern "C" unsigned e5_display_power_last = 0xFF; // owned here
extern "C" unsigned e5_display_power_read(void);

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
  if ((traceKicks++ % 16) == 0) {
    { extern unsigned e5_quiet; if (!e5_quiet) e5_iomfb_trace_dump(); }
    e5_display_runtime_census();
  }
  e5_display_state_dump(); // v16: wait-machinery state alongside the selector census
  e5_display_vsync_fire();  // v27: fire the armed vsync event (token bump + clear)
  e5_display_async_kick();   // v29.1: arm the delivery timer (fire moved off the echo thread)
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
  __atomic_store_n(&e5_display_power_last, e5_display_power_read(), __ATOMIC_RELAXED);
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
  {
    extern void *e5_display_service_instance();
    void *svc = e5_display_service_instance();
    if (svc) {
      __atomic_fetch_add(&e5_display_vsync_ticks, 1, __ATOMIC_RELAXED);
      void *expected = *(void **)((uint8_t *)svc + 0x1810);
      if (expected) {
        ((e5_notify_fn)0xfffffff006798018ull)(svc, expected);
        __atomic_fetch_add(&e5_display_vsync_delivered, 1, __ATOMIC_RELAXED);
      }
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

// ---------------------------------------------------------------------------
// D2 v1: native DPU takeover probe + paint + backlight modulation.
// The plan (display-touch-gpu-plan.md D2) wants E5-owned scanout BEFORE the
// GPU phase: continuous dynamic output and panel/backlight control, with the
// UEFI-programmed pipeline as the starting point. All register facts come
// from the M0 live decode (docs/m0-bringup-report.md): DPU core
// 0x31000000+0x3000 (PANEL_SIZE@0x18, LAYER_ENABLE@0x2c, LAY0_BASE@0x30,
// LAY0_CTRL@0x40, LAY0_PITCH@0x4c), PWM ch0 at 0x30000 (PRESCALE@0, MOD@4,
// DUTY@8, ENABLE@0x18, 16KiB channel stride, pwm-sprd.c).
// ---------------------------------------------------------------------------
static IODeviceMemory *sDpuMemory = nullptr;
static IOMemoryMap *sDpuMap = nullptr;
static IODeviceMemory *sPwmMemory = nullptr;
static IOMemoryMap *sPwmMap = nullptr;
static uint32_t sPwmOrigDuty = 0;
static unsigned sPaintTicks = 0;
// D2 v2: poll DPU vsync (REG_DPU_INT_STS @0x1E8 bit5, Linux sprd_dpu.c
// BIT_DPU_INT_VSYNC). A ~60Hz pulse here proves the scanout engine runs
// and frame-locks the SwapWait completion path.
extern "C" unsigned e5_display_vsync_edges(void);
// D3 v0: I2C3 controller probe. i2c@2270000 is a child of ap-ahb whose
// ranges are <0 0 0x20000000 0x22f0000> (live-fdt) -> controller PA
// 0x22270000. tlsc6x@2e (X320 Y480, irq gpio13, reset gpio14) rides it.
// Android used this bus until the handoff: first probe reads VERSION with
// NO gate/clock/pinmux touches to test the hot-state shortcut.
static IODeviceMemory *sI2cMemory = nullptr;
static IOMemoryMap *sI2cMap = nullptr;
static unsigned sI2cStage = 0;
static unsigned sI2cVersion = 0xeeeeeeee, sI2cProbeStatus = 0xeeeeeeee;

static volatile uint32_t *i2cReg(unsigned off) {
  return (volatile uint32_t *)((uint8_t *)sI2cMap->getVirtualAddress() + off);
}

// apahb gate block (live-fdt apahb-gate reg <0x0 0x3000> under ap-ahb
// ranges -> PA 0x20003000). ums9621-clk.c: i2c3_eb = SC gate BIT(13) with
// set-mirror at +0x1000 (write-1-to-set 0x20004000; status in 0x20003000).
// r145 proved the controller answers nothing while gated (data abort on the
// first register read), so the gate must be armed BEFORE any i2c access.
static IODeviceMemory *sGateMemory = nullptr;
static IOMemoryMap *sGateMap = nullptr;
static unsigned sGateStatusBefore = 0xeeeeeeee, sGateStatusAfter = 0xeeeeeeee;

static volatile uint32_t *gateReg(unsigned off) {
  return (volatile uint32_t *)((uint8_t *)sGateMap->getVirtualAddress() + off);
}

extern "C" void e5_i2c3_tick(unsigned now) {
  if (!sGateMap) {
    sGateMemory = IODeviceMemory::withRange(0x20003000ull, 0x2000ull);
    if (sGateMemory) sGateMap = sGateMemory->map();
    if (!sGateMap) return;
  }
  if (sI2cStage == 0) {
    sGateStatusBefore = *gateReg(0x0);
    *gateReg(0x1000) = (1u << 13); // set-mirror: i2c3_eb on
    for (int i = 0; i < 50; ++i) { __asm__ volatile("" ::: "memory"); }
    sGateStatusAfter = *gateReg(0x0);
    if (sGateStatusAfter & (1u << 13)) {
      sI2cMemory = IODeviceMemory::withRange(0x22270000ull, 0x1000ull);
      if (sI2cMemory) sI2cMap = sI2cMemory->map();
    }
    sI2cStage = 1;
    return;
  }
  if (!sI2cMap) {
    if ((now % 5000) == 30)
      e5_display_tracef("E5I2C gate %x->%x (i2c3_eb still off)",
                        sGateStatusBefore, sGateStatusAfter);
    return;
  }
  if (sI2cStage == 1) {
    sI2cVersion = *i2cReg(0x1c); // I2C_VERSION
    sI2cStage = 2;
    return;
  }
  if (sI2cStage == 2 && (now % 5000) == 20) {
    // 0-byte write probe of the TLSC6x address: ADDR_CFG=0x2e<<1,
    // COUNT=0, START. STATUS readback captures ACK/NACK state bits.
    uint32_t ctl = *i2cReg(0x00);
    *i2cReg(0x04) = (0x2e << 1);
    *i2cReg(0x08) = 0;
    *i2cReg(0x00) = (ctl | 1u) & ~(1u << 20);
    for (int i = 0; i < 200; ++i) { __asm__ volatile("" ::: "memory"); }
    sI2cProbeStatus = *i2cReg(0x14);
    *i2cReg(0x00) = ctl | (1u << 20); // STP releases the bus
    sI2cStage = 3;
  }
  if ((now % 5000) == 30)
    e5_display_tracef("E5I2C gate %x->%x ver=%x st=%x", sGateStatusBefore,
                      sGateStatusAfter, sI2cVersion, sI2cProbeStatus);
}

static bool sVsyncArmed = false;
static uint32_t sVsyncEdges = 0, sVsyncLastSts = 0;
extern "C" unsigned e5_display_vsync_edges(void) { return sVsyncEdges; }

static volatile uint32_t *dpuReg(unsigned off) {
  if (!sDpuMap) return nullptr;
  return (volatile uint32_t *)((uint8_t *)sDpuMap->getVirtualAddress() + off);
}
static volatile uint32_t *pwmReg(unsigned off) {
  if (!sPwmMap) return nullptr;
  return (volatile uint32_t *)((uint8_t *)sPwmMap->getVirtualAddress() + off);
}

// Paint the M0 acceptance pattern into the live scanout window: red / green
// / blue / white horizontal bands with a black vertical stripe whose x
// position rotates — one stripe step per call. Called from the delivery
// timer (thread context, never the echo thread).
extern "C" void e5_dpu_paint_step(void) {
  // r173: paint DISABLED — the D2 acceptance round (r144) proved the kernel
  // scanout path; the panel must now show the COMPOSITOR's output (pool B
  // benchmark profile). Keep the tick counter for telemetry only.
  ++sPaintTicks;
  return;
#if 0
  if (!sPanelMap) return;
  volatile uint16_t *px = (volatile uint16_t *)sPanelMap->getVirtualAddress();
  const unsigned W = 320, H = 480;
  // r141: tick rate ~1.8kHz means %8 = 4.4ms per stripe step - invisible.
  // r151: work now runs ~20Hz (50ms wall throttle) instead of 1.8kHz.
  // Stripe step every 20 ticks = 1s per step; same visual cadence as the
  // r144-accepted pattern.
  unsigned stripe = ((sPaintTicks++ / 20) % 8) * 40;
  for (unsigned y = 0; y < H; ++y) {
    uint16_t band = (y < 120) ? 0xF800 : (y < 240) ? 0x07E0 : (y < 360) ? 0x001F : 0xFFFF;
    for (unsigned x = 0; x < W; ++x) px[y * W + x] = (x >= stripe && x < stripe + 40) ? 0x0000 : band;
  }
#endif
}

extern "C" void e5_dpu_tick(void) {
  // first entry: map + dump + take a baseline of the backlight duty
  if (!sDpuMap) {
    sDpuMemory = IODeviceMemory::withRange(0x31000000ull, 0x3000ull);
    if (sDpuMemory) sDpuMap = sDpuMemory->map();
    // pwm@30000 is a CHILD of /soc/aon whose ranges make children OFFSETS of
    // 0x64000000 (same translation the ADI rounds taught us) — absolute PA of
    // PWM channel 0 is 0x64030000; mapping 0x30000 directly = data abort
    // (r137 panic, E5DPU dump had already proven the DPU side).
    sPwmMemory = IODeviceMemory::withRange(0x64030000ull, 0x20ull);
    if (sPwmMemory) sPwmMap = sPwmMemory->map();
    if (sDpuMap) {
      e5_display_tracef(
          "E5DPU id=%03x ctrl=%x panel=%08x layen=%x base=%08x layctrl=%x pitch=%03x",
          *dpuReg(0x000) & 0xFFF, *dpuReg(0x008), *dpuReg(0x018),
          *dpuReg(0x02c), *dpuReg(0x030), *dpuReg(0x040), *dpuReg(0x04c));
    } else {
      e5_display_tracef("E5DPU map failed");
    }
    if (sPwmMap) {
      sPwmOrigDuty = *pwmReg(0x08) & 0xFFFF;
      e5_display_tracef("E5PWM prescale=%x mod=%x duty=%x enable=%x",
                           *pwmReg(0x0), *pwmReg(0x4), sPwmOrigDuty,
                           *pwmReg(0x18));
    } else {
      e5_display_tracef("E5PWM map failed");
    }
  }
  // continuous dynamic output: step the paint pattern; every 8th tick also
  // pulse the backlight to 1/4 duty and back (panel/backlight acceptance)
  e5_dpu_paint_step();
  // r148: i2c tick disabled (r147 regression bisect: keep D2 paint only)

  if (sDpuMap) {
    volatile uint32_t *en =
        (volatile uint32_t *)sDpuMap->getVirtualAddress() + 0x1E0 / 4;
    volatile uint32_t *clr =
        (volatile uint32_t *)sDpuMap->getVirtualAddress() + 0x1E4 / 4;
    volatile uint32_t *sts =
        (volatile uint32_t *)sDpuMap->getVirtualAddress() + 0x1E8 / 4;
    if (!sVsyncArmed) {
      *clr = 0xffu;
      *en |= 0x20u;
      // r143: ctrl=0 (STOPPED) with zero vsync edges the whole window -
      // the firmware handoff left the DPU not scanning. Linux
      // sprd_dpu_run() = REG_DPU_CTRL(0x4) |= BIT_DPU_RUN(bit0); resume
      // the firmware-era scanout (layer base 0xbe000000 = logo buffer).
      volatile uint32_t *ctl =
          (volatile uint32_t *)sDpuMap->getVirtualAddress() + 0x04 / 4;
      *ctl |= 0x01u;
      sVsyncArmed = true;
    }
    uint32_t cur = *sts;
    if ((cur & 0x20u) && !(sVsyncLastSts & 0x20u)) sVsyncEdges++;
    if (cur) *clr = cur;
    sVsyncLastSts = cur;
  }

  // v1.3: 5s cadence kept but the line is minimal and self-verifying:
  // row0 crc computed right after paint proves the stripe advanced IN the
  // surviving line itself (r139's F= evidence was flooded out of the ring);
  // wdt peek (PMIC ADI cnt@0x50/ctrl@0x48 via NativeSd helper) tests the
  // r139 anomaly where ticks spanned >600s without a watchdog reboot.
  // r140 measured the real tick rate at ~1.8 kHz (thread_call interval
  // unit mismatch vs assumption): %50 printed ~40 Hz and flooded the ring
  // again. %5000 = one line every ~3 s.
  if ((sPaintTicks % 100) == 10) { // every ~5s at 20Hz
    unsigned rowCrc = 0;
    if (sPanelMap) {
      volatile uint16_t *px =
          (volatile uint16_t *)sPanelMap->getVirtualAddress();
      for (unsigned x = 0; x < 320; ++x) rowCrc = rowCrc * 31 + px[x];
    }
    if (sDpuMap) {
      volatile uint32_t *ctl2 =
          (volatile uint32_t *)sDpuMap->getVirtualAddress() + 1;
      extern unsigned e5_display_fire_calls;
      extern int e5_display_gate_last;
      extern void *e5_display_async_node3(void);
      void *n3 = e5_display_async_node3();
      unsigned long long *w3 = (unsigned long long *)n3;
      { unsigned long long *rw = (unsigned long long *)e5_display_async_reg3();
        e5_display_tracef("E5REG3 %p %llx %llx %llx %llx", rw ? rw[0] : 0, rw ? rw[1] : 0, rw ? rw[2] : 0, rw ? rw[3] : 0, rw ? rw[4] : 0); }
      { extern unsigned e5_quiet; if (!e5_quiet)
            e5_display_tracef("E5DPU t=%u ctl=%x duty=%x crc=%08x v=%u wdt=%x af=%u gl=%d n3=%p %llx %llx %llx %llx %llx %llx",
          sPaintTicks, *ctl2,
          sPwmMap ? (*pwmReg(0x08) & 0xFFFF) : 0, rowCrc, sVsyncEdges,
          e5_pmic_peek(0x48), e5_display_fire_calls, e5_display_gate_last,
          n3, w3 ? w3[0] : 0, w3 ? w3[1] : 0, w3 ? w3[2] : 0,
          w3 ? w3[3] : 0, w3 ? w3[4] : 0, w3 ? w3[5] : 0); }
    }
  }
  if (sPwmMap && sPwmOrigDuty) {
    // r173: backlight held at the firmware's original duty (no pulsing) —
    // stable brightness for the compositor-output visual round.
    *pwmReg(0x08) = sPwmOrigDuty;
  }
}
