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

    // k208: CORRECT sprd engine reset - Linux sdhci_reset(CMD|DATA) writes 0x06
    // (RESET_CMD=0x02|RESET_DATA=0x04), NEVER RESET_ALL(0x01) which wipes the
    // whole core (k204-D wedge), and the writeb accessor PRESERVES bit3 RST_n
    // ("clearing it by mistake will lead the card not work" - sdhci-sprd.c).
    {
      uint8_t v = r8(0x2F);
      v = (uint8_t)((v & 0x08u) | 0x06u);
      w8(0x2F, v);
      if (!waitBits8(0x2F, 0x06, 0, 100000, 10))
        elog("NativeEmmc: engine reset slow %02x\n", r8(0x2F));
    }
    w16(0x3E, 0x0000);                               // HOST_CONTROL2: SDR12/legacy, V4 off, strobe off
    w8(0x28, 0x00);                                  // HOST_CONTROL: 1-bit bus, no DMA/ADMA
    kprintf("NativeEmmc: mode cleared hc2=%04x hc1=%02x\n", r16(0x3E), r8(0x28));
    // (mirrors sdhci_sprd_hw_reset in e5-linux).
    w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) & (uint8_t)~0x08);
    IODelay(15);
    w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) | 0x08);
    IODelay(400);
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
    // k184: at <=400kHz the sprd driver clears the DLL delays entirely
    // (linux a816c path). LK's HS400 tuning values left here corrupt legacy
    // sampling - zero them.
    w32(0x204, 0);
    w32(0x208, r32(0x208) & ~((1u << 13) | (1u << 21) | (1u << 5) | (1u << 6)));
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
    // k196: cold-booted eMMC memory init can take seconds; poll with real
    // delays (2ms each, up to ~8s). Fail -> one more power cycle + retry.
    uint32_t ocr = 0;
    for (uint32_t k = 0; k < 4000; k = k + 1) {
      if (!cmd(1, 0x40FF8000, CMD_RESP_SHORT, resp)) break;
      ocr = resp[0];
      if (ocr & 0x80000000u) break;
      IODelay(2000);
    }
    this->ocr = ocr;
    if (!(ocr & 0x80000000u)) { elog("e5-emmc: cmd1 never ready ocr %08x\n", ocr); }
    elog("e5-emmc: cmd1 ocr %08x\n", ocr);

    // CMD2 ALL_SEND_CID (once) -> IDENT
    if (!cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd2 fail\n"); return false; }
    for (int q = 0; q < 4; q = q + 1) this->cid[q] = resp[q];
    elog("e5-emmc: cid %08x %08x %08x %08x\n", resp[0], resp[1], resp[2], resp[3]);

    // CMD3 SET_RELATIVE_ADDR (eMMC: HOST supplies RCA in arg; Android uses 0x0001,
    // k83-era LK used 0x0002 - never 0, because cmd7(0) = DESELECT_ALL)
    if (!cmd(3, 0x00010000u, CMD_RESP_SHORT | CMD_CRC_ENABLE, resp)) { elog("e5-emmc: cmd3 fail\n"); return false; }
    this->rca = 0x0001u;
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
    cmd(6, 0x03B70100u, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb);   // EXT_CSD[183]=1 (bus 1-bit) linux arg fmt
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

    // k190: RST_n discriminator. Pulse the hw reset line, then CMD13 with the
    // freshly assigned RCA. Timeout => card fell to pre-idle (reset WORKS).
    // 0x900 => card stayed in TRAN (RST_n does not reach the card).
    {
      uint32_t r13[4];
      w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) & (uint8_t)~0x08); IODelay(15);
      w8(SDHCI_SOFTWARE_RESET, r8(SDHCI_SOFTWARE_RESET) | 0x08); IODelay(400);
      bool alive = cmd(13, ((uint32_t)this->rca) << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, r13);
      elog("e5-emmc: k190 rstn alive=%d r1 %08x\n", alive ? 1 : 0, r13[0]);
      if (!alive) {
        // card fell to pre-idle: re-enumerate to TRAN
        cmd(0, 0, 0, r13);
        for (uint32_t k = 0; k < 200; k++) {
          if (!cmd(1, 0x40FFC000u, CMD_RESP_SHORT | CMD_CRC_ENABLE, r13)) break;
          if (r13[0] & 0x80000000u) break;
        }
        uint32_t cid[4];
        cmd(2, 0, CMD_RESP_LONG | CMD_CRC_ENABLE, cid);
        uint32_t r3[4];
        if (!cmd(3, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, r3)) { elog("e5-emmc: reenum cmd3 fail\n"); return false; }
        this->rca = (uint16_t)(r3[0] >> 16);
        cmd(7, ((uint32_t)this->rca) << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, r13);
        cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, r13);
        elog("e5-emmc: k190 reenum rca %04x r1 %08x\n", this->rca, r13[0]);
      }
    }

    // CMD8 READ_EXT_CSD
    if (!readExtCsd()) { elog("e5-emmc: ext_csd fail\n"); }
    elog("e5-emmc: card up: ocr %08x rca %04x\n", this->ocr, this->rca);
    return true;
  }

  bool hsRetune = false;
  bool readExtCsd() {
    // k205: lean single-probe version. The full k126-k203 ladder is retired; the
    // attribution now lives in the k204 block in start(). One U-Boot-verbatim
    // SDMA single-block read of sector 0x10000, full telemetry, return false
    // (the read has never succeeded; the caller just logs "ext_csd fail").
    if (!waitBits(SDHCI_PRESENT_STATE, PSTATE_CMD_INHIBIT | PSTATE_DAT_INHIBIT, 0, 5000, 10)) return false;
    IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 8192, 0x7FFFF000ull);
    if (!bmd) return false;
    bmd->prepare();
    uint32_t *d = (uint32_t *)(bmd->getBytesNoCopy());
    uint64_t dva = bmd->getPhysicalAddress();
    for (uint32_t i = 0; i < 128; i++) d[i] = 0xCAFE0000u;
    for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
      __asm__ volatile ("dc cvac, %0" :: "r"(la));
    // k216: THE DMA SELECT FIX. HOST_CONTROL bits[4:3]: 01=SDMA(0x08) 10=ADMA2(0x10).
    // w8(0x28,0x20) all along = 8-bit bus + DMA OFF -> engine armed but no mover.
    // k215 showed: hs1 accepted clean, clock rate changes behavior (26M TC@0.75s).
    {
      // k218: post-TM-fix retests. The working sequence (clock 26M, HS, SDMA)
      // now exists - redo the experiments that ran under broken no-TM code.
      uint32_t rr6[4];
      cmd(6, 0x0300B901u, CMD_RESP_SHORT | CMD_CRC_ENABLE, rr6);   // HS_TIMING=1
      elog("e5-emmc: k218 hs1 r1 %08x\n", rr6[0]);
      w32(0x2C, 0x08040200u);
      w32(0x2C, 0x08040202u);
      { uint32_t g = 0; while (g < 20000u) { if (r16(0x2C) & 0x2u) break; g++; } }
      w32(0x2C, 0x08040207u);
      { for (uint32_t q = 0; q < 2000; q++) (void)r32(0x24); }
      // helper lambda-ish: one armed read; returns spins; logs
      #define K218_ARM(tag, bufDva, to) do { \
        for (uint32_t q = 0; q < 128; q++) d[q] = 0xCAFE0000u; \
        for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64) \
          __asm__ volatile ("dc cvac, %0" :: "r"(la)); \
        { uint8_t rrx = r8(0x2F); w8(0x2F, (uint8_t)((rrx & 0x08u) | 0x06u)); \
          if (!waitBits8(0x2F, 0x06, 0, 100000, 10)) elog("e5-emmc: k218 rst wedge\n"); } \
        w32(0x30, 0xFFFFFFFFu); w16(0x06, 0); w16(0x04, 0x0200u); \
        w8(0x2B, (to)); \
        w32(0x00, 1); w32(0x58, (uint32_t)(bufDva)); w32(0x5C, 0); \
        w8(0x28, 0x08u); w32(0x08, 0x00010000u); w16(0x0C, 0x0013u); \
        w16(0x0E, (17u << 8) | 0x3Au); \
        { uint32_t stx = 0, spx = 0; \
          while (spx < 6000000u) { stx = r32(0x30); if ((stx & 0x2u) || (stx & 0xFFFF0000u)) break; spx++; } \
          __asm__ volatile ("dsb sy" ::: "memory"); \
          __asm__ volatile ("dc ivac, %0" :: "r"((void*)d) : "memory"); \
          __asm__ volatile ("dsb sy" ::: "memory"); \
          elog("e5-emmc: k218 %s st %08x d0 %08x d1 %08x ps %08x sp %u\n", \
               (tag), stx, d[0], d[1], r32(0x24), spx); \
          if (d[0] != 0xCAFE0000u && d[0] != 0) elog("e5-emmc: k218 *** DATA *** %08x %08x\n", d[2], d[3]); } \
        w32(0x30, 0xFFFFFFFFu); \
        { uint32_t rbx[4]; cmd(12, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx); \
          cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx); } \
      } while (0)

      // B1: LK's own buffer PA (k176 rerun with WORKING sequence)
      {
        IOMemoryDescriptor *lmd = IOMemoryDescriptor::withAddressRange(
            0xBF6C8CC0ull, 0x1000, kIODirectionNone, nullptr);
        IOMemoryMap *lmap = lmd ? lmd->map() : nullptr;
        if (lmap) {
          volatile uint32_t *ld = (volatile uint32_t *)lmap->getVirtualAddress();
          elog("e5-emmc: k218 lkbuf pre %08x\n", ld[0]);
          // point SDMA at LK's physical buffer, keep our d[] for comparison
          for (uint32_t q = 0; q < 128; q++) d[q] = 0xCAFE0000u;
          for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
            __asm__ volatile ("dc cvac, %0" :: "r"(la));
          { uint8_t rrx = r8(0x2F); w8(0x2F, (uint8_t)((rrx & 0x08u) | 0x06u));
            if (!waitBits8(0x2F, 0x06, 0, 100000, 10)) elog("e5-emmc: k218 rst wedge\n"); }
          w32(0x30, 0xFFFFFFFFu); w16(0x06, 0); w16(0x04, 0x0200u);
          w8(0x2B, 0x0Eu);
          w32(0x00, 1); w32(0x58, 0xBF6C8CC0u); w32(0x5C, 0);
          w8(0x28, 0x08u); w32(0x08, 0x00010000u); w16(0x0C, 0x0013u);
          w16(0x0E, (17u << 8) | 0x3Au);
          { uint32_t stx = 0, spx = 0;
            while (spx < 6000000u) { stx = r32(0x30); if ((stx & 0x2u) || (stx & 0xFFFF0000u)) break; spx++; }
            elog("e5-emmc: k218 LK-buf st %08x lk0 %08x lk1 %08x ps %08x sp %u\n",
                 stx, ld[0], ld[1], r32(0x24), spx);
            if (ld[0] != 0x39303030u && ld[0] != 0) elog("e5-emmc: k218 *** LK DATA *** %08x\n", ld[0]); }
          w32(0x30, 0xFFFFFFFFu);
          { uint32_t rbx[4]; cmd(12, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx);
            cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx); }
          lmap->release(); lmd->release();
        } else { elog("e5-emmc: k218 lkbuf map fail\n"); if (lmd) lmd->release(); }
      }
      // B2: timeout discrimination - 0x2B=0x0 (2^13 min) should collapse the 0.75s TC to ~0
      K218_ARM("T-min", (uint32_t)(uintptr_t)dva, 0x00u);
      // B3b: k219 - arm read, then mid-transfer cmd13 to interrogate card state.
      // R1 CURRENT_STATE: 4=tran (card never entered data), 6=sending data (card started)
      {
        for (uint32_t q = 0; q < 128; q++) d[q] = 0xCAFE0000u;
        for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
          __asm__ volatile ("dc cvac, %0" :: "r"(la));
        { uint8_t rrx = r8(0x2F); w8(0x2F, (uint8_t)((rrx & 0x08u) | 0x06u));
          if (!waitBits8(0x2F, 0x06, 0, 100000, 10)) elog("e5-emmc: k219 rst wedge\n"); }
        w32(0x30, 0xFFFFFFFFu); w16(0x06, 0); w16(0x04, 0x0200u);
        w8(0x2B, 0x0Eu);
        w32(0x00, 1); w32(0x58, (uint32_t)(uintptr_t)dva); w32(0x5C, 0);
        w8(0x28, 0x08u); w32(0x08, 0x00010000u); w16(0x0C, 0x0013u);
        w16(0x0E, (17u << 8) | 0x3Au);
        // wait for CC only
        { uint32_t stx = 0, spx = 0;
          while (spx < 200000u) { stx = r32(0x30); if (stx & 0x1u) break; spx++; }
          elog("e5-emmc: k219 cc st %08x ps %08x\n", stx, r32(0x24)); }
        // NOW: while data hangs, poll cmd13 8 times (CLK still available for commands?)
        // k221: 8-BIT BUS + LK clock - LK's actual read configuration.
        // All previous reads were 1-bit; LK reads 8-bit (EXT_CSD[183]=1).
        {
          uint32_t rb8[4];
          cmd(6, 0x0300B701u, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb8);   // BUS_WIDTH=8bit SDR
          elog("e5-emmc: k221 bw8 r1 %08x\n", rb8[0]);
          cmd(6, 0x0300B901u, CMD_RESP_SHORT | CMD_CRC_ENABLE, rb8);   // HS_TIMING=1
          elog("e5-emmc: k221 hs1 r1 %08x\n", rb8[0]);
          // LK's exact clock value
          w32(0x2C, 0x080E0200u);
          w32(0x2C, 0x080E0202u);
          { uint32_t g = 0; while (g < 20000u) { if (r16(0x2C) & 0x2u) break; g++; } }
          w32(0x2C, 0x080E0207u);
          { for (uint32_t q = 0; q < 2000; q++) (void)r32(0x24); }
          static const struct { uint8_t hc; const char *tag; } fv[3] = {
            { 0x28u, "B8-sdma" },  // 8-bit + SDMA
            { 0x30u, "B8-adma" },  // 8-bit + ADMA2
            { 0x08u, "B1-sdma" },  // 1-bit control (bus cmd may have failed)
          };
          for (int v = 0; v < 3; v++) {
            for (uint32_t q = 0; q < 128; q++) d[q] = 0xCAFE0000u;
            for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
              __asm__ volatile ("dc cvac, %0" :: "r"(la));
            { uint8_t rrx = r8(0x2F); w8(0x2F, (uint8_t)((rrx & 0x08u) | 0x06u));
              if (!waitBits8(0x2F, 0x06, 0, 100000, 10)) elog("e5-emmc: k220 rst wedge\n"); }
            w32(0x30, 0xFFFFFFFFu); w16(0x06, 0); w16(0x04, 0x0200u);
            w8(0x2B, 0x0Eu);
            w32(0x00, 1); w32(0x58, (uint32_t)(uintptr_t)dva); w32(0x5C, 0);
            w8(0x28, fv[v].hc); w32(0x08, 0x00010000u); w16(0x0C, 0x0013u);
            w16(0x0E, (17u << 8) | 0x3Au);
            uint32_t stx = 0, spx = 0;
            while (spx < 6000000u) { stx = r32(0x30); if ((stx & 0x2u) || (stx & 0xFFFF0000u)) break; spx++; }
            __asm__ volatile ("dsb sy" ::: "memory");
            __asm__ volatile ("dc ivac, %0" :: "r"((void*)d) : "memory");
            __asm__ volatile ("dsb sy" ::: "memory");
            // card state after: TRAN(4)=never started, SENDING(6)=data phase!
            uint32_t rs[4];
            cmd(13, ((uint32_t)this->rca) << 16, CMD_RESP_SHORT | CMD_CRC_ENABLE, rs);
            elog("e5-emmc: k220 %s st %08x r1 %08x d0 %08x d1 %08x ps %08x sp %u\n",
                 fv[v].tag, stx, rs[0], d[0], d[1], r32(0x24), spx);
            if (d[0] != 0xCAFE0000u && d[0] != 0) { elog("e5-emmc: k220 *** DATA *** %08x %08x %08x\n", d[2], d[3], d[4]); break; }
            w32(0x30, 0xFFFFFFFFu);
            { uint32_t rbx[4]; cmd(12, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx);
              cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx); }
          }
        }
        elog("e5-emmc: k220 done\n");
        return false;
        w32(0x30, 0xFFFFFFFFu);
        { uint32_t rbx[4]; cmd(12, 0, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx);
          cmd(16, 512, CMD_RESP_SHORT | CMD_CRC_ENABLE, rbx); }
      }
    }
    bmd->complete();
    bmd->release();
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
  // k192: AP pin controller state at XNU driver start (compare vs E5PIN UEFI dump)
  {
    IOMemoryDescriptor *pmd = IOMemoryDescriptor::withAddressRange(0x642E0000ull, 0x200, kIODirectionNone, nullptr);
    if (pmd) {
      IOMemoryMap *pmap = pmd->map();
      if (pmap) {
        volatile uint32_t *pin = (volatile uint32_t *)pmap->getVirtualAddress();
        for (uint32_t i = 0; i < 128; i += 8)
          elog("e5-emmc: PIN%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",
               i * 4, pin[i],pin[i+1],pin[i+2],pin[i+3],pin[i+4],pin[i+5],pin[i+6],pin[i+7]);
        pmap->release();
      }
      pmd->release();
    }
  }
  // LK leftovers in the response file, before any of our resets:
  elog("NativeEmmc: resp0 pre %08x %08x %08x %08x\n",
        r32(SDHCI_RESPONSE), r32(SDHCI_RESPONSE+4), r32(SDHCI_RESPONSE+8), r32(SDHCI_RESPONSE+12));
// k173/k177: LK live-state inheritance + on-device DLL_DLY sweep
      if ((r32(0x10) & 0x1F00u) == 0x0900u && (r8(0x28) & 0x20u) && (r32(0x0C) & 0xFF000000u) == 0x0D000000u) {
        elog("e5-emmc: k173 LK-live inherit (rsp %08x cmd %08x)\n", r32(0x10), r32(0x0C));
        IOBufferMemoryDescriptor *bmd2 = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 8192, 0x1000);
        IOPhysicalAddress dva2 = bmd2->getPhysicalAddress();
        uint32_t *d = (uint32_t *)((uint8_t *)bmd2->getBytesNoCopy() + 0x100u);
        for (uint32_t i = 0; i < 128; i = i + 1) d[i] = 0xCAFE0000u;
        for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
          __asm__ volatile ("dc cvac, %0" :: "r"(la));
        __asm__ volatile ("dsb sy" ::: "memory");
        uint32_t bdva = dva2 + 0x100u;
        // lkbase retired with k177 sweep
        // k178: exit HS400ES — card side to legacy SDR + 1-bit, then re-read
        {
          // k189: post-enum 400k clean ladder + write/multi/raw-DAT probes.
        // Note: LK live HC=0x20 (1-bit SDMA, no HS400) - HS400ES exit theories dead.
        // Card refuses all HS_TIMING from inherit state; try from clean 400k enum.
        {
          uint32_t rl;
          const uint32_t lad[4] = { 0x03B70100u, 0x03B90200u, 0x03B90100u, 0x03B90000u };
          for (uint32_t li = 0; li < 4; li++) {
            w32(0x30, 0xFFFFFFFFu);
            w32(0x08, lad[li]); w16(0x0E, (6u << 8) | 0x1Bu);
            for (uint32_t i = 0; i < 300000u; i++) { if (r32(0x30) & 0x8001u) break; }
            for (uint32_t i = 0; i < 300000u && (r32(0x24) & 0x3u); i++) { }
            rl = r32(0x10);
            elog("e5-emmc: k189 lad%u r1 %08x %s\n", li, rl, (rl & 0x80u) ? "ERR" : "ok");
          }
          // k191: single U-Boot-verbatim SDMA read at LK state (card now legacy via ladder).
          w32(0x30, 0xFFFFFFFFu);
          for (uint32_t i = 0; i < 128; i = i + 1) d[i] = 0xCAFE0000u;
          for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
            __asm__ volatile ("dc cvac, %0" :: "r"(la));
          w32(0x58, bdva); w32(0x5C, 0);
          w16(0x04, 0x7200u);   // blksz 512 | SDMA boundary
          w32(0x00, 1);
          w32(0x08, 0x00010000u);
          w32(0x0C, 0x113A0003u);   // (cmd17|0x3A)<<16 | TM 0x3 (U-Boot verbatim)
          uint32_t st191 = 0;
          for (uint32_t n = 0; n < 60000 * 60; n = n + 1) {
            st191 = r32(0x30);
            if ((st191 & 0x02u) || (st191 & 0xFFFF0000u)) break;
          }
          for (uint64_t la = (uint64_t)d & ~63ull; la < (uint64_t)d + 512; la += 64)
            __asm__ volatile ("dc ivac, %0" :: "r"(la));
          __asm__ volatile ("dsb sy" ::: "memory");
          elog("e5-emmc: k191 rd st %08x d0 %08x d1 %08x\n", st191, d[0], d[1]);
          w32(0x30, 0xFFFFFFFFu);
          OSSafeReleaseNULL(bmd2);
          // k191: fall through to clean hostInit + enum + k190 discriminator + readExtCsd.
        }
        }
      }

  // k194: PMIC VDDEMMCCORE power cycle. HS400ES has no CMD6 exit (JEDEC); VCC
  // power cycle cold-boots the card into legacy mode. UMP9620 PMIC via ADI r5p0
  // at 0x64400000: write = MMIO into slave window (+0x20000), read = RD_CMD/RD_DATA.
  {
    IOMemoryDescriptor *amd = IOMemoryDescriptor::withAddressRange(
        0x64400000ull, 0x40000ull, kIODirectionNone, NULL);
    if (amd) {
      IOMemoryMap *amap = amd->map();
      if (amap) {
        volatile uint32_t *adi = (volatile uint32_t *)(amap->getVirtualAddress());
        auto aRd = [&adi](uint32_t r) -> uint32_t {
          adi[0x28 / 4] = r;
          for (uint32_t i = 0; i < 100000u; i++) {
            uint32_t v = adi[0x2C / 4];
            if (!(v & 0x80000000u)) return v & 0xFFFFu;
          }
          return 0xDEADu;
        };
        auto aWr = [&adi](uint32_t r, uint32_t v) {
          for (uint32_t i = 0; i < 10000u; i++) {
            if (!(adi[0x30 / 4] & 0x800u)) break;
          }
          *(volatile uint32_t *)((uintptr_t)adi + 0x20000ull + r) = v;
        };
        uint32_t pd0 = aRd(0x21A0u);
        uint32_t vl0 = aRd(0x21A4u);
        elog("e5-emmc: k194 adi pd %04x vol %04x\n", pd0, vl0);
        aWr(0x21A0u, (pd0 & 0xFFFEu) | 1u);      // power down
        for (uint32_t i = 0; i < 5000000u; i++) { (void)adi[0x34 / 4]; }  // ~1s full discharge
        uint32_t pd1 = aRd(0x21A0u);
        elog("e5-emmc: k194 pd-down %04x\n", pd1);
        aWr(0x21A0u, pd1 & 0xFFFEu);             // power up
        // k195 cold boot: 50ms settle, then hostInit provides clocks; give the
        // card its boot clocks (74 @400kHz < 1ms) before any cmd0.
        for (uint32_t i = 0; i < 500000u; i++) { (void)adi[0x34 / 4]; }   // ~100ms
        elog("e5-emmc: k194 pd-up %04x vol %04x\n", aRd(0x21A0u), aRd(0x21A4u));
        // pre-cmd0 boot clocks: gate sd clock off/on then idle-spin 5ms
        w32(0x2C, 0x080E0000u);                  // int clk en only
        for (uint32_t i = 0; i < 20000u; i++) { (void)r32(0x24); }
        w32(0x2C, 0x080E0207u);                  // 400k sd clk on
        for (uint32_t i = 0; i < 200000u; i++) { (void)r32(0x24); }  // ~5ms = 2000 clocks
        elog("e5-emmc: k195 boot clocks done\n");
        amap->release();
      } else {
        elog("e5-emmc: k194 adi map fail\n");
      }
      amd->release();
    } else {
      elog("e5-emmc: k194 adi desc fail\n");
    }
  }

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
