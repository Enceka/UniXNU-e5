// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

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

extern "C" __attribute__((noreturn)) void e5_display_backend_guard(unsigned slot) {
  printf("E5Display: unqualified backend slot %u v=%x\n", slot, (unsigned)e5_display_construction);
  panic("E5Display construction probe entered unqualified backend slot %u", slot);
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
  printf("E5Display: construction v=%x size=%x registration=%x instances=%u/%u\n",
         (unsigned)e5_display_construction, (unsigned)e5_display_original_size,
         (unsigned)e5_display_registration_result, instances, owned->getInstanceCount());
  return e5_display_construction == 0x1ff;
}
