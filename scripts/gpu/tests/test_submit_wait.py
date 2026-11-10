#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify actual combined-dispatch and KMD code with transport/IRQ mocks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

gpu = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mesa', type=Path, required=True, help='prepared E5 Mesa source checkout')
mesa = parser.parse_args().mesa.resolve()

def block(text, start):
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening:end]

kernel = (gpu / 'E5MaliG1.cpp').read_text()
branch = block(kernel, kernel.index('    case kSelSubmitWaitMany:'))
kh = r'''
#include <cassert>
#include <cstring>
#include "E5MaliG1Abi.h"
#include "E5MaliG1Ext.h"
using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1;
static_assert(sizeof(E5MaliSubmitWaitOut)==32);
struct Driver { int lock=0; int *stateLock(){return &lock;} };
void IOLockLock(int *p){assert(!*p);*p=1;}
void IOLockUnlock(int *p){assert(*p);*p=0;}
struct Arguments {unsigned structureInputSize,structureOutputSize;void *structureInput,*structureOutput;};
struct Client {
 Driver driver;Driver *d=&driver;unsigned submits=0,waits=0;int submitResult=0,waitResult=0;
 unsigned state=kAtomDone,hw=1;
 IOReturn submitHandles(uint64_t,unsigned,const uint64_t*,const uint32_t*,unsigned,E5MaliSubmitOut *out){
  assert(driver.lock==1);++submits;out->atomId=submitResult?0:77;out->status=submitResult;return submitResult;
 }
 IOReturn waitAtom(const E5MaliWaitAtom *in,E5MaliWaitAtomOut *out,bool wait){
  assert(!driver.lock && wait && in->atomId==77 && in->timeoutMs==500);++waits;
  out->state=state;out->hwStatus=hw;out->completionSeq=9;out->status=waitResult;return waitResult;
 }
 IOReturn dispatch(Arguments *args)
'''
kcases = r'''
};
int main(){
 E5MaliSubmitMany in{};in.handleCount=256;E5MaliSubmitWaitOut out{};
 Arguments a{sizeof(in),sizeof(out),&in,&out};Client c;
 assert(c.dispatch(&a)==0 && c.submits==1 && c.waits==1 && !c.driver.lock);
 assert(out.atomId==77 && out.state==kAtomDone && out.hwStatus==1 && out.completionSeq==9 && !out.status);
 a.structureInputSize=E5_MALI_LEGACY_SUBMIT_MANY_BYTES;in.handleCount=65;
 assert(c.dispatch(&a)==kIOReturnBadArgument && c.submits==1);
 in.handleCount=64;assert(c.dispatch(&a)==0);
 a.structureInputSize=sizeof(in)-1;assert(c.dispatch(&a)==kIOReturnBadArgument);
 a.structureInputSize=sizeof(in);a.structureOutputSize=31;assert(c.dispatch(&a)==kIOReturnBadArgument);
 a.structureOutputSize=sizeof(out);in.handleCount=0;assert(c.dispatch(&a)==kIOReturnBadArgument);
 in.handleCount=1;c.submitResult=8;unsigned waits=c.waits;
 assert(c.dispatch(&a)==8 && out.status==8 && !out.atomId && c.waits==waits && !c.driver.lock);
 c.submitResult=0;c.state=kAtomOnHw;c.hw=0;
 assert(c.dispatch(&a)==0 && out.state==kAtomOnHw && !out.hwStatus); // never manufacture Done on timeout
 c.waitResult=9;assert(c.dispatch(&a)==9 && out.status==9 && out.atomId==77);
}
'''
kmod = (mesa / 'src/panfrost/lib/kmod/e5_kmod.c').read_text()
start = kmod.index('uint64_t kbase_kmod_job_submit(')
km = kmod[start:kmod.index('\nbool kbase_kmod_wait_atom(', start)]
mh = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "E5MaliG1Abi.h"
#include "E5MaliG1Ext.h"
#define BASE_JD_REQ_FS 1
struct e5_dev;
struct pan_kmod_dev {struct e5_dev *d;};
struct pan_kmod_bo {struct pan_kmod_dev *dev;};
struct base_external_resource {int unused;};
struct e5_dev {bool lost,submit_wait;int lock;uint64_t completed,job_count;
 struct {unsigned maxSubmitBos,maxBoHandles;} hw;unsigned submit_bytes;void *live[256];};
struct e5_dev *edev(struct pan_kmod_dev *d){return d->d;}
void pthread_mutex_lock(int *p){assert(!*p);*p=1;}
void pthread_mutex_unlock(int *p){assert(*p);*p=0;}
unsigned selectors[4],calls;unsigned returned_state=kAtomDone,returned_hw=1;int failure=0;
int call(struct e5_dev *d,unsigned sel,const void *in,size_t n,void *out,size_t bytes){
 assert(d->lock && calls<4);selectors[calls++]=sel;
 if(failure)return failure;
 if(sel==kSelWaitAtom){
  const struct E5MaliWaitAtom *wait=in;assert(wait->atomId==44 && wait->timeoutMs==500);
  struct E5MaliWaitAtomOut *r=out;assert(bytes==sizeof(*r));r->state=returned_state;r->hwStatus=returned_hw;return 0;
 }
 const E5MaliSubmitMany *s=in;assert(n==sizeof(*s) && s->handleCount==2 && s->handles[0]==1 && s->handles[1]==4);
 if(sel==kSelSubmitWaitMany){E5MaliSubmitWaitOut *r=out;assert(bytes==sizeof(*r));r->atomId=44;r->state=returned_state;r->hwStatus=returned_hw;}
 else {assert(sel==kSelSubmitMany);struct E5MaliSubmitOut *r=out;assert(bytes==sizeof(*r));r->atomId=44;}
 return 0;
}
'''
mcases = r'''
int main(void){
 struct e5_dev d={.submit_wait=true,.hw={256,256},.submit_bytes=sizeof(E5MaliSubmitMany)};
 struct pan_kmod_dev dev={&d};struct pan_kmod_bo bo={&dev};struct pan_kmod_bo *bos[]={&bo};
 d.live[0]=&bo;d.live[3]=&bo;
 assert(kbase_kmod_job_submit(&dev,0x1234,1,bos,1,NULL,0)==44 && calls==1 && selectors[0]==16 && !d.lost && !d.lock && d.completed==44);
 calls=0;d.submit_wait=false;d.completed=0;
 assert(kbase_kmod_job_submit(&dev,0x1234,1,bos,1,NULL,0)==44 && calls==2 && selectors[0]==14 && selectors[1]==7 && !d.lock);
 calls=0;d.submit_wait=true;d.completed=0;returned_state=kAtomOnHw;returned_hw=0;
 assert(!kbase_kmod_job_submit(&dev,0x1234,1,bos,1,NULL,0) && d.lost && !d.completed && !d.lock && calls==1);
 calls=0;d.lost=false;returned_state=kAtomFault;returned_hw=0x58;
 assert(!kbase_kmod_job_submit(&dev,0x1234,1,bos,1,NULL,0) && d.lost && !d.completed && !d.lock);
 calls=0;d.lost=false;failure=7;returned_state=kAtomDone;returned_hw=1;
 assert(!kbase_kmod_job_submit(&dev,0x1234,1,bos,1,NULL,0) && d.lost && !d.completed && !d.lock && calls==1);
}
'''
with tempfile.TemporaryDirectory(prefix='e5-submit-wait-') as temporary:
    folder = Path(temporary)
    for name, text, compiler, language in [
        ('kernel', kh + branch + kcases, 'clang++', 'c++17'),
        ('kmod', mh + km + mcases, 'clang', 'gnu11')]:
        source = folder / (name + ('.cpp' if compiler == 'clang++' else '.c'))
        source.write_text(text)
        binary = folder / name
        subprocess.run([compiler, '-std=' + language, '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-I' + str(gpu), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('PASS one-call submit/IRQ wait, legacy fallback, packet bounds, cache-retirement ownership and failed/pending completion refusal')
