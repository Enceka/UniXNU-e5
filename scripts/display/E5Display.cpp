// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Construction probe only. No provider, user client or hardware is published.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>

extern "C" __attribute__((noreturn)) void e5_display_backend_guard(unsigned slot);

class E5Display : public IOMobileFramebufferLegacy {
  OSDeclareDefaultStructors(E5Display)
public:
  // start is a qualified IOService prototype, but the E5 backend is not ready.
  bool start(IOService *) override { return false; }
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
