// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct E5TouchIrqState {
  uint32_t entries=0,callbacks=0,spurious=0,foreignBanks=0;
  uint64_t latestTick=0;
  // Workloop-only consumer fields. Retries leave callbacks unconsumed.
  unsigned consumed=0,reads=0,coalesced=0,checks=0,workRuns=0,notifications=0,workReads=0,timerReads=0;
  void record(uint64_t tick) {
    __atomic_store_n(&latestTick,tick,__ATOMIC_RELAXED);
    __atomic_fetch_add(&callbacks,1u,__ATOMIC_RELEASE);
  }
  unsigned pending()const {return __atomic_load_n(&callbacks,__ATOMIC_ACQUIRE)-consumed;}
  bool take(unsigned &batch,uint64_t &tick) {
    const unsigned current=__atomic_load_n(&callbacks,__ATOMIC_ACQUIRE);
    if(current==consumed)return false;
    batch=current-consumed;consumed=current;++reads;coalesced+=batch-1;
    // Latest primary observation before work starts, not a physical edge or
    // necessarily the first notification in this coalesced batch.
    tick=__atomic_load_n(&latestTick,__ATOMIC_ACQUIRE);return true;
  }
};

template<class Registers> bool e5TouchCaptureIrq(Registers &io,E5TouchIrqState &state,uint64_t tick) {
  constexpr uint32_t Pin=1u<<13;
  __atomic_fetch_add(&state.entries,1u,__ATOMIC_RELAXED);
  unsigned foreign=0;uint32_t ownedMis=0;
  for(unsigned bank=0;bank<16;++bank) {
    const unsigned base=bank*0x80;const uint32_t owned=bank==0?Pin:0;
    const uint32_t ie=io.read32(base+0x18)&0xffff,mis=io.read32(base+0x20)&0xffff;
    if((ie|mis)&~owned)foreign|=1u<<bank;
    if(!bank)ownedMis=mis&Pin;
  }
  if(foreign) {
    __atomic_fetch_or(&state.foreignBanks,foreign,__ATOMIC_RELEASE);
    io.write32(0x18,io.read32(0x18)&~Pin);io.maskParent();
    return true; // Wake deferred work to report the contained conflict.
  }
  if(!ownedMis) {__atomic_fetch_add(&state.spurious,1u,__ATOMIC_RELAXED);return false;}
  io.write32(0x24,Pin); // Only the owned W1C, before any deferred I2C work.
  state.record(tick);return true;
}
