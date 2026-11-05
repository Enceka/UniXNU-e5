// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, opt-in snapshots of the pinned 21H461 backboardd display server.
// Reads run on a separate thread_call, never on the media completion thread.
//
// r207: the r206 census answered "which displays exist" (1 AppleInternalDisplay
// + 5 AppleWirelessDisplay, all con=0) but left the conn paradox open: the ctor
// attaches a conn before create_display_of_type keeps a display, and no direct
// writer ever clears +0x6378, so a live display with con=0 means either the
// field was reassigned through an IOMobileFramebuffer*& reference helper or the
// conn lives elsewhere in the object.  This version therefore:
//   * dumps the IOMFB display-info pool (per-slot name/type/index/registry-id)
//     plus the populate counters (virtual count, Ref_FB, Act_FB, info count);
//   * scans each display object (0..0x6d38) for any pointer whose +0x880 slot
//     lands inside the IOMobileFramebufferCore text range — i.e. any referenced
//     connection object, whatever offset holds it — and reads its io_connect,
//     type and callback slots so native vs virtual profile is decided by data.
// r212: the IPC census guard accepted only the 0xfffffff0 image window, so it
//   bailed on backboardd's kalloc-backed itk_space and logged all zeros. The
//   window now covers the observed 0xfffffff1..0xfffffff5 heap. Also reads the
//   r209 "next discriminating measurements" (server+0x398/0x39c/0x39d, due
//   +0x338, gates +0x360/+0x370, display+0x6bee latch, timer callback serial)
//   and the r212 runloop/context fields (server+0x1f0/+0x1f8 thread handles,
//   +0x2e0/+0x2e8 published runloops, +0x70/+0x80 Render context list).
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <kern/thread_call.h>
#include <mach/task_info.h>
#include <sys/proc.h>
#include <vm/vm_map.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern "C" void e5_kputc(char);
extern "C" unsigned e5_quiet;
// This internal kernel helper is linked into this built-in archive, but its
// declaration is hidden from the IOKit compile record's private-header mode.
extern "C" kern_return_t vm_map_read_user(vm_map_t, vm_map_address_t, void *, vm_size_t);
// r214 thread snapshot: same linkage situation as vm_map_read_user. The queue
// offsets come from llvm-dwarfdump of this build's own objects, matching the
// ipc offsets below: task.threads +0x80 (queue_head_t), thread.task_threads
// +0x428 (queue_chain_t). r216 reads the frozen saved state directly and
// keeps no thread references.
extern "C" void task_lock(task_t);
extern "C" void task_unlock(task_t);

namespace {
using Word = unsigned long long;
enum Field {
  Phase, Error, Pid, Slide, WindowServer, Array, ArrayClass, FirstSlot,
  Capacity, Count, VirtCount, RefFB, ActFB, InfoCount, Fields
};
static const unsigned kMaxEntries = 8;
static const unsigned kMaxConns = 4;
static const unsigned kMaxSlots = 16;
// CADisplayTimer object internals (dispatch-free decode: the timer is an
// mk_timer mach port + CFMachPort pair; firing is observable kernel-side).
struct TimerState {
  Word portName, cfmach, third, hook, c8, c9, d0, d8, e0, e8;
};
struct ConnState {
  Word offset, conn, io, type, slot880, slot8a8, slot8e0, slot9d8, bm;
  Word k5cb, k5aux, k5port, k5src, de8;
};
struct EntryState {
  Word wrapper, isa, impl, server, svt, display, dvt;
  Word mode, flags, flags2, enabled, powerPtr, power, conn;
  Word timingType, modeCount, gate28, sdisabled, sshutdown, tc9, s340;
  unsigned conns;
  ConnState connState[kMaxConns];
  Word ssw, sctx, stimer, svsync, sfi, smetal;
  // r209 contract "next discriminating measurements": vsync enable refs,
  // enabled byte, timer/native selection bit, pending render due time, the
  // two vsync timestamp gates, and the ignore_update history latch.
  Word s398, s39c, s39d, s338, s360, s370, latch;
  // r212 static decode (docs/display-runloop-static-r212.md): runloop thread
  // handles +0x1f0/+0x1f8, published runloops +0x2e0/+0x2e8 (NULL until
  // IOMFBServer::add_runloop_source's thread stores CFRunLoopGetCurrent),
  // and the Render context list +0x70/+0x80 filled by context_insert.
  Word s1f0, s1f8, s2e0, s2e8, ctxList, ctxCount;
  // r213: the context list entries (16 bytes {ctx, order float}); context
  // flags at ctx+0xc — bit 0x8000 is the definesDisplayBounds option that
  // only a finished SpringBoard startup sets.
  struct CtxEntry { Word entry, ptr, w8, flags, w10; };
  CtxEntry ctxEntry[4];
  TimerState timer;
};
struct SlotState {
  Word present, name, type, vidx, id, flag;
  char text[20];
};
struct PoolState { Word virtCount, refFB, actFB, infoCount, timerSerial; SlotState slot[kMaxSlots]; };
// r214: where is SpringBoard startup waiting? Saved pc/lr/sp plus a bounded
// frame walk for the first threads of SpringBoard/liveactivitiesd/chronod,
// symbolicated offline against the pinned cache (the kernel-side analogue
// of UniXNU-qemu's snapshot-ui-threads).
struct ThreadSnap { Word pc, lr, sp; Word ret[6]; };
struct ProcSnap { unsigned pid, threads; char name[20]; ThreadSnap thread[12]; };
static const unsigned kMaxProcs = 3;
static const unsigned kMaxThreads = 12;
static const unsigned kFrames = 6;
struct ProcSnaps { unsigned count; ProcSnap proc[kMaxProcs]; };
// Kernel-side IPC census of the probed task: how many mk_timer ports exist,
// whether any fired-but-undrained messages sit in their queues, and the
// armed deadline of the CA display timer's own port.
struct IpcPortLine { Word idx, msgs, kotype, armed; };
struct IpcState {
  Word entries, ports, timers, timerMsgs, undrained;
  Word caIdx, caMsgs, caKotype, caDeadline, caFlags;
  IpcPortLine top[4];
};
static Word values[Fields];
static EntryState entries[kMaxEntries];
static PoolState pool;
static IpcState ipc;
static ProcSnaps threads;
static unsigned generation, busy, selectedPid;
static thread_call_t worker;
static bool enabled() {
  return PE_parse_boot_argn("e5-ca-state", nullptr, 0);
}
static void log(const char *format, ...) {
  char line[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);
  for (const char *p = line; *p; ++p) e5_kputc(*p);
  e5_kputc('\n');
}
struct Reader {
  vm_map_t map;
  Word *state;
  bool bytes(Word address, void *destination, unsigned size) {
    if (address < 0x1000 || address >= 0x8000000000ULL || !size || size > 256 ||
        size > 0x8000000000ULL - address) {
      state[Error] = 0x10001;
      return false;
    }
    kern_return_t kr = vm_map_read_user(map, address, destination, size);
    if (kr != KERN_SUCCESS) { state[Error] = (unsigned)kr; return false; }
    return true;
  }
  // Quiet variant for the speculative conn scan: a miss must not clobber the
  // probe error code.
  bool probeBytes(Word address, void *destination, unsigned size) {
    if (address < 0x1000 || address >= 0x8000000000ULL || !size || size > 256 ||
        size > 0x8000000000ULL - address) return false;
    return vm_map_read_user(map, address, destination, size) == KERN_SUCCESS;
  }
  bool word(Word address, Word &value) { return bytes(address, &value, sizeof(value)); }
  bool u32(Word address, Word &value) {
    uint32_t v = 0;
    if (!bytes(address, &v, sizeof(v))) return false;
    value = v;
    return true;
  }
  bool u16(Word address, Word &value) {
    uint16_t v = 0;
    if (!bytes(address, &v, sizeof(v))) return false;
    value = v;
    return true;
  }
  bool u8(Word address, Word &value) {
    uint8_t v = 0;
    if (!bytes(address, &v, sizeof(v))) return false;
    value = v;
    return true;
  }
};
// Vtable identities decoded from the pinned QuartzCore: IOMFBDisplay,
// base Display, AppleExternalDisplay; IOMFBServer, AccelServer, base Server.
// The AppleInternalDisplay/AppleWirelessDisplay subclasses (r206 census) are
// accepted as displays too — they inherit the layout under test.
static bool isDisplayVtable(Word vtable, Word slide) {
  return vtable == 0x1e691b360ULL + slide || vtable == 0x1e691e5b0ULL + slide ||
         vtable == 0x1e691aab0ULL + slide || vtable == 0x1e691d208ULL + slide ||
         vtable == 0x1e691dab0ULL + slide;
}
static bool isServerVtable(Word vtable, Word slide) {
  return vtable == 0x1e691a648ULL + slide || vtable == 0x1e691a8b0ULL + slide ||
         vtable == 0x1e691e3b0ULL + slide;
}
// A connection object is recognised by a callback-table entry pointing into
// the IOMobileFramebufferCore text range (conn+0x880 = swap-begin slot).
static bool connTextHit(Word value, Word slide) {
  return value >= 0x1b63cf000ULL + slide && value < 0x1b63de000ULL + slide;
}
static void scanForConns(Reader &r, Word slide, Word display, EntryState &e) {
  // Chunked walk of the display object; every aligned word in the heap range
  // whose +0x880 hits IOMFB text is recorded as a candidate connection.
  static const unsigned kObjectBytes = 0x6d38;
  unsigned found = 0;
  uint8_t chunk[256];
  for (unsigned base = 0; base < kObjectBytes && found < kMaxConns; base += sizeof(chunk)) {
    if (!r.probeBytes(display + base, chunk, sizeof(chunk))) break;
    for (unsigned off = 0; off + 8 <= sizeof(chunk) && found < kMaxConns; off += 8) {
      Word candidate;
      memcpy(&candidate, chunk + off, sizeof(candidate));
      if (candidate < 0x100000000ULL || candidate >= 0x8000000000ULL) continue;
      Word slot880 = 0;
      if (!r.probeBytes(candidate + 0x880, &slot880, sizeof(slot880))) continue;
      if (!connTextHit(slot880, slide)) continue;
      ConnState &c = e.connState[found];
      c.offset = base + off;
      c.conn = candidate;
      c.slot880 = slot880;
      r.probeBytes(candidate + 0x8a8, &c.slot8a8, sizeof(c.slot8a8));
      r.probeBytes(candidate + 0x8e0, &c.slot8e0, sizeof(c.slot8e0));
      r.probeBytes(candidate + 0x9d8, &c.slot9d8, sizeof(c.slot9d8));
      r.probeBytes(candidate + 0x788, &c.type, sizeof(c.type));
      r.probeBytes(candidate + 0x14, &c.io, sizeof(c.io));
      // kind-5 (vsync) notification registration state.
      r.probeBytes(candidate + 0x710, &c.k5cb, sizeof(c.k5cb));
      r.probeBytes(candidate + 0x738, &c.k5aux, sizeof(c.k5aux));
      r.probeBytes(candidate + 0x6f8, &c.k5port, sizeof(c.k5port));
      r.probeBytes(candidate + 0x6f0, &c.k5src, sizeof(c.k5src));
      Word de8 = 0;
      r.probeBytes(candidate + 0xde8, &de8, 1);
      c.de8 = de8;
      Word bm = 0;
      r.probeBytes(candidate + 0xca8, &bm, 1);
      c.bm = bm;
      ++found;
    }
  }
  e.conns = found;
}
static void inspectEntry(Reader &r, Word slide, Word wrapper, EntryState &e) {
  e.wrapper = wrapper;
  if (!wrapper) return;
  if (!r.word(wrapper, e.isa)) return;
  if (!r.word(wrapper + 8, e.impl) || !e.impl) return;
  if (!r.word(e.impl + 0x40, e.server) || !e.server) return;
  if (!r.word(e.server, e.svt)) return;
  // Server-side render/vsync state for every recognised server class.
  if (isServerVtable(e.svt, slide)) {
    r.word(e.server + 0xc8, e.ssw);
    r.word(e.server + 0xc0, e.sctx);
    r.word(e.server + 0x300, e.stimer);
    r.word(e.server + 0x2f0, e.svsync);
    r.word(e.server + 0x330, e.sfi);
    // render_for_time entry gates: disabled/shutdown bytes + bookkeeping.
    r.u8(e.server + 0x1e0, e.sdisabled);
    r.u8(e.server + 0x1e1, e.sshutdown);
    r.word(e.server + 0x340, e.s340);
    r.u8(e.server + 0x398, e.s398);
    r.u8(e.server + 0x39c, e.s39c);
    r.u8(e.server + 0x39d, e.s39d);
    r.word(e.server + 0x338, e.s338);
    r.word(e.server + 0x360, e.s360);
    r.word(e.server + 0x370, e.s370);
    r.word(e.server + 0x1f0, e.s1f0);
    r.word(e.server + 0x1f8, e.s1f8);
    r.word(e.server + 0x2e0, e.s2e0);
    r.word(e.server + 0x2e8, e.s2e8);
    r.word(e.server + 0x70, e.ctxList);
    r.word(e.server + 0x80, e.ctxCount);
    for (unsigned k = 0; k != 4 && k < e.ctxCount; ++k) {
      Word entry = e.ctxList + (Word)k * 0x10;
      Word ctxPtr = 0;
      if (!r.word(entry, ctxPtr) || !ctxPtr) break;
      EntryState::CtxEntry &t = e.ctxEntry[k];
      t.entry = entry;
      t.ptr = ctxPtr;
      r.word(ctxPtr + 8, t.w8);
      r.u32(ctxPtr + 0xc, t.flags);
      r.u32(ctxPtr + 0x10, t.w10);
    }
    if (e.stimer) { r.u8(e.stimer + 0xc9, e.tc9); }
    if (e.stimer) {
      TimerState &t = e.timer;
      r.u32(e.stimer + 0x78, t.portName);
      r.word(e.stimer + 0x80, t.cfmach);
      r.word(e.stimer + 0x88, t.third);
      r.word(e.stimer + 0xc0, t.hook);
      r.u8(e.stimer + 0xc8, t.c8);
      r.u8(e.stimer + 0xc9, t.c9);
      r.word(e.stimer + 0xd0, t.d0);
      r.word(e.stimer + 0xd8, t.d8);
      r.word(e.stimer + 0xe0, t.e0);
      r.word(e.stimer + 0xe8, t.e8);
    }
    if (e.svt == 0x1e691a648ULL + slide) r.word(e.server + 0x3b0, e.smetal);
  }
  if (!r.word(e.server + 0x60, e.display) || !e.display) return;
  if (!r.word(e.display, e.dvt)) return;
  if (!isDisplayVtable(e.dvt, slide)) return;
  // Display layout is shared from the base class; the connection pointer
  // only exists on IOMFBDisplay.
  r.word(e.display + 0x248, e.mode);
  r.u32(e.display + 0x2a1, e.flags);
  r.u32(e.display + 0x258, e.flags2);
  r.u16(e.display + 0x6bcd, e.enabled);
  r.u8(e.display + 0x6bee, e.latch);
  // +0x244 is SignalType; +0x66b8 counts the mode-info hash map. The >=2
  // gate belongs to power-on mode reselection. render_for_time instead
  // tests bit5 of display+0x30 when the pointer at display+0x28 is nonzero
  // (0x18951de78..0x18951de9c); it does not dereference that pointer.
  r.u8(e.display + 0x244, e.timingType);
  r.word(e.display + 0x66b8, e.modeCount);
  Word p28 = 0;
  if (r.word(e.display + 0x28, p28) && p28) { r.u8(e.display + 0x30, e.gate28); }
  if (r.word(e.display + 0x290, e.powerPtr) && e.powerPtr) r.u32(e.powerPtr, e.power);
  r.word(e.display + 0x6378, e.conn);
  // Locate connection objects anywhere in the display (conn field, virtual
  // framebuffer sub-objects, indirect references) and classify them.
  scanForConns(r, slide, e.display, e);
}
static void inspectPool(Reader &r, Word slide, PoolState &p) {
  // IOMFB __DATA statics share the cache slide.  Counters first, then the
  // 16-slot display-info pool filled by the match callback and the synthetic
  // wireless loop.  Slot: +0 name CFString, +8 type u32, +0x10 index,
  // +0x14 registry id u32, +0x98 flag byte.
  r.u32(0x1f4c4f568ULL + slide, p.virtCount);
  r.u32(0x1f4c4f698ULL + slide, p.refFB);
  r.u32(0x1f4c4f6acULL + slide, p.actFB);
  r.u32(0x1f4c5c0b8ULL + slide, p.infoCount);
  // CADisplayTimer callback serial in QuartzCore __DATA: nonzero means the
  // timer callback has run at least once, independent of RunLoop state.
  r.word(0x1ed7ab930ULL + slide, p.timerSerial);
  for (unsigned i = 0; i != kMaxSlots; ++i) {
    Word slot = 0x1f4c4f6b8ULL + slide + (Word)i * 0xa0;
    SlotState &s = p.slot[i];
    if (!r.u32(slot, s.name) || !s.name) continue;
    s.present = 1;
    r.u32(slot + 8, s.type);
    r.u32(slot + 0x10, s.vidx);
    r.u32(slot + 0x14, s.id);
    Word flag = 0;
    r.u8(slot + 0x98, flag);
    s.flag = flag;
    // CFString {isa,flags,ptr,len}: print up to 19 characters of the name so
    // 'primary' vs 'wireless0..' is visible without another deref level.
    Word strPtr = 0, len = 0;
    if (r.probeBytes(s.name + 0x10, &strPtr, 8) && r.probeBytes(s.name + 0x18, &len, 8) &&
        len && len < 64) {
      unsigned want = len < 19 ? (unsigned)len : 19;
      r.probeBytes(strPtr, s.text, want);
    }
  }
}
static void inspect(task_t task, Word *s, EntryState *out, PoolState *poolOut) {
  // All discovery state lands in the caller's scratch so a failed early phase
  // cannot inherit stale data; the caller publishes under the generation lock.
  s[Phase] = 1;
  task_dyld_info_data_t dyld = {};
  mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
  kern_return_t kr = task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld, &count);
  if (kr != KERN_SUCCESS) { s[Error] = (unsigned)kr; return; }
  if (dyld.all_image_info_format != TASK_DYLD_ALL_IMAGE_INFO_64 ||
      dyld.all_image_info_size < 184) { s[Error] = 0x10002; return; }
  vm_map_t map = get_task_map(task);
  if (!map) { s[Error] = 0x10003; return; }
  Reader r = {map, s};
  s[Phase] = 2;
  // kdp/kdp_dyld.h user64_dyld_all_image_infos: slide +0x98, UUID +0xa0.
  uint8_t infos[184] = {};
  if (!r.bytes(dyld.all_image_info_addr, infos, sizeof(infos))) return;
  uint32_t version = 0;
  memcpy(&version, infos, sizeof(version));
  if (version < 13) { s[Error] = 0x10004; return; }
  memcpy(&s[Slide], infos + 0x98, sizeof(Word));
  static const uint8_t cacheUuid[16] = {
    0x59,0x0d,0x83,0x9a,0x82,0xd2,0x30,0xf1,0xa1,0x7d,0x0c,0xc0,0x53,0x42,0x0f,0x73};
  if (s[Slide] >= 0x40000000 || memcmp(infos + 0xa0, cacheUuid, 16)) {
    s[Error] = 0x10005; return;
  }
  s[Phase] = 3;
  uint8_t uuid[16] = {};
  static const uint8_t quartzUuid[16] = {
    0xbc,0xa7,0x4c,0xf2,0x23,0x6a,0x30,0xac,0xbd,0x05,0x88,0x72,0xb2,0xc2,0x26,0x2d};
  if (!r.bytes(0x189429f80ULL + s[Slide], uuid, sizeof(uuid))) return;
  if (memcmp(uuid, quartzUuid, 16)) { s[Error] = 0x10006; return; }
  s[Phase] = 4;
  Word implementation = 0;
  if (!r.word(0x1ed7ab1e0ULL + s[Slide], s[WindowServer]) || !s[WindowServer]) return;
  if (!r.word(s[WindowServer] + 8, implementation) || !implementation ||
      !r.word(implementation, s[Array]) || !s[Array]) return;
  s[Phase] = 5;
  if (!r.word(s[Array], s[ArrayClass])) return;
  // Only the exact NSArrayM layout decoded from this cache is followed.
  if ((s[ArrayClass] & 0xffffffff8ULL) != 0x1f4854dd8ULL + s[Slide]) {
    s[Error] = 0x10007; return;
  }
  Word ivar = 0, storage = 0;
  if (!r.u32(0x1ed7777c4ULL + s[Slide], ivar)) return;
  if (ivar < 8 || ivar > 0x100 || (ivar & 7)) { s[Error] = 0x10008; return; }
  if (!r.word(s[Array] + ivar, storage) ||
      !r.u32(s[Array] + ivar + 8, s[FirstSlot]) ||
      !r.u32(s[Array] + ivar + 12, s[Capacity]) ||
      !r.u32(s[Array] + ivar + 20, s[Count])) return;
  if (!s[Count]) return;
  if (s[Count] > 64 || s[Capacity] > 4096 || s[FirstSlot] >= s[Capacity] ||
      s[Count] > s[Capacity]) {
    s[Error] = 0x10009; return;
  }
  s[Phase] = 6;
  // Walk every wrapper in the circular buffer; a failed read leaves that
  // entry's remaining fields zero and never stops the census.
  unsigned total = s[Count] < kMaxEntries ? (unsigned)s[Count] : kMaxEntries;
  for (unsigned i = 0; i != total; ++i) {
    Word wrapper = 0;
    unsigned slot = ((unsigned)s[FirstSlot] + i) % (unsigned)s[Capacity];
    if (!r.word(storage + (Word)slot * 8, wrapper)) continue;
    inspectEntry(r, s[Slide], wrapper, out[i]);
  }
  s[Phase] = 7;
  inspectPool(r, s[Slide], *poolOut);
  s[Phase] = 8;
}
// Private osfmk layouts are not on the EXPORT_HDRS compile path; the probe
// uses raw offsets extracted with llvm-dwarfdump from the built kernel's own
// object files (mk_timer.o, ipc_port.o, ipc_entry.o, ipc_space.h, task.o):
//   task.itk_space            +0x328
//   ipc_space.is_table        +0x20   (entries at offset 0, 0x18 bytes each)
//   ipc_entry.ie_object       +0x00, ie_bits +0x08
//   ipc_port.io_bits          +0x00   (otype = (bits & 0x7fff0000) >> 16,
//                                       kotype = bits & 0x3ff, IOT_PORT = 0)
//   ipc_port.ip_messages      +0x20, imq_msgcount (u16) +0x10
//   ipc_port.ip_kobject       +0x48
//   mk_timer.mkt_thread_call  +0x10, is_dead +0x90, is_armed +0x91,
//                             active (int) +0x94, port +0x98
extern "C" uint64_t thread_call_get_armed_deadline(void *call);
// r211 returned n=0 because this window only covered the kernel image at
// 0xfffffff0. The live kernel places kalloc objects across 0xfffffff1..
// 0xfffffff5 (r211 E5STATE svc/m1608/m1640/m1658/m1868), so itk_space and
// its table failed the guard and the census bailed before the first entry.
static bool kernelPointer(Word p) { return p >= 0xfffffff000000000ULL && p < 0xfffffff800000000ULL; }
static unsigned timerPortFlags(Word kobject, Word &deadline) {
  deadline = 0;
  if (!kernelPointer(kobject)) return 0xffffffffULL;
  unsigned flags = (unsigned)*(const unsigned char *)(kobject + 0x90) |
                   ((unsigned)*(const unsigned char *)(kobject + 0x91) << 8) |
                   ((unsigned)*(const volatile int *)(kobject + 0x94) << 16);
  deadline = thread_call_get_armed_deadline((void *)(kobject + 0x10));
  return flags;
}
static void ipcCensus(task_t task, Word caPortName, IpcState &out) {
  bzero(&out, sizeof(out));
  if (!kernelPointer((Word)task)) return;
  Word space = *(const Word *)((const char *)task + 0x328);
  if (!kernelPointer(space)) return;
  Word table = *(const volatile Word *)((const char *)space + 0x20);
  if (!kernelPointer(table)) return;
  unsigned caIdx = caPortName ? (unsigned)(caPortName >> 8) : 0;
  IpcPortLine worst[4] = {};
  unsigned idle = 0;
  for (unsigned i = 1; i < 2048; ++i) {
    const char *entry = (const char *)table + (Word)i * 0x18;
    Word object = *(const volatile Word *)(entry + 0);
    if (!object || !kernelPointer(object)) {
      if (++idle > 96) break;
      continue;
    }
    idle = 0;
    unsigned bits = *(const volatile unsigned *)object;  // io_bits at +0
    if (((bits & 0x7fff0000U) >> 16) != 0U) continue;    // IOT_PORT = 0
    ++out.ports;
    unsigned kotype = bits & 0x3ffU;
    unsigned msgs = *(const volatile unsigned short *)((const char *)object + 0x30);
    if (msgs) ++out.undrained;
    if (kotype == 8U) {  // IKOT_TIMER
      ++out.timers;
      out.timerMsgs += msgs;
    }
    if (i == caIdx) {
      out.caIdx = i;
      out.caMsgs = msgs;
      out.caKotype = kotype;
      if (kotype == 8U) {
        Word kobject = *(const volatile Word *)((const char *)object + 0x48);
        out.caFlags = timerPortFlags(kobject, out.caDeadline);
      }
    }
    if (msgs) {
      IpcPortLine line = { i, msgs, kotype, 0 };
      for (unsigned w = 0; w < 4; ++w) {
        if (worst[w].msgs < line.msgs) {
          IpcPortLine sink = worst[3];
          for (unsigned v = 3; v > w; --v) worst[v] = worst[v - 1];
          worst[w] = line;
          line = sink;
        }
      }
    }
  }
  out.entries = out.ports;  // live ports seen (table size is kalloc-private)
  for (unsigned w = 0; w < 4; ++w) out.top[w] = worst[w];
}
static void snapshotThreads(ProcSnaps &out) {
  // r216: read-only replacement for the r214 stop-based snapshot (which
  // leaked a reference on the thread_get_state failure path and ended that
  // boot in the os_refcnt resurrection panic, report 152225). A blocked
  // thread's saved user state is frozen on its kernel stack: take the
  // machine.ss pointers under the task lock (thread+0xf8 -> machine, ss at
  // +0; arm_saved_state flavor@0, ss_64 pc@+0x108 lr@+0xf8 sp@+0x100
  // fp@+0xf0), release, then read the frozen frames directly. No thread
  // references, no stops, no deallocations — nothing that can perturb
  // refcounts. A thread that exits between unlock and read only yields a
  // bounded, flavor-checked garbage frame.
  static const char *const wanted[kMaxProcs] = {"SpringBoard", "liveactivitiesd", "chronod"};
  bzero(&out, sizeof(out));
  for (int pid = 1; pid < 512 && out.count < kMaxProcs; ++pid) {
    proc_t process = proc_find(pid);
    if (!process) continue;
    char name[32] = {};
    proc_name(pid, name, sizeof(name));
    unsigned slot = kMaxProcs;
    for (unsigned i = 0; i != kMaxProcs; ++i)
      if (!strcmp(name, wanted[i])) { slot = i; break; }
    task_t task = TASK_NULL;
    if (slot != kMaxProcs && !proc_exiting(process)) {
      task = proc_task(process);
      if (task) task_reference(task);
    }
    proc_rele(process);
    if (!task) continue;
    ProcSnap &p = out.proc[out.count++];
    p.pid = (unsigned)pid;
    for (unsigned i = 0; i != sizeof(p.name); ++i) p.name[i] = name[i] ? name[i] : 0;
    Word saved[kMaxThreads] = {};
    unsigned count = 0;
    task_lock(task);
    Word head = (Word)task + 0x80;
    for (Word member = *(const Word *)head; member != head && count != kMaxThreads;
         member = *(const Word *)member) {
      saved[count++] = *(const Word *)(member - 0x428 + 0xf8);
    }
    task_unlock(task);
    p.threads = count;
    vm_map_t map = get_task_map(task);
    Reader r = {map, nullptr};
    for (unsigned i = 0; i != count; ++i) {
      Word ss = saved[i];
      if (!kernelPointer(ss) || *(const unsigned *)ss != 6U) continue;  // ARM_THREAD_STATE64
      ThreadSnap &t = p.thread[i];
      t.pc = *(const Word *)(ss + 0x108);
      t.lr = *(const Word *)(ss + 0xf8);
      t.sp = *(const Word *)(ss + 0x100);
      if (t.pc < 0x1000 || t.pc >= 0x8000000000ULL) { t.pc = t.lr = t.sp = 0; continue; }
      Word fp = *(const Word *)(ss + 0xf0);
      for (unsigned d = 0; d != kFrames; ++d) {
        Word next = 0, ret = 0;
        if (fp < 0x1000 || fp >= 0x8000000000ULL) break;
        if (!r.probeBytes(fp, &next, sizeof(next)) || !r.probeBytes(fp + 8, &ret, sizeof(ret))) break;
        if (ret < 0x1000 || ret >= 0x8000000000ULL) break;
        t.ret[d] = ret;
        if (next <= fp || next - fp > 0x200000) break;  // stacks grow down
        fp = next;
      }
    }
    task_deallocate(task);
  }
}
static void run(thread_call_param_t, thread_call_param_t) {
  // The request busy flag serializes workers. Keep the multi-KiB census out
  // of the kernel stack, including while vm_map_read_user faults in pages.
  static Word state[Fields];
  static EntryState snapshot[kMaxEntries];
  static PoolState poolSnapshot;
  static IpcState ipcSnapshot;
  static ProcSnaps threadSnapshot;
  bzero(state, sizeof(state));
  bzero(snapshot, sizeof(snapshot));
  bzero(&poolSnapshot, sizeof(poolSnapshot));
  bzero(&ipcSnapshot, sizeof(ipcSnapshot));
  bzero(&threadSnapshot, sizeof(threadSnapshot));
  unsigned pid = __atomic_load_n(&selectedPid, __ATOMIC_RELAXED);
  state[Pid] = pid;
  proc_t process = proc_find((int)pid);
  if (process) {
    char name[32] = {};
    proc_name((int)pid, name, sizeof(name));
    task_t task = !proc_exiting(process) && !strcmp(name, "backboardd") ? proc_task(process) : TASK_NULL;
    if (task) task_reference(task);
    proc_rele(process);
    if (task) {
      inspect(task, state, snapshot, &poolSnapshot);
      ipcCensus(task, snapshot[0].timer.portName, ipcSnapshot);
      snapshotThreads(threadSnapshot);
      task_deallocate(task);
    }
  }
  __atomic_add_fetch(&generation, 1U, __ATOMIC_ACQ_REL);
  for (unsigned i = 0; i != Fields; ++i) __atomic_store_n(&values[i], state[i], __ATOMIC_RELAXED);
  for (unsigned i = 0; i != kMaxEntries; ++i) entries[i] = snapshot[i];
  pool = poolSnapshot;
  ipc = ipcSnapshot;
  threads = threadSnapshot;
  __atomic_add_fetch(&generation, 1U, __ATOMIC_RELEASE);
  __atomic_store_n(&busy, 0U, __ATOMIC_RELEASE);
}
}

extern "C" void e5_ca_probe_request(unsigned pid) {
  if (!enabled()) return;
  static unsigned requests;
  if ((++requests & 15U) != 1U) return;
  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&busy, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
  __atomic_store_n(&selectedPid, pid, __ATOMIC_RELAXED);
  if (!worker) worker = thread_call_allocate(run, nullptr);
  if (worker) thread_call_enter(worker);
  else __atomic_store_n(&busy, 0U, __ATOMIC_RELEASE);
}

extern "C" void e5_ca_probe_dump(void) {
  if (!enabled() || e5_quiet) return;
  Word s[Fields];
  // R208 faulted in __chkstk_darwin_probe before entering this function:
  // its 0x1a60-byte frame overflowed the caller's remaining kernel stack.
  // A separate, serialized scratch copy preserves the generation check.
  static EntryState local[kMaxEntries];
  static PoolState poolLocal;
  static IpcState ipcLocal;
  static ProcSnaps threadLocal;
  static unsigned dumping;
  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&dumping, &expected, 1U, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  unsigned before = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
  if (!before || (before & 1U)) {
    __atomic_store_n(&dumping, 0U, __ATOMIC_RELEASE);
    return;
  }
  for (unsigned i = 0; i != Fields; ++i) s[i] = __atomic_load_n(&values[i], __ATOMIC_RELAXED);
  for (unsigned i = 0; i != kMaxEntries; ++i) local[i] = entries[i];
  poolLocal = pool;
  ipcLocal = ipc;
  threadLocal = threads;
  if (before != __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&dumping, 0U, __ATOMIC_RELEASE);
    return;
  }
  log("E5CA g=%u pid=%llu ph=%llu kr=%llx sl=%llx ws=%llx arr=%llx cls=%llx fi=%llu cap=%llu n=%llu vc=%llx rf=%llx af=%llx ic=%llx ts=%llx",
      before,s[Pid],s[Phase],s[Error],s[Slide],s[WindowServer],s[Array],s[ArrayClass],s[FirstSlot],s[Capacity],s[Count],
      poolLocal.virtCount,poolLocal.refFB,poolLocal.actFB,poolLocal.infoCount,poolLocal.timerSerial);
  for (unsigned i = 0; i != kMaxSlots; ++i) {
    const SlotState &p = poolLocal.slot[i];
    if (!p.present) continue;
    log("E5CAP i=%u nm=%s ty=%llx ix=%llx id=%llx fg=%llx",
        i, p.text[0] ? p.text : "-", p.type, p.vidx, p.id, p.flag);
  }
  unsigned total = s[Count] < kMaxEntries ? (unsigned)s[Count] : kMaxEntries;
  for (unsigned i = 0; i != total; ++i) {
    const EntryState &e = local[i];
    if (!e.wrapper && !e.server && !e.display) continue;
    log("E5CAD i=%u w=%llx isa=%llx srv=%llx svt=%llx dpy=%llx dvt=%llx md=%llx fl=%llx f2=%llx en=%llx pwr=%llx p32=%llx con=%llx tt=%llx mc=%llx g28=%llx",
        i,e.wrapper,e.isa,e.server,e.svt,e.display,e.dvt,e.mode,e.flags,e.flags2,e.enabled,e.powerPtr,e.power,e.conn,e.timingType,e.modeCount,e.gate28);
    for (unsigned c = 0; c != e.conns && c < kMaxConns; ++c) {
      const ConnState &k = e.connState[c];
      log("E5CCS i=%u n=%u off=%llx con=%llx io=%llx ty=%llx sb=%llx sw=%llx ev=%llx rl=%llx bm=%llx k5c=%llx k5x=%llx k5p=%llx k5s=%llx de=%llx",
          i,c,k.offset,k.conn,k.io,k.type,k.slot880,k.slot8a8,k.slot8e0,k.slot9d8,k.bm,
          k.k5cb,k.k5aux,k.k5port,k.k5src,k.de8);
    }
    if (isServerVtable(e.svt, s[Slide])) {
      log("E5CAS i=%u sw=%llx ctx=%llx gpu=%llx ti=%llx vs=%llx fi=%llx d=%llx sh=%llx tc9=%llx s340=%llx",
          i,e.ssw,e.sctx,e.smetal,e.stimer,e.svsync,e.sfi,e.sdisabled,e.sshutdown,e.tc9,e.s340);
      log("E5CASV i=%u x398=%llx x39c=%llx x39d=%llx s338=%llx s360=%llx s370=%llx lb=%llx",
          i,e.s398,e.s39c,e.s39d,e.s338,e.s360,e.s370,e.latch);
      log("E5CASR i=%u th=%llx:%llx rl=%llx:%llx cxs=%llx:%llx",
          i,e.s1f0,e.s1f8,e.s2e0,e.s2e8,e.ctxList,e.ctxCount);
      for (unsigned k = 0; k != 4 && k < e.ctxCount; ++k) {
        const EntryState::CtxEntry &t = e.ctxEntry[k];
        if (!t.entry) break;
        log("E5CAC i=%u k=%u e=%llx p=%llx c8=%llx fc=%llx d10=%llx",
            i,k,t.entry,t.ptr,t.w8,t.flags,t.w10);
      }
      if (e.stimer) {
        const TimerState &t = e.timer;
        log("E5CAT i=%u pn=%llx cfp=%llx x3=%llx hook=%llx c8=%llx c9=%llx d0=%llx d8=%llx e0=%llx e8=%llx",
            i,t.portName,t.cfmach,t.third,t.hook,t.c8,t.c9,t.d0,t.d8,t.e0,t.e8);
      }
    }
  }
  log("E5IPC n=%llu np=%llu nt=%llu tmsg=%llu und=%llu ca=%llu:%llx:%llx:%llx:%llx",
      ipcLocal.entries,ipcLocal.ports,ipcLocal.timers,ipcLocal.timerMsgs,ipcLocal.undrained,
      ipcLocal.caIdx,ipcLocal.caMsgs,ipcLocal.caKotype,ipcLocal.caFlags,ipcLocal.caDeadline);
  for (unsigned w = 0; w < 4; ++w) {
    if (!ipcLocal.top[w].msgs) break;
    log("E5IPCP w=%u idx=%llx m=%llx kt=%llx",
        w,ipcLocal.top[w].idx,ipcLocal.top[w].msgs,ipcLocal.top[w].kotype);
  }
  for (unsigned i = 0; i != threadLocal.count && i != kMaxProcs; ++i) {
    const ProcSnap &p = threadLocal.proc[i];
    log("E5THP i=%u pid=%u name=%s n=%u", i, p.pid, p.name[0] ? p.name : "-", p.threads);
    for (unsigned j = 0; j != p.threads && j != kMaxThreads; ++j) {
      const ThreadSnap &t = p.thread[j];
      log("E5TH i=%u j=%u pc=%llx lr=%llx sp=%llx", i, j, t.pc, t.lr, t.sp);
      log("E5THF i=%u j=%u r=%llx,%llx,%llx,%llx,%llx,%llx",
          i, j, t.ret[0], t.ret[1], t.ret[2], t.ret[3], t.ret[4], t.ret[5]);
    }
  }
  __atomic_store_n(&dumping, 0U, __ATOMIC_RELEASE);
}
