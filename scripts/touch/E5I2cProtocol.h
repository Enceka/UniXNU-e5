// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register/STOP/divider semantics are from Linux i2c-sprd.c, reimplemented
// for bounded polling. Registers supplies read/write32, read/write8, delayUs.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace E5Touch {
enum class Transfer { Good, Argument, Nack, Timeout, ShortRead };
template<class Registers> class I2c {
  Registers &io;
  uint32_t sampledStatus=0;
  unsigned sampledElapsed=0;
  bool sampledRead=false;
  static constexpr uint32_t Control=0,Address=4,Count=8,Rx=12,Tx=16,Status=20;
  static constexpr uint32_t Start=1,Enable=4,Read=8,Interrupt=4,Nack=2,Empty=8;
  static constexpr uint32_t SuppressStop=1u<<20,FullEnable=1u<<10;
  static constexpr uint32_t Configuration=(1u<<8)|(1u<<2)|(1u<<1)|(8u<<16)|(4u<<12);
  void clear() {io.write32(Status,io.read32(Status)&~(Interrupt|Nack));}
  Transfer message(const uint8_t *write,uint8_t *read,size_t size,bool stop) {
    if(!size || size>64 || (!read && (!write || size>12)))return Transfer::Argument;
    io.write32(0x2c,1);clear();io.write32(Address,0x2e<<1);io.write32(Count,static_cast<uint32_t>(size));
    uint32_t control=Configuration|(read?(Read|FullEnable):0)|(stop?0:SuppressStop);
    io.write32(Control,control);
    if(write)for(size_t i=0;i<size;++i)io.write8(Tx,write[i]);
    io.write32(Control,control|Start);
    sampledRead=read!=nullptr;
    size_t received=0;bool receiveQualified=false;
    for(unsigned elapsed=0;elapsed<10000;elapsed+=10) {
      uint32_t status=io.read32(Status);
      sampledStatus=status;sampledElapsed=elapsed;
      // RX_ACK is qualified at an interrupt/transfer completion, as in the
      // BSP ISR. Its value before completion must not abort an in-flight byte.
      if(read && (status&Interrupt))receiveQualified=true;
      // A final notification can arrive while the previous FIFO interrupt is
      // being acknowledged. Once this read has seen an interrupt, idle status
      // also qualifies its terminal ACK/FIFO tail; never grant that permission
      // from FIFO_EMPTY alone before the first notification of this message.
      const bool terminalRead=read && receiveQualified && !(status&1);
      if(((status&Interrupt) || terminalRead) && (status&Nack)){abort();return Transfer::Nack;}
      if(read && ((status&Interrupt) || terminalRead)) {
        while(received<size && !(io.read32(Status)&Empty))read[received++]=io.read8(Rx);
      }
      if(read && received==size) {
        const uint32_t finalStatus=io.read32(Status);
        sampledStatus=finalStatus;
        if(((finalStatus&Interrupt) || !(finalStatus&1)) && (finalStatus&Nack)){abort();return Transfer::Nack;}
        if(!(finalStatus&1)){io.write32(Control,control);clear();return Transfer::Good;}
      }
      if(status&Interrupt) {
        if(!read || (received==size && !(status&1))){io.write32(Control,control);clear();return Transfer::Good;}
        // FIFO threshold notification: drain it and keep receiving. An early
        // completion with an empty FIFO must not be reported as a full read.
        if(!(status&1)){abort();return Transfer::ShortRead;}
        clear();
      }
      else if(terminalRead && received<size){abort();return Transfer::ShortRead;}
      io.delayUs(10);
    }
    abort();return Transfer::Timeout;
  }
public:
  constexpr explicit I2c(Registers &registers):io(registers){}
  uint32_t lastStatus() const {return sampledStatus;}
  unsigned lastElapsedUs() const {return sampledElapsed;}
  bool lastWasRead() const {return sampledRead;}
  void configure() {
    // ext-26m -> 400 kHz, with 40/60 percent high/low timing.
    constexpr uint32_t divider=26000000/(4*400000)-1;
    constexpr uint32_t high=(divider*4)/5,low=(divider*6)/5;
    io.write32(Control,1u<<8);
    io.write32(0x20,(high<<16)|low);io.write32(0x24,0);io.write32(0x28,15);
    io.write32(0x2c,1);clear();io.write32(Control,Configuration);
  }
  void abort() {
    io.write32(Control,Configuration); // bit20 clear enables STOP, not suppresses it
    io.write32(0x2c,1);clear();
  }
  Transfer readRegister(uint8_t reg,uint8_t *data,size_t size) {
    if(!data || !size || size>64)return Transfer::Argument;
    const Transfer first=message(&reg,nullptr,1,false);
    return first==Transfer::Good?message(nullptr,data,size,true):first;
  }
  Transfer writeRegister(uint8_t reg,uint8_t value) {
    const uint8_t bytes[]={reg,value};return message(bytes,nullptr,sizeof(bytes),true);
  }
  Transfer readAddress(uint16_t address,uint8_t *data,size_t size) {
    if(!data || !size || size>64)return Transfer::Argument;
    const uint8_t prefix[]={static_cast<uint8_t>(address>>8),static_cast<uint8_t>(address)};
    const Transfer first=message(prefix,nullptr,sizeof(prefix),false);
    return first==Transfer::Good?message(nullptr,data,size,true):first;
  }
  Transfer writeAddress(uint16_t address,const uint8_t *data,size_t size) {
    if(!data || !size || size>10)return Transfer::Argument;
    uint8_t bytes[12]={static_cast<uint8_t>(address>>8),static_cast<uint8_t>(address)};
    for(size_t i=0;i<size;++i)bytes[2+i]=data[i];
    return message(bytes,nullptr,size+2,true);
  }
};
} // namespace E5Touch
