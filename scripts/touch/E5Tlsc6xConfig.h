// SPDX-License-Identifier: BSD-2-Clause-Patent
// TLSC6x running configuration validity, matching the vendor protocol.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace E5Touch {
enum class ConfigResult { Good, Length, Address, Checksum };
inline ConfigResult validateConfiguration(const uint8_t *data,size_t bytes) {
  if(!data || bytes!=204)return ConfigResult::Length;
  if(((data[106]>>1)&0x7f)!=0x2e)return ConfigResult::Address;
  uint32_t sum=0;
  for(size_t i=0;i<bytes;i+=2)sum+=uint32_t(data[i])|(uint32_t(data[i+1])<<8);
  return (sum&0xffff)?ConfigResult::Checksum:ConfigResult::Good;
}
}
