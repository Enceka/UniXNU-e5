// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors.
// Published lifecycle probe. Hardware initialization remains unqualified.
#include <kern/thread_call.h>
#include <kern/clock.h>
#include "OriginalLegacyFramebufferABI.h"
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>

extern "C" unsigned e5_quiet;

extern "C" IOReturn e5_display_backend_soft(unsigned slot, uint32_t selector,
                                            void *b, void *c, void *d);
extern "C" void e5_display_frame_kick(void);
extern "C" unsigned e5_display_open_count;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" {
unsigned e5_display_client_calls = 0;
unsigned e5_display_client_successes = 0;
unsigned e5_display_client_type = 0;
unsigned long long e5_display_uc_vtable = 0;
static unsigned e5_fake_e88_disabled = 0; // v43: ON — instance now 0x2110 so +0x1e88 is in-bounds
static void e5_fake_gate_init(void) {
  if (PE_parse_boot_argn("e5-fake-e88", nullptr, 0)) e5_fake_e88_disabled = 0;
}
unsigned long long e5_display_uc_slot179 = 0;
unsigned long long e5_display_uc_slot180 = 0;
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
extern "C" unsigned e5_display_vsync_edges(void); // D2 v2 (probe)
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
static void e5_state_log(const char *fmt, ...);
// v36: universal return-0 thunk + hand-built vtable for the fake family
// sub-objects (+0x1638 event source, +0x1658 swap-wait helper). arm64 KC =
// plain blr vtable dispatch (no PAC), so raw function pointers are safe.
extern "C" unsigned long e5FakeFamilyThunk(void) { return 0; }
// v44: the panel-info gate (helper 0x6bd6a90 vt+0x668) must return a NON-NULL
// pointer — cbz x0 takes the 0xe00002c7 bail (r203). The memcpy source is the
// RETURNED pointer +0x10. Static zero blob 0xc20 covers the 0xc0c copy.
static unsigned long long e5FakePanelBlob[0xc20 / 8];
extern "C" unsigned long e5FakeFamilyThunkObj(void) {
  return (unsigned long)&e5FakePanelBlob[0];
}
static void *const e5FakeFamilyVt[264] = {
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunkObj,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
    (void *)&e5FakeFamilyThunk, (void *)&e5FakeFamilyThunk,
};
static void *sAsyncNode = nullptr; // r153: kind-3 node captured at registration
// Registration arguments can live on the caller's stack. Retain only bounded
// diagnostic copies; r201 faulted while the DPU timer reread that dead stack.
static unsigned long long sAsyncRegSnapshot[4] = {};
static unsigned long long sAsyncNodeSnapshot[6] = {};
static unsigned sAsyncDiagnosticValid = 0;
extern "C" void e5_display_family_broadcast(void *svcRaw, unsigned kind,
                                            unsigned long long payload0);
static IOReturn e5_display_async_action(OSObject *owner, void *, void *, void *, void *) {
  static bool sNodeDumped = false;
  uint8_t *base = (uint8_t *)owner;
  if (!sNodeDumped) {
    sNodeDumped = true;
    e5_state_log("E5ACT base=%p node3=%p ak=%02x", owner,
                 __atomic_load_n(&sAsyncNode, __ATOMIC_RELAXED)
                     ? *(void **)&sAsyncNode : nullptr,
                 e5_display_async_kinds);
  }
  const unsigned long long now = mach_absolute_time();
  for (unsigned kind = 3; kind <= 5; kind += 2) {
    // r153: +0x1588 list head was NEVER populated (r152 proved: ak set, no
    // node, no E5NODE dump). Deliver through the node captured in opaque208.
    uint8_t *node = (kind == 3) ? (uint8_t *)sAsyncNode
                                : *(uint8_t **)(base + 0x1588 + kind * 16);
    for (unsigned visited = 0; node && visited < 64; ++visited) {
      unsigned long long *ref = (unsigned long long *)(node + 0x18);
      // r159: r158 proved node+0x18 is EMPTY while node+0x8 holds a kernel
      // pointer (the OSAsyncReference64 array the family registered). Try
      // BOTH candidates; sendAsyncResult64WithOptions validates and simply
      // returns an error for a wrong array (tracked in ar=).

      if (!*ref && !sNodeDumped) {
        sNodeDumped = true;
        // r152: node exists (ak bits prove it) but +0x18 ref is zero —
        // dump the node head once to find where the wake ref lives.
        e5_state_log("E5NODE k=%u node=%p w0=%llx w1=%llx w2=%llx w3=%llx w4=%llx w5=%llx",
                     kind, node,
                     ((unsigned long long *)node)[0],
                     ((unsigned long long *)node)[1],
                     ((unsigned long long *)node)[2],
                     ((unsigned long long *)node)[3],
                     ((unsigned long long *)node)[4],
                     ((unsigned long long *)node)[5]);
      }
      if (!*ref && kind == 3) {
        // r168: v28/r126 delivered 630/630 through the NODE ITSELF as the
        // OSAsyncReference64 (the sel-72 wrapper hands slot 208 a pointer at
        // ctx+8 that IS the reference array; node[0]==0 is fine — the kernel
        // delivery re-derives the port). Replicate exactly that call.
        unsigned long long v28payload[2] = {mach_absolute_time(), 0};
        int r = _ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj(
            (unsigned long long *)node, 0, v28payload, 2, 1);
        __atomic_add_fetch(&e5_display_async_sends, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&e5_display_async_last, (unsigned)r, __ATOMIC_RELAXED);
        if (r == 0) __atomic_add_fetch(&e5_display_async_send_ok, 1U, __ATOMIC_RELAXED);
        break; // one delivery per fire; r159 taught wrong-array = zone panic,
               // but v28's array is the registered pointer itself (r126-proven)
      }
      if (*ref) {
        __atomic_fetch_or(&e5_display_async_ports, 1u << kind, __ATOMIC_RELAXED);
        if (kind == 3) {
          // r160: the family's own broadcaster (raw 0xfffffff0067778c0)
          // resolves the registered ref internally; r136 proved it safe from
          // this thread_call context (asend counted 630/630 ok in that era).
          e5_display_family_broadcast(base, 3, 0);
          __atomic_add_fetch(&e5_display_async_sends, 1U, __ATOMIC_RELAXED);
          node = nullptr; // manual send below is dead for kind 3
          break;
        }
        // FrameInfoNotifyFunc accepts {property-mask, frame-id}; an empty mask
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

// v28: deliver a vsync-style async notification through the bound kernel
// export to every registered event context. Called from frame_kick.
// v29.1: r135 proved firing the family broadcast INLINE from frame_kick
// (NativeSd echo thread) stalls client service calls (C=0:e00002d8) — the
// marshaling is far heavier than r126's raw send. Move delivery to a
// self-rescheduling thread_call timer so the echo thread never blocks.
extern "C" void e5_display_async_fire(void); // timer body below
extern "C" void *e5_display_async_node3(void) {
  return __atomic_load_n(&sAsyncDiagnosticValid, __ATOMIC_ACQUIRE) ? sAsyncNodeSnapshot : nullptr;
}
extern "C" void *e5_display_async_reg3(void) {
  return __atomic_load_n(&sAsyncDiagnosticValid, __ATOMIC_ACQUIRE) ? sAsyncRegSnapshot : nullptr;
}
extern "C" void e5_display_family_broadcast(void *svcRaw, unsigned kind,
                                            unsigned long long payload0);

// r160: call the family's own event broadcaster. Raw KC VA 0xfffffff0067778c0;
// runtime slide derived from the un-overridden vtable entry at slot 175
// (raw 0xfffffff006792774): slide = *(void **)(*(void **)svc + 175*8) - raw.
extern "C" void e5_display_family_broadcast(void *svcRaw, unsigned kind,
                                            unsigned long long payload0) {
  if (!sPowerInstance) return;
  unsigned long long vtEntry =
      *(unsigned long long *)(*(unsigned long long **)svcRaw + 175);
  const long long slide = (long long)vtEntry - 0xfffffff006792774LL;
  void (*broadcast)(void *, int, unsigned long long, int, int) =
      (void (*)(void *, int, unsigned long long, int, int))(
          (unsigned long long)0xfffffff0067778c0 + slide);
  unsigned long long payload[1] = {payload0};
  broadcast(*(void **)((unsigned long long)svcRaw + 0x1608), 0,
            (unsigned long long)payload, (int)kind, 1);
}
static thread_call_t sFireTimer = nullptr;
extern "C" void e5_dpu_tick(void); // E5DisplayProbe.cpp: D2 v1 paint + backlight
// r150: thread_call_enter1_delayed takes an ABSOLUTE deadline, not a
// relative delay. Passing 100ms as a deadline fired the timer ~1.8kHz
// (r140 measured 1,090,560 ticks in 600s); that spin starved every display
// client (u=0:0:0:e00002d8 in ALL D2 rounds r137+; r128 with no D2 timer
// had u=4:4). Compute a real 100ms deadline.
static uint64_t e5_fire_deadline(void) {
  // r151: two-step canonical conversion; r150's one-step still yielded
  // ~1.8kHz (t=1,095,010 over the 600s window). Guarded additionally by
  // the wall-clock throttle in the timer body.
  uint64_t iv = 0;
  clock_interval_to_absolutetime_interval(100, NSEC_PER_SEC / 1000, &iv);
  return mach_absolute_time() + iv;
}
static void e5_fire_timer_fn(thread_call_param_t, thread_call_param_t) {
  // r149: r146/r147/r148 all-timeout regression root-caused to this timer:
  // the REAL tick rate is ~1.8kHz (r140 measurement), and calling
  // gate->attemptAction (display workloop gate close/open) at that rate
  // starves every client call (u=0:0:0:e00002d8). r144 proved paint-only
  // healthy. Rate-limit the gate entry: probe every 4096 ticks (~2.3s)
  // until a context registers (async_ports nonzero), then fire per tick
  // only while contexts exist (they unregister on close).
  static uint64_t sLastWorkAbstime = 0, sHalfInterval = 0;
  if (!sHalfInterval)
    clock_interval_to_absolutetime_interval(50, NSEC_PER_SEC / 1000, &sHalfInterval);
  const uint64_t nowAbs = mach_absolute_time();
  const bool timeToWork = (nowAbs - sLastWorkAbstime) >= sHalfInterval;
  if (timeToWork) {
    sLastWorkAbstime = nowAbs;
    static uint32_t sFireDivider = 0;
    const bool registered =
        __atomic_load_n(&e5_display_async_ports, __ATOMIC_RELAXED) != 0;
    // r155: the r151 form parsed as `++sFireDivider & (0x3F == 0)` —
    // always false, so the probe fire NEVER ran (r154 af=0 proved it).
    if (registered || (((++sFireDivider) & 0x3Fu) == 0))
      e5_display_async_fire();
    e5_dpu_tick(); // D2 v1: native scanout pattern + backlight pulse (timer ctx)
  }
  // v1.2: reschedule UNCONDITIONALLY. r138 proved the first fire lands
  // ~100ms after the first pager echo — before display start() sets
  // sPowerInstance — so the old guard killed the timer after one tick
  // (one E5DPU print, wrapped out of the ring; one paint; no animation).
  if (sFireTimer)
    thread_call_enter1_delayed(sFireTimer, nullptr, e5_fire_deadline());
}
extern "C" void e5_display_async_kick(void) {
  if (!sFireTimer) {
    sFireTimer = thread_call_allocate(&e5_fire_timer_fn, nullptr);
    if (!sFireTimer) return;
    thread_call_enter1_delayed(sFireTimer, nullptr, e5_fire_deadline());
  }
}
extern "C" unsigned e5_display_fire_calls = 0;
extern "C" int e5_display_gate_last = -99;
extern "C" void e5_display_async_fire(void) {
  if (!sPowerInstance) return;
  auto gate = *(IOCommandGate **)((uint8_t *)sPowerInstance + 0x1608);
  __atomic_add_fetch(&e5_display_fire_calls, 1U, __ATOMIC_RELAXED);
  if (gate)
    __atomic_store_n(&e5_display_gate_last,
                     (int)gate->attemptAction(e5_display_async_action),
                     __ATOMIC_RELAXED);
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
  // r220: the periodic E5STATE line floods the 32 KiB ramoops ring and pushes
  // the late E5CA/E5TH probe dumps out of every retained capture (r219 kept
  // only generation 6). e5-no-state keeps the display service untouched and
  // only silences this line so end-of-window snapshots survive.
  if (PE_parse_boot_argn("e5-no-state", nullptr, 0)) return;
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
  e5_kputc('\n');
}
extern "C" void e5_display_state_dump(void) {
  if (e5_quiet) return;
  if (!sPowerInstance) return;
  const volatile uint8_t *b = (const volatile uint8_t *)sPowerInstance;
  auto q = [&](unsigned off) -> unsigned long long {
    unsigned long long v = 0;
    for (unsigned i = 0; i != 8; ++i) v |= (unsigned long long)b[off + i] << (8 * i);
    return v;
  };
  unsigned long long e5_s175 = 0;
  if (e5_display_uc_vtable)
    e5_s175 = ((unsigned long long *)e5_display_uc_vtable)[175];
  // r207: registry entry id of this service so the host can match it against
  // the display-info pool slot id captured by IOMFB's match callback.
  // The instance is addressed through IORegistryEntry (complete type here)
  // because the E5Display class body is defined further below.
  unsigned long long e5_rid =
      ((IORegistryEntry *)e5_display_service_instance())->getRegistryEntryID();
  e5_state_log("E5STATE svc=%p rid=%llx m3c8=%llx m1608=%llx m1640=%llx m1658=%llx m1810=%llx m1858=%llx f1860=%02x%02x m1868=%llx m16d0=%llx e16e0=%02x vfire=%u ak=%02x asend=%u aok=%u ap=%02x ar=%x am=%02x ev=%u:%llx:%x af=%u gl=%d ucv=%llx ucm=%llx:%llx anch=%llx s175=%llx",
         (const void *)b, e5_rid, q(0x3c8), q(0x1608), q(0x1640), q(0x1658),
         q(0x1810), q(0x1858), b[0x1860], b[0x1861], q(0x1868), q(0x16d0), b[0x16e0], e5_display_vsync_fires,
         e5_display_async_kinds, e5_display_async_sends, e5_display_async_send_ok,
         e5_display_async_ports, e5_display_async_last, e5_display_async_armed,
         *(volatile const uint32_t *)(b + 0x16d0), e5_display_event_calls,
         e5_display_event_argument, e5_display_fire_calls, e5_display_gate_last,
         e5_display_uc_vtable, e5_display_uc_slot179, e5_display_uc_slot180,
         (unsigned long long)(void *)&e5_display_frame_kick, e5_s175);
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
    // v36 A-line: the family's enable_event 0x6797224 delegates its arm to the
    // sub-object at [+0x1638] (vt+0xf8(0,0,0)) and the sel-6 swap-wait fn A
    // 0x679795c NULL-checks [+0x1658] before registering the client waiter at
    // +0x1810. H9G builds both via internal factories we do not own; without
    // them enable_event arms nothing (+0x16e0 stays set forever, no waiter is
    // ever registered, no notify ever fires). The kernelcache is arm64 (plain
    // blr vtable calls, no PAC — b182 lesson), so a hand-built vtable of
    // return-0 thunks is safe: the family treats the fake as "source exists,
    // nothing to do". Registration then proceeds and the waiter vtable lands
    // at +0x1810 for our notify timer to complete (v37).
    if (!*(void * volatile *)((uint8_t *)this + 0x1638)) {
      void *obj = IOMalloc(0x40);
      if (obj) {
        bzero(obj, 0x40);
        *(void * volatile *)obj = (void *)&e5FakeFamilyVt[0];
        *(void * volatile *)((uint8_t *)this + 0x1638) = obj;
        __atomic_fetch_or(&e5_display_lifecycle, 0x40000U, __ATOMIC_RELAXED);
      }
    }
    if (!*(void * volatile *)((uint8_t *)this + 0x1658)) {
      void *obj = IOMalloc(0x40);
      if (obj) {
        bzero(obj, 0x40);
        *(void * volatile *)obj = (void *)&e5FakeFamilyVt[0];
        *(void * volatile *)((uint8_t *)this + 0x1658) = obj;
        __atomic_fetch_or(&e5_display_lifecycle, 0x80000U, __ATOMIC_RELAXED);
      }
    }
    // v37: sel 17 struct variant (pool-B client) walks the panel-info helper
    // at [+0x1e88]: gate call vt+0x668 (slot 205) bails with 0xE00002C7 when
    // the sub-object is NULL (b245 decode: 0x6bd6a90 NULL-check). The tail
    // memcpy of 0xc0c bytes from sub+0x10 then lands as zeros — proven
    // acceptable in the pool-A era (r117 sel-17 zero-fill ok=1). Fake vtable
    // extended to 256 slots so slot 205 answers success.
    e5_fake_gate_init();
    if (!e5_fake_e88_disabled && !*(void * volatile *)((uint8_t *)this + 0x1e88)) {
      void *obj = IOMalloc(0x1000);
      if (obj) {
        bzero(obj, 0x1000);
        *(void * volatile *)obj = (void *)&e5FakeFamilyVt[0];
        *(void * volatile *)((uint8_t *)this + 0x1e88) = obj;
        __atomic_fetch_or(&e5_display_lifecycle, 0x100000U, __ATOMIC_RELAXED);
      }
    }
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
    if (status == kIOReturnSuccess && client && *client) {
      __atomic_fetch_add(&e5_display_client_successes, 1U, __ATOMIC_RELAXED);
      // r174: capture the family user client's vtable + slot 179/180
      // (clientMemoryForType per IOUserClient declaration order after
      // IOService's 168 slots) to test the direct-map theory: if slot 179
      // points into family text the UC overrides memory mapping (fb map
      // path exists); if it equals the IOUserClient base impl there is no
      // map path at all.
      const void *const *vt = *(const void *const **)(*client);
      __atomic_store_n(&e5_display_uc_vtable, (unsigned long long)vt,
                       __ATOMIC_RELAXED);
      __atomic_store_n(&e5_display_uc_slot179,
                       (unsigned long long)(vt ? vt[179] : nullptr),
                       __ATOMIC_RELAXED);
      __atomic_store_n(&e5_display_uc_slot180,
                       (unsigned long long)(vt ? vt[180] : nullptr),
                       __ATOMIC_RELAXED);
    }
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
  // Selector 74 asks for (index, count, key[64], transport). r125/r126
  // proved that answering this query with count 1 lets the client register
  // the kind-3 FrameInfo context through the NATIVE slot-208 path, and the
  // merged async action delivers correctly shaped {mask=0, frame-id} Mach
  // notifications (opaque336 forces the Mach fallback, never the unimple-
  // mented shared queue). Transport stays 0 for the same reason. This is
  // the r147 experiment: un-decline and watch ak=/asend= plus the panel
  // drawer reaction.
  IOReturn opaque213(uint32_t index, uint32_t *count, char *key, uint32_t *transport) override {
    (void)index;
    __atomic_fetch_add(&e5_display_cap213_calls, 1U, __ATOMIC_RELAXED);
    IOReturn status = kIOReturnBadArgument;
    if (count && key && transport) {
      *count = 1;
      *transport = 1; // r164: v18/r125-era answer (cached at +0x3c8)
      bzero(key, 64); // r166: v18 left the key buffer UNTOUCHED (caller
                      // zeros); a non-empty unknown transport key may make
                      // the client disable the async path. Empty key = the
                      // exact last-known-good (r125/r126) behavior.
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
  // v30: original slot 229 enters the command gate with virtual slot 344
  // as its action. Bypassing it also bypasses the event-source arm, so an
  // acknowledged power request cannot produce a native completion token.
  IOReturn opaque229(uint32_t state, void *zero) override {
    __atomic_fetch_add(&e5_display_pw_calls, 1U, __ATOMIC_RELAXED);
    return IOMobileFramebufferLegacy::opaque229(state, zero);
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
      if (kind == 3) {
        sAsyncNode = node; // r153: deliver through the node the client handed us
        __atomic_store_n(&sAsyncDiagnosticValid, 0U, __ATOMIC_RELEASE);
        bzero(sAsyncRegSnapshot, sizeof(sAsyncRegSnapshot));
        if (registration) memcpy(sAsyncRegSnapshot, registration, sizeof(sAsyncRegSnapshot));
        memcpy(sAsyncNodeSnapshot, node, sizeof(sAsyncNodeSnapshot));
        __atomic_store_n(&sAsyncDiagnosticValid, 1U, __ATOMIC_RELEASE);
        unsigned long long *w = (unsigned long long *)node;
        e5_state_log("E5REG k=3 node=%p reg=%p w0=%llx w1=%llx w2=%llx w3=%llx w4=%llx",
                     node, registration, w[0], w[1], w[2], w[3], w[4]);
      }
    }
    return status;
  }
  IOReturn opaque210(uint32_t kind, void *node, uint32_t on) override {
    __atomic_fetch_or(&e5_display_swap_mask, 1u << 8, __ATOMIC_RELAXED);
    return IOMobileFramebufferLegacy::opaque210(kind, node, on);
  }
  // r148: opaque336 override REMOVED (r147 regression bisect). Original
  // default true stood in every healthy round through r144/r128.
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
    // r163: v25 proved the client RETRIES SwapWait on Timeout and its loop
    // revives on a periodic success pulse (n=4 with 3 timeouts + 1 ok at
    // 1-in-4). The pure native pass-through (r127) answers from empty
    // structures and the client abandons after one call. Serve the v25
    // cadence directly: ~3 timeouts then one success = a fake vsync tick.
    static unsigned sWaitBeats = 0;
    const bool pulse = (++sWaitBeats & 0x3u) == 0;
    const IOReturn status =
        pulse ? kIOReturnSuccess : kIOReturnTimeout;
    __atomic_store_n(&e5_display_wait_status,
                     (unsigned)(pulse ? 0 : 0xE00002D5u), __ATOMIC_RELAXED);
    return status;
    (void)client; (void)flags; (void)timeout;
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
    // v45: zero-fill + success (r170's decline A/B answered — the client
    // DOES distinguish failure: pool-B sel 17 n=1 ok=0 ret=e00002c7 with
    // E5CAX all-zero = display never configured while this declined).
    // ABI (r204 decode): wrapper 0x677a0c8 validates structureOutputSize
    // 0xc0c, forwarder 0x6778040 -> slot200(this, out).
    if (out) bzero(out, 0xc0c);
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
static_assert(sizeof(E5Display) == 0x2118, "v43: grown Legacy extent carries the +0x1e88 panel pointer in-bounds");

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
