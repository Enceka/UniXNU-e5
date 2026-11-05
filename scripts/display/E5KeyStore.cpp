// SPDX-License-Identifier: BSD-2-Clause-Patent
// r213: host side for the pinned AppleKeyStoreState component
// (UniXNU/AppleKeyStoreState 42ff83e, drivers/AppleKeyStoreState).
//
// Why: r212 proved the render server pipeline healthy (timer firing serial
// 1578, both runloops live, two client contexts attached) while selectors
// 3/4/5 stayed zero. The UniXNU-qemu 21H461 case traced the same shape to
// SpringBoard never finishing startup: its main thread waits on a synchronous
// chronod XPC, chronod waits on liveactivitiesd, and liveactivitiesd dies in
// SessionSyncEngine Keybag.shared because MKBEventsRegister gets no
// AppleKeyStore service (r126 retained the identical trap on E5; the r212
// census mask 0x38 shows the AppleKeyStore bit clear). The component publishes
// that service and answers the state selectors truthfully for this fixture:
// no keybag, no passcode, nothing locked. It provides no key and protects
// nothing.
//
// Opt-in through the e5-aks boot argument; without it nothing is published
// and every open is refused. First use of each selector logs one E5AKS line
// so the round record shows exactly what libaks asked for.
#include <IOKit/IOService.h>
#include <kern/kern_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern "C" void e5_kputc(char);

extern "C" IOService *native_keyevents_allocate(void);

namespace {
bool enabled() { return PE_parse_boot_argn("e5-aks", nullptr, 0); }
void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
void note(const char *format, ...) {
  char line[160];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
  e5_kputc('\n');
}
unsigned opens, refusals, calls, unsupported;
uint8_t seen[128];  // selector bitmap, 0..1023

void recordCall(uint32_t selector, int32_t result) {
  __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
  if ((uint32_t)result == 0xe00002c7U) __atomic_add_fetch(&unsupported, 1, __ATOMIC_RELAXED);
  if (selector < 1024) {
    uint8_t bit = (uint8_t)(1u << (selector & 7));
    uint8_t previous = seen[selector >> 3];
    seen[selector >> 3] = (uint8_t)(previous | bit);
    if (!(previous & bit))
      note("E5AKS sel=%u rc=%x first", selector, (unsigned)result & 0xffffffffu);
  }
}
}  // namespace

// Component host callbacks (NativeKeyStoreEvents.cpp).
extern "C" bool native_aks_state_enabled(void) { return enabled(); }
extern "C" uint32_t native_aks_lock_state(void) { return 0; }  // sls 0: unlocked.
extern "C" void native_aks_opened(void) { __atomic_add_fetch(&opens, 1, __ATOMIC_RELAXED); }
extern "C" void native_keyevents_open_refused(void) {
  __atomic_add_fetch(&refusals, 1, __ATOMIC_RELAXED);
}
extern "C" void native_aks_call(uint32_t selector, int32_t result) { recordCall(selector, result); }

// Publishes the component service before launchd starts daemons; called from
// e5_display_start() during e5_systemos_prepare. The service class is named
// AppleKeyStore, so IOServiceMatching("AppleKeyStore") finds it exactly as
// libaks/MobileKeyBag expects.
extern "C" int e5_keystore_start(void) {
  if (!enabled()) return 0;
  mach_timespec_t timeout = {20, 0};
  IOService *parent = IOService::waitForService(IOService::serviceMatching("IOPlatformExpertDevice"), &timeout);
  if (!parent) {
    note("E5AKS publish=0 platform-expert absent");
    return 0;
  }
  IOService *service = native_keyevents_allocate();
  if (!service) {
    note("E5AKS publish=0 allocate failed");
    return 0;
  }
  bool live = false;
  if (service->init()) {
    service->attach(parent);
    if (service->getProvider() == parent && service->start(parent)) {
      service->registerService();
      for (unsigned tries = 0; tries != 200; ++tries) {
        OSDictionary *match = IOService::serviceMatching("AppleKeyStore");
        IOService *seenService = match ? IOService::copyMatchingService(match) : nullptr;
        if (match) match->release();
        if (seenService) {
          seenService->release();
          live = true;
          break;
        }
        IOSleep(10);
      }
    }
  }
  if (!live) {
    service->detach(parent);
    service->release();
    note("E5AKS publish=0 start/register failed");
    return 0;
  }
  // The registry retains the published instance; drop the alloc reference.
  service->release();
  note("E5AKS publish=1 AppleKeyStore visible");
  return 1;
}
