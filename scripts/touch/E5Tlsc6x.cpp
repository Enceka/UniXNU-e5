// SPDX-License-Identifier: BSD-2-Clause-Patent
// E5 TLSC6x bring-up: media-independent, no firmware-flash update path.
// Hardware/protocol: sprd-linux-devs/linux e5-6.18 @6b355f72 and live E5 FDT.
#include "E5InputProtocol.h"
#include "E5I2cProtocol.h"
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
constexpr uint64_t I2cAddress=0x22270000,GateAddress=0x20003000;
constexpr uint64_t ClockAddress=0x20010000,AonAddress=0x64900000,GpioAddress=0x64170000;
IOMemoryMap *controller=nullptr,*gate=nullptr,*clockMap=nullptr,*aon=nullptr,*gpio=nullptr;
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
uint64_t retryDeadline=0;
uint8_t previous[18]={};bool havePrevious=false;
bool initialize() {
  gate=map(GateAddress,0x3000);clockMap=map(ClockAddress,0x1000);aon=map(AonAddress,0x3000);
  if(!gate || !clockMap || !aon){note("E5TOUCH map refused");return false;}
  const uint32_t before=read(gate,0);
  write(gate,0x1000,1u<<13);IODelay(1000);
  const uint32_t after=read(gate,0);
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
  write(gpio,4,read(gpio,4)|(1u<<13));
  write(gpio,8,read(gpio,8)&~(1u<<13));write(gpio,0x28,read(gpio,0x28)|(1u<<13));
  // Keep the working Android pinmux, reset and firmware state. Stage 1 is
  // resource qualification; no sensor reset or firmware update is performed.
  note("E5TOUCH resources pa=%llx gate=%x:%x clock=%x:%x version=%x irq=%u reset=%u",
       I2cAddress,before,after,clockBefore,read(clockMap,0xa0),version,
       (read(gpio,0)>>13)&1,(read(gpio,0)>>14)&1);
  if(mode>=2)bus.configure();stage=1;return true;
}
void recover() {
  bus.abort();write(gate,0x1004,1u<<19);IODelay(100);
  write(gate,0x2004,1u<<19);IODelay(100);bus.configure();++recoveries;
  havePrevious=false;lastCount=0;
  clock_interval_to_deadline(500,1000000,&retryDeadline);
}
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
    note("E5TOUCH identify a3=%x result=%u (ESD identity tag; firmware/config version unqualified)",tag,static_cast<unsigned>(result));
    if(result!=E5Touch::Transfer::Good){recover();return;}
    // TLSC6x's ordinary wake command, also used by the Linux resume path.
    const E5Touch::Transfer awake=bus.writeRegister(0xa5,0);
    note("E5TOUCH wake result=%u",static_cast<unsigned>(awake));
    if(awake!=E5Touch::Transfer::Good){recover();return;}
    stage=2;
  }
  if(mode==2)return;
  uint8_t packet[18]={};++polls;
  // IRQ13 is sampled, with periodic reads for lost edges and all-up recovery.
  if(((read(gpio,0)>>13)&1) && (polls%32)!=1)return;
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
  if((polls%2048)==1)note("E5TOUCH status polls=%u good=%u bad=%u recoveries=%u mode=%u (GPIO polling)",polls,good,bad,recoveries,mode);
}
