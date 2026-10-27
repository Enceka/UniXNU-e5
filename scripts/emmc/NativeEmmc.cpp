// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeEmmc: warm-handoff SDHCI driver for the E5's eMMC (sprd,sdhci-r11).
//
// v1 scope: map the controller the Android boot left running (probe in
// E5BringupDxe proved CAPS/PSTATE/DLL alive across the LK->UEFI handoff),
// reset the host, re-identify the card at 400 kHz (CMD0/1/2/3/9/7), read
// EXT_CSD, and IOLog the identity.  Block I/O and IOMedia come later;
// this round only proves MMIO + card protocol from inside XNU.
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>

#define E5_EMMC_BASE 0x22200000ULL
#define E5_EMMC_LEN  0x400ULL

// SDHCI standard registers
#define SDHCI_DMA_ADDR        0x00
#define SDHCI_BLOCK_SIZE      0x04
#define SDHCI_BLOCK_COUNT     0x06
#define SDHCI_ARGUMENT        0x08
#define SDHCI_TRANSFER_MODE   0x0C
#define SDHCI_COMMAND         0x0E
#define SDHCI_RESPONSE        0x10
#define SDHCI_BUFFER_DATA     0x20
#define SDHCI_PRESENT_STATE   0x24
#define SDHCI_HOST_CONTROL    0x28
#define SDHCI_CLOCK_CONTROL   0x2C
#define SDHCI_SOFTWARE_RESET  0x2F
#define SDHCI_INT_STATUS      0x30
#define SDHCI_INT_ENABLE      0x34
#define SDHCI_INT_SIGNAL_ENABLE 0x38
#define SDHCI_CAPABILITIES    0x40
#define SDHCI_CAPABILITIES2   0x44
#define SDHCI_HOST_VERSION    0xFE

#define INT_CMD_COMPLETE      0x00000001
#define INT_XFER_COMPLETE     0x00000002
#define INT_BUF_READ_READY    0x00000020
#define INT_BUF_WRITE_READY   0x00000040
#define INT_ERROR             0x00008000

#define PSTATE_CMD_INHIBIT    0x00000001
#define PSTATE_DAT_INHIBIT    0x00000002

#define CMD_RESP_NONE         0x00
#define CMD_RESP_LONG         0x01 // R2
#define CMD_RESP_SHORT        0x02 // R1/R1b/R3/R4/R5
#define CMD_CRC_ENABLE        0x08
#define CMD_INDEX_ENABLE      0x10
#define CMD_DATA_PRESENT      0x20

#define TM_READ               0x10

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
  uint8_t extCsd[512] = {0};
  uint64_t sectors = 0;
  bool cardUp = false;

  inline uint8_t  r8 (uint32_t o) const { return *(volatile uint8_t *)((uint8_t *)regs + o); }
  inline uint16_t r16(uint32_t o) const { return *(volatile uint16_t *)((uint8_t *)regs + o); }
  inline uint32_t r32(uint32_t o) const { return *(volatile uint32_t *)((uint8_t *)regs + o); }
  inline void w8 (uint32_t o, uint8_t  v) { *(volatile uint8_t  *)((uint8_t *)regs + o) = v; }
  inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)((uint8_t *)regs + o) = v; }
  inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)((uint8_t *)regs + o) = v; }

  bool waitBits(uint32_t off, uint32_t mask, uint32_t want, uint32_t loops, uint32_t stepUs) {
    for (uint32_t i = 0; i < loops; i++) {
      if ((r32(off) & mask) == want) return true;
      IODelay(stepUs);
    }
    return false;
  }

  // SDHCI_SOFTWARE_RESET (0x2F) is not 4-byte aligned; poll it bytewise.
  bool waitBits8(uint32_t off, uint8_t mask, uint8_t want, uint32_t loops, uint32_t stepUs) {
    for (uint32_t i = 0; i < loops; i++) {
      if ((r8(off) & mask) == want) return true;
      IODelay(stepUs);
    }
    return false;
  }

  // Wait for a normal-interrupt bit, W1C it.  Returns false on error/timeout.
  bool waitInt(uint32_t bits, uint32_t loops, uint32_t stepUs) {
    for (uint32_t i = 0; i < loops; i++) {
      uint32_t st = r32(SDHCI_INT_STATUS);
      if (st & INT_ERROR) {
        w32(SDHCI_INT_STATUS, st);
        elog("NativeEmmc: int error 0x%x after cmd\n", st);
        return false;
      }
      if (st & bits) {
        w32(SDHCI_INT_STATUS, st & bits);
        return true;
      }
      IODelay(stepUs);
    }
    elog("NativeEmmc: int timeout waiting 0x%x, status 0x%x\n", bits, r32(SDHCI_INT_STATUS));
    return false;
  }

  // Issue a command without data phases.
  bool cmd(uint16_t index, uint32_t arg, uint16_t flags, uint32_t *resp = nullptr) {
    if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_CMD_INHIBIT, 0, 5000, 10)) {
      elog("NativeEmmc: CMD%u: cmd line stuck (pstate 0x%x)\n", index, r32(SDHCI_PRESENT_STATE));
      return false;
    }
    w32(SDHCI_ARGUMENT, arg);
    w32(SDHCI_INT_STATUS, 0xFFFF); // W1C all
    w16(SDHCI_COMMAND, (uint16_t)(index << 8) | flags);
    if (!waitInt(INT_CMD_COMPLETE, 20000, 10)) return false;
    uint32_t ist = r32(SDHCI_INT_STATUS);
    if (ist & 0xFFFF0000u) {           // error interrupts are in the high half
      elog("NativeEmmc: CMD%u int error 0x%x\n", index, ist);
      w32(SDHCI_INT_STATUS, ist);      // W1C
    }
    if (resp) {
      resp[0] = r32(SDHCI_RESPONSE + 0x0);
      resp[1] = r32(SDHCI_RESPONSE + 0x4);
      resp[2] = r32(SDHCI_RESPONSE + 0x8);
      resp[3] = r32(SDHCI_RESPONSE + 0xC);
    }
    // R1b: wait for the busy (DAT) lines to release.
    if ((flags & CMD_RESP_SHORT) && (flags & CMD_CRC_ENABLE)) {
      if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_DAT_INHIBIT, 0, 50000, 10))
        elog("NativeEmmc: CMD%u: DAT still busy\n", index);
    }
    return true;
  }

  bool hostInit() {
    // Hardware reset of the card: sprd bit3 is active low, pulse it
    // (mirrors sdhci_sprd_hw_reset in e5-linux).
    w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) & (uint8_t)~0x08);
    IODelay(15);
    w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) | 0x08);
    IODelay(400);
    // Soft reset of the cmd+data lines (bit3 must stay set, sprd).
    w8(SDHCI_SOFTWARE_RESET, 0x01 | 0x02 | 0x08);
    if (!waitBits8(SDHCI_SOFTWARE_RESET, 0x03, 0, 100000, 10)) {
      elog("NativeEmmc: host soft reset did not clear (0x%x)\n", r8(SDHCI_SOFTWARE_RESET));
      return false;
    }
    // 400 kHz from the 26 MHz sprd base: SDHCI v3 divider base/(div*10):
    // div = 26e6/(400k*10) = 6.5 -> 7 (371 kHz, within spec tolerance).
    w16(SDHCI_CLOCK_CONTROL, 0);
    w16(SDHCI_CLOCK_CONTROL, 0x0001 | (7 << 8)); // internal clock enable, div 7
    for (uint32_t i = 0; i < 150000; i++) {
      if (r16(SDHCI_CLOCK_CONTROL) & 0x0002) break;
      IODelay(1);
    }
    if (!(r16(SDHCI_CLOCK_CONTROL) & 0x0002)) {
      elog("NativeEmmc: internal clock never stabilised\n");
      return false;
    }
    w16(SDHCI_CLOCK_CONTROL, r16(SDHCI_CLOCK_CONTROL) | 0x0004); // SD clock on
    // vendor init_config: DEBOUNCE DLL_BAK|DLL_VAL before anything else.
    w32(0x28C, r32(0x28C) | 0x3);
    // sprd: disable CLK_AUTO (BUSY_POSI 0x250 bits 25/24) at <=400kHz, or the
    // auto clock gating breaks low-speed command timing (e5-linux sdhci-sprd).
    {
      uint32_t bp = r32(0x250);
      elog("NativeEmmc: busy_posi 0x%x\n", bp);
      bp &= ~((1u << 25) | (1u << 24));
      w32(0x250, bp);
    }
    w8(0x2E, 0x0E); // timeout control: max
    // sprd gates STATUS bits on INT_ENABLE: enable everything so error
    // bits (timeout/CRC, high half) become visible to our polling.
    w32(SDHCI_INT_ENABLE, 0xFFFFu);
    w32(SDHCI_INT_SIGNAL_ENABLE, 0);
    w32(SDHCI_INT_STATUS, 0xFFFF);
    elog("NativeEmmc: host up, caps 0x%x/0x%x clkctl 0x%x\n",
          r32(SDHCI_CAPABILITIES), r32(SDHCI_CAPABILITIES2), r16(SDHCI_CLOCK_CONTROL));
    return true;
  }

  bool cardInit() {
    uint32_t resp[4];
    // ---- discriminator battery ----
    // (a) CMD1 with degenerate args: a real card changes its answer.
    cmd(0, 0, CMD_RESP_NONE);
    IODelay(5000);
    if (!cmd(1, 0x00000000, CMD_RESP_SHORT, resp)) return false;
    elog("bt: cmd1 arg0 -> %08x\n", resp[0]);
    if (!cmd(1, 0xC0FF8000, CMD_RESP_SHORT, resp)) return false;
    elog("bt: cmd1 argC0FF8000 -> %08x\n", resp[0]);
    // (b) loop with iteration telemetry
    uint32_t iter = 0;
    for (uint32_t i = 0; i < 200; i++) {
      iter++;
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT, resp)) return false;
      ocr = resp[0];
      if (ocr & 0x80000000) break;
      IODelay(1000);
    }
    elog("bt: cmd1 loop %u iters, last %08x\n", iter, resp[0]);
    // (c) illegal commands: SDIO CMD52 / SD CMD55 must draw no response.
    cmd(52, 0x00000000, CMD_RESP_SHORT, resp);
    elog("bt: cmd52 resp %08x (absent=>ff?)\n", resp[0]);
    cmd(55, 0x00000000, CMD_RESP_SHORT, resp);
    elog("bt: cmd55 resp %08x\n", resp[0]);
    // (d) CMD2 right here, pstate before/after.
    elog("bt: pstate pre-cmd2 %08x\n", r32(SDHCI_PRESENT_STATE));
    cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, cid);
    elog("bt: cmd2 %08x %08x %08x %08x pstate %08x\n",
          cid[0], cid[1], cid[2], cid[3], r32(SDHCI_PRESENT_STATE));
    // ---- k99: clock-off fabrication test ----
    {
      uint16_t save = r16(SDHCI_CLOCK_CONTROL);
      w16(SDHCI_CLOCK_CONTROL, save & ~0x0004);          // SD clock OFF
      for (volatile uint32_t i = 0; i < 20000; i++) {}
      uint32_t r4[4] = {0,0,0,0};
      bool got = cmd(1, 0x40FF8000, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4);
      elog("clkoff cmd1 -> %s %08x (fabricated if it answers)\n", got ? "complete" : "timeout", r4[0]);
      w16(SDHCI_CLOCK_CONTROL, save | 0x0004);           // clock back on
      for (volatile uint32_t i = 0; i < 20000; i++) {}
      got = cmd(1, 0x40FF8000, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4);
      elog("clkon  cmd1 -> %s %08x\n", got ? "complete" : "timeout", r4[0]);
      // garbage-arg CMD1: real card returns window+busy0 regardless
      got = cmd(1, 0x12345678, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4);
      elog("oddarg cmd1 -> %s %08x\n", got ? "complete" : "timeout", r4[0]);
    }
    // ---- k99: real hw reset then fresh enumeration ----
    {
      uint8_t sr = r8(SDHCI_SOFTWARE_RESET);
      w8(SDHCI_SOFTWARE_RESET, sr & ~0x08);              // RST_n low
      for (volatile uint32_t i = 0; i < 600000; i++) {}  // ~10ms at slow poll
      w8(SDHCI_SOFTWARE_RESET, sr | 0x08);               // release
      for (volatile uint32_t i = 0; i < 60000; i++) {}   // ~1ms
      elog("hwreset done, pstate %08x\n", r32(0x24));
      // fresh 74+ clocks + full enum
      cmd(0, 0, CMD_RESP_NONE, 0);
      for (volatile uint32_t i = 0; i < 400000; i++) {}  // ~10ms idle clocks
      uint32_t r4[4] = {0,0,0,0};
      if (cmd(1, 0x40FF8000, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4)) elog("postreset cmd1 %08x\n", r4[0]);
      if (cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, r4)) elog("postreset cmd2 %08x %08x %08x %08x\n", r4[0], r4[1], r4[2], r4[3]);
      if (cmd(3, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4)) elog("postreset cmd3 %08x\n", r4[0]);
    }
    // ---- end k99 ----
    // ---- k98: full command scan (which indexes move the latch) ----
    for (uint16_t ci = 2; ci <= 63; ci++) {
      uint16_t fl = (ci == 2 || ci == 9 || ci == 10) ? (CMD_RESP_LONG | CMD_CRC_ENABLE)
                                                      : CMD_RESP_SHORT | CMD_CRC_ENABLE;
      uint32_t r4[4] = {0, 0, 0, 0};
      if (!cmd(ci, 0x00000000, fl, r4)) { elog("scan: CMD%u timeout/err\n", ci); continue; }
      if (r4[0] != 0xc01f8000u || r4[1] != 0 || r4[2] != 0) {
        elog("scan: CMD%u ANSWERS %08x %08x %08x %08x\n", ci, r4[0], r4[1], r4[2], r4[3]);
      }
    }
    elog("scan done (stale = c01f8000)\n");
    // ---- k98: RCA sweep via CMD13 SEND_STATUS ----
    static const uint32_t rcas[] = { 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0010, 0x1000, 0x10000, 0x20000, 0x4564, 0xffff };
    for (uint32_t ri = 0; ri < 11; ri++) {
      uint32_t r4[4] = {0,0,0,0};
      if (!cmd(13, rcas[ri] << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4)) { elog("rca %04x: err\n", rcas[ri]); continue; }
      if (r4[0] != 0xc01f8000u) elog("rca %04x ANSWERS %08x\n", rcas[ri], r4[0]);
    }
    elog("rca sweep done\n");
    // ---- end battery ----
    if (!cmd(0, 0, CMD_RESP_NONE)) return false;          // GO_IDLE_STATE
    for (uint32_t i = 0; i < 200; i++) {                    // SEND_OP_COND until power-up
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT, resp)) return false;
      ocr = resp[0];
      if (ocr & 0x80000000) break;
      IODelay(1000);
    }
    // Re-send CMD1 with the card's own OCR (host accepts window) then wait:
    // some cards refuse CID identification until the negotiation closes.
    cmd(1, ocr & 0xFFFFFFFCu, CMD_RESP_SHORT, resp);
    elog("NativeEmmc: negotiated cmd1 -> %08x\n", resp[0]);
    IODelay(100000);   // 100 ms settle before CMD2
    if (!(ocr & 0x80000000)) {
      elog("NativeEmmc: card never powered up, OCR 0x%x\n", ocr);
      return false;
    }
    for (uint32_t tryn = 0; tryn < 20; tryn++) {             // ALL_SEND_CID
      if (!cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, cid)) return false;
      if (cid[0] != 0xFFFFFFFFu || cid[1] != 0xFFFFFFFFu ||
          cid[2] != 0xFFFFFFFFu || cid[3] != 0xFFFFFFFFu) {
        elog("NativeEmmc: cmd2 try%u raw %08x %08x %08x %08x\n",
             tryn, cid[0], cid[1], cid[2], cid[3]);
        if (cid[3] == 0x00FFFFFFu) continue;                 // still the idle-high ghost
        break;                                               // structurally new response
      }
      IODelay(1000);
    }
    rca = (uint16_t)(cid[0] >> 0); // placeholder; replaced by CMD3 below
    if (!cmd(3, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE, resp)) return false;
    elog("NativeEmmc: cmd3 raw %08x\n", resp[0]);
    rca = (uint16_t)(resp[0] >> 16);
    if (!cmd(9, (uint32_t)rca << 16, CMD_RESP_LONG | CMD_CRC_ENABLE, csd)) return false;
    if (!cmd(7, (uint32_t)rca << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE)) return false; // SELECT
    if (!cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE)) return false; // BLOCKLEN
    return true;
  }

  bool readExtCsd() {
    // CMD8 SEND_EXT_CSD: 512-byte single-block read through the buffer port.
    if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_CMD_INHIBIT | PSTATE_DAT_INHIBIT, 0, 5000, 10)) return false;
    w16(SDHCI_BLOCK_SIZE, 512);
    w16(SDHCI_BLOCK_COUNT, 1);
    w32(SDHCI_ARGUMENT, 0);
    w32(SDHCI_INT_STATUS, 0xFFFF);
    w16(SDHCI_TRANSFER_MODE, TM_READ);
    w16(SDHCI_COMMAND, (8 << 8) | CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE | CMD_DATA_PRESENT);
    uint32_t *out = (uint32_t *)extCsd;
    for (uint32_t i = 0; i < 512 / 4; i++) {
      if (!waitInt(INT_BUF_READ_READY, 100000, 10)) return false;
      out[i] = r32(SDHCI_BUFFER_DATA);
    }
    if (!waitInt(INT_XFER_COMPLETE, 100000, 10)) return false;
    sectors = (uint64_t)extCsd[212] | ((uint64_t)extCsd[213] << 8) | ((uint64_t)extCsd[214] << 16) | ((uint64_t)extCsd[215] << 24);
    return true;
  }

public:
  bool start(IOService *provider) override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Unisoc"); }
  char *getProductString() override { return const_cast<char *>("eMMC 5.1 (sprd,sdhci-r11)"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("warm-handoff SDHCI"); }
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
  if (!md) { elog("NativeEmmc: no memory descriptor\n"); return false; }
  IOMemoryMap *map = md->map();
  if (!map) { md->release(); elog("NativeEmmc: map failed\n"); return false; }
  regs = (volatile uint32_t *)map->getVirtualAddress();
  elog("NativeEmmc: regs mapped at %p\n", (void *)regs);
  // LK leftovers in the response file, before any of our resets:
  elog("NativeEmmc: resp0 pre %08x %08x %08x %08x\n",
        r32(SDHCI_RESPONSE), r32(SDHCI_RESPONSE+4), r32(SDHCI_RESPONSE+8), r32(SDHCI_RESPONSE+12));
  bool hostOk = false;
  for (uint32_t a = 0; a < 4 && !hostOk; a++) {
    hostOk = hostInit();
    if (!hostOk) { elog("NativeEmmc: host init retry %u\n", a); IODelay(20000); }
  }
  if (!hostOk) { elog("NativeEmmc: host init failed after retries\n"); return false; }
  if (!cardInit()) { elog("NativeEmmc: card init failed\n"); return false; }
  elog("NativeEmmc: OCR 0x%x RCA 0x%x\n", ocr, rca);
  elog("NativeEmmc: CID %08x %08x %08x %08x\n", cid[0], cid[1], cid[2], cid[3]);
  elog("NativeEmmc: CSD %08x %08x %08x %08x\n", csd[0], csd[1], csd[2], csd[3]);
  // sprd r11 probe matrix: do the CRC/INDEX check flags break the latch?
  {
    uint32_t r4[4];
    cmd(3, 0, CMD_RESP_SHORT, r4);                       // no CRC/INDEX check
    elog("NativeEmmc: probe cmd3 nocrc %08x\n", r4[0]);
    cmd(3, 0, CMD_RESP_SHORT | CMD_INDEX_ENABLE, r4);    // index only
    elog("NativeEmmc: probe cmd3 idx %08x\n", r4[0]);
    cmd(2, 0, CMD_RESP_LONG, r4);                        // long, no crc
    elog("NativeEmmc: probe cmd2 nocrc %08x %08x %08x %08x\n", r4[0], r4[1], r4[2], r4[3]);
    cmd(9, (uint32_t)rca << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, r4);
    elog("NativeEmmc: probe cmd9 crc %08x\n", r4[0]);
    elog("NativeEmmc: pstate %08x clkctl %04x\n", r32(SDHCI_PRESENT_STATE), r16(SDHCI_CLOCK_CONTROL));
  }
  // Divisor sweep: re-init from scratch at slower clocks, vendor DEBOUNCE
  // DLL_BAK|DLL_VAL, 74-clock settle after CMD0, CMD2 re-issued 3x.
  {
    static const uint16_t divs[] = { 7, 15, 31, 63, 127, 255 };
    uint32_t rr[4];
    for (uint32_t d = 0; d < 6; d++) {
      uint16_t div = divs[d];
      w32(0x28C, r32(0x28C) | 0x3);            // vendor init_config
      w16(SDHCI_CLOCK_CONTROL, 0);
      IODelay(50);
      w16(SDHCI_CLOCK_CONTROL, 0x0001 | (uint16_t)(div << 8));
      for (uint32_t i = 0; i < 150000; i++) { if (r16(SDHCI_CLOCK_CONTROL) & 2) break; IODelay(1); }
      w16(SDHCI_CLOCK_CONTROL, r16(SDHCI_CLOCK_CONTROL) | 0x0004);
      IODelay(1000);
      cmd(0, 0, CMD_RESP_NONE);
      IODelay(10000);
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT, rr)) { elog("NativeEmmc: sweep div%u cmd1 fail\n", div); continue; }
      for (uint32_t tryn = 0; tryn < 3; tryn++) {
        if (!cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, rr)) break;
        if (rr[0] != 0xFFFFFFFFu || rr[3] != 0x00FFFFFFu) {
          elog("NativeEmmc: sweep div%u try%u CID %08x %08x %08x %08x\n", div, tryn, rr[0], rr[1], rr[2], rr[3]);
          cid[0]=rr[0]; cid[1]=rr[1]; cid[2]=rr[2]; cid[3]=rr[3];
          d = 6; break;
        }
        IODelay(1000);
      }
      elog("NativeEmmc: sweep div%u cid still ff\n", div);
    }
  }
  if (readExtCsd()) {
    cardUp = true;
    elog("NativeEmmc: EXT_CSD rev %u, HS_TIMING %u, SEC_COUNT %llu (%.1f GiB)\n",
          extCsd[192], extCsd[185], sectors, (double)(sectors * 512) / (1024.0 * 1024.0 * 1024.0));
  } else {
    elog("NativeEmmc: EXT_CSD read failed\n");
  }
  // v1 stops here: identity proven, no block service yet.
  return true;
}
