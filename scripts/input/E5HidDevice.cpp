// SPDX-License-Identifier: BSD-2-Clause-Patent
// Report framing/lifecycle adapted from UniXNU/ShengHidInput, 71a40e76.
// E5 owns the matrix scanner and TLSC6x transport; no sheng MMIO is used.
#include "OriginalHidDeviceABI.h"
#include "E5InputProtocol.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSKext.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <stdarg.h>

extern "C" void e5_kputc(char);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void e5_touch_poll(void (*)(const E5Input::Frame &,void *),void *);

namespace {
void note(const char *format,...) __attribute__((format(printf,1,2)));
void note(const char *format,...) {
  char text[224];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);
  for (const char *p=text;*p;++p)e5_kputc(*p);e5_kputc('\n');
}
// One producer (the existing serial keypad poller) and one HID workloop consumer.
// Keep every transition, including a short press and release in the same scan.
constexpr unsigned QueueSize=64;
uint32_t queue[QueueSize],head=0,tail=0,enabled=0,latest=0,dropped=0,polls=0,physicalEvents=0;
E5Input::MatrixState matrix;
void enqueueHeld(uint32_t held) {
  __atomic_store_n(&latest,held,__ATOMIC_RELEASE);
  const uint32_t h=__atomic_load_n(&head,__ATOMIC_RELAXED);
  const uint32_t next=(h+1)%QueueSize;
  if(next==__atomic_load_n(&tail,__ATOMIC_ACQUIRE)){__atomic_add_fetch(&dropped,1,__ATOMIC_RELAXED);return;}
  queue[h]=held;__atomic_store_n(&head,next,__ATOMIC_RELEASE);
  __atomic_add_fetch(&physicalEvents,1,__ATOMIC_RELAXED);
}
bool dequeueHeld(uint32_t &held) {
  const uint32_t t=__atomic_load_n(&tail,__ATOMIC_RELAXED);
  if(t==__atomic_load_n(&head,__ATOMIC_ACQUIRE))return false;
  held=queue[t];__atomic_store_n(&tail,(t+1)%QueueSize,__ATOMIC_RELEASE);return true;
}
const unsigned char keyboardDescriptor[]={
  0x05,1,0x09,6,0xa1,1,0x85,1,
  0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
  0x75,8,0x95,1,0x81,3,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0,
  // Consumer Menu is the iOS Home button; keyboard Home (0x4a) is navigation.
  0x85,2,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,
  0x15,0,0x25,1,0x75,1,0x95,4,0x81,2,0x75,4,0x95,1,0x81,3,0xc0
};
#define FINGER_DESCRIPTOR \
  0x09,0x22,0xa1,2,0x09,0x42,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2, \
  0x09,0x47,0x81,2,0x75,6,0x95,1,0x81,3, \
  0x09,0x51,0x75,8,0x95,1,0x15,0,0x25,1,0x81,2, \
  0x05,1,0x09,0x30,0x15,0,0x26,0xff,0x7f,0x75,16,0x95,1,0x81,2, \
  0x09,0x31,0x81,2,0x05,0x0d,0xc0,
const unsigned char touchDescriptor[]={
  0x05,0x0d,0x09,4,0xa1,1,0x85,3,FINGER_DESCRIPTOR FINGER_DESCRIPTOR
  0x09,0x54,0x75,8,0x95,1,0x15,0,0x25,2,0x81,2,0xc0
};
bool eventDriver(IORegistryEntry *parent,unsigned depth) {
  if(!depth)return false;
  OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return false;
  bool found=false;
  while(OSObject *object=it->getNextObject()) {
    IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,object);
    if(entry && (entry->metaCast("IOHIDEventDriver") || eventDriver(entry,depth-1))){found=true;break;}
  }
  it->release();return found;
}
const OSMetaClass *meta(const char *name) {
  const OSSymbol *key=OSSymbol::withCString(name);
  const OSMetaClass *value=key?OSMetaClass::getMetaClassWithName(key):nullptr;
  OSSafeReleaseNULL(key);return value;
}
}

extern "C" void e5_input_matrix(uint32_t raw,uint32_t status,uint32_t gpioData) {
  if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
  __atomic_add_fetch(&polls,1,__ATOMIC_RELAXED);
  matrix.update(raw,status,(gpioData&(1u<<15))==0,enqueueHeld);
}

class E5HidDevice final:public IOHIDDevice {
  OSDeclareDefaultStructors(E5HidDevice)
  IOWorkLoop *loop=nullptr;
  IOTimerEventSource *timer=nullptr;
  bool driver=false,touch=false,probe=false,test=false,testDown=false,testDone=false;
  unsigned ticks=0,reports=0,failures=0,lastDropped=0,testAt=450;
  uint32_t held=0;
  uint64_t testDeadline=0;
  E5Input::TouchState touches;
  IOReturn send(const void *bytes,size_t length,const char *source) {
    IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(bytes,length,kIODirectionNone);
    const IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
    OSSafeReleaseNULL(buffer);++reports;if(result!=kIOReturnSuccess)++failures;
    if(reports<=16 || result!=kIOReturnSuccess)
      note("E5HID report=%u source=%s id=%u bytes=%u ret=%x",reports,source,
           static_cast<const uint8_t *>(bytes)[0],static_cast<unsigned>(length),static_cast<unsigned>(result));
    return result;
  }
  void sendKeys(uint32_t state,const char *source) {
    E5Input::KeyboardReport keyboard;E5Input::ConsumerReport consumer;
    E5Input::reports(state,keyboard,consumer);
    send(&keyboard,sizeof(keyboard),source);send(&consumer,sizeof(consumer),source);
  }
  static void touchReady(const E5Input::Frame &frame,void *context) {
    auto *self=static_cast<E5HidDevice *>(context);
    E5Input::TouchState before=self->touches;
    const E5Input::TouchReport report=self->touches.report(frame);
    if(report.count && self->send(&report,sizeof(report),"tlsc6x")!=kIOReturnSuccess)self->touches=before;
  }
  static void tick(OSObject *owner,IOTimerEventSource *source) {
    auto *self=OSDynamicCast(E5HidDevice,owner);if(!self)return;
    if((self->ticks++%64)==0)self->driver=eventDriver(self,4);
    if(self->driver) {
      uint32_t state;
      for(unsigned i=0;i<QueueSize && dequeueHeld(state);++i) {
        self->held=state;
        self->sendKeys(state|(self->testDown?(1u<<21):0),"matrix");
      }
      const uint32_t lost=__atomic_load_n(&dropped,__ATOMIC_RELAXED);
      if(lost!=self->lastDropped) {
        self->lastDropped=lost;self->held=__atomic_load_n(&latest,__ATOMIC_ACQUIRE);
        self->sendKeys(self->held|(self->testDown?(1u<<21):0),"overflow-reconcile");
      }
      if(self->test && !self->testDone && mach_absolute_time()>=self->testDeadline) {
        self->testDown=!self->testDown;
        self->sendKeys(self->held|(self->testDown?(1u<<21):0),"selftest-home");
        note("E5HID selftest=home down=%u driver=1 failures=%u",self->testDown?1:0,self->failures);
        if(self->testDown)clock_interval_to_deadline(100,1000000,&self->testDeadline);
        else self->testDone=true;
      }
    }
    if(self->probe)e5_touch_poll(self->touch && self->driver?touchReady:nullptr,self);
    if((self->ticks%2000)==1)note("E5HID status driver=%u polls=%u transitions=%u reports=%u failed=%u dropped=%u held=%x",
       self->driver?1:0,__atomic_load_n(&polls,__ATOMIC_RELAXED),__atomic_load_n(&physicalEvents,__ATOMIC_RELAXED),
       self->reports,self->failures,__atomic_load_n(&dropped,__ATOMIC_RELAXED),self->held);
    source->setTimeoutMS(16);
  }
public:
  IOWorkLoop *getWorkLoop() const override {return loop;}
  OSString *newTransportString() const override {return OSString::withCString("Internal");}
  OSString *newProductString() const override {return OSString::withCString("Rongyue E5 keypad and touchscreen");}
  IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
    if(!out)return kIOReturnBadArgument;
    uint8_t descriptor[sizeof(keyboardDescriptor)+sizeof(touchDescriptor)];
    memcpy(descriptor,keyboardDescriptor,sizeof(keyboardDescriptor));size_t size=sizeof(keyboardDescriptor);
    if(touch){memcpy(descriptor+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
    *out=IOBufferMemoryDescriptor::withBytes(descriptor,size,kIODirectionNone);
    return *out?kIOReturnSuccess:kIOReturnNoMemory;
  }
  bool start(IOService *provider) override {
    loop=IOWorkLoop::workLoop();if(!loop)return false;
    unsigned touchMode=0,testMode=0;
    touch=PE_parse_boot_argn("e5-touch",&touchMode,sizeof(touchMode)) && touchMode>=3;
    probe=touchMode>=1 && touchMode<=3;
    test=PE_parse_boot_argn("e5-hid-test",&testMode,sizeof(testMode)) && testMode==1;
    PE_parse_boot_argn("e5-hid-test-at",&testAt,sizeof(testAt));
    if(testAt<30 || testAt>600)testAt=450;
    if(!setProperty("HIDDefaultBehavior",true) || !setProperty("Built-In",true))return false;
    if(touch && (!setProperty("DisplayIntegrated",true) || !setProperty("DigitizerCollectionDispatch",true)))return false;
    if(!IOHIDDevice::start(provider))return false;
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!timer || loop->addEventSource(timer)!=kIOReturnSuccess){OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;}
    clock_interval_to_deadline(testAt,1000000000,&testDeadline);
    timer->setTimeoutMS(16);return true;
  }
  void stop(IOService *provider) override {
    __atomic_store_n(&enabled,0,__ATOMIC_RELEASE);
    if(timer){timer->cancelTimeout();loop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
    sendKeys(0,"stop");if(touch)touchReady(E5Input::Frame{},this);
    IOHIDDevice::stop(provider);
  }
  void free() override {OSSafeReleaseNULL(loop);IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(E5HidDevice,IOHIDDevice)
extern "C" void e5_hid_static_init(void) __asm__("__GLOBAL__sub_I_E5HidDevice.cpp");
extern "C" int e5_input_start(void) {
  unsigned requested=0;
  if(!PE_parse_boot_argn("e5-hid",&requested,sizeof(requested)) || requested!=1)return 0;
  static unsigned started;
  if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return 0;
  const char *const bundles[]={"com.apple.iokit.IOHIDFamily","com.apple.iokit.IOHIDEventDriver"};
  for(const char *bundle:bundles) {
    const OSReturn result=OSKext::loadKextWithIdentifier(bundle,false,false,kOSKextExcludeNone,kOSKextExcludeAll);
    note("E5HID load bundle=%s ret=%x",bundle,static_cast<unsigned>(result));
    if(result!=kOSReturnSuccess)return 0;
  }
  const OSMetaClass *parent=meta("IOHIDDevice");
  if(!parent || parent!=&IOHIDDevice::gMetaClass || parent->getClassSize()!=0xd0 || parent->getSuperClass()!=IOService::metaClass) {
    note("E5HID parent ABI refused meta=%p size=%u",parent,parent?parent->getClassSize():0);return 0;
  }
  void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return 0;
  e5_hid_static_init();const OSReturn registration=OSMetaClass::postModLoad(handle);
  if(registration!=kOSReturnSuccess){note("E5HID registration ret=%x",static_cast<unsigned>(registration));return 0;}
  const OSMetaClass *owned=meta("E5HidDevice");
  if(!owned || owned->getSuperClass()!=parent || owned->getClassSize()!=sizeof(E5HidDevice))return 0;
  OSDictionary *matching=IOService::serviceMatching("IOPlatformExpertDevice");
  IOService *provider=matching?IOService::copyMatchingService(matching):nullptr;
  OSSafeReleaseNULL(matching);
  auto *device=OSDynamicCast(E5HidDevice,OSMetaClass::allocClassWithName("E5HidDevice"));
  bool initialized=false,attached=false,live=false;
  if(provider && device){initialized=device->init();if(initialized)attached=device->attach(provider);if(attached)live=device->start(provider);}
  if(live){device->registerService();__atomic_store_n(&enabled,1,__ATOMIC_RELEASE);}
  else if(attached)device->detach(provider);
  note("E5HID publish=%u init=%u attach=%u parent_bytes=%u owned_bytes=%u",live?1:0,initialized?1:0,attached?1:0,parent->getClassSize(),static_cast<unsigned>(sizeof(E5HidDevice)));
  OSSafeReleaseNULL(provider);OSSafeReleaseNULL(device);return live?1:0;
}
