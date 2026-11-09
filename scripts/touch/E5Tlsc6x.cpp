// SPDX-License-Identifier: BSD-2-Clause-Patent
// E5 TLSC6x bring-up: media-independent, no firmware-flash update path.
// Hardware/protocol: sprd-linux-devs/linux e5-6.18 @6b355f72 and live E5 FDT.
#include "E5InputProtocol.h"
#include "E5I2cProtocol.h"
#include "E5Tlsc6xConfig.h"
#include "E5GpioEvents.h"
#include "E5GpioIrq.h"
#include "ArmCpuSpi.h"
#include <IOKit/IOInterruptEventSource.h>
#include <machine/machine_routines.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <stdarg.h>
extern "C" void e5_kputc(char);
extern "C" bool e5_touch_canvas_start(void);
extern "C" void e5_touch_canvas_frame(const E5Input::Frame &);
extern "C" void e5_touch_canvas_cancel(unsigned);
extern "C" void e5_touch_canvas_stop(void);

namespace {
void note(const char *format,...) __attribute__((format(printf,1,2)));
void note(const char *format,...) {
  char text[512];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);
  for(const char *p=text;*p;++p)e5_kputc(*p);e5_kputc('\n');
}
// Board addresses include the parent bus translation, not the FDT unit offset.
// apahb-gate reg <0 0x3000>: address offset 0, size 0x3000 under
// syscon@20000000; the size must never be added to the parent address.
constexpr uint64_t I2cAddress=0x22270000,GateAddress=0x20000000;
constexpr uint64_t ClockAddress=0x20010000,AonAddress=0x64900000,GpioAddress=0x64170000;
constexpr uint64_t PinAddress=0x642e0000;
IOMemoryMap *controller=nullptr,*gateMap=nullptr,*clockMap=nullptr,*aon=nullptr,*gpio=nullptr;
IOMemoryMap *pinMap=nullptr;
uint32_t padsBefore[4]={};unsigned padsChanged=0;
uint32_t gpioMaskBefore=0,gpioDirectionBefore=0,gpioInputBefore=0;
IOMemoryMap *map(uint64_t address,size_t bytes) {
  IODeviceMemory *memory=IODeviceMemory::withRange(address,bytes);
  IOMemoryMap *mapping=memory?memory->map():nullptr;OSSafeReleaseNULL(memory);return mapping;
}
uint32_t read(IOMemoryMap *mapping,unsigned offset) {
  return *reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()+offset);
}
void write(IOMemoryMap *mapping,unsigned offset,uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()+offset)=value;
  __asm__ volatile("dsb sy":::"memory");
}
struct Registers {
  uint32_t read32(unsigned offset){return read(controller,offset);}
  void write32(unsigned offset,uint32_t value){write(controller,offset,value);}
  uint8_t read8(unsigned offset){return *reinterpret_cast<volatile uint8_t *>(controller->getVirtualAddress()+offset);}
  void write8(unsigned offset,uint8_t value){*reinterpret_cast<volatile uint8_t *>(controller->getVirtualAddress()+offset)=value;}
  void delayUs(unsigned microseconds){IODelay(microseconds);}
} registers;
E5Touch::I2c<Registers> bus(registers);
struct GpioRegisters {
  uint32_t read32(unsigned offset){return read(gpio,offset);}
  void write32(unsigned offset,uint32_t value){write(gpio,offset,value);}
} gpioRegisters;
E5Touch::GpioEvents<GpioRegisters> gpioEvents(gpioRegisters);
unsigned mode=0,stage=0,polls=0,good=0,bad=0,recoveries=0,lastCount=0;
unsigned identityAttempts=0,resetRequested=0;
unsigned rawRequested=0,rawSamples[3]={},eventPollRequested=0,eventIntervalMs=16;
unsigned irqRequested=0,irqCycleRequested=0,irqGeneration=0;
bool irqAttached=false,irqCycleDone=false,irqInWork=false;
uint64_t irqCycleDeadline=0,irqCycleQuietTicks=0,gicPastCount=0;
IOInterruptEventSource *irqSource=nullptr;
E5TouchIrqState irqState;
struct IrqRegisters {
  uint32_t read32(unsigned offset){return read(gpio,offset);}
  void write32(unsigned offset,uint32_t value){write(gpio,offset,value);}
  void maskParent(){armcpu_spi_mask_irq(72,&irqState);}
} irqRegisters;
void touchInterrupt(void *) {
  if(e5TouchCaptureIrq(irqRegisters,irqState,mach_absolute_time()) && irqSource)
    irqSource->interruptOccurred(nullptr,nullptr,0);
}
bool sharedSourcesClear() {
  unsigned foreign=0;
  for(unsigned bank=0;bank<16;++bank) {
    const unsigned base=bank*0x80;const uint32_t owned=bank==0?1u<<13:0;
    if((read(gpio,base+0x18)|read(gpio,base+0x20))&0xffff&~owned)foreign|=1u<<bank;
  }
  note("E5TOUCH irq-ownership banks=16 foreign=%x",foreign);return !foreign;
}
bool attachTouchIrq() {
  if(!irqSource || !sharedSourcesClear())return false;
  uint32_t state=0;uint64_t router=0;
  const IOReturn attached=armcpu_spi_attach_masked(72,false,touchInterrupt,&irqState,&state,&router);
  if(attached!=kIOReturnSuccess){note("E5TOUCH irq-attach refused=%x",unsigned(attached));return false;}
  irqAttached=true;++irqGeneration;
  const uint32_t control=armcpu_spi_control();const bool groupsVisible=(control&(1u<<6))!=0;
  note("E5TOUCH irq-attached parent=72 masked=1 state=%x router=%llx generation=%u groups_visible=%u control=%x",
       state,router,irqGeneration,groupsVisible?1u:0u,control);
  if((state&0xff06)!=0xa000 || (groupsVisible && !(state&1)))return false;
  write(gpio,0x24,1u<<13);write(gpio,0x18,read(gpio,0x18)|(1u<<13));
  const IOReturn enabled=armcpu_spi_set_enabled(72,true,&irqState);
  const bool valid=enabled==kIOReturnSuccess && (read(gpio,0x18)&(1u<<13)) &&
      !__atomic_load_n(&irqState.foreignBanks,__ATOMIC_ACQUIRE);
  note("E5TOUCH irq-enabled valid=%u result=%x pin=13 edge=falling parent=level-high generation=%u",
       valid?1:0,unsigned(enabled),irqGeneration);return valid;
}
bool detachTouchIrq() {
  if(!irqAttached)return true;
  write(gpio,0x18,read(gpio,0x18)&~(1u<<13));
  const IOReturn masked=armcpu_spi_set_enabled(72,false,&irqState);
  const IOReturn detached=armcpu_spi_detach(72,&irqState);
  note("E5TOUCH irq-detached result=%x masked=%x drained=%u generation=%u",
       unsigned(detached),unsigned(masked),detached==kIOReturnSuccess?1:0,irqGeneration);
  if(detached!=kIOReturnSuccess)return false;
  gicPastCount+=armcpu_spi_count(72);irqAttached=false;return true;
}
bool resetAttempted=false,resetPinQualified=false;
uint64_t retryDeadline=0;
uint8_t previous[18]={};bool havePrevious=false;
void packetHex(const uint8_t *packet,char *hex) {
  const char digits[]="0123456789abcdef";
  for(unsigned i=0;i<18;++i){hex[2*i]=digits[packet[i]>>4];hex[2*i+1]=digits[packet[i]&15];}
  hex[36]=0;
}
bool resetSensor(const char *reason) {
  if(!resetPinQualified || !gpio)return false;
  resetAttempted=true;bus.abort();
  write(gpio,0,read(gpio,0)|(1u<<14));IOSleep(10);
  write(gpio,0,read(gpio,0)&~(1u<<14));IOSleep(10);
  write(gpio,0,read(gpio,0)|(1u<<14));IOSleep(60);
  const bool released=(read(gpio,0)&(1u<<14))!=0;
  note("E5TOUCH sensor-reset completed=%u reason=%s irq=%u reset=%u (no firmware programming)",
       released?1:0,reason,(read(gpio,0)>>13)&1,(read(gpio,0)>>14)&1);
  return released;
}
void probeInterruptBanks() {
  unsigned requested=0;
  PE_parse_boot_argn("e5-touch-irq-probe",&requested,sizeof(requested));
  if(requested!=1)return;
  // gpio-sprd.c: 256 lines, 16 lines per 0x80-byte bank. All banks share
  // SPI40/INTID72. Reading bank0 alone cannot establish exclusive ownership
  // of that parent. GPIO clock and mapping are already qualified here.
  unsigned otherEnabled=0,otherPending=0;
  for(unsigned bank=0;bank<16;++bank) {
    const unsigned base=bank*0x80;
    const uint32_t owned=bank==0?1u<<13:0;
    const uint32_t ie=read(gpio,base+0x18)&0xffff;
    const uint32_t ris=read(gpio,base+0x1c)&0xffff;
    const uint32_t mis=read(gpio,base+0x20)&0xffff;
    if(ie&~owned)otherEnabled|=1u<<bank;
    if(mis&~owned)otherPending|=1u<<bank;
    if(bank==0 || ie || mis)
      note("E5TOUCH irq-probe bank=%u ie=%x ris=%x mis=%x is=%x ibe=%x iev=%x (read-only)",
           bank,ie,ris,mis,read(gpio,base+0xc)&0xffff,
           read(gpio,base+0x10)&0xffff,read(gpio,base+0x14)&0xffff);
  }
  note("E5TOUCH irq-probe banks=16 other_enabled=%x other_pending=%x parent_intid=72 attached=0",
       otherEnabled,otherPending);
  // IC is a write-one-to-clear action, not restorable state. Do not touch
  // IE/IC or attach the shared parent until masking/lifetime are qualified.
}
bool initialize() {
  gateMap=map(GateAddress,0x3000);clockMap=map(ClockAddress,0x1000);aon=map(AonAddress,0x3000);
  if(!gateMap || !clockMap || !aon){note("E5TOUCH map refused");return false;}
  const uint32_t before=read(gateMap,0);
  write(gateMap,0x1000,1u<<13);IODelay(1000);
  const uint32_t after=read(gateMap,0);
  if(!(after&(1u<<13))){note("E5TOUCH clock gate refused before=%x after=%x",before,after);return false;}
  // SPRD_COMP_CLK_DATA_OFFSET: mux at 0xa0, divider offset -4 at 0x9c.
  const uint32_t clockBefore=read(clockMap,0xa0);
  write(clockMap,0xa0,clockBefore&~3u);
  write(clockMap,0x9c,read(clockMap,0x9c)&~7u);IODelay(100);
  // AON GPIO clock before ANY GPIO access; only IRQ13 is owned here.
  write(aon,0x1004,1u<<1);IODelay(1000);
  controller=map(I2cAddress,0x1000);gpio=map(GpioAddress,0x1000);
  if(!controller || !gpio)return false;
  const uint32_t version=read(controller,0x1c);
  if(version==0xffffffff){note("E5TOUCH controller version refused");return false;}
  gpioMaskBefore=read(gpio,4);gpioDirectionBefore=read(gpio,8);gpioInputBefore=read(gpio,0x28);
  write(gpio,4,read(gpio,4)|(1u<<13)|(1u<<14));
  write(gpio,8,read(gpio,8)&~(1u<<13));write(gpio,0x28,read(gpio,0x28)|(1u<<13));
  // Keep the working Android pinmux, reset and firmware state. Stage 1 is
  // resource qualification; no sensor reset or firmware update is performed.
  note("E5TOUCH resources pa=%llx gate=%x:%x clock=%x:%x version=%x irq=%u reset=%u",
       I2cAddress,before,after,clockBefore,read(clockMap,0xa0),version,
       (read(gpio,0)>>13)&1,(read(gpio,0)>>14)&1);
  probeInterruptBanks();
  unsigned probePins=0,pinMode=0;
  PE_parse_boot_argn("e5-touch-probe",&probePins,sizeof(probePins));
  if(mode>=2)PE_parse_boot_argn("e5-touch-pinmux",&pinMode,sizeof(pinMode));
  if(probePins==1 || pinMode==1 || pinMode==2) {
    // ums9621-clk.c pin-eb: AON_APB_EB1 bit11, set mirror +0x1000.
    // Probe only: GPIO aliases' func4 must not be inferred to mean I2C.
    write(aon,0x1004,1u<<11);IODelay(1000);
    if(read(aon,4)&(1u<<11)) {
      pinMap=map(PinAddress,0x1000);
      if(pinMap) {
        for(unsigned i=0;i<4;++i)padsBefore[i]=read(pinMap,0xa0+4*i);
        // qogirn6lite common offset0x44 + indices23..26 *4.
        note("E5TOUCH pads pa=%llx irq=%x reset=%x sda=%x scl=%x iic3_route=%x (read-only)",
             PinAddress,padsBefore[0],padsBefore[1],padsBefore[2],padsBefore[3],read(pinMap,0x20));
        if(pinMode==1 || pinMode==2) {
          // Preload/deassert reset before connecting the pad to the GPIO
          // output, so a stale output latch cannot create an active-low glitch.
          write(gpio,0,read(gpio,0)|(1u<<14));
          write(gpio,0x28,read(gpio,0x28)&~(1u<<14));
          write(gpio,8,read(gpio,8)|(1u<<14));
          // Actual FDT gpio13/14 aliases select func4. Dedicated SDA3/SCL3
          // use func1 (qogirn6 convention); mode2 tests that source inference.
          // Change only function bits; preserve drive/pull/sleep settings.
          for(unsigned i=0;i<(pinMode==2?4u:2u);++i) {
            const uint32_t wanted=(padsBefore[i]&~0x30u)|(i<2?0x30u:0);
            if(wanted!=padsBefore[i]){write(pinMap,0xa0+4*i,wanted);padsChanged|=1u<<i;}
          }
          note("E5TOUCH pads configured mode=%u changed=%x irq=%x reset=%x sda=%x scl=%x",
               pinMode,padsChanged,read(pinMap,0xa0),read(pinMap,0xa4),read(pinMap,0xa8),read(pinMap,0xac));
          IOSleep(60);
        }
        resetPinQualified=(read(pinMap,0xa4)&0x30)==0x30;
      } else note("E5TOUCH pads map refused");
    } else note("E5TOUCH pads gate refused");
    note("E5TOUCH bus-lines status=%x control=%x",read(controller,0x14),read(controller,0));
  }
  if(mode>=2) {
    PE_parse_boot_argn("e5-touch-reset",&resetRequested,sizeof(resetRequested));
    if(resetRequested>2)resetRequested=0;
    if(resetRequested && !resetPinQualified) {
      note("E5TOUCH sensor-reset refused: GPIO14 pad mode not qualified; use e5-touch-probe=1");
      resetRequested=0;
    }
    const bool resetWasLow=(read(gpio,0)&(1u<<14))==0;
    // Deassert the owned active-low reset without pulsing a running sensor.
    // Write the output latch first to avoid a glitch when changing direction.
    write(gpio,0,read(gpio,0)|(1u<<14));
    write(gpio,0x28,read(gpio,0x28)&~(1u<<14));write(gpio,8,read(gpio,8)|(1u<<14));
    if(resetWasLow)IOSleep(60);
    bus.configure();
    // Android uses the A5 wake-only path for needKeepRamCode parts. The
    // observed flash-backed 3536 instead resets on resume. Mode 2 tests that
    // entry condition once; normal mode/configuration must still qualify.
    if(resetRequested==2 && !resetSensor("startup"))return false;
    PE_parse_boot_argn("e5-touch-raw",&rawRequested,sizeof(rawRequested));
    if(mode==3)PE_parse_boot_argn("e5-touch-gpio-events",&eventPollRequested,sizeof(eventPollRequested));
    if(mode==3)PE_parse_boot_argn("e5-touch-irq",&irqRequested,sizeof(irqRequested));
    if(irqRequested==1) {
      eventPollRequested=1;
      PE_parse_boot_argn("e5-touch-irq-cycle",&irqCycleRequested,sizeof(irqCycleRequested));
    }
    if(eventPollRequested==1) {
      PE_parse_boot_argn("e5-touch-event-ms",&eventIntervalMs,sizeof(eventIntervalMs));
      if(eventIntervalMs<1 || eventIntervalMs>16)eventIntervalMs=16;
    }
  }
  e5_touch_canvas_start();
  stage=1;return true;
}
bool directMode(bool &direct) {
  uint8_t data[3]={};
  if(bus.readAddress(1,data,sizeof(data))!=E5Touch::Transfer::Good)return false;
  direct=(data[0]>>1)==0x2e && data[2]==1;return true;
}
bool normalMode() {
  const uint8_t normal=5;
  for(unsigned i=0;i<2;++i) {
    if(bus.writeAddress(3,&normal,1)!=E5Touch::Transfer::Good)return false;
    IOSleep(5);bool direct=true;
    if(directMode(direct) && !direct)return true;
  }
  return false;
}
uint32_t word(const uint8_t *p) {
  return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
bool identifyFirmware() {
  bool direct=false;
  if(!directMode(direct))return false;
  const uint8_t command[]={0x28,0x35,0xc1,0,0x35,0xae},key=0x66;
  for(unsigned i=0;!direct && i<2;++i) {
    IOSleep(10);
    if(bus.writeAddress(0x42bd,command,sizeof(command))!=E5Touch::Transfer::Good)break;
    IOSleep(20);
    if(bus.writeAddress(0x040f,&key,1)!=E5Touch::Transfer::Good || !directMode(direct))break;
  }
  uint8_t boot[12]={},config[204]={};bool valid=false;
  if(direct && bus.readAddress(0x8000,boot,sizeof(boot))==E5Touch::Transfer::Good) {
    const uint32_t entry=word(boot),signature=word(boot+8);
    const uint16_t address=entry==0x35368008?0x9e00:entry==0x35358008?0xd6e0:0;
    if(signature==0x544c4e4b && address) {
      valid=true;
      for(unsigned offset=0;offset<sizeof(config);offset+=64) {
        const unsigned count=sizeof(config)-offset<64?sizeof(config)-offset:64;
        if(bus.readAddress(static_cast<uint16_t>(address+offset),config+offset,count)!=E5Touch::Transfer::Good){valid=false;break;}
      }
      if(valid) {
        const auto checked=E5Touch::validateConfiguration(config,sizeof(config));
        valid=checked==E5Touch::ConfigResult::Good;
        note("E5TOUCH config-validation result=%u bytes=204",static_cast<unsigned>(checked));
      }
      if(valid) {
        note("E5TOUCH firmware entry=%x boot_version=%x config_version=%x chip_code=%x config_bytes=204",
             entry,unsigned(boot[4])|(unsigned(boot[5])<<8),word(config),unsigned(config[106])|(unsigned(config[107])<<8));
        const char hex[]="0123456789abcdef";
        for(unsigned offset=0;offset<sizeof(config);offset+=64) {
          const unsigned count=sizeof(config)-offset<64?sizeof(config)-offset:64;char text[129];
          for(unsigned i=0;i<count;++i){text[2*i]=hex[config[offset+i]>>4];text[2*i+1]=hex[config[offset+i]&15];}
          text[2*count]=0;note("E5TOUCH config offset=%u len=%u hex=%s",offset,count,text);
        }
      }
    } else note("E5TOUCH firmware unknown entry=%x signature=%x (no download or flash update)",entry,signature);
  }
  // Always restore normal sensor mode before any touch packet read, including
  // failed identification. The magic commands change RAM/protocol mode only.
  const bool restored=normalMode();
  note("E5TOUCH identity valid=%u normal_mode=%u",valid?1:0,restored?1:0);
  return valid && restored;
}
void recover() {
  bus.abort();write(gateMap,0x1004,1u<<19);IODelay(100);
  write(gateMap,0x2004,1u<<19);IODelay(100);bus.configure();++recoveries;
  havePrevious=false;lastCount=0;
  clock_interval_to_deadline(500,1000000,&retryDeadline);
}
void eventStatus(unsigned irqLevel) {
  if(irqRequested==1) {
    if((irqState.checks%2048)!=1)return;
    const unsigned callbacks=__atomic_load_n(&irqState.callbacks,__ATOMIC_ACQUIRE);
    note("E5TOUCH irq-status checks=%u callbacks=%u consumed=%u reads=%u coalesced=%u pending=%u work_runs=%u work_reads=%u timer_reads=%u notifications=%u entries=%u spurious=%u foreign=%x ie=%x gic=%llu generation=%u good=%u bad=%u recoveries=%u",
         irqState.checks,callbacks,irqState.consumed,irqState.reads,irqState.coalesced,callbacks-irqState.consumed,
         irqState.workRuns,irqState.workReads,irqState.timerReads,irqState.notifications,__atomic_load_n(&irqState.entries,__ATOMIC_RELAXED),
         __atomic_load_n(&irqState.spurious,__ATOMIC_RELAXED),__atomic_load_n(&irqState.foreignBanks,__ATOMIC_ACQUIRE),
         read(gpio,0x18)&0xffff,gicPastCount+armcpu_spi_count(72),irqGeneration,good,bad,recoveries);
    return;
  }
  if((gpioEvents.polls%2048)!=1)return;
  note("E5TOUCH status polls=%u good=%u bad=%u recoveries=%u mode=%u irq=%u (GPIO event polling)",polls,good,bad,recoveries,mode,irqLevel);
  note("E5TOUCH events polls=%u reads=%u edges=%u low_no_edge=%u skipped=%u ie=%x ris=%x",
       gpioEvents.polls,gpioEvents.reads,gpioEvents.edges,gpioEvents.lowWithoutEdge,gpioEvents.skipped,
       read(gpio,0x18)&0xffff,read(gpio,0x1c)&0xffff);
}
}

extern "C" void e5_touch_set_irq_source(IOInterruptEventSource *source) {irqSource=source;}
extern "C" void e5_touch_irq_work(bool begin,unsigned count) {
  irqInWork=begin;if(begin){++irqState.workRuns;irqState.notifications+=count;}
}
extern "C" bool e5_touch_stop(void) {
  if(gpio && !detachTouchIrq())return false; // Retain mappings/context on timeout.
  e5_touch_canvas_stop();
  if(gpio)gpioEvents.stop();
  if(controller && mode>=2)bus.abort();
  if(pinMap)for(unsigned i=0;i<4;++i)if(padsChanged&(1u<<i))
    write(pinMap,0xa0+4*i,(read(pinMap,0xa0+4*i)&~0x30u)|(padsBefore[i]&0x30u));
  if(gpio) {
    write(gpio,4,(read(gpio,4)&~(1u<<13))|(gpioMaskBefore&(1u<<13)));
    write(gpio,8,(read(gpio,8)&~(1u<<13))|(gpioDirectionBefore&(1u<<13)));
    write(gpio,0x28,(read(gpio,0x28)&~(1u<<13))|(gpioInputBefore&(1u<<13)));
  }
  OSSafeReleaseNULL(controller);OSSafeReleaseNULL(gateMap);OSSafeReleaseNULL(clockMap);
  OSSafeReleaseNULL(aon);OSSafeReleaseNULL(gpio);
  OSSafeReleaseNULL(pinMap);padsChanged=0;
  stage=0;mode=0;havePrevious=false;lastCount=0;retryDeadline=0;
  identityAttempts=0;resetRequested=0;resetAttempted=false;resetPinQualified=false;
  rawRequested=0;for(auto &sample:rawSamples)sample=0;
  eventPollRequested=0;eventIntervalMs=16;irqRequested=0;irqSource=nullptr;
  return true;
}
extern "C" bool e5_touch_ready(void) {return mode==3 && stage==2 && good>0 && havePrevious;}
extern "C" unsigned e5_touch_poll_interval_ms(void) {
  return irqRequested!=1 && eventPollRequested==1 && stage==2 && gpioEvents.active()?eventIntervalMs:16;
}

extern "C" void e5_touch_poll(void (*sink)(const E5Input::Frame &,void *),void *context) {
  if(!stage) {
    if(!mode && !PE_parse_boot_argn("e5-touch",&mode,sizeof(mode)))return;
    if(mode<1 || mode>3)return;
    if(!initialize()){stage=99;return;}
  }
  if(stage==99 || mode==1 || (retryDeadline && mach_absolute_time()<retryDeadline))return;
  if(stage==1) {
    uint8_t tag=0;
    const E5Touch::Transfer result=bus.readRegister(0xa3,&tag,1);
    ++identityAttempts;
    if(identityAttempts<=16 || (identityAttempts%64)==0 || result==E5Touch::Transfer::Good)
      note("E5TOUCH identify a3=%x result=%u attempt=%u phase=%s status=%x elapsed_us=%u reset_done=%u",
           tag,static_cast<unsigned>(result),identityAttempts,bus.lastWasRead()?"read":"write",
           bus.lastStatus(),bus.lastElapsedUs(),resetAttempted?1:0);
    if(result!=E5Touch::Transfer::Good) {
      if(resetRequested==1 && !resetAttempted && identityAttempts>=4) {
        // Optional single vendor-style GPIO14 pulse, only after failed reads.
        // No firmware programming. A RAM-only sensor may need a later reload;
        // default trials preserve its existing runtime firmware instead.
        resetSensor("failed-identification");
      }
      recover();return;
    }
    // TLSC6x's ordinary wake command, also used by the Linux resume path.
    const E5Touch::Transfer awake=bus.writeRegister(0xa5,0);
    note("E5TOUCH wake result=%u",static_cast<unsigned>(awake));
    if(awake!=E5Touch::Transfer::Good){recover();return;}
    if(!identifyFirmware()){recover();return;}
    // Start a fresh packet-event epoch only after identity/direct-mode work
    // has restored normal mode. Those setup reads are not touch packet reads.
    if(eventPollRequested==1) {
      if(!gpioEvents.start()){note("E5TOUCH gpio-events refused=1");stage=99;return;}
      if(irqRequested==1) {
        if(!attachTouchIrq()){detachTouchIrq();note("E5TOUCH irq-start refused=1");stage=99;return;}
        clock_interval_to_deadline(300,1000000000,&irqCycleDeadline);
        uint64_t future=0;clock_interval_to_deadline(2,1000000000,&future);irqCycleQuietTicks=future-mach_absolute_time();
        note("E5TOUCH irq-configured pin=13 parent=72 deferred=IOInterruptEventSource i2c_in_irq=0 cycle=%u",irqCycleRequested);
      } else {
        note("E5TOUCH gpio-events configured=1 pin=13 edge=falling masked=1 parent_attached=0 strict=1 clock=mach_absolute_time");
        note("E5TOUCH gpio-cadence interval_ms=%u scope=hid-workloop period_includes_work=1",eventIntervalMs);
      }
    }
    stage=2;
  }
  if(mode==2)return;
  const unsigned irqLevel=(read(gpio,0)>>13)&1;
  uint64_t observed=eventPollRequested==1?mach_absolute_time():0;unsigned irqBatch=0;
  if(irqRequested==1) {
    ++irqState.checks;
    const unsigned foreign=__atomic_load_n(&irqState.foreignBanks,__ATOMIC_ACQUIRE);
    if(foreign) {note("E5TOUCH irq-conflict foreign=%x",foreign);stage=99;e5_touch_canvas_cancel(1);if(sink)sink(E5Input::Frame{},context);return;}
    if(irqCycleRequested==1 && !irqCycleDone && havePrevious && !lastCount && !irqState.pending() &&
        mach_absolute_time()>=irqCycleDeadline &&
        mach_absolute_time()-__atomic_load_n(&irqState.latestTick,__ATOMIC_ACQUIRE)>=irqCycleQuietTicks) {
      irqCycleDone=true;
      const bool detached=detachTouchIrq();const bool attached=detached && attachTouchIrq();
      note("E5TOUCH irq-cycle completed=%u detached=%u generation=%u",attached?1:0,detached?1:0,irqGeneration);
      if(!attached){stage=99;return;}
    }
    if(!irqState.take(irqBatch,observed)){eventStatus(irqLevel);return;}
    if(irqInWork)++irqState.workReads;else ++irqState.timerReads;
  } else if(eventPollRequested==1) {
    if(!gpioEvents.acquire()){eventStatus(irqLevel);return;}
  }
  uint8_t packet[18]={};++polls;
  // Default retains the qualified 16ms polling baseline. The opt-in event
  // path consumes only the masked falling-edge latch, with no idle fallback.
  // Do not emit serial diagnostics between acknowledging RIS and the read.
  const uint64_t started=eventPollRequested==1?mach_absolute_time():0;
  const E5Touch::Transfer result=bus.readRegister(0,packet,sizeof(packet));
  const uint64_t ended=eventPollRequested==1?mach_absolute_time():0;
  const unsigned irqAfter=(read(gpio,0)>>13)&1;
  const unsigned pendingAfter=eventPollRequested==1?((read(gpio,0x1c)>>13)&1):0;
  E5Input::Frame frame;
  const E5Input::PacketResult decoded=result==E5Touch::Transfer::Good?
      E5Input::decode(packet,sizeof(packet),frame):E5Input::PacketResult::Length;
  if(irqRequested==1 && (irqState.reads<=64 || (irqState.reads%256)==0 ||
      result!=E5Touch::Transfer::Good || decoded!=E5Input::PacketResult::Good))
    note("E5TOUCH irq-read seq=%u consumed=%u batch=%u irq_tick=%llu start=%llu end=%llu work_context=%u irq=%u:%u pending=%u transfer=%u packet=%u",
         irqState.reads,irqState.consumed,irqBatch,observed,started,ended,ml_at_interrupt_context()?1u:0u,
         irqLevel,irqAfter,irqState.pending(),static_cast<unsigned>(result),static_cast<unsigned>(decoded));
  if(irqRequested!=1 && eventPollRequested==1 && (gpioEvents.reads<=64 || (gpioEvents.reads%256)==0 ||
      result!=E5Touch::Transfer::Good || decoded!=E5Input::PacketResult::Good))
    note("E5TOUCH event-read seq=%u poll=%u observed=%llu start=%llu end=%llu irq=%u:%u pending_after=%u transfer=%u packet=%u",
         gpioEvents.reads,gpioEvents.polls,observed,started,ended,irqLevel,irqAfter,pendingAfter,
         static_cast<unsigned>(result),static_cast<unsigned>(decoded));
  if(result!=E5Touch::Transfer::Good || decoded!=E5Input::PacketResult::Good) {
    char raw[37];packetHex(packet,raw);
    ++bad;note("E5TOUCH error transfer=%u packet=%u bad=%u raw=%s status=%x irq=%u:%u",static_cast<unsigned>(result),static_cast<unsigned>(decoded),bad,raw,bus.lastStatus(),irqLevel,irqAfter);
    e5_touch_canvas_cancel(bad);
    if(sink)sink(E5Input::Frame{},context);recover();
    if(eventPollRequested==1)eventStatus(irqAfter);
    return;
  }
  ++good;
  if(!havePrevious || memcmp(packet,previous,sizeof(packet))) {
    const unsigned bucket=frame.count==2 || (packet[2]&7)==2?2:frame.count?1:0;
    const unsigned limit=bucket==2?32:bucket==1?16:8;
    if(rawRequested==1 && rawSamples[bucket]<limit) {
      char raw[37];packetHex(packet,raw);++rawSamples[bucket];
      note("E5TOUCH raw good=%u polls=%u declared=%u active=%u irq=%u:%u status=%x hex=%s",
           good,polls,packet[2]&7,frame.count,irqLevel,irqAfter,bus.lastStatus(),raw);
    }
    e5_touch_canvas_frame(frame);
    if(frame.count || lastCount) {
      note("E5TOUCH frame=%u count=%u c0=%u:%u,%u c1=%u:%u,%u source=hardware",good,frame.count,
           frame.contacts[0].id,frame.contacts[0].x,frame.contacts[0].y,
           frame.contacts[1].id,frame.contacts[1].x,frame.contacts[1].y);
      if(sink)sink(frame,context);
    }
    memcpy(previous,packet,sizeof(packet));havePrevious=true;lastCount=frame.count;
  }
  if(eventPollRequested==1)eventStatus(irqAfter);
  else if((polls%2048)==1)note("E5TOUCH status polls=%u good=%u bad=%u recoveries=%u mode=%u irq=%u (16ms polling)",polls,good,bad,recoveries,mode,irqLevel);
}
