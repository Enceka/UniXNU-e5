// SPDX-License-Identifier: BSD-2-Clause-Patent
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FAIL line%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 REQUIRE(E5UserCache::start());
 auto page=[](uint64_t pa,unsigned mode,bool valid=true){modes[pa>>12]=mode;managed[pa>>12]=valid;return IOMemoryDescriptor{{pa}};};
 uint32_t n=0,m=0;
 auto wb=page(0x90000000,2);REQUIRE(E5UserCache::acquire(&wb,4096,&n));REQUIRE(E5UserCache::acquire(&wb,4096,&m));
 REQUIRE(modes[0x90000]==2 && setters==0 && E5UserCache::activePages()==1);
 REQUIRE(E5UserCache::syncUserRange(&wb,0,4096,1));REQUIRE(clean_ops==64);
 REQUIRE(E5UserCache::syncUserRange(&wb,0,4096,3));REQUIRE(flush_ops==64 && invalidate_ops==0);
 REQUIRE(E5UserCache::syncUserRange(&wb,64,128,2));REQUIRE(invalidate_ops==2);
 E5UserCache::release(&wb,n);REQUIRE(E5UserCache::activePages()==1);E5UserCache::release(&wb,m);REQUIRE(E5UserCache::activePages()==0 && modes[0x90000]==2 && setters==0);
 for(unsigned mode:{0x12u,0xbu,0x6u,0x17u}){
  auto p=page(0x90001000,mode);REQUIRE(E5UserCache::acquire(&p,4096,&n));REQUIRE(modes[0x90001]==mode && setters==0);E5UserCache::release(&p,n);
 }
 auto rt=page(0x90001000,0x17);REQUIRE(E5UserCache::syncUserRange(&rt,0,4096,3));REQUIRE(flush_ops==64);
 auto bad=page(0x90002000,7);auto unmanaged=page(0x90003000,2,false);
 REQUIRE(!E5UserCache::acquire(&bad,4096,&n) && n==0);REQUIRE(!E5UserCache::acquire(&unmanaged,4096,&n) && n==0);
 REQUIRE(!E5UserCache::syncUserRange(&bad,0,4096,0));REQUIRE(!E5UserCache::syncUserRange(&unmanaged,0,4096,0));
 IOMemoryDescriptor mixed{{0x90000000,0x90002000}};REQUIRE(!E5UserCache::acquire(&mixed,8192,&n) && n==0);REQUIRE(E5UserCache::activePages()==0 && setters==0 && modes[0x90000]==2);

 // Partial ranges must cover both boundary cache lines, including a second
 // physical page. The mock records the line selected by each DC operation.
 auto extraPage=page(0x90004000,2);(void)extraPage;
 IOMemoryDescriptor pair{{0x90000000,0x90004000}};
 for(unsigned op:{1u,2u,3u}) {
  cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&wb,1,64,op));
  REQUIRE((cache_lines==std::vector<uintptr_t>{0x190000000ull,0x190000040ull}));
  cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&wb,63,2,op));
  REQUIRE((cache_lines==std::vector<uintptr_t>{0x190000000ull,0x190000040ull}));
  cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&pair,4090,100,op));
  REQUIRE((cache_lines==std::vector<uintptr_t>{0x190000fc0ull,0x190004000ull,0x190004040ull}));
 }
 // Physical-segment reuse reduces descriptor queries, while preserving
 // every page-policy check and all cache-line operations across the run.
 IOMemoryDescriptor run;run.coalesced=true;
 for(unsigned i=0;i<32;++i){uint64_t pa=0x91000000ull+i*4096;run.physical.push_back(pa);modes[pa>>12]=2;managed[pa>>12]=true;}
 cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&run,0,32*4096,1));
 REQUIRE(run.segmentCalls==1 && cache_lines.size()==32*64);
 REQUIRE(cache_lines.front()==0x191000000ull && cache_lines.back()==0x19101ffc0ull);
 run.segmentCalls=0;cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&run,4090,100,3));
 REQUIRE(run.segmentCalls==1 && (cache_lines==std::vector<uintptr_t>{0x191000fc0ull,0x191001000ull,0x191001040ull}));
 // Unknown or unmanaged pages in the middle must still be rejected.
 modes[0x91010]=7;REQUIRE(!E5UserCache::syncUserRange(&run,0,32*4096,0));modes[0x91010]=2;
 managed[0x91010]=false;REQUIRE(!E5UserCache::syncUserRange(&run,0,32*4096,0));managed[0x91010]=true;
 // Requery a discontiguous segment rather than extrapolating its address.
 run.physical[16]=0x92000000;auto disjoint=page(0x92000000,2);(void)disjoint;
 run.segmentCalls=0;cache_lines.clear();REQUIRE(E5UserCache::syncUserRange(&run,15*4096,3*4096,3));
 REQUIRE(run.segmentCalls==3 && cache_lines.size()==3*64);
 REQUIRE(cache_lines[64]==0x192000000ull && cache_lines[128]==0x191011000ull);
 E5UserCache::finish();puts("PASS: CPU cache attributes/aliases/line boundaries preserved; contiguous segments queried once, fragmented/unknown/unmanaged pages remain checked");
}
