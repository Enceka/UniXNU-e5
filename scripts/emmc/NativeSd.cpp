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
  } host, ahb, aon, clocks;
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
  bool setClock(uint32_t target) {
    uint16_t div = E5Sd::clockDivider(26000000, target);
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
    sdlog("NativeSd: cold legacy begin (rmldo+hotplug enable)\n");
    { // Vendor fast-hotplug sequence (realme/oppo sdhci-sprd.c set_power):
      // rmldo-en aon 0xba0 bit0 through the APB-prot window; 0x520 gets
      // protect-en bit20, debounce-en bit0, debounce count bits1-8 = 0 and
      // detect-polar bit21 CLEARED (cd-gpios flags 0x23 -> polar clear).
      uint32_t rm0 = aon.get(0xba0), hp0 = aon.get(0x520);
      protPut(aon, 0xba0, rm0 | 1u);
      protPut(aon, 0x520, E5Sd::hotplugValue(hp0));
      w8(0x29, 0x0f); // mirror the proven eMMC host power byte
      IODelay(20000);
      sdlog("NativeSd: hotplug %08x->%08x rmldo %08x->%08x prot %08x pstate %04x power %02x\n",
            hp0, aon.get(0x520), rm0, aon.get(0xba0), aon.get(0xff0), r16(0x20), r8(0x29));
    }
    w16(0x2c, 0);
    clocks.put(0x280, (clocks.get(0x280) & ~7u) | 1u); // ext-26m.
    clocks.put(0x284, clocks.get(0x284) & ~0x7ffu);
    clocks.put(0x288, clocks.get(0x288) | 1u); // 1x = 2x/2.
    w16(0x3e, 0x3000); w8(0x28, 0); w8(0x2e, 0x0e);
    if (!resetLines()) return false;
    w32(0x200, r32(0x200) & ~(1u << 21)); // No DLL below 52 MHz.
    w32(0x204, 0); w32(0x208, r32(0x208) | (1u << 13) | (1u << 21));
    w32(0x250, r32(0x250) & ~0x03000000u);
    w32(0x34, 0x1b7f410b); w32(0x38, 0);
    if (!setClock(400000)) return false;
    uint32_t st = command(0x0000, 0);
    sdlog("NativeSd: CMD0 st %08x clk %04x\n", st, r16(0x2c));
    if (!commandOk(st)) return false;
    st = command(0x081a, 0x1aa);
    sdlog("NativeSd: CMD8 st %08x r7 %08x\n", st, r32(0x10));
    if (!commandOk(st) || (r32(0x10) & 0xfff) != 0x1aa) {
      sdlog("NativeSd: CMD8 fail pstate %04x err %04x dllsts %08x dly %08x\n",
            r16(0x20), r16(0x32), r32(0x210), r32(0x208));
      return false;
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
      sdlog("NativeSd: ACMD41 fail pstate %04x err %04x\n", r16(0x20), r16(0x32));
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
    if (!ahb.open(0x20000000, 0x2000) || !aon.open(0x64900000, 0x2000) ||
        !clocks.open(0x64920000, 0x1000)) { sdlog("NativeSd: clock map failed\n"); return false; }
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
