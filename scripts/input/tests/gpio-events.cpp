// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../touch/E5GpioEvents.h"
#include <assert.h>
#include <stdint.h>
#include <iostream>
#include <utility>
#include <vector>

constexpr uint32_t Pin=1u<<13;
struct Registers {
  uint32_t data=Pin,is=0xffff,ibe=0xa5a5,iev=0xffff,ie=0,ris=0;
  bool rejectIs=false;
  std::vector<std::pair<unsigned,uint32_t>> writes;
  uint32_t read32(unsigned offset) {
    switch(offset){case 0:return data;case 0xc:return is;case 0x10:return ibe;
      case 0x14:return iev;case 0x18:return ie;case 0x1c:return ris;default:assert(false);return 0;}
  }
  void write32(unsigned offset,uint32_t value) {
    writes.emplace_back(offset,value);
    switch(offset){case 0xc:if(!rejectIs)is=value;break;case 0x10:ibe=value;break;case 0x14:iev=value;break;
      case 0x18:assert(!(value&Pin));ie=value;break;case 0x24:assert(value==Pin);ris&=~value;break;default:assert(false);}
  }
};
int main() {
  Registers occupied;occupied.ie=Pin;E5Touch::GpioEvents<Registers> refused(occupied);
  assert(!refused.start() && occupied.writes.empty());
  Registers broken;broken.rejectIs=true;E5Touch::GpioEvents<Registers> invalid(broken);
  assert(!invalid.start() && !invalid.active() && !invalid.acquire());
  assert(broken.is==0xffff && broken.ibe==0xa5a5 && broken.iev==0xffff && broken.ie==0);
  Registers r;r.ris=Pin|(1u<<3);const auto oldIs=r.is,oldIbe=r.ibe,oldIev=r.iev;
  E5Touch::GpioEvents<Registers> events(r);assert(events.start());
  assert(!(r.ie&Pin) && r.ris==(1u<<3));
  assert((r.is&~Pin)==(oldIs&~Pin) && (r.ibe&~Pin)==(oldIbe&~Pin) && (r.iev&~Pin)==(oldIev&~Pin));
  // Idle startup must not issue an unconditional baseline packet read.
  for(unsigned i=0;i<500;++i)assert(!events.acquire());
  assert(events.reads==0 && events.skipped==500);
  // A short pulse has ended before the timer samples DATA, but RIS retains it.
  r.ris|=Pin;assert(events.acquire());assert(!(r.ris&Pin));
  // Model a second falling edge during the subsequent I2C transaction.
  r.ris|=Pin;assert(events.acquire());assert(events.edges==2);
  assert(!(r.ris&Pin) && r.ris==(1u<<3));
  // Held-low without an edge must not turn into repeated I2C reads, even
  // when a consumer has discarded its previous packet after a decode error.
  r.data&=~Pin;
  for(unsigned i=0;i<500;++i)assert(!events.acquire());
  assert(events.lowWithoutEdge==500 && events.reads==2);
  r.ris|=Pin;assert(events.acquire());assert(events.reads==3);
  assert(!events.acquire());
  r.data|=Pin;assert(!events.acquire());
  assert(events.polls==events.reads+events.skipped && events.reads==events.edges);
  // Foreign changes survive stop; only GPIO13 configuration is restored.
  r.is^=1;r.ibe^=2;r.iev^=4;r.ie|=1u<<6;
  events.stop();assert(!events.active());
  assert(r.is==(oldIs^1) && r.ibe==(oldIbe^2) && r.iev==(oldIev^4));
  assert(r.ie==(1u<<6) && r.ris==(1u<<3));
  assert(!events.acquire());
  assert(events.start() && events.reads==0 && events.polls==0);
  assert(!events.acquire());events.stop();
  std::cout<<"PASS: strict masked GPIO edges, idle/held-low skip, pre-read ack, new-edge retention, rejected ownership/readback and pin-only restoration\n";
}
