// SPDX-License-Identifier: BSD-2-Clause-Patent
// gpio-sprd.c falling-edge semantics, with GPIO13 masked at the shared parent.
#pragma once
#include <stdint.h>

namespace E5Touch {
template<class Registers> class GpioEvents {
  Registers &io;
  static constexpr uint32_t Pin=1u<<13;
  static constexpr unsigned Data=0,Is=0x0c,Ibe=0x10,Iev=0x14,Ie=0x18,Ris=0x1c,Ic=0x24;
  uint32_t saved[3]={};
  bool owned=false;
public:
  unsigned polls=0,reads=0,edges=0,lowWithoutEdge=0,skipped=0;
  constexpr explicit GpioEvents(Registers &registers):io(registers){}
  bool active()const{return owned;}
  bool start() {
    if(owned)return true;
    // This stage never enables the GPIO or GIC interrupt. Refuse a pin
    // which already has an unmasked owner instead of stealing its events.
    if(io.read32(Ie)&Pin)return false;
    const unsigned offsets[]={Is,Ibe,Iev};
    for(unsigned i=0;i<3;++i)saved[i]=io.read32(offsets[i]);
    owned=true;
    for(unsigned i=0;i<3;++i)io.write32(offsets[i],io.read32(offsets[i])&~Pin);
    io.write32(Ic,Pin); // W1C action, never saved/restored as ordinary state
    if((io.read32(Is)|io.read32(Ibe)|io.read32(Iev)|io.read32(Ie))&Pin){stop();return false;}
    polls=reads=edges=lowWithoutEdge=skipped=0;
    return true;
  }
  bool acquire() {
    if(!owned)return false;
    ++polls;
    const bool pending=(io.read32(Ris)&Pin)!=0,low=(io.read32(Data)&Pin)==0;
    if(!pending){if(low)++lowWithoutEdge;++skipped;return false;}
    // Ack before I2C: an edge arriving during the transaction must remain
    // latched for the next poll. A low level alone is diagnostic evidence,
    // never a read request, including the first packet and after recovery.
    // Multiple edges before this W1C can coalesce; RIS is not an edge counter.
    io.write32(Ic,Pin);++edges;
    ++reads;return true;
  }
  void stop() {
    if(!owned)return;
    io.write32(Ie,io.read32(Ie)&~Pin);
    const unsigned offsets[]={Is,Ibe,Iev};
    for(unsigned i=0;i<3;++i)
      io.write32(offsets[i],(io.read32(offsets[i])&~Pin)|(saved[i]&Pin));
    owned=false;
  }
};
} // namespace E5Touch
