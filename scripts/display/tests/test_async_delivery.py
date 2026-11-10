#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual notification action against the pinned XNU port contract."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'E5Display.cpp')
parser.add_argument('--expect-old-failure', action='store_true')
args = parser.parse_args()
source = args.source.read_text()
start = source.index('static IOReturn e5_display_async_action(')
end = source.index('\n// v28:', start)
action = source[start:end]
start = source.index('  IOReturn opaque213(')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
capability = 'struct Capability { IOReturn query(uint32_t index,uint32_t *count,char *key,uint32_t *transport)' + source[opening:end] + '};\n'
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
struct OSObject {};
using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=-2,kIOReturnUnsupported=-1;
unsigned e5_display_cap213_calls=0,e5_display_cap213_status=0;
unsigned frameInfoControl=1;
bool PE_parse_boot_argn(const char *key,void *out,unsigned bytes){assert(!strcmp(key,"e5-frameinfo") && bytes==sizeof(unsigned));*static_cast<unsigned*>(out)=frameInfoControl;return true;}
unsigned e5_display_async_kinds=8,e5_display_async_sends=0,e5_display_async_send_ok=0;
unsigned e5_display_async_ports=0,e5_display_async_last=0;
void *sAsyncNode=nullptr;
struct Message { unsigned long long port; std::vector<unsigned long long> values; };
std::vector<Message> messages;
unsigned attempts=0;
unsigned long long mach_absolute_time(){return 0x12345678abcdef01ULL;}
void e5_cpu_log(const char *,...){}
void e5_state_log(const char *,...){}
// The pinned XNU function returns Success for a null port without a send.
int _ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj(
 unsigned long long *ref,int,unsigned long long *values,unsigned n,unsigned){
 ++attempts;
 if(ref[0]&~3ULL)messages.push_back({ref[0]&~3ULL,{values,values+n}});
 return 0;
}
void e5_display_family_broadcast(void *base,unsigned kind,unsigned long long value){
 auto node=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(base)+0x1588+kind*16);
 if(node){auto ref=reinterpret_cast<unsigned long long*>(node+0x18);
 _ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj(ref,0,&value,1,1);}
}
'''
cases = r'''
struct Node { uint8_t data[0x78]{}; };
int main(){
 Capability cap;uint32_t count=99,transport=99;char key[64];memset(key,1,sizeof(key));
 assert(cap.query(0,&count,key,&transport)==0 && count==1 && transport==0);
 for(char c:key)assert(!c);
 frameInfoControl=0;count=transport=99;memset(key,1,sizeof(key));
 assert(cap.query(0,&count,key,&transport)==kIOReturnUnsupported && count==0 && transport==0);
 for(char c:key)assert(!c);
 assert(cap.query(0,nullptr,key,&transport)==kIOReturnBadArgument);
 alignas(8) uint8_t service[0x1650]{};
 auto frameHead=reinterpret_cast<uint8_t**>(service+0x1588+3*16);
 auto vsyncHead=reinterpret_cast<uint8_t**>(service+0x1588+5*16);
 auto first=new Node;
 *frameHead=first->data;sAsyncNode=first->data;
 // A native node's null next link is unrelated to its initialized async port.
 auto ref=reinterpret_cast<unsigned long long*>(first->data+0x18);
 ref[0]=0x1001;
 e5_display_async_action(reinterpret_cast<OSObject*>(service),nullptr,nullptr,nullptr,nullptr);
 assert(messages.size()==1 && messages[0].port==0x1000);
 assert(messages[0].values.size()==2 && messages[0].values[0]==0 && messages[0].values[1]==0);
 assert(e5_display_async_ports==8 && e5_display_async_send_ok==1);
 // Native unregistration removes and frees the node. A remembered node is
 // deliberately stale: an actual live-list traversal must not touch it.
 *frameHead=nullptr;delete first;
 messages.clear();attempts=0;
 e5_display_async_action(reinterpret_cast<OSObject*>(service),nullptr,nullptr,nullptr,nullptr);
 assert(messages.empty() && attempts==0 && e5_display_async_ports==0);
 Node absent;
 *frameHead=absent.data;
 reinterpret_cast<unsigned long long*>(absent.data+0x18)[0]=1; // flag without a port
 e5_display_async_action(reinterpret_cast<OSObject*>(service),nullptr,nullptr,nullptr,nullptr);
 assert(messages.empty() && attempts==0 && e5_display_async_ports==0);
 Node a,b,v;
 *frameHead=a.data;*reinterpret_cast<uint8_t**>(a.data)=b.data;
 reinterpret_cast<unsigned long long*>(a.data+0x18)[0]=0x2001;
 reinterpret_cast<unsigned long long*>(b.data+0x18)[0]=0x3001;
 *vsyncHead=v.data;reinterpret_cast<unsigned long long*>(v.data+0x18)[0]=0x4001;
 e5_display_async_action(reinterpret_cast<OSObject*>(service),nullptr,nullptr,nullptr,nullptr);
 assert(messages.size()==3 && attempts==3 && e5_display_async_ports==(8|32));
 assert(messages[0].port==0x2000 && messages[1].port==0x3000 && messages[2].port==0x4000);
 const std::vector<unsigned long long> expected={0xabcdef01,0x12345678,0xabcdef01,0x12345678,0,0};
 assert(messages[2].values==expected);
}
'''
with tempfile.TemporaryDirectory(prefix='e5-async-test-') as temporary:
    directory = Path(temporary)
    cpp = directory / 'delivery.cpp'
    cpp.write_text(harness + action + capability + cases)
    executable = directory / 'delivery'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    if args.expect_old_failure:
        assert result.returncode != 0, 'old sender unexpectedly passes the native port/payload contract'
        print('PASS old sender fails the native reference/payload test')
    else:
        if result.returncode:
            raise SystemExit(result.stderr)
        print('PASS capability output/control, native ports/payloads, multiple registrations, null-port refusal and unregister lifetime')
