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
extern "C" vm_size_t ml_nofault_copy(vm_offset_t, vm_offset_t, vm_size_t);
extern "C" uint64_t mach_absolute_time(void);

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
struct ThreadSnap {
  Word pc, lr, sp, fp, tid, tsd, port, savedState, flavor, copied, status, cpu;
  Word kernelState, waitEvent, continuation;
  Word arg[8];
  Word ret[24];
  // r233: path-like ASCII runs captured around the saved stack pointer —
  // names the file/bundle a scan thread is chewing on (lsd seeding).
  char stk[2][48];
};
struct ProcSnap {
  unsigned pid, threads, uid, images;
  char name[20];
  Word imageLoad[16], imagePath[16];
  char imageText[16][48];
  Word slide, dyldStatus, lockBefore, lockAfter, lockReadBefore, lockReadAfter;
  Word stamp, seedGroup, seedIsa, seedBefore, seedAfter, seedReadBefore, seedReadAfter;
  ThreadSnap thread[12];
};
static const unsigned kMaxProcs = 10;
static const unsigned kMaxThreads = 12;
static const unsigned kFrames = 24;
struct ProcSnaps { unsigned count; ProcSnap proc[kMaxProcs]; };
// r219: rolling sample of each proc's first-thread pc across worker runs —
// distinguishes a CPU spin (varying pc) from a frozen/stuck thread (constant).
struct PcSample { unsigned pid; Word tid, pc, lr, cpu, stamp; };
static const unsigned kPcHistory = 16;
struct PcHistory { unsigned count; PcSample sample[kPcHistory]; };
// r233: the seeding-group raw word sampled across worker runs — one number
// per census with a timestamp decides "progressing (count falling)" vs
// "frozen (bit-identical for the whole window)" without console flooding.
struct SeedSample { Word ts, raw, after, lock; };
static const unsigned kSeedHistory = 24;
struct SeedHistory { unsigned count; SeedSample s[kSeedHistory]; };
// Kernel-side IPC census of the probed task: how many mk_timer ports exist,
// whether any fired-but-undrained messages sit in their queues, and the
// armed deadline of the CA display timer's own port.
struct IpcPortLine { Word idx, msgs, kotype, armed; };
struct IpcState {
  Word entries, ports, timers, timerMsgs, undrained;
  Word caIdx, caMsgs, caKotype, caDeadline, caFlags;
  // r233: where the census actually stopped — distinguishes a bad space/
  // table deref (n=0, sp=0/tp=0) from an emptied table walk.
  Word spacePtr, tablePtr, caPortName, idleAt;
  IpcPortLine top[4];
};
static Word values[Fields];
static EntryState entries[kMaxEntries];
static PoolState pool;
static IpcState ipc;
static ProcSnaps threads;
static PcHistory pcHistory;
static SeedHistory seedHistory;
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
  out.caPortName = caPortName;
  if (!kernelPointer((Word)task)) return;
  Word space = *(const Word *)((const char *)task + 0x328);
  out.spacePtr = space;
  if (!kernelPointer(space)) return;
  Word table = *(const volatile Word *)((const char *)space + 0x20);
  out.tablePtr = table;
  if (!kernelPointer(table)) return;
  unsigned caIdx = caPortName ? (unsigned)(caPortName >> 8) : 0;
  IpcPortLine worst[4] = {};
  unsigned idle = 0;
  for (unsigned i = 1; i < 2048; ++i) {
    const char *entry = (const char *)table + (Word)i * 0x18;
    Word object = *(const volatile Word *)(entry + 0);
    if (!object || !kernelPointer(object)) {
      if (++idle > 96) { out.idleAt = i; break; }
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
// r227: kernel stacks can lie outside the old kalloc-window predicate. Copy
// only translated, wired kernel bytes, while task_lock keeps list members live.
// The helper never faults in a page or treats a failed read as valid zero state.
static bool kernelRead(Word address, void *out, unsigned size) {
  return address >= 0xffffff8000000000ULL && size && size <= 0x200 &&
         address <= ~0ULL - size &&
         ml_nofault_copy((vm_offset_t)address, (vm_offset_t)out, size) == size;
}
static void recordPcHistory(const ProcSnaps &snaps) {
  static PcHistory buffer;
  for (unsigned i = 0; i != snaps.count && i != kMaxProcs; ++i) {
    const ProcSnap &p = snaps.proc[i];
    const bool springboard = !strcmp(p.name, "SpringBoard");
    const bool lsd = !strcmp(p.name, "lsd");
    if (!springboard && !lsd) continue;
    // The SB main thread plus lsd's first four threads — the lsd scan
    // workers are TH_RUN and only their pc/cpu trajectory distinguishes
    // forward progress from a spin or a scheduling stall.
    unsigned limit = springboard ? 1u : 4u;
    if (limit > p.threads) limit = p.threads;
    for (unsigned j = 0; j < limit; ++j) {
      if (buffer.count == kPcHistory) {
        for (unsigned k = 1; k != kPcHistory; ++k) buffer.sample[k - 1] = buffer.sample[k];
        --buffer.count;
      }
      buffer.sample[buffer.count++] = PcSample{p.pid, p.thread[j].tid,
          p.thread[j].pc, p.thread[j].lr, p.thread[j].cpu, p.stamp};
    }
    if (lsd) {
      static SeedHistory buffer2;
      if (buffer2.count == kSeedHistory) {
        for (unsigned k = 1; k != kSeedHistory; ++k) buffer2.s[k - 1] = buffer2.s[k];
        --buffer2.count;
      }
      buffer2.s[buffer2.count++] = SeedSample{p.stamp, p.seedBefore, p.seedAfter, p.lockBefore};
      seedHistory = buffer2;
    }
  }
  pcHistory = buffer;
}
static void snapshotThreads(ProcSnaps &out) {
  static const char *const wanted[] = {"SpringBoard", "liveactivitiesd", "chronod", "backboardd", "lsd",
                                      "launchd", "logd", "runningboardd", "watchdogd"};
  bzero(&out, sizeof(out));
  for (int pid = 1; pid < 512 && out.count < kMaxProcs; ++pid) {
    proc_t process = proc_find(pid);
    if (!process) continue;
    char name[32] = {};
    proc_name(pid, name, sizeof(name));
    unsigned slot = kMaxProcs;
    for (unsigned i = 0; i != sizeof(wanted) / sizeof(wanted[0]); ++i)
      if (!strcmp(name, wanted[i])) { slot = i; break; }
    unsigned uid = proc_getuid(process);
    task_t task = TASK_NULL;
    if (slot != kMaxProcs && !proc_exiting(process)) {
      task = proc_task(process);
      if (task) task_reference(task);
    }
    proc_rele(process);
    if (!task) continue;
    ProcSnap &p = out.proc[out.count++];
    p.pid = (unsigned)pid;
    p.uid = uid;
    p.stamp = mach_absolute_time();
    for (unsigned i = 0; i != sizeof(p.name); ++i) p.name[i] = name[i] ? name[i] : 0;
    vm_map_t map = get_task_map(task);
    Reader r = {map, nullptr};
    const bool springboard = !strcmp(name, "SpringBoard");
    const bool lsd = !strcmp(name, "lsd");
    const bool deep = springboard || lsd;
    if (deep) {
      task_dyld_info_data_t dyld = {};
      mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
      p.dyldStatus = (unsigned)task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld, &count);
      uint8_t infos[184] = {};
      static const uint8_t cacheUuid[16] = {
        0x59,0x0d,0x83,0x9a,0x82,0xd2,0x30,0xf1,0xa1,0x7d,0x0c,0xc0,0x53,0x42,0x0f,0x73};
      if (!p.dyldStatus && dyld.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_64 &&
          dyld.all_image_info_size >= sizeof(infos) &&
          r.probeBytes(dyld.all_image_info_addr, infos, sizeof(infos))) {
        memcpy(&p.slide, infos + 0x98, sizeof(p.slide));
        if (p.slide >= 0x40000000 || memcmp(infos + 0xa0, cacheUuid, 16)) p.dyldStatus = 0x10005;
      } else if (!p.dyldStatus) p.dyldStatus = 0x10002;
      if (!p.dyldStatus) {
        // r231: lsd's pinned scan threads run in PRIVATELY loaded modules
        // (r230b: slid pc 0x1d312abdc -> unslid 0x15976abdc, below the cache
        // base). all_image_infos (user64 layout) keeps infoArrayCount at +0x8
        // and infoArray at +0x10; each entry leads with the image load
        // address at +0x0 and the path pointer at +0x28. Record the array so
        // the host maps the pinned pc to a module; this is the branch point
        // between "sqlite row I/O (preseed/speed up storage)" and "lock spin
        // (name the owner)" — the UI goal has no use for another window.
        Word arrayPtr = 0, arrayCount = 0;
        // r232: user64 layout is infoArrayCount@+4 (u32) and infoArray@+8;
        // r231 read the pointer as the count so the guard never opened.
        uint32_t arrayCount32 = 0;
        memcpy(&arrayCount32, infos + 4, 4);
        memcpy(&arrayPtr, infos + 8, 8);
        arrayCount = arrayCount32;
        if (arrayPtr && arrayCount && arrayCount < 512) {
          for (unsigned e = 0; e < arrayCount && p.images < 16; ++e) {
            Word entry = arrayPtr + 21 * e * 8;  // dyld_image_info: 3 pointers
            Word loadAddress = 0, pathPtr = 0, pathAlt = 0;
            if (!r.probeBytes(entry, &loadAddress, 8)) break;
            // r232 delivered a=… p=0 t=- for every entry: the +0x10 slot read
            // zero on this dyld. r233 reads +8 first (user-filed correction),
            // keeps +0x10 as a fallback, and trusts whichever decodes to
            // printable ASCII on the host side.
            if (!r.probeBytes(entry + 8, &pathPtr, 8)) pathPtr = 0;
            if (!r.probeBytes(entry + 0x10, &pathAlt, 8)) pathAlt = 0;
            char textA[48] = {}, textB[48] = {};
            if (pathPtr) r.probeBytes(pathPtr, textA, sizeof(textA) - 1);
            if (pathAlt) r.probeBytes(pathAlt, textB, sizeof(textB) - 1);
            bool okA = false, okB = false;
            for (unsigned c = 0; c != sizeof(textA); ++c) { if (!textA[c]) break; if (textA[c] < 0x20 || textA[c] >= 0x7f) { okA = false; break; } okA = c + 1 >= 4; }
            for (unsigned c = 0; c != sizeof(textB); ++c) { if (!textB[c]) break; if (textB[c] < 0x20 || textB[c] >= 0x7f) { okB = false; break; } okB = c + 1 >= 4; }
            if (!okA) pathPtr = 0;
            if (!okB) pathAlt = 0;
            if (!pathPtr && pathAlt) pathPtr = pathAlt;
            p.imageLoad[p.images] = loadAddress;
            p.imagePath[p.images] = pathPtr;
            for (unsigned c = 0; c != sizeof(p.imageText[0]); ++c)
              p.imageText[p.images][c] = 0;
            if (pathPtr) r.probeBytes(pathPtr, p.imageText[p.images], sizeof(p.imageText[0]) - 1);
            ++p.images;
          }
        }
        p.lockReadBefore = r.probeBytes(0x1f4884878ULL + p.slide, &p.lockBefore, 8);
        // Original LSDatabaseGetSeedingGroup global. The object's first word
        // is the OS_dispatch_group CLASS at 0x1f4844bf0+slide — r228 compared
        // the VTABLE (0x1f4844c18, __OS_dispatch_group_vtable), 0x28 higher,
        // so the check never passed and the count read as zeros. The count/
        // generation word at +0x30 is proven by _dispatch_group_enter
        // 0x18f5590d0 (atomic on [x0+0x30], -4 per enter); the RAW value and
        // its deltas are the rebuild progress evidence.
        if (lsd && r.probeBytes(0x1ed7756e8ULL + p.slide, &p.seedGroup, 8) &&
            p.seedGroup && r.probeBytes(p.seedGroup, &p.seedIsa, 8) &&
            p.seedIsa == 0x1f4844bf0ULL + p.slide)
          p.seedReadBefore = r.probeBytes(p.seedGroup + 0x30, &p.seedBefore, 8);
      }
    }
    // Offsets checked in the pinned build's thread.o DWARF and pcb.o code:
    // task.threads=0x80, thread.task_threads=0x428 (container queue),
    // machine=0xf8, upcb=machine+0x18, cthread_self=machine+0x50,
    // thread_id=0x508. Copy saved registers BEFORE releasing task_lock.
    task_lock(task);
    Word head = (Word)task + 0x80, th = 0;
    unsigned count = 0;
    if (kernelRead(head, &th, 8)) {
      while (th != head && th && count != kMaxThreads) {
        ThreadSnap &t = p.thread[count++];
        Word next = 0;
        if (!kernelRead(th + 0x428, &next, 8)) { t.status = 5; break; }
        kernelRead(th + 0x508, &t.tid, 8);
        // cpu_time_eqos_stats @ thread+0x4f8 (thread.o DWARF): eight 64-bit
        // per-QOS mach-absolute accumulators; their sum is the thread's total
        // CPU time. Read raw under the task lock like every other field.
        for (unsigned q = 0; q != 8; ++q) {
          Word v = 0;
          if (kernelRead(th + 0x4f8 + 8 * q, &v, 8)) t.cpu += v;
        }
        kernelRead(th + 0x1f8, &t.kernelState, 4);
        kernelRead(th + 0x20, &t.waitEvent, 8);
        kernelRead(th + 0xd8, &t.continuation, 8);
        kernelRead(th + 0x148, &t.tsd, 8);
        if (!kernelRead(th + 0x110, &t.savedState, 8)) t.status = 1;
        else {
          uint8_t saved[0x110] = {};
          t.copied = ml_nofault_copy((vm_offset_t)t.savedState, (vm_offset_t)saved, sizeof(saved));
          if (t.copied != sizeof(saved)) t.status = 2;
          else {
            uint32_t flavor = 0;
            memcpy(&flavor, saved, 4); t.flavor = flavor;
            if (flavor != 21) t.status = 3;
            else {
              memcpy(t.arg, saved + 8, sizeof(t.arg));
              memcpy(&t.fp, saved + 0xf0, 8);
              memcpy(&t.lr, saved + 0xf8, 8);
              memcpy(&t.sp, saved + 0x100, 8);
              memcpy(&t.pc, saved + 0x108, 8);
              if (t.pc < 0x1000 || t.pc >= 0x8000000000ULL) t.status = 4;
            }
          }
        }
        th = next;
      }
    }
    task_unlock(task);
    p.threads = count;
    // r275: new boot-graph targets use only no-fault kernel snapshots.
    // Do not fault their userspace stacks in merely to observe a slow boot.
    if (slot >= 5) {
      task_deallocate(task);
      continue;
    }
    for (unsigned i = 0; i != count; ++i) {
      ThreadSnap &t = p.thread[i];
      // cthread_self is TPIDRRO_EL0, i.e. the TSD base. Original pthread and
      // both os_unfair_recursive_lock variants read the self port at +0x18.
      uint32_t port = 0;
      if (t.tsd && r.probeBytes(t.tsd + 0x18, &port, sizeof(port))) t.port = port;
      if (t.status) continue;
      Word fp = t.fp;
      const unsigned depth = deep ? kFrames : 6;
      for (unsigned d = 0; d != depth; ++d) {
        Word next = 0, ret = 0;
        if (fp < 0x1000 || fp >= 0x8000000000ULL || (fp & 15)) break;
        if (!r.probeBytes(fp, &next, 8) || !r.probeBytes(fp + 8, &ret, 8)) break;
        if (ret < 0x1000 || ret >= 0x8000000000ULL) break;
        t.ret[d] = ret;
        if (next <= fp || next - fp > 0x200000) break;
        fp = next;
      }
      // r233: harvest path-like ASCII from the live stack window above sp.
      // A scan thread holds the bundle/file string it is working on within
      // a few hundred bytes of its frame; two longest runs are enough to
      // name the tar pit (e.g. a SeymourServices asset) for image surgery.
      if (deep && !t.status && t.sp >= 0x1000 && t.sp < 0x8000000000ULL) {
        static uint8_t window[0x500];
        bzero(window, sizeof(window));
        if (r.probeBytes(t.sp, window, sizeof(window))) {
          unsigned bestLen[2] = {0, 0};
          unsigned run = 0;
          for (unsigned off = 0; off <= sizeof(window); ++off) {
            const uint8_t c = off < sizeof(window) ? window[off] : 0;
            const bool printable = c >= 0x20 && c < 0x7f;
            if (printable) { ++run; continue; }
            if (run >= 12) {
              // Path-like: contains '/' or a bundle-ish extension.
              const char *s = (const char *)window + off - run;
              bool pathy = false;
              for (unsigned q = 0; q + 3 < run; ++q)
                if (s[q] == '/') { pathy = true; break; }
              if (!pathy)
                for (unsigned q = 0; q + 4 <= run; ++q) {
                  if (q + 8 <= run && !memcmp(s + q, ".strings", 8)) { pathy = true; break; }
                  if (q + 7 <= run && !memcmp(s + q, ".bundle", 7)) { pathy = true; break; }
                  if (q + 5 <= run && !memcmp(s + q, ".lsvb", 5)) { pathy = true; break; }
                  if (!memcmp(s + q, ".app", 4) || !memcmp(s + q, ".car", 4) ||
                      !memcmp(s + q, ".pdf", 4) || !memcmp(s + q, ".png", 4) ||
                      (q + 3 <= run && !memcmp(s + q, ".db", 3))) { pathy = true; break; }
                }
              if (pathy) {
                unsigned slot = 2;
                if (run > bestLen[0]) slot = 0;
                else if (run > bestLen[1]) slot = 1;
                if (slot == 0) {
                  memcpy(t.stk[1], t.stk[0], sizeof(t.stk[1]));
                  bestLen[1] = bestLen[0];
                }
                if (slot < 2) {
                  unsigned len = run < sizeof(t.stk[0]) - 1 ? run : sizeof(t.stk[0]) - 1;
                  // Keep the TAIL of long paths (the filename side).
                  memcpy(t.stk[slot], s + run - len, len);
                  bestLen[slot] = run;
                }
              }
            }
            run = 0;
          }
        }
      }
    }
    if (deep && !p.dyldStatus) {
      p.lockReadAfter = r.probeBytes(0x1f4884878ULL + p.slide, &p.lockAfter, 8);
      if (lsd && p.seedReadBefore)
        p.seedReadAfter = r.probeBytes(p.seedGroup + 0x30, &p.seedAfter, 8);
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
      task_deallocate(task);
    }
  }
  // r275: also snapshot when selectedPid is absent/exited; the periodic
  // entry must not depend on proc_find(0) succeeding.
  snapshotThreads(threadSnapshot);
  recordPcHistory(threadSnapshot);
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

// r245: unconditional periodic entry from the display paint tick. Runs the
// census worker with selectedPid 0 - no CA-pool inspect, but the thread/LS
// snapshot (SpringBoard/backboardd/lsd/... states, seeding-group counter,
// PC history) executes on every call and lands in the next dump.
extern "C" void e5_ca_probe_periodic(void) {
  if (!enabled()) return;
  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&busy, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
  __atomic_store_n(&selectedPid, 0U, __ATOMIC_RELAXED);
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
  static PcHistory pcLocal;
  static SeedHistory seedLocal;
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
  pcLocal = pcHistory;
  seedLocal = seedHistory;
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
  log("E5IPC n=%llu np=%llu nt=%llu tmsg=%llu und=%llu ca=%llu:%llx:%llx:%llx:%llx sp=%llx tp=%llx pn=%llx idle=%llu",
      ipcLocal.entries,ipcLocal.ports,ipcLocal.timers,ipcLocal.timerMsgs,ipcLocal.undrained,
      ipcLocal.caIdx,ipcLocal.caMsgs,ipcLocal.caKotype,ipcLocal.caFlags,ipcLocal.caDeadline,
      ipcLocal.spacePtr,ipcLocal.tablePtr,ipcLocal.caPortName,ipcLocal.idleAt);
  for (unsigned w = 0; w < 4; ++w) {
    if (!ipcLocal.top[w].msgs) break;
    log("E5IPCP w=%u idx=%llx m=%llx kt=%llx",
        w,ipcLocal.top[w].idx,ipcLocal.top[w].msgs,ipcLocal.top[w].kotype);
  }
  for (unsigned i = 0; i != pcLocal.count && i != kPcHistory; ++i) {
    const PcSample &h = pcLocal.sample[i];
    log("E5THH k=%u pid=%u tid=%llx pc=%llx lr=%llx cpu=%llx ts=%llx", i,h.pid,h.tid,h.pc,h.lr,h.cpu,h.stamp);
  }
  for (unsigned k = 0; k != seedLocal.count && k < kSeedHistory; ++k)
    log("E5LSDH k=%u ts=%llx raw=%llx af=%llx lk=%llx", k,
        seedLocal.s[k].ts, seedLocal.s[k].raw, seedLocal.s[k].after, seedLocal.s[k].lock);
  for (unsigned i = 0; i != threadLocal.count && i != kMaxProcs; ++i) {
    const ProcSnap &p = threadLocal.proc[i];
    log("E5THP i=%u pid=%u name=%s n=%u uid=%u ts=%llx", i,p.pid,p.name[0] ? p.name : "-",p.threads,p.uid,p.stamp);
    const bool springboard = !strcmp(p.name, "SpringBoard");
    const bool lsd = !strcmp(p.name, "lsd");
    const bool deep = springboard || lsd;
    if (lsd)
      log("E5LSD pid=%u uid=%u sl=%llx kr=%llx gp=%llx isa=%llx rd=%llu:%llu raw=%llx:%llx lk=%llx:%llx",
          p.pid,p.uid,p.slide,p.dyldStatus,p.seedGroup,p.seedIsa,p.seedReadBefore,p.seedReadAfter,p.seedBefore,p.seedAfter,p.lockBefore,p.lockAfter);
    for (unsigned e = 0; e != p.images && e != 16; ++e)
      log("E5LSDI pid=%u e=%u a=%llx p=%llx t=%.44s",
          p.pid, e, p.imageLoad[e], p.imagePath[e], p.imageText[e][0] ? p.imageText[e] : "-");
    if (springboard)
      log("E5LS pid=%u sl=%llx kr=%llx rd=%llu:%llu raw=%llx:%llx",
          p.pid, p.slide, p.dyldStatus, p.lockReadBefore, p.lockReadAfter, p.lockBefore, p.lockAfter);
    for (unsigned j = 0; j != p.threads && j != kMaxThreads; ++j) {
      const ThreadSnap &t = p.thread[j];
      log("E5TH i=%u j=%u pc=%llx lr=%llx sp=%llx tid=%llx mp=%llx tsd=%llx st=%llu ss=%llx fl=%llu nb=%llu ks=%llx we=%llx co=%llx cpu=%llx",
          i,j,t.pc,t.lr,t.sp,t.tid,t.port,t.tsd,t.status,t.savedState,t.flavor,t.copied,t.kernelState,t.waitEvent,t.continuation,t.cpu);
      if (deep || j == 0) log("E5THF i=%u j=%u r=%llx,%llx,%llx,%llx,%llx,%llx",
          i, j, t.ret[0], t.ret[1], t.ret[2], t.ret[3], t.ret[4], t.ret[5]);
      if (deep) {
        for (unsigned k = 6; k < kFrames && t.ret[k]; k += 6)
          log("E5THX i=%u j=%u k=%u r=%llx,%llx,%llx,%llx,%llx,%llx",
              i,j,k,t.ret[k],t.ret[k+1],t.ret[k+2],t.ret[k+3],t.ret[k+4],t.ret[k+5]);
        log("E5THA i=%u j=%u x=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx",
            i,j,t.arg[0],t.arg[1],t.arg[2],t.arg[3],t.arg[4],t.arg[5],t.arg[6],t.arg[7]);
      }
      if (deep && (t.stk[0][0] || t.stk[1][0]))
        log("E5THS i=%u j=%u s0=%.46s s1=%.46s",
            i, j, t.stk[0][0] ? t.stk[0] : "-", t.stk[1][0] ? t.stk[1] : "-");
    }
  }
  __atomic_store_n(&dumping, 0U, __ATOMIC_RELEASE);
}
