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

static void elog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  e5_panic_print(buf);
}

class NativeEmmc : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeEmmc)
  volatile uint32_t *regs = nullptr;
  uint16_t rca = 0;
  uint32_t ocr = 0;
  uint32_t cid[4] = {0, 0, 0, 0};
  uint32_t csd[4] = {0, 0, 0, 0};
  uint8_t  extCsd[512] = {0};
  uint64_t sectors = 0;
  bool cardUp = false;

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
    return (st & 0xFFFF8000u) == 0 && (st & 0x03u) == 0x03u;
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
  bool scanGpt(uint32_t *d, uint8_t *d8, uint64_t dva) {
    uint32_t st = sdmaRead512(0x113A, 1, d, dva);
    cacheInvalidate(d, 512);
    if ((st & 0x0000FFFFu) == 0x00000001u && d8[0] == 0x45 && d8[1] == 0x46 &&
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
          if ((st & 0x0000FFFFu) == 0x00000001u) {
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
  char *getRevisionString() override { return const_cast<char *>("2"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("UEFI-validated v4 port"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = sectors ? sectors - 1 : 0; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override { *present = cardUp; if (changed) *changed = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override { return kIOReturnUnsupported; }
};
OSDefineMetaClassAndStructors(NativeEmmc, IOBlockStorageDevice)

bool NativeEmmc::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(E5_EMMC_BASE, E5_EMMC_LEN, kIODirectionNone, nullptr);
  if (!md) { elog("NativeEmmc: no memory descriptor\n"); return true; }
  IOMemoryMap *map = md->map();
  if (!map) { md->release(); elog("NativeEmmc: map failed\n"); return true; }
  regs = (volatile uint32_t *)map->getVirtualAddress();
  elog("NativeEmmc: regs mapped at %p\n", (void *)regs);

  // One physically-contiguous page below 4 GiB: receive buffer at +0, the
  // ADMA2 descriptor at +512, mirroring the UEFI probe's DMA page layout.
  IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task,
      (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
      8192, 0xFFFFE000ull);
  if (!bmd) { elog("NativeEmmc: dma alloc failed\n"); map->release(); md->release(); return true; }
  bmd->prepare();
  uint32_t *d = (uint32_t *)bmd->getBytesNoCopy();
  uint8_t *d8 = (uint8_t *)d;
  uint64_t dva = bmd->getPhysicalAddress();
  elog("NativeEmmc: dma page %08llx\n", (unsigned long long)dva);

  cardUp = identifyAndRead(d, d8, dva);
  elog("NativeEmmc: v4 result %s (ocr %08x rca %04x sectors %llu)\n",
       cardUp ? "Success" : "DeviceError", ocr, rca, (unsigned long long)sectors);

  if (cardUp) {
    scanGpt(d, d8, dva);
    // Quiesce the DMA engine before returning the page to the kernel.
    resetLines();
  }
  bmd->complete();
  bmd->release();
  map->release();
  md->release();
  // v2 stops here: identity + reads proven, no block service yet.
  return true;
}
