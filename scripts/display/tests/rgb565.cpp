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
}
