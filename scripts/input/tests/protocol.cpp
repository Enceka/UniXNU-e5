// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../E5InputProtocol.h"
#include "../../touch/E5I2cProtocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>

using namespace E5Input;
struct MockBus {
  enum Fault { None, WriteNack, ReadNack, Stall, Short } fault=None;
  unsigned time=0,phase=0,availableAt=0,readBytes=0,available=0,expected=0;
  uint32_t control=0,registers[16]={};bool active=false,isRead=false;
  std::vector<uint32_t> starts;
  uint32_t read32(unsigned offset) {
    if(offset!=20)return registers[offset/4];
    if(!active)return 8;
    if(fault==Stall || time<availableAt)return 9;
    if((fault==WriteNack && !isRead)||(fault==ReadNack && isRead))return 6;
    if(!isRead)return 4|((control&(1u<<20))?1:0);
    const unsigned limit=fault==Short?3:expected;
    const bool empty=readBytes>=available;
    const bool busy=fault!=Short && readBytes<expected;
    return 4|(empty?8:0)|(busy?1:0)|(limit?0:0);
  }
  void write32(unsigned offset,uint32_t value) {
    registers[offset/4]=value;
    if(offset==0) {
      control=value;
      if(value&1) {
        active=true;isRead=(value&8)!=0;++phase;availableAt=time+20;expected=registers[2];readBytes=0;
        available=fault==Short?3:(expected<8?expected:8);starts.push_back(value);
      } else active=false;
    }
    if(offset==20 && active && isRead && !(value&4)) {
      availableAt=time+20;const unsigned next=available+8;
      available=next<expected?next:expected;
    }
  }
  void write8(unsigned offset,uint8_t){assert(offset==16);}
  uint8_t read8(unsigned offset) {
    assert(offset==12 && active && isRead && time>=availableAt && readBytes<available);
    return static_cast<uint8_t>(readBytes++);
  }
  void delayUs(unsigned microseconds){time+=microseconds;}
};

int main() {
  unsigned wired=0;for(const auto &key:Keys)wired+=key.linuxCode!=0;assert(wired==20);
  KeyboardReport k;ConsumerReport c;
  reports(1u<<21,k,c);assert(c.buttons==1 && k.keys[0]==0); // physical Menu -> iOS Home
  reports((1u<<4)|(1u<<3)|VolumeDown,k,c);assert(k.modifiers==2 && k.keys[0]==0x26 && k.keys[1]==0x20 && c.buttons==8);
  reports((1u<<5)|(1u<<6)|(1u<<7)|(1u<<8),k,c);
  assert(k.keys[0]==0x4f && k.keys[1]==0x52 && k.keys[2]==0x28 && k.keys[3]==0x50);
  reports((1u<<28)-1,k,c);assert(k.keys[0]==1 && c.buttons==1); // rollover still carries Home
  MatrixState matrix;std::vector<uint32_t> events;
  auto emit=[&](uint32_t held){events.push_back(held);};
  matrix.update(0x11,0x77777730,false,emit);
  assert(events.size()==2 && events[0]==(1u<<21) && events[1]==0); // complete fast tap
  events.clear();matrix.update(3,0x77770230,true,emit);assert(matrix.held==((1u<<21)|(1u<<2)|VolumeDown));
  matrix.update(0x10,0x77777777,true,emit);assert(matrix.held==((1u<<2)|VolumeDown)); // idle coords on release
  matrix.update(0x20,0x77777777,false,emit);assert(matrix.held==0);
  events.clear();matrix.update(1,0x77777723,false,emit);assert(events.empty()); // unwired row
  matrix.update(1,0x77777777,false,emit);assert(events.empty()); // invalid coordinate

  uint8_t packet[18]={};packet[3]=packet[9]=0xc0;Frame frame;
  packet[2]=1;packet[3]=0;packet[4]=0;packet[5]=0;packet[6]=0;
  assert(decode(packet,18,frame)==PacketResult::Good && frame.count==1 && frame.contacts[0].id==0);
  TouchState touch;auto report=touch.report(frame);assert(report.count==1 && report.fingers[0][0]==3);
  packet[2]=2;packet[9]=1;packet[10]=0x40;packet[11]=0x11;packet[12]=0xe0;
  assert(decode(packet,18,frame)==PacketResult::Good && frame.count==2);
  report=touch.report(frame);assert(report.count==2 && report.fingers[1][1]==1);
  assert(report.fingers[1][2]==0xff && report.fingers[1][3]==0x7f && report.fingers[1][4]==0xff && report.fingers[1][5]==0x7f);
  report=touch.report(Frame{});assert(report.count==2 && report.fingers[0][0]==2 && report.fingers[1][0]==2);
  assert(touch.report(Frame{}).count==0); // each vanished ID has one lift
  Frame unchanged=frame;packet[11]=0x21;assert(decode(packet,18,frame)==PacketResult::Identifier && frame.count==unchanged.count);
  packet[11]=1;assert(decode(packet,18,frame)==PacketResult::Duplicate);
  packet[11]=0x11;packet[10]=0x41;assert(decode(packet,18,frame)==PacketResult::Coordinate);
  packet[2]=3;assert(decode(packet,18,frame)==PacketResult::Count);
  assert(decode(packet,17,frame)==PacketResult::Length);
  packet[2]=0;assert(decode(packet,18,frame)==PacketResult::Good && frame.count==0); // authoritative all-up
  packet[2]=1;packet[3]=0x40;packet[9]=0xc0;assert(decode(packet,18,frame)==PacketResult::Good && frame.count==0); // record-count includes lift
  assert(normalize(0,320)==0 && normalize(160,320)==16384 && normalize(320,320)==32767);

  MockBus registers;E5Touch::I2c<MockBus> bus(registers);bus.configure();
  assert(registers.registers[8]==((12u<<16)|18) && registers.registers[10]==15);
  uint8_t data[18]={};assert(bus.readRegister(0,data,18)==E5Touch::Transfer::Good);
  for(unsigned i=0;i<18;++i)assert(data[i]==i);
  assert(registers.starts.size()==2 && (registers.starts[0]&(1u<<20)) && !(registers.starts[1]&(1u<<20)));
  for(auto fault:{MockBus::WriteNack,MockBus::ReadNack,MockBus::Stall,MockBus::Short}) {
    MockBus broken;broken.fault=fault;E5Touch::I2c<MockBus> failing(broken);failing.configure();
    const auto result=failing.readRegister(0,data,18);
    assert(result==(fault==MockBus::Stall?E5Touch::Transfer::Timeout:fault==MockBus::Short?E5Touch::Transfer::ShortRead:E5Touch::Transfer::Nack));
    assert(!(broken.control&((1u<<20)|1)) && broken.time<=10000); // STOP allowed; no unbounded wait
  }
  MockBus invalid;E5Touch::I2c<MockBus> bounded(invalid);
  assert(bounded.readRegister(0,data,65)==E5Touch::Transfer::Argument && invalid.phase==0);
  puts("PASS: E5 key wiring/Home/chords/fast-release, two-contact bounds/lifts, and repeated-start/FIFO/NACK/timeout recovery");
}
