// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../touch/E5GpioIrq.h"
#include <cassert>
#include <iostream>
struct Registers {
  uint32_t ie[16]={},mis[16]={};unsigned clears=0,masks=0;
  uint32_t read32(unsigned offset) {return offset%0x80==0x18?ie[offset/0x80]:mis[offset/0x80];}
  void write32(unsigned offset,uint32_t value) {
    if(offset==0x18){ie[0]=value;return;}
    assert(offset==0x24 && value==(1u<<13));++clears;mis[0]&=~value;
  }
  void maskParent(){++masks;}
};
int main() {
  constexpr uint32_t Pin=1u<<13;Registers r;r.ie[0]=Pin;E5TouchIrqState state;
  assert(!e5TouchCaptureIrq(r,state,10) && state.spurious==1);
  r.mis[0]=Pin;assert(e5TouchCaptureIrq(r,state,20) && r.clears==1 && !r.mis[0]);
  unsigned batch=0;uint64_t tick=0;assert(state.take(batch,tick) && batch==1 && tick==20);
  // New callbacks during a read remain authorized for the next work action.
  r.mis[0]=Pin;assert(e5TouchCaptureIrq(r,state,30));
  r.mis[0]=Pin;assert(e5TouchCaptureIrq(r,state,40));
  assert(state.pending()==2 && state.take(batch,tick) && batch==2 && tick==40);
  assert(state.reads==2 && state.coalesced==1 && state.consumed==3);
  assert(!state.take(batch,tick));
  // Foreign enabled sources are contained without acknowledging their bits.
  r.ie[7]=4;r.mis[7]=4;r.mis[0]=Pin;
  assert(e5TouchCaptureIrq(r,state,50) && state.foreignBanks==(1u<<7));
  assert(r.masks==1 && !(r.ie[0]&Pin) && r.mis[0]==Pin && r.mis[7]==4 && r.clears==3);
  std::cout<<"PASS: owned IRQ acknowledgment, idle skip, concurrent new-event retention, explicit coalescing and shared-source containment\n";
}
