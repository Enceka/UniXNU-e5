// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../E5TouchCanvas.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace E5Canvas;
static Sample point(unsigned id,unsigned x,unsigned y) {
  Sample s{};s.frame.count=1;s.frame.contacts[0]={uint8_t(id),uint16_t(x),uint16_t(y),true};return s;
}
struct Fixture {
  static constexpr uint16_t Guard=0x5a5a;
  std::vector<uint16_t> memory;
  Model model{};
  Fixture():memory(2*Pixels+2,Guard){assert(model.initialize(memory.data()+1,2*Pixels));}
  void guards()const{assert(memory.front()==Guard && memory.back()==Guard);}
};
static void clippingAndIds() {
  Fixture f;
  Sample s=point(1,320,480);s.frame.count=2;s.frame.contacts[1]={0,0,0,true};
  assert(f.model.apply(s));f.model.render(1,0);f.guards();
  assert(f.model.output[0]==White && f.model.output[Pixels-1]==White);
  assert(f.model.down[0] && f.model.down[1] && f.model.maxContacts==2 && f.model.downs==2);
  assert(f.model.apply(point(1,320,480)));f.model.render(2,0);f.guards();
  assert(f.model.output[0]==Ink[0] && f.model.output[Pixels-1]==White);
  assert(f.model.lifts==1 && f.model.current.count==1);
  assert(f.model.apply(Sample{}));f.model.render(3,0);f.guards();
  assert(f.model.current.count==0 && f.model.lifts==2 && f.model.ready);
  assert(f.model.output[Pixels-1]==Ink[1]);
}
static void movementAndCancellation() {
  Fixture moved;
  assert(moved.model.apply(point(0,40,180)) && moved.model.apply(point(0,280,180)));
  assert(moved.model.trail[180*Width+160]==Ink[0] && moved.model.downs==1);
  Fixture canceled;
  assert(canceled.model.apply(point(0,40,180)));
  Sample error{};error.kind=Kind::Cancel;error.errors=1;
  assert(canceled.model.apply(error));canceled.model.render(1,0);
  assert(canceled.model.current.count==0 && !canceled.model.ready && canceled.model.lifts==0 && canceled.model.cancels==1);
  Sample resumed=point(0,280,180);resumed.errors=1;assert(canceled.model.apply(resumed));
  assert(canceled.model.trail[180*Width+160]!=Ink[0]);
  assert(canceled.model.downs==2 && canceled.model.errors==1);canceled.guards();
}
static void overflowBreaksPaths() {
  Fixture f;Queue queue;
  assert(f.model.apply(point(0,40,180)));
  for(unsigned i=0;i<Queue::Capacity-1;++i)queue.push(point(0,40+i,180));
  queue.push(point(0,280,180));
  Sample latest{};assert(queue.overflows==1 && queue.pop(latest));
  assert(latest.breakPath && latest.frame.contacts[0].x==280 && !queue.pop(latest));
  assert(f.model.apply(latest));assert(f.model.trail[180*Width+160]!=Ink[0]);
  assert(f.model.current.count==1 && f.model.cancels==1 && f.model.lifts==0);
  queue.push(Sample{});assert(queue.pop(latest) && f.model.apply(latest));
  assert(f.model.current.count==0 && f.model.lifts==1);f.model.render(2,queue.overflows);f.guards();
}
static void keysDoNotReleaseFingers() {
  Fixture f;assert(f.model.apply(point(0,80,200)));
  Sample key{};key.kind=Kind::Key;key.keys=1u<<21;
  assert(f.model.apply(key) && f.model.apply(key));
  assert(f.model.home && f.model.homeEdges==1 && f.model.current.count==1);
  key.keys=0;assert(f.model.apply(key));
  assert(!f.model.home && f.model.homeEdges==2 && f.model.current.count==1);
  key.keys=1u<<21;assert(f.model.apply(key));key.keys=0;key.breakPath=true;assert(f.model.apply(key));
  assert(!f.model.home && f.model.current.count==0 && f.model.homeEdges==4);
}
static void rejectBadFramesAndStorage() {
  Fixture f;Model missing{};Sample sample=point(0,20,30);
  assert(!missing.initialize(nullptr,2*Pixels) && !missing.apply(sample));
  assert(!missing.initialize(f.memory.data()+1,2*Pixels-1));
  for(Sample bad: {point(2,1,1),point(0,321,1),point(0,1,481)}) {
    assert(f.model.apply(sample));assert(!f.model.apply(bad));
    assert(f.model.current.count==0 && !f.model.ready);f.guards();
  }
  Sample duplicate=point(0,10,20);duplicate.frame.count=2;duplicate.frame.contacts[1]={0,30,40,true};
  assert(!f.model.apply(duplicate));
  Sample count=point(0,1,1);count.frame.count=3;assert(!f.model.apply(count));
  f.model.errors=~0u;f.model.render(1,~0u);f.guards();
}
static void ppm(const std::string &path,const uint16_t *pixels) {
  std::ofstream out(path,std::ios::binary);out<<"P6\n320 480\n255\n";
  for(unsigned i=0;i<Pixels;++i) {
    const unsigned p=pixels[i];const char rgb[]={char(((p>>11)&31)*255/31),char(((p>>5)&63)*255/63),char((p&31)*255/31)};
    out.write(rgb,3);
  }
  assert(out.good());
}
int main(int argc,char **argv) {
  clippingAndIds();movementAndCancellation();overflowBreaksPaths();keysDoNotReleaseFingers();rejectBadFramesAndStorage();
  if(argc>1) {
    Fixture f;assert(f.model.apply(point(0,30,130)) && f.model.apply(point(0,90,210)));
    Sample two=point(1,250,340);two.frame.count=2;two.frame.contacts[1]={0,130,260,true};
    assert(f.model.apply(two));f.model.render(1,0);ppm(std::string(argv[1])+"-two.ppm",f.model.output);
    assert(f.model.apply(Sample{}));f.model.render(2,0);ppm(std::string(argv[1])+"-up.ppm",f.model.output);
  }
  std::cout<<"canvas bounds, identity, release, cancellation, overflow and key checks passed\n";
}
