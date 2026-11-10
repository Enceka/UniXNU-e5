// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstring>
#include <cstdint>
#include "../E5FramebufferCopy.h"
int main() {
  // Two independently padded source/destination rows; red, green, blue,
  // white, black and a premultiplied half-red pixel over a black background.
  const uint8_t src[32] = {0,0,255,255, 0,255,0,255, 255,0,0,255, 9,9,9,9,
                          255,255,255,255, 0,0,0,255, 0,0,128,128, 9,9,9,9};
  uint16_t dest[12]; for(auto &x:dest)x=0xa55a;
  assert(e5_copy_bgra_rgb565(dest+2,16,8,src,sizeof(src),16,3,2));
  const uint16_t expected[12]={0xa55a,0xa55a,0xf800,0x07e0,0x001f,0xa55a,
                              0xffff,0,0x8000,0xa55a,0xa55a,0xa55a};
  assert(!memcmp(dest,expected,sizeof(dest)));
  assert(!e5_copy_bgra_rgb565(dest+2,15,8,src,sizeof(src),16,3,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,8,src,31,16,3,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,8,src,sizeof(src),11,3,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,7,src,sizeof(src),16,3,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,8,src,sizeof(src),UINT64_MAX,3,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,8,src,sizeof(src),16,0,2));
  assert(!e5_copy_bgra_rgb565(dest+2,16,8,src,sizeof(src),16,3,0));
  assert(!e5_copy_bgra_rgb565((char*)dest+1,16,8,src,sizeof(src),16,3,2));
  assert(!memcmp(dest,expected,sizeof(dest)));
  assert(e5_blank_rgb565(dest+2,16,8,3,2));
  assert(dest[0]==0xa55a && dest[1]==0xa55a && dest[5]==0xa55a && dest[9]==0xa55a && dest[10]==0xa55a);
  assert(dest[2]==0 && dest[3]==0 && dest[4]==0 && dest[6]==0 && dest[7]==0 && dest[8]==0);
  E5PixelSummary summary{};
  assert(e5_summarize_bgra(src,sizeof(src),16,3,2,summary));
  assert(summary.pixels==6 && summary.rgb==5 && summary.alpha==6 && summary.opaque==5);
  const uint8_t zeroAlpha[8]={255,255,255,0,0,0,0,0};
  assert(e5_summarize_bgra(zeroAlpha,8,8,2,1,summary));
  assert(summary.rgb==1 && summary.alpha==0 && summary.rgbWithZeroAlpha==1);
  assert(e5_copy_bgra_rgb565(dest,4,4,zeroAlpha,8,8,2,1) && dest[0]==0xffff && dest[1]==0);
  const uint32_t hash=summary.hash;
  assert(!e5_summarize_bgra(zeroAlpha,7,8,2,1,summary) && summary.hash==hash);
  // The fused read/statistics path must match the independent byte scanner,
  // including unaligned input and independently padded destination rows.
  E5PixelSummary reference{};
  assert(e5_summarize_bgra(src,sizeof(src),16,3,2,reference));
  alignas(4) uint8_t shifted[40];
  for(unsigned offset=0;offset<4;++offset) {
    memset(shifted,0xcc,sizeof(shifted));memcpy(shifted+offset,src,sizeof(src));
    for(auto &x:dest)x=0xa55a;
    E5PixelSummary fused{};
    assert(e5_copy_bgra_rgb565(dest+2,16,8,shifted+offset,sizeof(src),16,3,2,&fused));
    assert(!memcmp(dest,expected,sizeof(dest)));
    assert(fused.pixels==reference.pixels && fused.rgb==reference.rgb &&
           fused.alpha==reference.alpha && fused.opaque==reference.opaque &&
           fused.rgbWithZeroAlpha==reference.rgbWithZeroAlpha && fused.hash==reference.hash);
    const uint32_t saved=fused.hash;
    assert(!e5_copy_bgra_rgb565(dest+2,16,8,shifted+offset,31,16,3,2,&fused));
    assert(fused.hash==saved && !memcmp(dest,expected,sizeof(dest)));
  }
  const uint8_t oddRows[15]={0,0,255,255,99,0,255,0,255,99,255,0,0,255,99};
  assert(e5_copy_bgra_rgb565(dest,6,2,oddRows,15,5,1,3));
  assert(dest[0]==0xf800 && dest[1]==0x07e0 && dest[2]==0x001f);
  // Halfword-aligned destinations and rows alternating their word alignment
  // must keep exact pixels and leave every prefix/suffix sentinel untouched.
  alignas(4) uint16_t halfAligned[12];
  for(auto &x:halfAligned)x=0xa55a;
  assert(e5_copy_bgra_rgb565(halfAligned+1,12,6,src,sizeof(src),16,3,2));
  const uint16_t halfExpected[12]={0xa55a,0xf800,0x07e0,0x001f,0xffff,0,0x8000,
                                  0xa55a,0xa55a,0xa55a,0xa55a,0xa55a};
  assert(!memcmp(halfAligned,halfExpected,sizeof(halfAligned)));

  // Independent XR10 codes: primaries, clipped negative/over-white, and
  // half red with alpha 128 (premultiplied color must not be scaled twice).
  uint8_t xr[64]; memset(xr, 0xcc, sizeof(xr));
  const uint32_t codes[6][3] = {{894,384,384}, {384,894,384}, {384,384,894},
                               {1023,1023,1023}, {0,384,383}, {640,384,384}};
  const uint8_t alphas[] = {255,255,0,255,255,128};
  for (unsigned i=0; i<6; ++i) {
    uint32_t word=0xc0000000u | (codes[i][0]<<20) | (codes[i][1]<<10) | codes[i][2];
    memcpy(xr+4+(i/3)*16+(i%3)*4,&word,4);
    xr[44+(i/3)*8+i%3]=alphas[i];
  }
  E5B3a8Layout layout{};
  assert(e5_b3a8_layout(sizeof(xr),3,2,4,16,44,8,layout));
  for(auto &x:dest)x=0xa55a;
  assert(e5_copy_b3a8_rgb565(dest+2,16,8,xr,sizeof(xr),layout,3,2,&summary));
  assert(!memcmp(dest,expected,sizeof(dest)));
  assert(summary.pixels==6 && summary.rgb==5 && summary.opaque==4 &&
         summary.alpha==5 && summary.rgbWithZeroAlpha==1);
  const E5B3a8Layout valid=layout;
  assert(!e5_b3a8_layout(59,3,2,4,16,44,8,layout));
  assert(!e5_b3a8_layout(64,3,2,4,16,20,8,layout)); // overlapping planes
  assert(!e5_b3a8_layout(64,3,2,4,11,44,8,layout));
  assert(!e5_b3a8_layout(64,3,2,4,16,44,0,layout)); // one omitted pitch
  assert(!e5_b3a8_layout(64,3,2,UINT64_MAX,16,44,8,layout));
  assert(!e5_b3a8_layout(UINT64_MAX,3,2,0,UINT64_MAX,44,8,layout));
  assert(!e5_copy_b3a8_rgb565(dest+2,15,8,xr,64,valid,3,2));
  assert(!e5_copy_b3a8_rgb565(dest+2,16,8,xr,59,valid,3,2));
  assert(!e5_copy_b3a8_rgb565(dest+2,16,8,(void *)(UINTPTR_MAX-16),64,valid,3,2));
  assert(!memcmp(dest,expected,sizeof(dest)));
  assert(e5_b3a8_layout(770048,320,480,0,0,0,0,layout));
  assert(layout.colorOffset==0 && layout.colorStride==1280 &&
         layout.alphaOffset==614400 && layout.alphaStride==320);
  assert(!e5_b3a8_layout(767999,320,480,0,0,0,0,layout));
  for(unsigned c=0;c<1024;++c) {
    const unsigned want=c<384?0:c>894?255:(c-384+1)/2;
    assert(e5_xr10_u8(c)==want);
  }
}
