// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Published lifecycle probe. Hardware initialization remains unqualified.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>

extern "C" __attribute__((noreturn)) void e5_display_backend_guard(unsigned slot);
extern "C" unsigned e5_display_open_count;
extern "C" {
unsigned e5_display_client_calls = 0;
unsigned e5_display_client_successes = 0;
unsigned e5_display_client_type = 0;
unsigned e5_display_client_status = (unsigned)kIOReturnNotReady;
unsigned e5_display_pitch_calls = 0;
unsigned e5_display_pitch_status = (unsigned)kIOReturnNotReady;
}

class E5Display : public IOMobileFramebufferLegacy {
  OSDeclareDefaultStructors(E5Display)
public:
  // V8 lifecycle probe, not the original family's initialized start chain.
  // R85 reaches read-only UserClient selectors even with handleOpen == 0.
  bool start(IOService *) override { return true; }
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
        OSObject *property = provider->getProperty("dot-pitch");
        status = kIOReturnNotFound;
        if (property) {
          OSData *data = OSDynamicCast(OSData, property);
          status = kIOReturnBadArgument;
          if (data && data->getLength() == sizeof(*pitch)) {
            memcpy(pitch, data->getBytesNoCopy(), sizeof(*pitch));
            status = kIOReturnSuccess;
          }
        }
      }
    }
    __atomic_store_n(&e5_display_pitch_status, (unsigned)status, __ATOMIC_RELAXED);
    return status;
  }
#define E5_GUARDED_SLOT(n) \
  __attribute__((noreturn)) void opaque##n() override { e5_display_backend_guard(n); }
  E5_GUARDED_SLOT(211)
  E5_GUARDED_SLOT(213)
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
static_assert(sizeof(E5Display) == 0x18a0, "E5 factory probe adds no private state");

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
  if (display && display->init()) {
    e5_display_attach |= 0x1000;
    display->attach(provider);
    // IOService::attach returns void; confirm the registry edge exists
    // before claiming the step (r83 evidence review: attach is not
    // verified by side effect).
    if (display->getProvider() == provider) {
      e5_display_attach |= 0x2000;
      if (display->start(provider)) {
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
    if (!live) display->detach(provider);
  }
  object->release();
  return live ? 1 : 0;
}
