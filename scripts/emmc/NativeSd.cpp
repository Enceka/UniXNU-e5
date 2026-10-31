// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed-card, read-only E5 SD bring-up. SDIO0 is separate from eMMC/WCN.
// Resources and clock encodings are from the live DT and vendor clock driver.
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "TransferStatus.h"
#include "SdProtocol.h"
#include "SystemOSWindow.h"

extern "C" void e5_panic_print(const char *);
static void sdlog(const char *fmt, ...) {
  char text[192]; va_list ap; va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap); va_end(ap); e5_panic_print(text);
}

class NativeSd;
static bool publishSystemOS(NativeSd *host);

class NativeSd : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeSd)
  struct Region {
    IOMemoryDescriptor *md = nullptr;
    IOMemoryMap *map = nullptr;
    volatile uint8_t *bytes = nullptr;
    bool open(uint64_t base, uint64_t size) {
      md = IOMemoryDescriptor::withAddressRange(base, size, kIODirectionNone, nullptr);
      if (!md || !(map = md->map())) return false;
      bytes = (volatile uint8_t *)map->getVirtualAddress(); return true;
    }
    void close() {
      if (map) { map->release(); map = nullptr; }
      if (md) { md->release(); md = nullptr; }
    }
    uint32_t get(uint32_t o) const { return *(volatile uint32_t *)(bytes + o); }
    void put(uint32_t o, uint32_t v) { *(volatile uint32_t *)(bytes + o) = v; }
  } host, ahb, aon, clocks, adi, adiSlave;
  IOBufferMemoryDescriptor *dma = nullptr;
  IOLock *lock = nullptr;
  uint8_t *buffer = nullptr;
  uint64_t physical = 0, sectors = 0;
  uint16_t rca = 0;
  bool prepared = false, cardUp = false, dmaQuarantined = false;
  // v15: softVerify makes read failures recoverable (chain experiments fall
  // back instead of tearing the driver down); multiOk gates the CMD18 path.
  bool softVerify = false, multiOk = false;
  const char *mode = "none";
  uint32_t calls = 0;
  uint32_t bigReads = 0;

  uint8_t r8(uint32_t o) const { return host.bytes[o]; }
  uint16_t r16(uint32_t o) const { return *(volatile uint16_t *)(host.bytes + o); }
  uint32_t r32(uint32_t o) const { return host.get(o); }
  void w8(uint32_t o, uint8_t v) { host.bytes[o] = v; }
  void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(host.bytes + o) = v; }
  void w32(uint32_t o, uint32_t v) { host.put(o, v); }

  // Unisoc AON APB protection (vendor drivers/soc/sprd/debug/apb_prot):
  // protected registers need the direct-access lock at 0xff0 before writing.
  void protPut(Region &reg, uint32_t off, uint32_t v) {
    uint32_t cmd = reg.get(0xff0);
    if (cmd & 0x8000u) reg.put(0xff0, 0x68960001u);   // lock direct access
    reg.put(off, v);
    if (cmd & 0x8000u) reg.put(0xff0, 0x97690001u);   // unlock
  }

  // UMS9620 ADI (qogirn6pro, r5px) master at PA 0x64400000 (aon simple-bus
  // child spi@400000 = 0x64000000 window + offset, per /soc/aon/ranges and
  // Android's 64400000.spi platform device; DT reg <0x400000 0x100000> =
  // PA 0x64400000..0x64500000). PMIC register X is written through the
  // 15-bit slave window at 0x64420000+X (controller + channel_offset
  // 0x20000, per vendor spi-sprd-adi.c) and read back through the
  // RD_CMD/RD_DATA pair (RD_CMD takes the physical slave address
  // 0x64420000+X; the controller masks it to 17 bits).
  bool adiDrain() {
    for (unsigned i = 0; i < 20000; i++) {
      if (adi.get(0x30) & (1u << 10)) return true; // ARM fifo empty.
      IODelay(10);
    }
    return false;
  }
  bool adiWriteWait() {
    for (unsigned i = 0; i < 20000; i++) {
      if (!(adi.get(0x30) & (1u << 31))) return true; // write retired.
      IODelay(10);
    }
    return false;
  }
  uint32_t pmicRead(uint32_t off) {
    adi.put(0x28, 0x64420000u + off);
    for (unsigned i = 0; i < 20000; i++) {
      uint32_t v = adi.get(0x2c);
      if (!(v & 0x80000000u)) return v & 0xffffu; // busy cleared.
      IODelay(10);
    }
    return 0xffffu;
  }
  bool pmicWrite(uint32_t off, uint32_t v) {
    if (!adiDrain() || (adi.get(0x30) & (1u << 11))) return false; // fifo full.
    adiSlave.put(off, v);
    return adiWriteWait();
  }

  static void cache(void *p, bool clean) {
    uint64_t start = (uint64_t)p;
    for (uint64_t at = start; at < start + 512; at += 64) {
      if (clean) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
      else __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
  }
  static void cacheN(void *p, uint32_t bytes, bool clean) {
    uint64_t start = (uint64_t)p;
    for (uint64_t at = start; at < start + bytes; at += 64) {
      if (clean) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
      else __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
  }
  static uint32_t crc32(const uint8_t *p) {
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < 512; i++) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
  }
  static uint32_t crc32n(const uint8_t *p, uint32_t bytes) {
    // Same polynomial over an arbitrary length; used for the ext4 directory
    // block whose golden reference (docs/logs/sd-ext4/rootdir.bin) is the
    // whole 4096-byte block, not its first sector.
    return ~E5Sd::crc32Update(0xffffffffu, p, bytes);
  }
  bool idle(uint32_t mask) {
    for (unsigned i = 0; i < 10000; i++) {
      if ((r32(0x24) & mask) == 0) return true;
      IODelay(10);
    }
    sdlog("NativeSd: inhibit timeout ps %08x mask %x\n", r32(0x24), mask); return false;
  }
  uint32_t wait(uint32_t done, unsigned timeoutUs) {
    uint32_t st = 0;
    for (unsigned i = 0; i < timeoutUs / 10; i++) {
      st = r32(0x30);
      if ((st & 0xffff8000u) || (st & done)) return st;
      IODelay(10);
    }
    return st;
  }
  uint32_t command(uint16_t word, uint32_t arg) {
    if (!idle(1)) return 0;
    w32(0x30, 0xffffffffu); w32(0x08, arg); w32(0x0c, (uint32_t)word << 16);
    return wait(1, 500000);
  }
  static bool commandOk(uint32_t st) { return (st & 0xffff8001u) == 1; }
  bool resetLines() {
    w8(0x2f, 0x0e); // Keep the Unisoc hardware-normal bit 3 set.
    for (unsigned i = 0; i < 10000; i++) {
      if ((r8(0x2f) & 6) == 0) return true;
      IODelay(10);
    }
    sdlog("NativeSd: reset timeout %02x\n", r8(0x2f)); return false;
  }
  uint32_t sourceHz = 26000000;
  bool setClock(uint32_t target) {
    uint16_t div = E5Sd::clockDivider(sourceHz, target);
    if (div == 0xffff) return false;
    w16(0x2c, 0); w16(0x2c, div | 1);
    for (unsigned i = 0; i < 10000; i++) {
      if (r16(0x2c) & 2) { w16(0x2c, div | 5); IODelay(2000); return true; }
      IODelay(10);
    }
    sdlog("NativeSd: clock unstable %04x\n", r16(0x2c)); return false;
  }
  void response136(uint32_t result[4]) {
    uint32_t raw[4]; for (unsigned i = 0; i < 4; i++) raw[i] = r32(0x10 + 4 * i);
    E5Sd::response136(raw, result);
  }
  bool getCsd() {
    uint32_t st = command(0x0909, (uint32_t)rca << 16), csd[4];
    if (!commandOk(st)) { sdlog("NativeSd: CMD9 failed st %08x\n", st); return false; }
    response136(csd); sectors = E5Sd::sdhcSectors(csd);
    sdlog("NativeSd: CSD %08x %08x %08x %08x sectors %llu\n", csd[0], csd[1], csd[2], csd[3], (unsigned long long)sectors);
    return sectors == 30244864ull;
  }
  bool select() {
    uint32_t st = command(0x071b, (uint32_t)rca << 16);
    if (!commandOk(st) || !idle(3)) return false;
    st = command(0x0d1a, (uint32_t)rca << 16);
    sdlog("NativeSd: selected st %08x r1 %08x rca %04x\n", st, r32(0x10), rca);
    return commandOk(st) && E5Sd::readyTran(r32(0x10));
  }
  bool readSector(uint32_t lba) {
    if (!idle(3)) return false;
    for (unsigned i = 0; i < 128; i++) ((uint32_t *)buffer)[i] = 0xcafebabe;
    cache(buffer, true); cache(buffer, false);
    w8(0x28, r8(0x28) & (uint8_t)~0x18); // SDMA, retaining bus width.
    w32(0x30, 0xffffffffu); w16(0x04, 512); w32(0x00, 1);
    w32(0x58, (uint32_t)physical); w32(0x5c, 0);
    w32(0x08, lba); w32(0x0c, 0x113a0013u);
    uint32_t st = wait(2, 2000000); cache(buffer, false);
    if (!E5Emmc::transferOk(st) || (r32(0x10) & E5Sd::r1Errors)) {
      sdlog("NativeSd: read fail lba %u st %08x r1 %08x ps %08x\n", lba, st, r32(0x10), r32(0x24));
      if (!softVerify) {
        cardUp = false;
        if (!resetLines()) dmaQuarantined = true;
      } else {
        resetLines();
      }
      return false;
    }
    return true;
  }
  // v15: multi-block CMD18 read of n sectors (n <= 16, the 8192B DMA page).
  // Unproven on this host (the eMMC path is single-block only), so callers
  // treat failure as evidence, not as a driver fault: softVerify guards the
  // error paths exactly like readSector.
  bool readSectors(uint32_t lba, uint32_t n) {
    if (n == 0 || n > 16) return false;
    if (!idle(3)) return false;
    for (uint32_t i = 0; i < 32u * n; i++) ((uint32_t *)buffer)[i] = 0xcafebabe;
    cacheN(buffer, n * 512, true); cacheN(buffer, n * 512, false);
    w8(0x28, r8(0x28) & (uint8_t)~0x18); // SDMA, retaining bus width.
    w32(0x30, 0xffffffffu); w16(0x04, 512); w32(0x00, n);
    w32(0x58, (uint32_t)physical); w32(0x5c, 0);
    w32(0x08, lba); w32(0x0c, 0x123a0033u); // CMD18, multi|count|DMA|read.
    uint32_t st = wait(2, 2000000u + 100000u * n); cacheN(buffer, n * 512, false);
    if (!E5Emmc::transferOk(st) || (r32(0x10) & E5Sd::r1Errors)) {
      sdlog("NativeSd: mread fail lba %u n %u st %08x r1 %08x ps %08x\n",
            lba, n, st, r32(0x10), r32(0x24));
      if (!softVerify) {
        cardUp = false;
        if (!resetLines()) dmaQuarantined = true;
      } else {
        resetLines();
      }
      return false;
    }
    uint32_t s12 = command(0x0c1a, 0); // CMD12 stop, R1.
    if (!commandOk(s12) || !idle(3)) {
      sdlog("NativeSd: mread stop fail lba %u st %08x s12 %08x\n", lba, st, s12);
      if (!softVerify) {
        cardUp = false;
        if (!resetLines()) dmaQuarantined = true;
      } else {
        resetLines();
      }
      return false;
    }
    return true;
  }
  bool verify() {
    // v17: the card now carries the original-system OS disk image raw at
    // LBA 0 (whole-card install, see docs/system-volume-audit.md). The
    // four reference sectors come from uefi/build/system-audit/userdata/
    // os-window.img (sha e5634d04..., byte-identical to what was dd'd):
    // LBA 0 is the protective MBR, LBA 1 the
    // "EFI PART" header, 2048/2050 land inside the Apple_APFS container.
    const uint32_t lbas[] = {0, 1, 2048, 2050};
    const uint32_t crcs[] = {0xec8ce612, 0x10fb5eed, 0xd2e6e0b2, 0xfba8aad8};
    for (unsigned i = 0; i < 4; i++) {
      if (!readSector(lbas[i])) return false;
      uint32_t crc = crc32(buffer);
      sdlog("NativeSd: reference lba %u st %08x crc %08x\n", lbas[i], r32(0x30), crc);
      if (crc != crcs[i]) return false;
      if (i == 1 && memcmp(buffer, "EFI PART", 8) != 0) return false;
    }
    for (unsigned i = 0; i < 16; i++) {
      unsigned index = (i & 1) ? 3 : 0;
      if (!readSector(lbas[index]) || crc32(buffer) != crcs[index]) return false;
    }
    sdlog("NativeSd: reference 4/4 repeat 16/16 mode %s\n", mode); return true;
  }
  bool initialize() {
    sdlog("NativeSd: inherit hc1 %02x hc2 %04x clk %04x power %02x reset %02x ps %08x\n",
          r8(0x28), r16(0x3e), r16(0x2c), r8(0x29), r8(0x2f), r32(0x24));
    sdlog("NativeSd: source %08x div %08x one %08x hotplug %08x rmldo %08x\n",
          clocks.get(0x280), clocks.get(0x284), clocks.get(0x288), aon.get(0x520), aon.get(0xba0));
    w32(0x34, 0x1b7f410b); w32(0x38, 0);
    w16(0x3e, r16(0x3e) | 0x3000);
    if ((r16(0x2c) & 5) == 5) {
      rca = 0x5048; uint32_t st = command(0x0d1a, (uint32_t)rca << 16);
      sdlog("NativeSd: warm CMD13 st %08x r1 %08x\n", st, r32(0x10));
      if (commandOk(st) && E5Sd::readyTran(r32(0x10)) &&
          commandOk(command(0x0700, 0)) && getCsd() && select()) {
        mode = "inherited";
        if (verify()) return true;
      }
    }
    if (dmaQuarantined) return false;
    sdlog("NativeSd: cold legacy begin (PMIC LDO enable)\n");
    { // The aon gate adi_eb (status 0x64900008 bit9, vendor ums9620-clk.c)
      // is turned off by the sprd watchdog's shutdown path on the Android
      // reboot into UEFI, and nothing re-enables it (the ADI DT node has no
      // clocks). Round 3 proved this: the first ADI load external-aborted.
      // SC gate: set-only write at base+0x1000, status at the base register.
      uint32_t g0 = aon.get(0x8);
      aon.put(0x1008, 1u << 9);
      IODelay(1000);
      uint32_t g1 = aon.get(0x8);
      sdlog("NativeSd: adi gate %08x->%08x\n", g0, g1);
      if (!(g1 & 0x200u)) { sdlog("NativeSd: adi gate enable refused\n"); return false; }
    }
    { // The ADI may be held in soft reset (RESET_AON_APB_ADI_SOFT_RST,
      // vendor ums9620-clk.c aon resets: reg 0x14 bit9, SC assert write
      // at +0x1000, deassert at +0x2000 per drivers/clk/sprd/reset.c).
      // A held reset decodes nothing and external-aborts reads, exactly
      // the round 3/4 signature.
      uint32_t r0 = aon.get(0x14);
      aon.put(0x1014, 1u << 9);            // assert
      IODelay(50);
      aon.put(0x2014, 1u << 9);            // deassert (release)
      IODelay(1000);
      uint32_t r1 = aon.get(0x14);
      sdlog("NativeSd: adi rst %08x->%08x\n", r0, r1);
      if (r1 & 0x200u) { sdlog("NativeSd: adi rst stuck\n"); return false; }
    }
    { // ADI functional clock: mux "adi" (aonapb-clk 0x64920034 bits[2:0];
      // parents 0 rco-100m-4m, 1 ext-26m, 2 rco-100m-25m, 3/4 PLLs).
      // Force the always-on RCO 25 MHz source Android itself selects.
      uint32_t m0 = clocks.get(0x34);
      clocks.put(0x34, (m0 & ~7u) | 2u);
      IODelay(1000);
      uint32_t m1 = clocks.get(0x34);
      sdlog("NativeSd: adi mux %08x->%08x\n", m0, m1);
      if ((m1 & 7u) != 2u) { sdlog("NativeSd: adi mux stuck\n"); return false; }
    }
    { // UMP9620 power controller over ADI (vendor ump9620-regulator.c):
      // write-unlock 0x23d0 = 0x6e7f once, then enable = clear the PD bit0,
      // VOL is linear 10mV from 1.2V -> 3.0V = 0xb4. vmmc first, then vqmmc.
      uint32_t prot = pmicRead(0x23d0);
      uint32_t scPd = pmicRead(0x21ac), scV = pmicRead(0x21b0);
      uint32_t ioPd = pmicRead(0x21b8), ioV = pmicRead(0x21bc);
      sdlog("NativeSd: pmic prot %04x sdcore pd %04x vol %04x sdio pd %04x vol %04x\n",
            prot, scPd, scV, ioPd, ioV);
      pmicWrite(0x23d0, 0x6e7f);                  // regulator write unlock.
      pmicWrite(0x21b0, E5Sd::ldoVolUv(3000000)); // vddsdcore (vmmc) 3.0V...
      pmicWrite(0x21ac, scPd & ~1u);              // ...PD clear = enabled.
      IODelay(1000);
      pmicWrite(0x21bc, E5Sd::ldoVolUv(3000000)); // vddsdio (vqmmc) 3.0V...
      pmicWrite(0x21b8, ioPd & ~1u);              // ...PD clear = enabled.
      IODelay(20000);
      { // Wait for the card pull-ups to raise CMD/DAT after the LDO ramp;
        // PSTATE 0x24 bits 20-24 mirror the five line levels.
        uint32_t ps = r32(0x24); unsigned lt;
        for (lt = 0; lt < 1000 && !(ps & 0x01f00000u); lt++) { IODelay(1000); ps = r32(0x24); }
        sdlog("NativeSd: lines ready pstate %08x tries %u\n", ps, lt);
      }
      sdlog("NativeSd: pmic after sdcore pd %04x vol %04x sdio pd %04x vol %04x pstate %08x\n",
            pmicRead(0x21ac), pmicRead(0x21b0), pmicRead(0x21b8), pmicRead(0x21bc), r32(0x24));
    }
    { // Vendor fast-hotplug sequence (realme/oppo sdhci-sprd.c set_power):
      // rmldo-en aon 0xba0 bit0 through the APB-prot window; 0x520 gets
      // protect-en bit20, debounce-en bit0, debounce count bits1-8 = 0 and
      // detect-polar bit21 CLEARED (cd-gpios flags 0x23 -> polar clear).
      uint32_t rm0 = aon.get(0xba0), hp0 = aon.get(0x520);
      protPut(aon, 0xba0, rm0 | 1u);
      protPut(aon, 0x520, E5Sd::hotplugValue(hp0));
      IODelay(20000);
      sdlog("NativeSd: hotplug %08x->%08x rmldo %08x->%08x prot %08x pstate %08x\n",
            hp0, aon.get(0x520), rm0, aon.get(0xba0), aon.get(0xff0), r32(0x24));
    }
    { // sdhci core init always does SDHCI_RESET_ALL (sdhci_init) and the
      // vendor probe sets DLL backup mode (sdhci_sprd_init_config:
      // 0x28c |= DLL_BAK|DLL_VAL, bits 0,1). We skipped both; the controller
      // carried SDR104-era sampler state across the warm reboot (round 9:
      // Linux re-inits the card warm with zero errors, we CRC every response).
      uint8_t hr0 = r8(0x2f);
      w8(0x2f, 0x09); // RESET_ALL (bit0, self-clears) | Unisoc card-hw-reset bit3.
      unsigned hr;
      for (hr = 0; hr < 1000 && (r8(0x2f) & 1); hr++) IODelay(10);
      IODelay(1000);
      w32(0x28c, r32(0x28c) | 0x3u); // Vendor DLL backup mode.
      sdlog("NativeSd: host reset %02x->%02x deb %08x\n", hr0, r8(0x2f), r32(0x28c));
      if (r8(0x2f) & 1) { sdlog("NativeSd: host reset stuck\n"); return false; }
    }
    { // v12: vendor sdhci_sprd_hw_reset -- the ONLY other 0x2f bit the vendor
      // code drives. Linux mmc core calls hw_reset during every mmc_power_up
      // (mmc_hw_reset_for_init); the vendor writeb workaround says clearing
      // bit3 by mistake breaks the card, and v10 proved the opposite extreme
      // (bit3 held set forever, never pulsed) still CRCs every response. Pulse
      // the card reset properly: low 10-20us, release, settle 300-500us.
      uint8_t cb = r8(0x2f);
      w8(0x2f, cb & ~(uint8_t)0x08); // assert card reset (active low)
      IODelay(20);
      w8(0x2f, cb | 0x08); // release
      IODelay(500);
      sdlog("NativeSd: card rst %02x->%02x pstate %08x\n", cb, r8(0x2f), r32(0x24));
    }
    w16(0x2c, 0);
    // v11: leave the Android-proven sdio0 chain untouched. Live Android
    // (mmc1 SDR104) shows mux 0x280=4 (rpll-390m), 2x div 0x284=0, 1x 0x288=1
    // (1x=2x/2 = 195 MHz) and clk_summary agrees; the Linux unbind/rebind
    // re-init kept this chain and got zero errors. Rounds 7-10 forced
    // ext-26m/13 MHz instead and every response CRC'd with a zero response
    // register, so borrow the residue rather than rewriting it, and derive the
    // host divider from whatever source the chain actually presents.
    const uint32_t sdMux = clocks.get(0x280) & 7u;
    const uint32_t sdD2x = clocks.get(0x284) & 0x7ffu;
    const uint32_t sdD1x = clocks.get(0x288) & 1u;
    sourceHz = E5Sd::sdio0SourceHz(sdMux, sdD2x, sdD1x);
    sdlog("NativeSd: sdio0 chain mux %u d2x %u d1x %u src %uHz\n", sdMux, sdD2x, sdD1x, sourceHz);
    w16(0x3e, 0x3000); w8(0x28, 0); w8(0x2e, 0x0e);
    if (!resetLines()) return false;
    w32(0x200, r32(0x200) & ~(1u << 21)); // No DLL below 52 MHz.
    w32(0x204, 0); w32(0x208, r32(0x208) | (1u << 13) | (1u << 21));
    w32(0x250, r32(0x250) & ~0x03000000u);
    w32(0x34, 0x1b7f410b); w32(0x38, 0);
    if (!setClock(400000)) return false;
    IODelay(10000);
    uint32_t st = command(0x0000, 0);
    IODelay(2000);
    st = command(0x0000, 0); // mmc_go_idle pulses CMD0 twice.
    sdlog("NativeSd: CMD0 st %08x clk %04x\n", st, r16(0x2c));
    if (!commandOk(st)) return false;
    // v13/v14: the aon detect polarity is the fix (round 13: with bit21 SET
    // the card echoes CMD8 instantly; rounds 1-12 CRC-failed every response
    // with it clear). cd-gpios = <eic-sync 35 0> -- flags 0, so the vendor
    // fast_hotplug_enable() takes the else branch and SETS the polarity bit;
    // E5Sd::hotplugValue() now encodes that. The cdpol matrix that proved
    // this (round 13, kernel 275) is removed; the hotplug block above
    // already applied the corrected value before CMD0.
    { // CMD8 with retries: a freshly powered card can CRC-flake the echo.
      unsigned ct = 0;
      for (;;) {
        st = command(0x081a, 0x1aa);
        if (commandOk(st) && (r32(0x10) & 0xfff) == 0x1aa) break;
        if (++ct >= 10) break;
        sdlog("NativeSd: CMD8 try %u st %08x r7 %08x err %08x\n", ct, st, r32(0x10), r16(0x32));
        IODelay(5000);
      }
      if (!commandOk(st) || (r32(0x10) & 0xfff) != 0x1aa) {
        // v9: deterministic CRC — discriminate response-path corruption from a
        // CMD8-specific issue. CMD55 carries an R1 (CRC-checked) response: if
        // it also CRCs, the whole response sampling path is off (phase/clock).
        uint32_t s55 = command(0x371a, 0);
        sdlog("NativeSd: CMD55 st %08x r1 %08x err %04x\n", s55, r32(0x10), r16(0x32));
        static const char *const names[3] = {"noinv", "clk200", "noinv-clk200"};
        const bool invOff[3] = {true, false, true};
        const uint32_t hz[3] = {400000, 200000, 200000};
        bool ok8 = false; int win = -1;
        for (int i = 0; i < 3 && !ok8; i++) {
          if (invOff[i]) w32(0x208, r32(0x208) & ~((1u << 13) | (1u << 21)));
          if (!setClock(hz[i])) continue;
          IODelay(1000);
          for (unsigned k = 0; k < 3; k++) {
            st = command(0x081a, 0x1aa);
            if (commandOk(st) && (r32(0x10) & 0xfff) == 0x1aa) { ok8 = true; win = i; break; }
            IODelay(2000);
          }
          sdlog("NativeSd: CMD8 %s st %08x r7 %08x\n", names[i], st, r32(0x10));
        }
        if (ok8) {
          sdlog("NativeSd: CMD8 win %s\n", names[win]);
        } else {
          uint32_t s55b = command(0x371a, 0);
          sdlog("NativeSd: CMD55b st %08x r1 %08x err %04x\n", s55b, r32(0x10), r16(0x32));
        }
        sdlog("NativeSd: CMD8 st %08x r7 %08x\n", st, r32(0x10));
        if (!commandOk(st) || (r32(0x10) & 0xfff) != 0x1aa) {
          sdlog("NativeSd: CMD8 fail pstate %08x err %04x dllsts %08x dly %08x\n",
                r32(0x24), r16(0x32), r32(0x210), r32(0x208));
          return false;
        }
      } else {
        // v14: first-try success must print the standard grammar line too
        // (the checker keys on it); it used to print only in the fail path.
        sdlog("NativeSd: CMD8 st %08x r7 %08x\n", st, r32(0x10));
      }
    }
    uint32_t ocr = 0; unsigned tries;
    for (tries = 0; tries < 1000; tries++) {
      st = command(0x371a, 0);
      if (!commandOk(st) || (r32(0x10) & E5Sd::r1Errors) || !(r32(0x10) & 0x20)) break;
      st = command(0x2902, 0x40ff8000); ocr = r32(0x10); // No S18R.
      if (!commandOk(st) || (ocr & 0x80000000)) break;
      IODelay(1000);
    }
    sdlog("NativeSd: ACMD41 st %08x ocr %08x tries %u\n", st, ocr, tries);
    if (!commandOk(st) || (ocr & 0xc0000000u) != 0xc0000000u) {
      sdlog("NativeSd: ACMD41 fail pstate %08x err %04x\n", r32(0x24), r16(0x32));
      return false;
    }
    st = command(0x0209, 0); if (!commandOk(st)) return false;
    st = command(0x031a, 0); rca = r32(0x10) >> 16;
    sdlog("NativeSd: CMD3 st %08x r6 %08x rca %04x\n", st, r32(0x10), rca);
    if (!commandOk(st) || !rca || (r32(0x10) & 0xe000) || !getCsd() || !select()) return false;
    if (!readSector(0) || crc32(buffer) != 0xec8ce612u) return false;
    st = command(0x371a, (uint32_t)rca << 16);
    if (!commandOk(st) || (r32(0x10) & E5Sd::r1Errors)) return false;
    st = command(0x061a, 2); // ACMD6: 4-bit, volatile bus setting only.
    if (!commandOk(st) || (r32(0x10) & E5Sd::r1Errors)) return false;
    w8(0x28, 2); if (!setClock(13000000)) return false;
    // Linux clears the <=400kHz CMD/POSRD delay-invert bits above 400kHz.
    w32(0x208, r32(0x208) & ~((1u << 13) | (1u << 21)));
    w32(0x250, r32(0x250) | 0x03000000u);
    mode = "legacy-4bit-13MHz";
    if (!verify()) return false;
    // v15: raise throughput. Switch the sdio0 functional chain to rpll
    // (Android's own runtime state: mux 4 = rpll_390m, 2x div 0 -> 390MHz,
    // 1x enable -> 195MHz) and run default speed (24.375MHz card clock,
    // divider N=2). Any recheck failure falls back to the proven ext-26m
    // chain; softVerify keeps the experiment non-fatal.
    softVerify = true;
    {
      w16(0x2c, 0); // SDCLK off while the chain moves.
      clocks.put(0x280, (clocks.get(0x280) & ~7u) | 4u);
      clocks.put(0x284, 0);
      clocks.put(0x288, 1);
      IODelay(1000);
      uint32_t rm = clocks.get(0x280) & 7, rd2 = clocks.get(0x284) & 0x7ff, rd1 = clocks.get(0x288) & 1;
      uint32_t src2 = E5Sd::sdio0SourceHz(rm, rd2, rd1);
      sdlog("NativeSd: chain rpll mux %u d2x %u d1x %u src %uHz\n", rm, rd2, rd1, src2);
      if (src2 == 195000000u) {
        sourceHz = src2; // setClock reads sourceHz for the divider.
        if (setClock(24375000u)) {
          mode = "default-4bit-24MHz";
          if (!verify()) {
            sdlog("NativeSd: default-speed recheck failed, falling back\n");
            mode = "legacy-4bit-13MHz";
            w16(0x2c, 0);
            clocks.put(0x280, (clocks.get(0x280) & ~7u) | 1u);
            clocks.put(0x284, 0);
            clocks.put(0x288, 1);
            IODelay(1000);
            sourceHz = E5Sd::sdio0SourceHz(1, 0, 1);
            if (!setClock(13000000) || !verify()) {
              softVerify = false;
              return false; // even the proven legacy state is gone; fail loud.
            }
          }
        }
      }
    }
    // ext4 read-side walk over the e5-openwrt image (offline-proven chain,
    // dumps/sd-ext4/): MBR partition @2048 -> sb @2050 -> GDT fs-blk 1 ->
    // gd0 itable 145 -> root inode 2 @+256 -> extent -> root dir block.
    // v17: the card may instead carry the original-system OS disk image
    // (whole-card GPT layout). The walk is conditional on the ext4 sb
    // magic; on an OS-image card one evidence line notes the layout and
    // the GPT header (already CRC-verified in verify()) is confirmed.
    {
      const uint32_t partLba = 2048;
      uint32_t sbLba = partLba + 2;
      if (readSectors(sbLba, 8)) {
        uint16_t magic = (uint16_t)(buffer[56] | (buffer[57] << 8));
        uint32_t logBs = (uint32_t)buffer[24] | ((uint32_t)buffer[25] << 8) |
                         ((uint32_t)buffer[26] << 16) | ((uint32_t)buffer[27] << 24);
        uint32_t inodes = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
                          ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
        uint32_t blocks = (uint32_t)buffer[4] | ((uint32_t)buffer[5] << 8) |
                          ((uint32_t)buffer[6] << 16) | ((uint32_t)buffer[7] << 24);
        uint32_t ipg = (uint32_t)buffer[40] | ((uint32_t)buffer[41] << 8) |
                       ((uint32_t)buffer[42] << 16) | ((uint32_t)buffer[43] << 24);
        if (magic == 0xef53) {
          if (logBs == 2 && ipg == 8192) {
            uint32_t bs = 4096;
            sdlog("NativeSd: ext4 sb ok inodes %u blocks %u ipg %u\n", inodes, blocks, ipg);
          // GDT at fs-blk 1 (first_data_block 0): gd0.inode_table @+8.
          if (readSectors(partLba + 8, 8)) {
            uint32_t itable = (uint32_t)buffer[8] | ((uint32_t)buffer[9] << 8) |
                              ((uint32_t)buffer[10] << 16) | ((uint32_t)buffer[11] << 24);
            sdlog("NativeSd: ext4 gd0 itable %u\n", itable);
            if (itable > 0) {
              // Root inode 2 at (2-1)*256 = byte 256 of the itable block.
              const uint32_t inodeSize = 256; // sb field corrupt; proven stride
              if (readSectors(partLba + itable * (bs / 512), 8)) {
                const uint8_t *root = buffer + inodeSize;
                E5Sd::Ext4Extent ext[4];
                unsigned ne = E5Sd::ext4ParseExtents(root, inodeSize, ext, 4);
                if (ne > 0) {
                  sdlog("NativeSd: ext4 root ext logical %u len %u start %u\n",
                        ext[0].logical, ext[0].len, ext[0].start);
                  if (readSectors(partLba + ext[0].start * (bs / 512), 8)) {
                    uint32_t dirCrc = crc32n(buffer, bs);
                    uint32_t dotInode = 0;
                    unsigned entries = E5Sd::ext4CountDirents(buffer, bs, &dotInode);
                    sdlog("NativeSd: ext4 rootdir %u entries dot %u crc %08x\n",
                          entries, dotInode, dirCrc);
                  } else {
                    sdlog("NativeSd: ext4 rootdir read fail\n");
                  }
                } else {
                  sdlog("NativeSd: ext4 root extents bad\n");
                }
              } else {
                sdlog("NativeSd: ext4 itable read fail\n");
              }
            }
          } else {
            sdlog("NativeSd: ext4 gd read fail\n");
          }
            } else {
              sdlog("NativeSd: ext4 sb mismatch magic %04x logbs %u\n", magic, logBs);
            }
        } else {
          // v17: OS-image card — whole-card GPT layout, no ext4 superblock.
          sdlog("NativeSd: ext4 absent (os image)\n");
          if (!readSector(1)) {
            sdlog("NativeSd: gpt read fail\n"); return false;
          } else if (memcmp(buffer, "EFI PART", 8) == 0) {
            uint32_t entries = (uint32_t)buffer[80] | ((uint32_t)buffer[81] << 8) |
                               ((uint32_t)buffer[82] << 16) | ((uint32_t)buffer[83] << 24);
            // v22: validate both GPT checksums, rather than calling the magic
            // alone "gpt ok". The v21 card patch left a valid header whose
            // declared table CRC did not match the actual 16 KiB entries.
            // The original image was restored from its verified backup.
            uint32_t entrySize = (uint32_t)buffer[84] | ((uint32_t)buffer[85] << 8) |
                                 ((uint32_t)buffer[86] << 16) | ((uint32_t)buffer[87] << 24);
            uint32_t headerSize = (uint32_t)buffer[12] | ((uint32_t)buffer[13] << 8) |
                                  ((uint32_t)buffer[14] << 16) | ((uint32_t)buffer[15] << 24);
            uint32_t headerCrc = (uint32_t)buffer[16] | ((uint32_t)buffer[17] << 8) |
                                 ((uint32_t)buffer[18] << 16) | ((uint32_t)buffer[19] << 24);
            uint32_t entriesCrc = (uint32_t)buffer[88] | ((uint32_t)buffer[89] << 8) |
                                  ((uint32_t)buffer[90] << 16) | ((uint32_t)buffer[91] << 24);
            uint64_t tableLba = 0;
            for (unsigned i = 0; i < 8; i++) tableLba |= (uint64_t)buffer[72 + i] << (8 * i);
            if (entries != 128 || entrySize != 128 || headerSize != 92 || tableLba != 2) {
              sdlog("NativeSd: gpt geometry bad\n"); return false;
            }
            memset(buffer + 16, 0, 4);
            uint32_t actualHeaderCrc = crc32n(buffer, headerSize);
            sdlog("NativeSd: gpt header crc %08x expected %08x match %c\n",
                  actualHeaderCrc, headerCrc, actualHeaderCrc == headerCrc ? 'y' : 'n');
            if (actualHeaderCrc != headerCrc) return false;
            uint64_t altLba = (uint64_t)buffer[32] | ((uint64_t)buffer[33] << 8) |
                ((uint64_t)buffer[34] << 16) | ((uint64_t)buffer[35] << 24) |
                ((uint64_t)buffer[36] << 32) | ((uint64_t)buffer[37] << 40);
            sdlog("NativeSd: gpt altLba %llu card %u\n",
                  (unsigned long long)altLba, (unsigned)sectors);
            // v18: replicate the kernel scheme's entries read (16-sector
            // chunks at LBA 2/18) and cross-check with 8-sector proven-path
            // reads. Golden from os-window.img: chunk CRCs 3a740ceb/d8f49994,
            // quarter CRCs 0c87a44e + c71c0011 x3, p0 type 7c3457ef first 34
            // count 11870208. Tells corrupt-16-chunk apart from a scheme
            // rejection that is not data.
            {
              bool e0 = readSectors(2, 16);
              uint32_t c0 = e0 ? crc32n(buffer, 8192) : 0;
              uint32_t t0 = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
                            ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
              uint32_t f0 = (uint32_t)buffer[32] | ((uint32_t)buffer[33] << 8) |
                            ((uint32_t)buffer[34] << 16) | ((uint32_t)buffer[35] << 24);
              uint32_t n0 = ((uint32_t)buffer[40] | ((uint32_t)buffer[41] << 8) |
                             ((uint32_t)buffer[42] << 16) | ((uint32_t)buffer[43] << 24)) - f0 + 1;
              bool e1 = readSectors(18, 16);
              uint32_t c1 = e1 ? crc32n(buffer, 8192) : 0;
              sdlog("NativeSd: gpt ent16 h0 %c crc %08x h1 %c crc %08x "
                    "p0 type %08x first %u count %u\n",
                    e0 ? 'y' : 'n', c0, e1 ? 'y' : 'n', c1, t0, f0, n0);
              uint32_t qs[4] = {0, 0, 0, 0};
              uint32_t tableState = 0xffffffffu;
              bool tableOk = true;
              char qf[5] = "nnnn";
              for (uint32_t q = 0; q < 4; q++) {
                if (readSectors(2 + q * 8, 8)) {
                  qs[q] = crc32n(buffer, 4096);
                  tableState = E5Sd::crc32Update(tableState, buffer, 4096);
                  qf[q] = 'y';
                } else {
                  tableOk = false;
                }
              }
              sdlog("NativeSd: gpt ent8 %c%c%c%c crc %08x %08x %08x %08x\n",
                    qf[0], qf[1], qf[2], qf[3], qs[0], qs[1], qs[2], qs[3]);
              uint32_t actualEntriesCrc = ~tableState;
              tableOk = tableOk && actualEntriesCrc == entriesCrc;
              sdlog("NativeSd: gpt entries crc %08x expected %08x match %c\n",
                    actualEntriesCrc, entriesCrc, tableOk ? 'y' : 'n');
              if (!tableOk) return false;
            }
            sdlog("NativeSd: gpt ok entries %u\n", entries);
          } else {
            sdlog("NativeSd: gpt absent\n"); return false;
          }
        }
      } else {
        sdlog("NativeSd: ext4 sb read fail\n");
      }
    }
    // Timed loops over 128 sequential sectors (LBA 16..143, inside the ext4
    // partition): single-block baseline, then the CMD18 experiment with
    // first-sector CRC comparison per 16-sector chunk.
    {
      const uint32_t base = 16, total = 128;
      uint64_t t0, t1, ns;
      clock_get_uptime(&t0);
      for (uint32_t i = 0; i < total; i++) {
        if (!readSector(base + i)) { softVerify = false; return false; }
      }
      clock_get_uptime(&t1);
      absolutetime_to_nanoseconds(t1 - t0, &ns);
      sdlog("NativeSd: tp single %u sectors in %llu us\n", total, (unsigned long long)(ns / 1000));
      uint32_t want[8]; bool ok = true;
      for (uint32_t c = 0; c < 8; c++) {
        if (!readSector(base + c * 16)) { ok = false; break; }
        want[c] = crc32(buffer);
      }
      if (ok) {
        clock_get_uptime(&t0);
        for (uint32_t c = 0; c < 8 && ok; c++) {
          if (!readSectors(base + c * 16, 16)) { ok = false; break; }
          if (crc32(buffer) != want[c]) {
            sdlog("NativeSd: mread mismatch lba %u\n", base + c * 16);
            ok = false;
          }
        }
        clock_get_uptime(&t1);
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        sdlog("NativeSd: tp multi %u sectors in %llu us match %c\n",
              total, (unsigned long long)(ns / 1000), ok ? 'y' : 'n');
        multiOk = ok;
      } else {
        sdlog("NativeSd: tp multi skipped\n");
      }
    }
    softVerify = false;
    return true;
  }

public:
  bool start(IOService *provider) override {
    if (!IOBlockStorageDevice::start(provider)) return false;
    sdlog("NativeSd: v1 start, independent SDIO0 host\n");
    if (!ahb.open(0x20000000, 0x2000) || !aon.open(0x64900000, 0x3000) ||
        !clocks.open(0x64920000, 0x1000) || !adi.open(0x64400000, 0x1000) ||
        !adiSlave.open(0x64420000, 0x8000)) { sdlog("NativeSd: clock map failed\n"); return false; }
    ahb.put(0x1000, 1u); aon.put(0x113c, (1u << 2) | (1u << 3));
    IODelay(1000);
    sdlog("NativeSd: gates ahb %08x aon %08x\n", ahb.get(0), aon.get(0x13c));
    if (!host.open(0x22210000, 0x1000)) return false;
    lock = IOLockAlloc();
    dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 8192, 0xffffe000ull);
    if (!lock || !dma || dma->prepare() != kIOReturnSuccess) return false;
    prepared = true; buffer = (uint8_t *)dma->getBytesNoCopy(); physical = dma->getPhysicalAddress();
    if (!buffer || physical == 0 || physical + 8192 > 0x100000000ull) return false;
    sdlog("NativeSd: dma %08llx\n", (unsigned long long)physical);
    cardUp = initialize();
    sdlog("NativeSd: probe %s sectors %llu mode %s\n", cardUp ? "Success" : "Failed",
          (unsigned long long)sectors, mode);
    if (!cardUp) return false;
    bool systemOS = PE_parse_boot_argn("e5-systemos", nullptr, 0);
    if (systemOS) {
      if (!E5SystemOS::fits(sectors) || !readSectors(E5SystemOS::base, 8) ||
          memcmp(buffer + 32, "NXSB", 4) ||
          crc32n(buffer, 4096) != E5SystemOS::superblockCrc) {
        sdlog("NativeSd: SystemOS superblock refused\n"); return false;
      }
      sdlog("NativeSd: SystemOS superblock crc %08x match y\n", E5SystemOS::superblockCrc);
    }
    sdlog("NativeSd: publishing read-only SD card %llu sectors\n",
          (unsigned long long)sectors);
    registerService();
    if (systemOS && !publishSystemOS(this)) sdlog("NativeSd: SystemOS publication failed\n");
    return true;
  }
  void stop(IOService *provider) override {
    if (lock) IOLockLock(lock);
    cardUp = false;
    if (lock) IOLockUnlock(lock);
    IOBlockStorageDevice::stop(provider);
  }
  void free() override {
    if (host.bytes) {
      w32(0x38, 0);
      dmaQuarantined = !resetLines();
      w16(0x2c, 0);
    }
    // Never recycle a DMA page if the data engine could still own it.
    if (dma && (!prepared || !dmaQuarantined)) {
      if (prepared) dma->complete();
      dma->release();
    } else if (dma) sdlog("NativeSd: quarantined DMA after failed reset\n");
    if (lock) IOLockFree(lock);
    host.close(); clocks.close(); aon.close(); ahb.close();
    adi.close(); adiSlave.close();
    IOBlockStorageDevice::free();
  }
  char *getVendorString() override { return const_cast<char *>("SD"); }
  char *getProductString() override { return const_cast<char *>("E5 SDHC (read-only bring-up)"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(mode); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn reportBlockSize(UInt64 *n) override { *n = 512; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *n) override { *n = sectors ? sectors - 1 : 0; return kIOReturnSuccess; }
  // Fixed boot card: iPadOS APFS filters ejectable/removable media before
  // reading the partition. This bring-up driver does not support hot removal.
  IOReturn reportEjectability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *p, bool *c) override { *p = cardUp; if (c) *c = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *b) override { *b = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *b) override { *b = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *data, UInt64 block, UInt64 count,
      IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!data || !completion || !completion->action || !count) return kIOReturnBadArgument;
    if (data->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    if (!lock || block >= sectors || count > sectors - block || data->getLength() < count * 512)
      return kIOReturnBadArgument;
    UInt64 done = 0; IOLockLock(lock);
    if (!cardUp) { IOLockUnlock(lock); return kIOReturnNotReady; }
    // Keep the GPT/first APFS requests, then sample progress. Logging every
    // multi-block read overwrites the persistent trace window during OS boot.
    uint32_t call = ++calls;
    if (call <= 12 || (call & (call - 1)) == 0)
      sdlog("NativeSd: rw %llu blocks at %llu\n", (unsigned long long)count, (unsigned long long)block);
    UInt64 wbShort = 0;
    while (done < count * 512) {
      uint32_t left = (uint32_t)((count * 512 - done) / 512);
      uint32_t n = 1;
      // v18: serve kernel reads in 8-sector chunks — the size proven golden
      // by the ext4 walk (16-sector chunks stay available for diagnostics).
      if (multiOk && left >= 4) n = left > 8 ? 8 : left;
      bool ok;
      if (n > 1) {
        ok = readSectors((uint32_t)(block + done / 512), n);
        if (ok && data->writeBytes(done, buffer, n * 512) != n * 512) { wbShort = done; ok = false; }
      } else {
        ok = readSector((uint32_t)(block + done / 512));
        if (ok && data->writeBytes(done, buffer, 512) != 512) { wbShort = done; ok = false; }
      }
      if (!ok) break;
      done += n * 512;
    }
    if (wbShort) sdlog("NativeSd: rw wb short %llu at %llu\n", (unsigned long long)wbShort, (unsigned long long)block);
    if (done != count * 512) sdlog("NativeSd: rw incomplete %llu/%llu blocks at %llu\n", (unsigned long long)(done / 512), (unsigned long long)count, (unsigned long long)block);
    // v23: large-read completion trace, THROTTLED — round 30's unthrottled
    // per-read lines (470+) flooded the 32 KB ramoops ring and erased the
    // boot's own history. Keep the first few plus every 128th, with the
    // running count so progress stays measurable.
    if (count >= 8 && (++bigReads <= 4 || (bigReads % 128) == 0))
      sdlog("NativeSd: rw done #%u %llu/%llu at %llu\n", bigReads, (unsigned long long)(done / 512),
            (unsigned long long)count, (unsigned long long)block);
    IOLockUnlock(lock);
    completion->action(completion->target, completion->parameter,
        done == count * 512 ? kIOReturnSuccess : kIOReturnIOError, done);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(NativeSd, IOBlockStorageDevice)

// A second storage device shares the host's lock, DMA buffer and error handling.
// Its offsets never enter the original OS image or the physical card's GPT.
class E5SystemOSDevice : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(E5SystemOSDevice)
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
    NativeSd *host = OSDynamicCast(NativeSd, getProvider());
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
    if (!data || !completion || !completion->action) return kIOReturnBadArgument;
    if (data->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    if (!E5SystemOS::contains(block, count)) return kIOReturnBadArgument;
    NativeSd *host = OSDynamicCast(NativeSd, getProvider());
    if (!host) return kIOReturnNotReady;
    return host->doAsyncReadWrite(data, E5SystemOS::base + block, count, attributes, completion);
  }
};
OSDefineMetaClassAndStructors(E5SystemOSDevice, IOBlockStorageDevice)

static bool publishSystemOS(NativeSd *host) {
  E5SystemOSDevice *window = new E5SystemOSDevice;
  if (!window) return false;
  if (!window->init(nullptr)) { window->release(); return false; }
  if (!window->attach(host)) { window->release(); return false; }
  if (!window->start(host)) { window->detach(host); window->release(); return false; }
  sdlog("NativeSd: publishing SystemOS read-only %llu sectors at %llu\n",
        (unsigned long long)E5SystemOS::sectors, (unsigned long long)E5SystemOS::base);
  window->registerService();
  window->release();
  return true;
}

// Select the APFS volume by its actual UUID, never by enumeration order.
extern "C" int e5_systemos_device(char *path, size_t size, int *major, int *minor) {
  OSDictionary *match = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *uuid = OSString::withCString(E5SystemOS::uuid);
  if (!match || !properties || !uuid) {
    if (match) match->release();
    if (properties) properties->release();
    if (uuid) uuid->release();
    return 0;
  }
  bool ready = properties->setObject("UUID", uuid) && match->setObject(gIOPropertyMatchKey, properties);
  properties->release(); uuid->release();
  if (!ready) { match->release(); return 0; }
  IOService *volume = IOService::waitForMatchingService(match, 30ULL * 1000000000ULL);
  match->release();
  if (!volume) return 0;
  OSString *name = OSDynamicCast(OSString, volume->getProperty("BSD Name"));
  OSNumber *maj = OSDynamicCast(OSNumber, volume->getProperty("BSD Major"));
  OSNumber *min = OSDynamicCast(OSNumber, volume->getProperty("BSD Minor"));
  bool ok = name && maj && min && size > strlen(name->getCStringNoCopy()) + 5 &&
      maj->unsigned64BitValue() <= 255 && min->unsigned64BitValue() <= 0xffffff;
  if (ok) {
    snprintf(path, size, "/dev/%s", name->getCStringNoCopy());
    *major = (int)maj->unsigned32BitValue(); *minor = (int)min->unsigned32BitValue();
    sdlog("NativeSd: SystemOS volume %s device %s\n", E5SystemOS::uuid, path);
  }
  volume->release();
  return ok ? 1 : 0;
}
