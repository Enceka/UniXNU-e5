// SPDX-License-Identifier: BSD-2-Clause-Patent
// E5 matrix wiring from the actual FDT; HID usages follow USB HID Usage Tables.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace E5Input {
constexpr unsigned Width = 320, Height = 480, Contacts = 2;
constexpr uint32_t VolumeDown = 1u << 28;
struct Key { uint16_t linuxCode; uint8_t usage, modifiers, consumer; };
// row 0/1/3 are populated; row 2 and unwired cells never produce a key.
constexpr Key Keys[28] = {
  {158,0x29,0,0}, {4,0x20,0,0}, {7,0x23,0,0}, {10,0x26,0,0},
  {523,0x20,2,0}, {106,0x4f,0,0}, {103,0x52,0,0},
  {353,0x28,0,0}, {105,0x50,0,0}, {108,0x51,0,0}, {3,0x1f,0,0},
  {6,0x22,0,0}, {9,0x25,0,0}, {11,0x27,0,0},
  {},{},{},{},{},{},{},
  {139,0,0,1}, {169,0x28,0,0}, {}, {2,0x1e,0,0},
  {5,0x21,0,0}, {8,0x24,0,0}, {55,0x55,0,0}
};
struct KeyboardReport { uint8_t id=1, modifiers=0, reserved=0, keys[6]={}; };
struct ConsumerReport { uint8_t id=2, buttons=0; };
static_assert(sizeof(KeyboardReport)==9 && sizeof(ConsumerReport)==2, "HID wire lengths");

inline void reports(uint32_t held, KeyboardReport &keyboard, ConsumerReport &consumer) {
  keyboard = KeyboardReport{}; consumer = ConsumerReport{};
  unsigned count=0;
  for (unsigned i=0;i<28;++i) {
    if (!(held & (1u<<i))) continue;
    const Key &key=Keys[i];
    keyboard.modifiers |= key.modifiers; consumer.buttons |= key.consumer;
    if (!key.usage) continue;
    bool duplicate=false;
    for (unsigned j=0;j<count && j<6;++j) duplicate |= keyboard.keys[j]==key.usage;
    if (!duplicate && count<6) keyboard.keys[count++]=key.usage;
    else if (!duplicate) { for (auto &usage:keyboard.keys) usage=1; }
  }
  if (held & VolumeDown) consumer.buttons |= 8;
}

struct MatrixState {
  uint32_t held=0;
  uint8_t owners[4]={255,255,255,255};
  // Release coordinates can already be 0x77; remember each event slot's owner.
  template<class Emit> void update(uint32_t raw, uint32_t status, bool volumeDown, Emit emit) {
    for (unsigned slot=0;slot<4;++slot) {
      const unsigned col=(status>>(slot*8))&7, row=(status>>(slot*8+4))&7;
      if ((raw&(1u<<slot)) && row<4 && col<7 && Keys[row*7+col].linuxCode) {
        const unsigned key=row*7+col;
        if (owners[slot]!=255 && owners[slot]!=key) {
          held &= ~(1u<<owners[slot]); emit(held);
        }
        owners[slot]=static_cast<uint8_t>(key);
        const uint32_t next=held|(1u<<key);
        if (next!=held) {held=next;emit(held);}
      }
      if ((raw&(1u<<(slot+4))) && owners[slot]!=255) {
        held &= ~(1u<<owners[slot]); owners[slot]=255; emit(held);
      }
    }
    const uint32_t next=volumeDown?(held|VolumeDown):(held&~VolumeDown);
    if (next!=held) {held=next;emit(held);}
  }
};

struct Contact { uint8_t id=0; uint16_t x=0,y=0; bool down=false; };
struct Frame { unsigned count=0; Contact contacts[Contacts]{}; };
enum class PacketResult { Good, Length, Count, Identifier, Duplicate, Coordinate, Inconsistent };
// TLSC6x reg 0 report: two 6-byte slots at offsets 3 and 9; 0xc0 is unused,
// bit 6 means lift, id is the high Y nibble, and coordinates have 12 bits.
inline PacketResult decode(const uint8_t *packet,size_t size,Frame &output) {
  if (!packet || size!=18) return PacketResult::Length;
  const unsigned active=packet[2]&7;
  if (active>Contacts) return PacketResult::Count;
  if (!active) {output=Frame{};return PacketResult::Good;}
  Frame frame{}; unsigned down=0, records=0, ids=0;
  for (unsigned i=0;i<Contacts;++i) {
    const uint8_t *slot=packet+3+6*i;
    if ((slot[0]&0xc0)==0xc0) continue;
    ++records;
    const unsigned id=slot[2]>>4;
    if (id>=Contacts) return PacketResult::Identifier;
    if (ids&(1u<<id)) return PacketResult::Duplicate;
    ids |= 1u<<id;
    Contact c{};c.id=static_cast<uint8_t>(id);
    c.x=static_cast<uint16_t>(((slot[0]&15)<<8)|slot[1]);
    c.y=static_cast<uint16_t>(((slot[2]&15)<<8)|slot[3]);
    c.down=(slot[0]&0x40)==0;
    if (c.x>Width || c.y>Height) return PacketResult::Coordinate;
    if (c.down) {++down;frame.contacts[frame.count++]=c;}
  }
  // Firmware variants count either active tips or the records including lifts.
  if (active!=down && active!=records) return PacketResult::Inconsistent;
  output=frame;return PacketResult::Good;
}
inline uint16_t normalize(unsigned coordinate,unsigned maximum) {
  return static_cast<uint16_t>((coordinate*32767u+maximum/2)/maximum);
}
struct TouchReport { uint8_t id=3; uint8_t fingers[Contacts][6]={}; uint8_t count=0; };
static_assert(sizeof(TouchReport)==14,"Two-finger HID report wire length");
struct TouchState {
  Frame previous{};
  TouchReport report(const Frame &frame) {
    TouchReport result{};
    // One slot per stable hardware ID; absent previous IDs get an explicit lift.
    for (unsigned id=0;id<Contacts;++id) {
      const Contact *contact=nullptr;bool down=false;
      for (unsigned i=0;i<frame.count;++i) if (frame.contacts[i].id==id) {contact=&frame.contacts[i];down=true;}
      if (!contact) for (unsigned i=0;i<previous.count;++i) if (previous.contacts[i].id==id) contact=&previous.contacts[i];
      if (!contact) continue;
      uint8_t *slot=result.fingers[result.count++];
      const uint16_t x=normalize(contact->x,Width),y=normalize(contact->y,Height);
      slot[0]=down?3:2;slot[1]=static_cast<uint8_t>(id); // confidence stays set on lift
      slot[2]=static_cast<uint8_t>(x);slot[3]=static_cast<uint8_t>(x>>8);
      slot[4]=static_cast<uint8_t>(y);slot[5]=static_cast<uint8_t>(y>>8);
    }
    previous=frame;return result;
  }
};
} // namespace E5Input
