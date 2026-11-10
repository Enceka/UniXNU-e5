// SPDX-License-Identifier: BSD-2-Clause-Patent
// A single diagnostic writer for the already-qualified firmware scanout.
#include "E5TouchCanvas.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern "C" void e5_kputc(char);
extern "C" bool e5_input_emit_frame(unsigned,const uint16_t *,size_t);

extern "C" bool e5_touch_canvas_selected(void) {
  unsigned canvas=0,hid=0,touch=0;
  PE_parse_boot_argn("e5-touch-canvas",&canvas,sizeof(canvas));
  PE_parse_boot_argn("e5-hid",&hid,sizeof(hid));
  PE_parse_boot_argn("e5-touch",&touch,sizeof(touch));
  return canvas==1 && hid==1 && touch==3 && PE_parse_boot_argn("e5-no-vc",nullptr,0);
}

namespace {
constexpr size_t Bytes=E5Canvas::Pixels*sizeof(uint16_t);
IOMemoryMap *scanout=nullptr;
IOLock *lock=nullptr;
thread_call_t call=nullptr;
uint16_t *storage=nullptr;
E5Canvas::Queue queue{};
E5Canvas::Model model{};
unsigned enabled=0,totalErrors=0,latestKeys=0,draws=0,heartbeat=0,readbackErrors=0;
unsigned snapshotMask=0,snapshotAttempts[5]={};
bool tried=false;
uint64_t started=0,heartbeatDeadline=0,longCaptureDeadline=0;

void note(const char *format,...) __attribute__((format(printf,1,2)));
void note(const char *format,...) {
  char text[384];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
  for(const char *p=text;*p;++p)e5_kputc(*p);e5_kputc('\n');
}
void flush() {
  auto *pixels=reinterpret_cast<volatile uint16_t *>(scanout->getVirtualAddress());
  for(unsigned i=0;i<E5Canvas::Pixels;++i)pixels[i]=model.output[i];
  __asm__ volatile("dsb sy":::"memory");++draws;
}
void snapshot(unsigned index) {
  if((snapshotMask&(1u<<index)) || snapshotAttempts[index]>=3)return;
  ++snapshotAttempts[index];
  const auto *pixels=reinterpret_cast<const volatile uint16_t *>(scanout->getVirtualAddress());
  for(unsigned i=0;i<E5Canvas::Pixels;++i)if(pixels[i]!=model.output[i]) {
    ++readbackErrors;
    note("E5CANVAS readback-failed=%u index=%u actual=%x expected=%x",readbackErrors,i,pixels[i],model.output[i]);
    return;
  }
  // This worker is the sole writer. Every physical pixel has just matched
  // the immutable raster passed to the existing RLE/checksum encoder.
  note("E5CANVAS snapshot id=%u draw=%u seq=%u count=%u c0=%u:%u,%u c1=%u:%u,%u readback=1",
       200+index,draws,model.sequence,model.current.count,
       model.current.contacts[0].id,model.current.contacts[0].x,model.current.contacts[0].y,
       model.current.contacts[1].id,model.current.contacts[1].x,model.current.contacts[1].y);
  if(e5_input_emit_frame(200+index,model.output,Bytes))snapshotMask|=1u<<index;
}
void worker(thread_call_param_t,thread_call_param_t) {
  if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
  const unsigned beforeCount=model.current.count,beforeErrors=model.errors;
  const bool beforeHome=model.home;
  E5Canvas::Sample sample{};bool dirty=draws==0;
  for(unsigned i=0;i<E5Canvas::Queue::Capacity;++i) {
    IOLockLock(lock);const bool have=queue.pop(sample);IOLockUnlock(lock);
    if(!have)break;
    if(!model.apply(sample))note("E5CANVAS invalid-frame seq=%u",model.sequence);
    dirty=true;
  }
  const uint64_t now=mach_absolute_time();
  if(!heartbeatDeadline || now>=heartbeatDeadline) {
    ++heartbeat;dirty=true;clock_interval_to_deadline(1,1000000000,&heartbeatDeadline);
  }
  IOLockLock(lock);const unsigned drops=queue.overflows;IOLockUnlock(lock);
  if(dirty) {
    model.render(heartbeat,drops);flush();
    if(model.ready) {
      snapshot(0);
      if(model.current.count==1)snapshot(1);
      if(model.current.count==2)snapshot(2);
      if((snapshotMask&4) && !model.current.count)snapshot(3);
      if(now>=longCaptureDeadline)snapshot(4);
    }
    if(draws<=4 || (draws%32)==1 || model.current.count!=beforeCount ||
       model.errors!=beforeErrors || model.home!=beforeHome) {
      uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-started,&ns);
      note("E5CANVAS status draw=%u seq=%u ready=%u active=%u max=%u down=%u up=%u cancel=%u errors=%u dropped=%u home=%u edges=%u readback_errors=%u age_ms=%llu",
           draws,model.sequence,model.ready?1:0,model.current.count,model.maxContacts,
           model.downs,model.lifts,model.cancels,model.errors,drops,model.home?1:0,
           model.homeEdges,readbackErrors,(unsigned long long)(ns/1000000));
    }
  }
  // Serialize rearming with stop's disable, so cancel_wait cannot miss a
  // rearm decided just before the worker observes the disabled state.
  IOLockLock(lock);
  if(__atomic_load_n(&enabled,__ATOMIC_ACQUIRE)) {
    uint64_t deadline=0;clock_interval_to_deadline(33,1000000,&deadline);
    thread_call_enter1_delayed(call,nullptr,deadline);
  }
  IOLockUnlock(lock);
}
void submit(E5Canvas::Sample sample) {
  if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
  IOLockLock(lock);
  if(sample.kind==E5Canvas::Kind::Cancel)totalErrors=sample.errors;
  if(sample.kind==E5Canvas::Kind::Key)latestKeys=sample.keys;
  sample.errors=totalErrors;sample.keys=latestKeys;
  queue.push(sample);IOLockUnlock(lock);
}
}

extern "C" bool e5_touch_canvas_start(void) {
  if(tried)return __atomic_load_n(&enabled,__ATOMIC_ACQUIRE)!=0;
  if(!e5_touch_canvas_selected())return false;
  tried=true;
  PE_Video video{};
  if(PE_current_console(&video) || video.v_baseAddr!=0xbe000000ULL ||
     video.v_width!=320 || video.v_height!=480 || video.v_depth!=16 ||
     video.v_rowBytes!=640 || video.v_rotate) {
    note("E5CANVAS refused reason=firmware-geometry");return false;
  }
  IODeviceMemory *memory=IODeviceMemory::withRange(0xbe000000ULL,Bytes);
  scanout=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
  OSSafeReleaseNULL(memory);
  lock=IOLockAlloc();storage=static_cast<uint16_t *>(IOMalloc(2*Bytes));
  call=thread_call_allocate_with_options(worker,nullptr,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
  if(!scanout || !scanout->getVirtualAddress() || scanout->getPhysicalAddress()!=0xbe000000ULL ||
     scanout->getLength()!=Bytes || !lock || !storage || !call || !model.initialize(storage,2*E5Canvas::Pixels)) {
    if(call){thread_call_free(call);call=nullptr;}
    if(storage){IOFree(storage,2*Bytes);storage=nullptr;}
    if(lock){IOLockFree(lock);lock=nullptr;}
    OSSafeReleaseNULL(scanout);note("E5CANVAS refused reason=resources");return false;
  }
  queue=E5Canvas::Queue{};started=mach_absolute_time();
  clock_interval_to_deadline(600,1000000000,&longCaptureDeadline);
  __atomic_store_n(&enabled,1,__ATOMIC_RELEASE);
  note("E5CANVAS start=1 width=320 height=480 stride=640 physical=be000000 source=hardware auto_input=0");
  thread_call_enter(call);return true;
}
extern "C" void e5_touch_canvas_frame(const E5Input::Frame &frame) {
  E5Canvas::Sample sample{};sample.frame=frame;submit(sample);
}
extern "C" void e5_touch_canvas_cancel(unsigned errors) {
  E5Canvas::Sample sample{};sample.kind=E5Canvas::Kind::Cancel;sample.errors=errors;submit(sample);
}
extern "C" void e5_touch_canvas_key(unsigned keys) {
  E5Canvas::Sample sample{};sample.kind=E5Canvas::Kind::Key;sample.keys=keys;submit(sample);
}
extern "C" void e5_touch_canvas_stop(void) {
  if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
  IOLockLock(lock);const bool wasEnabled=__atomic_exchange_n(&enabled,0,__ATOMIC_ACQ_REL)!=0;IOLockUnlock(lock);
  if(!wasEnabled)return;
  // No producer remains after the HID timer is removed. Never hold the queue
  // lock across cancellation: an already running callback must be able to exit.
  thread_call_cancel_wait(call);
  if(!thread_call_free(call)) {
    note("E5CANVAS stop-quarantined pending-callback=1");return;
  }
  call=nullptr;model.cancel();model.home=false;model.render(heartbeat,queue.overflows);flush();
  note("E5CANVAS stop=1 active=0 draw=%u errors=%u dropped=%u",draws,model.errors,queue.overflows);
  OSSafeReleaseNULL(scanout);IOFree(storage,2*Bytes);storage=nullptr;IOLockFree(lock);lock=nullptr;
  model=E5Canvas::Model{};queue=E5Canvas::Queue{};
  draws=heartbeat=readbackErrors=snapshotMask=totalErrors=latestKeys=0;
  for(auto &attempt:snapshotAttempts)attempt=0;
  started=heartbeatDeadline=longCaptureDeadline=0;tried=false;
}
