// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, opt-in snapshots of the pinned 21H461 backboardd display server.
// Reads run on a separate thread_call, never on the media completion thread.
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

namespace {
using Word = unsigned long long;
enum Field {
  Phase, Error, Pid, Slide, WindowServer, Array, ArrayClass, Count,
  Wrapper, Server, Display, ServerVtable, DisplayVtable, Mode, Flags,
  Enabled, Power, Benchmark, Software, Context, Metal, Timer, Vsync,
  FrameInfo, Connection, Fields
};
static Word values[Fields];
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
static void inspect(task_t task, Word *s) {
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
  Word ivar = 0, storage = 0, first = 0, capacity = 0;
  if (!r.u32(0x1ed7777c4ULL + s[Slide], ivar)) return;
  if (ivar < 8 || ivar > 0x100 || (ivar & 7)) { s[Error] = 0x10008; return; }
  if (!r.word(s[Array] + ivar, storage) ||
      !r.u32(s[Array] + ivar + 8, first) ||
      !r.u32(s[Array] + ivar + 12, capacity) ||
      !r.u32(s[Array] + ivar + 20, s[Count])) return;
  if (!s[Count]) return;
  if (s[Count] > 64 || capacity > 4096 || first >= capacity || s[Count] > capacity) {
    s[Error] = 0x10009; return;
  }
  s[Phase] = 6;
  Word isa = 0;
  if (!r.word(storage + first * 8, s[Wrapper]) || !s[Wrapper] ||
      !r.word(s[Wrapper], isa)) return;
  if ((isa & 0xffffffff8ULL) != 0x1ed7aaed8ULL + s[Slide]) {
    s[Error] = 0x1000a; return;
  }
  if (!r.word(s[Wrapper] + 8, implementation) || !implementation ||
      !r.word(implementation + 0x40, s[Server]) || !s[Server]) return;
  s[Phase] = 7;
  if (!r.word(s[Server], s[ServerVtable])) return;
  const bool accel = s[ServerVtable] == 0x1e691a648ULL + s[Slide];
  if (!accel && s[ServerVtable] != 0x1e691a8b0ULL + s[Slide]) {
    s[Error] = 0x1000b; return;
  }
  if (!r.word(s[Server] + 0x60, s[Display]) || !s[Display]) return;
  s[Phase] = 8;
  if (!r.word(s[Display], s[DisplayVtable])) return;
  if (s[DisplayVtable] != 0x1e691b360ULL + s[Slide]) {
    s[Error] = 0x1000c; return;
  }
  s[Phase] = 9;
  Word power = 0;
  if (!r.word(s[Display] + 0x248, s[Mode]) ||
      !r.u32(s[Display] + 0x2a1, s[Flags]) ||
      !r.u16(s[Display] + 0x6bcd, s[Enabled]) ||
      !r.word(s[Display] + 0x290, power)) return;
  if (power && !r.u32(power, s[Power])) return;
  s[Phase] = 10;
  if (!r.word(s[Server] + 0xc8, s[Software]) ||
      !r.word(s[Server] + 0xc0, s[Context]) ||
      !r.word(s[Server] + 0x300, s[Timer]) ||
      !r.word(s[Server] + 0x2f0, s[Vsync]) ||
      !r.word(s[Server] + 0x330, s[FrameInfo])) return;
  if (accel && !r.word(s[Server] + 0x3b0, s[Metal])) return;
  s[Phase] = 11;
  if (!r.word(s[Display] + 0x6378, s[Connection]) || !s[Connection] ||
      !r.u8(s[Connection] + 0xca8, s[Benchmark])) return;
  s[Phase] = 12;
}
static void run(thread_call_param_t, thread_call_param_t) {
  Word state[Fields] = {};
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
      inspect(task, state);
      task_deallocate(task);
    }
  }
  __atomic_add_fetch(&generation, 1U, __ATOMIC_ACQ_REL);
  for (unsigned i = 0; i != Fields; ++i) __atomic_store_n(&values[i], state[i], __ATOMIC_RELAXED);
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
  unsigned before = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
  if (!before || (before & 1U)) return;
  for (unsigned i = 0; i != Fields; ++i) s[i] = __atomic_load_n(&values[i], __ATOMIC_RELAXED);
  if (before != __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) return;
  log("E5CA g=%u pid=%llu ph=%llu kr=%llx sl=%llx ws=%llx arr=%llx cls=%llx n=%llu srv=%llx dpy=%llx",
      before,s[Pid],s[Phase],s[Error],s[Slide],s[WindowServer],s[Array],s[ArrayClass],s[Count],s[Server],s[Display]);
  log("E5CAX g=%u mode=%llx fl=%llx en=%llx pwr=%llx bm=%llx sw=%llx ctx=%llx gpu=%llx timer=%llx src=%llx fi=%llx",
      before,s[Mode],s[Flags],s[Enabled],s[Power],s[Benchmark],s[Software],s[Context],s[Metal],s[Timer],s[Vsync],s[FrameInfo]);
}
