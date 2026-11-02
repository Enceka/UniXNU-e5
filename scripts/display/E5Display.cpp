// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Published lifecycle probe. Hardware initialization remains unqualified.
#include "OriginalLegacyFramebufferABI.h"
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>

extern "C" IOReturn e5_display_backend_soft(unsigned slot, uint32_t selector,
                                            void *b, void *c, void *d);
extern "C" unsigned e5_display_open_count;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" {
unsigned e5_display_client_calls = 0;
unsigned e5_display_client_successes = 0;
unsigned e5_display_client_type = 0;
unsigned e5_display_client_status = (unsigned)kIOReturnNotReady;
unsigned e5_display_pitch_calls = 0;
unsigned e5_display_pitch_status = (unsigned)kIOReturnNotReady;
unsigned e5_display_lifecycle = 0;
unsigned e5_display_resources = 0;
unsigned e5_display_cap213_calls = 0;
unsigned e5_display_cap213_status = (unsigned)kIOReturnNotReady;
unsigned e5_display_ok211_calls = 0;
unsigned e5_display_ok306_calls = 0;
// v15 power-band override counters (slots 229/255/256/258/260/261).
unsigned e5_display_pw_calls = 0;
extern "C" unsigned e5_display_swap_mask = 0;
extern "C" unsigned e5_display_wait_calls = 0;
extern "C" unsigned e5_display_wait_last = 0;
unsigned e5_display_pw_results = 0;
// v14 runtime bracket of the family power byte at +0x8c (see newUserClient).
unsigned e5_display_power_before_nuc = 0xFF;
unsigned e5_display_power_after_nuc = 0xFF;
}
extern "C" unsigned e5_display_vsync_fires = 0; // v30 observed native completion transitions
extern "C" unsigned e5_display_wait_status = 0;
extern "C" unsigned e5_display_event_calls = 0;
extern "C" unsigned long long e5_display_event_argument = 0;
extern "C" unsigned e5_display_event_status = 0;
extern "C" unsigned e5_display_power_last; // owned by E5DisplayProbe.cpp
// v28: the real async-event delivery. IOUserClient::sendAsyncResult64WithOptions
// is a protected static of IOUserClient (iokit/IOKit/IOUserClient.h:341); the
// family reaches it through a GOT thunk. Bind the mangled export directly.
extern "C" int _ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj(
    unsigned long long *asyncRef, int result, const unsigned long long *args,
    unsigned int numArgs, unsigned int options);
extern "C" unsigned e5_display_async_kinds = 0; // bitmask of registered subops
extern "C" unsigned e5_display_async_sends = 0; // deliveries attempted
extern "C" unsigned e5_display_async_send_ok = 0; // deliveries returning 0
extern "C" unsigned e5_display_async_ports = 0;
extern "C" unsigned e5_display_async_last = 0;
extern "C" unsigned e5_display_async_armed = 0;

// The published instance whose +0x8c byte the family power dispatcher gates
// on. Set by start(); read by e5_display_power_read() from the echo probe.
class E5Display;
static E5Display *sPowerInstance = nullptr;

extern "C" unsigned e5_display_power_read(void) {
  if (!sPowerInstance) return 0xFF;
  return *(volatile uint8_t *)((uint8_t *)sPowerInstance + 0x8c);
}

extern "C" void *e5_display_service_instance() { return sPowerInstance; }

// v29: x2 of slot 208 is an event-list node, NOT an async reference.
// The pinned registration action at raw 0x6795ec0 initializes the reference
// at node+0x18 and links the node at service+0x1588+kind*16. Use that native
// list, including native unregistration, under its command gate. A media
// read must never block acquiring the display gate: display setup can itself
// need a media read. attemptAction skips that sample if the gate is busy.
static IOReturn e5_display_async_action(OSObject *owner, void *, void *, void *, void *) {
  uint8_t *base = (uint8_t *)owner;
  const unsigned long long now = mach_absolute_time();
  for (unsigned kind = 3; kind <= 5; kind += 2) {
    uint8_t *node = *(uint8_t **)(base + 0x1588 + kind * 16);
    for (unsigned visited = 0; node && visited < 64; ++visited) {
      unsigned long long *ref = (unsigned long long *)(node + 0x18);
      if (*ref) {
        __atomic_fetch_or(&e5_display_async_ports, 1u << kind, __ATOMIC_RELAXED);
        // FrameInfoNotifyFunc accepts {property-mask, status}; an empty mask
        // reports no changed properties. VsyncNotifyFunc requires SIX words:
        // three u64 values split low/high, as in the original broadcaster
        // at raw 0x6796720. The v28 two-word timestamp could overrun that
        // callback's input and must never be used for kind 5.
        unsigned long long payload[6] = {0, 0, 0, 0, 0, 0};
        unsigned count = 2;
        if (kind == 5) {
          payload[0] = (uint32_t)now;
          payload[1] = now >> 32;
          payload[2] = (uint32_t)now;
          payload[3] = now >> 32;
          count = 6;
        }
        int r = _ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj(
            ref, 0, payload, count, 1);
        __atomic_add_fetch(&e5_display_async_sends, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&e5_display_async_last, (unsigned)r, __ATOMIC_RELAXED);
        if (r == 0) __atomic_add_fetch(&e5_display_async_send_ok, 1U, __ATOMIC_RELAXED);
      }
      node = *(uint8_t **)node;
    }
  }
  return kIOReturnSuccess;
}
extern "C" void e5_display_async_fire(void) {
  if (!sPowerInstance) return;
  auto gate = *(IOCommandGate **)((uint8_t *)sPowerInstance + 0x1608);
  if (gate) gate->attemptAction(e5_display_async_action);
}
// v30: observe native token/completion transitions. The old v27 implementation
// wrote the token itself, while its slot-176 ABI did not even inspect the
// flags argument. Preserve the original slot-344 event source and its state.
extern "C" void e5_display_vsync_fire(void) {
  void *svc = sPowerInstance;
  if (!svc) return;
  const uint8_t *base = (const uint8_t *)svc;
  static uint32_t previous_token, previous_enabled;
  uint32_t token = *(volatile const uint32_t *)(base + 0x16d0);
  uint32_t enabled = *(volatile const uint32_t *)(base + 0x16e0);
  if (token != previous_token || (previous_enabled && !enabled))
    __atomic_add_fetch(&e5_display_vsync_fires, 1U, __ATOMIC_RELAXED);
  previous_token = token;
  previous_enabled = enabled;
}

// v16: one-line dump of the family wait/swap machinery state so the host can
// see which gate is closed (members: +0x3c8 slot-213 cap, +0x1608 power helper,
// +0x1640 provider cache, +0x1658 wait sub-object, +0x1810 waiter vtable,
// +0x1818 waiter out-object, +0x1858 pending object, +0x1860/+0x1861 flags,
// +0x1868 lock, +0x16d0 reader-A object, +0x3c4 last power arg).
extern "C" void e5_kputc(char);
static void e5_state_log(const char *format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
  e5_kputc('\n');
}
extern "C" void e5_display_state_dump(void) {
  if (!sPowerInstance) return;
  const volatile uint8_t *b = (const volatile uint8_t *)sPowerInstance;
  auto q = [&](unsigned off) -> unsigned long long {
    unsigned long long v = 0;
    for (unsigned i = 0; i != 8; ++i) v |= (unsigned long long)b[off + i] << (8 * i);
    return v;
  };
  e5_state_log("E5STATE svc=%p m3c8=%llx m1608=%llx m1640=%llx m1658=%llx m1810=%llx m1858=%llx f1860=%02x%02x m1868=%llx m16d0=%llx e16e0=%02x vfire=%u ak=%02x asend=%u aok=%u ap=%02x ar=%x am=%02x tok=%x ev=%u:%llx:%x wr=%x",
         (const void *)b, q(0x3c8), q(0x1608), q(0x1640), q(0x1658),
         q(0x1810), q(0x1858), b[0x1860], b[0x1861], q(0x1868), q(0x16d0), b[0x16e0], e5_display_vsync_fires,
         e5_display_async_kinds, e5_display_async_sends, e5_display_async_send_ok,
         e5_display_async_ports, e5_display_async_last, e5_display_async_armed,
         *(volatile const uint32_t *)(b + 0x16d0), e5_display_event_calls,
         e5_display_event_argument, e5_display_event_status, e5_display_wait_status);
}

class E5Display : public IOMobileFramebufferLegacy {
  OSDeclareDefaultStructors(E5Display)
  bool familyStarted = false;
  bool bootValidated = false;
public:
  bool start(IOService *provider) override {
    __atomic_fetch_or(&e5_display_lifecycle, 0x001U, __ATOMIC_RELAXED);
    if (!provider || OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface") != 0)
      return false;
    OSDictionary *match = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
    if (match) match->release();
    if (!surface) return false;
    surface->release();
    __atomic_fetch_or(&e5_display_lifecycle, 0x400U, __ATOMIC_RELAXED);
    if (!IOMobileFramebufferLegacy::start(provider)) return false;
    // Power-change support contract: the original Legacy::start clears the
    // family's power-support byte (family __TEXT_EXEC raw 0xfffffff006787e58,
    // `strb wzr,[x8,#0x8c]`) and NOTHING inside the family ever sets it again
    // (sole writer). Every power/readiness path gates on its bit0:
    //   - request_power_change dispatcher 0xfffffff0067779fc returns
    //     0xE00002C7 while clear, and only calls the power backend
    //     (vtable slot 229 -> the +0x1608 sub-object the same start chain
    //     allocated at 0xfffffff006786f0c) when it is set;
    //   - the swap readiness check at 0xfffffff006792148 returns
    //     0xE00002BC (kIOReturnNotReady) while clear.
    // A concrete platform driver arms the byte once its power backend
    // exists. Ours is the inherited firmware scanout: UEFI already powered
    // the panel, so a power-state request completes without extra work.
    *(volatile uint8_t *)((uint8_t *)this + 0x8c) |= 1U;
    // v22: the original start caches its AppleARMIODevice provider at +0x1640
    // and bails early ("provider was not AppleARMIODevice") when the nub is a
    // plain IOService — leaving +0x1640 NULL. Family internals (DefaultSurface
    // creation 0x6792804 reads [this+0x1640] as x0 for the provider EDT calls)
    // then see NULL. Cache our lcd nub there directly.
    *(IOService * volatile *)((uint8_t *)this + 0x1640) = provider;
    // v23: display-online state the concrete driver owns: +0x1788 bit0 =
    // panel online (readers 0x678b60c/0x678b748/0x678cd7c all skip when clear;
    // the DefaultSurface fn 0x6792804 clears it at entry then gates on it);
    // +0x16d4 nonzero = mode/format known (reader 0x678b624 skips when 0).
    // v24: r120 showed bit0 SET routes fn 0x6792804 into the EDT-override
    // reads (token appeared) but not surface construction. tbz bit0 -> 0x6792958
    // = the OSObject surface-setup path. A/B: keep the bit CLEAR, keep +0x16d4.
    *(volatile uint8_t *)((uint8_t *)this + 0x1788) &= (uint8_t)~1U;
    *(volatile uint32_t *)((uint8_t *)this + 0x16d4) = 1;
    sPowerInstance = this; // v14: let the echo probe poll the +0x8c byte.
    __atomic_fetch_or(&e5_display_lifecycle, 0x20000U, __ATOMIC_RELAXED);
    __atomic_fetch_or(&e5_display_lifecycle, 0x010U, __ATOMIC_RELAXED);
    unsigned resources = lifecycleResourceMask();
    __atomic_store_n(&e5_display_resources, resources, __ATOMIC_RELAXED);
    if (!bootValidated || resources != 0x3f) {
      __atomic_fetch_or(&e5_display_lifecycle, 0x100U, __ATOMIC_RELAXED);
      IOMobileFramebufferLegacy::stop(provider);
      bootValidated = false;
      return false;
    }
    __atomic_fetch_or(&e5_display_lifecycle, 0x020U, __ATOMIC_RELAXED);
    familyStarted = true;
    return true;
  }
  void stop(IOService *provider) override {
    familyStarted = false;
    bootValidated = false;
    __atomic_fetch_or(&e5_display_lifecycle, 0x200U, __ATOMIC_RELAXED);
    IOMobileFramebufferLegacy::stop(provider);
  }
  // The original start calls virtual registerService before it returns.
  // Publication is released by e5_display_publish only after the entire
  // start chain and its lock/gate/workloop checks have completed.
  void registerService(IOOptionBits options = 0) override {
    if (!familyStarted) {
      __atomic_fetch_or(&e5_display_lifecycle, 0x040U, __ATOMIC_RELAXED);
      return;
    }
    __atomic_fetch_or(&e5_display_lifecycle, 0x080U, __ATOMIC_RELAXED);
    IOService::registerService(options);
  }
  bool isMainDisplay() override { return bootValidated; }
  // D1 inherits firmware scanout. This hook validates that exact handoff
  // and installs its geometry; it does not reprogram the DPU or claim a
  // surface mapping, frame transfer, power transition or vblank.
  bool startHardware() override {
    __atomic_fetch_or(&e5_display_lifecycle, 0x002U, __ATOMIC_RELAXED);
    PE_Video video = {};
    if (!getProvider() || PE_current_console(&video) ||
        video.v_baseAddr != 0xbe000000ULL || video.v_width != 320 ||
        video.v_height != 480 || video.v_rowBytes != 640 ||
        video.v_depth != 16 || video.v_rotate != 0) return false;
    __atomic_fetch_or(&e5_display_lifecycle, 0x004U, __ATOMIC_RELAXED);
    installBootDimensions(320, 480);
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    if (width != 320 || height != 480) return false;
    bootValidated = true;
    __atomic_fetch_or(&e5_display_lifecycle, 0x008U, __ATOMIC_RELAXED);
    return true;
  }
  // IOService::open accounting only; IOServiceOpen need not call handleOpen.
  bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) override {
    __atomic_fetch_add(&e5_display_open_count, 1U, __ATOMIC_RELAXED);
    return IOService::handleOpen(forClient, options, arg);
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type,
                        IOUserClient **client) override {
    __atomic_fetch_add(&e5_display_client_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_client_type, type, __ATOMIC_RELAXED);
    IOReturn status = IOMobileFramebufferLegacy::newUserClient(task, security, type, client);
    // v14: the family power-support byte at +0x8c is armed in start() and
    // re-cleared by *something* between start and the first power dispatch
    // (r110: selectors return 0xE00002C7 although start armed bit0). Read it
    // before and after the original newUserClient to bracket the clear.
    __atomic_store_n(&e5_display_power_before_nuc,
                     (unsigned)*(volatile uint8_t *)((uint8_t *)this + 0x8c),
                     __ATOMIC_RELAXED);
    if (status == kIOReturnSuccess && client && *client)
      __atomic_fetch_add(&e5_display_client_successes, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_client_status, (unsigned)status, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_power_after_nuc,
                     (unsigned)*(volatile uint8_t *)((uint8_t *)this + 0x8c),
                     __ATOMIC_RELAXED);
    return status;
  }
  // Original slot 188 takes one uint32_t output and returns an IOReturn.
  // Its cached AppleARMIODevice (+0x1640) is absent on our generic lcd nub.
  // Use the registry edge instead. Do not invent a pitch from panel geometry:
  // the provider has no qualified dot-pitch encoding yet.
  IOReturn getDotPitch(uint32_t *pitch) override {
    __atomic_fetch_add(&e5_display_pitch_calls, 1U, __ATOMIC_RELAXED);
    IOReturn status = kIOReturnBadArgument;
    if (pitch) {
      *pitch = 0;
      IOService *provider = getProvider();
      status = kIOReturnNotReady;
      if (provider) {
        auto retained = provider->copyProperty("dot-pitch");
#if defined(IOKIT_ENABLE_SHARED_PTR)
        OSObject *property = retained.get();
#else
        OSObject *property = retained;
#endif
        status = kIOReturnNotFound;
        if (property) {
          OSData *data = OSDynamicCast(OSData, property);
          status = kIOReturnBadArgument;
          if (data && data->getLength() == sizeof(*pitch)) {
            memcpy(pitch, data->getBytesNoCopy(), sizeof(*pitch));
            status = kIOReturnSuccess;
          }
        }
#if !defined(IOKIT_ENABLE_SHARED_PTR)
        OSSafeReleaseNULL(property);
#endif
      }
    }
    __atomic_store_n(&e5_display_pitch_status, (unsigned)status, __ATOMIC_RELAXED);
    return status;
  }
#define E5_GUARDED_SLOT(n) \
  IOReturn opaque##n(uint32_t selector, void *b, void *c, void *d) override { \
    return e5_display_backend_soft(n, selector, b, c, d); \
  }
  // Slot 213 (r89: the walk's single gate, selector 0): the family wrapper at
  // 0x6778688 passes (&out1, structureOutput, &out2) and caches out2 at
  // service+0x3c8. Answer success with a minimal capability pair so the
  // backboardd open sequence can advance past the first backend query.
  IOReturn opaque213(uint32_t selector, void *b, void *c, void *d) override {
    (void)selector;
    (void)c;
    __atomic_fetch_add(&e5_display_cap213_calls, 1U, __ATOMIC_RELAXED);
    IOReturn status = kIOReturnBadArgument;
    if (b && d) {
      *static_cast<uint32_t *>(b) = 1;
      *static_cast<uint32_t *>(d) = 1;
      status = kIOReturnSuccess;
    }
    __atomic_store_n(&e5_display_cap213_status, (unsigned)status, __ATOMIC_RELAXED);
    return status;
  }
  // Slot 211 (r90: generic 3-scalar backend entry, sel=3) and slot 306
  // (r90: set/clear boolean pair at 0x6796094/0x67963e4) answered with
  // success to reveal the next walk gate; the other guards stay loud.
  IOReturn opaque211(uint32_t kind, void *node, uint32_t on) override {
    (void)node;
    if (kind < 6) {
      if (on) __atomic_fetch_or(&e5_display_async_armed, 1u << kind, __ATOMIC_RELAXED);
      else __atomic_fetch_and(&e5_display_async_armed, ~(1u << kind), __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&e5_display_ok211_calls, 1U, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque306(uint32_t selector, void *b, void *c, void *d) override {
    (void)selector; (void)b; (void)c; (void)d;
    __atomic_fetch_add(&e5_display_ok306_calls, 1U, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  // v15: the family power band. The inherited Legacy stubs make
  // request_power_change reject every caller (r109/r110 telemetry:
  // SwapWait >= 11911 x Busy, RequestPowerChange Unsupported). The
  // firmware scanout is already powered: apply = acknowledge.
  IOReturn opaque229(uint32_t state, void *zero) override {
    (void)state; (void)zero;
    __atomic_fetch_add(&e5_display_pw_calls, 1U, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque255() override { // get power state: dispatcher tests bit0 (1 = on)
    __atomic_fetch_add(&e5_display_pw_calls, 1U, __ATOMIC_RELAXED);
    return 1; // panel is powered by firmware scanout: state = ON
  }
  IOReturn opaque256(uint32_t state) override { // set power state
    (void)state;
    __atomic_fetch_add(&e5_display_pw_calls, 1U, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  bool opaque258() override { // power controllable
    __atomic_fetch_add(&e5_display_pw_calls, 1U, __ATOMIC_RELAXED);
    return true;
  }
  // Preserve the native gate, setAsyncReference64, list insertion and removal.
  // x3 is the registration record {client, reserved, callback, refcon}.
  IOReturn opaque208(uint32_t kind, void *node, void *registration) override {
    IOReturn status = IOMobileFramebufferLegacy::opaque208(kind, node, registration);
    if (status == kIOReturnSuccess && kind < 6 && node) {
      __atomic_fetch_or(&e5_display_async_kinds, 1u << kind, __ATOMIC_RELAXED);
    }
    return status;
  }
  IOReturn opaque210(uint32_t kind, void *node, uint32_t on) override {
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 8, __ATOMIC_RELAXED);
    return IOMobileFramebufferLegacy::opaque210(kind, node, on);
  }
  // Original slot 336 defaults to true, skipping setAsyncReference64 for
  // FrameInfo (kind 3) in favor of an unimplemented shared-queue backend.
  // Select the family's existing Mach-notification fallback explicitly.
  bool opaque336() override {
    return false;
  }
  // The real enable_event entry is slot 344, not the unrelated 260/261
  // Legacy stubs. Retain the native argument, 32-bit enable word, timer
  // arming and native return status; report calls without forcing readiness.
  IOReturn opaque344(uint64_t token, void *context) override {
    __atomic_fetch_add(&e5_display_event_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_event_argument, token, __ATOMIC_RELAXED);
    IOReturn status = IOMobileFramebufferLegacy::opaque344(token, context);
    __atomic_store_n(&e5_display_event_status, (unsigned)status, __ATOMIC_RELAXED);
    return status;
  }
  IOReturn opaque176(void *client, uint32_t swap, uint32_t flags, uint32_t timeout) override {
    __atomic_fetch_add(&e5_display_wait_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_wait_last, (swap & 0xFFFFu) | ((flags & 0xFFu) << 16) | ((timeout & 0xFFu) << 24), __ATOMIC_RELAXED);
    IOReturn status = IOMobileFramebufferLegacy::opaque176(client, swap, flags, timeout);
    __atomic_store_n(&e5_display_wait_status, (unsigned)status, __ATOMIC_RELAXED);
    return status;
  }
  // v18: the swap/surface band. The Legacy base returns kIOReturnUnsupported
  // from every one of these (r114: sel 15/22/27/33/50/57/79/84 all rejected);
  // the concrete platform driver owns them. Firmware-scanout semantics: every
  // surface/swap operation succeeds as a no-op (the DPU scans 0xBE000000
  // continuously; buffers written by the compositor ARE the scanout).
  IOReturn opaque180(uint32_t a, uint32_t b, void *out) override {
    (void)a; (void)b; if (out) *(uint32_t *)out = 0;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 0, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque183(uint32_t a) override {
    (void)a;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 1, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque199(void *out) override {
    if (out) *(uint64_t *)out = 0; // 3084-byte info struct: zero head only
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 2, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque206(void *a) override {
    (void)a;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 3, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque207(uint32_t a) override {
    (void)a;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 4, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque232(uint32_t a) override {
    (void)a;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 5, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  // v19: r115 leftovers — sel 84 -> slot 201 (w1, ptr), sel 79 -> slot 224
  // (the set-block marshaling: client-handles + counts). Base Legacy returns
  // Unsupported from both. No-op success.
  IOReturn opaque201(uint32_t a, void *b) override {
    (void)a; (void)b;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 6, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque224(void *a, uint32_t b, uint64_t c, void *d, uint32_t e,
                     void *f, uint32_t g) override {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 7, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  // v20: r116 leftovers — sel 17 -> slot 200 (0x640/8; the 3084-byte
  // display-info struct query), sel 84 -> slot 203 (0x658/8; earlier
  // miscategorized as 207 — 0x658/8 IS 203, an unoverridden Unsupported
  // stub). Zero-fill + success.
  IOReturn opaque200(void *out) override {
    if (out) {
      uint64_t *q = (uint64_t *)out;
      for (unsigned i = 0; i < 384; i++) q[i] = 0; // 3072B of the 3084B head
    }
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 8, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  IOReturn opaque203(void *a) override {
    (void)a;
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 9, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
  }
  E5_GUARDED_SLOT(245)
  E5_GUARDED_SLOT(246)
  E5_GUARDED_SLOT(307)
  E5_GUARDED_SLOT(337)
  E5_GUARDED_SLOT(338)
  E5_GUARDED_SLOT(345)
#undef E5_GUARDED_SLOT
};

OSDefineMetaClassAndStructors(E5Display, IOMobileFramebufferLegacy)
static_assert(sizeof(E5Display) == 0x18a8, "Only owned lifecycle flags extend Legacy");

extern "C" unsigned e5_display_compiled_size(void) { return sizeof(E5Display); }

// V8 attach phase. Runs after the construction probe reached 0x1ff on the
// same boot. Bits in e5_display_attach (field owned by NativeSd.cpp):
//   0x400  lcd provider nub handed in
//   0x800  E5Display factory allocation
//   0x1000 init() on the fresh instance
//   0x2000 attach(provider) entered (IOService::attach keeps no status)
//   0x4000 start(provider) returned true
//   0x8000 registerService() issued
//   0x10000 service visible via copyMatchingService(name "E5Display")
// The registry path retains the published instance; the alloc reference is
// dropped here. The construction probe's instance-count balance (r=0)
// covers only its own transient object and stays valid.
extern int e5_display_attach;
extern "C" unsigned e5_display_open_count = 0;

extern "C" int e5_display_publish(IOService *provider) {
  if (!provider) return 0;
  e5_display_attach |= 0x400;
  OSObject *object = OSMetaClass::allocClassWithName("E5Display");
  if (!object) return 0;
  e5_display_attach |= 0x800;
  IOService *display = OSDynamicCast(IOService, object);
  bool live = false;
  bool started = false;
  if (display && display->init()) {
    e5_display_attach |= 0x1000;
    display->attach(provider);
    // IOService::attach returns void; confirm the registry edge exists
    // before claiming the step (r83 evidence review: attach is not
    // verified by side effect).
    if (display->getProvider() == provider) {
      e5_display_attach |= 0x2000;
      if (display->start(provider)) {
        started = true;
        e5_display_attach |= 0x4000;
        display->registerService();
        e5_display_attach |= 0x8000;
        // r84 evidence: registerService() registers ASYNCHRONOUSLY on the
        // IOKit workloop; an immediate copyMatchingService() raced and
        // missed (a=0xfc03). Bounded retry (up to 2s, 10ms steps).
        for (unsigned tries = 0; tries != 200; ++tries) {
          OSDictionary *match = IOService::nameMatching("E5Display");
          IOService *seen = match ? IOService::copyMatchingService(match) : nullptr;
          if (match) match->release();
          if (seen) {
            e5_display_attach |= 0x10000;
            seen->release();
            live = true;  // published AND observable by name
            break;
          }
          IOSleep(10);
        }
      }
    }
    if (!live) {
      if (started) display->stop(provider);
      display->detach(provider);
    }
  }
  object->release();
  return live ? 1 : 0;
}
