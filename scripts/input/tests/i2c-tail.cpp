// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../touch/E5I2cProtocol.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

// The last two bytes arrive between observing the threshold FIFO empty and
// acknowledging that interrupt. The level is cleared after completion, leaving
// the R294 terminal status: idle, nonempty FIFO, no interrupt. Readiness before
// the first interrupt must remain qualified, even if FIFO_EMPTY is transient.
struct TailBus {
  enum Fault { None, InitialUnqualified, TerminalNack, TerminalShort, FinalByteNack } fault=None;
  unsigned size=0,used=0,available=0,emptyReads=0,elapsed=0,reads=0;
  uint32_t control=0;
  bool active=false,reading=false,irq=false,terminal=false,lost=false,qualified=false;
  uint32_t read32(unsigned offset) {
    if(offset!=20)return 0;
    if(!active)return 8;
    if(!reading)return 5;
    if(fault==InitialUnqualified)return 0x310420; // not a completed read
    if(!terminal && used==available && available==16 && ++emptyReads>=2) {
      terminal=true;available=fault==TerminalShort?17:18;irq=true;
    }
    if(irq)qualified=true;
    return 0x310420|(irq?4:0)|(used==available?8:0)|(!terminal?1:0)|
        (terminal && (fault==TerminalNack || (fault==FinalByteNack && used==18))?2:0);
  }
  void write32(unsigned offset,uint32_t value) {
    if(offset==8)size=value;
    if(offset==0) {
      control=value;active=(value&1)!=0;
      if(active) {
        reading=(value&8)!=0;irq=true;used=0;available=8;
        terminal=false;emptyReads=0;qualified=false;
        if(reading && fault==InitialUnqualified)irq=false;
      }
    }
    if(offset==20 && active && reading && !(value&4)) {
      if(terminal){lost=true;irq=false;}
      else if(used==8){available=16;irq=true;}
    }
  }
  void write8(unsigned offset,uint8_t){assert(offset==16);}
  uint8_t read8(unsigned offset) {
    assert(offset==12 && reading && active && qualified && used<available && used<size);
    ++reads;return static_cast<uint8_t>(used++);
  }
  void delayUs(unsigned us){elapsed+=us;}
};

int main() {
  for(auto fault:{TailBus::None,TailBus::InitialUnqualified,TailBus::TerminalNack,TailBus::TerminalShort,TailBus::FinalByteNack}) {
    TailBus io;io.fault=fault;E5Touch::I2c<TailBus> bus(io);bus.configure();uint8_t bytes[18]={};
    const auto result=bus.readRegister(0,bytes,sizeof(bytes));
    if(fault==TailBus::None) {
      if(result!=E5Touch::Transfer::Good)
        fprintf(stderr,"tail result=%u received=%u lost=%u status=%x elapsed=%u\n",unsigned(result),io.used,io.lost?1:0,bus.lastStatus(),io.elapsed);
      assert(result==E5Touch::Transfer::Good && io.used==18 && io.lost);
      for(unsigned i=0;i<18;++i)assert(bytes[i]==i);
    } else if(fault==TailBus::InitialUnqualified) {
      assert(result==E5Touch::Transfer::Timeout && io.reads==0);
    } else if(fault==TailBus::TerminalNack || fault==TailBus::FinalByteNack) {
      assert(result==E5Touch::Transfer::Nack);
    } else assert(result==E5Touch::Transfer::ShortRead);
    assert(io.elapsed<=10000 && !(io.control&((1u<<20)|1)));
  }
  puts("PASS: lost terminal IRQ tail, initial readiness, terminal NACK/short-read, bounded STOP");
}
