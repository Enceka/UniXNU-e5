// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Construction probe only. No provider, user client or hardware is published.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>

extern "C" __attribute__((noreturn)) void e5_display_backend_guard(unsigned slot);
extern "C" unsigned e5_display_open_count;

class E5Display : public IOMobileFramebufferLegacy {
  OSDeclareDefaultStructors(E5Display)
public:
  // V8 lifecycle publication: no hardware is touched. The guarded backend
  // slots below keep any premature consumer from reaching the DPU path; the
  // first attach round runs on headless Data where no userland opens us.
  bool start(IOService *) override { return true; }
  // V9 open accounting: slot 91 is IOService::handleOpen in the original
  // Legacy table (audit-verified: Legacy does not override it), so counting
  // here observes every IOServiceOpen on the published service without
  // altering semantics. This is the "Open works" acceptance instrument.
  bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) override {
    e5_display_open_count++;
    return IOService::handleOpen(forClient, options, arg);
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
