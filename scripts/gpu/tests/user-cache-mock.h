// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <vector>
using ppnum_t=uint32_t;using IOByteCount=uint64_t;
constexpr unsigned kIOMemoryMapperNone=1;
struct IOLock {};
IOLock *IOLockAlloc(){return new IOLock;}
void IOLockFree(IOLock *p){delete p;}
void IOLockLock(IOLock*){} void IOLockUnlock(IOLock*){}
void *IOMalloc(size_t n){return malloc(n);} void IOFree(void*p,size_t){free(p);}
void note(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);puts("");}
std::map<ppnum_t,unsigned> modes;std::map<ppnum_t,bool> managed;unsigned setters=0;
unsigned pmap_cache_attributes(ppnum_t p){return modes[p];}
void pmap_set_cache_attributes(ppnum_t p,unsigned mode){++setters;modes[p]=mode;}
int pmap_valid_page(ppnum_t p){return managed[p];}
struct IOMemoryDescriptor {
 std::vector<uint64_t> physical;
 bool coalesced=false;
 unsigned segmentCalls=0;
 uint64_t getPhysicalSegment(uint64_t off,IOByteCount *n,unsigned options=0){
  ++segmentCalls;
  if(off/4096>=physical.size()){*n=0;return 0;}
  *n=4096;
  if(coalesced)for(size_t i=off/4096+1;i<physical.size() && physical[i]==physical[i-1]+4096;++i)*n+=4096;
  return physical[off/4096]+(options==kIOMemoryMapperNone?0:0x100000000ull);
 }
};

using vm_offset_t=uintptr_t;vm_offset_t ml_static_ptovirt(vm_offset_t p){return p+0x100000000ull;}
std::vector<uintptr_t> cache_lines;
unsigned clean_ops=0,invalidate_ops=0,flush_ops=0;void test_cache_op(uintptr_t at,unsigned op){cache_lines.push_back(at & ~uintptr_t(63));if(op==1)clean_ops++;else if(op==2)invalidate_ops++;else flush_ops++;}
