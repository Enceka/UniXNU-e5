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
#include <IOKit/IOBufferMemoryDescriptor.h>
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
#define SDHCI_TIMEOUT_CONTROL 0x2E
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
#define CMD_DATA_PRESENT      0x20   // linux sdhci.h SDHCI_CMD_DATA=0x20 (bit5); 0x10 is INDEX

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
    for (uint32_t i = 0; i < loops; i = i + 1) {
      if ((r32(off) & mask) == want) return true;
      IODelay(stepUs);
    }
    return false;
  }

  // SDHCI_SOFTWARE_RESET (0x2F) is not 4-byte aligned; poll it bytewise.
  bool waitBits8(uint32_t off, uint8_t mask, uint8_t want, uint32_t loops, uint32_t stepUs) {
    for (uint32_t i = 0; i < loops; i = i + 1) {
      if ((r8(off) & mask) == want) return true;
      IODelay(stepUs);
    }
    return false;
  }

  // Wait for a normal-interrupt bit, W1C it.  Returns false on error/timeout.
  bool waitInt(uint32_t bits, uint32_t loops, uint32_t stepUs) {
    for (uint32_t i = 0; i < loops; i = i + 1) {
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
    // sprd r11: CMD-line reset between commands — the engine hangs after 136-bit R2 responses
    // SDHCI_HW_RESET_CARD (bit3) must stay 1 on sprd or the card stops working
    w8(SDHCI_SOFTWARE_RESET, 0x02 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
    for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x02)) break; }
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
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

  // ---- sprd clock gating: apahb-gate sdio0_eb + ap-clk emmc_2x/emmc_1x ----
  volatile uint32_t *gate = nullptr, *apclk = nullptr;
  bool clocksUp() {
    IOMemoryDescriptor *gmd = IOMemoryDescriptor::withAddressRange(0x20000000, 0x3000, kIODirectionNone, nullptr);
    if (!gmd) { kprintf("NativeEmmc: gate md failed\n"); return false; }
    IOMemoryMap *gmap = gmd->map();
    if (!gmap) { gmd->release(); kprintf("NativeEmmc: gate map failed\n"); return false; }
    gate = (volatile uint32_t *)gmap->getVirtualAddress();
    IOMemoryDescriptor *amd = IOMemoryDescriptor::withAddressRange(0x20010000, 0x200, kIODirectionNone, nullptr);
    if (!amd) { kprintf("NativeEmmc: apclk md failed\n"); return false; }
    IOMemoryMap *amap = amd->map();
    if (!amap) { amd->release(); kprintf("NativeEmmc: apclk map failed\n"); return false; }
    apclk = (volatile uint32_t *)amap->getVirtualAddress();
    kprintf("NativeEmmc: gate0 %08x emmc2x %08x emmc1x %08x\n",
            gate[0], apclk[0x124 / 4], apclk[0x12c / 4]);
    gate[0x1000 / 4] = (1u << 0);           // sdio0_eb SET (enable)
    apclk[0x124 / 4] = 0x00000001;          // emmc_2x: parent ext-26m (sel 1), div 1x
    apclk[0x12c / 4] = 0x00000000;          // emmc_1x: div 1x
    gate[0x1004 / 4] = (1u << 0);           // sdio0 soft reset assert
    IODelay(2000);
    gate[0x2004 / 4] = (1u << 0);           // deassert
    IODelay(20000);
    kprintf("NativeEmmc: clk up: gate0 %08x emmc2x %08x emmc1x %08x\n",
            gate[0], apclk[0x124 / 4], apclk[0x12c / 4]);
    return true;
  }

  bool hostInit() {
    if (!clocksUp()) return false;
    // Hardware reset of the card: sprd bit3 is active low, pulse it

    // Full controller reset + legacy mode (Android/LK left HS400ES strobe + V4 mode: dead at 400kHz)
    w8(0x2F, 0x03);                                  // reset CMD+DAT state machines
    if (!waitBits8(0x2F, 0x03, 0, 100000, 10)) { kprintf("NativeEmmc: soft reset stuck %02x\n", r8(0x2F)); }
    w16(0x3E, 0x0000);                               // HOST_CONTROL2: SDR12/legacy, V4 off, strobe off
    w8(0x28, 0x00);                                  // HOST_CONTROL: 1-bit bus, no DMA/ADMA
    kprintf("NativeEmmc: mode cleared hc2=%04x hc1=%02x\n", r16(0x3E), r8(0x28));
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
    for (uint32_t i = 0; i < 150000; i = i + 1) {
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

  void microscope(const char *tag, uint16_t idx, uint16_t flags, uint32_t arg) {
    w32(0x08, arg);
    w32(0x30, 0xFFFFFFFFu);              // clear all (w1c)
    w32(0x34, 0xFFFFFFFFu);              // enable everything
    uint32_t inhibitSeen = 0, iters = 0, st = 0;
    w16(0x0E, (uint16_t)((idx << 8) | flags));
    for (uint32_t n = 0; n < 5000000; n = n + 1) {
      uint32_t ps = r32(0x24);
      if (ps & 1) inhibitSeen = 1;
      st = r32(0x30);
      if (st & 0x0000FFFFu) { iters = n; break; }
      iters = n;
    }
    kprintf("ms: %s idx%u inh%u iters%u st%08x r %08x %08x %08x %08x\n",
            tag, idx, inhibitSeen, iters, st, r32(0x10), r32(0x14), r32(0x18), r32(0x1C));
    w32(0x30, 0xFFFFFFFFu);
    IODelay(50);
  }

  void runMicroscope() {
    // fresh reset to get the card into READY (cmd1 busy set)
    w8(0x2F, r8(0x2F) & (uint8_t)~0x08); IODelay(15);
    w8(0x2F, r8(0x2F) | 0x08); IODelay(400);
    for (uint32_t k = 0; k < 200; k = k + 1) {
      uint32_t resp[4];
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE, resp)) break;
      if (resp[0] & 0x80000000u) break;
    }
    microscope("cmd1", 1, 0x1A, 0);
    microscope("cmd2-long", 2, 0x09, 0);
    microscope("cmd2-short", 2, 0x1A, 0);
    microscope("cmd2-busy", 2, 0x1B, 0);
    microscope("cmd3-short", 3, 0x1A, 0x10000);
    microscope("cmd54", 54, 0x1A, 0);
    microscope("cmd17-data", 17, 0x3A, 0);
  }

  bool cardInit() {
    // ---- pure JEDEC eMMC enumeration: NO extra commands between states ----
    uint32_t resp[4];

    // CMD0 GO_IDLE_STATE
    if (!cmd(0, 0x00000000, CMD_RESP_NONE, resp)) { elog("e5-emmc: cmd0 fail\n"); return false; }
    elog("e5-emmc: cmd0 ok\n");

    // CMD1 SEND_OP_COND loop until busy set (card ready)
    uint32_t ocr = 0;
    for (uint32_t k = 0; k < 2000; k = k + 1) {
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT, resp)) break;
      ocr = resp[0];
      if (ocr & 0x80000000u) break;
    }
    this->ocr = ocr;
    if (!(ocr & 0x80000000u)) { elog("e5-emmc: cmd1 never ready ocr %08x\n", ocr); }
    elog("e5-emmc: cmd1 ocr %08x\n", ocr);

    // CMD2 ALL_SEND_CID (once) -> IDENT
    if (!cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd2 fail\n"); return false; }
    for (int q = 0; q < 4; q = q + 1) this->cid[q] = resp[q];
    elog("e5-emmc: cid %08x %08x %08x %08x\n", resp[0], resp[1], resp[2], resp[3]);

    // CMD3 SEND_RELATIVE_ADDR (once) -> RCA, card -> STBY
    if (!cmd(3, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd3 fail\n"); return false; }
    this->rca = (uint16_t)(resp[0] >> 16);
    elog("e5-emmc: rca %04x status %08x\n", this->rca, resp[0] & 0xFFFFu);

    // CMD9 SEND_CSD (R2, arg = rca<<16)
    if (!cmd(9, ((uint32_t)this->rca) << 16, CMD_RESP_LONG | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd9 fail\n"); return false; }
    for (int q = 0; q < 4; q = q + 1) this->csd[q] = resp[q];
    elog("e5-emmc: csd %08x %08x %08x %08x\n", resp[0], resp[1], resp[2], resp[3]);

    // CMD7 SELECT_CARD (arg = rca<<16) -> TRAN
    if (!cmd(7, ((uint32_t)this->rca) << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd7 fail\n"); return false; }
    elog("e5-emmc: cmd7 status %08x\n", resp[0]);

    // CMD6 SWITCH (dataless R1b): reset card-side modes inherited from Android/LK.
    // HS_TIMING[185]=1 (High Speed 52MHz) + BUS_WIDTH[183]=1 (1-bit SDR).
    uint32_t rb[4];
    // k152: STAY AT LEGACY 26MHz — HS 52M gave response CRC error (phase).
    // Only reset bus width to 1-bit if the card inherited 8-bit from Android.
    cmd(6, 0x0300B701u, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);   // EXT_CSD[183]=1 (bus 1-bit)
    elog("e5-emmc: cmd6 bus_width r1 %08x\n", rb[0]);

    // Re-tune controller for HS: Android left DLL delays in HS400ES state.
    // Mirrors vendor sdhci_sprd_set_clock(52M) + DTS sprd,phy-delay-mmc-highspeed
    // = <0x3f 0x40 0x3f 0x3f> -> DLL_DLY=0x3f3f403f; mmc0@HS needs DLL invert bits.
    if (this->hsRetune) {   // k152: disabled — variants run at legacy 26M
      uint16_t cc0 = r16(0x2C);
      w16(0x2C, cc0 & (uint16_t)~0x4);   // sd_clk_off (CARD_EN)
      w16(0x2C, 0x0201);                  // ICE + select=2 (EMMC_2X 390M/2 -> 48.75M)
      uint32_t spin = 0;
      while (!(r16(0x2C) & 0x2) && spin < 2000000) spin++;  // wait internal stable
      w16(0x2C, 0x0205);                  // card clock on
      w32(0x250, r32(0x250) | (1u<<24) | (1u<<25));         // BUSY_POSI auto gate
      w32(0x208, r32(0x208) | (1u<<13) | (1u<<21));         // DLL_DLY_OFFSET invert
      w32(0x204, 0x3f3f403fu);            // DLL_DLY = phy-delay-mmc-highspeed
      elog("e5-emmc: hs clk cc %04x->%04x spin %u dly %08x off %08x busy %08x\n",
           cc0, r16(0x2C), spin, r32(0x204), r32(0x208), r32(0x250));
    }

    // CMD16 SET_BLOCKLEN 512
    if (!cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd16 fail\n"); return false; }
    elog("e5-emmc: cmd16 status %08x\n", resp[0]);

    // CMD8 READ_EXT_CSD
    if (!readExtCsd()) { elog("e5-emmc: ext_csd fail\n"); }
    elog("e5-emmc: card up: ocr %08x rca %04x\n", this->ocr, this->rca);
    return true;
  }

  bool hsRetune = false;
  bool readExtCsd() {
    // CMD8 SEND_EXT_CSD: 512-byte single-block read. Try PIO then SDMA.
    if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_CMD_INHIBIT | PSTATE_DAT_INHIBIT, 0, 5000, 10)) return false;
    w32(SDHCI_INT_ENABLE, 0xFFFFFFFFu);
    w32(0x04, 1);   // V4: 0x04 as 32-bit block count

    // vendor dll backup mode (the one sprd init step never run in the live path)
    w32(0x28C, r32(0x28C) | 0x3);
    w8(0x28, r8(0x28) & ~0x03);
    // full sprd PHY DLL init (LTS sdhci_sprd_enable_phy_dll, r11p3 phase=0x2):
    // our clkctl=0 reprogram killed LK's locked DLL and DAT never sampled.
    {
      uint32_t c = r32(0x200);
      c &= ~(0x00200000u | 0x0F040000u);            // clear DLL_EN | ALL_CPST
      w32(0x200, c);
      for (uint32_t w = 0; w < 60000 * 4; w = w + 1) { }
      c = r32(0x200);
      c |= 0x0F040000u | 0x00010000u | 0xc00u | 0x2u; // CPST|SEARCH|INIT|phase2
      w32(0x200, c);
      for (uint32_t w = 0; w < 60000 * 4; w = w + 1) { }
      w32(0x200, r32(0x200) | 0x00200000u);          // DLL_EN
      for (uint32_t w = 0; w < 60000 * 4; w = w + 1) { }
      uint32_t lk = 0;
      for (uint32_t w = 0; w < 60000 * 40; w = w + 1) {
        lk = r32(0x210);
        if (lk & 0x40000u) break;
      }
      elog("e5-emmc: dll cfg %08x sts %08x %s\n", r32(0x200), lk, (lk & 0x40000u) ? "LOCKED" : "unlocked");
    {
      // byte-granular probe: where does a distinctive count actually land?
      w16(0x06, 0xA5A5);
      elog("e5-emmc: after w06=A5A5: b04=%02x b05=%02x b06=%02x b07=%02x r04=%08x\n",
           r8(0x04), r8(0x05), r8(0x06), r8(0x07), r32(0x04));
      // CQ block lives at base+0x300 (DTS reg triple #2). If CQE is enabled,
      // the legacy SDHCI data path is bypassed. Probe + halt + retry legacy.
      {
        uint32_t cq[8];
        for (uint32_t i = 0; i < 8; i = i + 1) cq[i] = r32(0x300 + 4 * i);
        elog("e5-emmc: cq %08x %08x %08x %08x %08x %08x %08x %08x\n",
             cq[0], cq[1], cq[2], cq[3], cq[4], cq[5], cq[6], cq[7]);
        // halt CQE (CQ_CTL=0x0C bit0) and disable (CQ_CFG=0x08)
        w32(0x30C, 1);
        for (uint32_t n = 0; n < 50000; n = n + 1) { if (r32(0x30C) & 1) break; }
        if (cq[1] & 1) { w32(0x308, cq[1] & ~1u); }   // clear CQ_CFG enable if set
        elog("e5-emmc: cq halted ctl %08x cfg %08x\n", r32(0x30C), r32(0x308));
        elog("e5-emmc: post-halt blk %04x/%04x hc2 %04x\n",
             r16(0x04), r16(0x06), r16(0x3E));
        // legacy block regs retry
        w16(0x04, 512); w16(0x06, 1);
        elog("e5-emmc: retry blk %04x/%04x\n", r16(0x04), r16(0x06));
      }
      {
        // ---- k150: U-Boot ground truth transcription (uboot_a @0xa8bd8-0xa8c5c) ----
        // U-Boot sprd_sdhci_send_command SDMA sequence, verbatim:
        //   w32(0x30,0xFFFFFFFF); w32(0x58,buf_lo); w32(0x5C,buf_hi);
        //   w16(0x04, blksz|0x7000); w32(0x00, blocks); w32(0x08, arg);
        //   w32(0x0C, (cmd<<16)|TM)  with TM=0x3 (DMA|BCNT, NO read-direction bit)
        // Three variants: exact / +read-bit / -boundary to isolate each delta.
        IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionIn, 8192, 0x7FFFF000ull);
        if (bmd) {
          uint32_t *dt = (uint32_t *)bmd->getBytesNoCopy();
          uint64_t dva = bmd->getPhysicalAddress();
          static const struct { const char *tag; uint32_t tm; uint32_t dly; } dvars[3] = {
            { "FLADMA ", 0x13u, 0xFFFFFFFFu },  // k159: ADMA + flushed desc table + ivac data
            { "FLADMA2", 0x13u, 0xFFFFFFFFu },  // repeat after reselect
            { "FLSDMA ", 0x3u, 0xFFFFFFFFu },   // control: sdma-style TM
          };
          // k158: DMA buffer non-coherent — invalidate dcache lines before reading
          auto invRange = [](volatile void *p, uint32_t n) {
            uint64_t a = ((uint64_t)p) & ~63ull;
            uint64_t e = ((uint64_t)p) + n;
            for (; a < e; a += 64) __asm__ volatile ("dc ivac, %0" :: "r"(a));
            __asm__ volatile ("dsb sy" ::: "memory");
          };
          uint32_t dlyLK0 = r32(0x204);
          elog("e5-emmc: dlyLK initial %08x dly_off %08x\n", dlyLK0, r32(0x208));
          for (uint32_t vi = 0; vi < 4; vi = vi + 1) {
            uint32_t *d = (uint32_t *)((uint8_t *)dt + 0x100 * (vi + 1)); (void)d;
            for (uint32_t i = 0; i < 128; i = i + 1) d[i] = 0xCAFE0000u + vi;
            if (dvars[vi].dly != 0xFFFFFFFFu) w32(0x204, dvars[vi].dly);
            __asm__ volatile ("dmb sy" ::: "memory");
            w8(SDHCI_SOFTWARE_RESET, 0x06 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
            for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x06)) break; }
            // k154: unstick card — CMD12 STOP (R1b) + wait busy + CMD7 re-select
            {
              uint32_t dummy[4];
              w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
              w16(SDHCI_TRANSFER_MODE, 0);
              w16(SDHCI_COMMAND, (12 << 8) | 0x03u | 0x08u | 0x10u);  // R1b select=11
              for (uint32_t n = 0; n < 60000 * 20; n = n + 1) {
                if (r32(SDHCI_INT_STATUS) & 0x01u) break;
              }
              for (uint32_t n = 0; n < 60000 * 20; n = n + 1) {
                if (!(r32(SDHCI_PRESENT_STATE) & 0x02u)) break;       // DAT_INHIBIT clear
              }
              cmd(7, ((uint32_t)this->rca) << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, dummy);
            }
            // k156: U-Boot per-request interrupt trio (uboot_a @0xa7d34)
            w32(0x3C, r32(0x3C) | 0x20000000u);   // FORCE_EVENT bit29
            w32(0x34, 0x027F003Bu);               // INT_ENABLE
            w32(0x38, 0x027F803Bu);               // SIGNAL_ENABLE
            w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);              // clear all int status
            w8(0x28, r8(0x28) & ~0x18u);                     // HC: SDMA select
            uint64_t bdva = dva + 0x100u * (vi + 1);
            uint64_t tva = (uint64_t)dt + 0x1000u + 0x20u * vi; // desc table VA (k161: VA not PA!)
            uint64_t tdva = dva + 0x1000u + 0x20u * vi;           // desc table PA for 0x58
            // k159: build 16B v4 descriptor (linux values) at table, then NOP_END
            volatile uint16_t *dt16 = (volatile uint16_t *)tva;
            volatile uint32_t *dt32 = (volatile uint32_t *)tva;
            dt16[0] = 0x0021u; dt16[1] = 512u;                // TRAN_VALID, len
            dt32[1] = (uint32_t)bdva; dt32[2] = (uint32_t)(bdva >> 32);
            dt16[8] = 0x0003u; dt16[9] = 0;                   // NOP_END_VALID
            dt32[5] = 0; dt32[6] = 0;
            __asm__ volatile ("dc cvac, %0" :: "r"(tva & ~63ull));
            __asm__ volatile ("dc cvac, %0" :: "r"((tva & ~63ull) + 64));
            __asm__ volatile ("dsb sy" ::: "memory");
            w16(0x3E, (uint16_t)(r16(0x3E) | 0x0001u));      // HC2 |= 64BIT_ADDR
            w16(SDHCI_HOST_CONTROL, (uint16_t)((r16(SDHCI_HOST_CONTROL) & ~0x18u) | 0x10u)); // ADMA2 select
            w32(0x58, (uint32_t)tdva); w32(0x5C, (uint32_t)(tdva >> 32));  // TABLE pointer
            w16(0x06, 0);                                     // clear legacy count
            w16(0x04, 512u);
            w16(0x00, 1);                                     // v4 16-bit count
            w32(SDHCI_ARGUMENT, 0);                          // LBA 0
            w32(0x0C, (((17u << 8) | 0x1Au | 0x20u) << 16) | dvars[vi].tm);  // 0x3A + TM
            uint32_t stx = 0;
            for (uint32_t w = 0; w < 60000 * 60; w = w + 1) {
              stx = r32(SDHCI_INT_STATUS);
              if (stx & (0x01u | 0x20u | 0xFFFF0000u)) break;
            }
            uint32_t stc = 0;
            for (uint32_t w = 0; w < 60000 * 60; w = w + 1) {      // k153: TC poll ALWAYS
              stc = r32(SDHCI_INT_STATUS);
              if (stc & (0x02u | 0x00080000u | 0x00100000u)) break; // TC / DMA err / ADMA err
              if ((stc & 0xFFFF0000u) && !(stx & 0x01u)) break;     // early err w/o CC
            }
            __asm__ volatile ("dmb sy" ::: "memory");
            invRange((volatile void *)bdva, 512);            // k159: pull DMA data from RAM
            elog("e5-emmc: %s st %08x tc %08x r1 %08x d0 %08x d1 %08x\n",
                 dvars[vi].tag, stx, stc, r32(SDHCI_RESPONSE), *(volatile uint32_t *)bdva, *(((volatile uint32_t *)bdva) + 1));
            elog("e5-emmc: %s rsp %08x %08x %08x %08x\n", dvars[vi].tag,
                 r32(0x10), r32(0x14), r32(0x18), r32(0x1C));
            elog("e5-emmc: %s abuf %08x blkc %08x/%08x proc %08x pstate %08x\n",
                 dvars[vi].tag, r32(0x220), r32(0x228), r32(0x22c), r32(0x240), r32(SDHCI_PRESENT_STATE));
          }
          elog("e5-emmc: k150 dva %08x\n", (uint32_t)dva);
        } else {
          elog("e5-emmc: k150 alloc fail\n");
        }
      }
    }
    }
    w8(SDHCI_TIMEOUT_CONTROL, 0x0E);   // data timeout counter
    {
      // ---- register write-stick map probe ----
      {
        w32(0x00, 0x12345678u); w32(0x3C, 0x0A0B0C0Du);
        w16(0x0C, 0x0013u); w16(0x04, 0x0222u); w16(0x06, 0x0001u);
        w32(0x04, 0x00010222u);
        w8(0x28, (r8(0x28) & ~0x18u) | 0x10u);
        w8(0x2B, 0x0Eu); w8(0x2E, 0x0Eu);
        elog("e5-emmc: ws sys %08x.%08x tm %04x blk %08x hc %02x to %02x%02x\n",
             r32(0x00), r32(0x3C), r16(0x0C), r32(0x04), r8(0x28), r8(0x2B), r8(0x2E));
        elog("e5-emmc: vnd %08x %08x %08x %08x %08x %08x\n",
             r32(0x238), r32(0x23C), r32(0x240), r32(0x244), r32(0x250), r32(0x28C));
      }
      // CMD17 probe: plain single-block read of LBA0 via PIO
      w32(0x04, 1);   // V4: 0x04 may be the 32-bit block count
      w32(SDHCI_ARGUMENT, 0);
      w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
      w32(SDHCI_INT_ENABLE, 0xFFFFFFFFu);
      w16(SDHCI_TRANSFER_MODE, 0x10);
      w16(SDHCI_COMMAND, (17 << 8) | CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE | CMD_DATA_PRESENT);
      uint32_t st = 0;
      uint32_t got = 0;
      for (uint32_t n = 0; n < 300000; n = n + 1) {
        st = r32(SDHCI_INT_STATUS);
        if (st & INT_BUF_READ_READY) { got = 1; break; }
        if (st & 0xFFFF0000u) break;
      }
      uint32_t w0 = got ? r32(SDHCI_BUFFER_DATA) : 0;
      elog("e5-emmc: cmd17 st %08x r1 %08x buf0 %08x\n", st, r32(SDHCI_RESPONSE), w0);
      if (!got) {
        for (uint32_t s = 0; s < 8; s = s + 1) {
          elog("e5-emmc: s%u pstate %08x\n", s, r32(SDHCI_PRESENT_STATE));
          for (uint32_t w = 0; w < 60000; w = w + 1) { }
        }
      }
      w8(SDHCI_SOFTWARE_RESET, 0x06 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
      for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x06)) break; }
    }
    // ---- variant A: PIO buffer-port read ----
    w32(SDHCI_ARGUMENT, 0);
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w16(SDHCI_TRANSFER_MODE, TM_READ);
    w16(SDHCI_COMMAND, (8 << 8) | CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE | CMD_DATA_PRESENT);
    if (waitInt(INT_CMD_COMPLETE, 20000, 10) && waitInt(INT_BUF_READ_READY, 100000, 10)) {
      uint32_t *out = (uint32_t *)extCsd;
      for (uint32_t i = 0; i < 512 / 4; i = i + 1) {
        out[i] = r32(SDHCI_BUFFER_DATA);
        if (i != 127) { if (!waitInt(INT_BUF_READ_READY, 100000, 10)) break; }
      }
      waitInt(INT_XFER_COMPLETE, 100000, 10);
      elog("e5-emmc: ext_csd pio rev %u hs %u sec lo %02x%02x\n",
           extCsd[192], extCsd[185], extCsd[213], extCsd[212]);
      sectors = (uint64_t)extCsd[212] | ((uint64_t)extCsd[213] << 8) | ((uint64_t)extCsd[214] << 16) | ((uint64_t)extCsd[215] << 24);
      return true;
    }
    elog("e5-emmc: pio fail st %08x pstate %08x\n", r32(SDHCI_INT_STATUS), r32(SDHCI_PRESENT_STATE));

    // ---- variant B: SDMA ----
    IOPhysicalAddress pa = 0;
    void *dmaBuf __attribute__((unused)) = nullptr;
    {
      IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionIn, 8192, 0x7FFFF000ull);
      if (bmd) {
        dmaBuf = bmd->getBytesNoCopy();
        pa = bmd->getPhysicalAddress();
        elog("e5-emmc: sdma va %p pa %08llx\n", dmaBuf, (unsigned long long)pa);
      }
    }
    if (!dmaBuf || !pa) {
      elog("e5-emmc: sdma alloc fail\n");
      return false;
    }
    uint8_t *va = (uint8_t *)dmaBuf;
    elog("e5-emmc: sdma buf pa %08llx\n", (unsigned long long)pa);
    w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    w32(0x48, (uint32_t)pa);              // SDMA system address
    w16(SDHCI_TRANSFER_MODE, TM_READ | 0x0001);  // DMA enable + read
    w16(SDHCI_COMMAND, (8 << 8) | CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE | CMD_DATA_PRESENT);
    if (waitInt(INT_CMD_COMPLETE, 20000, 10)) {
      uint32_t guard = 0;
      while (!(r32(SDHCI_INT_STATUS) & (INT_XFER_COMPLETE | 0x80000000u | 0x8000u))) {
        if (r32(SDHCI_INT_STATUS) & 0x8u) {          // SDMA boundary: rewrite address
          w32(SDHCI_INT_STATUS, 0x8u);
          w32(0x48, (uint32_t)pa);
        }
        if (++guard > 200000000u) break;
      }
      uint32_t st = r32(SDHCI_INT_STATUS);
      if (st & INT_XFER_COMPLETE) {
        memcpy(extCsd, (const void *)va, 512);
        elog("e5-emmc: ext_csd sdma rev %u hs %u sec lo %02x%02x\n",
             extCsd[192], extCsd[185], extCsd[213], extCsd[212]);
        sectors = (uint64_t)extCsd[212] | ((uint64_t)extCsd[213] << 8) | ((uint64_t)extCsd[214] << 16) | ((uint64_t)extCsd[215] << 24);
        return true;
      }
      elog("e5-emmc: sdma fail st %08x\n", st);
    }

    // ---- variant C: ADMA2 ----
    w8(SDHCI_SOFTWARE_RESET, 0x04 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
    for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x04)) break; }
    if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_CMD_INHIBIT | PSTATE_DAT_INHIBIT, 0, 5000, 10)) {
      elog("e5-emmc: adma: dat busy\n");
      return false;
    }
    {
      // descriptor at buf+0, data lands at buf+512
      uint32_t tabPA = (uint32_t)pa;
      uint32_t datPA = tabPA + 512;
      uint8_t *tab = va;
      for (uint32_t i = 0; i < 16; i = i + 1) tab[i] = 0;
      tab[0] = 0x23;                       // VALID | TRAN | END
      tab[2] = 512 & 0xff; tab[3] = (512 >> 8) & 0xff;
      tab[4] = datPA & 0xff; tab[5] = (datPA >> 8) & 0xff;
      tab[6] = (datPA >> 16) & 0xff; tab[7] = (datPA >> 24) & 0xff;
      w16(SDHCI_BLOCK_SIZE, 512 | (7 << 12));   // SDMA boundary 512K << hmm, keep 512
      w16(SDHCI_BLOCK_COUNT, 1);
      w32(0x58, tabPA);                    // ADMA_SYS_ADDRESS
      w32(SDHCI_ARGUMENT, 0);
      w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
      w32(SDHCI_INT_ENABLE, 0xFFFFFFFFu);
      w16(SDHCI_TRANSFER_MODE, 0x11);      // DMA_EN | read
      w16(SDHCI_COMMAND, (8 << 8) | CMD_RESP_SHORT | CMD_CRC_ENABLE | CMD_INDEX_ENABLE | CMD_DATA_PRESENT);
      uint32_t st = 0;
      for (uint32_t n = 0; n < 200000; n = n + 1) {
        st = r32(SDHCI_INT_STATUS);
        if (st & (INT_XFER_COMPLETE | 0xFFFF0000u)) break;
      }
      elog("e5-emmc: adma st %08x\n", st);
      if (st & INT_XFER_COMPLETE) {
        for (uint32_t i = 0; i < 512; i = i + 1) extCsd[i] = va[512 + i];
        elog("e5-emmc: adma ext_csd rev %u hs %u\n", extCsd[192], extCsd[185]);
        sectors = (uint64_t)extCsd[212] | ((uint64_t)extCsd[213] << 8) | ((uint64_t)extCsd[214] << 16) | ((uint64_t)extCsd[215] << 24);
        elog("e5-emmc: adma sectors %llu\n", (unsigned long long)sectors);
        return true;
      }
    }
    return false;
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
  // post-R2 recovery battery: the engine dies after the first R2; try resets
  {
    uint32_t rb[4];
    cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, rb);
    elog("b1 cid %08x %08x %08x %08x\n", rb[0], rb[1], rb[2], rb[3]);
    // variant A: CMD|DATA line reset
    w8(SDHCI_SOFTWARE_RESET, 0x06 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
    for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x06)) break; }
    cmd(3, 0x00010000, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b2 cmd3-after-0x06 %08x\n", rb[0]);
    // variant B: full reset + clock re-init
    cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, rb);
    elog("b3 cid2 %08x\n", rb[0]);
    w8(SDHCI_SOFTWARE_RESET, 0x07 | (r8(SDHCI_SOFTWARE_RESET) & 0x08));
    for (uint32_t n = 0; n < 50000; n = n + 1) { if (!(r8(SDHCI_SOFTWARE_RESET) & 0x07)) break; }
    w16(SDHCI_CLOCK_CONTROL, 0x0001 | (7 << 8));
    for (uint32_t n = 0; n < 150000; n = n + 1) { if (r16(SDHCI_CLOCK_CONTROL) & 0x0002) break; }
    w16(SDHCI_CLOCK_CONTROL, r16(SDHCI_CLOCK_CONTROL) | 0x0004);
    w32(SDHCI_INT_ENABLE, 0x00000003);
    cmd(1, 0x40FF8000, CMD_RESP_SHORT, rb);
    elog("b4 cmd1-after-full %08x\n", rb[0]);
    cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, rb);
    elog("b5 cid-after-full %08x %08x\n", rb[0], rb[3]);
    cmd(3, 0x00010000, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b2 cmd3 arg0001 %08x\n", rb[0]);
    cmd(3, 0x55550000, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b3 cmd3 arg5555 %08x\n", rb[0]);
    cmd(3, 0x00400000, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b4 cmd3 arg0040 %08x\n", rb[0]);
    cmd(9, 0x00010000, CMD_RESP_LONG | CMD_CRC_ENABLE, rb);
    elog("b5 cmd9 rca0001 %08x %08x\n", rb[0], rb[1]);
    cmd(9, 0x00400000, CMD_RESP_LONG | CMD_CRC_ENABLE, rb);
    elog("b6 cmd9 rca0040 %08x %08x\n", rb[0], rb[1]);
    cmd(7, 0x00400000, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b7 cmd7 %08x\n", rb[0]);
    cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b8 cmd16 %08x\n", rb[0]);
    cmd(8, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);
    elog("b9 cmd8 %08x\n", rb[0]);
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
      for (uint32_t i = 0; i < 150000; i = i + 1) { if (r16(SDHCI_CLOCK_CONTROL) & 2) break; IODelay(1); }
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
