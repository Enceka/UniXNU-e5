// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// E5MaliG1.hpp — minimal Mali-G57 (Valhall v9, Job Manager, NOT CSF) kernel
// driver for the Rongyue E5 (UNISOC UMS9621 / qogirn6lite) XNU 21H461 port.
//
// Scope (G1 of docs/display-touch-gpu-plan.md, requirements at ~line 409):
//   platform power/clock lifecycle (vendor qogirn6l sequence),
//   shared GIC SPI146/INTID178 JOB+MMU+GPU dispatch,
//   AArch64-4K 4 KiB GPU MMU with ASID pool,
//   BO alloc/map/remap/unmap with DMA-safe release,
//   JM submit / wait / completion / fault / soft-stop / reset rollback,
//   per-task UserClient with handle scoping and exit cleanup.
//
// Every hardware number is annotated with its source; see
// ../audit-report.md for the full table. Unverified assumptions are marked
// TODO/UNVERIFIED inline.
//
// NOT in scope (by plan): CSF firmware queues, DVFS scaling, QoS, SIP
// voltage tables, perf counters, Prime/IOSurface sharing.
#pragma once

#include <stdint.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>

#include "E5MaliG1Mmu.hpp"

class E5MaliG1;
class E5MaliG1UserClient;
class E5MaliG1Context;

// ===========================================================================
// Hardware map. Sources: F = live-fdt.dts GPU node (1664-1696),
// V = kernel_sprd_ums9158@63b80110 kbase r41p0, L = linux-lts-e5@6b355f72
// panfrost (+ panfrost_sprd.c, device-verified by the e5-linux project).
// ===========================================================================
namespace E5MaliG1Regs {

// Platform windows (F: address-cells map; E5Mali.cpp probe R198 mapping order)
constexpr uint64_t kAonApbBase  = 0x64900000;   // aon gate; SET mirror +0x1000
constexpr uint64_t kAonApbLen   = 0x3000;
constexpr uint64_t kPmuBase     = 0x64910000;   // PMU APB syscon (F phandle 0x48)
constexpr uint64_t kPmuLen      = 0x3000;
constexpr uint64_t kGpuApbBase  = 0x23000000;   // GPU APB syscon (F phandle 0x58)
constexpr uint64_t kGpuApbLen   = 0x3000;
constexpr uint64_t kDvfsBase    = 0x23014000;   // GPU DVFS APB (F phandle 0x59)
constexpr uint64_t kDvfsLen     = 0x3000;
constexpr uint64_t kGpuMmioBase = 0x23140000;   // Mali cores (F:1665)
constexpr uint64_t kGpuMmioLen  = 0x4000;
constexpr uint64_t kAnlgBase    = 0x64940000;   // ANLG_PHY syscon (F phandle 0x5a)
constexpr uint64_t kAnlgLen     = 0x3000;

// AON gate bits (V ums9621-clk.c:1100-1103: mm_eb BIT(9), gpu_eb BIT(11),
// sc_offset 0x1000; dt-binding index 8 = CLK_GPU_EB, NOT the bit).
constexpr uint32_t kAonGateSet    = 0x1000;      // SET mirror of word 0
constexpr uint32_t kAonGateClear  = 0x2000;
constexpr uint32_t kAonGateGpuEb  = (1u << 11);
constexpr uint32_t kAonGateMmEb   = (1u << 9);   // left to the display domain

// DVFS ownership handoff (F:1669-1670; V qogirn6l:244-245)
constexpr uint32_t kAnlgTopDvfsCfg      = 0xd84;   // clear bit 5
constexpr uint32_t kAnlgTopDvfsCfgMask  = 0x20;
constexpr uint32_t kAnlgSwDvfsCtrl      = 0x4b8;   // clear bit 0
constexpr uint32_t kAnlgSwDvfsCtrlMask  = 0x1;

// PMU registers (F:1678-1684; V qogirn6l mali_power_on/off 535-594)
constexpr uint32_t kPmuGpllFrcOff   = 0xa20;   // bit 1: 1 = force off
constexpr uint32_t kPmuGpllFrcOn    = 0xa20;   // bit 0: 1 = force on
constexpr uint32_t kPmuForceShut    = 0x2c4;   // bit 25: 1 = force shutdown
constexpr uint32_t kPmuTopState     = 0x500;   // bits 20:16 (mask 0x1f0000): 0 = up
constexpr uint32_t kPmuTopStateMask = 0x1f0000;
constexpr uint32_t kPmuGpuSoftRst   = 0xb98;   // bit 2 (V ums9621-clk.c:139)
// PMIC (via ADI, NativeSd e5_pmic_*):
constexpr uint32_t kPmicDcdcGpuPd   = 0x201c;  // bit 3: 0 = enabled (F:1684)

// GPU APB (F:1675 + V qogirn6l-regs.h)
constexpr uint32_t kGpuApbCoreEb      = 0x0;    // bit 2 (mask 0x4), plain RMW
constexpr uint32_t kGpuApbCoreEbMask  = 0x4;
constexpr uint32_t kGpuApbBridge      = 0x24;   // ASYBC_BRIDGE_TOP_W; bit 1 = idle
constexpr uint32_t kGpuApbBridgeIdle  = 0x2;
// R198 diagnostic fallback writes (ums512 lineage, UNVERIFIED for ums9621):
constexpr uint32_t kGpuApbLegacyClk0 = 0x4, kGpuApbLegacyClk1 = 0x8,
                   kGpuApbLegacyClk2 = 0xc;

// GPU DVFS APB (F via V qogirn6l-regs.h:18-20)
constexpr uint32_t kDvfsFreqUpd      = 0x9ac;  // mask 0x3 -> 1 (HDSK_EN)
constexpr uint32_t kDvfsFreqUpdMask  = 0x3;
constexpr uint32_t kDvfsIndexCfg     = 0x794;  // mask 0x7 -> 3 (384 MHz)
constexpr uint32_t kDvfsIndexMask    = 0x7;
constexpr uint32_t kDvfsIndexDefault = 3;

// ---- Mali core registers (L panfrost_regs.h == V kbase JM regmap) -------
constexpr uint32_t GPU_ID            = 0x000;
constexpr uint32_t GPU_L2_FEATURES   = 0x004;
constexpr uint32_t GPU_CORE_FEATURES = 0x008;
constexpr uint32_t GPU_TILER_FEATURES= 0x00c;
constexpr uint32_t GPU_MEM_FEATURES  = 0x010;
constexpr uint32_t GPU_MMU_FEATURES  = 0x014;
constexpr uint32_t GPU_AS_PRESENT    = 0x018;
constexpr uint32_t GPU_JS_PRESENT    = 0x01c;
constexpr uint32_t GPU_INT_RAWSTAT   = 0x020;
constexpr uint32_t GPU_INT_CLEAR     = 0x024;
constexpr uint32_t GPU_INT_MASK      = 0x028;
constexpr uint32_t GPU_INT_STAT      = 0x02c;
constexpr uint32_t GPU_CMD           = 0x030;
constexpr uint32_t GPU_STATUS        = 0x034;
constexpr uint32_t GPU_FAULT_STATUS  = 0x03c;
constexpr uint32_t GPU_FAULTADDR_LO  = 0x040;
constexpr uint32_t GPU_FAULTADDR_HI  = 0x044;
constexpr uint32_t GPU_JS_FEATURES   = 0x0c0;   // + 4*n
constexpr uint32_t GPU_SHADER_PRESENT_LO = 0x100;
constexpr uint32_t GPU_SHADER_PRESENT_HI = 0x104;
constexpr uint32_t GPU_TILER_PRESENT_LO  = 0x110;
constexpr uint32_t GPU_L2_PRESENT_LO     = 0x120;
constexpr uint32_t GPU_SHADER_READY_LO   = 0x140;
constexpr uint32_t GPU_TILER_READY_LO    = 0x150;
constexpr uint32_t GPU_L2_READY_LO       = 0x160;
constexpr uint32_t GPU_SHADER_PWRON_LO   = 0x180;   // WO
constexpr uint32_t GPU_TILER_PWRON_LO    = 0x190;   // WO
constexpr uint32_t GPU_L2_PWRON_LO       = 0x1a0;   // WO

constexpr uint32_t GPU_IRQ_FAULT             = (1u << 0);
constexpr uint32_t GPU_IRQ_MULTIPLE_FAULT    = (1u << 7);
constexpr uint32_t GPU_IRQ_RESET_COMPLETED   = (1u << 8);
constexpr uint32_t GPU_CMD_SOFT_RESET        = 0x01;
constexpr uint32_t GPU_CMD_HARD_RESET        = 0x02;

// Job control block
constexpr uint32_t JOB_INT_RAWSTAT  = 0x1000;
constexpr uint32_t JOB_INT_CLEAR    = 0x1004;
constexpr uint32_t JOB_INT_MASK     = 0x1008;
constexpr uint32_t JOB_INT_STAT     = 0x100c;
constexpr uint32_t JOB_INT_JS_STATE = 0x1010;
constexpr uint32_t JS_BASE          = 0x1800;
constexpr uint32_t JS_SLOT_STRIDE   = 0x80;
constexpr uint32_t JS_HEAD_NEXT_LO  = 0x40;
constexpr uint32_t JS_HEAD_NEXT_HI  = 0x44;
constexpr uint32_t JS_AFFINITY_NEXT_LO = 0x50;
constexpr uint32_t JS_AFFINITY_NEXT_HI = 0x54;
constexpr uint32_t JS_CONFIG_NEXT   = 0x58;
constexpr uint32_t JS_COMMAND_NEXT  = 0x60;
constexpr uint32_t JS_HEAD_LO       = 0x00;
constexpr uint32_t JS_TAIL_LO       = 0x08;
constexpr uint32_t JS_TAIL_HI       = 0x0c;
constexpr uint32_t JS_COMMAND       = 0x20;
constexpr uint32_t JS_STATUS        = 0x24;
constexpr uint32_t JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE = (3u << 8);
constexpr uint32_t JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   = (3u << 12);
constexpr uint32_t JS_CONFIG_JOB_CHAIN_FLAG               = (1u << 11);
static inline constexpr uint32_t jsConfigThreadPri(unsigned n) { return n << 16; }
constexpr uint32_t JS_COMMAND_NOP        = 0x00;
constexpr uint32_t JS_COMMAND_START      = 0x01;
constexpr uint32_t JS_COMMAND_SOFT_STOP  = 0x02;

// MMU block
constexpr uint32_t MMU_INT_RAWSTAT = 0x2000;
constexpr uint32_t MMU_INT_CLEAR   = 0x2004;
constexpr uint32_t MMU_INT_MASK    = 0x2008;
constexpr uint32_t MMU_INT_STAT    = 0x200c;
constexpr uint32_t MMU_BASE        = 0x2400;
constexpr uint32_t MMU_AS_SHIFT    = 0x6;      // 0x40 bytes per AS
constexpr uint32_t AS_TRANSTAB_LO  = 0x00;
constexpr uint32_t AS_TRANSTAB_HI  = 0x04;
constexpr uint32_t AS_MEMATTR_LO   = 0x08;
constexpr uint32_t AS_MEMATTR_HI   = 0x0c;
constexpr uint32_t AS_LOCKADDR_LO  = 0x10;
constexpr uint32_t AS_LOCKADDR_HI  = 0x14;
constexpr uint32_t AS_COMMAND      = 0x18;
constexpr uint32_t AS_FAULTSTATUS  = 0x1c;
constexpr uint32_t AS_FAULTADDR_LO = 0x20;
constexpr uint32_t AS_FAULTADDR_HI = 0x24;
constexpr uint32_t AS_STATUS       = 0x28;
constexpr uint32_t AS_TRANSCFG_LO  = 0x30;
constexpr uint32_t AS_TRANSCFG_HI  = 0x34;
constexpr uint32_t AS_COMMAND_UPDATE    = 0x1;
constexpr uint32_t AS_COMMAND_LOCK      = 0x2;
constexpr uint32_t AS_COMMAND_FLUSH_PT  = 0x4;
constexpr uint32_t AS_COMMAND_FLUSH_MEM = 0x5;
constexpr uint32_t AS_STATUS_AS_ACTIVE  = 0x01;
constexpr uint32_t AS_TRANSCFG_ADRMODE_AARCH64_4K = (6u << 0);
constexpr uint32_t AS_TRANSCFG_ADRMODE_UNMAPPED    = (1u << 0);
constexpr uint32_t AS_TRANSCFG_PTW_MEMATTR_WB      = (2u << 24);
constexpr uint32_t AS_TRANSCFG_PTW_SH_OS           = (2u << 28);
constexpr uint32_t AS_TRANSCFG_PTW_RA              = (1u << 30);
// AS_MEMATTR lanes (V mali_kbase_mmu_jm.c:43-55):
//   idx0 0x88 impl-def WB | idx1 0x8F cache-all | idx2 0x8D write-alloc
//   idx3 0x88 | idx4 0x8D | idx5 0x4C non-cacheable
constexpr uint64_t kAsMemattr =
    (0x88ull << (8 * 0)) | (0x8full << (8 * 1)) | (0x8dull << (8 * 2)) |
    (0x88ull << (8 * 3)) | (0x8dull << (8 * 4)) | (0x4cull << (8 * 5));

// Identity expectations (R198 + Android sysfs evidence)
constexpr uint32_t kExpectedGpuIdFamily = 0x9091;   // >> 16 of GPU_ID
constexpr uint32_t kExpectedShaderMask  = 0x5;      // sparse: cores 0 and 2
// TODO UNVERIFIED: GPU_MMU_FEATURES / AS_PRESENT / JS_PRESENT have never been
// read on-target at the correct offsets (R198's CAP line mislabelled them);
// the driver reads them at start and sanity-checks, but their exact values
// are a first-flash observation item (board-test-plan.md step 2).

constexpr uint32_t kMaxSlots = 3;      // BASE_JM_MAX_NR_SLOTS (V defs.h:92)
constexpr uint32_t kMaxAs    = 16;     // BASE_MAX_NR_AS (V defs.h:99)
constexpr int32_t  kGicIntid = 178;    // FDT SPI146 + 32; shared JOB/MMU/GPU

}  // namespace E5MaliG1Regs

using E5MaliG1Regs::kMaxSlots;
using E5MaliG1Regs::kMaxAs;
using E5MaliG1Regs::kGicIntid;

// ===========================================================================
// UserClient ABI (see ../userclient-abi.md). Plain C layouts so the
// userspace harness can mirror them byte-for-byte; every struct size is
// validated exactly by the kernel.
// ===========================================================================
#include "E5MaliG1Abi.h"

// Console/diagnostic counters (extern C so the serial echo can read them).
extern "C" {
extern volatile uint32_t e5_mali_g1_stage;      // last lifecycle stage
extern volatile uint32_t e5_mali_g1_status;    // 1 powered | 2 hw-init | 4 irq
extern volatile uint32_t e5_mali_g1_ready;     // 1 = service published
extern volatile uint64_t e5_mali_g1_gpu_id, e5_mali_g1_shader, e5_mali_g1_as_present;
extern volatile uint64_t e5_mali_g1_js_present, e5_mali_g1_mmu_features;
extern volatile uint64_t e5_mali_g1_submitted, e5_mali_g1_completed, e5_mali_g1_faults;
extern volatile uint64_t e5_mali_g1_timeouts, e5_mali_g1_resets, e5_mali_g1_mmu_faults;
extern volatile uint64_t e5_mali_g1_last_fault_code, e5_mali_g1_last_fault_addr;
// Entry the storage driver calls after the display service is up, next to
// today's e5_mali_probe() call (NativeSd.cpp). providerService is the
// IOService* to attach to. Returns kIOReturnSuccess when the E5MaliG1
// service reached registerService.
int e5_mali_g1_start(void *providerService);
bool e5_mali_g1_scanout_selected(void);
}

// ===========================================================================
// Atom — one submitted job chain (fixed pool, no hot-path allocation).
// ===========================================================================
class E5MaliG1Atom {
public:
    uint64_t id = 0;
    uint64_t jc = 0;
    uint8_t slot = 0;
    uint8_t state = kAtomFree;
    uint32_t hwStatus = 0;               // JS_STATUS exception code
    uint64_t udata[2] = {};
    E5MaliG1UserClient *owner = nullptr; // completion/wake bookkeeping
    E5MaliG1Context *ctx = nullptr;      // address space the chain runs in
    uint32_t *pinnedBo = nullptr; // service-owned table; keeps OSObject below its 16KiB limit
    uint32_t pinnedCount = 0;
    uint32_t submittedTickMs = 0;
};

// ===========================================================================
// Per-client GPU context: page tables + VA window + AS number.
// Table pages are 4 KiB IOBufferMemoryDescriptor allocations mapped once
// for their whole lifetime (kbase pgd semantics).
// ===========================================================================
class E5MaliG1Context {
public:
    E5MaliG1Mmu::PageTable table;
    E5MaliG1Mmu::VaAllocator va;
    int32_t as = -1;
    bool asFaulted = false;
    E5MaliG1UserClient *client = nullptr;

    struct Tp {
        uint64_t phys = 0;
        void *cpu = nullptr;
        IOBufferMemoryDescriptor *md = nullptr;
    };
    static constexpr uint32_t kMaxTablePages = 1024;   // 4 MiB of tables
    Tp tp[kMaxTablePages];
    uint32_t tpCount = 0, tpFreed = 0;
    uint64_t mappedBytes = 0;

    static uint64_t tpAlloc(void *ctx);
    static void tpFree(void *ctx, uint64_t phys);
    static void *tpCpu(void *ctx, uint64_t phys);
    bool init(uint64_t vaBase, uint64_t vaSize);
    void teardown();                    // frees every remaining table page
};

// ===========================================================================
// Driver
// ===========================================================================
class E5MaliG1 : public IOService {
    OSDeclareDefaultStructors(E5MaliG1)
public:
    static constexpr uint32_t kSlotQueue = 8;    // software wait depth/slot
    static constexpr uint32_t kAtomPool = 64;
    static constexpr uint32_t kJobTimeoutMs = 500;   // panfrost JOB_TIMEOUT_MS

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn newUserClient(task_t task, void *securityID, UInt32 type,
                           IOUserClient **client) override;

    // Register access (little-endian, 32-bit). All writes complete with a
    // full dsb so ordering vs. the GPU is explicit.
    uint32_t read32(uint32_t off) const {
        return *(volatile uint32_t *)(mmioVaddr_ + off);
    }
    void write32(uint32_t off, uint32_t v) {
        *(volatile uint32_t *)(mmioVaddr_ + off) = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint64_t read64(uint32_t lo) const {
        return (uint64_t)read32(lo) | ((uint64_t)read32(lo + 4) << 32);
    }
    void write64(uint32_t lo, uint64_t v) {
        write32(lo, (uint32_t)v);
        write32(lo + 4, (uint32_t)(v >> 32));
    }

    // Engine internals (state_ held unless noted).
    void handleJobIrq();                  // workloop
    void handleMmuIrq(uint32_t status);
    void handleGpuIrq(uint32_t status);
    void resetEngine(const char *why);    // soft-stop + GPU reset + requeue
    bool slotKick(uint32_t slot);
    void completeAtom(E5MaliG1Atom *a, uint8_t state, uint32_t hwStatus);
    void enqueueFront(E5MaliG1Atom *a, uint32_t slot);
    E5MaliG1Atom *slotDequeue(uint32_t slot);
    E5MaliG1Atom *atomPool() { return atomPool_; }
    uint32_t faultyAsMask() const;

    int32_t asAcquire(E5MaliG1Context *ctx);
    void asRelease(E5MaliG1Context *ctx);
    void asReprogram(int32_t as);
    bool asWaitReady(int32_t as, uint32_t timeoutUs);
    void asCommand(int32_t as, uint32_t cmd);
    void asFlushRange(int32_t as, uint64_t va, uint64_t len);

    // identity accessors for the UserClient
    uint32_t gpuId() const { return gpuId_; }
    uint64_t shaderPresent() const { return shaderPresent_; }
    uint64_t l2Present() const { return l2Present_; }
    uint64_t tilerPresent() const { return tilerPresent_; }
    uint32_t mmuFeatures() const { return mmuFeatures_; }
    uint32_t asPresent() const { return asPresent_; }
    uint32_t jsPresent() const { return jsPresent_; }
    uint32_t numSlots() const { return numSlots_; }
    uint32_t numAs() const { return numAs_; }
    IOLock *stateLock() const { return state_; }
    bool resetPending() const { return resetPending_ != 0; }
    uint64_t completionSeq() const { return completionSeq_; }
    bool slotUsable(uint32_t slot) const {
        return slot < kMaxSlots && (jsPresent_ & (1u << slot)) != 0 &&
               (bootSlotMask_ & (1u << slot)) != 0;
    }
    static uint32_t bootVaMb() { return bootVaMb_; }
    static uint32_t bootImport() { return bootImport_; }
    static uint32_t bootSelftest() { return bootSelftest_; }
    bool selftestActive() const { return selftestActive_ != 0; }
    void selftestFinished() { selftestActive_ = 0; }
    // Spawn the delayed in-kernel self-test thread (no-op when the
    // e5-mali-g1-selftest boot argument is absent/zero). Board-test-plan
    // §2/§3 sequences; all serial output is E5MALIS-prefixed.
    void scheduleSelftest();
    // Boot-arg knobs (read once; file-scope readBootArgs writes these).
    static uint32_t bootEnable_;     // e5-mali-g1=1
    static uint32_t bootLegacyClk_;  // e5-mali-g1-clk-legacy=1 (R198 writes)
    static uint32_t bootSkipReset_;  // e5-mali-g1-reset=0 (skip PMU 0xb98)
    static uint32_t bootImport_;     // e5-mali-g1-import=1 (ImportFixed)
    static uint32_t bootVaMb_;       // e5-mali-g1-va-mb=N
    static uint32_t bootSlotMask_;   // e5-mali-g1-slots=N
    static uint32_t bootSelftest_;   // e5-mali-g1-selftest=N (bit0 §2, bit1 §3)

private:
    bool mapAll();
    bool platformPowerOn();
    void platformPowerOff();
    bool gpuInit();
    bool attachIrq();
    void detachIrq();
    void fail();
    static void irqPrimary(void *ctx);           // interrupt context
    static void workAction(OSObject *owner, IOInterruptEventSource *, int);
    static void timerAction(OSObject *owner, IOTimerEventSource *);
    void watchdogTick();
    IOMemoryMap *map(uint64_t pa, uint32_t len);
    void aonSet(uint32_t bits);
    void aonClear(uint32_t bits);
    uint32_t pmuRmw(uint32_t off, uint32_t mask, uint32_t val);
    uint32_t sysconRmw(volatile uint32_t *base, uint32_t off, uint32_t mask,
                       uint32_t val);

    // platform windows
    IOMemoryMap *aonMap_ = nullptr, *pmuMap_ = nullptr, *apbMap_ = nullptr,
                *dvfsMap_ = nullptr, *mmioMap_ = nullptr, *anlgMap_ = nullptr;
    volatile uint32_t *aon_ = nullptr, *pmu_ = nullptr, *apb_ = nullptr,
                      *dvfs_ = nullptr, *anlg_ = nullptr;
    uintptr_t mmioVaddr_ = 0;

    // identity
    uint32_t gpuId_ = 0, mmuFeatures_ = 0, asPresent_ = 0, jsPresent_ = 0;
    uint64_t shaderPresent_ = 0, l2Present_ = 0, tilerPresent_ = 0;
    uint32_t numSlots_ = 0, numAs_ = 0;

    // engine
    IOWorkLoop *workloop_ = nullptr;
    IOInterruptEventSource *irqSource_ = nullptr;
    IOTimerEventSource *timer_ = nullptr;
    bool irqAttached_ = false;
    IOLock *state_ = nullptr;
    IOSimpleLock *hw_ = nullptr;
    uint32_t slotChainFlag_ = 0;
    uint64_t completionSeq_ = 0;
    uint32_t tickMs_ = 0;
    volatile uint32_t pendingJob_ = 0, pendingMmu_ = 0, pendingGpu_ = 0;
    volatile uint32_t resetPending_ = 0;
    volatile uint32_t selftestActive_ = 0;   // stop() waits for this to clear

    struct AsSlot {
        E5MaliG1Context *ctx = nullptr;
    } as_[kMaxAs];

    struct SlotState {
        E5MaliG1Atom *ring[2] = {nullptr, nullptr};   // hardware 2-deep view
        uint32_t queued = 0;
        E5MaliG1Atom *wait[kSlotQueue] = {};
    } slots_[kMaxSlots];

    E5MaliG1Atom atomPool_[kAtomPool];
    uint32_t *pinStorage_ = nullptr;

    friend class E5MaliG1UserClient;
};
