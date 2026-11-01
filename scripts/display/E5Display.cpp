// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Published lifecycle probe. Hardware initialization remains unqualified.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>

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
    if (status == kIOReturnSuccess && client && *client)
      __atomic_fetch_add(&e5_display_client_successes, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&e5_display_client_status, (unsigned)status, __ATOMIC_RELAXED);
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
  E5_GUARDED_SLOT(211)
  E5_GUARDED_SLOT(245)
  E5_GUARDED_SLOT(246)
  E5_GUARDED_SLOT(306)
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
