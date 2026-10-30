// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed-card, read-only E5 SD bring-up. SDIO0 is separate from eMMC/WCN.
// Resources and clock encodings are from the live DT and vendor clock driver.
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "TransferStatus.h"
#include "SdProtocol.h"

extern "C" void e5_panic_print(const char *);
static void sdlog(const char *fmt, ...) {
  char text[192]; va_list ap; va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap); va_end(ap); e5_panic_print(text);
}

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
  const char *mode = "none";
  uint32_t calls = 0;

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
  static uint32_t crc32(const uint8_t *p) {
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < 512; i++) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
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
      cardUp = false;
      if (!resetLines()) dmaQuarantined = true;
      return false;
    }
    return true;
  }
  bool verify() {
    const uint32_t lbas[] = {0, 1, 2048, 2050};
    const uint32_t crcs[] = {0xf9e85b82, 0xb2aa7578, 0xb2aa7578, 0xc1117912};
    for (unsigned i = 0; i < 4; i++) {
      if (!readSector(lbas[i])) return false;
      uint32_t crc = crc32(buffer);
      sdlog("NativeSd: reference lba %u st %08x crc %08x\n", lbas[i], r32(0x30), crc);
      if (crc != crcs[i]) return false;
      if (i == 0 && (buffer[510] != 0x55 || buffer[511] != 0xaa)) return false;
      if (i == 3 && (buffer[56] != 0x53 || buffer[57] != 0xef)) return false;
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
    { // v13: presence/polarity matrix. The Linux-proven state has PSTATE
      // bit16 (CINS) SET: the vendor driver never overrides get_cd, so the
      // sdhci default (PSTATE CINS) is the card detect, and fast_hotplug_enable
      // only runs when get_cd() is true -- the card inits fine under Android,
      // so CINS=1 there. Every powered round of ours reads CINS=0
      // (ps 01f00000 = line bits only). If the fast-hotplug hardware gates the
      // card bus clock while it believes "no card", that is exactly our
      // signature: CC on every command, responses never sampled (CRC error +
      // zero response register). Probe the aon detect plumbing under both
      // polarities and with fast-hotplug off entirely, CMD8 in each state.
      struct Cdp { const char* name; uint32_t hp; };
      const Cdp cdp[3] = {
        { "vendor", E5Sd::hotplugValue(aon.get(0x520)) },               // polar clr (cd-gpios 0x23)
        { "pinv",   E5Sd::hotplugValue(aon.get(0x520)) | (1u << 21) },  // polar set
        { "none",   0u },                                               // hotplug off
      };
      int win = -1;
      for (int i = 0; i < 3; i++) {
        protPut(aon, 0x520, cdp[i].hp);
        IODelay(20000);
        uint32_t ps = r32(0x24);
        uint32_t c8 = command(0x081a, 0x1aa);
        sdlog("NativeSd: cdpol %s hp %08x cins %c pstate %08x cmd8 st %08x r7 %08x\n",
              cdp[i].name, aon.get(0x520), (ps & 0x10000u) ? '1' : '0', ps, c8, r32(0x10));
        if (win < 0 && (r32(0x10) & 0xfff) == 0x1aa) win = i;
      }
      // Adopt the first state in which the card echoed CMD8; otherwise fall
      // back to the vendor state and let the retry loop below report it.
      protPut(aon, 0x520, (win >= 0) ? cdp[win].hp : cdp[0].hp);
      IODelay(20000);
      if (win >= 0) sdlog("NativeSd: cdpol win %s\n", cdp[win].name);
    }
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
    if (!readSector(0) || crc32(buffer) != 0xf9e85b82u) return false;
    st = command(0x371a, (uint32_t)rca << 16);
    if (!commandOk(st) || (r32(0x10) & E5Sd::r1Errors)) return false;
    st = command(0x061a, 2); // ACMD6: 4-bit, volatile bus setting only.
    if (!commandOk(st) || (r32(0x10) & E5Sd::r1Errors)) return false;
    w8(0x28, 2); if (!setClock(13000000)) return false;
    // Linux clears the <=400kHz CMD/POSRD delay-invert bits above 400kHz.
    w32(0x208, r32(0x208) & ~((1u << 13) | (1u << 21)));
    w32(0x250, r32(0x250) | 0x03000000u);
    mode = "legacy-4bit-13MHz"; return verify();
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
    sdlog("NativeSd: publishing read-only SD card %llu sectors\n", (unsigned long long)sectors);
    registerService(); return true;
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
  IOReturn reportEjectability(bool *b) override { *b = true; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *b) override { *b = true; return kIOReturnSuccess; }
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
    if (calls++ < 3) sdlog("NativeSd: rw %llu blocks at %llu\n", (unsigned long long)count, (unsigned long long)block);
    while (done < count * 512) {
      if (!readSector((uint32_t)(block + done / 512)) || data->writeBytes(done, buffer, 512) != 512) break;
      done += 512;
    }
    IOLockUnlock(lock);
    completion->action(completion->target, completion->parameter,
        done == count * 512 ? kIOReturnSuccess : kIOReturnIOError, done);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(NativeSd, IOBlockStorageDevice)
