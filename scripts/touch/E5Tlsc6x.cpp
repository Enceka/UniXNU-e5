// SPDX-License-Identifier: BSD-2-Clause-Patent
// E5 TLSC6x bring-up: media-independent, no firmware-flash update path.
// Hardware/protocol: sprd-linux-devs/linux e5-6.18 @6b355f72 and live E5 FDT.
#include "E5InputProtocol.h"
#include "E5I2cProtocol.h"
#include "E5Tlsc6xConfig.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <stdarg.h>
extern "C" void e5_kputc(char);

namespace {
void note(const char *format,...) __attribute__((format(printf,1,2)));
void note(const char *format,...) {
  char text[256];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);
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
unsigned mode=0,stage=0,polls=0,good=0,bad=0,recoveries=0,lastCount=0;
unsigned identityAttempts=0,resetRequested=0;
bool resetAttempted=false,resetPinQualified=false;
uint64_t retryDeadline=0;
uint8_t previous[18]={};bool havePrevious=false;
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
    if(resetRequested==1 && !resetPinQualified) {
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
  }
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
}

extern "C" void e5_touch_stop(void) {
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
}
extern "C" bool e5_touch_ready(void) {return mode==3 && stage==2 && good>0 && havePrevious;}

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
        resetAttempted=true;bus.abort();
        write(gpio,0,read(gpio,0)|(1u<<14));IOSleep(10);
        write(gpio,0,read(gpio,0)&~(1u<<14));IOSleep(10);
        write(gpio,0,read(gpio,0)|(1u<<14));IOSleep(60);
        note("E5TOUCH sensor-reset completed=1 irq=%u reset=%u (optional; no firmware programming)",
             (read(gpio,0)>>13)&1,(read(gpio,0)>>14)&1);
      }
      recover();return;
    }
    // TLSC6x's ordinary wake command, also used by the Linux resume path.
    const E5Touch::Transfer awake=bus.writeRegister(0xa5,0);
    note("E5TOUCH wake result=%u",static_cast<unsigned>(awake));
    if(awake!=E5Touch::Transfer::Good){recover();return;}
    if(!identifyFirmware()){recover();return;}
    stage=2;
  }
  if(mode==2)return;
  uint8_t packet[18]={};++polls;
  // Until GPIO IRQ routing is implemented, read every 16ms tick. A short
  // falling-edge pulse must not turn a tap/lift into a 500ms polling gap.
  const unsigned irqLevel=(read(gpio,0)>>13)&1;
  const E5Touch::Transfer result=bus.readRegister(0,packet,sizeof(packet));
  E5Input::Frame frame;
  const E5Input::PacketResult decoded=result==E5Touch::Transfer::Good?
      E5Input::decode(packet,sizeof(packet),frame):E5Input::PacketResult::Length;
  if(result!=E5Touch::Transfer::Good || decoded!=E5Input::PacketResult::Good) {
    ++bad;note("E5TOUCH error transfer=%u packet=%u bad=%u",static_cast<unsigned>(result),static_cast<unsigned>(decoded),bad);
    if(sink)sink(E5Input::Frame{},context);recover();return;
  }
  ++good;
  if(!havePrevious || memcmp(packet,previous,sizeof(packet))) {
    if(frame.count || lastCount) {
      note("E5TOUCH frame=%u count=%u c0=%u:%u,%u c1=%u:%u,%u source=hardware",good,frame.count,
           frame.contacts[0].id,frame.contacts[0].x,frame.contacts[0].y,
           frame.contacts[1].id,frame.contacts[1].x,frame.contacts[1].y);
      if(sink)sink(frame,context);
    }
    memcpy(previous,packet,sizeof(packet));havePrevious=true;lastCount=frame.count;
  }
  if((polls%2048)==1)note("E5TOUCH status polls=%u good=%u bad=%u recoveries=%u mode=%u irq=%u (16ms polling)",polls,good,bad,recoveries,mode,irqLevel);
}
