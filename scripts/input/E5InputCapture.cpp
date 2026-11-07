// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opt-in, read-only scanout snapshots for autonomous before/after input checks.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
extern "C" void e5_kputc(char);
namespace {
void line(const char *text){for(const char *p=text;*p;++p)e5_kputc(*p);e5_kputc('\n');}
thread_call_t captureCall=nullptr;
unsigned pendingCapture=0,captureAttempts=0;
}
extern "C" bool e5_input_capture(unsigned id) {
  constexpr unsigned Width=320,Height=480,Bytes=Width*Height*2;
  char stage[96];snprintf(stage,sizeof(stage),"E5HID snapshot start id=%u stage=map",id);line(stage);
  IODeviceMemory *memory=IODeviceMemory::withRange(0xbe000000,Bytes);
  IOMemoryMap *map=memory?memory->map(kIOMapAnywhere|kIOMapReadOnly):nullptr;
  OSSafeReleaseNULL(memory);if(!map)return false;
  snprintf(stage,sizeof(stage),"E5HID snapshot start id=%u stage=allocate",id);line(stage);
  auto *copy=static_cast<uint16_t *>(IOMalloc(Bytes));
  if(!copy){map->release();return false;}
  const auto *source=reinterpret_cast<const volatile uint16_t *>(map->getVirtualAddress());
  snprintf(stage,sizeof(stage),"E5HID snapshot start id=%u stage=read",id);line(stage);
  for(unsigned i=0;i<Width*Height;++i)copy[i]=source[i];
  __asm__ volatile("dsb sy":::"memory");
  bool stable=true;
  for(unsigned i=0;i<Width*Height;++i)if(copy[i]!=source[i]){stable=false;break;}
  map->release();
  if(!stable){IOFree(copy,Bytes);line("E5HID snapshot unstable; retry without publishing a frame");return false;}
  char text[128];snprintf(text,sizeof(text),"E5HID snapshot id=%u source=retained-scanout-memory stable_reads=2",id);line(text);
  snprintf(text,sizeof(text),"E5FRAME begin id=%u width=320 height=480 format=RGB565",id);line(text);
  const char hex[]="0123456789abcdef";uint32_t checksum=2166136261u;
  for(unsigned y=0;y<Height;++y) {
    snprintf(text,sizeof(text),"E5FRAMEROW id=%u y=%u ",id,y);
    for(const char *p=text;*p;++p)e5_kputc(*p);
    for(unsigned x=0;x<Width;) {
      const uint16_t pixel=copy[y*Width+x];unsigned run=1;
      while(x+run<Width && copy[y*Width+x+run]==pixel)++run;
      for(unsigned i=0;i<run;++i){checksum=(checksum^(pixel&255))*16777619u;checksum=(checksum^(pixel>>8))*16777619u;}
      const uint32_t pair=(run<<16)|pixel;
      for(int shift=28;shift>=0;shift-=4)e5_kputc(hex[(pair>>shift)&15]);
      x+=run;
    }
    e5_kputc('\n');
  }
  snprintf(text,sizeof(text),"E5FRAME end id=%u fnv=%08x",id,checksum);line(text);
  IOFree(copy,Bytes);return true;
}
namespace {
void captureWorker(thread_call_param_t,thread_call_param_t) {
  const unsigned id=__atomic_load_n(&pendingCapture,__ATOMIC_ACQUIRE);
  if(!id)return;
  if(!e5_input_capture(id) && ++captureAttempts<5) {
    uint64_t deadline=0;clock_interval_to_deadline(1,1000000000,&deadline);
    thread_call_enter1_delayed(captureCall,nullptr,deadline);return;
  }
  __atomic_store_n(&pendingCapture,0,__ATOMIC_RELEASE);
}
}
extern "C" bool e5_input_request_capture(unsigned id) {
  if(!captureCall)captureCall=thread_call_allocate_with_options(captureWorker,nullptr,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
  if(!captureCall)return false;
  unsigned empty=0;
  if(!__atomic_compare_exchange_n(&pendingCapture,&empty,id,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
  captureAttempts=0;thread_call_enter(captureCall);return true;
}
