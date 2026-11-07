// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeEmmc v2: XNU-side port of the UEFI-validated E5 eMMC probe
// (E5BootMenu EmmcProbe; acceptance runs dumps/uefi-report-193046.txt and
// uefi-report-193701.txt passed tools/e5-emmc-check.py on hardware, with
// SDMA+ADMA2 reads whose CRC32s match the Android dd reference).
//
// The k126-k221 experiment ladder is retired.  Its data-phase failures trace
// to register-layout self-inconsistency, all of which the UEFI path avoids:
//   - V4EN cleared in HC2 while still writing the v4 block count at 0x00 and
//     the system address at 0x58/0x5C (mixed layouts; with V4EN=0 the count
//     register 0x06 stayed 0 -> instant fake TRANSFER_COMPLETE, no data);
//   - TRANSFER_MODE without the READ bit / command flags without DATA;
//   - a 400 kHz divider computed from an assumed 26 MHz base clock;
//   - a CMD-line software reset before EVERY command;
//   - clearing the DLL delays LK had tuned; reprogramming the apahb/apclk
//     clock tree; PMIC power cycling.  None of that happens here.
//
// This driver only ever:
//   - maps the SDHCI host window at 0x22200000 (sprd,sdhci-r11);
//   - keeps Unisoc bit3 (RST_n) set across line resets (0x2F |= 0x06);
//   - keeps HC2=0x3000 (V4EN + 64-bit addressing) for all data transfers;
//   - uses the maximum divider (0xFFC1/0xFFC5) for the identification clock;
//   - breaks data waits on TRANSFER_COMPLETE or error, never COMMAND_COMPLETE;
//   - maintains cache ownership: fill pattern + clean + invalidate before the
//     transfer, invalidate after, descriptors cleaned before issue.
//
// v2 scope (stage-2 step 1 of the bringup plan): from inside XNU, re-run the
// warm-inherit read, the legacy re-identification, EXT_CSD, and LBA0/LBA1
// SDMA/ADMA reads with alternating rereads, logging CRC32s for comparison
// with the Android reference (LBA0 31575900, LBA1 76AEFCF6).  No IOMedia, no
// block requests yet - those come after this round is accepted on device.
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "TransferStatus.h"
#include "BootLog.h"
#include "SystemOSWindow.h"
#include "E5SelfTestTables.h"

#define E5_EMMC_BASE 0x22200000ULL
#define E5_EMMC_LEN  0x400ULL

// SDHCI register offsets used by the validated sequence.
#define SDHCI_DMA_ADDR          0x00   // v3: sysaddr; v4: 32-bit block count
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06   // v3 only; read-only 16-bit on sprd
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE_CMD 0x0C   // (cmdword << 16) | TM in 0x0C/0x0E
#define SDHCI_RESPONSE          0x10
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28   // bits[4:3]: 00 SDMA, 10 ADMA2
#define SDHCI_TIMEOUT_CONTROL   0x2E
#define SDHCI_SOFTWARE_RESET    0x2F   // bit3 = RST_n on Unisoc, keep set
#define SDHCI_INT_STATUS        0x30   // W1C; errors mirrored in high half
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_INT_SIGNAL_ENABLE 0x38
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_SYSADDR_LOW       0x58   // v4: system address / desc address
#define SDHCI_SYSADDR_HIGH      0x5C

#define INT_CMD_COMPLETE  0x00000001u
#define INT_XFER_COMPLETE 0x00000002u
#define PSTATE_DAT_INHIBIT 0x00000002u

// Kernel printf does not reach the ram tee from IOKit context; the tee
// printer e5_panic_print() (e5-ram-console-tee.patch) does.
extern "C" void e5_panic_print(const char *s);
// r251: uncached device-map window over physical memory (the same call the
// prz tee uses). Used for the panic scratch at 0xC6000000.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
class NativeEmmc; // r247: the panic-flush host pointer lives before the class
static NativeEmmc *sJournalHost = nullptr;
// r251: panic scratch pointers (0xC6000000/0xC6002000) - declared here so
// the journal worker heartbeat can touch them; armed in start().
static volatile uint32_t *sPanicScratch = nullptr;
static volatile uint32_t *sPanicTrace = nullptr;

static void elog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  e5_panic_print(buf);
}

// Window I/O telemetry (RD=ops:blocks WR=ops:blocks:failures RV=unstable).
static uint64_t e5_win_rd_blocks, e5_win_rd_ops;
static uint64_t e5_win_wr_blocks, e5_win_wr_ops, e5_win_wr_bad;
static uint64_t e5_win_unstable;
static uint64_t e5_win_w25_ops, e5_win_w25_verified, e5_win_w25_ticks;
// Enabled by this board's kernel config; that source-file define is not
// present in the IOKit archive's compile record.
extern "C" uint64_t cs_native4k_read_ticks, cs_native4k_hash_ticks, cs_native4k_bytes;
extern "C" unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count;
extern "C" unsigned int vm_page_wire_count, vm_page_pageable_internal_count;
extern "C" unsigned int vm_page_pageable_external_count, vm_page_free_wanted;

// r275: sampled without taking the host lock or issuing any media reads.
// These are cumulative submitted-request counters (512-byte blocks), not
// successful completions or a transactionally consistent tuple.
extern "C" void e5_emmc_runtime_dump(void) {
  elog("E5IO RD=%llu:%llu WR=%llu:%llu:%llu RV=%llu\n",
       (unsigned long long)__atomic_load_n(&e5_win_rd_ops, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_rd_blocks, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_wr_ops, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_wr_blocks, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_wr_bad, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_unstable, __ATOMIC_RELAXED));
  uint64_t readNs = 0, hashNs = 0;
  absolutetime_to_nanoseconds(__atomic_load_n(&cs_native4k_read_ticks, __ATOMIC_RELAXED), &readNs);
  absolutetime_to_nanoseconds(__atomic_load_n(&cs_native4k_hash_ticks, __ATOMIC_RELAXED), &hashNs);
  elog("E5CS4K bytes=%llu read_ms=%llu hash_ms=%llu\n",
       (unsigned long long)__atomic_load_n(&cs_native4k_bytes, __ATOMIC_RELAXED),
       (unsigned long long)(readNs / 1000000), (unsigned long long)(hashNs / 1000000));
  // Approximate lock-free VM census, in this kernel's 4 KiB pages.
  elog("E5VM free=%u active=%u inactive=%u wired=%u internal=%u external=%u wanted=%u\n",
       vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
       vm_page_wire_count, vm_page_pageable_internal_count,
       vm_page_pageable_external_count, vm_page_free_wanted);
  uint64_t writeNs = 0;
  absolutetime_to_nanoseconds(__atomic_load_n(&e5_win_w25_ticks, __ATOMIC_RELAXED), &writeNs);
  elog("E5W25 ops=%llu verified=%llu ms=%llu\n",
       (unsigned long long)__atomic_load_n(&e5_win_w25_ops, __ATOMIC_RELAXED),
       (unsigned long long)__atomic_load_n(&e5_win_w25_verified, __ATOMIC_RELAXED),
       (unsigned long long)(writeNs / 1000000));
}

// Report the executing context, not an inferred originating process: APFS
// may submit reads on a kernel worker. Bound trace volume during the scan.
class E5WindowReadTrace {
  uint64_t sequence = 0, start = 0, waiting = 0;
  uint64_t lba, blocks;
  int pid = 0;
  char name[32] = {};
public:
  bool enabled = false;
  IOReturn result = kIOReturnIOError;
  E5WindowReadTrace(uint64_t at, uint64_t count) : lba(at), blocks(count) {
    if (count < 512) return;
    static uint64_t samples;
    sequence = __atomic_add_fetch(&samples, 1ULL, __ATOMIC_RELAXED);
    enabled = sequence <= 4 || (sequence % 128) == 0;
    if (!enabled) return;
    start = mach_absolute_time();
    pid = proc_selfpid();
    proc_name(pid, name, sizeof(name));
    elog("E5READ begin id=%llu lba=%llu blocks=%llu ctx_pid=%d ctx=%s\n",
         (unsigned long long)sequence, (unsigned long long)lba,
         (unsigned long long)blocks, pid, name);
  }
  void waited(uint64_t since) { if (enabled) waiting += mach_absolute_time() - since; }
  ~E5WindowReadTrace() {
    if (!enabled) return;
    uint64_t ns = 0, waitNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    absolutetime_to_nanoseconds(waiting, &waitNs);
    elog("E5READ end id=%llu us=%llu lock_us=%llu ret=%x\n",
         (unsigned long long)sequence, (unsigned long long)(ns / 1000),
         (unsigned long long)(waitNs / 1000), (unsigned)result);
  }
};

// Console context never sleeps, allocates or performs storage I/O. A failed
// try-lock drops a byte instead of waiting behind an interrupted producer.
static uint8_t bootLogRing[65536];
static uint8_t bootLogRingLock;
static uint64_t bootLogHead, bootLogDropped;
extern "C" void e5_bootlog_putc(char c) {
  if (__atomic_test_and_set(&bootLogRingLock, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&bootLogDropped, 1, __ATOMIC_RELAXED); return;
  }
  bootLogRing[bootLogHead++ & (sizeof(bootLogRing) - 1)] = (uint8_t)c;
  __atomic_clear(&bootLogRingLock, __ATOMIC_RELEASE);
}

static unsigned bootLogCopy(uint64_t &cursor, uint8_t *bytes, uint64_t &stream, uint64_t &lost) {
  if (__atomic_test_and_set(&bootLogRingLock, __ATOMIC_ACQUIRE)) return 0;
  if (bootLogHead - cursor > sizeof(bootLogRing)) {
    lost += bootLogHead - cursor - sizeof(bootLogRing);
    cursor = bootLogHead - sizeof(bootLogRing);
  }
  stream = cursor;
  unsigned n = (unsigned)(bootLogHead - cursor);
  if (n > E5BootLog::recordPayload) n = E5BootLog::recordPayload;
  for (unsigned i = 0; i < n; ++i) bytes[i] = bootLogRing[(cursor + i) & (sizeof(bootLogRing) - 1)];
  cursor += n;
  __atomic_clear(&bootLogRingLock, __ATOMIC_RELEASE);
  return n;
}

class NativeEmmc : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeEmmc)
  volatile uint32_t *regs = nullptr;
  IOLock *ioLock = nullptr;
  IOBufferMemoryDescriptor *bmd = nullptr;
  IOMemoryMap *map = nullptr;
  IOMemoryDescriptor *md = nullptr;
  uint32_t *dmaD = nullptr;
  uint8_t *dmaD8 = nullptr;
  uint64_t dmaPhys = 0;
  // r259: window I/O (readMulti/writeSingle) uses the SECOND half of the DMA
  // page so journal record staging (first 4 KiB) and window transfers can
  // never alias — the r258 corruption served journal records as Data-window
  // content while every lock was held.
  uint8_t *winD8 = nullptr;
  uint32_t winPhys = 0;
  uint64_t winTraceSeq = 0;
  uint64_t jrPasses = 0;
  // r268: dedicated 64 KiB physically-contiguous bulk-read buffer — the LS
  // scan needs megabytes per second and the 8-sector/verify ceiling measured
  // 0.21 MiB/s (r267). Separate allocation keeps the class struct under
  // KALLOC_SAFE_ALLOC_SIZE (the r265 readShadow lesson).
  IOBufferMemoryDescriptor *bigBMD = nullptr;
  uint8_t *bigV = nullptr;
  uint32_t bigPhys = 0;
  // r260: window self-test scheduling (journal OFF rounds).
  thread_call_t stCall = nullptr;
  uint32_t stPass = 0;
  // r269: LK's live eMMC clock/ADMA/DLL state, snapshotted BEFORE the
  // identify re-init clobbers it. LK reads the kernel at tens of MB/s with
  // clk=0207 hc1=20 dll=a0000102 — our 0xFFC1 re-id writes the slow parent
  // into the clock-source mux bits (0xC0) and that capped EVERYTHING at
  // ~2.3 MB/s regardless of divider (r267/r268 lesson).
  uint16_t inhClk = 0;
  uint8_t  inhHc1 = 0;
  uint32_t inhDll = 0, inhDly = 0, inhOff = 0;
  uint16_t rca = 0;
  uint32_t ocr = 0;
  uint32_t cid[4] = {0, 0, 0, 0};
  uint32_t csd[4] = {0, 0, 0, 0};
  uint8_t  extCsd[512] = {0};
  uint64_t sectors = 0;
  bool cardUp = false;
  bool windowMultiWriteReady = false;
  bool partMode = false;
  uint64_t baseLba = 0;
  uint64_t cacheSectors = E5Emmc::cacheSectorCount;
  static NativeEmmc *s_host;
  bool windowPub = false;
  bool logWindowReady = false;
  uint32_t logPayloadBytes = 0;
  thread_call_t logCall = nullptr;
  bool logActive = false;
  uint32_t logNonce = 0, logSlot = 0, logLastStatus = 0, logLastR1 = 0;
  uint64_t logGeneration = 0, logSeq = 0, logCursor = 0, logOverrun = 0;
  // r257: window machinery (SystemOS + Data media windows on this eMMC).
  uint8_t readShadow[8192];      // r253 verify-retry shadow (not a DMA buffer)
  uint16_t windowSdclkfs = 0;    // lifted divider (0 = identify clock)
  uint32_t windowBaseMHz = 0;    // capabilities base clock when lifted
  bool wideBus = false;          // EXT_CSD 4-bit switch held

  inline uint8_t  r8 (uint32_t o) const { return *(volatile uint8_t  *)((uint8_t *)regs + o); }
  inline uint16_t r16(uint32_t o) const { return *(volatile uint16_t *)((uint8_t *)regs + o); }
  inline uint32_t r32(uint32_t o) const { return *(volatile uint32_t *)((uint8_t *)regs + o); }
  inline void w8 (uint32_t o, uint8_t  v) { *(volatile uint8_t  *)((uint8_t *)regs + o) = v; }
  inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)((uint8_t *)regs + o) = v; }
  inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)((uint8_t *)regs + o) = v; }

  // ---- cache maintenance by VA (PoC), 64-byte lines, matching the UEFI probe
  static void cacheClean(void *addr, uint32_t len) {
    uint64_t line = (uint64_t)addr & ~63ull;
    uint64_t end = ((uint64_t)addr + len + 63ull) & ~63ull;
    while (line < end) {
      __asm__ volatile ("dc cvac, %0" :: "r"(line) : "memory");
      line += 64;
    }
    __asm__ volatile ("dsb sy" ::: "memory");
  }
  static void cacheInvalidate(void *addr, uint32_t len) {
    uint64_t line = (uint64_t)addr & ~63ull;
    uint64_t end = ((uint64_t)addr + len + 63ull) & ~63ull;
    while (line < end) {
      __asm__ volatile ("dc ivac, %0" :: "r"(line) : "memory");
      line += 64;
    }
    __asm__ volatile ("dsb sy" ::: "memory");
  }

  // Standard reflected CRC-32 (poly 0xEDB88320), same result as the UEFI
  // probe's CalculateCrc32 so logged values compare directly with the
  // Android reference CRCs.
  static uint32_t crc32(const uint8_t *p, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
      c ^= p[i];
      for (int k = 0; k < 8; k++) {
        uint32_t mask = (uint32_t)-(int32_t)(c & 1u);
        c = (c >> 1) ^ (0xEDB88320u & mask);
      }
    }
    return c ^ 0xFFFFFFFFu;
  }

  // ---- interrupt waits: errors (0x32 half / bit15) always win; data waits
  // never break on COMMAND_COMPLETE (it fires when the R1 lands, before data).
  uint32_t waitCmdDone(uint32_t timeoutUs) {
    uint32_t st = 0;
    for (uint32_t i = 0; i * 10u < timeoutUs; i++) {
      st = r32(SDHCI_INT_STATUS);
      if (st & 0xFFFF8000u) return st;
      if (st & INT_CMD_COMPLETE) return st;
      IODelay(10);
    }
    return st;
  }
  uint32_t waitDmaDone(uint32_t timeoutUs) {
    uint32_t st = 0;
    for (uint32_t i = 0; i * 10u < timeoutUs; i++) {
      st = r32(SDHCI_INT_STATUS);
      if (st & 0xFFFF8000u) return st;
      if (st & INT_XFER_COMPLETE) return st;
      IODelay(10);
    }
    return st;
  }

  // One command without data: wipe 0x30 so a stale CC cannot fake success,
  // then argument, (cmdword<<16)|TM=0, and a CC wait.
  uint32_t cmdNoData(uint16_t cmdWord, uint32_t arg) {
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w32(SDHCI_ARGUMENT, arg);
    w32(SDHCI_TRANSFER_MODE_CMD, ((uint32_t)cmdWord << 16) | 0x0000u);
    return waitCmdDone(500000);
  }
  static bool cmdOk(uint32_t st) { return (st & 0xFFFF8001u) == INT_CMD_COMPLETE; }
  static bool transferOk(uint32_t st) {
    return E5Emmc::transferOk(st);
  }

  // CMD|DATA line reset with Unisoc RST_n (bit3) preserved; never RESET_ALL.
  bool resetLines() {
    w8(SDHCI_SOFTWARE_RESET, (r8(SDHCI_SOFTWARE_RESET) & 0x08u) | 0x06u);
    for (uint32_t i = 0; i < 10000; i++) {
      if ((r8(SDHCI_SOFTWARE_RESET) & 0x06u) == 0) return true;
      IODelay(10);
    }
    elog("NativeEmmc: line reset timeout %02x\n", r8(SDHCI_SOFTWARE_RESET));
    return false;
  }

  bool waitClockStable() {
    for (uint32_t i = 0; i < 10000; i++) {
      if (r16(0x2C) & 0x2u) return true;
      IODelay(10);
    }
    return false;
  }
  void waitDatInhibitClear() {
    for (uint32_t i = 0; i < 10000; i++) {
      if ((r32(SDHCI_PRESENT_STATE) & PSTATE_DAT_INHIBIT) == 0) return;
      IODelay(10);
    }
  }

  void fillPattern(uint32_t *d) {
    for (uint32_t i = 0; i < 128; i++) d[i] = 0xCAFEBABEu;
  }

  // ---- one SDMA single-block 512-byte read (v4 layout: count at 0x00,
  // sysaddr at 0x58/0x5C; if V4EN were somehow off, fall back to v3 sysaddr
  // at 0x00 - both branches mirror the UEFI probe exactly).
  uint32_t sdmaRead512(uint16_t cmdWord, uint32_t arg, void *buf, uint64_t bufPhys) {
    cacheClean(buf, 512);
    cacheInvalidate(buf, 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u); // SDMA select
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w16(SDHCI_BLOCK_SIZE, 512);
    if ((r16(0x3E) & 0x1000u) != 0) {
      w32(SDHCI_DMA_ADDR, 1);
      w32(SDHCI_SYSADDR_LOW, (uint32_t)bufPhys);
      w32(SDHCI_SYSADDR_HIGH, 0);
    } else {
      w32(SDHCI_DMA_ADDR, (uint32_t)bufPhys);
    }
    w32(SDHCI_ARGUMENT, arg);
    w32(SDHCI_TRANSFER_MODE_CMD, ((uint32_t)cmdWord << 16) | 0x0013u);
    return waitDmaDone(2000000);
  }

  bool logRead(uint64_t lba) {
    if (lba >= sectors || lba > 0xffffffffu) return false;
    uint32_t st = sdmaRead512(0x113a, (uint32_t)lba, dmaD, dmaPhys);
    cacheInvalidate(dmaD, 512);
    bool ok = transferOk(st) && !(r32(SDHCI_RESPONSE) & E5BootLog::r1Errors);
    if (!ok) { cardUp = false; logWindowReady = false; }
    return ok;
  }

  bool locateLogWindow() {
    using namespace E5BootLog;
    if (!cardUp || (extCsd[179] & 7u) || extCsd[15] || !logRead(1)) return false;
    Gpt g;
    if (!header(dmaD8, sectors, g)) return false;
    uint32_t crcState = 0xffffffffu, matches = 0;
    uint32_t remaining = g.count * 128u;
    for (uint32_t i = 0; remaining; ++i) {
      if (!logRead(g.entriesLba + i)) return false;
      unsigned bytes = remaining < 512 ? remaining : 512;
      crcState = crcUpdate(crcState, dmaD8, bytes);
      for (unsigned off = 0; off < bytes; off += 128)
        if (bootEntry(dmaD8 + off, sectors)) ++matches;
      remaining -= bytes;
    }
    if (~crcState != g.entriesCrc || matches != 1) return false;
    if (!logRead(partitionFirst) || !bootHeader(dmaD8)) return false;
    logPayloadBytes = le32(dmaD8 + 8);
    if (!logRead(partitionFirst + partitionSectors - 1) || !footer(dmaD8)) return false;
    logWindowReady = true;
    return true;
  }

  bool waitLogReady(uint32_t timeoutMs) {
    // CMD13 itself has a bounded 500ms command wait. A transport error aborts
    // immediately; a busy but responding card is polled at 1ms intervals.
    uint64_t deadline = 0, now = 0;
    clock_interval_to_deadline(timeoutMs, NSEC_PER_MSEC, &deadline);
    for (;;) {
      clock_get_uptime(&now);
      if (now >= deadline) return false;
      uint32_t st = cmdNoData(0x0d1a, (uint32_t)rca << 16);
      uint32_t r1 = r32(SDHCI_RESPONSE);
      if (!cmdOk(st) || (r1 & E5BootLog::r1Errors)) return false;
      if (E5BootLog::ready(r1) && !(r32(SDHCI_PRESENT_STATE) & 3u)) return true;
      IODelay(1000);
    }
  }

  bool flushLogCache() {
    if (!(extCsd[33] & 1u)) return waitLogReady(5000);
    uint32_t st = cmdNoData(0x061b, (3u << 24) | (32u << 16) | (1u << 8));
    if (!cmdOk(st) || (r32(SDHCI_RESPONSE) & E5BootLog::r1Errors)) return false;
    return waitLogReady(30000);
  }

  bool writeLogSector(uint64_t relative) {
    if (!logWindowReady || !E5BootLog::contains(relative, 1) || !waitLogReady(5000)) return false;
    uint64_t lba = E5BootLog::partitionFirst + E5BootLog::firstSector + relative;
    if (lba >= sectors || lba > 0xffffffffu || (r16(0x3e) & 0x1000u) == 0) return false;
    cacheClean(dmaD, 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
    w32(SDHCI_INT_STATUS, 0xffffffffu);
    w16(SDHCI_BLOCK_SIZE, 512); w32(SDHCI_DMA_ADDR, 1);
    w32(SDHCI_SYSADDR_LOW, (uint32_t)dmaPhys); w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, (uint32_t)lba);
    w32(SDHCI_TRANSFER_MODE_CMD, 0x183a0003u); // CMD24, SDMA, count=1, WRITE.
    uint32_t st = waitDmaDone(5000000);
    uint32_t r1 = r32(SDHCI_RESPONSE);
    logLastStatus = st; logLastR1 = r1;
    return transferOk(st) && !(r1 & E5BootLog::r1Errors) && waitLogReady(5000);
  }

  void logWriteProbe(uint32_t nonce) {
    using namespace E5BootLog;
    if (!locateLogWindow()) { elog("NativeEmmc: log probe layout refused\n"); return; }
    uint64_t lba = partitionFirst + firstSector;
    if (!logRead(lba)) { elog("NativeEmmc: log probe pre-read failed\n"); return; }
    unsigned nonzero = 0; for (unsigned i = 0; i < 512; ++i) nonzero |= dmaD8[i];
    if (nonzero && !validProbe(dmaD8)) { elog("NativeEmmc: log probe foreign sector refused\n"); return; }
    E5BootLog::probe(dmaD8, nonce, lba, logPayloadBytes);
    uint32_t expected = le32(dmaD8 + 508);
    elog("NativeEmmc: log probe nonce %u lba %llu crc %08x cache %u\n",
         nonce, (unsigned long long)lba, expected, extCsd[33]);
    bool wrote = writeLogSector(0);
    elog("NativeEmmc: log CMD24 st %08x r1 %08x\n", logLastStatus, logLastR1);
    bool flushed = wrote && flushLogCache();
    bool readback = flushed && logRead(lba) && validProbe(dmaD8) &&
        le32(dmaD8 + 12) == nonce && le32(dmaD8 + 508) == expected;
    elog("NativeEmmc: log probe wrote %u flushed %u readback %u\n", wrote, flushed, readback);
    // A failed write may still own DMA. Quarantine the host on any failed
    // write/flush: the recovery path must not reuse this buffer for reads.
    if (!wrote || !flushed || !readback) { cardUp = false; logWindowReady = false; }
  }

  static uint64_t logUptime() {
    uint64_t ticks = 0, ns = 0; clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks, &ns); return ns;
  }

  bool beginJournal(uint32_t nonce) {
    using namespace E5BootLog;
    if (!locateLogWindow()) return false;
    uint64_t newest = 0;
    for (unsigned i = 0; i < slots; ++i) {
      if (!logRead(partitionFirst + firstSector + i * slotSectors)) return false;
      if (validRecord(dmaD8) && le64(dmaD8 + 24) == 0 && le32(dmaD8 + 52) == 1) {
        uint64_t gen = le64(dmaD8 + 16); if (gen > newest) newest = gen;
      } else {
        unsigned nonzero = 0; for (unsigned k = 0; k < 512; ++k) nonzero |= dmaD8[k];
        bool ownTorn = !memcmp(dmaD8, "E5BLOG1", 8) && le32(dmaD8 + 8) == 1;
        if (nonzero && !(i == 0 && validProbe(dmaD8)) && !ownTorn)
          return false;
      }
    }
    if (newest == ~0ull) return false;
    logCall = thread_call_allocate(journalEntry, this);
    if (!logCall) return false;
    logNonce = nonce; logGeneration = newest + 1; logSlot = (uint32_t)((logGeneration - 1) % slots);
    record(dmaD8, nonce, logGeneration, 0, logUptime(), 0, 1, 0, nullptr, 0);
    if (!writeLogSector(logSlot * slotSectors) || !flushLogCache()) {
      thread_call_free(logCall); logCall = nullptr;
      cardUp = false; logWindowReady = false; return false;
    }
    logActive = true;
    sJournalHost = this; // r247: the panic flush needs the running journal host
    uint64_t deadline = 0; clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(logCall, deadline);
    return true;
  }

  static void journalEntry(thread_call_param_t context, thread_call_param_t) {
    NativeEmmc *host = (NativeEmmc *)context;
    if (!host || !host->logActive) return;
    // r251: heartbeat FIRST - if this pass hangs inside eMMC the scratch
    // still timestamps the pass that hung (the journal record it was about
    // to write will be missing). Words: [2]=pass count, [3]=uptime ms.
    // Lock-free uncached stores; single writer thread.
    if (sPanicScratch) {
      // r251: plain load+store, not ++ (C++20 deprecates volatile compound ops; -Werror).
      sPanicScratch[2] = sPanicScratch[2] + 1;
      sPanicScratch[3] = (uint32_t)(logUptime() / 1000000ull);
    }
    host->flushJournal();
    if (host->logActive) {
      // r247: 2 s cadence (was 5) - short pre-panic windows keep more context.
      uint64_t deadline = 0; clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
      thread_call_enter_delayed(host->logCall, deadline);
    }
  }

public:
  // r247: called from the panic path (e5-ram-console-tee patch) after the
  // panic reason is teed. The panic CPU is the only runner; a plain
  // IOLockLock could spin forever if a stopped CPU died holding ioLock, so
  // try-lock once and give up - a partial flush still beats none.
  void flushJournalPanic() {
    if (!logActive || !ioLock) return;
    if (!logWindowReady) {
      e5_panic_print("E5JRN: panic flush skipped (journal down)\n");
      return;
    }
    if (!IOLockTryLock(ioLock)) {
      e5_panic_print("E5JRN: lock busy, bare panic sector\n");
      panicWriteBare();
      return;
    }
    IOLockUnlock(ioLock);
    if (!cardUp) {
      e5_panic_print("E5JRN: panic flush skipped (card down)\n");
      return;
    }
    logPanicMode = true;
    flushJournal();
    logPanicMode = false;
    e5_panic_print("E5JRN: panic flush done\n");
  }

  // r249: if the ioLock try failed above, the journal thread died holding it
  // (r248/r248b: commits stop at ~84 s with no stopped line, and the later
  // panic's flush could not run). The panic CPU owns the world after the
  // debugger IPIs - the frozen holder will never run again - so write one
  // bare sector of the tee tail after a line reset, lock-free. Content beats
  // protocol here: a hand-rolled E5BLOG record with the newest console bytes
  // lands in this slot's sector 511.
  void panicWriteBare() {
    if (!logWindowReady) return;
    uint8_t *sect = dmaD8;
    // drain what the tee still holds into a raw text block (not a standard
    // record): magic + nonce + uptime + up to 470 bytes of console tail.
    uint64_t stream = 0;
    uint8_t bytes[E5BootLog::recordPayload];
    // Seed the cursor to leave only the newest recordPayload bytes pending,
    // using the same ring the journal thread reads (its own cursor may be
    // stuck behind a lock; the ring head is lock-free enough for panic).
    uint64_t tailCursor = logCursor;
    uint64_t lost = 0;
    uint64_t head = __atomic_load_n(&bootLogHead, __ATOMIC_RELAXED);
    if (head > tailCursor + E5BootLog::recordPayload)
      tailCursor = head - E5BootLog::recordPayload;
    unsigned n = bootLogCopy(tailCursor, bytes, stream, lost);
    bzero(sect, 512);
    memcpy(sect, "E5PANIC1", 8);
    struct Pack { uint32_t nonce; uint32_t len; uint64_t uptime; } pack = { logNonce, n, logUptime() };
    memcpy(sect + 16, &pack, sizeof(pack));
    if (n) memcpy(sect + 32, bytes, n < 470 ? n : 470);
    // line reset then one CMD24, mirroring writeLogSector's sequence but
    // without taking ioLock or touching cardUp state.
    resetLines();
    uint64_t lba = E5BootLog::partitionFirst + E5BootLog::firstSector
                 + logSlot * E5BootLog::slotSectors + (E5BootLog::slotSectors - 1);
    cacheClean(sect, 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
    w32(SDHCI_INT_STATUS, 0xffffffffu);
    w16(SDHCI_BLOCK_SIZE, 512); w32(SDHCI_DMA_ADDR, 1);
    w32(SDHCI_SYSADDR_LOW, (uint32_t)dmaPhys); w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, (uint32_t)lba);
    w32(SDHCI_TRANSFER_MODE_CMD, 0x183a0003u); // CMD24, SDMA, WRITE
    waitDmaDone(5000000);
    flushLogCache();
  }

private:

  // r247: in panic mode every host lock is a try-lock - a stopped CPU may
  // have died holding ioLock and the panic CPU must never spin on it.
  bool logPanicMode = false;
  bool lockIo() {
    if (logPanicMode) return IOLockTryLock(ioLock);
    IOLockLock(ioLock);
    return true;
  }

  void flushJournal() {
    using namespace E5BootLog;
    uint64_t cursor = logCursor, lost = logOverrun;
    unsigned written = 0;
    bool ok = true;
    for (unsigned i = 0; i < 16; ++i) {
      uint8_t bytes[recordPayload]; uint64_t stream = 0;
      unsigned n = bootLogCopy(cursor, bytes, stream, lost);
      if (!n) break;
      uint64_t seq = logSeq + 1;
      // Sectors 1..71 retain early output; 72..511 roll within this boot.
      uint32_t offset = seq < 72 ? (uint32_t)seq : 72 + (uint32_t)((seq - 72) % 440);
      if (!lockIo()) { ok = false; break; }
      if (!cardUp) { IOLockUnlock(ioLock); ok = false; break; }
      record(dmaD8, logNonce, logGeneration, seq, logUptime(), stream, 2,
             lost + __atomic_load_n(&bootLogDropped, __ATOMIC_RELAXED), bytes, n);
      ok = writeLogSector(logSlot * slotSectors + offset);
      if (!ok) { cardUp = false; logWindowReady = false; }
      IOLockUnlock(ioLock);
      if (!ok) break;
      logSeq = seq; ++written;
    }
    if (ok && !written) {
      uint64_t seq = logSeq + 1;
      uint32_t offset = seq < 72 ? (uint32_t)seq : 72 + (uint32_t)((seq - 72) % 440);
      ok = lockIo();
      if (ok) {
        if (cardUp) {
          record(dmaD8, logNonce, logGeneration, seq, logUptime(), cursor, 4,
                 lost + __atomic_load_n(&bootLogDropped, __ATOMIC_RELAXED), nullptr, 0);
          ok = writeLogSector(logSlot * slotSectors + offset);
        } else ok = false;
        if (!ok) { cardUp = false; logWindowReady = false; }
        IOLockUnlock(ioLock);
      }
      if (ok) { logSeq = seq; ++written; }
    }
    if (ok && written) {
      if (lockIo()) {
        ok = cardUp && flushLogCache();
        if (!ok) { cardUp = false; logWindowReady = false; }
        IOLockUnlock(ioLock);
      } else ok = false;
    }
    if (ok) { logCursor = cursor; logOverrun = lost; }
    else { logActive = false; elog("NativeEmmc: journal stopped st %08x r1 %08x\n", logLastStatus, logLastR1); }
    // r259: one line per pass (first 8, then every 16th) pins the journal
    // timeline against window-read traces when correlating corruption.
    if (written) {
      ++jrPasses;
      if (jrPasses <= 8 || (jrPasses % 16) == 0)
        elog("E5JRN pass %llu seq %llu+%u\n",
             (unsigned long long)jrPasses, (unsigned long long)(logSeq - written + 1), written);
    }
  }

  // ---- one ADMA2 single-block 512-byte read: 16-byte v4 descriptor in the
  // same page (buffer+512), Valid|End|Transfer = 0x23, length 512.
  uint32_t admaRead512(uint16_t cmdWord, uint32_t arg, void *buf, uint64_t bufPhys) {
    volatile uint32_t *desc = (volatile uint32_t *)((uint8_t *)buf + 512);
    desc[0] = 0x23u | (512u << 16);
    desc[1] = (uint32_t)bufPhys;
    desc[2] = 0;
    desc[3] = 0;
    cacheClean(buf, 512);
    cacheInvalidate(buf, 512);
    cacheClean((void *)desc, 32);   // the controller fetches descriptors
    w8(SDHCI_HOST_CONTROL, (r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u) | 0x10u); // ADMA2
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w16(SDHCI_BLOCK_SIZE, 512);
    w32(SDHCI_DMA_ADDR, 1);
    w32(SDHCI_SYSADDR_LOW, (uint32_t)(bufPhys + 512));
    w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, arg);
    w32(SDHCI_TRANSFER_MODE_CMD, ((uint32_t)cmdWord << 16) | 0x0013u);
    return waitDmaDone(2000000);
  }

  // The validated probe, ported 1:1 from E5BootMenu.c EmmcProbe.  Returns
  // true only if every check the UEFI acceptance run made also passes here.
  bool identifyAndRead(uint32_t *d, uint8_t *d8, uint64_t dva) {
    uint32_t st, crc;

    // --- inherit state: LK left the card selected in TRAN (RCA 1) ---
    elog("NativeEmmc: inherit hc1 %02x hc2 %04x clk %04x reset %02x\n",
         r8(SDHCI_HOST_CONTROL), r16(0x3E), r16(0x2C), r8(SDHCI_SOFTWARE_RESET));
    elog("NativeEmmc: inherit dll %08x dly %08x off %08x ps %08x\n",
         r32(0x200), r32(0x204), r32(0x208), r32(SDHCI_PRESENT_STATE));
    // r269: snapshot before any write below clobbers the live fast state.
    inhClk = r16(0x2C); inhHc1 = r8(SDHCI_HOST_CONTROL);
    inhDll = r32(0x200); inhDly = r32(0x204); inhOff = r32(0x208);
    st = cmdNoData(0x0D1A, 0x00010000u);   // CMD13, LK's RCA
    elog("NativeEmmc: inherit cmd13 st %08x r1 %08x\n", st, r32(SDHCI_RESPONSE));
    w32(SDHCI_INT_ENABLE, 0x1B7F413Bu);    // unmask status+error bits for polling
    w32(SDHCI_INT_SIGNAL_ENABLE, 0);

    // --- warm V4 read: prove the LK-live state still moves data ---
    fillPattern(d);
    st = sdmaRead512(0x113A, 1, d, dva);   // CMD17 READ_SINGLE_BLOCK, LBA1
    cacheInvalidate(d, 512);
    crc = crc32(d8, 512);
    elog("NativeEmmc: warm V4 LBA1 SDMA st %08x crc %08x b %08x %08x\n",
         st, crc, d[0], d[1]);

    // --- re-identify in a known legacy one-bit state at the maximum divider;
    //     RESET_ALL is deliberately avoided (it loses LK's clock/power config)
    w8(SDHCI_SOFTWARE_RESET, 0);           // RST_n low pulse resets the card
    IODelay(1000);
    w8(SDHCI_SOFTWARE_RESET, 0x08);
    IODelay(1000);
    if (!resetLines()) return false;
    w16(0x2C, 0);
    w16(0x3E, 0x3000);                     // HC2: V4EN + 64-bit addressing
    w8(SDHCI_HOST_CONTROL, 0);
    w8(SDHCI_TIMEOUT_CONTROL, 0x0E);
    w32(SDHCI_INT_ENABLE, 0x1B7F413Bu);
    w32(SDHCI_INT_SIGNAL_ENABLE, 0);
    w16(0x2C, 0xFFC1);                     // internal clock on, max divider
    if (!waitClockStable()) { elog("NativeEmmc: clock timeout\n"); return false; }
    w16(0x2C, 0xFFC5);                     // SD clock on
    IODelay(2000);

    st = cmdNoData(0x0000, 0);             // CMD0 GO_IDLE_STATE
    elog("NativeEmmc: cmd0 st %08x clk %04x reset %02x\n",
         st, r16(0x2C), r8(SDHCI_SOFTWARE_RESET));
    if (!cmdOk(st)) return false;

    uint32_t tries = 0;                    // CMD1 SEND_OP_COND, 1 ms apart
    for (; tries < 1000; tries++) {
      st = cmdNoData(0x0102, 0x40FF8000u);
      if (!cmdOk(st)) break;
      ocr = r32(SDHCI_RESPONSE);
      if (ocr & 0x80000000u) break;
      IODelay(1000);
    }
    elog("NativeEmmc: cmd1 st %08x ocr %08x tries %u\n", st, ocr, tries);
    if (!cmdOk(st) || (ocr & 0x80000000u) == 0) return false;

    st = cmdNoData(0x0209, 0);             // CMD2 ALL_SEND_CID (R2+CRC)
    cid[0] = r32(SDHCI_RESPONSE + 0x0); cid[1] = r32(SDHCI_RESPONSE + 0x4);
    cid[2] = r32(SDHCI_RESPONSE + 0x8); cid[3] = r32(SDHCI_RESPONSE + 0xC);
    elog("NativeEmmc: cmd2 st %08x cid %08x %08x %08x %08x\n",
         st, cid[0], cid[1], cid[2], cid[3]);
    if (!cmdOk(st)) return false;

    st = cmdNoData(0x031A, 0x00010000u);   // CMD3: eMMC takes a host RCA
    rca = 0x0001u;
    elog("NativeEmmc: cmd3 st %08x r1 %08x rca 0001\n", st, r32(SDHCI_RESPONSE));
    if (!cmdOk(st)) return false;

    st = cmdNoData(0x0909, ((uint32_t)rca) << 16);   // CMD9 SEND_CSD
    csd[0] = r32(SDHCI_RESPONSE + 0x0); csd[1] = r32(SDHCI_RESPONSE + 0x4);
    csd[2] = r32(SDHCI_RESPONSE + 0x8); csd[3] = r32(SDHCI_RESPONSE + 0xC);
    elog("NativeEmmc: cmd9 st %08x csd %08x %08x\n", st, csd[0], csd[1]);
    if (!cmdOk(st)) return false;

    st = cmdNoData(0x071B, ((uint32_t)rca) << 16);   // CMD7 SELECT -> TRAN
    elog("NativeEmmc: cmd7 st %08x r1 %08x\n", st, r32(SDHCI_RESPONSE));
    if (!cmdOk(st)) return false;
    waitDatInhibitClear();
    st = cmdNoData(0x0D1A, ((uint32_t)rca) << 16);
    elog("NativeEmmc: selected cmd13 st %08x r1 %08x ps %08x\n",
         st, r32(SDHCI_RESPONSE), r32(SDHCI_PRESENT_STATE));
    if (!cmdOk(st) || ((r32(SDHCI_RESPONSE) & 0x1F00u) != 0x0900u)) return false;

    // --- EXT_CSD (CMD8) via SDMA ---
    fillPattern(d);
    st = sdmaRead512(0x083A, 0, d, dva);
    cacheInvalidate(d, 512);
    elog("NativeEmmc: extcsd SDMA st %08x b %08x %08x r1 %08x ps %08x\n",
         st, d[0], d[1], r32(SDHCI_RESPONSE), r32(SDHCI_PRESENT_STATE));
    bool passed = false;
    if (transferOk(st)) {
      for (uint32_t i = 0; i < 512; i++) extCsd[i] = d8[i];
      sectors = d[53];                     // SEC_COUNT at EXT_CSD byte 212
      elog("NativeEmmc: extcsd rev %u sectors %08x width %02x timing %02x part %02x cmdq %02x\n",
           d8[192], d[53], d8[183], d8[185], d8[179], d8[15]);
      passed = d8[192] >= 2 && d8[192] <= 8 && d[53] != 0 && ((d8[179] & 7u) == 0);
    } else if (!resetLines()) {
      return false;
    }

    // --- LBA0 (protective MBR) and LBA1 (GPT header) via SDMA ---
    uint32_t crc1 = 0;
    for (uint32_t lba = 0; lba < 2; lba++) {
      fillPattern(d);
      st = sdmaRead512(0x113A, lba, d, dva);
      cacheInvalidate(d, 512);
      crc = crc32(d8, 512);
      elog("NativeEmmc: LBA%u SDMA st %08x crc %08x b %08x %08x sig %02x%02x\n",
           lba, st, crc, d[0], d[1], d8[511], d8[510]);
      if (lba == 1) crc1 = crc;
      if (!transferOk(st) ||
          (lba == 0 && (d8[510] != 0x55 || d8[511] != 0xAA)) ||
          (lba == 1 && (d[0] != 0x20494645u || d[1] != 0x54524150u))) {
        passed = false;
      }
      if (!transferOk(st) && !resetLines()) return false;
    }

    // --- SDMA reread, then ADMA2, both must reproduce the LBA1 CRC ---
    fillPattern(d);
    st = sdmaRead512(0x113A, 1, d, dva);
    cacheInvalidate(d, 512);
    crc = crc32(d8, 512);
    elog("NativeEmmc: LBA1 SDMA st %08x crc %08x b %08x %08x\n", st, crc, d[0], d[1]);
    if (!transferOk(st) || crc != crc1) passed = false;
    if (!transferOk(st) && !resetLines()) return false;

    fillPattern(d);
    st = admaRead512(0x113A, 1, d, dva);
    cacheInvalidate(d, 512);
    crc = crc32(d8, 512);
    elog("NativeEmmc: LBA1 ADMA st %08x crc %08x b %08x %08x err %02x\n",
         st, crc, d[0], d[1], r8(SDHCI_ADMA_ERROR));
    if (!transferOk(st) || crc != crc1) {
      elog("NativeEmmc: ADMA mismatch, stop rereads\n");
      return false;
    }

    // --- 16 alternating-engine rereads (mode switches + cache ownership) ---
    uint32_t n;
    for (n = 0; n < 16; n++) {
      fillPattern(d);
      if ((n & 1u) == 0) st = sdmaRead512(0x113A, 1, d, dva);
      else st = admaRead512(0x113A, 1, d, dva);
      cacheInvalidate(d, 512);
      crc = crc32(d8, 512);
      if (!transferOk(st) || crc != crc1) { passed = false; break; }
    }
    elog("NativeEmmc: repeat %u/16 st %08x crc %08x\n", n, st, crc);
    return passed;
  }

public:
  bool start(IOService *provider) override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Unisoc"); }
  void verifyReadback(uint32_t *d, uint8_t *d8, uint64_t dva) {
    const uint32_t bases[2] = {0x17E800u, 0x21000u};
    const uint32_t offs[2] = {8u, 0u};
    const char *tags[2] = {"bootb", "misc"};
    for (uint32_t i = 0; i < 2; i++) {
      for (uint32_t b = 0; b < 2; b++) {
        uint32_t st = sdmaRead512(0x113A, bases[i] + offs[i] + b, d, dva);
        cacheInvalidate(d, 512);
        elog("NativeEmmc: verify %s blk %u st %08x words %08x %08x %08x %08x crc %08x",
             tags[i], b, st, d[0], d[1], d[2], d[3], crc32(d8, 512));
      }
    }
  }


  bool scanGpt(uint32_t *d, uint8_t *d8, uint64_t dva) {
    uint32_t st = sdmaRead512(0x113A, 1, d, dva);
    cacheInvalidate(d, 512);
    if (transferOk(st) && d8[0] == 0x45 && d8[1] == 0x46 &&
        d8[2] == 0x49 && d8[3] == 0x20 && d8[4] == 0x50 && d8[5] == 0x41 &&
        d8[6] == 0x52 && d8[7] == 0x54) {
      uint64_t entriesLba = 0;
      for (uint32_t i = 0; i < 8; i++) entriesLba |= (uint64_t)d8[72 + i] << (8 * i);
      uint32_t nEntries = d8[80] | (d8[81] << 8) | (d8[82] << 16) | ((uint32_t)d8[83] << 24);
      uint32_t eSize = d8[84] | (d8[85] << 8) | (d8[86] << 16) | ((uint32_t)d8[87] << 24);
      elog("NativeEmmc: gpt hdr ok entries %u esize %u at lba %llu",
           nEntries, eSize, (unsigned long long)entriesLba);
      if (eSize >= 128 && eSize <= 512) {
        uint32_t perLba = 512 / eSize;
        uint32_t count = (nEntries < 128) ? nEntries : 128;
        for (uint32_t e = 0; e < count; e++) {
          uint32_t lba = (uint32_t)entriesLba + e / perLba;
          st = sdmaRead512(0x113A, lba, d, dva);
          cacheInvalidate(d, 512);
          if (transferOk(st)) {
            uint8_t *ent = d8 + (e % perLba) * eSize;
            uint32_t nz = 0;
            for (uint32_t i = 0; i < 16; i++) nz |= ent[i];
            if (nz == 0) continue;
            uint64_t first = 0, last = 0;
            for (uint32_t i = 0; i < 8; i++) {
              first |= (uint64_t)ent[32 + i] << (8 * i);
              last |= (uint64_t)ent[40 + i] << (8 * i);
            }
            uint64_t szMB = ((last - first + 1) * 512ull) >> 20;
            char name[40]; uint32_t ni = 0;
            for (uint32_t i = 0; i < 36; i++) {
              uint16_t c16 = (uint16_t)(ent[56 + 2 * i] | (ent[57 + 2 * i] << 8));
              if (c16 == 0) break;
              name[ni++] = (c16 >= 32 && c16 < 127) ? (char)c16 : 46;
              if (ni >= 38) break;
            }
            name[ni] = 0;
            elog("NativeEmmc: part %u first %llx last %llx szMB %llu type %02x%02x%02x%02x %s",
                 e, (unsigned long long)first, (unsigned long long)last,
                 (unsigned long long)szMB, ent[3], ent[2], ent[1], ent[0], name);
          }
        }
      }
      return true;
    }
    elog("NativeEmmc: gpt hdr missing (st %08x sig %02x %02x)", st, d8[0], d8[1]);
    return false;
  }

  char *getProductString() override { return const_cast<char *>("eMMC 5.1 (sprd,sdhci-r11)"); }
  char *getRevisionString() override { return const_cast<char *>("8"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("UEFI-validated v4 port"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { UInt64 n = (partMode || windowPub) ? cacheSectors : sectors; *value = n ? n - 1 : 0; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override { *present = cardUp; if (changed) *changed = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                   IOStorageAttributes *, IOStorageCompletion *completion) override {
    NativeEmmc *h = partMode ? s_host : this;
    UInt64 baseOff = (partMode || windowPub) ? baseLba : 0;
    if (buffer == nullptr || completion == nullptr || completion->action == nullptr || count == 0)
      return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    if (h->cardUp == false || h->ioLock == nullptr || h->dmaD == nullptr) return kIOReturnNotReady;
    UInt64 maxSec = (partMode || windowPub) ? cacheSectors : sectors;
    if (block > maxSec || count > maxSec - block) return kIOReturnBadArgument;
    UInt64 total = count * 512;
    if (buffer->getLength() < total) return kIOReturnBadArgument;
    UInt64 done = 0;
    static uint32_t s_calls = 0, s_fails = 0;
    bool trace = __sync_bool_compare_and_swap(&s_calls, s_calls, s_calls + 1) && s_calls <= 3;
    if (trace) elog("NativeEmmc: rw%s call %llu blocks @%llu dir %d len %llu\n", partMode ? "P" : "",
                    (unsigned long long)count, (unsigned long long)block,
                    (int)buffer->getDirection(), (unsigned long long)buffer->getLength());
    IOLockLock(h->ioLock);
    if (!h->cardUp) {
      IOLockUnlock(h->ioLock);
      completion->action(completion->target, completion->parameter, kIOReturnNotReady, 0);
      return kIOReturnSuccess;
    }
    while (done + 512 <= total) {
      uint32_t st = h->sdmaRead512(0x113A, (uint32_t)(baseOff + block + done / 512), h->dmaD, h->dmaPhys);
      cacheInvalidate(h->dmaD, 512);
      if (transferOk(st) &&
          buffer->writeBytes(done, h->dmaD8, 512) == 512) {
        done += 512;
      } else {
        if (s_fails < 3) elog("NativeEmmc: rw fail @blk %llu st %08x done %llu\n",
                              (unsigned long long)(block + done / 512), st, (unsigned long long)done);
        s_fails++;
        break;
      }
    }
    IOLockUnlock(h->ioLock);
    completion->action(completion->target, completion->parameter,
                       done == total ? kIOReturnSuccess : kIOReturnIOError, done);
    return kIOReturnSuccess;
  }

  // ---- r257: SystemOS/Data window machinery -------------------------------
  // Accessors for the out-of-line publication helpers.
  uint64_t windowCardSectors() const { return sectors; }
  const uint8_t *dmaPeek() const { return winD8; } // r259: window reads land in the second half-page
  // r264 diagnostics: CRC of the last window read + same-LBA cross-check
  // through the legacy single-sector SDMA path (first half-page; the
  // journal is dormant in journal-off rounds).
  uint32_t windowCrc() { return crc32(winD8, 4096); }
  bool legacyReadProbe(uint32_t lba) {
    uint32_t st = sdmaRead512(0x113A, lba, dmaD8, dmaPhys);
    cacheInvalidate(dmaD8, 512);
    if (!transferOk(st)) return false;
    return memcmp(dmaD8 + 32, "NXSB", 4) == 0;
  }
  // One multi-block SDMA CMD18 read of n<=16 sectors into the DMA page.
  // v4 layout: block count at 0x00, system address at 0x58/0x5C. The manual
  // CMD12 and line-reset-on-failure mirror the SD driver's proven r253 shape.
  bool readMulti(uint32_t lba, uint32_t n) {
    // r259 DMA overrun fix: window I/O lands in the SECOND 4 KiB half of the
    // 8 KiB page (journal staging keeps the first), so 8 sectors is the hard
    // ceiling — a 16-sector read wrote 4 KiB past the page and data-aborted
    // the kernel (r259/r260, far = regs+0x3000, x22 = poison).
    if (!n || n > 8 || !dmaD || !winD8 || (r16(0x3E) & 0x1000u) == 0) return false;
    // Poison the whole range first: if the engine does not advance the SDMA
    // system address across blocks (the v4 multi-block behavior is unproven
    // on this controller), the untouched tail sectors still hold the fill
    // pattern and the read fails instead of returning stale garbage — the
    // r256 lesson (console text delivered as APFS objects) at the source.
    for (uint32_t i = 0; i < n * 128; ++i) ((uint32_t *)winD8)[i] = 0xCAFEBABEu;
    cacheClean(winD8, n * 512);
    cacheInvalidate(winD8, n * 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w16(SDHCI_BLOCK_SIZE, 512);
    w32(SDHCI_DMA_ADDR, n);
    w32(SDHCI_SYSADDR_LOW, winPhys);
    w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, lba);
    w32(SDHCI_TRANSFER_MODE_CMD, 0x123A0033u); // CMD18 MULTI|COUNT|DMA|READ
    uint32_t st = waitDmaDone(2000000 + 100000 * n);
    bool ok = transferOk(st) && !(r32(SDHCI_RESPONSE) & E5BootLog::r1Errors);
    // r259: catch any argument/mode clobbering while the transfer ran.
    uint32_t argNow = r32(SDHCI_ARGUMENT), tmNow = r32(SDHCI_TRANSFER_MODE_CMD);
    if (ok && (argNow != lba || tmNow != 0x123A0033u))
      elog("NativeEmmc: xfer drift lba %u arg %08x tm %08x st %08x\n", lba, argNow, tmNow, st);
    cacheInvalidate(winD8, n * 512);
    if (ok && ((uint32_t *)winD8)[n * 128 - 1] == 0xCAFEBABEu &&
        ((uint32_t *)winD8)[n * 128 - 2] == 0xCAFEBABEu) {
      // Tail never left the fill pattern: the transfer did not cover the
      // full multi-block range. Fail it; the reread path retries.
      ok = false;
      elog("NativeEmmc: multi tail poison intact lba %u n %u st %08x\n", lba, n, st);
    }
    // r272: the stop must LAND before the next CMD18 — if the card never
    // left the previous stream the next transfer "completes" with the OLD
    // stream's continuation (r271: Data-window reads served OsRoot bytes;
    // media held zeros). Poll CMD13 back to ready/TRAN or fail + reset.
    cmdNoData(0x0C1A, 0); // CMD12 stop
    if (!waitLogReady(20)) {
      ok = false;
      elog("NativeEmmc: stop settle timeout lba %u n %u\n", lba, n);
    }
    if (!ok) resetLines();
    return ok;
  }

  // r268: one multi-block SDMA CMD18 read of n<=128 sectors (64 KiB) into the
  // dedicated bulk buffer — the readMulti poison/drift/stop discipline at
  // 16x the transfer size. LS-scan throughput: 3 GiB must fit a 300 s window.
  bool readMultiBig(uint32_t lba, uint32_t n) {
    if (!n || n > 128 || !bigV || (r16(0x3E) & 0x1000u) == 0) return false;
    for (uint32_t i = 0; i < n * 128; ++i) ((uint32_t *)bigV)[i] = 0xCAFEBABEu;
    cacheClean(bigV, n * 512);
    cacheInvalidate(bigV, n * 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w16(SDHCI_BLOCK_SIZE, 512);
    w32(SDHCI_DMA_ADDR, n);
    w32(SDHCI_SYSADDR_LOW, bigPhys);
    w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, lba);
    w32(SDHCI_TRANSFER_MODE_CMD, 0x123A0033u); // CMD18 MULTI|COUNT|DMA|READ
    uint32_t st = waitDmaDone(2000000 + 100000 * n);
    bool ok = transferOk(st) && !(r32(SDHCI_RESPONSE) & E5BootLog::r1Errors);
    uint32_t argNow = r32(SDHCI_ARGUMENT), tmNow = r32(SDHCI_TRANSFER_MODE_CMD);
    if (ok && (argNow != lba || tmNow != 0x123A0033u))
      elog("NativeEmmc: xfer drift lba %u arg %08x tm %08x st %08x\n", lba, argNow, tmNow, st);
    cacheInvalidate(bigV, n * 512);
    if (ok && ((uint32_t *)bigV)[n * 128 - 1] == 0xCAFEBABEu &&
        ((uint32_t *)bigV)[n * 128 - 2] == 0xCAFEBABEu) {
      ok = false;
      elog("NativeEmmc: big tail poison intact lba %u n %u st %08x\n", lba, n, st);
    }
    // r272: same stop-settle discipline for the bulk path.
    cmdNoData(0x0C1A, 0); // CMD12 stop
    if (!waitLogReady(20)) {
      ok = false;
      elog("NativeEmmc: big stop settle timeout lba %u n %u\n", lba, n);
    }
    if (!ok) resetLines();
    return ok;
  }
  // One single-block SDMA CMD24 write from winD8[srcOff .. srcOff+512) —
  // writeLogSector's shape, parameterized for the Data window. r271 fix: the
  // caller stages up to 8 sectors in winD8; without srcOff every sector of a
  // multi-sector write landed with sector-0 content (r270: dyld's generated
  // dyldlinkedit resource-fork offsets came back interleaved garbage and every
  // service paging it crashed).
  bool writeSingle(uint32_t lba, uint32_t srcOff = 0) {
    if (!dmaD || !winD8 || srcOff > 4096 - 512 || lba >= sectors || (r16(0x3E) & 0x1000u) == 0) return false;
    if (!waitLogReady(5000)) return false;
    cacheClean(winD8 + srcOff, 512);
    w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
    w32(SDHCI_INT_STATUS, 0xffffffffu);
    w16(SDHCI_BLOCK_SIZE, 512); w32(SDHCI_DMA_ADDR, 1);
    w32(SDHCI_SYSADDR_LOW, winPhys + srcOff); w32(SDHCI_SYSADDR_HIGH, 0);
    w32(SDHCI_ARGUMENT, lba);
    w32(SDHCI_TRANSFER_MODE_CMD, 0x183A0003u); // CMD24, SDMA, WRITE
    uint32_t st = waitDmaDone(5000000);
    uint32_t r1 = r32(SDHCI_RESPONSE);
    bool ok = transferOk(st) && !(r1 & E5BootLog::r1Errors) && waitLogReady(5000);
    if (!ok) { logLastStatus = st; logLastR1 = r1; resetLines(); }
    return ok;
  }

  // r278: program a staged Data chunk with CMD25 instead of eight CMD24s.
  // Caller owns ioLock throughout staging, programming and readback. The
  // readback poisons/overwrites winD8, so retain the exact staged bytes in
  // the non-DMA shadow; no success is reported until every byte agrees.
  bool writeWindowVerified(uint32_t lba, uint32_t n) {
    if (!n || n > 8 || !dmaD || !winD8 || lba >= sectors || n > sectors - lba ||
        (r16(0x3E) & 0x1000u) == 0) return false;
    const uint64_t started = mach_absolute_time();
    const uint64_t sequence = __atomic_add_fetch(&e5_win_w25_ops, 1ULL, __ATOMIC_RELAXED);
    memcpy(readShadow, winD8, (size_t)n * 512);
    bool ok = false;
    if (n == 1) {
      ok = writeSingle(lba);
    } else if (waitLogReady(5000)) {
      cacheClean(winD8, n * 512);
      w8(SDHCI_HOST_CONTROL, r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x18u);
      w32(SDHCI_INT_STATUS, 0xffffffffu);
      w16(SDHCI_BLOCK_SIZE, 512);
      w32(SDHCI_DMA_ADDR, n);
      w32(SDHCI_SYSADDR_LOW, winPhys);
      w32(SDHCI_SYSADDR_HIGH, 0);
      w32(SDHCI_ARGUMENT, lba);
      w32(SDHCI_TRANSFER_MODE_CMD, 0x193A0023u); // CMD25 MULTI|COUNT|DMA, WRITE
      const uint32_t st = waitDmaDone(5000000);
      const uint32_t r1 = r32(SDHCI_RESPONSE);
      ok = transferOk(st) && !(r1 & E5BootLog::r1Errors);
      const uint32_t stop = cmdNoData(0x0C1A, 0);
      const bool ready = waitLogReady(5000);
      ok = ok && cmdOk(stop) && ready;
      if (!ok) {
        logLastStatus = st; logLastR1 = r1;
        resetLines();
      }
    }
    if (ok) ok = readMulti(lba, n) && !memcmp(readShadow, winD8, (size_t)n * 512);
    if (ok) __atomic_add_fetch(&e5_win_w25_verified, 1ULL, __ATOMIC_RELAXED);
    const uint64_t elapsed = mach_absolute_time() - started;
    __atomic_add_fetch(&e5_win_w25_ticks, elapsed, __ATOMIC_RELAXED);
    if (!ok || sequence <= 4) {
      uint64_t ns = 0;
      absolutetime_to_nanoseconds(elapsed, &ns);
      elog("E5W25 id=%llu lba=%u n=%u ok=%u us=%llu\n",
           (unsigned long long)sequence, lba, n, ok,
           (unsigned long long)(ns / 1000));
    }
    return ok;
  }

  // Qualify CMD25 on one existing spare page before exposing writable Data.
  // Restore with the accepted CMD24 path, then compare a fresh media read.
  // A failed qualification keeps CMD24; a failed restore refuses publication.
  bool qualifyWindowWrite() {
    constexpr uint32_t lba = E5SelfTest::kSpareBase;
    constexpr uint32_t first = lba - 8;
    static_assert(first >= E5Data::base + E5Data::sectors, "write probe overlaps Data");
    static uint8_t before[3][4096]; // target plus two guards; never on the stack
    windowMultiWriteReady = false;
    if (first >= sectors || 24 > sectors - first) return true;
    for (unsigned page = 0; page < 3; ++page) {
      if (!readMulti(first + page * 8, 8)) return true;
      memcpy(before[page], winD8, 4096);
      if (!readMulti(first + page * 8, 8) || memcmp(before[page], winD8, 4096)) return true;
    }
    for (unsigned i = 0; i < 4096 / 4; ++i)
      ((uint32_t *)winD8)[i] = 0xE5278000u ^ (i * 0x01010101u);
    bool passed = writeWindowVerified(lba, 8);
    for (unsigned page = 0; page < 3; page += 2) {
      if (!readMulti(first + page * 8, 8) || memcmp(before[page], winD8, 4096)) passed = false;
    }
    bool restored = true;
    for (unsigned page = 0; restored && page < 3; ++page) {
      memcpy(winD8, before[page], 4096);
      for (uint32_t i = 0; restored && i < 8; ++i)
        restored = writeSingle(first + page * 8 + i, i * 512);
      if (restored) restored = readMulti(first + page * 8, 8) && !memcmp(before[page], winD8, 4096);
    }
    windowMultiWriteReady = passed && restored;
    elog("E5W25 qualification lba=%u passed=%u restored=%u enabled=%u\n",
         lba, passed, restored, windowMultiWriteReady);
    return restored;
  }

  bool lba1Crc(uint32_t *out) {
    if (!dmaD) return false;
    uint32_t st = sdmaRead512(0x113A, 1, dmaD, dmaPhys);
    cacheInvalidate(dmaD, 512);
    if (!transferOk(st)) return false;
    *out = crc32(dmaD8, 512);
    return true;
  }
  bool stableLba1(uint32_t ref) {
    uint32_t c = 0;
    for (unsigned i = 0; i < 4; ++i)
      if (!lba1Crc(&c) || c != ref) return false;
    return true;
  }

  // r257 clock lift; r269 rewrite. r267/r268 lesson: every divider rung
  // delivered the same ~2.3 MB/s because the identify re-init (0xFFC1) had
  // muxed the SLOW parent into the clock-source bits (0xC0), and LK's fast
  // state (clk=0207: SDCLKFS=2 + source 0x07, hc1=0x20: 8-BIT bus, tuned
  // vendor DLL a0000102/3434b760/00202000) was never restored. New order:
  // restore DLL/delay regs, switch the CARD to 8-bit (CMD6 183=2, HC1 bit5),
  // then walk SDCLKFS rungs on the INHERITED source bits with stability
  // gates; probe with the 64 KiB bulk read. 4-bit is the fallback. HS/DDR
  // stay out — LK runs none of it.
  uint16_t clkSrcBits = 0x00C0u; // r269: from inhClk (identify sets slow mux)
  void setDivider(uint16_t sdclkfs) {
    w16(0x2C, (uint16_t)((sdclkfs << 8) | clkSrcBits | 0x0001u));
    waitClockStable();
    w16(0x2C, (uint16_t)((sdclkfs << 8) | clkSrcBits | 0x0005u));
    IODelay(2000);
  }
  void liftClock() {
    uint32_t ref = 0;
    if (!lba1Crc(&ref)) { elog("NativeEmmc: lift ref read failed\n"); return; }
    uint32_t caps = r32(0x40);
    elog("NativeEmmc: caps %08x inh clk %04x hc1 %02x ref crc %08x\n",
         caps, inhClk, inhHc1, ref);
    // Restore LK's vendor DLL/delay tuning (idempotent if untouched).
    w32(0x200, inhDll);
    w32(0x204, inhDly);
    w32(0x208, inhOff);
    // Bus width: 8-bit like LK; 4-bit fallback.
    bool wide8 = false, wide4 = false;
    {
      uint32_t st = cmdNoData(0x061B, (3u << 24) | (183u << 16) | (2u << 8)); // 8-bit
      uint32_t r1 = r32(SDHCI_RESPONSE);
      if (cmdOk(st) && !(r1 & E5BootLog::r1Errors) && waitLogReady(1000)) {
        w8(SDHCI_HOST_CONTROL, (r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x02u) | 0x20u);
        wide8 = true;
        elog("NativeEmmc: 8-bit switch y\n");
      } else {
        elog("NativeEmmc: 8-bit switch refused st %08x r1 %08x\n", st, r1);
        st = cmdNoData(0x061B, (3u << 24) | (183u << 16) | (1u << 8)); // 4-bit
        r1 = r32(SDHCI_RESPONSE);
        if (cmdOk(st) && !(r1 & E5BootLog::r1Errors) && waitLogReady(1000)) {
          w8(SDHCI_HOST_CONTROL, (r8(SDHCI_HOST_CONTROL) & (uint8_t)~0x20u) | 0x02u);
          wide4 = true;
          elog("NativeEmmc: 4-bit switch y\n");
        } else {
          elog("NativeEmmc: width switches refused st %08x r1 %08x\n", st, r1);
        }
      }
    }
    // Rung order: LK's own SDCLKFS first, then slower. Source bits are the
    // INHERITED ones (fast parent), not whatever identify left in 0x2C.
    clkSrcBits = (uint16_t)(inhClk & 0x00F8u);
    uint16_t inhSd = (uint16_t)((inhClk >> 8) & 0xFFu);
    uint16_t ladder[5]; unsigned nl = 0;
    if (inhSd >= 1 && inhSd <= 0xFE) {
      ladder[nl++] = inhSd;
      if (inhSd != 4) ladder[nl++] = 4;
      if (inhSd != 8) ladder[nl++] = 8;
      ladder[nl++] = 16; ladder[nl++] = 0xFF;
    } else {
      ladder[nl++] = 2; ladder[nl++] = 4; ladder[nl++] = 8; ladder[nl++] = 16;
      ladder[nl++] = 0xFF;
    }
    for (unsigned i = 0; i < nl; ++i) {
      setDivider(ladder[i]);
      if (!stableLba1(ref)) {
        elog("NativeEmmc: div %u unstable\n", ladder[i]);
        continue;
      }
      uint64_t t0 = 0, t1 = 0;
      clock_get_uptime(&t0);
      bool fr = readMultiBig(1, 128);
      clock_get_uptime(&t1);
      uint64_t us = (t1 - t0) / 1000ull;
      if (!fr) {
        elog("NativeEmmc: div %u big probe FAIL\n", ladder[i]);
        continue;
      }
      if (!stableLba1(ref)) {
        elog("NativeEmmc: div %u post-probe unstable\n", ladder[i]);
        continue;
      }
      windowSdclkfs = ladder[i]; windowBaseMHz = 0; wideBus = wide8 || wide4;
      elog("NativeEmmc: fast mode div %u src %02x %s; 128-sector in %llu us (%llu KB/s)\n",
           ladder[i], clkSrcBits, wide8 ? "8-bit" : (wide4 ? "4-bit" : "1-bit"),
           (unsigned long long)us,
           (unsigned long long)(us ? (uint64_t)65536 / us : 0));
      return;
    }
    setDivider(0xFF);
    elog("NativeEmmc: fast mode refused, identify clock restored\n");
  }

  // One verified n<=8 sector window read (r266): two reads must agree; the
  // DELIVERED bytes come from a memcpy of the verified shadow copy, so any
  // post-verify winD8 mutation cannot reach the client. Disagreement logs
  // both crcs.
  bool windowReadVerifiedDeliver(uint8_t *deliver, uint32_t lba, uint32_t n) {
    static uint8_t shadow2[8192]; // ioLock-serialized; keeps the class under KALLOC_SAFE_ALLOC_SIZE
    bool have = false;
    // r272: a failed readMulti (poison-intact / settle-timeout / line reset)
    // retries instead of failing the whole request; the final-read-wins
    // delivery only happens when at least one full transfer succeeded.
    for (unsigned round = 0; round < 3; ++round) {
      if (!readMulti(lba, n)) {
        elog("NativeEmmc: rv dma fail lba %u round %u\n", lba, round);
        continue;
      }
      memcpy(shadow2, winD8, (size_t)n * 512);
      have = true;
      if (!readMulti(lba, n)) {
        elog("NativeEmmc: rv dma fail2 lba %u round %u\n", lba, round);
        continue;
      }
      if (!memcmp(shadow2, winD8, (size_t)n * 512)) {
        memcpy(deliver, shadow2, (size_t)n * 512);
        return true;
      }
      elog("NativeEmmc: rv disagree lba %u round %u crcA %08x crcB %08x\n",
           lba, round, crc32(shadow2, n * 512), crc32(winD8, n * 512));
      __atomic_fetch_add(&e5_win_unstable, 1, __ATOMIC_RELAXED);
    }
    if (!have) return false;
    // Final read wins, delivered from its verified copy.
    memcpy(deliver, shadow2, (size_t)n * 512);
    return true;
  }

  bool windowReadVerified(uint32_t lba, uint32_t n) {
    for (unsigned round = 0; round < 2; ++round) {
      if (!readMulti(lba, n)) return false;
      memcpy(readShadow, winD8, (size_t)n * 512);
      if (!readMulti(lba, n)) return false;
      if (!memcmp(readShadow, winD8, (size_t)n * 512)) return true;
      __atomic_fetch_add(&e5_win_unstable, 1, __ATOMIC_RELAXED);
      elog("NativeEmmc: rv unstable lba %u n %u round %u\n", lba, n, round);
    }
    return true; // final read wins
  }

  // SystemOS window reads for E5SystemOSDeviceEmmc: 8-sector verified chunks
  // under the host lock with the r254 big-read trace shape (the host-side
  // scanner's 'rw+' regexes match the progress lines).
  // r268: reads of >=64 sectors take the bulk path — 128-sector readMultiBig
  // chunks into the dedicated 64 KiB buffer, single-read with per-chunk CRC
  // logging (host-verifiable against the image like r267); the FIRST chunk
  // of each bulk read is two-agree gated (double read, CRCs must match) as a
  // cheap per-call integrity canary. 3 GiB in a 300 s window needs this.
  IOReturn windowRead(IOMemoryDescriptor *data, UInt64 block, UInt64 count, uint64_t base) {
    if (!ioLock || !dmaD) return kIOReturnNotReady;
    if (data->getLength() < count * 512) return kIOReturnBadArgument;
    E5WindowReadTrace trace(base + block, count);
    bool big = count >= 512;
    if (big) elog("NativeEmmc: rw+ enter %llu at %llu\n",
                  (unsigned long long)count, (unsigned long long)(base + block));
    __atomic_fetch_add(&e5_win_rd_ops, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&e5_win_rd_blocks, count, __ATOMIC_RELAXED);
    if (count >= 64 && bigV) {
      UInt64 chunks = (count + 127) / 128;
      for (UInt64 c = 0; c < chunks; ++c) {
        uint32_t n = (uint32_t)((count - c * 128) > 128 ? 128 : count - c * 128);
        uint32_t crc = 0;
        bool ok;
        const uint64_t ts = __atomic_add_fetch(&winTraceSeq, 1ULL, __ATOMIC_RELAXED);
        const bool logCrc = ts <= 4096 && (ts <= 16 || (c & 63u) == 0);
        uint64_t lockStart = trace.enabled ? mach_absolute_time() : 0;
        IOLockLock(ioLock);
        trace.waited(lockStart);
        ok = cardUp && readMultiBig((uint32_t)(base + block + c * 128), n);
        if (ok && c == 0) {
          // First-chunk two-agree canary: reread and compare CRCs.
          const uint32_t crc1 = crc32(bigV, (size_t)n * 512);
          if (readMultiBig((uint32_t)(base + block + c * 128), n)) {
            uint32_t crc2 = crc32(bigV, (size_t)n * 512);
            crc = crc2;
            if (crc1 != crc2) {
              elog("NativeEmmc: rv disagree big %llx+%u crcA %08x crcB %08x\n",
                   (unsigned long long)(base + block + c * 128), n, crc1, crc2);
              __atomic_fetch_add(&e5_win_unstable, 1, __ATOMIC_RELAXED);
            }
          } else {
            ok = false;
          }
        }
        // r274: deliver INSIDE the lock. bigV is the DMA target of every bulk
        // transfer and readMultiBig fills it with 0xCAFEBABE before each one,
        // so a copy made after the unlock can carry another request's poison
        // fill (or its verified bytes) into the client — the r273 APFS
        // "cksum 0xcafebabecafebabe size 4096" corruption.
        if (ok) {
          // r276: CRCs outside the first-chunk canary are diagnostic only.
          // Once their trace is suppressed, do not CRC every byte twice.
          // Reuse the canary's CRC of the bytes actually being delivered.
          if (logCrc && c != 0) crc = crc32(bigV, (size_t)n * 512);
          if (data->writeBytes(c * 128 * 512, bigV, (size_t)n * 512) != (IOByteCount)(n * 512))
            ok = false;
        }
        IOLockUnlock(ioLock);
        if (logCrc)
          elog("NativeEmmc: wr #%llu ob %llx+%u %s crc %08x\n",
               (unsigned long long)ts, (unsigned long long)(base + block + c * 128), n,
               ok ? "y" : "FAIL", crc);
        if (!ok) return kIOReturnIOError;
        if (big && ((c + 1) % 64) == 0)
          elog("NativeEmmc: rw+ %llu/%llu at %llu+%llu\n",
               (unsigned long long)((c + 1) * 128), (unsigned long long)count,
               (unsigned long long)(base + block),
               (unsigned long long)(c * 128 * 512));
      }
      if (big) elog("NativeEmmc: rw+ exit full %llu/%llu at %llu RD=%llu:%llu RV=%llu\n",
                    (unsigned long long)count, (unsigned long long)count,
                    (unsigned long long)(base + block),
                    (unsigned long long)e5_win_rd_ops, (unsigned long long)e5_win_rd_blocks,
                    (unsigned long long)e5_win_unstable);
      trace.result = kIOReturnSuccess;
      return kIOReturnSuccess;
    }
    UInt64 chunks = (count + 7) / 8;
    for (UInt64 c = 0; c < chunks; ++c) {
      uint32_t n = (uint32_t)((count - c * 8) > 8 ? 8 : count - c * 8);
      uint32_t crc = 0;
      bool ok;
      const uint64_t ts = __atomic_add_fetch(&winTraceSeq, 1ULL, __ATOMIC_RELAXED);
      uint64_t lockStart = trace.enabled ? mach_absolute_time() : 0;
      IOLockLock(ioLock);
      trace.waited(lockStart);
      ok = cardUp && windowReadVerifiedDeliver(winD8, (uint32_t)(base + block + c * 8), n);
      // r274: deliver inside the lock — winD8 is the DMA target of every
      // window transfer and readMulti re-poisons it with 0xCAFEBABE before
      // each one, so a copy taken after the unlock can hand the client
      // another request's poison fill (r273: APFS read a 4 KiB
      // 0xCAFEBABE-checksum block on disk4s1/disk6s1).
      if (ok) {
        if (ts <= 4096) crc = crc32(winD8, (size_t)n * 512);
        if (data->writeBytes(c * 8 * 512, winD8, (size_t)n * 512) != (IOByteCount)(n * 512))
          ok = false;
      }
      IOLockUnlock(ioLock);
      if (ts <= 4096)
        elog("NativeEmmc: wr #%llu os %llx+%u %s crc %08x\n",
             (unsigned long long)ts, (unsigned long long)(base + block + c * 8), n,
             ok ? "y" : "FAIL", crc);
      if (!ok) return kIOReturnIOError;
      if (big && ((c + 1) % 64) == 0)
        elog("NativeEmmc: rw+ %llu/%llu at %llu+%llu\n",
             (unsigned long long)((c + 1) * 8), (unsigned long long)count,
             (unsigned long long)(base + block),
             (unsigned long long)(c * 8 * 512));
    }
    if (big) elog("NativeEmmc: rw+ exit full %llu/%llu at %llu RD=%llu:%llu RV=%llu\n",
                  (unsigned long long)count, (unsigned long long)count,
                  (unsigned long long)(base + block),
                  (unsigned long long)e5_win_rd_ops, (unsigned long long)e5_win_rd_blocks,
                  (unsigned long long)e5_win_unstable);
    trace.result = kIOReturnSuccess;
    return kIOReturnSuccess;
  }

  // Data window media-direct I/O for E5DataDeviceEmmc: reads stream verified
  // CMD18 chunks. Qualified multi-sector writes use CMD25 plus byte-for-byte
  // readback; otherwise use the accepted CMD24 path, under the host lock.
  IOReturn dataMediaIO(IOMemoryDescriptor *data, UInt64 block, UInt64 count, bool out) {
    if (!ioLock || !dmaD) return kIOReturnNotReady;
    if (data->getLength() < count * 512) return kIOReturnBadArgument;
    if (out) {
      __atomic_fetch_add(&e5_win_wr_ops, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&e5_win_wr_blocks, count, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&e5_win_rd_ops, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&e5_win_rd_blocks, count, __ATOMIC_RELAXED);
    }
    for (UInt64 off = 0; off < count; off += 8) {
      uint32_t n = (uint32_t)((count - off) > 8 ? 8 : count - off);
      bool ok;
      uint32_t crc = 0;
      uint64_t ts = 0;
      if (out) {
        // r274: stage AND write under the lock. readBytes used to run before
        // the lock, so a concurrent window read's 0xCAFEBABE pre-fill (or its
        // verified bytes) landed in the staged sector and was then written to
        // media — the r272-era zero/foreign-crc write trace.
        IOLockLock(ioLock);
        ok = cardUp;
        if (ok && data->readBytes(off * 512, winD8, (size_t)n * 512) != (IOByteCount)(n * 512))
          ok = false;
        if (windowMultiWriteReady) {
          if (ok) ok = writeWindowVerified((uint32_t)(E5Data::base + block + off), n);
        } else {
          for (uint32_t s = 0; ok && s < n; ++s)
            ok = writeSingle((uint32_t)(E5Data::base + block + off + s), s * 512);
        }
        IOLockUnlock(ioLock);
      } else {
        ts = __atomic_add_fetch(&winTraceSeq, 1ULL, __ATOMIC_RELAXED);
        IOLockLock(ioLock);
        ok = cardUp && windowReadVerifiedDeliver(winD8, (uint32_t)(E5Data::base + block + off), n);
        // r274: deliver inside the lock — see windowRead's note.
        if (ok) {
          if (ts <= 4096) crc = crc32(winD8, (size_t)n * 512);
          if (data->writeBytes(off * 512, winD8, (size_t)n * 512) != (IOByteCount)(n * 512))
            ok = false;
        }
        IOLockUnlock(ioLock);
        if (ts <= 4096)
          elog("NativeEmmc: wr #%llu dm %llx+%u %s crc %08x\n",
               (unsigned long long)ts, (unsigned long long)(E5Data::base + block + off), n,
               ok ? "y" : "FAIL", crc);
      }
      if (!ok) {
        if (out) __atomic_fetch_add(&e5_win_wr_bad, 1, __ATOMIC_RELAXED);
        elog("NativeEmmc: data media %s fail %llu+%llu\n",
             out ? "write" : "read", (unsigned long long)block, (unsigned long long)off);
        return kIOReturnIOError;
      }
    }
    return kIOReturnSuccess;
  }

  // r260/r262: deterministic window self-test, host-verifiable. Journal OFF
  // is the variable under test (r258/r259 served journal bytes to Data-window
  // reads; r261 with journal off still corrupted one 16 KiB read at window
  // chunk 6170 — APFS hash-mismatched inode 49, media verified clean). The
  // r262 tables: sparse 1-per-1MiB maps of both windows (corruption map),
  // dense coverage of the fault region, and the spare pattern. Single
  // readMulti per chunk — no two-agree masking. Wrong chunks dump their
  // first 32 bytes so the wrong-content source is identifiable offline.
  void windowSelfTest(const char *tag) {
    struct Area { const char *name; uint32_t base; const uint32_t *crcs;
                  uint32_t count; uint32_t chunkStep; };
    // r264 order = diagnostic priority: Data head (r263 base fault), the
    // cryptex fault region (chunk 6170), then the fault-adjacent dense band,
    // spare, and only then the wide sparse sweeps.
    static const Area areas[6] = {
      {"dmh", (uint32_t)E5Data::base, E5SelfTest::dmh,
       (uint32_t)(sizeof(E5SelfTest::dmh) / 4), 1},
      {"osf", (uint32_t)E5SystemOS::base + (E5SelfTest::kOsDenseFirst + 16) * 8,
       E5SelfTest::osf, (uint32_t)(sizeof(E5SelfTest::osf) / 4), 1},
      {"osd", (uint32_t)E5SystemOS::base + E5SelfTest::kOsDenseFirst * 8,
       E5SelfTest::osd, (uint32_t)(sizeof(E5SelfTest::osd) / 4), 1},
      {"sp", E5SelfTest::kSpareBase, E5SelfTest::spare,
       (uint32_t)(sizeof(E5SelfTest::spare) / 4), 1},
      {"oss", (uint32_t)E5SystemOS::base, E5SelfTest::os,
       (uint32_t)(sizeof(E5SelfTest::os) / 4), 256},
      {"dms", (uint32_t)E5Data::base, E5SelfTest::dms,
       (uint32_t)(sizeof(E5SelfTest::dms) / 4), 256},
    };
    if (!ioLock || !winD8) return;
    uint32_t bad = 0;
    bool dead = !cardUp;
    uint64_t t0ns = 0, t1ns = 0;
    clock_get_uptime(&t0ns);
    if (!dead) {
      IOLockLock(ioLock);
      dead = !cardUp; // re-check under the lock
      if (!dead) {
        // r263: hard deadline so a wedged area cannot stall the worker
        // thread forever (r262 t0 hung silently inside start()).
        uint64_t deadline = 0;
        clock_interval_to_absolutetime_interval(90, NSEC_PER_SEC, &deadline);
        deadline += t0ns;
        uint64_t chunk = 0, total = 0;
        for (int a = 0; a < 6; ++a) total += areas[a].count;
        for (int a = 0; a < 6; ++a) {
          elog("NativeEmmc: ST %s %s begin (%u chunks)\n", tag, areas[a].name, areas[a].count);
          for (uint32_t i = 0; i < areas[a].count; ++i) {
            if ((++chunk & 255u) == 0) {
              uint64_t now = 0;
              clock_get_uptime(&now);
              if (now > deadline) {
                elog("NativeEmmc: ST %s %s deadline bail at %llu/%llu\n",
                     tag, areas[a].name, (unsigned long long)chunk, (unsigned long long)total);
                bad += 0x80000000u;
                break;
              }
              elog("NativeEmmc: ST %s %s progress %llu/%llu\n",
                   tag, areas[a].name, (unsigned long long)chunk, (unsigned long long)total);
            }
            uint32_t lba = areas[a].base + i * areas[a].chunkStep * 8;
            if (!readMulti(lba, 8)) {
              elog("NativeEmmc: ST %s %s#%u READFAIL lba %u\n", tag, areas[a].name, i, lba);
              ++bad;
              continue;
            }
            uint32_t c = crc32(winD8, 4096);
            if (c != areas[a].crcs[i]) {
              char hex[65];
              static const char xd[] = "0123456789abcdef";
              for (int k = 0; k < 32; ++k) {
                hex[k * 2] = xd[winD8[k] >> 4];
                hex[k * 2 + 1] = xd[winD8[k] & 15];
              }
              hex[64] = 0;
              elog("NativeEmmc: ST %s %s#%u lba %u crc %08x want %08x got32 %s\n",
                   tag, areas[a].name, i, lba, c, areas[a].crcs[i], hex);
              ++bad;
            }
          }
        }
      }
      IOLockUnlock(ioLock);
    }
    clock_get_uptime(&t1ns);
    elog("NativeEmmc: SELFTEST %s %s bad=%u %llums\n",
         tag, (bad || dead) ? "FAIL" : "CLEAN", bad,
         (unsigned long long)((t1ns - t0ns) / 1000000ull));
  }

  // r260: t1..t3 repeat the self-test on 45 s spacing so reads land inside
  // the APFS mount window where the corruption was observed.
  static void stEntry(thread_call_param_t context, thread_call_param_t) {
    NativeEmmc *host = (NativeEmmc *)context;
    if (!host || !host->stCall) return;
    static const char *tags[3] = {"t1", "t2", "t3"};
    if (host->stPass < 3) {
      host->windowSelfTest(tags[host->stPass]);
      host->stPass += 1;
    }
    if (host->stPass < 3) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(45, NSEC_PER_SEC, &deadline);
      thread_call_enter_delayed(host->stCall, deadline);
    }
  }
};
OSDefineMetaClassAndStructors(NativeEmmc, IOBlockStorageDevice)

// r247: panic-path entry from e5-ram-console-tee (class complete here).
extern "C" void e5_journal_flush_panic(void) {
  if (sJournalHost) sJournalHost->flushJournalPanic();
}

// r250: panic-site scratch. The firmware's ring-resume dumps physical
// 0xC6000000 words 0..3 ("scratch raw") and 0xC6002000 words 0..7 ("trace")
// on every boot, so a panic can leave its caller PC and reason text in plain
// DRAM with no locks, no console and no eMMC. Armed during start() (r251:
// uncached ml_io_map window + rolling heartbeat in the journal worker).
extern "C" void e5_panic_scratch_write(uint64_t pc, const char *reason) {
  if (!sPanicScratch) return;
  uint32_t hash = 2166136261u;
  for (const char *p = reason; p && *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619u;
  sPanicScratch[0] = 0x494E4150u; // "PANI"
  sPanicScratch[1] = (uint32_t)(pc & 0xffffffffu);
  sPanicScratch[2] = (uint32_t)(pc >> 32);
  sPanicScratch[3] = hash;
  if (sPanicTrace) {
    for (unsigned i = 0; i != 8; ++i) sPanicTrace[i] = 0;
    for (unsigned i = 0; i != 8; ++i) {
      uint32_t w = 0;
      for (unsigned b = 0; b != 4; ++b) {
        char c = reason[i * 4 + b];
        if (!c) break;
        w |= (uint32_t)(uint8_t)c << (8 * b);
      }
      sPanicTrace[i] = w;
    }
  }
}
NativeEmmc *NativeEmmc::s_host = nullptr;

// ---- r257: window publication (the NativeSd r245 pattern, eMMC host) ----
// Separate IOBlockStorageDevice instances attached to the eMMC host; the
// host itself keeps publishing its read-only log cache window unchanged.
class E5SystemOSDeviceEmmc : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(E5SystemOSDeviceEmmc)
public:
  char *getVendorString() override { return const_cast<char *>("E5"); }
  char *getProductString() override { return const_cast<char *>("SystemOS read-only window"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(E5SystemOS::uuid); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn reportBlockSize(UInt64 *n) override { *n = 512; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *n) override { *n = E5SystemOS::sectors - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *p, bool *c) override {
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    return host->reportMediaState(p, c);
  }
  IOReturn reportWriteProtection(bool *b) override { *b = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *data, UInt64 block, UInt64 count,
      IOStorageAttributes *attributes, IOStorageCompletion *completion) override {
    (void)attributes;
    if (!data || !completion || !completion->action) return kIOReturnBadArgument;
    if (data->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    if (!E5SystemOS::contains(block, count)) return kIOReturnBadArgument;
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    IOReturn rc = host->windowRead(data, block, count, E5SystemOS::base);
    completion->action(completion->target, completion->parameter, rc,
                       rc == kIOReturnSuccess ? count * 512 : 0);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(E5SystemOSDeviceEmmc, IOBlockStorageDevice)

// r263: the OS root container window, read-only. The wrapper media carries
// the root volume UUID (04769D26) as its device-info string so the APFS
// volume synthesized on top inherits the value the /chosen root-matching
// dictionary waits for; the ADT stays untouched.
class E5OsRootDeviceEmmc : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(E5OsRootDeviceEmmc)
public:
  char *getVendorString() override { return const_cast<char *>("E5"); }
  char *getProductString() override { return const_cast<char *>("OS root window"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(E5OsRoot::uuid); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn reportBlockSize(UInt64 *n) override { *n = 512; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *n) override { *n = E5OsRoot::sectors - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *p, bool *c) override {
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    return host->reportMediaState(p, c);
  }
  IOReturn reportWriteProtection(bool *b) override { *b = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *data, UInt64 block, UInt64 count,
      IOStorageAttributes *attributes, IOStorageCompletion *completion) override {
    (void)attributes;
    if (!data || !completion || !completion->action) return kIOReturnBadArgument;
    if (!E5OsRoot::contains(block, count)) return kIOReturnBadArgument;
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    IOReturn rc = host->windowRead(data, block, count, E5OsRoot::base);
    completion->action(completion->target, completion->parameter, rc,
                       rc == kIOReturnSuccess ? count * 512 : 0);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(E5OsRootDeviceEmmc, IOBlockStorageDevice)

static bool publishOsRootEmmc(NativeEmmc *host) {
  E5OsRootDeviceEmmc *window = new E5OsRootDeviceEmmc;
  if (!window) return false;
  if (!window->init(nullptr)) { window->release(); return false; }
  if (!window->attach(host)) { window->release(); return false; }
  if (!window->start(host)) { window->detach(host); window->release(); return false; }
  elog("NativeEmmc: publishing OsRoot read-only %llu sectors at %llu (uuid %s)\n",
       (unsigned long long)E5OsRoot::sectors, (unsigned long long)E5OsRoot::base,
       E5OsRoot::uuid);
  window->registerService();
  window->release();
  return true;
}

// The system's one writable dataset: the 4 GiB eMMC window served
// media-direct. The kernel startup patch mounts the volume at /private/var
// before launchd starts; every completed write is durable by construction.
class E5DataDeviceEmmc : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(E5DataDeviceEmmc)
public:
  char *getVendorString() override { return const_cast<char *>("E5"); }
  char *getProductString() override { return const_cast<char *>("Data media window"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(E5Data::uuid); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn reportBlockSize(UInt64 *n) override { *n = 512; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *n) override { *n = E5Data::sectors - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *p, bool *c) override {
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    return host->reportMediaState(p, c);
  }
  IOReturn reportWriteProtection(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *data, UInt64 block, UInt64 count,
      IOStorageAttributes *attributes, IOStorageCompletion *completion) override {
    (void)attributes;
    if (!data || !completion || !completion->action) return kIOReturnBadArgument;
    if (!E5Data::contains(block, count)) return kIOReturnBadArgument;
    NativeEmmc *host = OSDynamicCast(NativeEmmc, getProvider());
    if (!host) return kIOReturnNotReady;
    IOReturn rc = host->dataMediaIO(data, block, count,
                                    data->getDirection() == kIODirectionOut);
    completion->action(completion->target, completion->parameter, rc,
                       rc == kIOReturnSuccess ? count * 512 : 0);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(E5DataDeviceEmmc, IOBlockStorageDevice)

static bool publishSystemOSEmmc(NativeEmmc *host) {
  E5SystemOSDeviceEmmc *window = new E5SystemOSDeviceEmmc;
  if (!window) return false;
  if (!window->init(nullptr)) { window->release(); return false; }
  if (!window->attach(host)) { window->release(); return false; }
  if (!window->start(host)) { window->detach(host); window->release(); return false; }
  elog("NativeEmmc: publishing SystemOS read-only %llu sectors at %llu\n",
       (unsigned long long)E5SystemOS::sectors, (unsigned long long)E5SystemOS::base);
  window->registerService();
  window->release();
  return true;
}

// Probe-time Data preparation: gate on the raw-container superblock and the
// container/window block-count match, all before any IOKit consumer can race
// the host (the round 55/56 root-attach lesson).
static E5DataDeviceEmmc *prepareDataDeviceEmmc(NativeEmmc *host) {
  if (!E5Data::fits(host->windowCardSectors())) return nullptr;
  if (!host->readMulti((uint32_t)E5Data::base, 8)) {
    elog("NativeEmmc: data window read failed\n");
    return nullptr;
  }
  const uint8_t *probe = host->dmaPeek();
  if (memcmp(probe + 32, "NXSB", 4)) {
    // r264: the r263 boot probe failed here — readMulti(base) returned
    // zeros while the media verified byte-perfect from Android. Dump what
    // arrived, retry readMulti for consistency, and cross-check the same
    // LBA through the legacy single-sector path before giving up.
    elog("NativeEmmc: data window not NXSB (crc %08x first16 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x)\n",
         host->windowCrc(),
         probe[0], probe[1], probe[2], probe[3], probe[4], probe[5], probe[6], probe[7],
         probe[8], probe[9], probe[10], probe[11], probe[12], probe[13], probe[14], probe[15]);
    for (unsigned r = 0; r < 2; ++r) {
      IOSleep(100);
      if (host->readMulti((uint32_t)E5Data::base, 8))
        elog("NativeEmmc: data probe retry %u crc %08x\n", r, host->windowCrc());
    }
    if (host->legacyReadProbe((uint32_t)E5Data::base))
      elog("NativeEmmc: data probe legacy sdmaRead512 SEES NXSB (readMulti-only fault)\n");
    else
      elog("NativeEmmc: data probe legacy sdmaRead512 also misses NXSB\n");
    return nullptr;
  }
  uint64_t containerBlocks = *(const uint64_t *)(probe + 40);
  elog("NativeEmmc: data media window %llu sectors, container %llu blocks %s\n",
       (unsigned long long)E5Data::sectors, (unsigned long long)containerBlocks,
       containerBlocks * 8 == E5Data::sectors ? "match" : "MISMATCH");
  E5DataDeviceEmmc *device = new E5DataDeviceEmmc;
  if (!device || !device->init(nullptr)) {
    if (device) device->release();
    return nullptr;
  }
  return device;
}

static bool publishDataEmmc(E5DataDeviceEmmc *device, NativeEmmc *host) {
  if (!device->attach(host)) { device->release(); return false; }
  if (!device->start(host)) { device->detach(host); device->release(); return false; }
  elog("NativeEmmc: publishing Data read-write %llu sectors at %llu\n",
       (unsigned long long)E5Data::sectors, (unsigned long long)E5Data::base);
  device->registerService();
  device->release();
  return true;
}

bool NativeEmmc::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  if (partMode) { registerService(); return true; }
  md = IOMemoryDescriptor::withAddressRange(E5_EMMC_BASE, E5_EMMC_LEN, kIODirectionNone, nullptr);
  if (!md) { elog("NativeEmmc: no memory descriptor\n"); return true; }
  map = md->map();
  if (!map) { md->release(); elog("NativeEmmc: map failed\n"); return true; }
  regs = (volatile uint32_t *)map->getVirtualAddress();
  elog("NativeEmmc: regs mapped at %p\n", (void *)regs);

  // One physically-contiguous page below 4 GiB: receive buffer at +0, the
  // ADMA2 descriptor at +512, mirroring the UEFI probe's DMA page layout.
  bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task,
      (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
      8192, 0xFFFFE000ull);
  if (!bmd) { elog("NativeEmmc: dma alloc failed\n"); map->release(); md->release(); return true; }
  bmd->prepare();
  uint32_t *d = (uint32_t *)bmd->getBytesNoCopy();
  uint8_t *d8 = (uint8_t *)d;
  uint64_t dva = bmd->getPhysicalAddress();
  dmaD = d; dmaD8 = d8; dmaPhys = dva; ioLock = IOLockAlloc();
  winD8 = d8 + 4096; winPhys = (uint32_t)(dva + 4096); // r259 second half-page
  elog("NativeEmmc: dma page %08llx\n", (unsigned long long)dva);

  // r268: 64 KiB physically-contiguous bulk buffer for the LS-scan reads.
  bigBMD = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task,
      (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
      65536, 0xFFFF0000ull);
  if (bigBMD) {
    bigBMD->prepare();
    bigV = (uint8_t *)bigBMD->getBytesNoCopy();
    bigPhys = (uint32_t)bigBMD->getPhysicalAddress();
    elog("NativeEmmc: big page %08x\n", bigPhys);
  } else {
    elog("NativeEmmc: big alloc failed (bulk path off)\n");
  }

  cardUp = identifyAndRead(d, d8, dva);
  elog("NativeEmmc: v4 result %s (ocr %08x rca %04x sectors %llu)\n",
       cardUp ? "Success" : "DeviceError", ocr, rca, (unsigned long long)sectors);

  if (cardUp) {
    scanGpt(d, d8, dva);
    verifyReadback(d, d8, dva);
    // r257: the windows need megabytes per second; the identify clock
    // delivers kilobytes. Lift-and-verify, falling back safely.
    liftClock();
    // r265: turn OFF the eMMC's internal cache (EXT_CSD CACHE_CTRL byte 33).
    // Volatile cache-on can serve stale pages for interleaved reads; rule it
    // out as the roaming wrong-content source.
    {
      uint32_t st = cmdNoData(0x061B, (3u << 24) | (33u << 16));
      if (cmdOk(st)) {
        waitLogReady(1000);
        elog("NativeEmmc: ext_csd cache ctrl off y\n");
      } else {
        elog("NativeEmmc: ext_csd cache ctrl off FAIL st %08x\n", st);
      }
    }
    uint32_t logTest = 0;
    if (PE_parse_boot_argn("e5-emmc-log-test", &logTest, sizeof(logTest)) && logTest) {
      IOLockLock(ioLock);
      logWriteProbe(logTest);
      IOLockUnlock(ioLock);
      if (!cardUp) { elog("NativeEmmc: log probe quarantined host\n"); return true; }
    }
    uint32_t journalNonce = 0;
    if (!logTest && PE_parse_boot_argn("e5-emmc-log", &journalNonce, sizeof(journalNonce)) && journalNonce) {
      IOLockLock(ioLock);
      {
      // r251: map the firmware-read panic scratch (plain DRAM at the initrd
      // reserve top) UNCACHED via ml_io_map - the mapping class the prz
      // console tee uses, whose writes demonstrably survive the watchdog
      // reset. r250 mapped this cacheable (IODeviceMemory::map) and the next
      // boot read 0xFFFFFFFF from both zones: not even the init zeros
      // reached DRAM before the hard freeze killed the dirty cache lines.
      // Layout: [0]=magic "E5X1" [1]=journal nonce [2]=worker pass count
      // [3]=uptime ms of that pass; a panic overwrites [0..3] with PANI/pc.
      volatile uint32_t *base = (volatile uint32_t *)ml_io_map(0xC6000000ull, 0x3000ull);
      if (base) {
        sPanicScratch = base;
        sPanicTrace = base + 0x2000 / 4;
        // Read what the PREVIOUS boot left before touching anything: this
        // lands in the console/journal, so the answer is readable offline
        // even if the firmware-side read stays 0xFFFFFFFF.
        elog("NativeEmmc: scratch pre %08x %08x %08x %08x | tr %08x %08x %08x %08x %08x %08x %08x %08x\n",
             base[0], base[1], base[2], base[3],
             base[0x800], base[0x801], base[0x802], base[0x803],
             base[0x804], base[0x805], base[0x806], base[0x807]);
        base[0] = 0x31583545u; // "E5X1"
        base[1] = journalNonce;
        base[2] = 0; base[3] = 0;
        elog("NativeEmmc: panic scratch %s (readback %08x)\n",
             base[0] == 0x31583545u ? "armed" : "VERIFY-FAILED", base[0]);
      } else {
        elog("NativeEmmc: panic scratch FAILED\n");
      }
    }
    bool started = beginJournal(journalNonce);
      IOLockUnlock(ioLock);
      elog("NativeEmmc: journal started %u nonce %u gen %llu slot %u\n", started,
           journalNonce, (unsigned long long)logGeneration, logSlot);
      if (!cardUp) return true;
    }
    // r257: the SystemOS and Data windows live on this eMMC now. Probe both
    // superblocks BEFORE registerService (the r55/56 root-attach race), and
    // publish the wrapper devices after it — the NativeSd publication order.
    bool systemOS = PE_parse_boot_argn("e5-systemos", nullptr, 0);
    bool osWindowOk = false;
    bool osRootOk = false;
    if (systemOS) {
      IOLockLock(ioLock); // r259: journal passes are live by now
      // r272: the r271 boot refused a media-verified window on the FIRST
      // single read (read flake) and lost the whole boot to the cryptex
      // helper timeout — probes retry, each attempt logged with its CRC.
      bool probeOk = false;
      for (unsigned a = 0; !probeOk && a < 3; ++a) {
        probeOk = E5SystemOS::fits(sectors) && readMulti((uint32_t)E5SystemOS::base, 8) &&
            memcmp(winD8 + 32, "NXSB", 4) == 0 &&
            crc32(winD8, 4096) == E5SystemOS::superblockCrc;
        elog("NativeEmmc: SystemOS probe try %u crc %08x %s\n", a, windowCrc(), probeOk ? "y" : "n");
      }
      // r263: OS root window (bare container, same NXSB-at-+32 shape). Its
      // wrapper media carries the root volume UUID so /chosen root-matching
      // binds here before the SD stack registers.
      bool rootOk = false;
      for (unsigned a = 0; !rootOk && a < 3; ++a) {
        rootOk = E5OsRoot::fits(sectors) && readMulti((uint32_t)E5OsRoot::base, 8) &&
            memcmp(winD8 + 32, "NXSB", 4) == 0;
        elog("NativeEmmc: OsRoot probe try %u crc %08x %s\n", a, rootOk ? windowCrc() : 0u,
             rootOk ? "y" : "n");
      }
      IOLockUnlock(ioLock);
      if (probeOk) {
        osWindowOk = true;
        elog("NativeEmmc: SystemOS superblock crc %08x match y\n", E5SystemOS::superblockCrc);
      } else {
        elog("NativeEmmc: SystemOS superblock refused\n");
      }
      if (rootOk) {
        osRootOk = true;
        elog("NativeEmmc: OsRoot NXSB at %llu match y\n",
             (unsigned long long)E5OsRoot::base);
      } else {
        elog("NativeEmmc: OsRoot window refused\n");
      }
    }
    E5DataDeviceEmmc *dataDevice = nullptr;
    if (osWindowOk && PE_parse_boot_argn("e5-data", nullptr, 0)) {
      IOLockLock(ioLock); // r259: probe under the same lock as journal passes
      if (!qualifyWindowWrite()) {
        cardUp = false;
        IOLockUnlock(ioLock);
        elog("E5W25 spare restore failed; refusing window publication\n");
        return true;
      }
      dataDevice = prepareDataDeviceEmmc(this);
      IOLockUnlock(ioLock);
      if (!dataDevice) elog("NativeEmmc: data window NXSB refused\n");
    }
    // v5: keep regs/DMA page alive; the read path runs in doAsyncReadWrite.
    baseLba = E5Emmc::cacheFirstLba;
    if (!E5Emmc::windowFitsCard(sectors, baseLba, cacheSectors)) {
      elog("NativeEmmc: cache window exceeds card capacity\n");
      return true;
    }
    windowPub = true;
    elog("NativeEmmc: v8 read-only service; errors override completion\n");
    elog("NativeEmmc: publishing cache window (%llu sectors @ lba %llx)\n",
         (unsigned long long)cacheSectors, (unsigned long long)baseLba);
    registerService();
    if (osRootOk && !publishOsRootEmmc(this))
      elog("NativeEmmc: OsRoot publication failed\n");
    if (osWindowOk && !publishSystemOSEmmc(this))
      elog("NativeEmmc: SystemOS publication failed\n");
    if (dataDevice && !publishDataEmmc(dataDevice, this))
      elog("NativeEmmc: data publication failed\n");
    // r263: the r262 synchronous t0 wedged inside start() (silent IOLock
    // wait) and stalled IOKit serialization past the root timeout. The
    // self-test now runs ONLY on the delayed thread_call; tags t1..t3.
    stCall = thread_call_allocate(stEntry, this);
    if (stCall) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(45, NSEC_PER_SEC, &deadline);
      thread_call_enter_delayed(stCall, deadline);
    }
    return true;
  }
  resetLines();
  if (bigBMD) { bigBMD->complete(); bigBMD->release(); bigBMD = nullptr; bigV = nullptr; }
  bmd->complete();
  bmd->release();
  map->release();
  md->release();
  // v2 stops here: identity + reads proven, no block service yet.
  return true;
}
