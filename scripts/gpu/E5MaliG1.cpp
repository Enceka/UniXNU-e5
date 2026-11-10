// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// E5MaliG1.cpp — minimal Mali-G57 JM kernel driver for the E5 XNU port.
// See E5MaliG1.hpp for scope and ../audit-report.md for every register's
// provenance. Nothing here touches the phone: bring-up is staged by boot
// args and every failure path unwinds without panicking.
//
// Integration (mirrors the existing E5Mali.cpp probe): the object file is
// archived into the E5 storage kext alongside NativeSd; one call
//
//     extern "C" int e5_mali_g1_start(void *providerService);
//
// is made from NativeSd::start() after the display service is up (the same
// place e5_mali_probe() is called today), guarded by the e5-mali-g1=1 boot
// argument. The driver needs e5_pmic_rmw()/e5_pmic_peek() from NativeSd and
// the armcpu_spi_* GIC SPI interface from ArmCpu.
//
// Board status: DELIVERED, NOT BOARD-TESTED. All hardware interactions are
// first-flash items per ../board-test-plan.md.

#include <stdarg.h>
#include <string.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
// IOMemoryMap arrives via IOService/IODeviceMemory headers in the kernel
// header subset used by this archive (E5DisplayProbe.cpp relies on the same
// transitive include; IOKit/IOMemoryMap.h is not shipped there).
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <mach/mach_types.h>
#include <kern/task.h>
#include <vm/pmap.h>
// vm/pmap.h hides these from the IOKit compile mode; signatures are native XNU.
extern "C" unsigned int pmap_cache_attributes(ppnum_t);
extern "C" void pmap_set_cache_attributes(ppnum_t, unsigned int);
extern "C" boolean_t pmap_valid_page(ppnum_t);
extern "C" vm_offset_t ml_static_ptovirt(vm_offset_t);
#include <pexpert/pexpert.h>

#include "E5MaliG1.hpp"
#include "E5MaliG1Ext.h"
static_assert(sizeof(E5MaliCacheRange) == 24, "cache ABI");
static_assert(sizeof(E5MaliGpuProperties) == 104, "properties ABI");
static_assert(sizeof(E5MaliSubmitMany) == 1056, "submit-many ABI1.6");
static_assert(sizeof(E5MaliImportUser) == 24, "import-user ABI");

// Offline-compiled Valhall v9 shader assets for self-test §4.2 (user-space
// subagent deliverable; provenance header inside the file).
#include "E5MaliG1Shaders.inc"
#include "E5GpuUserProbeImage.inc"

extern "C" int e5_gpu_user_probe_image(const uint8_t **bytes, uint64_t *size) {
    if (!bytes || !size) return 0;
    *bytes = e5GpuUserProbeImage;
    *size = sizeof(e5GpuUserProbeImage);
    return 1;
}

using namespace E5MaliG1Regs;
static void e5_publish_metal(IOService *gpu);

// ---------------------------------------------------------------------------
// External kernel interfaces (same archive / conventions as the E5Tlsc6x
// touch driver and the E5Mali probe).
// ---------------------------------------------------------------------------
extern "C" void e5_kputc(char);
extern "C" bool e5_display_gpu_scanout_owned(void);
extern "C" unsigned e5_input_presented_frames(void);
extern "C" bool e5_input_emit_frame(unsigned, const uint16_t *, size_t);
// PMIC ADI channel owned by the storage driver (NativeSd.cpp:1151-1166).
extern "C" unsigned e5_pmic_peek(unsigned off);
extern "C" int e5_pmic_rmw(unsigned off, unsigned mask, unsigned val);
// ArmCpu shared-SPI interface (ArmCpuSpi.h). Redeclared verbatim because
// that header lives in the ArmCpu component checkout.
extern "C" {
IOReturn armcpu_spi_attach_masked(uint32_t, bool, void (*)(void *), void *,
                                  uint32_t *, uint64_t *);
IOReturn armcpu_spi_set_enabled(uint32_t, bool, void *);
void armcpu_spi_mask_irq(uint32_t, void *);
IOReturn armcpu_spi_detach(uint32_t, void *);
uint64_t armcpu_spi_count(uint32_t);
}
// Kernel-internal entries for the self-test thread (same declarations the
// ArmCpu component uses for secondary-CPU bring-up).
extern "C" {
kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
kern_return_t thread_terminate(thread_t);
void thread_deallocate(thread_t);
thread_t current_thread(void);
}

namespace {

void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
void note(const char *format, ...) {
    char text[288];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    for (const char *p = text; *p; ++p) e5_kputc(*p);
    e5_kputc('\n');
}

// Exception decode: the Valhall JM JS_STATUS codes (drm_panfrost_exception_
// type, linux-lts-e5 panfrost_device.h:263+). Codes >= 0x40 are faults;
// 0x03 STOPPED / 0x04 TERMINATED are control statuses.
struct ExceptionName { uint8_t code; const char *name; };
const ExceptionName kExceptions[] = {
    {0x00, "OK"}, {0x01, "DONE"}, {0x02, "INTERRUPTED"}, {0x03, "STOPPED"},
    {0x04, "TERMINATED"}, {0x05, "KABOOM"}, {0x06, "EUREKA"}, {0x08, "ACTIVE"},
    {0x40, "JOB_CONFIG_FAULT"}, {0x41, "JOB_POWER_FAULT"}, {0x42, "JOB_READ_FAULT"},
    {0x43, "JOB_WRITE_FAULT"}, {0x44, "JOB_AFFINITY_FAULT"}, {0x48, "JOB_BUS_FAULT"},
    {0x50, "INSTR_INVALID_PC"}, {0x51, "INSTR_INVALID_ENC"}, {0x52, "INSTR_TYPE_MISMATCH"},
    {0x53, "INSTR_OPERAND_FAULT"}, {0x54, "INSTR_TLS_FAULT"}, {0x55, "INSTR_BARRIER_FAULT"},
    {0x56, "INSTR_ALIGN_FAULT"}, {0x58, "DATA_INVALID_FAULT"}, {0x59, "TILE_RANGE_FAULT"},
    {0x5a, "ADDR_RANGE_FAULT"}, {0x5b, "IMPRECISE_FAULT"}, {0x60, "OOM"},
    {0x61, "OOM_AFBC"}, {0x7f, "UNKNOWN"}, {0x80, "DELAYED_BUS_FAULT"},
    {0x88, "GPU_SHAREABILITY_FAULT"}, {0x89, "SYS_SHAREABILITY_FAULT"},
    {0x8a, "GPU_CACHEABILITY_FAULT"}, {0xc0, "TRANSLATION_FAULT_0"},
    {0xc1, "TRANSLATION_FAULT_1"}, {0xc2, "TRANSLATION_FAULT_2"},
    {0xc3, "TRANSLATION_FAULT_3"}, {0xc4, "TRANSLATION_FAULT_4"},
    {0xc7, "TRANSLATION_FAULT_IDENTITY"}, {0xc8, "PERM_FAULT_0"},
    {0xc9, "PERM_FAULT_1"}, {0xca, "PERM_FAULT_2"}, {0xcb, "PERM_FAULT_3"},
    {0xd0, "TRANSTAB_BUS_FAULT_0"}, {0xd1, "TRANSTAB_BUS_FAULT_1"},
    {0xd2, "TRANSTAB_BUS_FAULT_2"}, {0xd3, "TRANSTAB_BUS_FAULT_3"},
    {0xd8, "ACCESS_FLAG_0"}, {0xd9, "ACCESS_FLAG_1"}, {0xda, "ACCESS_FLAG_2"},
    {0xdb, "ACCESS_FLAG_3"}, {0xe0, "ADDR_SIZE_FAULT_IN0"}, {0xe1, "ADDR_SIZE_FAULT_IN1"},
    {0xe2, "ADDR_SIZE_FAULT_IN2"}, {0xe3, "ADDR_SIZE_FAULT_IN3"},
    {0xe4, "ADDR_SIZE_FAULT_OUT0"}, {0xe5, "ADDR_SIZE_FAULT_OUT1"},
    {0xe6, "ADDR_SIZE_FAULT_OUT2"}, {0xe7, "ADDR_SIZE_FAULT_OUT3"},
    {0xe8, "MEM_ATTR_FAULT_0"}, {0xe9, "MEM_ATTR_FAULT_1"},
    {0xea, "MEM_ATTR_FAULT_2"}, {0xeb, "MEM_ATTR_FAULT_3"},
    {0xec, "MEM_ATTR_NONCACHE_0"}, {0xed, "MEM_ATTR_NONCACHE_1"},
    {0xee, "MEM_ATTR_NONCACHE_2"}, {0xef, "MEM_ATTR_NONCACHE_3"},
};
const char *exceptionName(uint32_t code) {
    for (const ExceptionName &e : kExceptions)
        if (e.code == code) return e.name;
    return "UNLISTED";
}

constexpr uint32_t kMaxBoHandles = E5_MALI_MAX_BO_HANDLES;
constexpr uint32_t kMaxBoMappings = 4;      // remaps per BO
constexpr uint32_t kMaxClientAtoms = 64;
constexpr uint64_t kVaWindowDefault = 512ull << 20;
constexpr uint64_t kVaWindowBase = 0x10000000ull;   // 256 MiB .. +window
constexpr uint64_t kDramBase = 0x40000000ull, kDramEnd = 0xc0000000ull;

// Self-test slot discriminator (e5-mali-g1-selftest-slot=N, default 2):
// HakiOS completed its write job on SLOT 1 (refs/hakios_mali.c:323 "job
// slot 1, where compute-like jobs go") with the exact cfg we already use
// ((3<<8)|(3<<12)|(8<<16)); slot 2 is only a PanVK-side convention that has
// never completed on this board. R320 submits the same chain to the
// requested slot to discriminate slot wiring vs. IRQ masking.
uint32_t gSelftestSlot = 2;

// Clear-descriptor experiment variant (e5-mali-g1-clear-variant=N):
//   0 (default) = R327-final astra short form: FBD Tiler=0, no tiler
//                 structures, single fragment job clean-tile clear
//                 (R332: RT gains internal_format/dither/swizzle fields)
//   3           = R327 full-tiler form (tctx w0=0 hw writeback, mask 0xFF)
uint32_t gClearVariant = 0;

// Reset-storm guard (R319 board finding: "reset #20..27+ (deferred)" loop
// after teardown). Every resetEngine() entry counts; past kMaxResets the
// engine is parked PERMANENTLY for the boot (sources masked, resetPending_
// stays set, one log line). Bounded churn, no unrecoverable accumulation.
constexpr uint32_t kMaxResets = 32;
uint32_t gResetTotal = 0;
// Per-source IRQ counters (storm identification, R320/R321).
uint32_t gIrqJob = 0, gIrqMmu = 0, gIrqGpu = 0;
// R323 storm data: gpu +3 per deferred reset. Kill policy evolution:
// R325/R326 sampled/killed too early or missed; R326b's "2nd reset of any
// tag" killed LEGITIMATE fault recovery on board (R327: MVJ timeout →
// reset #1, one re-fire → reset #2 = recovery, engine parked wrongly).
// R328: kill only on a genuine STORM — the 3rd resetEngine within a
// 10-second window. A reset that completes with sources quiet for >10s
// resets the window, so isolated double-faults never park the engine.
bool gGpuIntsDead = false;
// R335: TILER_FEATURES raw + Mesa-formula hierarchy mask.
uint32_t gTilerFeaturesRaw = 0;
uint32_t gHierMask = 0xff;
uint32_t gResetWindow[3] = {0, 0, 0};   // ring of recent reset ticks (ms)
uint32_t gResetWindowIdx = 0;

}  // namespace

// Read before either service starts. Reserving scanout requires every
// diagnostic gate, a silent video console and no competing touch canvas.
// This predicate has no MMIO, allocation or GPU-start side effects.
extern "C" bool e5_mali_g1_scanout_selected(void) {
    unsigned enable = 0, importEnabled = 0, scanout = 0, selftest = 0, canvas = 0;
    PE_parse_boot_argn("e5-mali-g1", &enable, sizeof(enable));
    PE_parse_boot_argn("e5-mali-g1-import", &importEnabled, sizeof(importEnabled));
    PE_parse_boot_argn("e5-mali-g1-scanout", &scanout, sizeof(scanout));
    PE_parse_boot_argn("e5-mali-g1-selftest", &selftest, sizeof(selftest));
    PE_parse_boot_argn("e5-touch-canvas", &canvas, sizeof(canvas));
    return enable == 1 && importEnabled == 1 && scanout == 1 &&
           (selftest & 31u) == 31u && canvas == 0 &&
           PE_parse_boot_argn("e5-no-vc", nullptr, 0);
}

// Boot-arg knobs (static members, read once in e5_mali_g1_start/start).
uint32_t E5MaliG1::bootEnable_ = 0;     // e5-mali-g1=1
uint32_t E5MaliG1::bootLegacyClk_ = 0;  // e5-mali-g1-clk-legacy=1 (R198 writes)
uint32_t E5MaliG1::bootSkipReset_ = 0;  // e5-mali-g1-reset=0 (skip 0xb98)
uint32_t E5MaliG1::bootImport_ = 0;     // e5-mali-g1-import=1 (ImportFixed)
uint32_t E5MaliG1::bootVaMb_ = 0;       // e5-mali-g1-va-mb=N
uint32_t E5MaliG1::bootSlotMask_ = 7;   // e5-mali-g1-slots=N
uint32_t E5MaliG1::bootSelftest_ = 0;   // e5-mali-g1-selftest=N (bit0 §2, bit1 §3)
static uint32_t gTraceJobs = 0;
static uint32_t gTracePerf = 0;
// Optional timing lives outside the IOService's size-constrained atom pool.
// All accesses use the same state lock as the corresponding atom transition.
struct E5AtomPerf {
    uint64_t id, cleanTicks, submittedAt, completedAt, importedBytes;
};
static E5AtomPerf gAtomPerf[E5MaliG1::kAtomPool];
static uint64_t e5PerfUs(uint64_t ticks) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 1000;
}

// Diagnostic counters (serial echo).
extern "C" {
volatile uint32_t e5_mali_g1_stage = 0;
volatile uint32_t e5_mali_g1_status = 0;
volatile uint32_t e5_mali_g1_ready = 0;
volatile uint64_t e5_mali_g1_gpu_id = 0, e5_mali_g1_shader = 0, e5_mali_g1_as_present = 0;
volatile uint64_t e5_mali_g1_js_present = 0, e5_mali_g1_mmu_features = 0;
volatile uint64_t e5_mali_g1_submitted = 0, e5_mali_g1_completed = 0, e5_mali_g1_faults = 0;
volatile uint64_t e5_mali_g1_timeouts = 0, e5_mali_g1_resets = 0, e5_mali_g1_mmu_faults = 0;
volatile uint64_t e5_mali_g1_last_fault_code = 0, e5_mali_g1_last_fault_addr = 0;
}

OSDefineMetaClassAndStructors(E5MaliG1, IOService)

static void readBootArgs() {
    PE_parse_boot_argn("e5-mali-g1", &E5MaliG1::bootEnable_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-clk-legacy", &E5MaliG1::bootLegacyClk_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-reset", &E5MaliG1::bootSkipReset_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-import", &E5MaliG1::bootImport_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-va-mb", &E5MaliG1::bootVaMb_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-slots", &E5MaliG1::bootSlotMask_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-mali-g1-selftest", &E5MaliG1::bootSelftest_, sizeof(uint32_t));
    PE_parse_boot_argn("e5-gpu-job-trace", &gTraceJobs, sizeof(gTraceJobs));
    PE_parse_boot_argn("e5-gpu-perf", &gTracePerf, sizeof(gTracePerf));
    PE_parse_boot_argn("e5-mali-g1-selftest-slot", &gSelftestSlot, sizeof(gSelftestSlot));
    if (gSelftestSlot > 2) gSelftestSlot = 2;
    PE_parse_boot_argn("e5-mali-g1-clear-variant", &gClearVariant, sizeof(gClearVariant));
}

// Track imported physical pages without retyping their CPU mappings. Shared
// bitmap pages also contain CPU atomic reference counts; changing the whole
// page to non-cacheable breaks that protocol on E5. DMA visibility is handled
// by cache maintenance at submission and retirement instead.
namespace E5UserCache {
// Pinned xnu-10063.141.1 arm/memory_types.h values; the IOKit header mode
// hides their VM_MEM_* definitions. Do not enable broader private layouts.
constexpr unsigned cacheWriteBack = 0x2, cacheInnerWriteBack = 0x12;
constexpr unsigned cacheWriteThrough = 0xb, cacheWriteCombine = 0x6;
// This pinned E5 kernel has HAS_UCNORMAL_MEM=0: VM_WIMG_RT is device
// uncached (CACHE_ATTRINDX_DISABLE), as used by original IOSurface buffers.
constexpr unsigned cacheRealtime = 0x17;
constexpr uint64_t base = 0x80000000ull;
constexpr uint64_t top = 0x100000000ull;
constexpr size_t count = (top - base) / 4096;
struct Page { uint32_t references, previous; };
static Page *pages;
static IOLock *lock;
static uint64_t active;
static bool start() {
    if (!lock) lock = IOLockAlloc();
    return lock != nullptr;
}
static bool index(IOMemoryDescriptor *md, uint64_t offset, size_t *slot, ppnum_t *page,
                  IOByteCount *segmentBytes = nullptr) {
    IOByteCount bytes = 0;
    // Our GPU page tables require CPU physical addresses, never an I/O
    // mapper's address for a pre-existing shared memory object.
    uint64_t physical = md->getPhysicalSegment(offset, &bytes, kIOMemoryMapperNone);
    if (bytes < 4096 || (physical & 4095) || physical < base || physical > top - 4096) return false;
    *slot = (size_t)((physical - base) >> 12); *page = (ppnum_t)(physical >> 12);
    if (segmentBytes) *segmentBytes = bytes;
    return true;
}
static void releaseLocked(IOMemoryDescriptor *md, uint32_t number) {
    uint32_t restored = 0, mismatch = 0;
    for (uint32_t n = 0; n < number; ++n) {
        size_t slot = 0; ppnum_t page = 0;
        if (!index(md, (uint64_t)n * 4096, &slot, &page) || !pages[slot].references) { ++mismatch; continue; }
        if (--pages[slot].references == 0) {
            // Imports never change the caller's CPU memory type.
            mismatch += pmap_cache_attributes(page) != pages[slot].previous;
            pages[slot].previous = 0;
            --active; ++restored;
        }
    }
    note("E5WC RELEASE pages=%u released=%u mismatch=%u live=%llu preserve=1", number, restored, mismatch, (unsigned long long)active);
}
static bool acquire(IOMemoryDescriptor *md, uint64_t length, uint32_t *number) {
    *number = 0;
    IOLockLock(lock);
    if (!pages) {
        pages = (Page *)IOMalloc(count * sizeof(Page));
        if (pages) bzero(pages, count * sizeof(Page));
    }
    bool ok = pages != nullptr;
    const char *reason = "tracker";
    uint64_t offset = 0;
    uint32_t realtimePages = 0;
    for (; ok && offset < length; offset += 4096) {
        size_t slot = 0; ppnum_t page = 0;
        reason = "segment";
        ok = index(md, offset, &slot, &page);
        if (!ok) break;
        reason = "refcount";
        ok = pages[slot].references != UINT32_MAX;
        if (!ok) break;
        if (pages[slot].references &&
            pmap_cache_attributes(page) != pages[slot].previous) {
            reason = "cpu-policy-changed"; ok = false; break;
        }
        if (!pages[slot].references) {
            unsigned mode = pmap_cache_attributes(page);
            reason = "cache-mode";
            if (mode != cacheWriteBack && mode != cacheInnerWriteBack &&
                mode != cacheWriteThrough && mode != cacheWriteCombine &&
                mode != cacheRealtime) { ok = false; break; }
            // Do not extend this to unmanaged I/O or unknown cache modes.
            if (!pmap_valid_page(page)) {
                reason = "unmanaged"; ok = false; break;
            }
            pages[slot].previous = mode; ++active;
        }
        ++pages[slot].references;
        if (pages[slot].previous == cacheRealtime) ++realtimePages;
        ++*number;
    }
    if (!ok && *number) { releaseLocked(md, *number); *number = 0; }
    if (ok) {
        note("E5WC ACQUIRE PASS pages=%u live=%llu rt=%u preserve=1", *number, (unsigned long long)active, realtimePages);
    } else {
        static uint64_t failures;
        const uint64_t n = ++failures;
        if (n <= 4 || !(n & (n - 1))) {
            IOByteCount rawLength = 0, physicalLength = 0;
            const uint64_t raw = md->getPhysicalSegment(offset, &rawLength);
            const uint64_t physical = md->getPhysicalSegment(offset, &physicalLength, kIOMemoryMapperNone);
            const bool ram = !(physical & 4095) && physical >= base && physical <= top - 4096;
            const ppnum_t page = (ppnum_t)(physical >> 12);
            note("E5WC ACQUIRE FAIL n=%llu reason=%s off=%llx len=%llx pa=%llx raw=%llx seg=%llx mode=%x managed=%u pages=%u live=%llu",
                 (unsigned long long)n, reason, (unsigned long long)offset,
                 (unsigned long long)length, (unsigned long long)physical,
                 (unsigned long long)raw, (unsigned long long)physicalLength,
                 ram ? pmap_cache_attributes(page) : 0xffffu,
                 ram ? (unsigned)pmap_valid_page(page) : 0u, *number,
                 (unsigned long long)active);
        }
    }
    IOLockUnlock(lock);
    return ok;
}
static uint64_t activePages() {
    IOLockLock(lock); uint64_t result = active; IOLockUnlock(lock); return result;
}
// CPU memory types stay unchanged. Operation 0 validates, 1 cleans before
// DMA, 2 explicitly invalidates, and 3 cleans/invalidates on retirement.
// The retirement clean preserves CPU-owned metadata lines modified while
// the GPU accessed other cache lines of the same imported bitmap pages.
static bool syncUserRange(IOMemoryDescriptor *md, uint64_t offset, uint64_t length, unsigned operation) {
    uint64_t physical = 0, segmentLeft = 0;
    while (length) {
        const uint64_t pageOffset = offset & ~4095ull;
        const uint64_t within = offset & 4095;
        const uint64_t bytes = length < 4096 - within ? length : 4096 - within;
        if (!segmentLeft) {
            size_t slot = 0; ppnum_t first = 0; IOByteCount span = 0;
            if (!index(md, pageOffset, &slot, &first, &span)) return false;
            physical = (uint64_t)first << 12;
            // Reuse only whole pages of the descriptor's physical segment.
            // A fragmented or partial segment is queried again at its edge.
            segmentLeft = span & ~4095ull;
        }
        if (physical < base || physical > top - 4096) return false;
        const ppnum_t page = (ppnum_t)(physical >> 12);
        if (!pmap_valid_page(page)) return false;
        const unsigned mode = pmap_cache_attributes(page);
        const bool cached = mode == cacheWriteBack || mode == cacheInnerWriteBack || mode == cacheWriteThrough;
        if (!cached && mode != cacheWriteCombine && mode != cacheRealtime) return false;
        if (operation && cached) {
            // The native direct map avoids altering the user's RO mapping.
            const uintptr_t start = (uintptr_t)ml_static_ptovirt((vm_offset_t)((uint64_t)page << 12)) + within;
            // DC operates on whole lines. An unaligned range can touch one
            // more line than ceil(bytes / 64); include both boundary lines.
            for (uintptr_t at = start & ~uintptr_t(63); at < start + bytes; at += 64) {
                if (operation == 1) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
                else if (operation == 2) __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
                else __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
            }
        }
        offset += bytes; length -= bytes;
        physical += 4096; segmentLeft -= 4096;
    }
    if (operation) __asm__ volatile("dsb sy" ::: "memory");
    return true;
}
static void release(IOMemoryDescriptor *md, uint32_t number) {
    if (!number) return;
    IOLockLock(lock); releaseLocked(md, number); IOLockUnlock(lock);
}
static void finish() {
    if (pages) { IOFree(pages, count * sizeof(Page)); pages = nullptr; }
    if (lock) { IOLockFree(lock); lock = nullptr; }
}
}

// ===========================================================================
// Lifecycle
// ===========================================================================
bool E5MaliG1::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pinStorage_ = (uint32_t *)IOMalloc(kAtomPool * E5_MALI_MAX_SUBMIT_BOS * sizeof(uint32_t));
    if (!pinStorage_) return false;
    bzero(pinStorage_, kAtomPool * E5_MALI_MAX_SUBMIT_BOS * sizeof(uint32_t));
    for (uint32_t i = 0; i < kAtomPool; ++i)
        atomPool_[i].pinnedBo = pinStorage_ + i * E5_MALI_MAX_SUBMIT_BOS;
    readBootArgs();
    state_ = IOLockAlloc();
    hw_ = IOSimpleLockAlloc();
    if (!state_ || !hw_ || !E5UserCache::start()) { fail(); return false; }
    e5_mali_g1_stage = 1;
    if (!mapAll()) { fail(); return false; }
    e5_mali_g1_stage = 2;
    if (!platformPowerOn()) { fail(); return false; }
    e5_mali_g1_stage = 3;
    if (!gpuInit()) { fail(); return false; }
    e5_mali_g1_stage = 4;
    workloop_ = IOWorkLoop::workLoop();
    if (!workloop_) { fail(); return false; }
    // Source masks stay quiet until the GIC line is armed.
    write32(JOB_INT_MASK, 0);
    write32(MMU_INT_MASK, 0);
    write32(GPU_INT_MASK, 0);
    e5_mali_g1_stage = 5;
    if (!attachIrq()) { fail(); return false; }
    e5_mali_g1_stage = 6;
    // Unmask now: job DONE|ERR for usable slots, MMU all-AS, GPU errors.
    // Mask bitmap: bit n = DONE(slot n), bit n+16 = ERR(slot n) — three
    // usable slots => 0x70007 (GENMASK(18,16)|GENMASK(2,0), panfrost
    // panfrost_job.c:793-794). Read back so the wire value is on record.
    uint32_t jobMask = 0;
    for (uint32_t j = 0; j < kMaxSlots; ++j)
        if (slotUsable(j)) jobMask |= (0x10001u << j);
    write32(JOB_INT_CLEAR, ~0u);
    write32(JOB_INT_MASK, jobMask);
    note("E5MALI1 job mask wrote=%08x readback=%08x stat=%08x",
         jobMask, read32(JOB_INT_MASK), read32(JOB_INT_STAT));
    write32(MMU_INT_CLEAR, ~0u);
    write32(MMU_INT_MASK, ~0u);
    timer_ = IOTimerEventSource::timerEventSource(this, &E5MaliG1::timerAction);
    if (!timer_ || workloop_->addEventSource(timer_) != kIOReturnSuccess) {
        fail();
        return false;
    }
    timer_->setTimeoutMS(100);
    e5_mali_g1_stage = 100;
    e5_mali_g1_status |= 7;   // powered | hw-init | irq
    e5_mali_g1_ready = 1;
    note("E5MALI1 tilerfeat=%08x maxlevels=%u hiermask=%03x",
         gTilerFeaturesRaw, (gTilerFeaturesRaw >> 8) & 0xfu, gHierMask);
    note("E5MALI1 ready id=%08x shader=%llx as=%x js=%x mmu=%08x slots=%u",
         gpuId_, (unsigned long long)shaderPresent_, asPresent_, jsPresent_,
         mmuFeatures_, numSlots_);
    registerService();
    e5_publish_metal(this);
    scheduleSelftest();
    return true;
}

void E5MaliG1::fail() {
    // Unwind everything start() built; never panic.
    note("E5MALI1 start failed at stage %u status=%x — rolling back",
         e5_mali_g1_stage, e5_mali_g1_status);
    if (timer_) {
        timer_->cancelTimeout();
        workloop_->removeEventSource(timer_);
        timer_->release();
        timer_ = nullptr;
    }
    if (irqSource_) {
        workloop_->removeEventSource(irqSource_);
        irqSource_->release();
        irqSource_ = nullptr;
    }
    detachIrq();
    if ((e5_mali_g1_status & 1) != 0) platformPowerOff();
    if (hw_) { IOSimpleLockFree(hw_); hw_ = nullptr; }
    if (state_) { IOLockFree(state_); state_ = nullptr; }
    if (workloop_) { workloop_->release(); workloop_ = nullptr; }
    OSSafeReleaseNULL(aonMap_); OSSafeReleaseNULL(pmuMap_); OSSafeReleaseNULL(apbMap_);
    OSSafeReleaseNULL(dvfsMap_); OSSafeReleaseNULL(mmioMap_); OSSafeReleaseNULL(anlgMap_);
    aon_ = pmu_ = apb_ = dvfs_ = anlg_ = nullptr;
    mmioVaddr_ = 0;
    e5_mali_g1_ready = 0;   // never set to 1 on a failing start; keep clear
}

void E5MaliG1::stop(IOService *provider) {
    // Retire the readiness flag FIRST: the delayed self-test thread and any
    // UserClient creation check it before touching state_/maps, so it must
    // already be 0 while the fields below are still valid — and long before
    // they are freed.
    e5_mali_g1_ready = 0;
    // Quiesce sources, drop the GIC line, power the island off (audit §2.10).
    if (timer_) { timer_->cancelTimeout(); }
    // The in-kernel self-test thread touches the locks/maps below; wait for
    // it to leave (bounded — it sleeps 30 s up front, then runs <1 s).
    for (int i = 0; i < 1200 && selftestActive_; ++i) IOSleep(10);
    if (selftestActive_) note("E5MALI1 stop: selftest still active, continuing");
    if (mmioVaddr_) {
        write32(JOB_INT_MASK, 0);
        write32(MMU_INT_MASK, 0);
        write32(GPU_INT_MASK, 0);
        IODelay(100);
    }
    detachIrq();
    if (irqSource_) {
        workloop_->removeEventSource(irqSource_);
        irqSource_->release();
        irqSource_ = nullptr;
    }
    if (timer_) {
        workloop_->removeEventSource(timer_);
        timer_->release();
        timer_ = nullptr;
    }
    if (workloop_) { workloop_->release(); workloop_ = nullptr; }
    if ((e5_mali_g1_status & 1) != 0) platformPowerOff();
    if (hw_) { IOSimpleLockFree(hw_); hw_ = nullptr; }
    if (state_) { IOLockFree(state_); state_ = nullptr; }
    OSSafeReleaseNULL(aonMap_); OSSafeReleaseNULL(pmuMap_); OSSafeReleaseNULL(apbMap_);
    OSSafeReleaseNULL(dvfsMap_); OSSafeReleaseNULL(mmioMap_); OSSafeReleaseNULL(anlgMap_);
    aon_ = pmu_ = apb_ = dvfs_ = anlg_ = nullptr;
    mmioVaddr_ = 0;
    // (e5_mali_g1_ready was cleared at the top of stop().)
    e5_mali_g1_status = 0;
    IOService::stop(provider);
}

void E5MaliG1::free() {
    E5UserCache::finish();
    if (pinStorage_) {
        IOFree(pinStorage_, kAtomPool * E5_MALI_MAX_SUBMIT_BOS * sizeof(uint32_t));
        pinStorage_ = nullptr;
    }
    IOService::free();
}

IOMemoryMap *E5MaliG1::map(uint64_t pa, uint32_t len) {
    // E5Mali.cpp mapWindow discipline: refuse short maps outright.
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, len);
    if (!mem) return nullptr;
    IOMemoryMap *mapping = mem->map();
    if (!mapping || mapping->getLength() < len) {
        note("E5MALI1 map pa=%llx len=%x REFUSED", (unsigned long long)pa, len);
        if (mapping) mapping->release();
        mem->release();
        return nullptr;
    }
    mem->release();   // the map retains the descriptor
    return mapping;
}

bool E5MaliG1::mapAll() {
    // AON/PMU/ANLG are always-on domains; the GPU-side windows map later.
    aonMap_ = map(kAonApbBase, kAonApbLen);
    pmuMap_ = map(kPmuBase, kPmuLen);
    anlgMap_ = map(kAnlgBase, kAnlgLen);
    if (!aonMap_ || !pmuMap_ || !anlgMap_) return false;
    aon_ = (volatile uint32_t *)aonMap_->getVirtualAddress();
    pmu_ = (volatile uint32_t *)pmuMap_->getVirtualAddress();
    anlg_ = (volatile uint32_t *)anlgMap_->getVirtualAddress();
    note("E5MALI1 aon w0=%08x (gpu_eb=%u)", aon_[0], (aon_[0] >> 11) & 1u);
    return true;
}

void E5MaliG1::aonSet(uint32_t bits) {
    // SC gate: +0x1000 SET, +0x2000 CLEAR (ums9621-clk.c gate.c:39-53).
    IOSimpleLockLock(hw_);
    aon_[(kAonGateSet) / 4] = bits;
    IOSimpleLockUnlock(hw_);
    IODelay(2);
}

void E5MaliG1::aonClear(uint32_t bits) {
    IOSimpleLockLock(hw_);
    aon_[(kAonGateClear) / 4] = bits;
    IOSimpleLockUnlock(hw_);
    IODelay(2);
}

uint32_t E5MaliG1::pmuRmw(uint32_t off, uint32_t mask, uint32_t val) {
    IOSimpleLockLock(hw_);
    const uint32_t v = pmu_[off / 4];
    pmu_[off / 4] = (v & ~mask) | (val & mask);
    IOSimpleLockUnlock(hw_);
    __asm__ volatile("dsb sy" ::: "memory");
    return pmu_[off / 4];
}

uint32_t E5MaliG1::sysconRmw(volatile uint32_t *base, uint32_t off, uint32_t mask,
                             uint32_t val) {
    IOSimpleLockLock(hw_);
    const uint32_t v = base[off / 4];
    base[off / 4] = (v & ~mask) | (val & mask);
    IOSimpleLockUnlock(hw_);
    __asm__ volatile("dsb sy" ::: "memory");
    return base[off / 4];
}

// ---------------------------------------------------------------------------
// Platform lifecycle: vendor qogirn6l sequence with R198-proven ordering
// (gate -> DVFS handoff -> power -> island windows -> clocks). Audit §2.1-2.4.
// ---------------------------------------------------------------------------
bool E5MaliG1::platformPowerOn() {
    // A1: open the AON gate for the GPU windows (gpu_eb BIT(11), set mirror).
    aonSet(kAonGateGpuEb);
    if (((aon_[0] >> 11) & 1u) == 0) {
        note("E5MALI1 gpu_eb gate did not latch (w0=%08x)", aon_[0]);
        return false;
    }
    // B1/B2: hand DVFS to the hardware engine before power (vendor order).
    sysconRmw(anlg_, kAnlgTopDvfsCfg, kAnlgTopDvfsCfgMask, 0);
    sysconRmw(anlg_, kAnlgSwDvfsCtrl, kAnlgSwDvfsCtrlMask, 0);
    // C1/C2: GPLL force-off clear, then force-on set.
    pmuRmw(kPmuGpllFrcOff, 0x2, 0);
    IODelay(100);
    pmuRmw(kPmuGpllFrcOn, 0x1, 0x1);
    IODelay(150);
    // C3: GPU DCDC enable (0 = enabled) over ADI.
    if (e5_pmic_rmw(kPmicDcdcGpuPd, 0x8, 0x0) != 0) {
        note("E5MALI1 PMIC DCDC write refused (ADI path absent?)");
        return false;
    }
    IODelay(400);
    // C4: leave forced shutdown.
    pmuRmw(kPmuForceShut, 0x1u << 25, 0);
    // C5: GPU soft reset (skippable — r177 abort at this sub-region).
    if (!bootSkipReset_) {
        pmuRmw(kPmuGpuSoftRst, 0x4, 0x4);
        IODelay(10);
        pmuRmw(kPmuGpuSoftRst, 0x4, 0x0);
        IODelay(300);
    } else {
        note("E5MALI1 platform soft reset skipped by boot arg");
    }
    // C6: bounded top-state wait (200 x 50 us).
    bool up = false;
    for (int i = 0; i < 200; ++i) {
        if ((pmu_[kPmuTopState / 4] & kPmuTopStateMask) == 0) { up = true; break; }
        IODelay(50);
    }
    if (!up) {
        note("E5MALI1 top state stuck: %08x", pmu_[kPmuTopState / 4]);
        return false;
    }
    // Island is up: map the GPU-side windows now (r194-197 lesson).
    apbMap_ = map(kGpuApbBase, kGpuApbLen);
    dvfsMap_ = map(kDvfsBase, kDvfsLen);
    mmioMap_ = map(kGpuMmioBase, kGpuMmioLen);
    if (!apbMap_ || !dvfsMap_ || !mmioMap_) return false;
    apb_ = (volatile uint32_t *)apbMap_->getVirtualAddress();
    dvfs_ = (volatile uint32_t *)dvfsMap_->getVirtualAddress();
    mmioVaddr_ = mmioMap_->getVirtualAddress();
    // D1: clk_core_gpu_eb (word0 bit2, plain RMW) + top-state re-check.
    sysconRmw(apb_, kGpuApbCoreEb, kGpuApbCoreEbMask, kGpuApbCoreEbMask);
    IODelay(400);
    up = false;
    for (int i = 0; i < 200; ++i) {
        if ((pmu_[kPmuTopState / 4] & kPmuTopStateMask) == 0) { up = true; break; }
        IODelay(50);
    }
    if (!up) { note("E5MALI1 top state stuck after clk eb"); return false; }
    // R198 diagnostic fallback (ums512-lineage mux writes): off by default.
    if (bootLegacyClk_) {
        apb_[kGpuApbLegacyClk0 / 4] = (1u << 0) | (1u << 8);
        apb_[kGpuApbLegacyClk1 / 4] = (1u << 0) | (1u << 8);
        apb_[kGpuApbLegacyClk2 / 4] = (1u << 0);
        IODelay(10);
        note("E5MALI1 legacy clk writes applied (0x4/0x8/0xc)");
    }
    // D3/D4: hardware DVFS handshake + index 3 (384 MHz).
    sysconRmw(dvfs_, kDvfsFreqUpd, kDvfsFreqUpdMask, 1);
    sysconRmw(dvfs_, kDvfsIndexCfg, kDvfsIndexMask, kDvfsIndexDefault);
    e5_mali_g1_status |= 1;   // powered
    note("E5MALI1 platform up: pmic201c=%08x top=%08x",
         e5_pmic_peek(kPmicDcdcGpuPd), pmu_[kPmuTopState / 4]);
    return true;
}

void E5MaliG1::platformPowerOff() {
    // Audit §2.10 order; waits bounded, failures only log.
    if (apb_) {
        bool idle = false;
        for (int i = 0; i < 200; ++i) {
            if ((apb_[kGpuApbBridge / 4] & kGpuApbBridgeIdle) != 0) { idle = true; break; }
            IODelay(10);
        }
        if (!idle) note("E5MALI1 bridge not idle at power-off; continuing");
        sysconRmw(apb_, kGpuApbCoreEb, kGpuApbCoreEbMask, 0);
        IODelay(200);
    }
    pmuRmw(kPmuForceShut, 0x1u << 25, 0x1u << 25);
    e5_pmic_rmw(kPmicDcdcGpuPd, 0x8, 0x8);
    IODelay(10);
    pmuRmw(kPmuGpllFrcOn, 0x1, 0);
    pmuRmw(kPmuGpllFrcOff, 0x2, 0x2);
    aonClear(kAonGateGpuEb);
    e5_mali_g1_status &= ~(uint32_t)1;
    note("E5MALI1 platform powered off");
}

// ---------------------------------------------------------------------------
// GPU-core init (panfrost_gpu.c semantics)
// ---------------------------------------------------------------------------
bool E5MaliG1::gpuInit() {
    // Soft reset with a hard-reset fallback (panfrost_gpu_soft_reset).
    write32(GPU_INT_MASK, 0);
    write32(GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    write32(GPU_CMD, GPU_CMD_SOFT_RESET);
    bool done = false;
    for (int i = 0; i < 1000; ++i) {           // 10 ms
        if (read32(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED) { done = true; break; }
        IODelay(10);
    }
    if (!done) {
        write32(GPU_CMD, GPU_CMD_HARD_RESET);
        for (int i = 0; i < 1000; ++i) {
            if (read32(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED) { done = true; break; }
            IODelay(10);
        }
    }
    if (!done) { note("E5MALI1 GPU soft/hard reset timed out"); return false; }
    write32(GPU_INT_CLEAR, ~0u);
    write32(GPU_INT_MASK, GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);

    // Identity + capability snapshot with the corrected offsets (the R198
    // CAP line mislabelled 0x08/0x0c/0x10/0x14 and read MMU_INT as "as").
    gpuId_ = read32(GPU_ID);
    mmuFeatures_ = read32(GPU_MMU_FEATURES);
    asPresent_ = read32(GPU_AS_PRESENT);
    jsPresent_ = read32(GPU_JS_PRESENT);
    shaderPresent_ = read64(GPU_SHADER_PRESENT_LO);
    l2Present_ = read64(GPU_L2_PRESENT_LO);
    tilerPresent_ = read64(GPU_TILER_PRESENT_LO);
    // R335: read TILER_FEATURES (0x0C) — max_levels = (raw>>8)&0xF per
    // pan_query_tiler_features (pan_props.c:27-38) — and derive the
    // hierarchy mask with Mesa's exact formula (below) instead of the
    // hardcoded 0xFF (R334 fault: rasterizer read the list base and
    // reported DATA_INVALID; if this part exposes fewer than 8 levels,
    // enabling nonexistent level-7 headers is exactly that shape).
    gTilerFeaturesRaw = read32(0x00c);
    {
        const uint32_t maxLevels = (gTilerFeaturesRaw >> 8) & 0xfu;
        // GENX(pan_select_tiler_hierarchy_mask), pan_desc.c:1747-1810.
        uint32_t mask;
        static const uint32_t kDefaultMask[8] = {
            0, 0x80, 0x82, 0xa2, 0xaa, 0xea, 0xee, 0xfe
        };
        if (maxLevels < 8) mask = kDefaultMask[maxLevels];
        else mask = (1u << (maxLevels > 16 ? 16 : maxLevels)) - 1u;
        // last_hierarchy_bit = last_bit(ceil(max(w,h)/16)); shift up if the
        // fb needs more levels than exist.
        const uint32_t maxWh = 480;   // framebuffer max dimension (fixed probe)
        uint32_t lastBit = 0;
        {
            const uint32_t bins = (maxWh + 15) / 16;
            uint32_t v = bins;
            while (v) { ++lastBit; v >>= 1; }
        }
        if (lastBit > maxLevels) mask <<= (lastBit - maxLevels);
        // 16x16 tile area = 256: disable loop stops at 0.
        // Budget loop: bins-pointer size must fit the heap budget
        // (pan_calc_bins_pointer_size :1703-1743, bin_ptr_size=8 for v9;
        // bins halve with ceil per level).
        for (uint32_t d = 0; d < 16; ++d) {
            uint32_t bx[16], by[16];
            bx[0] = (320 + 15) / 16;   // framebuffer 320x480 (fixed probe)
            by[0] = (480 + 15) / 16;
            for (uint32_t i = 1; i < 16; ++i) {
                bx[i] = (bx[i - 1] + 1) / 2;
                by[i] = (by[i - 1] + 1) / 2;
            }
            uint32_t binsTotal = 0;
            for (uint32_t lvl = 0; lvl < 16; ++lvl)
                if (mask & (1u << lvl)) binsTotal += bx[lvl] * by[lvl];
            const uint32_t ptrSize = ((binsTotal + 7) / 8) * 8 * 8;
            if (ptrSize < 0x200000u) break;  // heap budget (kHeapSize, R336)
            mask &= ~((1u << (d + 1)) - 1u);
        }
        gHierMask = mask;
    }
    e5_mali_g1_gpu_id = gpuId_;
    e5_mali_g1_shader = shaderPresent_;
    e5_mali_g1_as_present = asPresent_;
    e5_mali_g1_js_present = jsPresent_;
    e5_mali_g1_mmu_features = mmuFeatures_;
    if ((gpuId_ >> 16) != kExpectedGpuIdFamily) {
        note("E5MALI1 unexpected family %04x (want %04x) — refusing",
             gpuId_ >> 16, kExpectedGpuIdFamily);
        return false;
    }
    if ((uint32_t)shaderPresent_ != kExpectedShaderMask)
        note("E5MALI1 shader mask %llx differs from R198 0x5 — using as read",
             (unsigned long long)shaderPresent_);
    numSlots_ = 0;
    for (uint32_t j = 0; j < kMaxSlots; ++j)
        if (slotUsable(j)) ++numSlots_;
    numAs_ = 0;
    for (uint32_t a = 0; a < kMaxAs; ++a)
        if (asPresent_ & (1u << a)) ++numAs_;
    if (numSlots_ == 0 || numAs_ == 0) {
        note("E5MALI1 no usable slots/AS (js=%x as=%x)", jsPresent_, asPresent_);
        return false;
    }
    note("E5MALI1 id %08x mmu %08x (va=%u pa=%u) l2 %llx tiler %llx",
         gpuId_, mmuFeatures_, mmuFeatures_ & 0xff, (mmuFeatures_ >> 8) & 0xff,
         (unsigned long long)l2Present_, (unsigned long long)tilerPresent_);

    // Core power-on with bounded ready waits (panfrost_gpu_power_on). The
    // sparse shader mask is used verbatim (0x5 on this part).
    const uint32_t coreMask = (l2Present_ == 1) ? 0xffffffffu
                                                : (uint32_t)(l2Present_ - 1);
    write32(GPU_L2_PWRON_LO, (uint32_t)l2Present_ & coreMask);
    done = false;
    for (int i = 0; i < 2000; ++i) {           // 20 ms
        if (read32(GPU_L2_READY_LO) == ((uint32_t)l2Present_ & coreMask)) { done = true; break; }
        IODelay(10);
    }
    if (!done) note("E5MALI1 L2 ready timeout (ready=%08x)", read32(GPU_L2_READY_LO));
    write32(GPU_SHADER_PWRON_LO, (uint32_t)shaderPresent_ & coreMask);
    done = false;
    for (int i = 0; i < 2000; ++i) {
        if (read32(GPU_SHADER_READY_LO) == ((uint32_t)shaderPresent_ & coreMask)) { done = true; break; }
        IODelay(10);
    }
    if (!done) note("E5MALI1 shader ready timeout (ready=%08x want=%08x)",
                    read32(GPU_SHADER_READY_LO), (uint32_t)shaderPresent_ & coreMask);
    write32(GPU_TILER_PWRON_LO, (uint32_t)tilerPresent_);
    done = false;
    for (int i = 0; i < 100; ++i) {            // 1 ms
        if (read32(GPU_TILER_READY_LO) == (uint32_t)tilerPresent_) { done = true; break; }
        IODelay(10);
    }
    if (!done) note("E5MALI1 tiler ready timeout (ready=%08x)", read32(GPU_TILER_READY_LO));
    // TODO UNVERIFIED: whether G57 r0p1 needs GPU_SHADER_CONFIG / GPU_JM_CONFIG
    // errata bits on this SoC (kbase applies them from its hwconfig tables;
    // panfrost runs G57 with none). First flash observes behavior.
    e5_mali_g1_status |= 2;   // hw-init
    return true;
}

// ---------------------------------------------------------------------------
// Shared IRQ: one line, three sources (audit §2.7)
// ---------------------------------------------------------------------------
bool E5MaliG1::attachIrq() {
    uint32_t state = 0;
    uint64_t router = 0;
    const IOReturn ok = armcpu_spi_attach_masked(kGicIntid, /*edge=*/false,
                                                 &E5MaliG1::irqPrimary, this,
                                                 &state, &router);
    if (ok != kIOReturnSuccess) {
        note("E5MALI1 spi attach %d refused=%x", kGicIntid, (unsigned)ok);
        return false;
    }
    irqSource_ = IOInterruptEventSource::interruptEventSource(this,
                                                              &E5MaliG1::workAction);
    if (!irqSource_ || workloop_->addEventSource(irqSource_) != kIOReturnSuccess) {
        armcpu_spi_detach(kGicIntid, this);
        return false;
    }
    if (armcpu_spi_set_enabled(kGicIntid, true, this) != kIOReturnSuccess) {
        armcpu_spi_detach(kGicIntid, this);
        return false;
    }
    irqAttached_ = true;
    note("E5MALI1 irq attached intid=%d state=%x", kGicIntid, state);
    return true;
}

void E5MaliG1::detachIrq() {
    if (!irqAttached_) return;
    armcpu_spi_set_enabled(kGicIntid, false, this);
    const IOReturn ok = armcpu_spi_detach(kGicIntid, this);
    irqAttached_ = false;
    note("E5MALI1 irq detached rc=%x count=%llu", (unsigned)ok,
         (unsigned long long)armcpu_spi_count(kGicIntid));
}

// Primary interrupt context: read the MASKED status of all three blocks
// R330/astra defect fix — the lost-event bug: the old top half read
// JOB_INT_STAT, CLEARED the hardware, and stashed the bits; the bottom half
// then re-read the already-cleared RAWSTAT, got 0, and DROPPED the event —
// the GPU had been raising DONE all along. New split follows panfrost
// (panfrost_job.c:815 top half / :494-548 bottom half):
//   top:    mask the JOB block, wake the workloop. No clears here.
//   bottom: read RAWSTAT -> process -> CLEAR (JS_STATE is sampled by the
//           hardware AT THE CLEAR WRITE) -> re-read RAWSTAT until 0 ->
//           restore the mask. MMU/GPU blocks keep the R321 masked-read
//           semantics (their handlers read live state, not stale bits).
void E5MaliG1::irqPrimary(void *ctx) {
    E5MaliG1 *d = static_cast<E5MaliG1 *>(ctx);
    if (!d->mmioVaddr_) return;
    const uint32_t job = d->read32(JOB_INT_STAT);
    const uint32_t mmu = d->read32(MMU_INT_STAT);
    const uint32_t gpu = d->read32(GPU_INT_STAT);
    if (!job && !mmu && !gpu) return;
    if (job) {
        ++gIrqJob;
        d->write32(JOB_INT_MASK, 0);   // top half: mask only, no clear
    }
    if (mmu) {
        ++gIrqMmu;
        d->write32(MMU_INT_CLEAR, mmu);
        __atomic_fetch_or(&d->pendingMmu_, mmu, __ATOMIC_RELAXED);
    }
    if (gpu) {
        ++gIrqGpu;
        d->write32(GPU_INT_CLEAR, gpu);
        __atomic_fetch_or(&d->pendingGpu_, gpu, __ATOMIC_RELAXED);
    }
    if (d->irqSource_) d->irqSource_->interruptOccurred(nullptr, nullptr, 0);
}

void E5MaliG1::workAction(OSObject *owner, IOInterruptEventSource *, int) {
    E5MaliG1 *d = static_cast<E5MaliG1 *>(owner);
    // R330: JOB events live in RAWSTAT until handleJobIrq processes them
    // (top half masks, does not clear). MMU/GPU still use stashed bits.
    const uint32_t jobMasked = d->read32(JOB_INT_STAT);
    const uint32_t jobRaw = d->read32(JOB_INT_RAWSTAT);
    const uint32_t mmu = __atomic_exchange_n(&d->pendingMmu_, 0u, __ATOMIC_RELAXED);
    const uint32_t gpu = __atomic_exchange_n(&d->pendingGpu_, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&d->pendingJob_, 0u, __ATOMIC_RELAXED);
    if (!jobMasked && !jobRaw && !mmu && !gpu) return;
    IOLockLock(d->state_);
    if (jobMasked || jobRaw) {
        d->handleJobIrq();
        // handleJobIrq processes-and-clears RAWSTAT; on exit re-arm the
        // JOB mask so later completions interrupt again (panfrost's bottom
        // half restores the mask at :818).
        if (!d->resetPending_) {
            uint32_t jobMask = 0;
            for (uint32_t j = 0; j < kMaxSlots; ++j)
                if (d->slotUsable(j)) jobMask |= (0x10001u << j);
            d->write32(JOB_INT_MASK, jobMask);
        }
    }
    if (mmu) d->handleMmuIrq(mmu);
    if (gpu) d->handleGpuIrq(gpu);
    if (d->resetPending_) d->resetEngine("deferred");
    IOLockUnlock(d->state_);
}

void E5MaliG1::timerAction(OSObject *owner, IOTimerEventSource *t) {
    E5MaliG1 *d = static_cast<E5MaliG1 *>(owner);
    d->watchdogTick();
    t->setTimeoutMS(100);
}

void E5MaliG1::watchdogTick() {
    tickMs_ += 100;
    if (!state_ || resetPending_) return;
    IOLockLock(state_);
    bool hung = false;
    for (uint32_t i = 0; i < kAtomPool; ++i) {
        E5MaliG1Atom *a = &atomPool_[i];
        if (a->state != kAtomOnHw) continue;
        if (tickMs_ - a->submittedTickMs < kJobTimeoutMs) continue;
        // Insurance before declaring a hang: a completion IRQ may have been
        // lost or raced — poll RAWSTAT first, then the slot's JS_STATUS.
        const uint32_t raw = read32(JOB_INT_RAWSTAT);
        if (raw) handleJobIrq();
        if (a->state != kAtomOnHw) continue;   // completed by the RAWSTAT pass
        const uint32_t status =
            read32(JS_BASE + a->slot * JS_SLOT_STRIDE + JS_STATUS);
        if (status == 0x01) {
            // Slot reports DONE with no IRQ: retire without a reset.
            note("E5MALI1 late-done poll atom %llu slot %u (JS_STATUS 01, no IRQ)",
                 (unsigned long long)a->id, a->slot);
            completeAtom(a, kAtomDone, 0x01);   // drops ring/wait refs
            continue;
        }
        // completeAtom also drops the atom from its slot ring, so the
        // reset below cannot double-retire it.
        completeAtom(a, kAtomTimedOut, 0);
        e5_mali_g1_timeouts = e5_mali_g1_timeouts + 1;
        hung = true;
        // R320 diagnostic snapshot: both job-block views plus the whole
        // slot register group, one shot (offsets: RAWSTAT 0x1000,
        // STAT 0x100c; slot base 0x1800 + n*0x80).
        note("E5MALI1 timeout atom %llu slot %u (tick %u status=%02x rawstat=%08x stat=%08x)",
             (unsigned long long)a->id, a->slot, tickMs_, status, raw,
             read32(JOB_INT_STAT));
        {
            const uint32_t b = JS_BASE + a->slot * JS_SLOT_STRIDE;
            note("E5MALI1 slot%u dump head=%08x:%08x tail=%08x:%08x cmd=%08x st=%08x headN=%08x:%08x cfgN=%08x cmdN=%08x",
                 a->slot, read32(b + 0x00), read32(b + 0x04), read32(b + 0x08),
                 read32(b + 0x0c), read32(b + JS_COMMAND), read32(b + JS_STATUS),
                 read32(b + JS_HEAD_NEXT_LO), read32(b + JS_HEAD_NEXT_HI),
                 read32(b + JS_CONFIG_NEXT), read32(b + JS_COMMAND_NEXT));
        }
    }
    if (hung) {
        // The timed-out atom may still own its slot: reset rolls everything
        // back and retires whatever remains on the rings as kAtomReset.
        resetPending_ = 1;
        resetEngine("job timeout");
    }
    IOLockUnlock(state_);
}

// ---------------------------------------------------------------------------
// Slot engine
// ---------------------------------------------------------------------------
E5MaliG1Atom *E5MaliG1::slotDequeue(uint32_t slot) {
    // panfrost_dequeue_job: current first, then next.
    E5MaliG1Atom *a = slots_[slot].ring[0];
    slots_[slot].ring[0] = slots_[slot].ring[1];
    slots_[slot].ring[1] = nullptr;
    return a;
}

void E5MaliG1::completeAtom(E5MaliG1Atom *a, uint8_t state, uint32_t hwStatus) {
    if (gTracePerf == 1) gAtomPerf[a - atomPool_].completedAt = mach_absolute_time();
    // Single terminal-transition point: drop any slot-ring reference first
    // so a retired pool entry can never be touched again by the engine
    // (pool entries are reused the moment they read kAtomFree).
    for (uint32_t s = 0; s < kMaxSlots; ++s) {
        if (slots_[s].ring[0] == a) {
            slots_[s].ring[0] = slots_[s].ring[1];
            slots_[s].ring[1] = nullptr;
        } else if (slots_[s].ring[1] == a) {
            slots_[s].ring[1] = nullptr;
        }
        for (uint32_t i = 0; i < kSlotQueue; ++i)
            if (slots_[s].wait[i] == a) {
                for (uint32_t k = i + 1; k < kSlotQueue; ++k) slots_[s].wait[k - 1] = slots_[s].wait[k];
                slots_[s].wait[kSlotQueue - 1] = nullptr;
                if (slots_[s].queued > 0) --slots_[s].queued;
            }
    }
    a->state = state;
    a->hwStatus = hwStatus;
    completionSeq_++;
    if (state == kAtomDone) e5_mali_g1_completed = e5_mali_g1_completed + 1;
    if (state == kAtomFault) e5_mali_g1_faults = e5_mali_g1_faults + 1;
}

void E5MaliG1::handleJobIrq() {
    // panfrost_job_handle_irq: clear-and-reread loop so two completions in
    // one interrupt window are not collapsed into one DONE.
    E5MaliG1Atom *done[kMaxSlots][2] = {};
    E5MaliG1Atom *failed[kMaxSlots] = {};
    uint32_t jsState = 0, jsEvents = 0;
    uint32_t status = read32(JOB_INT_RAWSTAT);
    while (status) {
        uint32_t jsStateMask = 0;
        for (uint32_t j = 0; j < kMaxSlots; ++j) {
            const uint32_t mask = 0x10001u << j;
            if (status & mask) jsStateMask |= mask;
            if (status & (1u << j)) {           // DONE(j)
                if (done[j][0]) done[j][1] = slotDequeue(j);
                else done[j][0] = slotDequeue(j);
            }
            if (status & (0x10000u << j)) {     // ERR(j)
                write32(JS_BASE + j * JS_SLOT_STRIDE + JS_COMMAND_NEXT,
                        JS_COMMAND_NOP);
                failed[j] = slotDequeue(j);
            }
        }
        write32(JOB_INT_CLEAR, status);
        jsState &= ~jsStateMask;
        jsState |= read32(JOB_INT_JS_STATE) & jsStateMask;
        jsEvents |= status;
        status = read32(JOB_INT_RAWSTAT);
    }
    for (uint32_t j = 0; j < kMaxSlots; ++j) {
        if (!(jsEvents & (0x10001u << j))) continue;
        if (failed[j]) {
            const uint32_t code =
                read32(JS_BASE + j * JS_SLOT_STRIDE + JS_STATUS);
            note("E5MALI1 js%u fault %02x (%s) head=%08x tail=%08x", j, code,
                 exceptionName(code),
                 read32(JS_BASE + j * JS_SLOT_STRIDE + JS_HEAD_LO),
                 read32(JS_BASE + j * JS_SLOT_STRIDE + JS_TAIL_LO));
            if (code == 0x03) {
                // STOPPED by our soft-stop: re-arm the chain from TAIL.
                E5MaliG1Atom *a = failed[j];
                a->jc = read64(JS_BASE + j * JS_SLOT_STRIDE + JS_TAIL_LO);
                a->state = kAtomQueued;
                enqueueFront(a, j);
            } else {
                completeAtom(failed[j],
                             code == 0x04 ? kAtomCancelled : kAtomFault, code);
            }
        } else if (slots_[j].ring[0] && !(jsState & (0x10001u << j))) {
            // Both hardware slots inactive but a ring entry remains: it
            // finished together with the other one (panfrost's case).
            E5MaliG1Atom *extra = slotDequeue(j);
            if (!done[j][0]) done[j][0] = extra;
            else if (done[j][1] == nullptr) done[j][1] = extra;
        }
        for (uint32_t i = 0; i < 2; ++i)
            if (done[j][i]) completeAtom(done[j][i], kAtomDone, 0x01);
    }
    for (uint32_t j = 0; j < kMaxSlots; ++j)
        if (!resetPending_) slotKick(j);
}

// Push waiting atoms of a slot into the hardware NEXT registers
// (panfrost_job_hw_submit + kbase JS_CONFIG assembly, audit §2.9).
bool E5MaliG1::slotKick(uint32_t slot) {
    SlotState &s = slots_[slot];
    while (s.queued > 0 && (!s.ring[0] || !s.ring[1]) && !resetPending_) {
        E5MaliG1Atom *a = s.wait[0];
        for (uint32_t i = 1; i < kSlotQueue; ++i) s.wait[i - 1] = s.wait[i];
        s.wait[kSlotQueue - 1] = nullptr;
        --s.queued;
        const uint32_t base = JS_BASE + slot * JS_SLOT_STRIDE;
        if (read32(base + JS_COMMAND_NEXT) != 0) {
            // Hardware still holds a NEXT command; put the atom back.
            for (uint32_t i = kSlotQueue - 1; i > 0; --i) s.wait[i] = s.wait[i - 1];
            s.wait[0] = a;
            ++s.queued;
            break;
        }
        uint32_t cfg = (uint32_t)a->ctx->as;
        cfg |= JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
               JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE | jsConfigThreadPri(8);
        // JOB_CHAIN_FLAG alternates per hardware queue position so a stopped
        // chain can be told apart from a fresh one (kbase semantics).
        if (slotChainFlag_ & (1u << slot)) cfg |= JS_CONFIG_JOB_CHAIN_FLAG;
        slotChainFlag_ ^= 1u << slot;
        // kbase submit writes exactly: AFFINITY_NEXT, HEAD_NEXT,
        // CONFIG_NEXT [, FLUSH_ID_NEXT], COMMAND_NEXT (mali_kbase_jm_hw.c:
        // 110-334). JS_TAIL is READ-ONLY (regmap_jm_uapi.h:33-34 "job queue
        // tail pointer") — there is no JS_TAIL_NEXT; TAIL==HEAD for a
        // consumed single-job chain is the JM's own bookkeeping (kbase
        // reads TAIL only as the STOPPED restart point, :447-449).
        write32(base + JS_HEAD_NEXT_LO, (uint32_t)a->jc);
        write32(base + JS_HEAD_NEXT_HI, (uint32_t)(a->jc >> 32));
        write32(base + JS_AFFINITY_NEXT_LO, (uint32_t)shaderPresent_);
        write32(base + JS_AFFINITY_NEXT_HI, (uint32_t)(shaderPresent_ >> 32));
        write32(base + JS_CONFIG_NEXT, cfg);
        if (gTracePerf == 1) gAtomPerf[a - atomPool_].submittedAt = mach_absolute_time();
        write32(base + JS_COMMAND_NEXT, JS_COMMAND_START);
        if (!s.ring[0]) s.ring[0] = a;
        else s.ring[1] = a;
        a->state = kAtomOnHw;
        a->submittedTickMs = tickMs_;
        if (gTraceJobs == 1 || bootSelftest_ || a->id <= 8 || !(a->id & (a->id - 1)))
          note("E5MALI1 submit atom %llu js%u as=%d headN=%llx affN=%llx cfgN=%08x cmdN=1 (tail: RO, never written)",
             (unsigned long long)a->id, slot, a->ctx->as,
             (unsigned long long)a->jc, (unsigned long long)shaderPresent_, cfg);
    }
    return true;
}

void E5MaliG1::enqueueFront(E5MaliG1Atom *a, uint32_t slot) {
    SlotState &s = slots_[slot];
    if (s.queued >= kSlotQueue) { completeAtom(a, kAtomCancelled, 0x04); return; }
    for (uint32_t i = kSlotQueue - 1; i > 0; --i) s.wait[i] = s.wait[i - 1];
    s.wait[0] = a;
    ++s.queued;
    a->state = kAtomQueued;
    slotKick(slot);
}

// ---------------------------------------------------------------------------
// MMU address-space plumbing (audit §2.8)
// ---------------------------------------------------------------------------
uint32_t E5MaliG1::faultyAsMask() const {
    uint32_t m = 0;
    for (uint32_t a = 0; a < kMaxAs; ++a)
        if (as_[a].ctx && as_[a].ctx->asFaulted) m |= 1u << a;
    return m;
}

bool E5MaliG1::asWaitReady(int32_t as, uint32_t timeoutUs) {
    const uint32_t base = MMU_BASE + ((uint32_t)as << MMU_AS_SHIFT);
    for (uint32_t i = 0; i < timeoutUs / 10 + 1; ++i) {
        if ((read32(base + AS_STATUS) & AS_STATUS_AS_ACTIVE) == 0) return true;
        IODelay(10);
    }
    return false;
}

void E5MaliG1::asCommand(int32_t as, uint32_t cmd) {
    const uint32_t base = MMU_BASE + ((uint32_t)as << MMU_AS_SHIFT);
    if (!asWaitReady(as, 100000)) {   // 100 ms bounded (panfrost waitready)
        note("E5MALI1 AS_ACTIVE stuck as=%d cmd=%u", as, cmd);
        return;
    }
    write32(base + AS_COMMAND, cmd);
}

void E5MaliG1::asFlushRange(int32_t as, uint64_t va, uint64_t len) {
    // lock_region + FLUSH_PT (panfrost lock_region + mmu_hw_do_operation).
    const uint32_t base = MMU_BASE + ((uint32_t)as << MMU_AS_SHIFT);
    const uint64_t region = E5MaliG1Mmu::lockRegionEncode(va, len);
    write64(base + AS_LOCKADDR_LO, region);
    asCommand(as, AS_COMMAND_LOCK);
    asCommand(as, AS_COMMAND_FLUSH_PT);
}

void E5MaliG1::asReprogram(int32_t as) {
    // kbase kbase_mmu_hw_configure: AARCH64_4K mode, WB+RA page-table walks,
    // outer-shareable PTW on an IO-coherent system (FDT coherency 0x1f).
    const uint32_t base = MMU_BASE + ((uint32_t)as << MMU_AS_SHIFT);
    E5MaliG1Context *ctx = as_[as].ctx;
    if (!ctx) return;
    asCommand(as, AS_COMMAND_FLUSH_MEM);
    write64(base + AS_TRANSTAB_LO, ctx->table.rootPhys());
    const uint64_t transcfg = AS_TRANSCFG_ADRMODE_AARCH64_4K |
                              AS_TRANSCFG_PTW_MEMATTR_WB | AS_TRANSCFG_PTW_RA |
                              AS_TRANSCFG_PTW_SH_OS;
    write64(base + AS_TRANSCFG_LO, transcfg);
    write64(base + AS_MEMATTR_LO, kAsMemattr);
    asCommand(as, AS_COMMAND_UPDATE);
}

int32_t E5MaliG1::asAcquire(E5MaliG1Context *ctx) {
    for (uint32_t a = 0; a < kMaxAs; ++a) {
        if (!(asPresent_ & (1u << a)) || as_[a].ctx) continue;
        as_[a].ctx = ctx;
        ctx->as = (int32_t)a;
        ctx->asFaulted = false;
        asReprogram(a);
        write32(MMU_INT_CLEAR, (1u << a) | (1u << (16 + a)));
        write32(MMU_INT_MASK, ~faultyAsMask());
        note("E5MALI1 as %u acquired (root=%llx)",
             a, (unsigned long long)ctx->table.rootPhys());
        return (int32_t)a;
    }
    return -1;
}

void E5MaliG1::asRelease(E5MaliG1Context *ctx) {
    const int32_t as = ctx->as;
    if (as < 0 || as >= (int32_t)kMaxAs || as_[as].ctx != ctx) return;
    const uint32_t base = MMU_BASE + ((uint32_t)as << MMU_AS_SHIFT);
    asCommand(as, AS_COMMAND_FLUSH_MEM);
    write64(base + AS_TRANSTAB_LO, 0);
    write64(base + AS_MEMATTR_LO, 0);
    write64(base + AS_TRANSCFG_LO, AS_TRANSCFG_ADRMODE_UNMAPPED);   // panfrost_mmu_disable
    asCommand(as, AS_COMMAND_UPDATE);
    as_[as].ctx = nullptr;
    ctx->as = -1;
    note("E5MALI1 as %d released", as);
}

// ---------------------------------------------------------------------------
// Fault paths
// ---------------------------------------------------------------------------
void E5MaliG1::handleMmuIrq(uint32_t status) {
    if (!status) return;
    // Per-AS bits: capture status/address, mark the context faulty, reset.
    // AS_FAULTSTATUS layout (V gpu/mali_kbase_gpu_regmap.h:242-268):
    //   [7:0] exception type — 0xC0|level = TRANSLATION_FAULT at walk level
    //         (0xC0=L0 .. 0xC3=L3, 0xC7=identity), 0xD0+ = transtab bus etc.
    //   [9:8] access type: 0 atomic, 1 execute, 2 read, 3 write
    //   [31:16] source id (job/L2 requestor, informational)
    // R316 sample: 0x7c1002c1 = TRANSLATION_FAULT_1 (PTW missed the just-
    // written L1 entry), access READ, source 0x7c10, addr == jc.
    static const char *const kAccess[4] = {"atomic", "execute", "read", "write"};
    for (uint32_t bit = 0; bit < kMaxAs; ++bit) {
        if (!(status & (1u << bit))) continue;
        const uint32_t base = MMU_BASE + (bit << MMU_AS_SHIFT);
        const uint32_t fs = read32(base + AS_FAULTSTATUS);
        const uint64_t fa = read64(base + AS_FAULTADDR_LO);
        e5_mali_g1_mmu_faults = e5_mali_g1_mmu_faults + 1;
        e5_mali_g1_last_fault_code = fs;
        e5_mali_g1_last_fault_addr = fa;
        note("E5MALI1 mmu fault as=%u code=%02x(%s) access=%s source=%04x addr=%llx raw=%08x",
             bit, fs & 0xff, exceptionName(fs & 0xff),
             kAccess[(fs >> 8) & 3], (fs >> 16) & 0xffff,
             (unsigned long long)fa, fs);
        write32(MMU_INT_CLEAR, 1u << bit);
        if (as_[bit].ctx) as_[bit].ctx->asFaulted = true;
    }
    write32(MMU_INT_MASK, ~faultyAsMask());
    resetPending_ = 1;
}

void E5MaliG1::handleGpuIrq(uint32_t status) {
    if (!status) return;
    if (status & (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT)) {
        const uint32_t fs = read32(GPU_FAULT_STATUS);
        const uint64_t fa = read64(GPU_FAULTADDR_LO);
        note("E5MALI1 gpu fault status=%08x addr=%llx", fs, (unsigned long long)fa);
        write32(GPU_INT_MASK, 0);
        resetPending_ = 1;
    }
    // RESET_COMPLETED is consumed inline by the polling loops.
}

// ---------------------------------------------------------------------------
// Reset: soft-stop every slot, GPU soft reset, reprogram ASs and IRQs,
// requeue the atoms that never reached hardware (audit §2.9 reset row).
// Runs on the workloop with state_ held; every wait is bounded.
// ---------------------------------------------------------------------------
void E5MaliG1::resetEngine(const char *why) {
    static uint32_t count = 0;
    // Storm guard (R319 board finding: deferred-reset loop #20..27+ after
    // teardown). Past kMaxResets the engine stays parked for the whole
    // boot: no more resets, no reprogramming, sources masked. Every cycle
    // of the old loop was IRQ + workloop + bounded polls, so 32 cycles is
    // a few seconds of churn at worst — nothing accumulates.
    if (gResetTotal >= kMaxResets) {
        if (gResetTotal == kMaxResets) {
            note("E5MALI1 reset cap %u reached (%s) — parked for this boot, no further resets",
                 kMaxResets, why);
            write32(JOB_INT_MASK, 0);
            write32(MMU_INT_MASK, 0);
            write32(GPU_INT_MASK, 0);
        }
        ++gResetTotal;
        return;   // resetPending_ intentionally stays set
    }
    ++gResetTotal;
    ++count;
    e5_mali_g1_resets = e5_mali_g1_resets + 1;
    resetPending_ = 1;
    note("E5MALI1 reset #%u (%s) total=%u/%u irqSrc job=%u mmu=%u gpu=%u",
         count, why, gResetTotal, kMaxResets, gIrqJob, gIrqMmu, gIrqGpu);
    // GPU storm kill (R328): 3rd reset within a 10s window = storm; a
    // single/double fault-recovery must survive (R327 board evidence:
    // MVJ timeout → reset, one re-fire → reset = LEGITIMATE recovery that
    // R326b's 2nd-reset rule wrongly parked). Ticks come from the watchdog
    // clock (tickMs_, 100ms cadence — coarse but sufficient for a 10s
    // window).
    {
        const uint32_t now = tickMs_;
        gResetWindow[gResetWindowIdx % 3] = now;
        gResetWindowIdx++;
        if (gResetWindowIdx >= 3) {
            bool allRecent = true;
            for (uint32_t i = 0; i < 3; ++i)
                if (now - gResetWindow[i] > 10000) allRecent = false;
            if (allRecent && !gGpuIntsDead) {
                gGpuIntsDead = true;
                write32(GPU_INT_CLEAR, ~0u);
                write32(GPU_INT_MASK, 0);
                note("E5MALI1 GPU ints killed: 3rd reset within 10s (storm, confirmed)");
            }
            if (!allRecent) {
                // Window expired quiet: judge any later burst on its own.
                gResetWindowIdx = 1;
                gResetWindow[0] = now;
            }
        }
    }
    // Stop new submissions and yank the next chain.
    write32(JOB_INT_MASK, 0);
    for (uint32_t j = 0; j < kMaxSlots; ++j) {
        const uint32_t base = JS_BASE + j * JS_SLOT_STRIDE;
        write32(base + JS_COMMAND_NEXT, JS_COMMAND_NOP);
        write32(base + JS_COMMAND, JS_COMMAND_SOFT_STOP);
    }
    // Bounded wait for the slots to go idle (10 ms).
    bool idle = false;
    for (int i = 0; i < 1000; ++i) {
        const uint32_t jsState = read32(JOB_INT_JS_STATE);
        bool all = true;
        for (uint32_t j = 0; j < kMaxSlots; ++j)
            if (jsState & (0x10001u << j)) { all = false; break; }
        if (all) { idle = true; break; }
        IODelay(10);
    }
    if (!idle) {
        note("E5MALI1 soft-stop incomplete (js_state=%08x) — escalating",
             read32(JOB_INT_JS_STATE));
        for (uint32_t j = 0; j < kMaxSlots; ++j)
            write32(JS_BASE + j * JS_SLOT_STRIDE + JS_COMMAND, 0x03);  // HARD_STOP
        for (int i = 0; i < 500; ++i) {
            const uint32_t jsState = read32(JOB_INT_JS_STATE);
            bool all = true;
            for (uint32_t j = 0; j < kMaxSlots; ++j)
                if (jsState & (0x10001u << j)) { all = false; break; }
            if (all) break;
            IODelay(10);
        }
    }
    // Drain anything the soft-stop completed before wiping state.
    handleJobIrq();
    // GPU reset, escalated for the wedged-slot case (R318: rawstat stayed 0
    // through soft AND hard reset): SOFT_RESET -> HARD_RESET -> settle,
    // clear, HARD_RESET again with a longer bound. RESET_COMPLETED is
    // polled in INT_RAWSTAT, which latches regardless of INT_MASK. Keep
    // GPU interrupts masked: irqPrimary clears masked status before the
    // workloop runs, so enabling RESET_COMPLETED lets it steal our signal.
    write32(GPU_INT_MASK, 0);
    write32(GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    write32(GPU_CMD, GPU_CMD_SOFT_RESET);
    bool done = false;
    for (int i = 0; i < 1000; ++i) {           // 10 ms
        if (read32(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED) { done = true; break; }
        IODelay(10);
    }
    if (!done) {
        note("E5MALI1 soft reset no completion (rawstat=%08x gpu_status=%08x) — hard reset",
             read32(GPU_INT_RAWSTAT), read32(GPU_STATUS));
        write32(GPU_CMD, GPU_CMD_HARD_RESET);
        for (int i = 0; i < 1000; ++i) {       // 10 ms
            if (read32(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED) { done = true; break; }
            IODelay(10);
        }
    }
    if (!done) {
        note("E5MALI1 hard reset no completion — settling 1 ms and retrying once");
        IODelay(1000);
        write32(GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
        write32(GPU_CMD, GPU_CMD_HARD_RESET);
        for (int i = 0; i < 10000; ++i) {      // 100 ms final bound
            if (read32(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED) { done = true; break; }
            IODelay(10);
        }
    }
    if (!done) {
        // R318 lesson: a fully wedged JM must not be "reprogrammed anyway" —
        // writes to JS/MMU state on a dead GPU are meaningless and can
        // corrupt the recovery picture. Park the engine instead: sources
        // stay masked and resetPending_ stays set, so Submit returns
        // NotReady, slotKick refuses, and the watchdog will not stack
        // another reset. Client exit (quiesce) still retires atoms.
        note("E5MALI1 GPU reset did NOT complete — engine parked (submissions refused; power-cycle needed to recover)");
        write32(JOB_INT_MASK, 0);
        write32(MMU_INT_MASK, 0);
        write32(GPU_INT_MASK, 0);
        return;   // resetPending_ intentionally left set
    }
    write32(GPU_INT_CLEAR, ~0u);
    // Re-arm core power (panfrost_device_reset re-runs gpu_power_on).
    const uint32_t coreMask = (l2Present_ == 1) ? 0xffffffffu
                                                : (uint32_t)(l2Present_ - 1);
    write32(GPU_L2_PWRON_LO, (uint32_t)l2Present_ & coreMask);
    write32(GPU_SHADER_PWRON_LO, (uint32_t)shaderPresent_ & coreMask);
    write32(GPU_TILER_PWRON_LO, (uint32_t)tilerPresent_);
    for (int i = 0; i < 2000; ++i) {
        const uint32_t l2 = read32(GPU_L2_READY_LO);
        const uint32_t sh = read32(GPU_SHADER_READY_LO);
        if (l2 == ((uint32_t)l2Present_ & coreMask) &&
            sh == ((uint32_t)shaderPresent_ & coreMask)) break;
        IODelay(10);
        if (i == 1999) note("E5MALI1 cores not ready after reset l2=%08x sh=%08x", l2, sh);
    }
    // Reprogram every active address space (the reset cleared them).
    for (uint32_t a = 0; a < kMaxAs; ++a) {
        if (!as_[a].ctx) continue;
        as_[a].ctx->asFaulted = false;
        asReprogram(a);
    }
    // Retire whatever is still on the rings (completed atoms were already
    // dropped by completeAtom's ring hygiene); queued atoms stay queued.
    for (uint32_t j = 0; j < kMaxSlots; ++j) {
        for (uint32_t i = 0; i < 2; ++i) {
            E5MaliG1Atom *a = slots_[j].ring[i];
            if (a && a->state == kAtomOnHw) completeAtom(a, kAtomReset, a->hwStatus);
        }
    }
    // Re-enable interrupts and kick the queues. GPU steady-state mask is
    // FAULT|MULTIPLE only: RESET_COMPLETED is consumed by polling
    // GPU_INT_RAWSTAT (mask-independent), and leaving it unmasked made every
    // reset raise a GPU IRQ of its own (+1 of the R323 +3-per-reset storm).
    // If stale GPU fault bits were ever observed post-reset (gGpuIntsDead),
    // GPU interrupts stay off for the rest of the boot — re-enabling them
    // on the next reset is exactly what sustained the deferred-reset loop.
    uint32_t jobMask = 0;
    for (uint32_t j = 0; j < kMaxSlots; ++j)
        if (slotUsable(j)) jobMask |= (0x10001u << j);
    write32(JOB_INT_CLEAR, ~0u);
    write32(JOB_INT_MASK, jobMask);
    write32(MMU_INT_CLEAR, ~0u);
    write32(MMU_INT_MASK, ~faultyAsMask());
    write32(GPU_INT_MASK,
            gGpuIntsDead ? 0 : (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT));
    // R321 storm identification + self-healing break: snapshot all source
    // views the instant we re-enable. A source that is STILL asserted after
    // one W1C clear (with no new submission outstanding) is stale or
    // pathological — mask exactly those bits off instead of letting them
    // re-trigger the deferred-reset loop; the rest stay live.
    {
        uint32_t jobStat = read32(JOB_INT_STAT);
        uint32_t mmuStat = read32(MMU_INT_STAT);
        uint32_t gpuStat = read32(GPU_INT_STAT);
        // GPU rawstat is also sampled here: fault bits sitting in RAWSTAT
        // behind the mask are the delayed re-assertion the R321 STAT-only
        // check missed (R323: gpu count kept climbing while STAT sampled 0).
        const uint32_t gpuRaw = read32(GPU_INT_RAWSTAT);
        if ((gpuRaw & (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT)) != 0) {
            write32(GPU_INT_CLEAR, gpuRaw);
            IODelay(100);
            const uint32_t gpuRaw2 = read32(GPU_INT_RAWSTAT);
            if ((gpuRaw2 & (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT)) != 0) {
                gGpuIntsDead = true;
                write32(GPU_INT_MASK, 0);
                note("E5MALI1 stale GPU fault bits %08x persist after clear — GPU ints off for this boot (storm break)",
                     gpuRaw2);
            }
        }
        if (jobStat | mmuStat | gpuStat) {
            note("E5MALI1 post-reset src asserted job=%08x mmu=%08x gpu=%08x (rawstat job=%08x gpu=%08x) — clearing once",
                 jobStat, mmuStat, gpuStat, read32(JOB_INT_RAWSTAT),
                 read32(GPU_INT_RAWSTAT));
            if (jobStat) write32(JOB_INT_CLEAR, jobStat);
            if (mmuStat) write32(MMU_INT_CLEAR, mmuStat);
            if (gpuStat) write32(GPU_INT_CLEAR, gpuStat);
            IODelay(100);
            jobStat = read32(JOB_INT_STAT);
            mmuStat = read32(MMU_INT_STAT);
            gpuStat = read32(GPU_INT_STAT);
            if (jobStat) {
                write32(JOB_INT_MASK, jobMask & ~jobStat);
                note("E5MALI1 stale JOB bits %08x masked off (mask now %08x)",
                     jobStat, jobMask & ~jobStat);
            }
            if (mmuStat) {
                write32(MMU_INT_MASK, ~faultyAsMask() & ~mmuStat);
                note("E5MALI1 stale MMU bits %08x masked off", mmuStat);
            }
            if (gpuStat) {
                gGpuIntsDead = true;
                write32(GPU_INT_MASK, 0);
                note("E5MALI1 stale GPU bits %08x — GPU ints masked off for this boot",
                     gpuStat);
            }
        }
    }
    note("E5MALI1 job mask wrote=%08x readback=%08x stat=%08x",
         jobMask, read32(JOB_INT_MASK), read32(JOB_INT_STAT));
    resetPending_ = 0;
    for (uint32_t j = 0; j < kMaxSlots; ++j) slotKick(j);
}

// ===========================================================================
// Per-client context (table pages in IOBufferMemoryDescriptor allocations)
// ===========================================================================
uint64_t E5MaliG1Context::tpAlloc(void *ctx) {
    E5MaliG1Context *c = static_cast<E5MaliG1Context *>(ctx);
    if (c->tpCount >= kMaxTablePages) return 0;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
        E5MaliG1Mmu::kPageSize, 0xfffff000ull);
    if (!md || md->prepare() != kIOReturnSuccess) {
        if (md) md->release();
        return 0;
    }
    const uint64_t phys = md->getPhysicalAddress();
    void *cpu = md->getBytesNoCopy();
    if (!phys || !cpu) {
        md->complete();
        md->release();
        return 0;
    }
    bzero(cpu, E5MaliG1Mmu::kPageSize);
    c->tp[c->tpCount].phys = phys;
    c->tp[c->tpCount].cpu = cpu;
    c->tp[c->tpCount].md = md;
    ++c->tpCount;
    return phys;
}

void E5MaliG1Context::tpFree(void *ctx, uint64_t phys) {
    E5MaliG1Context *c = static_cast<E5MaliG1Context *>(ctx);
    for (uint32_t i = 0; i < c->tpCount; ++i) {
        if (c->tp[i].phys != phys) continue;
        IOBufferMemoryDescriptor *md = c->tp[i].md;
        c->tp[i] = c->tp[c->tpCount - 1];
        --c->tpCount;
        ++c->tpFreed;
        if (md) { md->complete(); md->release(); }
        return;
    }
}

void *E5MaliG1Context::tpCpu(void *ctx, uint64_t phys) {
    E5MaliG1Context *c = static_cast<E5MaliG1Context *>(ctx);
    for (uint32_t i = 0; i < c->tpCount; ++i)
        if (c->tp[i].phys == phys) return c->tp[i].cpu;
    return nullptr;
}

bool E5MaliG1Context::init(uint64_t vaBase, uint64_t vaSize) {
    E5MaliG1Mmu::TableOps ops = {this, &tpAlloc, &tpFree, &tpCpu};
    table.init(ops);
    if (!va.init(vaBase, vaSize)) return false;
    return table.initRoot();
}

void E5MaliG1Context::teardown() {
    // Free every remaining table page. Callers must have quiesced the GPU
    // first (the client-exit path resets the engine when atoms were live).
    while (tpCount > 0) {
        Tp &p = tp[tpCount - 1];
        if (p.md) { p.md->complete(); p.md->release(); }
        --tpCount;
    }
}

// ===========================================================================
// UserClient — one per task; owns the context, BO handles and atom registry.
// ===========================================================================
// Defined below (after cacheCleanRange); every PTE-modifying path calls it
// before issuing MMU commands.
static void e5CtxCleanTables(E5MaliG1Context *ctx);

class E5MaliG1UserClient : public IOUserClient {
    OSDeclareDefaultStructors(E5MaliG1UserClient)
public:
    static E5MaliG1UserClient *withTask(task_t task, E5MaliG1 *driver) {
        E5MaliG1UserClient *uc = OSTypeAlloc(E5MaliG1UserClient);
        if (!uc) return nullptr;
        if (!uc->init()) { uc->release(); return nullptr; }
        uc->task_ = task;
        if (task) task_reference(task);
        uc->driver_ = driver;
        if (!uc->attach(driver)) { uc->release(); return nullptr; }
        if (!uc->start(driver)) {
            uc->detach(driver);
            uc->release();
            return nullptr;
        }
        return uc;
    }
    bool start(IOService *provider) override {
        if (!IOUserClient::start(provider)) return false;
        if (!driver_ || !e5_mali_g1_ready) return false;
        // IOKit objects are limited to 16 KiB. Keep the larger handle table
        // separately allocated, with the same client/DMA lifetime.
        bos_ = new Bo[kMaxBoHandles];
        if (!bos_) return false;
        E5MaliG1Context *ctx = new E5MaliG1Context;
        if (!ctx) return false;
        ctx->client = this;
        const uint64_t window = E5MaliG1::bootVaMb()
                                    ? ((uint64_t)E5MaliG1::bootVaMb() << 20)
                                    : kVaWindowDefault;
        bool ok = ctx->init(kVaWindowBase, window);
        if (ok) {
            // The root page was bzero'd by the allocator (dirty in the CPU
            // cache); push it to DRAM before the MMU ever walks it.
            e5CtxCleanTables(ctx);
            IOLockLock(driver_->stateLock());
            ok = driver_->asAcquire(ctx) >= 0;
            IOLockUnlock(driver_->stateLock());
        }
        if (!ok) {
            ctx->teardown();
            delete ctx;
            return false;
        }
        ctx_ = ctx;
        note("E5MALI1 client up (window %llx..%llx as=%d)",
             (unsigned long long)kVaWindowBase,
             (unsigned long long)(kVaWindowBase + window), ctx_->as);
        return true;
    }
    IOReturn clientClose() override {
        // Process-exit path: retire atoms, unmap everything, free the AS.
        // Nothing may panic or leak (G1 acceptance requirement).
        quiesceAndRelease();
        return kIOReturnSuccess;
    }
    void free() override {
        if (ctx_ && ctx_->as >= 0) quiesceAndRelease();
        if (ctx_) { ctx_->teardown(); delete ctx_; ctx_ = nullptr; }
        if (bos_) { delete[] bos_; bos_ = nullptr; }
        if (task_) { task_deallocate(task_); task_ = nullptr; }
        IOUserClient::free();
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
        if (!memory || type == 0 || type > kMaxBoHandles || !driver_ || !bos_)
            return kIOReturnBadArgument;
        IOLockLock(driver_->stateLock());
        IOMemoryDescriptor *descriptor = bos_[type - 1].descriptor();
        if (options && bos_[type - 1].userReadOnly) *options |= kIOMapReadOnly;
        if (descriptor) descriptor->retain();
        IOLockUnlock(driver_->stateLock());
        *memory = descriptor;
        return descriptor ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch = 0,
                            OSObject *target = 0, void *reference = 0) override;

    E5MaliG1Context *ctx() const { return ctx_; }

    void quiesceAndRelease() {
        E5MaliG1 *d = driver_;
        if (!d || !ctx_) return;
        IOLockLock(d->stateLock());
        bool active = false;
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] && (atoms_[i]->state == kAtomOnHw || atoms_[i]->state == kAtomQueued))
                active = true;
        if (active) {
            d->resetPending_ = 1;
            d->resetEngine("client exit");
        }
        // Retire every atom of this client and drop ring/wait entries.
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i) {
            E5MaliG1Atom *a = atoms_[i];
            if (!a) continue;
            if (a->state == kAtomOnHw || a->state == kAtomQueued)
                d->completeAtom(a, kAtomCancelled, 0x04);
            a->state = kAtomFree;
            a->owner = nullptr;
            atoms_[i] = nullptr;
        }
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            for (uint32_t i = 0; i < 2; ++i)
                if (d->slots_[s].ring[i] && d->slots_[s].ring[i]->owner == this)
                    d->slots_[s].ring[i] = nullptr;
            for (uint32_t i = 0; i < E5MaliG1::kSlotQueue; ++i)
                if (d->slots_[s].wait[i] && d->slots_[s].wait[i]->owner == this)
                    d->slots_[s].wait[i] = nullptr;
            uint32_t n = 0;
            for (uint32_t i = 0; i < E5MaliG1::kSlotQueue; ++i)
                if (d->slots_[s].wait[i]) ++n;
            d->slots_[s].queued = n;
        }
        // Unmap and free every BO (DMA quiesced by the reset above).
        for (uint32_t h = 1; h <= kMaxBoHandles; ++h) destroyBoLocked(h);
        if (ctx_->as >= 0) d->asRelease(ctx_);
        IOLockUnlock(d->stateLock());
        note("E5MALI1 client closed (as released, BOs freed)");
    }

    struct Bo {
        IOBufferMemoryDescriptor *md = nullptr;
        IOMemoryDescriptor *userMd = nullptr;
        IOMemoryMap *userMap = nullptr;
        uint32_t userCachePages = 0;
        IOMemoryDescriptor *descriptor() const { return md ? md : userMd; }
        void *bytes() const {
            return md ? md->getBytesNoCopy() :
                   userMap ? (void *)(uintptr_t)userMap->getVirtualAddress() : nullptr;
        }
        uint64_t size = 0;
        bool imported = false;
        bool userReadOnly = false;
        uint64_t importPa = 0;
        struct Mapping { uint64_t va = 0, len = 0; } maps[kMaxBoMappings];
        uint32_t mapCount = 0;
    };

    IOReturn createBo(const E5MaliCreateBo *in, E5MaliCreateBoOut *out);
    IOReturn importUser(const E5MaliImportUser *in, E5MaliCreateBoOut *out);
    IOReturn destroyBoLocked(uint32_t handle);
    IOReturn mapBo(const E5MaliMapBo *in, E5MaliMapBoOut *out);
    IOReturn unmapBo(const E5MaliUnmapBo *in, uint32_t *status);
    IOReturn importFixed(const E5MaliImportFixed *in, E5MaliMapBoOut *out);
    IOReturn submit(const E5MaliSubmit *in, E5MaliSubmitOut *out);
    IOReturn submitHandles(uint64_t jc, uint32_t slot, const uint64_t *udata,
                           const uint32_t *handles, uint32_t count, E5MaliSubmitOut *out);
    IOReturn waitAtom(const E5MaliWaitAtom *in, E5MaliWaitAtomOut *out, bool wait);
    static void cacheCleanRange(void *p, uint32_t bytes);
    static void cacheInvalidateRange(void *p, uint32_t bytes);
    bool dmaActive() const {
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] && (atoms_[i]->state == kAtomQueued || atoms_[i]->state == kAtomOnHw))
                return true;
        return false;
    }
    bool handlePinned(uint32_t handle) const {
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] && atoms_[i]->state != kAtomFree)
                for (uint32_t p = 0; p < atoms_[i]->pinnedCount; ++p)
                    if (atoms_[i]->pinnedBo[p] == handle) return true;
        return false;
    }

    // ---- in-kernel self-test (board-test-plan §2/§3, kernel edition) ----
    // Runs on the delayed self-test kernel thread (NOT the workloop, so
    // WaitAtom's 1 ms poll sleeps never block IRQ processing). Boundary
    // note: it drives THIS UserClient's externalMethod dispatch with kernel
    // buffers — the exact validation/implementation path a userspace client
    // reaches through IOConnectCallStructMethod — bypassing only the mach
    // message + IOUserClient connection layer (no user task exists here).
    void runSelftest(uint32_t mask);
    bool runTri4(bool scanout = false); // §4 offscreen / §5 imported target
    IOReturn selfCall(uint32_t sel, const void *in, uint32_t inLen, void *out,
                      uint32_t outLen, uint32_t *outLenUsed);

    task_t task_ = nullptr;
    E5MaliG1 *driver_ = nullptr;
    E5MaliG1Context *ctx_ = nullptr;
    Bo *bos_ = nullptr;
    E5MaliG1Atom *atoms_[kMaxClientAtoms] = {};
    uint64_t liveBoBytes_ = 0;
};

OSDefineMetaClassAndStructors(E5MaliG1UserClient, IOUserClient)

IOReturn E5MaliG1UserClient::createBo(const E5MaliCreateBo *in, E5MaliCreateBoOut *out) {
    bzero(out, sizeof(*out));
    if (in->size == 0 || in->size > (256u << 20)) {
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    const uint64_t size =
        (in->size + E5MaliG1Mmu::kPageSize - 1) & ~(E5MaliG1Mmu::kPageSize - 1);
    // Physically-scattered is allowed: the map path walks segments.
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, (IOOptionBits)kIODirectionInOut, size, 0xfffff000ull);
    if (!md || md->prepare() != kIOReturnSuccess) {
        if (md) md->release();
        out->status = kIOReturnNoMemory;
        return kIOReturnNoMemory;
    }
    for (uint32_t h = 0; h < kMaxBoHandles; ++h) {
        if (bos_[h].descriptor() || bos_[h].imported) continue;
        bos_[h].md = md;
        bos_[h].size = size;
        bos_[h].mapCount = 0;
        liveBoBytes_ += size;
        out->handle = h + 1;
        out->size = size;
        out->status = kIOReturnSuccess;
        if (h == 64 || h == 127 || h == 255)
            note("E5MALI BO TABLE handle=%u bytes=%llu capacity=%u", h + 1,
                 (unsigned long long)size, kMaxBoHandles);
        return kIOReturnSuccess;
    }
    md->complete();
    md->release();
    out->status = kIOReturnNoSpace;
    return kIOReturnNoSpace;
}

IOReturn E5MaliG1UserClient::importUser(const E5MaliImportUser *in, E5MaliCreateBoOut *out) {
    bzero(out, sizeof(*out));
    out->status = kIOReturnBadArgument;
    if (!task_ || task_ == kernel_task || !in->address || !in->length ||
        in->length > (256ull << 20) || (in->address & 4095) || (in->length & 4095) ||
        in->address >= (1ull << 48) || in->length > (1ull << 48) - in->address ||
        (in->flags & ~E5_MALI_IMPORT_READ_ONLY) || in->reserved) return out->status;
    const bool readOnly = (in->flags & E5_MALI_IMPORT_READ_ONLY) != 0;
    const IODirection direction = readOnly ? kIODirectionOut : kIODirectionInOut;
    // Wire before publishing the handle; VM operations run outside the GPU lock.
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
        in->address, in->length, ((IOOptionBits)direction | (IOOptionBits)kIOMemoryMapperNone), task_);
    if (!md) { out->status = kIOReturnNoMemory; return out->status; }
    IOReturn result = md->prepare(direction);
    if (result != kIOReturnSuccess) { md->release(); out->status = result; return result; }
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0,
        kIOMapAnywhere | (readOnly ? kIOMapReadOnly : 0));
    if (!map || !map->getVirtualAddress()) {
        if (map) map->release();
        md->complete(); md->release(); out->status = kIOReturnNoMemory; return out->status;
    }
    uint32_t cachePages = 0;
    if (!(readOnly ? E5UserCache::syncUserRange(md, 0, in->length, 0) :
                    E5UserCache::acquire(md, in->length, &cachePages))) {
        map->release(); md->complete(); md->release();
        out->status = kIOReturnNoResources; return out->status;
    }
    IOLockLock(driver_->stateLock());
    result = kIOReturnNotReady;
    if (ctx_ && ctx_->as >= 0 && !ctx_->asFaulted) {
        result = kIOReturnNoSpace;
        for (uint32_t i = 0; i < kMaxBoHandles; ++i) {
            Bo &bo = bos_[i];
            if (bo.descriptor() || bo.imported) continue;
            bo.userMd = md; bo.userMap = map; bo.userCachePages = cachePages;
            bo.userReadOnly = readOnly;
            bo.size = in->length; bo.mapCount = 0;
            liveBoBytes_ += bo.size;
            out->handle = i + 1; out->size = bo.size; result = kIOReturnSuccess; break;
        }
    }
    IOLockUnlock(driver_->stateLock());
    if (result == kIOReturnSuccess && readOnly) {
        static uint32_t reports;
        if (__atomic_add_fetch(&reports, 1u, __ATOMIC_RELAXED) <= 8)
            note("E5MALI RO IMPORT handle=%u bytes=%llu", out->handle, (unsigned long long)in->length);
    }
    if (result != kIOReturnSuccess) {
        E5UserCache::release(md, cachePages);
        map->release(); md->complete(); md->release();
    }
    out->status = result; return result;
}

// DMA-safe release: refuses while an atom pins the handle; otherwise unmaps
// every mapping, flushes the AS range, then drops the buffer.
IOReturn E5MaliG1UserClient::destroyBoLocked(uint32_t handle) {
    if (handle == 0 || handle > kMaxBoHandles) return kIOReturnBadArgument;
    Bo &bo = bos_[handle - 1];
    if (!bo.descriptor() && !bo.imported) return kIOReturnBadArgument;
    if (handlePinned(handle) || dmaActive()) return kIOReturnBusy;
    E5MaliG1 *d = driver_;
    for (uint32_t m = 0; m < bo.mapCount; ++m) {
        const uint64_t va = bo.maps[m].va, len = bo.maps[m].len;
        ctx_->table.unmap(va, len);
        e5CtxCleanTables(ctx_);   // cleared PTEs to DRAM before FLUSH_PT
        ctx_->va.free(va, len);
        ctx_->mappedBytes -= len;
        if (ctx_->as >= 0 && !ctx_->asFaulted) d->asFlushRange(ctx_->as, va, len);
    }
    bo.mapCount = 0;
    if (!bo.imported && bo.md) {
        bo.md->complete();
        bo.md->release();
        bo.md = nullptr;
    }
    if (bo.userMd) { E5UserCache::release(bo.userMd, bo.userCachePages); bo.userCachePages = 0; }
    if (bo.userMap) { bo.userMap->release(); bo.userMap = nullptr; }
    if (bo.userMd) {
        bo.userMd->complete(); bo.userMd->release(); bo.userMd = nullptr;
    }
    bo.imported = false;
    bo.userReadOnly = false;
    bo.importPa = 0;
    liveBoBytes_ -= bo.size;
    bo.size = 0;
    return kIOReturnSuccess;
}

IOReturn E5MaliG1UserClient::mapBo(const E5MaliMapBo *in, E5MaliMapBoOut *out) {
    bzero(out, sizeof(*out));
    E5MaliG1 *d = driver_;
    if (in->handle == 0 || in->handle > kMaxBoHandles ||
        (!bos_[in->handle - 1].descriptor() && !bos_[in->handle - 1].imported)) {
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    Bo &bo = bos_[in->handle - 1];
    if (bo.userMd && !(in->flags & 2)) {
        out->status = kIOReturnNotPermitted;
        return kIOReturnNotPermitted;
    }
    if (bo.mapCount >= kMaxBoMappings) { out->status = kIOReturnNoSpace; return kIOReturnNoSpace; }
    uint64_t length = in->length ? in->length : bo.size - in->offset;
    if (in->offset > bo.size || length == 0 || length % E5MaliG1Mmu::kPageSize ||
        in->offset % E5MaliG1Mmu::kPageSize || in->offset + length > bo.size) {
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    if (in->flags & 1) {
        // Fixed VA: must be free in the window.
        if (in->vaHint % E5MaliG1Mmu::kPageSize) {
            out->status = kIOReturnBadArgument;
            return kIOReturnBadArgument;
        }
        const E5MaliG1Mmu::PageTable::Lookup lu =
            ctx_->table.lookupEntry(in->vaHint);
        if (lu.mapped) {
            out->status = kIOReturnBusy;
            return kIOReturnBusy;
        }
        if (!ctx_->va.reserveFixed(in->vaHint, length)) {
            out->status = kIOReturnBusy;
            return kIOReturnBusy;
        }
    }
    uint64_t va = in->flags & 1 ? in->vaHint : ctx_->va.alloc(length);
    if (!va) { out->status = kIOReturnNoSpace; return kIOReturnNoSpace; }
    E5MaliG1Mmu::MapFlags flags;
    flags.noExec = (in->flags & 2) != 0;
    if (bo.userReadOnly) flags.write = false;
    uint64_t mapped = 0;
    if (bo.imported) {
        // Fixed physical window: straight 4 KiB ATEs.
        for (; mapped < length; mapped += E5MaliG1Mmu::kPageSize) {
            if (!ctx_->table.map(va + mapped, bo.importPa + in->offset + mapped,
                                 E5MaliG1Mmu::kPageSize, flags)) goto fail;
        }
    } else {
        // Walk the buffer's physical segments page by page.
        IOByteCount segLen = 0;
        uint64_t seg = bo.descriptor()->getPhysicalSegment(in->offset, &segLen, kIOMemoryMapperNone);
        while (mapped < length && seg) {
            if (!ctx_->table.map(va + mapped, seg & ~(E5MaliG1Mmu::kPageSize - 1),
                                 E5MaliG1Mmu::kPageSize, flags)) goto fail;
            mapped += E5MaliG1Mmu::kPageSize;
            if (segLen > E5MaliG1Mmu::kPageSize) {
                segLen -= E5MaliG1Mmu::kPageSize;
                seg += E5MaliG1Mmu::kPageSize;
            } else {
                seg = bo.descriptor()->getPhysicalSegment(in->offset + mapped, &segLen, kIOMemoryMapperNone);
            }
        }
        if (mapped < length) goto fail;
    }
    bo.maps[bo.mapCount].va = va;
    bo.maps[bo.mapCount].len = length;
    ++bo.mapCount;
    ctx_->mappedBytes += length;
    // Fresh PTEs to DRAM, then let the MMU re-walk (LOCK + FLUSH_PT).
    e5CtxCleanTables(ctx_);
    if (ctx_->as >= 0 && !ctx_->asFaulted) d->asFlushRange(ctx_->as, va, length);
    out->va = va;
    out->status = kIOReturnSuccess;
    return kIOReturnSuccess;
fail:
    if (mapped) {
        ctx_->table.unmap(va, mapped);
        e5CtxCleanTables(ctx_);
    }
    ctx_->va.free(va, length);
    out->status = kIOReturnNoMemory;
    return kIOReturnNoMemory;
}

IOReturn E5MaliG1UserClient::unmapBo(const E5MaliUnmapBo *in, uint32_t *status) {
    E5MaliG1 *d = driver_;
    if (in->handle == 0 || in->handle > kMaxBoHandles ||
        (!bos_[in->handle - 1].descriptor() && !bos_[in->handle - 1].imported)) {
        *status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    Bo &bo = bos_[in->handle - 1];
    for (uint32_t m = 0; m < bo.mapCount; ++m) {
        if (bo.maps[m].va != in->va || bo.maps[m].len != in->length) continue;
        // Refuse while an atom may still DMA through this mapping.
        if (handlePinned(in->handle) || dmaActive()) { *status = kIOReturnBusy; return kIOReturnBusy; }
        if (!ctx_->table.unmap(in->va, in->length)) {
            *status = kIOReturnIOError;
            return kIOReturnIOError;
        }
        e5CtxCleanTables(ctx_);   // invalidated PTEs to DRAM before FLUSH_PT
        ctx_->va.free(in->va, in->length);
        ctx_->mappedBytes -= in->length;
        bo.maps[m] = bo.maps[bo.mapCount - 1];
        --bo.mapCount;
        if (ctx_->as >= 0 && !ctx_->asFaulted)
            d->asFlushRange(ctx_->as, in->va, in->length);
        *status = kIOReturnSuccess;
        return kIOReturnSuccess;
    }
    *status = kIOReturnBadArgument;
    return kIOReturnBadArgument;
}

IOReturn E5MaliG1UserClient::importFixed(const E5MaliImportFixed *in, E5MaliMapBoOut *out) {
    bzero(out, sizeof(*out));
    if (!E5MaliG1::bootImport()) {
        out->status = kIOReturnNotPermitted;
        return kIOReturnNotPermitted;
    }
    if (in->physAddr < kDramBase || in->physAddr >= kDramEnd ||
        in->physAddr % E5MaliG1Mmu::kPageSize || in->length == 0 ||
        in->length % E5MaliG1Mmu::kPageSize ||
        in->length > kDramEnd - in->physAddr) {
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    E5MaliG1 *d = driver_;
    IOLockLock(d->stateLock());
    uint32_t h = 0;
    for (uint32_t i = 0; i < kMaxBoHandles; ++i)
        if (!bos_[i].descriptor() && !bos_[i].imported) { h = i + 1; break; }
    if (!h) { IOLockUnlock(d->stateLock()); return kIOReturnNoSpace; }
    Bo &bo = bos_[h - 1];
    const uint64_t va = ctx_->va.alloc(in->length);
    if (!va) { IOLockUnlock(d->stateLock()); return kIOReturnNoSpace; }
    E5MaliG1Mmu::MapFlags flags;
    flags.noExec = (in->flags & 1) != 0;
    bool ok = true;
    for (uint64_t off = 0; off < in->length && ok; off += E5MaliG1Mmu::kPageSize)
        ok = ctx_->table.map(va + off, in->physAddr + off,
                             E5MaliG1Mmu::kPageSize, flags);
    if (!ok) {
        ctx_->table.unmap(va, in->length);
        e5CtxCleanTables(ctx_);
        ctx_->va.free(va, in->length);
        IOLockUnlock(d->stateLock());
        out->status = kIOReturnNoMemory;
        return kIOReturnNoMemory;
    }
    bo.imported = true;
    bo.importPa = in->physAddr;
    bo.size = in->length;
    bo.maps[0].va = va;
    bo.maps[0].len = in->length;
    bo.mapCount = 1;
    liveBoBytes_ += in->length;
    ctx_->mappedBytes += in->length;
    e5CtxCleanTables(ctx_);
    if (ctx_->as >= 0 && !ctx_->asFaulted) d->asFlushRange(ctx_->as, va, in->length);
    IOLockUnlock(d->stateLock());
    out->va = va;
    out->status = kIOReturnSuccess;
    note("E5MALI1 import pa=%llx len=%llx va=%llx",
         (unsigned long long)in->physAddr, (unsigned long long)in->length,
         (unsigned long long)va);
    return kIOReturnSuccess;
}

void E5MaliG1UserClient::cacheCleanRange(void *p, uint32_t bytes) {
    const uintptr_t start = (uintptr_t)p & ~(uintptr_t)63;
    for (uintptr_t at = start; at < (uintptr_t)p + bytes; at += 64)
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

void E5MaliG1UserClient::cacheInvalidateRange(void *p, uint32_t bytes) {
    // Drop cached lines so a GPU write to DRAM becomes CPU-visible. Only
    // valid after the CPU side of the range was cleaned (dirty lines would
    // be lost) — the self-test always CleanBo's before submit.
    const uintptr_t start = (uintptr_t)p & ~(uintptr_t)63;
    for (uintptr_t at = start; at < (uintptr_t)p + bytes; at += 64)
        __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// Clean every live page-table page of a context to the Point of Coherence
// so the (R316-proven non-snooping) page-table walker reads current entries
// from DRAM. R316 evidence: first walk of a freshly mapped jc faulted with
// AS_FAULTSTATUS type 0xC1 (TRANSLATION_FAULT at walk level 1 — the
// just-written L1 entry), fault address == jc.
//
// This mirrors kbase kbase_mmu_sync_pgd_cpu (V mali_kbase_mmu.c:352-368:
// "In non-coherent system, ensure the GPU can read the pages from memory")
// and panfrost's per-PTE dma_sync (__arm_lpae_sync_pte, L io-pgtable-arm.c:
// 317-322), applied unconditionally: the FDT's system-coherency=0x1f claim
// evidently does not make this PTW snoop kernel dirty lines under XNU.
static void e5CtxCleanTables(E5MaliG1Context *ctx) {
    for (uint32_t i = 0; i < ctx->tpCount; ++i)
        if (ctx->tp[i].cpu)
            E5MaliG1UserClient::cacheCleanRange(ctx->tp[i].cpu,
                                                E5MaliG1Mmu::kPageSize);
}

IOReturn E5MaliG1UserClient::submit(const E5MaliSubmit *in, E5MaliSubmitOut *out) {
    uint32_t count = 0;
    while (count < 8 && in->boHandles[count]) ++count;
    return submitHandles(in->jc, in->slot, in->udata, in->boHandles, count, out);
}

IOReturn E5MaliG1UserClient::submitHandles(uint64_t jc, uint32_t slot, const uint64_t *udata,
                                         const uint32_t *handles, uint32_t count,
                                         E5MaliSubmitOut *out) {
    bzero(out, sizeof(*out));
    E5MaliG1 *d = driver_;
    if (count > E5_MALI_MAX_SUBMIT_BOS || slot >= kMaxSlots || !d->slotUsable(slot)) {
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    // R326 sequencing fix: a reset in flight (stale-source IRQ already set
    // resetPending_, workloop reset not yet run) must not fail the submit
    // outright — drain it, bounded. Permanent park keeps returning NotReady.
    for (uint32_t ms = 0; ms < 250 && d->resetPending(); ++ms) IOSleep(1);
    if (d->resetPending()) { out->status = kIOReturnNotReady; return kIOReturnNotReady; }
    if (ctx_->asFaulted) { out->status = kIOReturnNotPermitted; return kIOReturnNotPermitted; }
    // jc must land inside a mapped, executable GPU VA of this context.
    const E5MaliG1Mmu::PageTable::Lookup lu = ctx_->table.lookupEntry(jc);
    if (!lu.mapped) {
        note("E5MALI1 submit jc=%llx unmapped — refused", (unsigned long long)jc);
        out->status = kIOReturnBadArgument;
        return kIOReturnBadArgument;
    }
    if (lu.entry & E5MaliG1Mmu::kNoExecBit) {
        out->status = kIOReturnNotPermitted;
        return kIOReturnNotPermitted;
    }
    // Pin every BO handle first; all-or-nothing.
    uint32_t pins[E5_MALI_MAX_SUBMIT_BOS] = {};
    uint32_t pinCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t h = handles[i];
        if (!h || h > kMaxBoHandles || (!bos_[h - 1].descriptor() && !bos_[h - 1].imported)) {
            note("E5MALI1 SUBMIT REJECT invalid handle=%u count=%u", h, count);
            out->status = kIOReturnBadArgument;
            return kIOReturnBadArgument;
        }
        bool dup = false;
        for (uint32_t k = 0; k < pinCount; ++k) dup |= pins[k] == h;
        if (dup) continue;
        pins[pinCount++] = h;
    }
    bool chainPinned = false;
    for (uint32_t i = 0; i < pinCount; ++i) {
        const Bo &bo = bos_[pins[i] - 1];
        for (uint32_t m = 0; m < bo.mapCount; ++m)
            if (jc >= bo.maps[m].va && jc - bo.maps[m].va < bo.maps[m].len)
                chainPinned = true;
    }
    if (!chainPinned) { out->status = kIOReturnBadArgument; return kIOReturnBadArgument; }
    const uint64_t cleanBegin = gTracePerf == 1 ? mach_absolute_time() : 0;
    uint64_t importedBytes = 0;
    for (uint32_t i = 0; i < pinCount; ++i) {
        const Bo &bo = bos_[pins[i] - 1];
        if (bo.userMd) importedBytes += bo.size;
        if (bo.userMd && !E5UserCache::syncUserRange(bo.userMd, 0, bo.size, 1)) {
            out->status = kIOReturnNotPermitted;
            return out->status;
        }
    }
    // Atom from the shared pool.
    E5MaliG1Atom *a = nullptr;
    for (uint32_t i = 0; i < E5MaliG1::kAtomPool; ++i)
        if (d->atomPool()[i].state == kAtomFree) { a = &d->atomPool()[i]; break; }
    if (!a) { out->status = kIOReturnNoSpace; return kIOReturnNoSpace; }
    static uint64_t nextId = 1;
    a->id = nextId++;
    if (gTracePerf == 1) {
        auto &perf = gAtomPerf[a - d->atomPool()];
        perf = {a->id, mach_absolute_time() - cleanBegin, 0, 0, importedBytes};
    }
    a->jc = jc;
    a->slot = (uint8_t)slot;
    a->state = kAtomQueued;
    a->hwStatus = 0;
    a->udata[0] = udata[0];
    a->udata[1] = udata[1];
    a->owner = this;
    a->ctx = ctx_;
    a->pinnedCount = pinCount;
    for (uint32_t i = 0; i < pinCount; ++i) a->pinnedBo[i] = pins[i];
    // Register in this client's table.
    bool placed = false;
    for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
        if (!atoms_[i]) { atoms_[i] = a; placed = true; break; }
    if (!placed) { a->state = kAtomFree; out->status = kIOReturnNoSpace; return kIOReturnNoSpace; }
    E5MaliG1::SlotState &s = d->slots_[slot];
    if (s.queued >= E5MaliG1::kSlotQueue) {
        a->state = kAtomFree;
        a->owner = nullptr;
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] == a) atoms_[i] = nullptr;
        out->status = kIOReturnNoSpace;
        return kIOReturnNoSpace;
    }
    s.wait[s.queued++] = a;
    e5_mali_g1_submitted = e5_mali_g1_submitted + 1;
    d->slotKick(slot);
    out->atomId = a->id;
    out->status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn E5MaliG1UserClient::waitAtom(const E5MaliWaitAtom *in, E5MaliWaitAtomOut *out,
                                      bool wait) {
    bzero(out, sizeof(*out));
    E5MaliG1 *d = driver_;
    uint32_t spins = 0;
    for (;;) {
        IOLockLock(d->stateLock());
        E5MaliG1Atom *a = nullptr;
        uint32_t idx = 0xff;
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] && atoms_[i]->id == in->atomId) { a = atoms_[i]; idx = i; break; }
        if (!a) {
            IOLockUnlock(d->stateLock());
            out->status = kIOReturnNotFound;
            return kIOReturnNotFound;
        }
        // Signal (a): the slot's DONE/ERR bit latched in RAWSTAT (the real
        // IRQ path — kept in case the GIC dispatch is merely delayed).
        // R321 proved bare write-value jobs complete WITHOUT this bit, so
        // the jm-internal signal lives one layer up (selftest pairs the
        // consumption regs with the data predicate; see runSelftest).
        if (a->state == kAtomOnHw &&
            (d->read32(JOB_INT_RAWSTAT) & (0x10001u << a->slot)))
            d->handleJobIrq();
        if (a->state != kAtomQueued && a->state != kAtomOnHw) {
            const uint8_t state = a->state;
            const uint32_t hw = a->hwStatus;
            const uint64_t seq = d->completionSeq();
            // Publish GPU-written cache lines before the completion reaches
            // userspace and before dropping the BO pins. Preserve concurrently
            // modified CPU-only metadata lines in shared bitmap allocations.
            IOReturn synced = kIOReturnSuccess;
            const uint64_t retireBegin = gTracePerf == 1 ? mach_absolute_time() : 0;
            for (uint32_t n = 0; n < a->pinnedCount; ++n) {
                const Bo &bo = bos_[a->pinnedBo[n] - 1];
                if (bo.userMd && !bo.userReadOnly &&
                    !E5UserCache::syncUserRange(bo.userMd, 0, bo.size, 3))
                    synced = kIOReturnNotPermitted;
            }
            if (gTracePerf == 1 && (a->id <= 8 || !(a->id & 63))) {
                const auto &perf = gAtomPerf[a - d->atomPool()];
                const uint64_t now = mach_absolute_time();
                if (perf.id == a->id && perf.submittedAt && perf.completedAt >= perf.submittedAt)
                    note("E5GPUPERF atom=%llu client=%p slot=%u bos=%u imported_bytes=%llu submit_sync_us=%llu hw_us=%llu observe_us=%llu retire_sync_us=%llu polls=%u state=%u",
                         (unsigned long long)a->id, this, a->slot, a->pinnedCount,
                         (unsigned long long)perf.importedBytes, (unsigned long long)e5PerfUs(perf.cleanTicks),
                         (unsigned long long)e5PerfUs(perf.completedAt - perf.submittedAt),
                         (unsigned long long)e5PerfUs(retireBegin - perf.submittedAt),
                         (unsigned long long)e5PerfUs(now - retireBegin), spins, state);
            }
            // Terminal: retire the pool slot and the client registry entry
            // (the id becomes non-queryable afterwards; see userclient-abi.md).
            a->pinnedCount = 0;
            a->state = kAtomFree;
            a->owner = nullptr;
            atoms_[idx] = nullptr;
            IOLockUnlock(d->stateLock());
            out->state = state;
            out->hwStatus = hw;
            out->completionSeq = seq;
            out->status = synced;
            return synced;
        }
        if (!wait || in->timeoutMs == 0 || spins >= in->timeoutMs) {
            const uint8_t state = a->state;
            const uint32_t hw = a->hwStatus;
            const uint64_t seq = d->completionSeq();
            IOLockUnlock(d->stateLock());
            out->state = state;
            out->hwStatus = hw;
            out->completionSeq = seq;
            out->status = kIOReturnSuccess;
            return kIOReturnSuccess;
        }
        IOLockUnlock(d->stateLock());
        IOSleep(1);
        ++spins;
    }
}

// ---------------------------------------------------------------------------
// externalMethod dispatch: exact structure sizes, no scalar fallback.
// ---------------------------------------------------------------------------
IOReturn E5MaliG1UserClient::externalMethod(uint32_t selector,
                                            IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *, OSObject *,
                                            void *) {
    if (!driver_ || !ctx_ || !e5_mali_g1_ready) return kIOReturnNotReady;
    if (selector >= kSelCount) return kIOReturnUnsupported;
    E5MaliG1 *d = driver_;
    switch (selector) {
    case kSelGetVersion: {
        if (args->structureOutputSize < sizeof(E5MaliInfo)) return kIOReturnNoSpace;
        E5MaliInfo info = {};
        info.abiMajor = kE5MaliAbiMajor;
        info.abiMinor = kE5MaliAbiMinor;
        info.gpuIdRaw = d->gpuId();
        info.gpuStatus = e5_mali_g1_status;
        info.shaderPresent = d->shaderPresent();
        info.l2Present = d->l2Present();
        info.tilerPresent = d->tilerPresent();
        info.mmuFeatures = d->mmuFeatures();
        info.asPresent = d->asPresent();
        info.jsPresent = d->jsPresent();
        info.vaBits = d->mmuFeatures() & 0xff;
        info.paBits = (d->mmuFeatures() >> 8) & 0xff;
        info.maxSlots = d->numSlots();
        info.maxAs = d->numAs();
        info.vaWindowBytes = (uint32_t)ctx_->va.size();
        memcpy(args->structureOutput, &info, sizeof(info));
        args->structureOutputSize = sizeof(info);
        return kIOReturnSuccess;
    }
    case kSelCreateBo: {
        if (args->structureInputSize != sizeof(E5MaliCreateBo) ||
            args->structureOutputSize < sizeof(E5MaliCreateBoOut)) return kIOReturnBadArgument;
        E5MaliCreateBoOut out;
        IOLockLock(d->stateLock());
        const IOReturn r = createBo((const E5MaliCreateBo *)args->structureInput, &out);
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelDestroyBo: {
        if (args->structureInputSize != sizeof(E5MaliDestroyBo)) return kIOReturnBadArgument;
        IOLockLock(d->stateLock());
        const IOReturn r = destroyBoLocked(((const E5MaliDestroyBo *)args->structureInput)->handle);
        IOLockUnlock(d->stateLock());
        return r;
    }
    case kSelMapBo: {
        if (args->structureInputSize != sizeof(E5MaliMapBo) ||
            args->structureOutputSize < sizeof(E5MaliMapBoOut)) return kIOReturnBadArgument;
        E5MaliMapBoOut out;
        IOLockLock(d->stateLock());
        const IOReturn r = mapBo((const E5MaliMapBo *)args->structureInput, &out);
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelUnmapBo: {
        if (args->structureInputSize != sizeof(E5MaliUnmapBo)) return kIOReturnBadArgument;
        uint32_t status = 0;
        IOLockLock(d->stateLock());
        const IOReturn r = unmapBo((const E5MaliUnmapBo *)args->structureInput, &status);
        IOLockUnlock(d->stateLock());
        if (r != kIOReturnSuccess && status == 0) status = (uint32_t)r;
        return r;
    }
    case kSelImportUser: {
        if (args->structureInputSize != sizeof(E5MaliImportUser) ||
            args->structureOutputSize < sizeof(E5MaliCreateBoOut)) return kIOReturnBadArgument;
        E5MaliCreateBoOut out = {};
        const IOReturn r = importUser((const E5MaliImportUser *)args->structureInput, &out);
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelImportFixed: {
        if (args->structureInputSize != sizeof(E5MaliImportFixed) ||
            args->structureOutputSize < sizeof(E5MaliMapBoOut)) return kIOReturnBadArgument;
        E5MaliMapBoOut out;
        const IOReturn r = importFixed((const E5MaliImportFixed *)args->structureInput, &out);
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelSubmit: {
        if (args->structureInputSize != sizeof(E5MaliSubmit) ||
            args->structureOutputSize < sizeof(E5MaliSubmitOut)) return kIOReturnBadArgument;
        E5MaliSubmitOut out;
        IOLockLock(d->stateLock());
        const IOReturn r = submit((const E5MaliSubmit *)args->structureInput, &out);
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelWaitAtom:
    case kSelQueryAtom: {
        if (args->structureInputSize != sizeof(E5MaliWaitAtom) ||
            args->structureOutputSize < sizeof(E5MaliWaitAtomOut)) return kIOReturnBadArgument;
        E5MaliWaitAtomOut out;
        const IOReturn r = waitAtom((const E5MaliWaitAtom *)args->structureInput, &out,
                                    selector == kSelWaitAtom);
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelSubmitMany: {
        if (!e5_mali_submit_many_size_supported(args->structureInputSize) ||
            args->structureOutputSize < sizeof(E5MaliSubmitOut)) return kIOReturnBadArgument;
        const E5MaliSubmitMany *in = (const E5MaliSubmitMany *)args->structureInput;
        if (!e5_mali_submit_many_count_valid(args->structureInputSize, in->handleCount))
            return kIOReturnBadArgument;
        E5MaliSubmitOut out = {};
        IOLockLock(d->stateLock());
        if (in->handleCount > 64) {
            static uint32_t reports;
            if (++reports <= 8)
                note("E5MALI SUBMIT MANY handles=%u packet=%u", in->handleCount,
                     (unsigned)args->structureInputSize);
        }
        const IOReturn r = submitHandles(in->jc, in->slot, in->udata,
                                         in->handles, in->handleCount, &out);
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return r;
    }
    case kSelSubmitWaitMany: {
        if (!e5_mali_submit_many_size_supported(args->structureInputSize) ||
            args->structureOutputSize < sizeof(E5MaliSubmitWaitOut)) return kIOReturnBadArgument;
        const auto *in = (const E5MaliSubmitMany *)args->structureInput;
        if (!e5_mali_submit_many_count_valid(args->structureInputSize, in->handleCount))
            return kIOReturnBadArgument;
        E5MaliSubmitOut submitted = {};
        IOLockLock(d->stateLock());
        IOReturn result = submitHandles(in->jc, in->slot, in->udata,
                                       in->handles, in->handleCount, &submitted);
        IOLockUnlock(d->stateLock());
        E5MaliSubmitWaitOut out = {};
        out.atomId = submitted.atomId;
        out.status = result;
        if (result == kIOReturnSuccess) {
            E5MaliWaitAtom request = {submitted.atomId, 500, 0};
            E5MaliWaitAtomOut completion = {};
            // The IRQ/workloop can complete the atom while this call waits.
            // The existing waiter owns cache publication, pin lifetime and
            // terminal retirement; timeout leaves active pins intact.
            result = waitAtom(&request, &completion, true);
            out.state = completion.state;
            out.hwStatus = completion.hwStatus;
            out.completionSeq = completion.completionSeq;
            out.status = result;
        }
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return result;
    }
    case kSelGpuProperties: {
        if (args->structureInputSize || args->structureOutputSize < sizeof(E5MaliGpuProperties))
            return kIOReturnBadArgument;
        // Read-only GPU feature registers, from the pinned vendor
        // gpu/mali_kbase_gpu_regmap.h. Return raw values, not guessed caps.
        E5MaliGpuProperties out = {};
        IOLockLock(d->stateLock());
        out.gpuId = d->gpuId();
        out.l2Features = d->read32(0x004);
        out.tilerFeatures = d->read32(0x00c);
        out.memFeatures = d->read32(0x010);
        out.mmuFeatures = d->mmuFeatures();
        out.threadMaxThreads = d->read32(0x0a0);
        out.threadMaxWorkgroup = d->read32(0x0a4);
        out.threadMaxBarrier = d->read32(0x0a8);
        out.threadFeatures = d->read32(0x0ac);
        out.threadTlsAlloc = d->read32(0x310);
        for (uint32_t i = 0; i < 4; ++i) out.textureFeatures[i] = d->read32(0x0b0 + 4 * i);
        out.shaderPresent = d->shaderPresent();
        out.l2Present = d->l2Present();
        out.tilerPresent = d->tilerPresent();
        out.vaBase = ctx_->va.base();
        out.vaBytes = ctx_->va.size();
        out.maxBoHandles = kMaxBoHandles;
        out.maxSubmitBos = E5_MALI_MAX_SUBMIT_BOS;
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnSuccess;
    }
    case kSelCacheRange: {
        if (args->structureInputSize != sizeof(E5MaliCacheRange)) return kIOReturnBadArgument;
        const E5MaliCacheRange *in = (const E5MaliCacheRange *)args->structureInput;
        if (!in->handle || in->handle > kMaxBoHandles || in->operation > E5_MALI_CACHE_INVALIDATE)
            return kIOReturnBadArgument;
        IOLockLock(d->stateLock());
        Bo &bo = bos_[in->handle - 1];
        IOReturn r = kIOReturnBadArgument;
        if (bo.descriptor() && !bo.imported && in->offset < bo.size) {
            const uint64_t bytes = in->length ? in->length : bo.size - in->offset;
            if (!(in->offset & 63u) && bytes && !(bytes & 63u) && bytes <= bo.size - in->offset) {
                r = kIOReturnBusy;
                if (!handlePinned(in->handle)) {
                    if (bo.userMd) {
                        r = E5UserCache::syncUserRange(bo.userMd, in->offset, bytes,
                            in->operation == E5_MALI_CACHE_CLEAN ? 1 : 2)
                            ? kIOReturnSuccess : kIOReturnNotPermitted;
                    } else {
                        uint8_t *cpu = (uint8_t *)bo.bytes();
                        r = kIOReturnNoMemory;
                        if (cpu) {
                            if (in->operation == E5_MALI_CACHE_CLEAN)
                                cacheCleanRange(cpu + in->offset, (uint32_t)bytes);
                            else
                                cacheInvalidateRange(cpu + in->offset, (uint32_t)bytes);
                            r = kIOReturnSuccess;
                        }
                    }
                }
            }
        }
        IOLockUnlock(d->stateLock());
        return r;
    }
    case kSelStats: {
        if (args->structureOutputSize < sizeof(E5MaliStats)) return kIOReturnNoSpace;
        E5MaliStats st = {};
        IOLockLock(d->stateLock());
        st.submitted = e5_mali_g1_submitted;
        st.completed = e5_mali_g1_completed;
        st.faults = e5_mali_g1_faults;
        st.timedOut = e5_mali_g1_timeouts;
        st.resets = e5_mali_g1_resets;
        st.mmuFaults = e5_mali_g1_mmu_faults;
        st.lastMmuFaultCode = e5_mali_g1_last_fault_code;
        st.lastMmuFaultAddr = e5_mali_g1_last_fault_addr;
        for (uint32_t i = 0; i < kMaxClientAtoms; ++i)
            if (atoms_[i] && (atoms_[i]->state == kAtomOnHw || atoms_[i]->state == kAtomQueued))
                ++st.activeAtoms;
        st.userCachePages = E5UserCache::activePages();
        st.mappedBytes = (uint32_t)ctx_->mappedBytes;
        st.liveBoBytes = (uint32_t)liveBoBytes_;
        IOLockUnlock(d->stateLock());
        memcpy(args->structureOutput, &st, sizeof(st));
        args->structureOutputSize = sizeof(st);
        return kIOReturnSuccess;
    }
    case kSelCleanBo: {
        if (args->structureInputSize != sizeof(E5MaliCleanBo)) return kIOReturnBadArgument;
        const uint32_t h = ((const E5MaliCleanBo *)args->structureInput)->handle;
        IOLockLock(d->stateLock());
        const bool valid = h != 0 && h <= kMaxBoHandles && bos_[h - 1].descriptor() &&
                           !bos_[h - 1].imported;
        void *bytes = valid ? bos_[h - 1].bytes() : nullptr;
        const uint32_t bytesLen = valid ? (uint32_t)bos_[h - 1].size : 0;
        const bool busy = valid && handlePinned(h);
        bool synced = false;
        if (bytes && !busy) {
            if (bos_[h - 1].userMd)
                synced = E5UserCache::syncUserRange(bos_[h - 1].userMd, 0, bytesLen, 1);
            else { cacheCleanRange(bytes, bytesLen); synced = true; }
        }
        IOLockUnlock(d->stateLock());
        return busy ? kIOReturnBusy : synced ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    case kSelSelfTestMmu: {
        if (args->structureOutputSize < sizeof(E5MaliSelfTestMmuOut)) return kIOReturnNoSpace;
        E5MaliSelfTestMmuOut out = {};
        // Software half: scratch table over fictional physical pages; the
        // table math never dereferences the encoded addresses. Heap, not
        // stack: the context (~28 KiB) exceeds the kernel stack.
        E5MaliG1Context *probe = new E5MaliG1Context;
        if (!probe) {
            out.status = kIOReturnNoMemory;
            memcpy(args->structureOutput, &out, sizeof(out));
            args->structureOutputSize = sizeof(out);
            return kIOReturnSuccess;
        }
        if (!probe->init(0x40000000, 64u << 20)) {
            probe->teardown();
            delete probe;
            out.status = kIOReturnNoMemory;
            memcpy(args->structureOutput, &out, sizeof(out));
            args->structureOutputSize = sizeof(out);
            return kIOReturnSuccess;
        }
        const uint64_t testVa = 0x40000000;
        E5MaliG1Mmu::MapFlags flags;
        for (uint64_t i = 0; i < 64; ++i) {
            if (!probe->table.map(testVa + i * E5MaliG1Mmu::kPageSize,
                                  0x82000000 + i * E5MaliG1Mmu::kPageSize,
                                  E5MaliG1Mmu::kPageSize, flags)) break;
            ++out.entriesMapped;
        }
        for (uint64_t i = 0; i < out.entriesMapped; ++i) {
            const E5MaliG1Mmu::PageTable::Lookup lu =
                probe->table.lookupEntry(testVa + i * E5MaliG1Mmu::kPageSize);
            if (lu.mapped && lu.phys == 0x82000000 + i * E5MaliG1Mmu::kPageSize)
                ++out.entriesVerified;
        }
        out.tablesAllocated = probe->tpCount;
        probe->table.unmap(testVa, out.entriesMapped * E5MaliG1Mmu::kPageSize);
        out.tablesFreed = probe->tpFreed;
        probe->teardown();
        delete probe;
        // Hardware half: read back the live AS registers.
        IOLockLock(d->stateLock());
        if (ctx_->as >= 0) {
            const uint32_t base = MMU_BASE + ((uint32_t)ctx_->as << MMU_AS_SHIFT);
            out.readbackTranstab = d->read64(base + AS_TRANSTAB_LO);
            out.readbackTranscfg = d->read64(base + AS_TRANSCFG_LO);
            out.readbackMemattr = d->read64(base + AS_MEMATTR_LO);
            out.asUsed = (uint32_t)ctx_->as;
        }
        IOLockUnlock(d->stateLock());
        out.status = (out.entriesVerified == out.entriesMapped &&
                      out.tablesFreed == out.tablesAllocated - 1)  // root survives
                          ? kIOReturnSuccess
                          : kIOReturnInternalError;
        memcpy(args->structureOutput, &out, sizeof(out));
        args->structureOutputSize = sizeof(out);
        return kIOReturnSuccess;
    }
    }
    return kIOReturnUnsupported;
}

// ---------------------------------------------------------------------------
// In-kernel self-test (board-test-plan §2/§3, kernel edition).
//
// Launched by E5MaliG1::scheduleSelftest() when the e5-mali-g1-selftest=N
// boot argument is present: bit0 = §2 ABI/MMU/BO sequence, bit1 = §3 minimal
// hardware atom (a real Submit through the slot engine; runs only when §2
// fully passed). All output lines are E5MALIS-prefixed; every step prints
// PASS or FAIL; a FAIL stops new submissions but keeps recording. Nothing
// here may panic: every path is the same validation the UserClient performs
// for a userspace caller, just with kernel-resident structures.
// ---------------------------------------------------------------------------

IOReturn E5MaliG1UserClient::selfCall(uint32_t sel, const void *in,
                                      uint32_t inLen, void *out, uint32_t outLen,
                                      uint32_t *outLenUsed) {
    IOExternalMethodArguments args;
    bzero(&args, sizeof(args));
    args.structureInput = in;
    args.structureInputSize = inLen;
    args.structureOutput = out;
    args.structureOutputSize = outLen;
    const IOReturn r = externalMethod(sel, &args, 0, 0, 0);
    if (outLenUsed) *outLenUsed = args.structureOutputSize;
    return r;
}

// v9 job encodings, mirroring the userspace harness (probes/v9_desc.h of
// .work/gpu-user-r310, transcribed from Mesa genxml v9) — R327 final
// corrections applied against v9.xml directly: Job Header w4 bit0 is
// RESERVED on v9 ("All jobs are 64-bit" comment, v9.xml:1430 — HakiOS's
// bit0=1 descends from older generations); Type[7:1], Index[31:16];
// w6-7 = Next. Write Value payload at +0x20: Address u64, Type u32
// (6 = imm32), Immediate u64.
static const uint32_t kV9JobTypeWriteValue = 2u;
static const uint32_t kV9WriteValueImm32 = 6u;
static const uint32_t kSelftestDataOff = 0x800; // mirrors probe DATA_OFF

static void selftestWriteValueJob(void *job64, uint64_t dstVa, uint64_t value,
                                  uint32_t index, uint64_t next) {
    bzero(job64, 64);
    uint32_t *w = (uint32_t *)job64;
    w[4] = ((kV9JobTypeWriteValue & 0x7fu) << 1) | ((index & 0xffffu) << 16);
    w[6] = (uint32_t)next;
    w[7] = (uint32_t)(next >> 32);
    w[8] = (uint32_t)dstVa;               // payload +0x20: Address
    w[9] = (uint32_t)(dstVa >> 32);
    w[10] = kV9WriteValueImm32;           // +0x28: Type = imm32
    w[12] = (uint32_t)value;              // +0x30: Immediate
    w[13] = (uint32_t)(value >> 32);
}

// ---- §4.1 offscreen-clear encoders (v9.xml-verified; see R324 audit) ----
// R324 audit against refs/v9.xml fixed three v9_desc.h/probe_clear.c bugs
// that produced the R323 DATA_INVALID_FAULT (JS_STATUS 0x58):
//   1. RT Writeback Format is enum "Color Format" -> R5G6B5 = 21
//      (v9.xml:993). v9_desc.h's 64 is outside the enum (max 31); 64<<3
//      collides with Block Format bits and leaves Writeback Format = 0
//      (RAW8) -> invalid RT for the writeback config.
//   2. Tiler Heap w0 = Type Buffer(9, v9.xml:63) | BufferType TilerHeap
//      (2, :69) | ChunkSize 2MiB (3, :1343) | Partitioning Dynamic (0,
//      :1347) = 0x329. v9_desc.h's 0x111 decoded as Sampler/Simple/256KiB.
//   3. The probe_clear layout overlapped TCTX (0x180..0x240) with the
//      polygon prologue (0x200..) and declared a 0x20000 heap whose Top ran
//      far past the 0x400 descriptor BO. New layout below keeps every
//      region disjoint and the whole heap inside one mapped BO.
static const uint32_t kV9JobTypeFragment = 9u;
static const uint32_t kV9ColorFormatR5G6B5 = 21u;    // v9.xml Color Format
static const uint32_t kV9HeapWord0 = 0x329u;         // Buffer|TilerHeap|2MiB|Dynamic
static const uint32_t kClearFbW = 320, kClearFbH = 480, kClearStride = 640;
static const uint32_t kClearFbBytes = kClearFbW * kClearFbH * 2;   // 307200
static const uint32_t kClearWord = 0xf800f800u;   // RGB565 red, packed x2
// R332-final: RT clear field takes the TILEBUFFER INTERNAL value
// (R5G6B5A0 fixed-point, pure red = 31<<5 = 0x3E0), NOT the memory word —
// pan_clear.c:16-69. The framebuffer memory after writeback is still
// 0xF800 per pixel (kClearWord for verification stays memory-domain).
static const uint32_t kClearInternal = 0x000003e0u;
// Descriptor BO layout (0x201000 total): head structures + 2MiB heap arena.
// heap descriptor + arena on their own pages. R325: the POLYGON LIST lives
// INSIDE the heap arena (Mesa panvk shape — refs/panvk_gpu_queue_kbase_mx.c
// :644-676 dumps the polygon list from the tiler heap BO, poly offset
// relative to heap base; R324's DATA_INVALID fault pointer was our
// desc-BO-resident polygon list at +0x240). Flat list for hierarchy 0:
// 0x200 bytes, zeroed, arena-relative +0x40.
static const uint32_t kOffFragJob = 0x000, kOffFbd = 0x080, kOffRt0 = 0x100,
                      kOffHeap = 0x140, kOffTctx = 0x180,
                      kHeapArena = 0x1000,
                      // R336: the heap descriptor's Chunk Size CLASS is
                      // 2 MiB (v9.xml:1357 DEFAULT, Mesa never overrides —
                      // its heap is 512MB). R335's 0x66000 heap is SMALLER
                      // than one chunk of the declared class: the tiler's
                      // chunk allocator may place header blocks past the
                      // heap top (board: cursor moved 0x1F80 from the top,
                      // list base = cursor+0x600, but the region reads zero
                      // — headers likely written beyond the BO). Fix: size
                      // the heap to one full 2 MiB chunk (matches the class
                      // exactly; hierarchy needs only 0x65240).
                      kHeapSize = 0x200000,              // one full 2 MiB chunk
                      kDescBytes = kHeapArena + kHeapSize;   // 0x201000

static void selftestFragmentJob(void *job64, unsigned tminx, unsigned tminy,
                                unsigned tmaxx, unsigned tmaxy,
                                uint64_t fbdTagged, unsigned index) {
    bzero(job64, 64);
    uint32_t *w = (uint32_t *)job64;
    w[4] = ((kV9JobTypeFragment & 0x7fu) << 1) | ((index & 0xffffu) << 16);
    w[8] = (tminx & 0xfffu) | ((tminy & 0xfffu) << 16);   // tile-unit bounds
    w[9] = (tmaxx & 0xfffu) | ((tmaxy & 0xfffu) << 16);
    w[10] = (uint32_t)fbdTagged;                          // +0x28: fb pointer
    w[11] = (uint32_t)(fbdTagged >> 32);
}

static void selftestFbdParams(void *fbd, unsigned w, unsigned h,
                              unsigned rtCount, uint32_t tileColorBytes,
                              uint64_t tilerCtxVa) {
    // v9 "Framebuffer Parameters" 128B aggregate. w11 final encoding
    // (R327 final, astra cross-check): Effective Tile Size field is
    // log2(tile area) — 16x16 = 256 = 2^8 (v9.xml:1109 modifier="log2";
    // pan_fb.c:69-70), Color Buffer Allocation is in 1024-byte units
    // (v9.xml:1116 shr(10) — our old (512>>10)=0 left the field EMPTY).
    // For 16x16 RGB565: w11 = 0x01009000 = eff_tile=8 | originMaxY(bit15)
    // | rt_count-1=0 | cbuf_alloc=1 (1024B, 1024-aligned).
    bzero(fbd, 128);
    uint32_t *p = (uint32_t *)fbd;
    p[8] = ((w - 1) & 0xffffu) | (((h - 1) & 0xffffu) << 16);
    p[9] = 0;                                             // bound min 0,0
    // R332-final: bound max is INCLUSIVE (astra; pan_job.c:476-477
    // `maxx = width - 1`): 320x480 -> (319, 479) = 0x01df013f. Our old
    // exclusive 320/480 was one past the framebuffer.
    p[10] = ((w - 1) & 0xffffu) | (((h - 1) & 0xffffu) << 16);
    const uint32_t log2TileArea = 8;                      // 16x16 = 256
    p[11] = (log2TileArea << 9)
          | (1u << 15)                                    // sprite origin maxY
          | (((rtCount - 1) & 0xfu) << 19)
          | ((((tileColorBytes + 1023) / 1024) & 0xffu) << 24);
    p[14] = (uint32_t)tilerCtxVa;
    p[15] = (uint32_t)(tilerCtxVa >> 32);
}

static void selftestRgbRt(void *rt64, uint64_t baseVa, uint32_t rowStride,
                          uint32_t surfaceStride, uint32_t clearInternal) {
    // R332-final (astra cross-check + Mesa-tree): writeback base is a RAW
    // address (pan_desc.c:840). The zero-write fix set + astra's three
    // byte corrections:
    //   w0[31:26] internal_format = R5G6B5A0 = 5 (common.xml CBI Format);
    //   w1[15]    dithering_enable = 1 (pan_fb.c:578);
    //   w1[27:16] swizzle = **0x80A = B,G,R,0** — the R5G6B5 MEMORY layout
    //             has blue in the low 5 bits, so the writeback swizzle is
    //             BGR0 not RGB1 (astra: u_format.yaml:247/pan_util.c:28;
    //             PAN_V6_SWIZZLE(B=2,G=1,R=0,0=4) = 2|1<<3|0<<6|4<<9);
    //   w12-15    clear colour = **tilebuffer INTERNAL value** (pan_clear.c
    //             :16-69: R5G6B5A0 layout {R:5+5, G:6+4, B:5+5, A:0+2},
    //             pure red (1,0,0,1) -> R=31<<5 = 0x3E0, all four words).
    //             The MEMORY word after writeback is still 0xF800.
    bzero(rt64, 64);
    uint32_t *w = (uint32_t *)rt64;
    w[0] = 5u << 26;                  // internal format R5G6B5A0
    w[1] = (1u << 0)                  // write enable
         | (kV9ColorFormatR5G6B5 << 3)
         | (2u << 8)                  // block format LINEAR
         | (1u << 15)                 // dithering enable
         | (0x80au << 16)             // swizzle B,G,R,0 (memory order)
         | (1u << 31);                // clean tile write
    w[8] = (uint32_t)baseVa;
    w[9] = (uint32_t)(baseVa >> 32);
    w[10] = rowStride;
    w[11] = surfaceStride;
    w[12] = clearInternal;
    w[13] = clearInternal;
    w[14] = clearInternal;
    w[15] = clearInternal;
}

static void selftestTilerHeap(void *heap32, uint64_t base, uint64_t size) {
    // v9.xml "Tiler Heap" (1354-1363): w0 Type=Buffer(9)|BT=TilerHeap(2)|
    // Chunk=2MiB(3)|Partition=Dynamic(0) = 0x329; w1 Size (4096-aligned);
    // w2-3 Base; w4-5 Bottom; w6-7 Top.
    bzero(heap32, 32);
    uint32_t *w = (uint32_t *)heap32;
    w[0] = kV9HeapWord0;
    w[1] = (uint32_t)size;
    w[2] = (uint32_t)base;         w[3] = (uint32_t)(base >> 32);
    w[4] = (uint32_t)base;         w[5] = (uint32_t)(base >> 32);
    w[6] = (uint32_t)(base + size);
    w[7] = (uint32_t)((base + size) >> 32);
}

// v9 "Tiler Context" 192B. R327 (full Mesa tree, authoritative):
// panvk NEVER packs polygon_list on v9 (jm/panvk_vX_cmd_buffer.c:339-350
// sets only hierarchy_mask/fb_*/heap/sample_pattern/first_provoking) —
// the TILER HARDWARE writes the list pointer into w0-1 when the MVJ runs
// (MX gpu_queue dumps tc[0..1] AFTER the tiler atom and masks its tag
// bits). w0 must therefore be ZERO on submission. Hierarchy mask per
// lib/pan_desc.c GENX(pan_select_tiler_hierarchy_mask) :1747-1810:
// 320x480, max_levels=8 (G57 tiler_features default), 16x16 tiles →
// mask=BITFIELD_MASK(8)=0xFF (bins_ptr_size 6528B, budget loop no-op).
static void selftestTilerCtx(void *ctx192, uint32_t hier,
                             unsigned fbW, unsigned fbH, uint64_t heapVa) {
    bzero(ctx192, 192);
    uint32_t *w = (uint32_t *)ctx192;
    w[2] = hier;
    w[3] = ((fbW - 1) & 0xffffu) | (((fbH - 1) & 0xffffu) << 16);
    w[6] = (uint32_t)heapVa;       w[7] = (uint32_t)(heapVa >> 32);
}

// FNV-1a 64 (hash-golden.md algorithm; vectors "" -> cbf29ce484222325 and
// "foobar" -> 85944171f73967e8 verified; the doc's "a" vector
// af63dc6c8601ec01 matches NEITHER FNV-1a nor FNV-1 and is wrong — the
// true FNV-1a("a") is af63dc4c8601ec8c).
static uint64_t fnv1a64Update(uint64_t h, const void *data, uint32_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
static uint64_t fnv1a64(const void *data, uint32_t bytes) {
    return fnv1a64Update(0xcbf29ce484222325ull, data, bytes);
}
static uint64_t fnv1a64WordPattern(uint64_t h, uint32_t word, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) h = fnv1a64Update(h, &word, 4);
    return h;
}

// ---- §4.2 triangle encoders (v9.xml-verified layout; field citations
// inline). Descriptor BO shares the §4.1 arena scheme. ---------------------
static const uint32_t kTriOffVsBin = 0x000, kTriOffFsConstBin = 0x080,
                      kTriOffFsTriBin = 0x100, kTriOffVsFau = 0x200,
                      kTriOffFsFau = 0x220, kTriOffVsProg = 0x240,
                      kTriOffFsProg = 0x260, kTriOffResTable = 0x280,
                      kTriOffPosBuf = 0x380, kTriOffMvj = 0x400,
                      kTriOffFragJob = 0x580, kTriOffBlend = 0x5c0,
                      kTriOffFbd = 0x600, kTriOffRt0 = 0x680,
                      kTriOffHeap = 0x6c0, kTriOffTctx = 0x700,
                      kTriOffTls = 0x780,     // Local Storage desc (R327)
                      kTriOffZsd = 0x7c0,     // Depth/Stencil desc (R327)
                      kTriOffFsRes = 0x800;   // empty fs resource table
static const uint32_t kTriDescBytes = kHeapArena + kHeapSize;   // 0x201000
static const uint32_t kTriResEntries = 4;      // Mesa ALIGN_POT(1+0, 4)
                                               // (panvk desc_state :267)
static const uint32_t kTriFullWord = 0xd942d942u;   // RGB565 (219,42,18) x2
static const uint16_t kTriInside565 = 0xffe0u;      // yellow

// v9.xml "Shader Environment" (16 words, :1091-ish layout per struct):
// w0 attribute offset, w1 FAU count[7:0], w8-9 Resources — PACKED as
// table-address|entry-count (astra/panvk: the count rides the low bits of
// the pointer; 16 entries => |16, table 64B-aligned so the bits are free),
// w10-11 Shader (prog desc VA), w12-13 thread storage, w14-15 FAU VA.
static void selftestSe(void *se64, uint32_t fauCount, uint64_t resVa,
                       uint64_t progVa, uint64_t fauVa, uint64_t tlsVa) {
    bzero(se64, 64);
    uint32_t *w = (uint32_t *)se64;
    w[1] = fauCount & 0xffu;
    w[8] = (uint32_t)resVa;   w[9] = (uint32_t)(resVa >> 32);
    w[10] = (uint32_t)progVa; w[11] = (uint32_t)(progVa >> 32);
    w[12] = (uint32_t)tlsVa;  w[13] = (uint32_t)(tlsVa >> 32);
    w[14] = (uint32_t)fauVa;  w[15] = (uint32_t)(fauVa >> 32);
}
static inline uint64_t selftestSeResources(uint64_t tableVa, uint32_t entries) {
    // SE Resources = table pointer | entry count (count in the low bits
    // freed by the 64B table alignment; entries 4-aligned).
    return tableVa | (entries & 0x3fu);
}

// v9.xml "Blend" (:16 words? 4 words/16B): w0 Enable(bit9)=1; w1 equation
// with Color Mask[31:28]=0xF (no blend functions); w2 Internal Blend
// Mode[1:0]=Opaque(1) + Fixed-Function NumComps[4:3] (default 1 -> 0) +
// RT[19:16]=0; w3 Internal Conversion Memory Format = Pixel Format enum
// R5G6B5=4 (v9.xml:755). UNVERIFIED-on-hw: exact opaque encoding — the
// DUMP line carries the words for triage.
static void selftestBlendOpaque(void *blend16) {
    // R331 (Mesa-tree authoritative, panvk_vX_blend.c:168-192 +
    // pan_format.c:30-100 + pan_format.h:355-363): the internal conversion
    // for R5G6B5 is the DITHERED FORMAT table value, not a raw enum pick —
    // MALI_BLEND_PU_R5G6B5A0 = (R5G6B5_PU=227 << 12) | RGB1(16) = 0xE3010
    // (v9.xml blendable-format enum :291-292 + Component Order :311-323).
    // num_comps MUST be 4 for the conversion to work (panvk comment :174),
    // packed minus(1) => field 3. Mode Opaque(1), RT 0.
    bzero(blend16, 16);
    uint32_t *w = (uint32_t *)blend16;
    w[0] = 1u << 9;              // Enable
    w[1] = 0xfu << 28;           // color mask RGBA, no equations
    w[2] = 1u | (3u << 3);       // Mode=Opaque, num_comps=4 (field 3), RT=0
    w[3] = (227u << 12) | 16u;   // conversion: R5G6B5 PU | RGB1 order
}

// v9.xml "Malloc Vertex Job" (384B aggregate, :1641-1653). Fixed sections;
// Draw/Position/Varying SEs are filled by the caller at +0x80/+0x100/+0x140.
static void selftestMallocVertexJob(void *job384, uint64_t tctxVa) {
    bzero(job384, 384);
    uint32_t *w = (uint32_t *)job384;
    // Header: Type=11 Malloc vertex, index 1, 64-bit descriptor, Next=0.
    w[4] = ((11u & 0x7fu) << 1) | (1u << 16);
    // Primitive (+0x20 = word 8): Draw mode Triangles=8 (v9.xml "Draw Mode"
    // enum :8 — NOTE the SE-DCD-FILL.md sheet's "3" is the Lines-adjacency
    // value, wrong), Allow rotating (bit15), low/high depth cull (bits 16/17).
    w[8] = 8u | (1u << 15) | (1u << 16) | (1u << 17);
    w[11] = 3;                   // +0x2c: index count (word 11 = +0x2c)
    // Instance count (+0x30 = word 12) = 1.
    w[12] = 1;
    // Allocation (+0x34 = word 13): vertex packet stride 16, attr stride 0.
    w[13] = 16;
    // Tiler pointer (+0x38 = words 14-15).
    w[14] = (uint32_t)tctxVa;
    w[15] = (uint32_t)(tctxVa >> 32);
    // Scissor (+0x68 = words 26-27): full 320x480.
    w[26] = 0 | (0u << 16);
    w[27] = kClearFbW | (kClearFbH << 16);
    // Primitive Size (+0x70 = word 28): fixed 1.0f = 0x3F800000.
    w[28] = 0x3f800000u;
    // Indices (+0x78 = words 30-31): 0 (Index type NONE -> verts 0..2).
}

// v9.xml "Draw" (DCD, :128 words): w0 Flags0, w1 Flags1, w2-4 Vertex Array
// (Packet bit0), w6/w7 min/max Z, w10-11 depth/stencil (0, UNVERIFIED),
// w12 blend count[3:0] + Blend address>>4, w14-15 occlusion 0, w16+ = the
// fragment Shader Environment.
static void selftestDcd(void *dcd128, uint32_t flags0, uint64_t blendVa,
                        uint64_t zsdVa) {
    bzero(dcd128, 128);
    uint32_t *w = (uint32_t *)dcd128;
    w[0] = flags0;
    w[1] = 0xffffu | (0xffu << 16);   // sample mask, RT mask
    w[2] = 1;                         // Vertex Array: Packet
    w[6] = 0;                         // minimum Z 0.0f
    w[7] = 0x3f800000u;               // maximum Z 1.0f
    w[10] = (uint32_t)zsdVa;          // R327: depth/stencil desc (Mesa
    w[11] = (uint32_t)(zsdVa >> 32);  // allocates it unconditionally)
    w[12] = 1u | ((uint32_t)(blendVa >> 4) << 4);   // blend count 1 + ptr
}

// Point-in-triangle on doubled coordinates (integer only — the kernel must
// not use FP). Mirrors probes/probe_triangle.c pt_in_tri with the pixel
// centre at (2x+1, 2y+1); vertices doubled: (160,40),(60,420),(280,380).
static bool selftestPtInTri(int px2, int py2) {
    const int ax = 320, ay = 80, bx = 120, by = 840, cx = 560, cy = 760;
    const int d1 = (px2 - bx) * (ay - by) - (ax - bx) * (py2 - by);
    const int d2 = (px2 - cx) * (by - cy) - (bx - cx) * (py2 - cy);
    const int d3 = (px2 - ax) * (cy - ay) - (cx - ax) * (py2 - ay);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

static bool selftestStep(bool ok, const char *name, const char *detail) {
    note("E5MALIS %s %s %s", name, ok ? "PASS" : "FAIL", detail ? detail : "");
    return ok;
}

void E5MaliG1UserClient::runSelftest(uint32_t mask) {
    note("E5MALIS start mask=%u (bit0=seq2 bit1=seq3) as=%d",
         mask, ctx_ ? ctx_->as : -1);
    bool seq2 = (mask & 1u) != 0;
    bool wantSeq3 = (mask & 2u) != 0;
    bool pass2 = true;
    bool tri4Pass = false, clear4Pass = false;

    // ---- §2.1 GetVersion: identity hard gate (R313 values) ---------------
    if (seq2) {
        E5MaliInfo info;
        bzero(&info, sizeof(info));
        uint32_t used = 0;
        const IOReturn rc = selfCall(kSelGetVersion, nullptr, 0, &info,
                                     sizeof(info), &used);
        char d[160];
        snprintf(d, sizeof(d), "abi=%u.%u gpuId=%08x shader=%llx mmu=%08x as=%x js=%x",
                 info.abiMajor, info.abiMinor, info.gpuIdRaw,
                 (unsigned long long)info.shaderPresent, info.mmuFeatures,
                 info.asPresent, info.jsPresent);
        const bool ok = selftestStep(
            rc == kIOReturnSuccess && info.abiMajor == kE5MaliAbiMajor &&
            info.abiMinor == kE5MaliAbiMinor && info.gpuIdRaw == 0x90910010u &&
            info.shaderPresent == 0x5ull, "VERSION", d);
        pass2 = pass2 && ok;
    }

    // ---- §2.2 SelfTestMmu ---------------------------------------------------
    if (seq2) {
        E5MaliSelfTestMmuOut o;
        bzero(&o, sizeof(o));
        uint32_t used = 0;
        const IOReturn rc = selfCall(kSelSelfTestMmu, nullptr, 0, &o,
                                     sizeof(o), &used);
        char d[192];
        snprintf(d, sizeof(d),
                 "st=%x map=%u ver=%u tbl=%u/%u tt=%llx cfg=%llx attr=%llx",
                 o.status, o.entriesMapped, o.entriesVerified,
                 o.tablesAllocated, o.tablesFreed,
                 (unsigned long long)o.readbackTranstab,
                 (unsigned long long)o.readbackTranscfg,
                 (unsigned long long)o.readbackMemattr);
        const bool ok = selftestStep(
            rc == kIOReturnSuccess && o.status == kIOReturnSuccess &&
            o.entriesMapped == 64 && o.entriesVerified == 64 &&
            o.tablesAllocated == o.tablesFreed + 1 &&
            (o.readbackTranstab & 0xfffull) == 0 &&
            (o.readbackTranscfg & 0xfull) == 6 &&
            o.readbackMemattr == kAsMemattr, "SELFTESTMMU", d);
        pass2 = pass2 && ok;
    }

    // ---- §2.3 BO lifecycle --------------------------------------------------
    uint32_t boHandle = 0;
    if (seq2) {
        E5MaliCreateBo cb = {};
        cb.size = 4096;
        E5MaliCreateBoOut cbo;
        uint32_t used = 0;
        const IOReturn rc = selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo,
                                     sizeof(cbo), &used);
        const bool ok = selftestStep(rc == kIOReturnSuccess &&
                                     cbo.status == kIOReturnSuccess &&
                                     cbo.handle != 0, "CREATEBO",
                                     rc == kIOReturnSuccess ? "handle set" : "rc nonzero");
        pass2 = pass2 && ok;
        boHandle = ok ? cbo.handle : 0;
    }
    uint64_t va1 = 0, va2 = 0;
    if (pass2 && boHandle) {
        E5MaliMapBo mb = {};
        mb.handle = boHandle;          // default flags: executable, driver VA
        E5MaliMapBoOut m1, m2;
        uint32_t used = 0;
        const IOReturn r1 = selfCall(kSelMapBo, &mb, sizeof(mb), &m1,
                                     sizeof(m1), &used);
        used = 0;
        const IOReturn r2 = selfCall(kSelMapBo, &mb, sizeof(mb), &m2,
                                     sizeof(m2), &used);
        char d[128];
        snprintf(d, sizeof(d), "va1=%llx va2=%llx",
                 (unsigned long long)m1.va, (unsigned long long)m2.va);
        const bool ok = selftestStep(r1 == kIOReturnSuccess &&
                                     m1.status == kIOReturnSuccess &&
                                     r2 == kIOReturnSuccess &&
                                     m2.status == kIOReturnSuccess &&
                                     m1.va != 0 && m2.va != 0 && m1.va != m2.va,
                                     "MAPBO-ALIAS", d);
        pass2 = pass2 && ok;
        va1 = m1.va;
        va2 = m2.va;
    } else if (seq2) {
        pass2 = selftestStep(false, "MAPBO-ALIAS", "no BO handle");
    }
    if (pass2 && boHandle) {
        E5MaliCleanBo cl = {};
        cl.handle = boHandle;
        const IOReturn rc = selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);
        pass2 = selftestStep(rc == kIOReturnSuccess, "CLEANBO", "dc cvac to PoC") && pass2;
    }
    if (pass2 && boHandle) {
        E5MaliUnmapBo um = {};
        um.handle = boHandle;
        um.va = va1;
        um.length = 4096;
        const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
        um.va = va2;
        const IOReturn r2 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
        pass2 = selftestStep(r1 == kIOReturnSuccess && r2 == kIOReturnSuccess,
                             "UNMAPBO-X2", "both aliases") && pass2;
    }
    if (seq2 && boHandle && pass2) {
        E5MaliDestroyBo db = {};
        db.handle = boHandle;
        const IOReturn rc = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
        pass2 = selftestStep(rc == kIOReturnSuccess, "DESTROYBO", "post-unmap") && pass2;
    } else if (seq2) {
        note("E5MALIS DESTROYBO SKIPPED (earlier failure)");
    }

    // ---- §2.4 Stats: this sequence never submitted --------------------------
    if (seq2) {
        E5MaliStats st;
        uint32_t used = 0;
        const IOReturn rc = selfCall(kSelStats, nullptr, 0, &st, sizeof(st), &used);
        char d[160];
        snprintf(d, sizeof(d),
                 "sub=%llu comp=%llu fault=%llu tmo=%llu reset=%llu mmu=%llu",
                 (unsigned long long)st.submitted, (unsigned long long)st.completed,
                 (unsigned long long)st.faults, (unsigned long long)st.timedOut,
                 (unsigned long long)st.resets, (unsigned long long)st.mmuFaults);
        pass2 = selftestStep(rc == kIOReturnSuccess && st.submitted == 0 &&
                             st.completed == 0 && st.faults == 0,
                             "STATS-ZERO", d) && pass2;
    }
    note("E5MALIS SEQ2 VERDICT %s", seq2 ? (pass2 ? "PASS" : "FAIL") : "SKIPPED");

    // ---- §3 minimal hardware atom (only after §2 fully passed) -------------
    bool seq3Pass = false;
    do {
    if (!wantSeq3) {
        note("E5MALIS SEQ3 SKIPPED (mask bit1 not set)");
        break;
    }
    if (!pass2) {
        note("E5MALIS SEQ3 SKIPPED (seq2 FAIL — no atoms submitted after failure)");
        break;
    }
    if (!e5_mali_g1_ready) {
        note("E5MALIS SEQ3 SKIPPED (driver no longer ready)");
        break;
    }

    // One WRITE_VALUE job at BO offset 0 (page aligned = 64B aligned chain,
    // jc inside the BO as required), writing imm32 value 2 to BO+0x800.
    E5MaliCreateBo cb = {};
    cb.size = 4096;
    E5MaliCreateBoOut cbo;
    uint32_t used = 0;
    if (selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo, sizeof(cbo), &used) != kIOReturnSuccess ||
        cbo.status != kIOReturnSuccess || cbo.handle == 0) {
        note("E5MALIS SEQ3 FAIL CreateBo rc/status");
        return;
    }
    const uint32_t h = cbo.handle;
    E5MaliMapBo mb = {};
    mb.handle = h;                    // executable mapping for the chain
    E5MaliMapBoOut mbo;
    used = 0;
    if (selfCall(kSelMapBo, &mb, sizeof(mb), &mbo, sizeof(mbo), &used) != kIOReturnSuccess ||
        mbo.status != kIOReturnSuccess) {
        note("E5MALIS SEQ3 FAIL MapBo");
        E5MaliDestroyBo db = {}; db.handle = h;
        selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
        return;
    }
    const uint64_t chainVa = mbo.va;
    // Kernel-side CPU view of the BO (in a userspace client this is
    // IOConnectMapMemory; here the buffer is already resident).
    void *cpu = bos_[h - 1].md ? bos_[h - 1].md->getBytesNoCopy() : nullptr;
    if (!cpu) {
        note("E5MALIS SEQ3 FAIL no CPU view of BO");
        return;
    }
    volatile uint32_t *dataWord = (volatile uint32_t *)((uint8_t *)cpu + kSelftestDataOff);
    *dataWord = 0;                    // reference pre-state
    // R321 write-provenance experiment: dump the 16 bytes around the data
    // word BEFORE submit and AFTER the wait. PRE must be all zero; POST
    // +0x800==2 proves the GPU performed the write regardless of the slot
    // DONE bookkeeping (HakiOS's wait_for() return was cast to void — its
    // pass criterion was always the value, so a bare write-value job
    // completing without a slot IRQ was never excluded).
    const volatile uint32_t *win = (const volatile uint32_t *)((uint8_t *)cpu + 0x7f8);
    // Chain: a SINGLE write-value job, Next=0 — the HakiOS-proven minimal
    // form on real hardware (refs/hakios_mali.c:360-375): w4 bit0=1
    // (64-bit descriptor). Note: HakiOS's wait_for(RAWSTAT) return value is
    // CAST TO VOID — its pass criterion was always the written value, so
    // "bare write-value job raises slot DONE" was never actually proven
    // there either (R320 finding). R317 (bit0=0): write executed, no DONE.
    // R318 (Null-job terminator): slot wedged. R319/R320 (single job,
    // bit0=1, slot 1/2): write executes, TAIL/HEAD consumed, st=00 —
    // no DONE. R321 adds the PRE/POST provenance dump.
    selftestWriteValueJob(cpu, chainVa + kSelftestDataOff, 2 /* value */,
                          1 /* index */, 0 /* next: end of chain */);
    E5MaliCleanBo cl = {};
    cl.handle = h;
    selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);
    // PRE snapshot AFTER the clean: invalidate the window so the read
    // observes DRAM (the state the GPU will see), not the CPU cache.
    cacheInvalidateRange((void *)((uintptr_t)win & ~(uintptr_t)63), 64);
    note("E5MALIS BO-PRE  +7f8: %08x %08x %08x %08x", win[0], win[1], win[2], win[3]);

    E5MaliSubmit sub = {};
    sub.jc = chainVa;
    sub.slot = (uint8_t)gSelftestSlot;  // e5-mali-g1-selftest-slot=N (R320)
    sub.boHandles[0] = h;               // pin for the atom lifetime
    E5MaliSubmitOut so;
    bzero(&so, sizeof(so));
    used = 0;
    const IOReturn src = selfCall(kSelSubmit, &sub, sizeof(sub), &so,
                                  sizeof(so), &used);
    note("E5MALIS SUBMIT %s atom=%llu jc=%llx slot=%u rc=%x",
         (src == kIOReturnSuccess && so.status == kIOReturnSuccess) ? "PASS" : "FAIL",
         (unsigned long long)so.atomId, (unsigned long long)chainVa,
         sub.slot, (unsigned)src);
    if (src != kIOReturnSuccess || so.status != kIOReturnSuccess || so.atomId == 0) {
        note("E5MALIS SEQ3 VERDICT FAIL (submit refused — no retry)");
        goto teardown;
    }

    {
        // R322 completion semantics (R321 finding: bare write-value jobs are
        // executed by the JM internally and never raise the slot DONE
        // interrupt). One 500 ms budget at 1 ms cadence, three signals:
        //   (a) slot DONE/ERR latched in JOB_INT_RAWSTAT (real IRQ path —
        //       also probed by every WaitAtom/QueryAtom call);
        //   (b) consumption: headN==0 && cmdN==0 with no JS_STATUS exception
        //       — NOTE: NEXT clears at ACCEPTANCE, so (b) alone cannot
        //       distinguish "executing" from "retired";
        //   (c) data predicate: BO+DATA_OFF reads the expected value through
        //       an invalidated view (the GPU wrote it).
        // (a) or (c) completes; the jm-internal record line fires when (b)
        // and (c) hold in the same frame.
        E5MaliWaitAtomOut wo;
        bzero(&wo, sizeof(wo));
        IOReturn wrc = kIOReturnSuccess;
        bool jmInternal = false;
        for (uint32_t ms = 0; ms < 500; ++ms) {
            E5MaliWaitAtom q = {};
            q.atomId = so.atomId;
            q.timeoutMs = 0;
            E5MaliWaitAtomOut wq;
            bzero(&wq, sizeof(wq));
            used = 0;
            wrc = selfCall(kSelQueryAtom, &q, sizeof(q), &wq, sizeof(wq), &used);
            // (a) already probed inside waitAtom: a terminal state here
            // (IRQ-path done/fault) ends the loop early.
            if (wq.state != kAtomQueued && wq.state != kAtomOnHw) break;
            // (c): data predicate through an invalidated view.
            cacheInvalidateRange((void *)((uintptr_t)dataWord & ~(uintptr_t)63), 64);
            if (*dataWord == 2) {
                // Confirm with (b) in the same frame, then complete.
                IOLockLock(driver_->stateLock());
                const uint32_t base = JS_BASE + sub.slot * JS_SLOT_STRIDE;
                const uint32_t headN = driver_->read32(base + JS_HEAD_NEXT_LO);
                const uint32_t headNh = driver_->read32(base + JS_HEAD_NEXT_HI);
                const uint32_t cmdN = driver_->read32(base + JS_COMMAND_NEXT);
                const uint32_t st = driver_->read32(base + JS_STATUS);
                bool completed = false;
                for (uint32_t i = 0; i < kMaxClientAtoms; ++i) {
                    E5MaliG1Atom *a = atoms_[i];
                    if (!a || a->id != so.atomId) continue;
                    if (a->state == kAtomOnHw || a->state == kAtomQueued) {
                        driver_->completeAtom(a, kAtomDone,
                                              0x00 /* hw: jm-internal */);
                        completed = true;
                    }
                    break;
                }
                IOLockUnlock(driver_->stateLock());
                if (completed)
                    note("E5MALI1 jm-internal done atom=%llu slot=%u (headN=%08x:%08x cmdN=%08x st=%02x)",
                         (unsigned long long)so.atomId, sub.slot, headNh, headN,
                         cmdN, st);
                jmInternal = completed;
                break;
            }
            if (ms >= 499) break;
            IOSleep(1);
        }
        // Final query: picks up whatever terminal state resulted ((a) via
        // the IRQ path or the jm-internal completion above).
        {
            E5MaliWaitAtom w = {};
            w.atomId = so.atomId;
            w.timeoutMs = 0;
            used = 0;
            wrc = selfCall(kSelWaitAtom, &w, sizeof(w), &wo, sizeof(wo), &used);
        }
        // R331 race closure: with real IRQs the atom can be completed AND
        // retired (by an in-flight waitAtom/query from the poll loop)
        // before this final query — NotFound then does NOT mean failure.
        // If the driver completed an atom since our snapshot, count it.
        bool lateNotFound = false;
        if (wrc != kIOReturnSuccess || wo.state != kAtomDone) {
            E5MaliStats stx;
            bzero(&stx, sizeof(stx));
            uint32_t ux = 0;
            selfCall(kSelStats, nullptr, 0, &stx, sizeof(stx), &ux);
            if (stx.completed >= 1) lateNotFound = true;
        }
        if (lateNotFound) {
            wrc = kIOReturnSuccess;
            wo.state = kAtomDone;
            if (wo.hwStatus != 0x01) wo.hwStatus = 0x00;
            note("E5MALIS late-complete NotFound atom=%llu (IRQ retired it first)",
                 (unsigned long long)so.atomId);
        }
        const bool doneOk = wrc == kIOReturnSuccess && wo.state == kAtomDone &&
                            (wo.hwStatus == 0x01 || wo.hwStatus == 0x00);
        note("E5MALIS WAITATOM %s state=%u hw=%02x(%s) rc=%x seq=%llu",
             doneOk ? "PASS" : "FAIL", wo.state, wo.hwStatus,
             jmInternal ? "jm-internal"
                        : (wo.hwStatus == 0x01 ? "slot-done" : "none"),
             (unsigned)wrc, (unsigned long long)wo.completionSeq);
        // GPU writes land in DRAM; drop the CPU's cached view of the data
        // line (clean since CleanBo ran) before reading it back.
        cacheInvalidateRange((void *)((uintptr_t)dataWord & ~(uintptr_t)63), 64);
        note("E5MALIS BO-POST +7f8: %08x %08x %08x %08x", win[0], win[1], win[2], win[3]);
        const uint32_t readback = *dataWord;
        const bool valueOk = readback == 2;
        note("E5MALIS READBACK %s got=%08x want=00000002", valueOk ? "PASS" : "FAIL",
             readback);
        if (!doneOk) {
            // Full failure context (AS_FAULTSTATUS decode: code/access/
            // source per kbase regmap; the E5MALI1 mmu/js lines on serial
            // carry the same event from IRQ context).
            E5MaliStats st;
            bzero(&st, sizeof(st));
            uint32_t u2 = 0;
            selfCall(kSelStats, nullptr, 0, &st, sizeof(st), &u2);
            const uint64_t fs = st.lastMmuFaultCode;
            static const char *const kAcc[4] = {"atomic", "execute", "read", "write"};
            note("E5MALIS FAULT state=%u hw=%02x mmuCode=%02x(%s) access=%s source=%04x mmuAddr=%llx resets=%llu",
                 wo.state, wo.hwStatus, (uint32_t)(fs & 0xff),
                 exceptionName((uint32_t)(fs & 0xff)), kAcc[(fs >> 8) & 3],
                 (uint32_t)((fs >> 16) & 0xffff),
                 (unsigned long long)st.lastMmuFaultAddr,
                 (unsigned long long)st.resets);
        }
        E5MaliStats st;
        bzero(&st, sizeof(st));
        uint32_t u3 = 0;
        selfCall(kSelStats, nullptr, 0, &st, sizeof(st), &u3);
        const bool statsOk = st.submitted == 1 && st.completed == 1 && st.faults == 0 &&
                             st.timedOut == 0 && st.resets == 0 && st.mmuFaults == 0;
        note("E5MALIS SEQ3STATS %s sub=%llu comp=%llu fault=%llu tmo=%llu reset=%llu mmu=%llu",
             statsOk ? "PASS" : "FAIL",
             (unsigned long long)st.submitted, (unsigned long long)st.completed,
             (unsigned long long)st.faults, (unsigned long long)st.timedOut,
             (unsigned long long)st.resets, (unsigned long long)st.mmuFaults);
        note("E5MALIS SEQ3 VERDICT %s",
             (doneOk && valueOk && statsOk) ? "PASS" : "FAIL");
        seq3Pass = doneOk && valueOk && statsOk;
    }

teardown:
    // Post-retire teardown per ABI §6 (no Busy expected).
    {
        E5MaliUnmapBo um = {};
        um.handle = h;
        um.va = chainVa;
        um.length = 4096;
        const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
        E5MaliDestroyBo db = {};
        db.handle = h;
        const IOReturn r2 = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
        note("E5MALIS TEARDOWN %s unmap=%x destroy=%x",
             (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? "PASS" : "FAIL",
             (unsigned)r1, (unsigned)r2);
    }
    } while (false);

    // ---- §4.2 triangle tier-1 (mask bit3) ------------------------------------
    // Two consecutive draw variants through the full IDVS/tiler/fragment
    // pipeline with the offline-compiled shader assets:
    //   (i)  vs + fs_const  — fullscreen constant RGB(219,42,18) -> 0xD942;
    //        golden = exact full-frame pattern + FNV-1a64.
    //   (ii) vs + fs_tri    — analytic triangle ((160,40),(60,420),(280,380)),
    //        yellow inside, clear colour outside; golden = integer
    //        point-in-triangle at pixel centres (top-left-class rule),
    //        exact pixel/FNV equality (R339). (i) FAIL => (ii) SKIP.
    if (mask & 8u) {
        if (!e5_mali_g1_ready) {
            note("E5MALIS TRI4 SKIPPED (driver no longer ready)");
        } else if (driver_->resetPending() && gGpuIntsDead) {
            note("E5MALIS TRI4 SKIPPED (engine parked after reset storm)");
        } else {
            // R326b drain; R329: when the GPU block is already dead
            // (storm-killed) do NOT attempt a recovery reset — the wedged
            // engine's reset either hangs the wait or re-fires IRQs that
            // restart the climb (R328: recovery resets #4/#5 kept coming).
            if (!gGpuIntsDead) {
                for (uint32_t ms = 0; ms < 500 && driver_->resetPending(); ++ms)
                    IOSleep(1);
                if (driver_->resetPending()) {
                    driver_->resetPending_ = 1;
                    driver_->resetEngine("tri recovery");
                }
            }
            if (driver_->resetPending() || gGpuIntsDead) {
                note("E5MALIS TRI4 SKIPPED (reset would not clear; engine %s)",
                     gGpuIntsDead ? "parked" : "busy");
            } else {
                tri4Pass = runTri4();   // §4.2 two-draw sequence
            }
        }
    }

    // ---- §4.1 offscreen RGB565 clear (mask bit2) ----------------------------
    // Layout mirrors probes/probe_clear.c byte-for-byte (see the encoders
    // above); completion reuses the R322 three-signal framework. A real
    // fragment job is expected to raise the slot DONE interrupt (hw=01);
    // the data predicate (full-frame clear word + FNV-1a64 vs the CPU
    // reference) is the authoritative check either way.
    if (mask & 4u) {
        if ((mask & 2u) && !seq3Pass) {
            note("E5MALIS CLEAR4 SKIPPED (seq3 FAIL — no further atoms after failure)");
        } else if (!e5_mali_g1_ready) {
            note("E5MALIS CLEAR4 SKIPPED (driver no longer ready)");
        } else if (driver_->resetPending() && gGpuIntsDead) {
            note("E5MALIS CLEAR4 SKIPPED (engine parked after reset storm)");
        } else {
            // R326b: same drain/recovery gate as TRI4 — a pending reset
            // from an earlier stage must not fail the clear with NotReady.
            // R329: same parked guard as TRI4 — no recovery reset on a
            // storm-dead engine.
            if (!gGpuIntsDead) {
                for (uint32_t ms = 0; ms < 500 && driver_->resetPending(); ++ms)
                    IOSleep(1);
                if (driver_->resetPending()) {
                    driver_->resetPending_ = 1;
                    driver_->resetEngine("clear recovery");
                }
            }
            if (driver_->resetPending() || gGpuIntsDead) {
                note("E5MALIS CLEAR4 SKIPPED (reset would not clear; engine %s)",
                     gGpuIntsDead ? "parked" : "busy");
            } else {
            uint32_t hFb = 0, hDesc = 0;
            uint64_t vaFb = 0, vaDesc = 0;
            uint8_t *fbCpu = nullptr, *dCpu = nullptr;
            bool ok = true;
            do {
                E5MaliCreateBo cb = {};
                cb.size = kClearFbBytes;
                cb.flags = 1;                      // noExec BO
                E5MaliCreateBoOut cbo;
                bzero(&cbo, sizeof(cbo));
                uint32_t used = 0;
                if (selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo, sizeof(cbo), &used) != kIOReturnSuccess ||
                    cbo.status != kIOReturnSuccess || cbo.handle == 0) {
                    note("E5MALIS CLEAR4 FAIL CreateBo(fb)");
                    ok = false;
                    break;
                }
                hFb = cbo.handle;
                cb = E5MaliCreateBo();
                cb.size = kDescBytes;
                bzero(&cbo, sizeof(cbo));
                used = 0;
                if (selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo, sizeof(cbo), &used) != kIOReturnSuccess ||
                    cbo.status != kIOReturnSuccess || cbo.handle == 0) {
                    note("E5MALIS CLEAR4 FAIL CreateBo(desc)");
                    ok = false;
                    break;
                }
                hDesc = cbo.handle;
                E5MaliMapBo mb = {};
                mb.handle = hFb;
                mb.flags = 2;                      // noExec mapping
                E5MaliMapBoOut mo;
                bzero(&mo, sizeof(mo));
                used = 0;
                if (selfCall(kSelMapBo, &mb, sizeof(mb), &mo, sizeof(mo), &used) != kIOReturnSuccess ||
                    mo.status != kIOReturnSuccess) {
                    note("E5MALIS CLEAR4 FAIL MapBo(fb)");
                    ok = false;
                    break;
                }
                vaFb = mo.va;
                mb = E5MaliMapBo();
                mb.handle = hDesc;                 // executable mapping
                bzero(&mo, sizeof(mo));
                used = 0;
                if (selfCall(kSelMapBo, &mb, sizeof(mb), &mo, sizeof(mo), &used) != kIOReturnSuccess ||
                    mo.status != kIOReturnSuccess) {
                    note("E5MALIS CLEAR4 FAIL MapBo(desc)");
                    ok = false;
                    break;
                }
                vaDesc = mo.va;
                fbCpu = (uint8_t *)bos_[hFb - 1].md->getBytesNoCopy();
                dCpu = (uint8_t *)bos_[hDesc - 1].md->getBytesNoCopy();
                if (!fbCpu || !dCpu) {
                    note("E5MALIS CLEAR4 FAIL no CPU view");
                    ok = false;
                    break;
                }
            } while (false);

            if (ok) {
                // R327 final: DEFAULT = astra short form — FBD Tiler
                // field (w14-15) = 0, NO tiler context/heap built at all;
                // a single fragment job with clean-tile-write performs the
                // clear (no tiler structures are read). gClearVariant=3
                // keeps the R327 full-tiler form (tctx w0=0 hardware
                // writeback + mask 0xFF) as the A/B fallback.
                if (gClearVariant == 3) {
                    selftestTilerHeap(dCpu + kOffHeap, vaDesc + kHeapArena,
                                      kHeapSize);
                    selftestTilerCtx(dCpu + kOffTctx, gHierMask,
                                     kClearFbW, kClearFbH,
                                     vaDesc + kOffHeap);
                    selftestFbdParams(dCpu + kOffFbd, kClearFbW, kClearFbH,
                                      1, 2u * 16u * 16u, vaDesc + kOffTctx);
                } else {
                    // Short form: FBD with Tiler = 0 (p[14]/p[15] zero).
                    selftestFbdParams(dCpu + kOffFbd, kClearFbW, kClearFbH,
                                      1, 2u * 16u * 16u, 0);
                }
                selftestRgbRt(dCpu + kOffRt0, vaFb, kClearStride, kClearStride,
                              kClearInternal);
                const unsigned tw = (kClearFbW + 15) / 16;   // 20 tiles
                const unsigned th = (kClearFbH + 15) / 16;   // 30 tiles
                const uint64_t fbdTag = ((vaDesc + kOffFbd) & ~0x3full) | 1u;
                selftestFragmentJob(dCpu + kOffFragJob, 0, 0, tw - 1, th - 1,
                                    fbdTag, 1);
                // R332-final sentinel (astra): pre-fill the framebuffer
                // with a NON-zero pattern so "still 0x5A5A after the job"
                // proves no-write vs "0xF800 everywhere" proves write.
                {
                    volatile uint32_t *spx = (volatile uint32_t *)fbCpu;
                    for (uint32_t i = 0; i < kClearFbBytes / 4; ++i)
                        spx[i] = 0x5a5a5a5au;
                }
                E5MaliCleanBo cl = {};
                cl.handle = hFb;
                selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);
                cl.handle = hDesc;
                selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);

                E5MaliStats s0;
                bzero(&s0, sizeof(s0));
                uint32_t used = 0;
                selfCall(kSelStats, nullptr, 0, &s0, sizeof(s0), &used);

                E5MaliSubmit sub = {};
                sub.jc = vaDesc + kOffFragJob;
                sub.slot = 0;                      // fragment slot
                sub.boHandles[0] = hFb;
                sub.boHandles[1] = hDesc;
                E5MaliSubmitOut so;
                bzero(&so, sizeof(so));
                used = 0;
                const IOReturn src = selfCall(kSelSubmit, &sub, sizeof(sub), &so,
                                              sizeof(so), &used);
                note("E5MALIS CLEAR4 SUBMIT %s atom=%llu jc=%llx rc=%x",
                     (src == kIOReturnSuccess && so.status == kIOReturnSuccess)
                         ? "PASS" : "FAIL",
                     (unsigned long long)so.atomId,
                     (unsigned long long)sub.jc, (unsigned)src);
                ok = src == kIOReturnSuccess && so.status == kIOReturnSuccess &&
                     so.atomId != 0;

                // Graphics jobs complete through the JM event path. Keep the
                // one terminal reply: QueryAtom also retires terminal atoms.
                E5MaliWaitAtomOut wo;
                bzero(&wo, sizeof(wo));
                if (ok) {
                    E5MaliWaitAtom w = {};
                    w.atomId = so.atomId;
                    w.timeoutMs = 500;
                    used = 0;
                    const IOReturn wr = selfCall(kSelWaitAtom, &w, sizeof(w),
                                                 &wo, sizeof(wo), &used);
                    ok = wr == kIOReturnSuccess && wo.status == kIOReturnSuccess &&
                         wo.state == kAtomDone;
                }
                // Authoritative verification: full-frame equality + FNV-1a64
                // against the CPU reference pattern. Sentinel counts are
                // reported alongside (R332-final): untouched = still
                // 0x5A5A (proves no write), cleared = 0xF800F800.
                cacheInvalidateRange(fbCpu, kClearFbBytes);
                volatile uint32_t *px = (volatile uint32_t *)fbCpu;
                uint64_t mismatch = 0, untouched = 0;
                for (uint32_t i = 0; i < kClearFbBytes / 4; ++i) {
                    if (px[i] != kClearWord) ++mismatch;
                    if (px[i] == 0x5a5a5a5au) ++untouched;
                }
                const uint64_t got = fnv1a64((const void *)fbCpu, kClearFbBytes);
                const uint64_t ref = fnv1a64WordPattern(0xcbf29ce484222325ull,
                                                        kClearWord,
                                                        kClearFbBytes / 4);
                E5MaliStats s1;
                bzero(&s1, sizeof(s1));
                used = 0;
                selfCall(kSelStats, nullptr, 0, &s1, sizeof(s1), &used);
                const uint64_t ds = s1.submitted - s0.submitted;
                const uint64_t dc = s1.completed - s0.completed;
                const uint64_t df = s1.faults - s0.faults;
                const uint64_t dt = s1.timedOut - s0.timedOut;
                const uint64_t dr = s1.resets - s0.resets;
                const uint64_t dm = s1.mmuFaults - s0.mmuFaults;
                const bool statsOk = ds == 1 && dc == 1 && df == 0 && dt == 0 &&
                                     dr == 0 && dm == 0;
                // Require both the saved terminal reply and the complete
                // frame comparison. Pixel changes never retire an atom.
                const bool clearOk = ok && mismatch == 0 && untouched == 0 &&
                                     got == ref;
                note("E5MALIS CLEAR4 %s mismatch=%llu untouched=%llu fnv=%016llx ref=%016llx state=%u hw=%02x(%s) ds=%llu dc=%llu df=%llu dt=%llu dr=%llu dm=%llu",
                     (clearOk && statsOk) ? "PASS" : "FAIL",
                     (unsigned long long)mismatch, (unsigned long long)untouched,
                     (unsigned long long)got,
                     (unsigned long long)ref, wo.state, wo.hwStatus,
                     wo.hwStatus == 0x01 ? "slot-done" : "none",
                     (unsigned long long)ds, (unsigned long long)dc,
                     (unsigned long long)df, (unsigned long long)dt,
                     (unsigned long long)dr, (unsigned long long)dm);
                if (!clearOk || !statsOk) {
                    // Descriptor dump for on-board byte-level triage (the
                    // encoders above cite v9.xml for every field). R325:
                    // job w4-7 (Type/Index/Next — R324's dump omitted them
                    // and w0's GPU-written exception status was misread as
                    // the type), tiler ctx/heap words, and the polygon
                    // list state AFTER the fault (the GPU may write
                    // diagnostics there; R324's fault pointer was the list).
                    const uint32_t *dj = (const uint32_t *)(dCpu + kOffFragJob);
                    const uint32_t *df2 = (const uint32_t *)(dCpu + kOffFbd);
                    const uint32_t *dr2 = (const uint32_t *)(dCpu + kOffRt0);
                    const uint32_t *dt2 = (const uint32_t *)(dCpu + kOffTctx);
                    const uint32_t *dh = (const uint32_t *)(dCpu + kOffHeap);
                    // R327: the tiler writes the list pointer into tctx
                    // w0-1; dump the list THROUGH that pointer (bounded to
                    // the desc BO) plus the arena head.
                    const uint64_t polyPtr = (uint64_t)dt2[0] | ((uint64_t)dt2[1] << 32);
                    const uint32_t *dp = (polyPtr >= vaDesc &&
                                         polyPtr + 16 <= vaDesc + kDescBytes)
                        ? (const uint32_t *)(dCpu + (polyPtr - vaDesc))
                        : (const uint32_t *)(dCpu + kHeapArena);
                    note("E5MALIS DUMP job w0=%08x w2=%08x w3=%08x w4=%08x w6=%08x w7=%08x w8=%08x w9=%08x wA=%08x wB=%08x",
                         dj[0], dj[2], dj[3], dj[4], dj[6], dj[7], dj[8], dj[9],
                         dj[10], dj[11]);
                    note("E5MALIS DUMP fbd w8=%08x w9=%08x wA=%08x wB=%08x wE=%08x rt w1=%08x w8=%08x wA=%08x",
                         df2[8], df2[9], df2[10], df2[11], df2[14],
                         dr2[1], dr2[8], dr2[10]);
                    note("E5MALIS DUMP tctx w0=%08x w1=%08x w2=%08x w3=%08x w6=%08x w7=%08x heap w0=%08x w1=%08x w2=%08x w6=%08x",
                         dt2[0], dt2[1], dt2[2], dt2[3], dt2[6], dt2[7],
                         dh[0], dh[1], dh[2], dh[6]);
                    note("E5MALIS DUMP poly[0..3]=%08x %08x %08x %08x (post-fault)",
                         dp[0], dp[1], dp[2], dp[3]);
                }
                ok = clearOk && statsOk;
                clear4Pass = ok;
            }

            // Teardown both BOs (post-retire; no Busy expected).
            if (hFb) {
                E5MaliUnmapBo um = {};
                um.handle = hFb;
                um.va = vaFb;
                um.length = kClearFbBytes;
                const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
                E5MaliDestroyBo db = {};
                db.handle = hFb;
                const IOReturn r2 = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
                clear4Pass = clear4Pass && r1 == kIOReturnSuccess && r2 == kIOReturnSuccess;
                note("E5MALIS CLEAR4 TEARDOWN fb %s unmap=%x destroy=%x",
                     (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? "PASS" : "FAIL",
                     (unsigned)r1, (unsigned)r2);
            }
            if (hDesc) {
                E5MaliUnmapBo um = {};
                um.handle = hDesc;
                um.va = vaDesc;
                um.length = kDescBytes;
                const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
                E5MaliDestroyBo db = {};
                db.handle = hDesc;
                const IOReturn r2 = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
                clear4Pass = clear4Pass && r1 == kIOReturnSuccess && r2 == kIOReturnSuccess;
                note("E5MALIS CLEAR4 TEARDOWN desc %s unmap=%x destroy=%x",
                     (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? "PASS" : "FAIL",
                     (unsigned)r1, (unsigned)r2);
            }
            }
        }
    }

    if (mask & 16u) {
        if (!seq3Pass || !tri4Pass || !clear4Pass || !e5_mali_g1_ready ||
            driver_->resetPending() || gGpuIntsDead) {
            note("E5MALIS SCAN5 FAIL prerequisites seq3=%u triangle=%u clear=%u",
                 seq3Pass, tri4Pass, clear4Pass);
        } else {
            const bool passed = runTri4(true);
            note("E5MALIS SCAN5 AUTO %s physical=UNVERIFIED", passed ? "PASS" : "FAIL");
        }
    }
}

// ---------------------------------------------------------------------------
// §4.2: fullscreen-const draw + analytic-triangle draw, one BO pair, the
// offline-compiled shader assets, and the R322 three-signal completion
// framework (per-atom). All encodings cite v9.xml in the helpers above.
// ---------------------------------------------------------------------------
bool E5MaliG1UserClient::runTri4(bool scanout) {
    const char *tag = scanout ? "SCAN5" : "TRI4";
    note("E5MALIS %s start (assets: mesa offline, dump 5436c168… R339 edge fix)", tag);
    uint32_t hFb = 0, hDesc = 0;
    uint64_t vaFb = 0, vaDesc = 0;
    uint8_t *fbCpu = nullptr, *dCpu = nullptr;
    IOMemoryMap *scanoutMap = nullptr;
    const uint64_t initialMappedBytes = ctx_->mappedBytes;
    const uint64_t initialBoBytes = liveBoBytes_;
    E5MaliStats initialStats = {};
    selfCall(kSelStats, nullptr, 0, &initialStats, sizeof(initialStats), nullptr);
    bool allOk = false;
    uint16_t *golden = (uint16_t *)IOMalloc(kClearFbBytes);
    bool haveGolden = golden != nullptr;
    bool v0Pass = false, v1Pass = false, v1Run = false;
    const unsigned repeatsPerVariant = 8;
    unsigned passedBatches = 0;

    do {
        if (!haveGolden) { note("E5MALIS %s FAIL reference allocation", tag); break; }
        if (scanout && (!e5_mali_g1_scanout_selected() ||
                        !e5_display_gpu_scanout_owned() || e5_input_presented_frames())) {
            note("E5MALIS SCAN5 FAIL exclusive scanout ownership");
            break;
        }
        E5MaliCreateBo cb = {};
        cb.size = kClearFbBytes;
        cb.flags = 1;                       // noExec BO
        E5MaliCreateBoOut cbo;
        bzero(&cbo, sizeof(cbo));
        uint32_t used = 0;
        if (scanout) {
            // 307200 bytes is exactly 75 GPU pages. No padding page or
            // adjacent reserved memory is exposed to this render target.
            E5MaliImportFixed im = {};
            im.physAddr = 0xbe000000ull;
            im.length = kClearFbBytes;
            im.flags = 1;                  // NX render target
            E5MaliMapBoOut io = {};
            const IOReturn ir = selfCall(kSelImportFixed, &im, sizeof(im),
                                         &io, sizeof(io), &used);
            if (ir != kIOReturnSuccess || io.status != kIOReturnSuccess || !io.va) {
                note("E5MALIS SCAN5 FAIL ImportFixed rc=%x status=%x", (unsigned)ir, io.status);
                break;
            }
            vaFb = io.va;
            // ABI 1.0 returns only the VA. Resolve this client's own record
            // for the normal BO pin/unmap/destroy path; no ABI extension.
            for (uint32_t i = 0; i < kMaxBoHandles; ++i)
                if (bos_[i].imported && bos_[i].importPa == im.physAddr &&
                    bos_[i].mapCount == 1 && bos_[i].maps[0].va == vaFb &&
                    bos_[i].size == im.length) { hFb = i + 1; break; }
            bool exact = hFb && ctx_->mappedBytes == initialMappedBytes + im.length &&
                         liveBoBytes_ == initialBoBytes + im.length;
            unsigned verifiedPages = 0;
            for (uint32_t off = 0; off < kClearFbBytes; off += E5MaliG1Mmu::kPageSize) {
                const auto lu = ctx_->table.lookupEntry(vaFb + off);
                const bool pageOk = lu.mapped && lu.phys == im.physAddr + off &&
                                    (lu.entry & E5MaliG1Mmu::kNoExecBit);
                if (pageOk) ++verifiedPages;
                exact = exact && pageOk;
            }
            IODeviceMemory *memory = IODeviceMemory::withRange(im.physAddr, im.length);
            if (memory) {
                scanoutMap = memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
                memory->release();
            }
            exact = exact && scanoutMap && scanoutMap->getVirtualAddress() &&
                    scanoutMap->getPhysicalAddress() == im.physAddr &&
                    scanoutMap->getLength() == im.length;
            note("E5MALIS SCAN5 MAP-CHECK pages=%u/75 entry0=%016llx cpu_va=%llx cpu_pa=%llx cpu_bytes=%llu",
                 verifiedPages, (unsigned long long)ctx_->table.lookupEntry(vaFb).entry,
                 (unsigned long long)(scanoutMap ? scanoutMap->getVirtualAddress() : 0),
                 (unsigned long long)(scanoutMap ? scanoutMap->getPhysicalAddress() : 0),
                 (unsigned long long)(scanoutMap ? scanoutMap->getLength() : 0));
            note("E5MALIS SCAN5 IMPORT %s pa=%llx bytes=%u pages=75 va=%llx handle=%u cpu=read-only-uncached mapped=%llu",
                 exact ? "PASS" : "FAIL", (unsigned long long)im.physAddr, kClearFbBytes,
                 (unsigned long long)vaFb, hFb, (unsigned long long)ctx_->mappedBytes);
            if (!exact) break;
            fbCpu = (uint8_t *)scanoutMap->getVirtualAddress();
        } else {
            if (selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo, sizeof(cbo), &used) != kIOReturnSuccess ||
                cbo.status != kIOReturnSuccess || cbo.handle == 0) {
                note("E5MALIS %s FAIL CreateBo(fb)", tag);
                break;
            }
            hFb = cbo.handle;
        }
        cb = E5MaliCreateBo();
        cb.size = kTriDescBytes;            // 0x201000, executable
        bzero(&cbo, sizeof(cbo));
        used = 0;
        if (selfCall(kSelCreateBo, &cb, sizeof(cb), &cbo, sizeof(cbo), &used) != kIOReturnSuccess ||
            cbo.status != kIOReturnSuccess || cbo.handle == 0) {
            note("E5MALIS %s FAIL CreateBo(desc)", tag);
            break;
        }
        hDesc = cbo.handle;
        E5MaliMapBo mb = {};
        mb.handle = hFb;
        mb.flags = 2;                       // noExec mapping
        E5MaliMapBoOut mo;
        bzero(&mo, sizeof(mo));
        used = 0;
        if (!scanout) {
            if (selfCall(kSelMapBo, &mb, sizeof(mb), &mo, sizeof(mo), &used) != kIOReturnSuccess ||
                mo.status != kIOReturnSuccess) { note("E5MALIS %s FAIL MapBo(fb)", tag); break; }
            vaFb = mo.va;
        }
        mb = E5MaliMapBo();
        mb.handle = hDesc;                  // executable mapping (shader bin)
        bzero(&mo, sizeof(mo));
        used = 0;
        if (selfCall(kSelMapBo, &mb, sizeof(mb), &mo, sizeof(mo), &used) != kIOReturnSuccess ||
            mo.status != kIOReturnSuccess) { note("E5MALIS %s FAIL MapBo(desc)", tag); break; }
        vaDesc = mo.va;
        if (!scanout) fbCpu = (uint8_t *)bos_[hFb - 1].md->getBytesNoCopy();
        dCpu = (uint8_t *)bos_[hDesc - 1].md->getBytesNoCopy();
        if (!fbCpu || !dCpu) { note("E5MALIS %s FAIL no CPU view", tag); break; }

        // ---- static descriptor content (variant-independent) --------------
        memcpy(dCpu + kTriOffVsBin, e5MaliVsFullscreenBin, sizeof(e5MaliVsFullscreenBin));
        memcpy(dCpu + kTriOffFsConstBin, e5MaliFsConstBin, sizeof(e5MaliFsConstBin));
        memcpy(dCpu + kTriOffFsTriBin, e5MaliFsTriBin, sizeof(e5MaliFsTriBin));
        // FAU buffers padded to 16B (astra): the SE's FAU pointer is
        // 16-byte granular — 3 words + 1 zero word so the 4th slot reads
        // as 0.0f and never garbage.
        bzero(dCpu + kTriOffVsFau, 16);
        memcpy(dCpu + kTriOffVsFau, e5MaliVsFullscreenFau, sizeof(e5MaliVsFullscreenFau));
        {
            uint32_t vp[8];
            memcpy(vp, e5MaliVsFullscreenProg, sizeof(vp));
            const uint64_t vsBinVa = vaDesc + kTriOffVsBin;
            vp[2] = (uint32_t)vsBinVa;
            vp[3] = (uint32_t)(vsBinVa >> 32);
            memcpy(dCpu + kTriOffVsProg, vp, sizeof(vp));
        }
        // Resource table (R328, full-tree authority): ALL ZERO. The v9
        // resource model has TWO table classes — SE Resources covers
        // descriptor-set tables 0..11 (panvk_vX_cmd_desc_state.c:259-309
        // fills entry[0]=driver set + used sets; our shader uses none),
        // while compiler tables 60..63 fold to hardware-managed tables
        // (valhall.h:152-163: 61→13 = the vertex attribute buffer that
        // the MALLOC-VERTEX hardware itself allocates from the heap;
        // v9.xml "Vertex Array": "Pointer: Written by hardware"). Our
        // R326/R327 entry[13]=posBuf fill was the wrong model — the
        // shader's LEA_BUF table:0xD resolves against the HARDWARE table,
        // never against this one. posBuf staying zero is expected; the
        // position outputs land in heap-allocated vertex packets.
        {
            uint64_t *rt = (uint64_t *)(dCpu + kTriOffResTable);
            bzero(rt, kTriResEntries * 16);
        }
        selftestMallocVertexJob(dCpu + kTriOffMvj, vaDesc + kTriOffTctx);
        // Position SE at MVJ+0x100 (vs; FAU 3 words -> count 2).
        // R327: Mesa passes a Local Storage descriptor unconditionally
        // (jm/panvk_vX_cmd_draw.c:2382,2396 — thread_storage = tls.gpu).
        // Zero-size TLS => all-zero words except WLS Instances log2 field
        // = No Workgroup Mem (v9.xml:940-950). PACKED form (astra): the
        // WLS Instances field is modifier="log2", so the logical enum
        // 0x80000000 packs to log2 = 31 = 5-bit 0x1f — w1 = 0x0000001f.
        {
            uint32_t *tls = (uint32_t *)(dCpu + kTriOffTls);
            bzero(tls, 32);
            tls[1] = 0x0000001fu;
        }
        // R327: Depth/Stencil descriptor is allocated unconditionally
        // (v9_prepare_blend_zsd, :2164-2199) — R334/astra fix: an ALL-ZERO
        // ZSD packs depth_function = 0 = **NEVER** (Func enum v9.xml:99-107)
        // which kills EVERY fragment. Mesa explicitly fills
        // `depth_function = test_z ? … : MALI_FUNC_ALWAYS` (:2193-2194) —
        // no depth test => ALWAYS(7) at w4[31:29], plus depth_cull_enable
        // (bit22, default true, :2186). Type field w0[3:0] = Depth/stencil
        // = 7 (Descriptor Type enum). Stencil tests stay off.
        {
            uint32_t *zsd = (uint32_t *)(dCpu + kTriOffZsd);
            bzero(zsd, 32);
            zsd[0] = 7u;                          // Type = Depth/stencil
            zsd[4] = (1u << 22) | (7u << 29);     // cull_enable | ALWAYS
        }
        // R327: fs SE resources — Mesa always passes fs.desc.res_table
        // (:2380); provide an empty 16-entry table instead of 0.
        bzero(dCpu + kTriOffFsRes, kTriResEntries * 16);
        selftestSe(dCpu + kTriOffMvj + 0x100, 2,
                   selftestSeResources(vaDesc + kTriOffResTable, kTriResEntries),
                   vaDesc + kTriOffVsProg, vaDesc + kTriOffVsFau,
                   vaDesc + kTriOffTls);
        // Varying SE at MVJ+0x140: all zero (secondary_enable=0).
        selftestBlendOpaque(dCpu + kTriOffBlend);
        // R327: tctx w0 = 0 (tiler writes the list pointer; Mesa never
        // packs polygon_list on v9), mask 0xFF (pan_select_tiler…).
        selftestTilerHeap(dCpu + kTriOffHeap, vaDesc + kHeapArena, kHeapSize);
        selftestTilerCtx(dCpu + kTriOffTctx, gHierMask,
                         kClearFbW, kClearFbH, vaDesc + kTriOffHeap);
        selftestFbdParams(dCpu + kTriOffFbd, kClearFbW, kClearFbH, 1,
                          2u * 16u * 16u, vaDesc + kTriOffTctx);
        selftestRgbRt(dCpu + kTriOffRt0, vaFb, kClearStride, kClearStride,
                      kClearInternal);     // internal-format clear value
        const uint64_t fbdTag = ((vaDesc + kTriOffFbd) & ~0x3full) | 1u;
        selftestFragmentJob(dCpu + kTriOffFragJob, 0, 0, 19, 29, fbdTag, 1);

        E5MaliStats s0;
        bzero(&s0, sizeof(s0));
        used = 0;
        selfCall(kSelStats, nullptr, 0, &s0, sizeof(s0), &used);
        (void)s0;

        for (unsigned batch = 0; batch < repeatsPerVariant * 2; ++batch) {
            if (scanout && (!e5_display_gpu_scanout_owned() || e5_input_presented_frames())) {
                note("E5MALIS SCAN5 FAIL ownership changed");
                break;
            }
            const bool tri = (batch & 1u) != 0;
            note("E5MALIS %s REPEAT %u/%u pattern=%s", tag, batch / 2 + 1,
                 repeatsPerVariant, tri ? "TRIANGLE" : "FULLSCREEN");
            if (tri) {
                v1Run = true;
                if (!v0Pass) {
                    note("E5MALIS %s-TRIANGLE SKIPPED (fullscreen variant FAIL)", tag);
                    v1Run = false;
                    break;
                }
            }
            // Variant-dependent content.
            const uint32_t *fsProg = tri ? e5MaliFsTriProg : e5MaliFsConstProg;
            const uint32_t *fsFau = tri ? e5MaliFsTriFau : e5MaliFsConstFau;
            const uint32_t fsFauWords =
                tri ? (uint32_t)(sizeof(e5MaliFsTriFau) / sizeof(uint32_t))
                    : (uint32_t)(sizeof(e5MaliFsConstFau) / sizeof(uint32_t));
            const uint32_t fsBinOff = tri ? kTriOffFsTriBin : kTriOffFsConstBin;
            {
                uint32_t fp[8];
                memcpy(fp, fsProg, sizeof(fp));
                const uint64_t fsBinVa = vaDesc + fsBinOff;
                fp[2] = (uint32_t)fsBinVa;
                fp[3] = (uint32_t)(fsBinVa >> 32);
                memcpy(dCpu + kTriOffFsProg, fp, sizeof(fp));
            }
            bzero(dCpu + kTriOffFsFau, 32);
            memcpy(dCpu + kTriOffFsFau, fsFau, fsFauWords * 4);
            // Reset only the GPU-written job-header prefix before reuse.
            // Mesa JM does memset(job, 0, 4 * 4); preserve type/index/Next.
            bzero(dCpu + kTriOffMvj, 16);
            bzero(dCpu + kTriOffFragJob, 16);
            // Deterministic test contents; ALLOC_ON_FAULT does not imply
            // fresh zero-filled pages when an existing heap is reused.
            bzero(dCpu + kHeapArena, kHeapSize);
            // R337/astra-5: restore the TEMPLATES each variant too — the
            // hardware rewrites the heap descriptor's bottom/top and the
            // tctx polygon-list word during every MVJ. Mesa re-emits both
            // from templates per batch (heap_templ memcpy, jm cmd_buffer
            // :352-353; tctx w0 packed 0). ALLOC_ON_FAULT does NOT
            // guarantee fresh zero pages per round — only first touch.
            selftestTilerHeap(dCpu + kTriOffHeap, vaDesc + kHeapArena,
                              kHeapSize);
            selftestTilerCtx(dCpu + kTriOffTctx, gHierMask,
                             kClearFbW, kClearFbH, vaDesc + kTriOffHeap);
            // DCD: fst needs shader_modifies_coverage(bit20) + pixel/zs
            // FORCE_LATE (3<<2 | 3<<4); fsc uses FORCE_EARLY defaults and
            // allow_forward_pixel_to_kill (bit0 — Mesa sets it for
            // can_fpk/no-dest-read const shaders, :2373-2376).
            selftestDcd(dCpu + kTriOffMvj + 0x80,
                        tri ? 0x100032u : 0x3u, vaDesc + kTriOffBlend,
                        vaDesc + kTriOffZsd);
            selftestSe(dCpu + kTriOffMvj + 0x80 + 0x40, (fsFauWords + 1) / 2,
                       selftestSeResources(vaDesc + kTriOffFsRes, kTriResEntries),
                       vaDesc + kTriOffFsProg, vaDesc + kTriOffFsFau,
                       vaDesc + kTriOffTls);
            E5MaliCleanBo cl = {};
            if (!scanout) {
                cl.handle = hFb;
                selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);
            } // Imported target has no CPU writes or cache-clean operation.
            cl.handle = hDesc;
            selfCall(kSelCleanBo, &cl, sizeof(cl), nullptr, 0, nullptr);

            E5MaliStats sv;
            bzero(&sv, sizeof(sv));
            used = 0;
            selfCall(kSelStats, nullptr, 0, &sv, sizeof(sv), &used);

            // Atom A: malloc-vertex (slot 1), completed before fragment.
            E5MaliSubmit sub = {};
            sub.jc = vaDesc + kTriOffMvj;
            sub.slot = 1;
            sub.boHandles[0] = hFb;
            sub.boHandles[1] = hDesc;
            E5MaliSubmitOut so;
            bzero(&so, sizeof(so));
            used = 0;
            const IOReturn rA = selfCall(kSelSubmit, &sub, sizeof(sub), &so,
                                         sizeof(so), &used);
            E5MaliWaitAtomOut woA;
            bzero(&woA, sizeof(woA));
            bool mvOk = rA == kIOReturnSuccess && so.status == kIOReturnSuccess &&
                        so.atomId != 0;
            if (mvOk) {
                E5MaliWaitAtom wa = {};
                wa.atomId = so.atomId;
                wa.timeoutMs = 500;
                used = 0;
                const IOReturn wr = selfCall(kSelWaitAtom, &wa, sizeof(wa),
                                             &woA, sizeof(woA), &used);
                mvOk = wr == kIOReturnSuccess && woA.status == kIOReturnSuccess &&
                       woA.state == kAtomDone;
                if (!mvOk)
                    note("E5MALIS %s MVJ incomplete state=%u hw=%02x rc=%x; fragment skipped", tag,
                         woA.state, woA.hwStatus, (unsigned)wr);
            } else {
                note("E5MALIS %s FAIL submit mvj rc=%x", tag, (unsigned)rA);
            }

            // The fragment consumes the complete hardware-written TCTX.
            // No CPU tag rewrite and no pixel-based completion shortcut.
            bool fjOk = false;
            E5MaliWaitAtomOut woB;
            bzero(&woB, sizeof(woB));
            if (mvOk) {
                E5MaliSubmit subB = {};
                subB.jc = vaDesc + kTriOffFragJob;
                subB.slot = 0;
                subB.boHandles[0] = hFb;
                subB.boHandles[1] = hDesc;
                E5MaliSubmitOut soB;
                bzero(&soB, sizeof(soB));
                used = 0;
                const IOReturn rB = selfCall(kSelSubmit, &subB, sizeof(subB), &soB,
                                             sizeof(soB), &used);
                if (rB == kIOReturnSuccess && soB.status == kIOReturnSuccess &&
                    soB.atomId != 0) {
                    E5MaliWaitAtom wb = {};
                    wb.atomId = soB.atomId;
                    wb.timeoutMs = 500;
                    used = 0;
                    const IOReturn wr = selfCall(kSelWaitAtom, &wb, sizeof(wb),
                                                 &woB, sizeof(woB), &used);
                    fjOk = wr == kIOReturnSuccess && woB.status == kIOReturnSuccess &&
                           woB.state == kAtomDone;
                } else {
                    note("E5MALIS %s FAIL submit frag rc=%x", tag, (unsigned)rB);
                }
            }

            // ---- verification ------------------------------------------------
            if (!scanout) cacheInvalidateRange(fbCpu, kClearFbBytes);
            __asm__ volatile("dsb sy" ::: "memory");
            volatile uint32_t *px = (volatile uint32_t *)fbCpu;
            uint64_t mismatch = 0, inside = 0;
            uint64_t got = 0, ref = 0;
            bool valueOk = false;
            if (!tri) {
                for (uint32_t i = 0; i < kClearFbBytes / 4; ++i)
                    if (px[i] != kTriFullWord) ++mismatch;
                got = fnv1a64((const void *)fbCpu, kClearFbBytes);
                ref = fnv1a64WordPattern(0xcbf29ce484222325ull, kTriFullWord,
                                         kClearFbBytes / 4);
                valueOk = mismatch == 0 && got == ref;
            } else if (haveGolden) {
                // Integer-only rasterizer: pixel centres (2x+1, 2y+1) on the
                // doubled-vertex triangle; inside=yellow, outside=bg.
                volatile uint16_t *px16 = (volatile uint16_t *)fbCpu;
                for (uint32_t y = 0; y < kClearFbH; ++y)
                    for (uint32_t x = 0; x < kClearFbW; ++x) {
                        const bool inTri = selftestPtInTri(2 * (int)x + 1,
                                                           2 * (int)y + 1);
                        if (inTri) ++inside;
                        golden[(uint64_t)y * kClearFbW + x] =
                            inTri ? kTriInside565 : (uint16_t)0xf800u;
                    }
                for (uint64_t i = 0; i < (uint64_t)kClearFbBytes / 2; ++i)
                    if (px16[i] != golden[i]) ++mismatch;
                got = fnv1a64((const void *)fbCpu, kClearFbBytes);
                ref = fnv1a64((const void *)golden, kClearFbBytes);
                valueOk = mismatch == 0 && got == ref && inside == 39810;
            }
            E5MaliStats s1;
            bzero(&s1, sizeof(s1));
            used = 0;
            selfCall(kSelStats, nullptr, 0, &s1, sizeof(s1), &used);
            const uint64_t ds = s1.submitted - sv.submitted;
            const uint64_t dc = s1.completed - sv.completed;
            const uint64_t derr = (s1.faults - sv.faults) +
                                  (s1.timedOut - sv.timedOut) +
                                  (s1.resets - sv.resets) +
                                  (s1.mmuFaults - sv.mmuFaults);
            const bool statsOk = ds == 2 && dc == 2 && derr == 0;
            cacheInvalidateRange(dCpu + kTriOffMvj, 64);
            cacheInvalidateRange(dCpu + kTriOffFragJob, 64);
            const uint32_t mvStatus = ((const uint32_t *)(dCpu + kTriOffMvj))[0];
            const uint32_t fjStatus = ((const uint32_t *)(dCpu + kTriOffFragJob))[0];
            bool pass = mvOk && fjOk && mvStatus == 1 && fjStatus == 1 &&
                        valueOk && statsOk;
            if (scanout && pass) {
                const unsigned frame = 5000u + batch;
                note("E5MALIS SCAN5 HOLD frame=%u pattern=%s ms=2000", frame,
                     tri ? "TRIANGLE" : "FULLSCREEN");
                IOSleep(2000);
                // Capture GPU output into ordinary host memory only. The
                // imported framebuffer mapping cannot be written by the CPU.
                const volatile uint16_t *source = (const volatile uint16_t *)fbCpu;
                for (uint32_t i = 0; i < kClearFbBytes / 2; ++i) golden[i] = source[i];
                __asm__ volatile("dsb sy" ::: "memory");
                bool stable = true;
                for (uint32_t i = 0; i < kClearFbBytes / 2; ++i)
                    if (golden[i] != source[i]) { stable = false; break; }
                const uint64_t snapshotHash = fnv1a64(golden, kClearFbBytes);
                const unsigned cpuCopies = e5_input_presented_frames();
                bool captureOk = stable && snapshotHash == got && !cpuCopies &&
                                 e5_display_gpu_scanout_owned();
                if (captureOk) captureOk = e5_input_emit_frame(frame, golden, kClearFbBytes);
                note("E5MALIS SCAN5 CAPTURE %s frame=%u fnv=%016llx stable_reads=%u cpu_copies=%u",
                     captureOk ? "PASS" : "FAIL", frame, (unsigned long long)snapshotHash,
                     stable ? 2u : 0u, cpuCopies);
                pass = captureOk;
            }
            if (pass) ++passedBatches;
            note("E5MALIS %s HEADERS mv=%08x frag=%08x", tag, mvStatus, fjStatus);
            if (!tri) v0Pass = pass;
            else v1Pass = pass;
            note("E5MALIS %s-%s %s mismatch=%llu fnv=%016llx ref=%016llx inside=%llu stateA=%u/%02x stateB=%u/%02x ds=%llu dc=%llu derr=%llu", tag,
                 tri ? "TRIANGLE" : "FULLSCREEN", pass ? "PASS" : "FAIL",
                 (unsigned long long)mismatch, (unsigned long long)got,
                 (unsigned long long)ref, (unsigned long long)inside,
                 woA.state, woA.hwStatus, woB.state, woB.hwStatus,
                 (unsigned long long)ds, (unsigned long long)dc,
                 (unsigned long long)derr);
            if (!pass) {
                // Invalidate GPU-written metadata before deriving pointers.
                cacheInvalidateRange(dCpu + kTriOffMvj, 384);
                cacheInvalidateRange(dCpu + kTriOffHeap, 32);
                cacheInvalidateRange(dCpu + kTriOffTctx, 192);
                // On-board bytes for triage (SE-DCD-FILL §8 ordering).
                const uint32_t *mj = (const uint32_t *)(dCpu + kTriOffMvj);
                const uint32_t *dd = (const uint32_t *)(dCpu + kTriOffMvj + 0x80);
                const uint32_t *vpr = (const uint32_t *)(dCpu + kTriOffVsProg);
                const uint32_t *fpw = (const uint32_t *)(dCpu + kTriOffFsProg);
                const uint32_t *sev = (const uint32_t *)(dCpu + kTriOffMvj + 0x100);
                const uint32_t *fw = (const uint32_t *)(dCpu + kTriOffMvj + 0x80 + 0x40);
                const uint64_t *rt = (const uint64_t *)(dCpu + kTriOffResTable);
                const uint32_t *bl = (const uint32_t *)(dCpu + kTriOffBlend);
                const uint32_t *tcw = (const uint32_t *)(dCpu + kTriOffTctx);
                const uint32_t *hpw = (const uint32_t *)(dCpu + kTriOffHeap);
                // Through the tiler-written list pointer (bounded), with
                // cache invalidate so the tiler's writes are visible.
                const uint64_t tpoly = ((uint64_t)tcw[0] | ((uint64_t)tcw[1] << 32)) &
                                       0x0000ffffffffffffull;
                const uint32_t *plw = (tpoly >= vaDesc &&
                                       tpoly + 64 <= vaDesc + kTriDescBytes)
                    ? (const uint32_t *)(dCpu + (tpoly - vaDesc))
                    : (const uint32_t *)(dCpu + kHeapArena);
                cacheInvalidateRange((void *)((uintptr_t)plw & ~(uintptr_t)63), 64);
                note("E5MALIS DUMP mvj w4=%08x prim=%08x alloc=%08x dcd w0=%08x w1=%08x w2=%08x wC=%08x",
                     mj[4], mj[8], mj[13], dd[0], dd[1], dd[2], dd[12]);
                // R336/R337: MVJ's OWN header w0-w4 after the wait — w0 is
                // the GPU-written exception status (fault code/access or
                // 0x01 DONE), w2-3 its fault pointer; settles "faulted vs
                // silently stalled". Also index_count at +0x2c (mj[11]) —
                // astra-5's prime suspect, verified here on-board (we set 3).
                cacheInvalidateRange((void *)((uintptr_t)mj & ~(uintptr_t)63), 64);
                note("E5MALIS DUMP mvjHdr w0=%08x w2=%08x w3=%08x w4=%08x indexCount(+2c)=%08x inst(+30)=%08x",
                     mj[0], mj[2], mj[3], mj[4], mj[11], mj[12]);
                note("E5MALIS DUMP tctx w0=%08x w1=%08x w2=%08x w3=%08x w6=%08x w7=%08x heap w0=%08x w1=%08x w2=%08x w6=%08x",
                     tcw[0], tcw[1], tcw[2], tcw[3], tcw[6], tcw[7],
                     hpw[0], hpw[1], hpw[2], hpw[6]);
                // R335: the list head in full — 16 words through the
                // (masked) tiler-written pointer; the level-0 block-header
                // format becomes directly comparable across rounds.
                note("E5MALIS DUMP poly[0..15]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                     plw[0], plw[1], plw[2], plw[3], plw[4], plw[5], plw[6], plw[7],
                     plw[8], plw[9], plw[10], plw[11], plw[12], plw[13], plw[14], plw[15]);
                // R337/astra-5: full-allocation scan — the tiler reserved
                // [new_top, old_top) (~0x1F80): 809-bin pointer table plus
                // headers, poly pointer ≈ new_top+0x600. Dump a probe
                // ladder through the region (empty bins may legally be
                // zero — nonzero words are the structure).
                {
                    const uint64_t heapTop = ((uint64_t)hpw[6] | ((uint64_t)hpw[7] << 32)) &
                                             0x0000ffffffffffffull;
                    const uint64_t heapBase = vaDesc + kHeapArena;
                    const uint64_t oldTop = heapBase + kHeapSize;
                    if (heapTop > heapBase && heapTop < oldTop) {
                        const uint64_t rel = heapTop - vaDesc;
                        cacheInvalidateRange((void *)((uintptr_t)(dCpu + rel) & ~(uintptr_t)63), 128);
                        const uint32_t *at = (const uint32_t *)(dCpu + rel);
                        note("E5MALIS DUMP newtop+%03x: %08x %08x %08x %08x",
                             0x0u, at[0], at[1], at[2], at[3]);
                        note("E5MALIS DUMP newtop+%03x: %08x %08x %08x %08x",
                             0x10u, at[4], at[5], at[6], at[7]);
                        if (oldTop - heapTop >= 0x1f80) {
                            const uint32_t *mid = (const uint32_t *)(dCpu + rel + 0x600);
                            note("E5MALIS DUMP newtop+600: %08x %08x %08x %08x",
                                 mid[0], mid[1], mid[2], mid[3]);
                        }
                    }
                }
                {
                    // R331: FBD tiler field (w14-15 — MUST be the tctx
                    // pointer for TRI4; R327 only zeroed CLEAR4's) and the
                    // RT writeback base (w8-9).
                    const uint32_t *fbw = (const uint32_t *)(dCpu + kTriOffFbd);
                    const uint32_t *rtw = (const uint32_t *)(dCpu + kTriOffRt0);
                    note("E5MALIS DUMP fbd w14=%08x w15=%08x rt w1=%08x w8=%08x w9=%08x wA=%08x",
                         fbw[14], fbw[15], rtw[1], rtw[8], rtw[9], rtw[10]);
                }
                note("E5MALIS DUMP vsprog w0=%08x w1=%08x w2=%08x w3=%08x fsprog w0=%08x w1=%08x w2=%08x w3=%08x",
                     vpr[0], vpr[1], vpr[2], vpr[3], fpw[0], fpw[1], fpw[2], fpw[3]);
                note("E5MALIS DUMP vsSE w1=%08x w8=%08x wA=%08x wE=%08x fsSE w1=%08x w8=%08x wA=%08x wE=%08x res13=%016llx/%016llx blend=%08x %08x %08x %08x",
                     sev[1], sev[8], sev[10], sev[14], fw[1], fw[8], fw[10], fw[14],
                     (unsigned long long)rt[13 * 2], (unsigned long long)rt[13 * 2 + 1],
                     bl[0], bl[1], bl[2], bl[3]);
                // R333: the embedded fragment SE lives at DCD+0x40 =
                // MVJ+0xC0 (v9.xml Draw "Shader" start=16:0 size=512 —
                // INLINE, not a pointer; fw above already reads it). Dump
                // the full 16 words plus the DCD head so the on-board
                // bytes settle embed-vs-pointer and preload bitmaps.
                note("E5MALIS DUMP dcd w0=%08x w1=%08x w2=%08x wC=%08x fsSE[0..15]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                     dd[0], dd[1], dd[2], dd[12],
                     fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fw[6], fw[7],
                     fw[8], fw[9], fw[10], fw[11], fw[12], fw[13], fw[14], fw[15]);
                // R334: FJ job-header w0-w3 after the fault — w0 is the
                // GPU-written exception status (code + access), w2-3 the
                // 64-bit fault pointer: the exact DATA_INVALID location.
                // (Note: the dcd w2 "tag" the hardware writes — e.g.
                // 0x1005c001 — is the Vertex Array packet/count form,
                // NOT masked by Mesa's fragment path; MX :460-493 has
                // exactly ONE mask, the polygon pointer. No second mask.)
                {
                    const uint32_t *fj = (const uint32_t *)(dCpu + kTriOffFragJob);
                    cacheInvalidateRange((void *)((uintptr_t)fj & ~(uintptr_t)63), 64);
                    note("E5MALIS DUMP fjHdr w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x",
                         fj[0], fj[1], fj[2], fj[3], fj[4]);
                }
                // R328: tiler side effects + old probe buffer (expected
                // zero under the hw-table model) for on-board triage.
                {
                    const uint32_t *pb =
                        (const uint32_t *)(dCpu + kTriOffPosBuf);
                    note("E5MALIS DUMP posBuf=%08x %08x tctx w0=%08x w1=%08x heap w4=%08x w5=%08x w6=%08x",
                         pb[0], pb[1], tcw[0], tcw[1], hpw[4], hpw[5], hpw[6]);
                    // R329: vertex packet first words — via the DCD's
                    // hardware-written Vertex Array pointer (dcd w2/w3,
                    // "Pointer: Written by hardware", v9.xml) and the heap
                    // Bottom (w4/w5). R332-final: the hardware pointer
                    // carries count bits in [5:0] AND lives in the heap
                    // arena (NOT the desc BO) — mask to 64B alignment and
                    // INVALIDATE the dcd words + the target line before
                    // reading (R331's "all zero" was a cache artifact).
                    {
                        const uint64_t vaPtr =
                            ((uint64_t)dd[2] | ((uint64_t)dd[3] << 32)) &
                            ~0x3full;
                        cacheInvalidateRange((void *)((uintptr_t)dd & ~(uintptr_t)63), 64);
                        // Clamp to either mapping (desc BO or heap arena
                        // inside the same BO).
                        const bool inDesc = vaPtr >= vaDesc &&
                            vaPtr + 32 <= vaDesc + kTriDescBytes;
                        if (inDesc) {
                            const uint32_t *vpk =
                                (const uint32_t *)(dCpu + (vaPtr - vaDesc));
                            cacheInvalidateRange((void *)((uintptr_t)vpk & ~(uintptr_t)63), 64);
                            note("E5MALIS DUMP vpack(dcdw2)=%08x %08x %08x %08x %08x %08x %08x %08x",
                                 vpk[0], vpk[1], vpk[2], vpk[3],
                                 vpk[4], vpk[5], vpk[6], vpk[7]);
                        } else {
                            note("E5MALIS DUMP vpack: dcdw2=%llx out of desc BO",
                                 (unsigned long long)vaPtr);
                        }
                    }
                    const uint64_t bottom =
                        (uint64_t)hpw[4] | ((uint64_t)hpw[5] << 32);
                    if (bottom >= vaDesc && bottom + 32 <= vaDesc + kTriDescBytes) {
                        const uint32_t *hb =
                            (const uint32_t *)(dCpu + (bottom - vaDesc));
                        note("E5MALIS DUMP heapBottom[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x",
                             hb[0], hb[1], hb[2], hb[3],
                             hb[4], hb[5], hb[6], hb[7]);
                    }
                }
            }
            if (!pass) break;
        }
        allOk = passedBatches == repeatsPerVariant * 2 && v0Pass && v1Pass;
        note("E5MALIS %s REUSE %s batches=%u/%u repeats_per_variant=%u", tag,
             passedBatches == repeatsPerVariant * 2 ? "PASS" : "FAIL",
             passedBatches, repeatsPerVariant * 2, repeatsPerVariant);
        note("E5MALIS %s VERDICT fullscreen=%s triangle=%s", tag,
             v0Pass ? "PASS" : "FAIL",
             !v1Run ? "SKIPPED" : (v1Pass ? "PASS" : "FAIL"));
    } while (false);

    if (golden) IOFree(golden, kClearFbBytes);
    if (hFb) {
        E5MaliUnmapBo um = {};
        um.handle = hFb;
        um.va = vaFb;
        um.length = kClearFbBytes;
        const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
        E5MaliDestroyBo db = {};
        db.handle = hFb;
        const IOReturn r2 = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
        allOk = allOk && r1 == kIOReturnSuccess && r2 == kIOReturnSuccess;
        note("E5MALIS %s TEARDOWN fb %s unmap=%x destroy=%x", tag,
             (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? "PASS" : "FAIL",
             (unsigned)r1, (unsigned)r2);
    }
    if (hDesc) {
        E5MaliUnmapBo um = {};
        um.handle = hDesc;
        um.va = vaDesc;
        um.length = kTriDescBytes;
        const IOReturn r1 = selfCall(kSelUnmapBo, &um, sizeof(um), nullptr, 0, nullptr);
        E5MaliDestroyBo db = {};
        db.handle = hDesc;
        const IOReturn r2 = selfCall(kSelDestroyBo, &db, sizeof(db), nullptr, 0, nullptr);
        allOk = allOk && r1 == kIOReturnSuccess && r2 == kIOReturnSuccess;
        note("E5MALIS %s TEARDOWN desc %s unmap=%x destroy=%x", tag,
             (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess) ? "PASS" : "FAIL",
             (unsigned)r1, (unsigned)r2);
    }
    if (scanoutMap) scanoutMap->release();
    const bool released = liveBoBytes_ == initialBoBytes && ctx_->mappedBytes == initialMappedBytes;
    allOk = allOk && released;
    note("E5MALIS %s RELEASE %s bo=%llu mapped=%llu", tag, released ? "PASS" : "FAIL",
         (unsigned long long)liveBoBytes_, (unsigned long long)ctx_->mappedBytes);
    if (scanout) {
        E5MaliStats st = {};
        const IOReturn rc = selfCall(kSelStats, nullptr, 0, &st, sizeof(st), nullptr);
        const bool statsOk = rc == kIOReturnSuccess &&
            st.submitted - initialStats.submitted == repeatsPerVariant * 4u &&
            st.completed - initialStats.completed == repeatsPerVariant * 4u &&
            !st.faults && !st.timedOut && !st.resets && !st.mmuFaults &&
            !st.cancelled && !st.activeAtoms && !st.mappedBytes && !st.liveBoBytes;
        allOk = allOk && statsOk;
        note("E5MALIS SCAN5 STATS %s sub=%llu comp=%llu fault=%llu tmo=%llu reset=%llu mmu=%llu active=%u mapped=%u bo=%u",
             statsOk ? "PASS" : "FAIL", (unsigned long long)st.submitted,
             (unsigned long long)st.completed, (unsigned long long)st.faults,
             (unsigned long long)st.timedOut, (unsigned long long)st.resets,
             (unsigned long long)st.mmuFaults, st.activeAtoms, st.mappedBytes, st.liveBoBytes);
    }
    return allOk;
}

// ---------------------------------------------------------------------------
// Service entry points
// ---------------------------------------------------------------------------
// Delayed in-kernel self-test: a plain kernel thread (NOT the workloop, so
// the blocking WaitAtom poll never starves IRQ processing) sleeps 30 s to
// clear the boot peak, then drives an in-kernel E5MaliG1UserClient through
// the board-test-plan §2/§3 sequences. Equivalent to the thread_call(30 s)
// suggestion; a sleeping thread needs no extra event source.
static void e5MaliG1SelftestThread(void *ctx, wait_result_t) {
    E5MaliG1 *d = static_cast<E5MaliG1 *>(ctx);
    IOSleep(30000);
    if (!e5_mali_g1_ready) {
        note("E5MALIS SKIPPED driver not ready after delay");
        d->selftestFinished();
        thread_terminate(current_thread());
        return;   // not reached
    }
    E5MaliG1UserClient *uc = E5MaliG1UserClient::withTask(kernel_task, d);
    if (!uc) {
        note("E5MALIS FAIL in-kernel client creation");
        d->selftestFinished();
        thread_terminate(current_thread());
        return;   // not reached
    }
    uc->runSelftest(E5MaliG1::bootSelftest());
    // Deterministic teardown of the in-kernel client: retire/unmap/free the
    // AS synchronously (the same quiesce a process exit triggers), then the
    // IOKit detach path.
    uc->quiesceAndRelease();
    uc->stop(d);
    uc->detach(d);
    uc->release();
    note("E5MALIS done");
    d->selftestFinished();
    thread_terminate(current_thread());
}

void E5MaliG1::scheduleSelftest() {
    if (bootSelftest_ == 0) return;
    if (selftestActive_) return;
    thread_t thread = nullptr;
    const kern_return_t rc = kernel_thread_start(&e5MaliG1SelftestThread, this, &thread);
    if (rc != 0) {   // KERN_SUCCESS
        note("E5MALIS FAIL thread spawn rc=%d — selftest disabled", (int)rc);
        return;
    }
    selftestActive_ = 1;
    if (thread) thread_deallocate(thread);   // thread holds its own reference
    note("E5MALIS armed mask=%u delay=30s (bit0=ABI/MMU/BO bit1=minimal-atom bit2=offscreen-clear bit3=triangle bit4=imported-scanout)",
         bootSelftest_);
}

IOReturn E5MaliG1::newUserClient(task_t task, void *, UInt32 type,
                                 IOUserClient **client) {
    if (type != 0) return kIOReturnUnsupported;
    E5MaliG1UserClient *uc = E5MaliG1UserClient::withTask(task, this);
    if (!uc) return kIOReturnNoMemory;
    *client = uc;
    return kIOReturnSuccess;
}

// Called from NativeSd::start() after the display service is up, next to
// today's e5_mali_probe() call, when e5-mali-g1=1 is present.
extern "C" int e5_mali_g1_start(void *providerService) {
    readBootArgs();
    if (!E5MaliG1::bootEnable_) return kIOReturnNotPermitted;
    if (e5_mali_g1_ready) return kIOReturnSuccess;
    IOService *provider = static_cast<IOService *>(providerService);
    E5MaliG1 *driver = OSTypeAlloc(E5MaliG1);
    if (!driver || !driver->init()) {
        if (driver) driver->release();
        return kIOReturnNoMemory;
    }
    if (!driver->attach(provider)) {
        driver->release();
        return kIOReturnNoResources;
    }
    if (!driver->start(provider)) {
        driver->stop(provider);
        driver->detach(provider);
        driver->release();
        note("E5MALI1 driver start failed at stage %u — display/input untouched",
             e5_mali_g1_stage);
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

#include "E5MetalDiscovery.inc"
static void e5_publish_metal(IOService *gpu) { E5MetalDiscovery::publish(gpu); }
