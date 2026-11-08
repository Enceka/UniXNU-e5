// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure CPU diagnostic raster and bounded event queue; no hardware or UIKit API.
#pragma once
#include "E5InputProtocol.h"
#include <stddef.h>
#include <stdint.h>

namespace E5Canvas {
constexpr unsigned Width=320,Height=480,Pixels=Width*Height;
constexpr uint16_t Background=0x0842,Grid=0x18e3,Target=0x7bef,White=0xffff;
constexpr uint16_t Ink[2]={0x0330,0x680d},Tip[2]={0x07ff,0xf81f};
enum class Kind : unsigned { Hardware, Cancel, Key };
struct Sample {
  E5Input::Frame frame{};
  Kind kind=Kind::Hardware;
  unsigned errors=0,keys=0;
  bool breakPath=false;
};
struct Queue {
  static constexpr unsigned Capacity=64;
  Sample samples[Capacity]{};
  unsigned head=0,tail=0,overflows=0;
  // The platform serializes push/pop. If samples are lost, the next sample
  // explicitly breaks all old paths instead of inventing a move or a lift.
  void push(Sample sample) {
    if((head+1)%Capacity==tail) {
      head=tail=0;++overflows;sample.breakPath=true;
    }
    samples[head]=sample;head=(head+1)%Capacity;
  }
  bool pop(Sample &sample) {
    if(head==tail)return false;
    sample=samples[tail];tail=(tail+1)%Capacity;return true;
  }
};

inline void pixel(uint16_t *pixels,int x,int y,uint16_t color) {
  if(x>=0 && y>=0 && x<int(Width) && y<int(Height))pixels[y*Width+x]=color;
}
inline int coordinate(unsigned value,unsigned limit) {return int(value<limit?value:limit-1);}
inline void line(uint16_t *pixels,int x0,int y0,int x1,int y1,uint16_t color) {
  const int dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y0-y1:y1-y0;
  const int sx=x0<x1?1:-1,sy=y0<y1?1:-1;int error=dx+dy;
  for(;;) {
    pixel(pixels,x0,y0,color);
    if(x0==x1 && y0==y1)break;
    const int twice=2*error;
    if(twice>=dy){error+=dy;x0+=sx;}
    if(twice<=dx){error+=dx;y0+=sy;}
  }
}
inline void box(uint16_t *pixels,int x,int y,unsigned width,unsigned height,uint16_t color) {
  for(unsigned row=0;row<height;++row)for(unsigned col=0;col<width;++col)
    pixel(pixels,x+int(col),y+int(row),color);
}
inline uint8_t glyph(char c,unsigned row) {
  static constexpr uint8_t letters[26][7]={
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
  static constexpr uint8_t digits[10][7]={
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}};
  if(c>='A' && c<='Z')return letters[c-'A'][row];
  if(c>='0' && c<='9')return digits[c-'0'][row];
  if(c=='-')return row==3?31:0;
  return 0;
}
inline void text(uint16_t *pixels,int x,int y,const char *string,uint16_t color,unsigned scale=2) {
  for(;*string;++string,x+=int(6*scale))for(unsigned row=0;row<7;++row)
    for(unsigned col=0;col<5;++col)if(glyph(*string,row)&(1u<<(4-col)))
      box(pixels,x+int(col*scale),y+int(row*scale),scale,scale,color);
}
inline void number(uint16_t *pixels,int x,int y,unsigned value,uint16_t color,unsigned scale=2) {
  char digits[11]={};unsigned count=0;
  do{digits[count++]=char('0'+value%10);value/=10;}while(value && count<10);
  for(unsigned i=0;i<count/2;++i){const char c=digits[i];digits[i]=digits[count-i-1];digits[count-i-1]=c;}
  text(pixels,x,y,digits,color,scale);
}
inline bool valid(const E5Input::Frame &frame) {
  if(frame.count>2)return false;
  unsigned ids=0;
  for(unsigned i=0;i<frame.count;++i) {
    const auto &c=frame.contacts[i];
    if(c.id>=2 || !c.down || c.x>Width || c.y>Height || (ids&(1u<<c.id)))return false;
    ids|=1u<<c.id;
  }
  return true;
}

struct Model {
  uint16_t *trail=nullptr,*output=nullptr;
  E5Input::Frame current{};
  E5Input::Contact last[2]{};
  bool down[2]={},ready=false,home=false;
  unsigned sequence=0,errors=0,maxContacts=0,downs=0,lifts=0,cancels=0,homeEdges=0;
  bool initialize(uint16_t *storage,size_t count) {
    if(!storage || count<2*Pixels)return false;
    *this=Model{};trail=storage;output=storage+Pixels;
    for(unsigned i=0;i<Pixels;++i)trail[i]=Background;
    for(unsigned x=0;x<Width;x+=40)line(trail,x,0,x,Height-1,Grid);
    for(unsigned y=0;y<Height;y+=40)line(trail,0,y,Width-1,y,Grid);
    const unsigned targets[][2]={{16,16},{303,16},{160,240},{16,463},{303,463}};
    for(const auto &target:targets) {
      line(trail,int(target[0])-7,target[1],int(target[0])+7,target[1],Target);
      line(trail,target[0],int(target[1])-7,target[0],int(target[1])+7,Target);
    }
    return true;
  }
  void cancel() {
    if(current.count)++cancels;
    current=E5Input::Frame{};down[0]=down[1]=false;ready=false;
  }
  bool apply(const Sample &sample) {
    if(!trail || !output)return false;
    ++sequence;errors=sample.errors;
    if(sample.breakPath){cancel();if(sample.kind!=Kind::Key)home=(sample.keys&(1u<<21))!=0;}
    if(sample.kind==Kind::Key) {
      const bool next=(sample.keys&(1u<<21))!=0;
      if(next!=home)++homeEdges;
      home=next;return true;
    }
    if(sample.kind==Kind::Cancel){cancel();return true;}
    if(!valid(sample.frame)){cancel();return false;}
    bool nextDown[2]={};
    for(unsigned i=0;i<sample.frame.count;++i) {
      const auto &c=sample.frame.contacts[i];const unsigned id=c.id;
      const int x=coordinate(c.x,Width),y=coordinate(c.y,Height);
      if(down[id])line(trail,coordinate(last[id].x,Width),coordinate(last[id].y,Height),x,y,Ink[id]);
      else{pixel(trail,x,y,Ink[id]);++downs;}
      nextDown[id]=true;last[id]=c;
    }
    for(unsigned id=0;id<2;++id){if(down[id] && !nextDown[id])++lifts;down[id]=nextDown[id];}
    current=sample.frame;ready=true;
    if(current.count>maxContacts)maxContacts=current.count;
    return true;
  }
  void render(unsigned heartbeat,unsigned drops) {
    for(unsigned i=0;i<Pixels;++i)output[i]=trail[i];
    box(output,64,2,192,65,Background);box(output,56,428,208,48,Background);
    text(output,100,6,"TOUCH TEST",White);
    text(output,76,26,ready?"ACTIVE":"WAIT",White);
    number(output,150,26,current.count,White);
    text(output,180,26,"ERR",errors?0xf800:White);number(output,220,26,errors,errors?0xf800:White);
    text(output,94,47,home?"HOME DOWN":"HOME UP",home?0xffe0:Target);
    for(unsigned id=0;id<2;++id) {
      const int y=432+18*int(id);
      number(output,64,y,id+1,Tip[id]);text(output,88,y,"X",Tip[id]);number(output,108,y,last[id].x,Tip[id]);
      text(output,160,y,"Y",Tip[id]);number(output,180,y,last[id].y,Tip[id]);
    }
    box(output,154,471,12,5,(heartbeat&1)?0x07e0:0x0320);
    if(drops){text(output,68,70,"QUEUE LOSS",0xf800);number(output,200,70,drops,0xf800);}
    for(unsigned id=0;id<2;++id)if(down[id]) {
      const int x=coordinate(last[id].x,Width),y=coordinate(last[id].y,Height);
      for(int dy=-10;dy<=10;++dy)for(int dx=-10;dx<=10;++dx) {
        const int distance=dx*dx+dy*dy;
        if(distance>=64 && distance<=100)pixel(output,x+dx,y+dy,Tip[id]);
      }
      line(output,x-5,y,x+5,y,White);line(output,x,y-5,x,y+5,White);
    }
  }
};
} // namespace E5Canvas
