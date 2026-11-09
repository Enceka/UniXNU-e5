#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build E5 input against the hash-qualified original 21H461 IOHIDDevice ABI.

Opaque interface and deferred registration follow UniXNU's qualified
build-sheng-hid-input.py; no guessed private method prototype is called.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT=Path(sys.argv[1] if len(sys.argv)>1 else '.').resolve()
SOURCE=Path(__file__).resolve().parent
OUT=ROOT/'build/e5-input'
TEXT_HASH='7e4c5104814f2e773d5be13d092dec53ee44ba5a61a81264b229780406e86b72'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
spec=importlib.util.spec_from_file_location('input_derive',SOURCE/'e5_derive_audit.py')
derive=importlib.util.module_from_spec(spec);spec.loader.exec_module(derive)

def require(value,why):
    if not value:raise SystemExit('e5-input: '+why)

def run(command,**kwargs):
    result=subprocess.run([str(x) for x in command],capture_output=True,text=True,**kwargs)
    if result.returncode:
        sys.stderr.write((result.stdout+result.stderr)[-7000:]);raise SystemExit('e5-input: command failed')
    return result.stdout

def symbols(data,image):
    sym=next(p for c,p,_ in image['commands'] if c==2)
    start,count,strings,_=struct.unpack_from('<4I',data,sym+8)
    return [(data[strings+ix:data.index(b'\0',strings+ix)].decode(),value,kind)
            for ix,kind,section,desc,value in
            (struct.unpack_from('<IBBHQ',data,start+i*16) for i in range(count))]

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    kc=derive.load_kc(ROOT);image=macho(kc);slide=derive.SLIDE
    def read(address,size):
        offset=backed_offset(image['segments'],address-slide,size,len(kc));return kc[offset:offset+size]
    derive.MODULE_ID='com.apple.iokit.IOHIDFamily';load,_=derive.find_module(kc)
    family=macho(kc,backed_offset(image['segments'],load,32,len(kc)))
    text=next(s for s in family['segments'] if s['name']=='__TEXT_EXEC')
    require(hashlib.sha256(read(text['va']+slide,text['filesize'])).hexdigest()==TEXT_HASH,'original HID text changed')
    vptr=0xfffffff005d045c0
    table=struct.unpack('<207Q',read(vptr,207*8))
    require(all(table) and read(vptr+207*8,24)==bytes(24),'original HID vtable extent changed')
    native={name:address for address,names in exports(kc,image).items() for name in names}
    require([i for i,address in enumerate(table) if address==native['___cxa_pure_virtual']]==[180], 'HID abstract slot changed')
    require(read(0xfffffff004ad94f8,32)==bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),'protected HID constructor changed')
    require(table[181]+slide==0xfffffff004add0c4,'handleReport entry changed')
    require(read(0xfffffff004ad9aec,52)==bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'),'descriptor/start call ABI changed')
    header='''// Generated from the hash-qualified original IOHIDFamily 2031.120.4.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice:public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
    for i in range(168,207):
        if i==171:header+=' virtual OSString *newTransportString() const;\n'
        elif i==173:header+=' virtual OSString *newProductString() const;\n'
        elif i==180:header+=' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
        elif i==181:header+=' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
        else:header+=f' virtual __attribute__((deprecated("Unqualified prototype: never call"))) void opaque{i}();\n'
    header+='};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original IOHIDDevice extent");\n'
    (OUT/'OriginalHidDeviceABI.h').write_text(header)
    record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    def compile(name,path):
        command=[];args=record['arguments'];i=0
        while i<len(args):
            arg=args[i]
            if arg==record['file']:command.append(path)
            elif arg=='-o':command+=['-o',OUT/(name+'.o')];i+=1
            elif arg in ('-MF','-MJ'):i+=1
            elif arg=='-Xclang' and args[i+1].startswith('-W'):i+=1
            elif not arg.startswith('-W') and arg not in ('-MD','-MMD','-MP'):command.append(arg)
            i+=1
        command+=['-I',OUT,'-I',SOURCE/'input','-I',ROOT/'drivers/ArmCpu','-I',ROOT/'.cache/xnu-work/osfmk',
                  '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier',
                  '-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-Wno-incompatible-sysroot']
        (OUT/(name+'-command.json')).write_text(json.dumps([str(x) for x in command],indent=2)+'\n')
        run(command,cwd=record['directory']);return OUT/(name+'.o')
    owned=compile('E5HidDevice',SOURCE/'input/E5HidDevice.cpp')
    data=owned.read_bytes();obj=macho(data);syms=symbols(data,obj)
    vt=next(value for name,value,_ in syms if name=='__ZTV11E5HidDevice')
    relocations={}
    for segment in obj['segments']:
        for section in segment['sections']:
            if section['va']<=vt<section['va']+section['size']:
                offset,count=struct.unpack_from('<2I',data,section['command_offset']+56)
                for i in range(count):
                    site,bits=struct.unpack_from('<2I',data,offset+i*8)
                    if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[section['va']+site]=syms[bits&0xffffff][0]
    compiled=[relocations.get(vt+16+i*8) for i in range(207)]
    require(all(compiled) and vt+16+207*8 not in relocations,'compiled HID vtable is not exactly 207 slots')
    bindings={'__ZN11IOHIDDeviceC2EPK11OSMetaClass':0xfffffff004ad94f8,
              '__ZN11IOHIDDeviceD2Ev':0xfffffff004ad9518,
              '__ZN11IOHIDDevice10gMetaClassE':0xfffffff006ae6fa0,
              '__ZTV11IOHIDDevice':vptr-16}
    owned_slots=[]
    for i,name in enumerate(compiled):
        if name.startswith(('__ZN11E5HidDevice','__ZNK11E5HidDevice')):owned_slots.append(i)
        elif '11IOHIDDevice' in name:
            require(i!=180,'unimplemented abstract descriptor');bindings[name]=table[i]+slide
        else:require(i<168 and native.get(name)==table[i],'native inherited method mismatch '+str(i))
    require(owned_slots==[0,1,7,14,86,87,111,171,173,180],'unexpected owned HID overrides '+str(owned_slots))
    undefined={name for name,_,kind in syms if kind==1 and '11IOHIDDevice' in name}
    require(undefined<=bindings.keys(),'unqualified HID import')
    bindings={name:bindings[name] for name in undefined}
    assembly=OUT/'E5HidBindings.S';assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name,value in sorted(bindings.items())))
    run([record['arguments'][0],'--target=arm64-apple-ios17.0','-Werror','-c',assembly,'-o',OUT/'E5HidBindings.o'],cwd=record['directory'])
    objcopy=shutil.which('llvm-objcopy');require(objcopy,'llvm-objcopy unavailable')
    late=OUT/'E5HidDevice.late.o';run([objcopy,'--remove-section=__DATA,__mod_init_func',owned,late])
    display_spec=importlib.util.spec_from_file_location('input_globalize',SOURCE/'e5_display_build.py')
    helper=importlib.util.module_from_spec(display_spec);display_spec.loader.exec_module(helper)
    helper.globalize(late,'__GLOBAL__sub_I_E5HidDevice.cpp')
    later=macho(late.read_bytes());require(not any(s['name']=='__mod_init_func' for g in later['segments'] for s in g['sections']),'automatic registration not removed')
    def code(blob,image):
        section=next(s for g in image['segments'] for s in g['sections'] if s['name']=='__text')
        return blob[section['fileoff']:section['fileoff']+section['size']]
    require(code(data,obj)==code(late.read_bytes(),later),'deferred staging altered owned code')
    touch=compile('E5Tlsc6x',SOURCE/'touch/E5Tlsc6x.cpp')
    capture=compile('E5InputCapture',SOURCE/'input/E5InputCapture.cpp')
    canvas=compile('E5TouchCanvas',SOURCE/'input/E5TouchCanvas.cpp')
    for candidate in (canvas,touch):
        candidate_image=macho(candidate.read_bytes())
        require(not any(s['name']=='__mod_init_func' for g in candidate_image['segments'] for s in g['sections']),
                candidate.name+' introduced an automatic C++ initializer')
    archive=ROOT/'build/e5-emmc/libE5Emmc.a';require(archive.is_file(),'storage archive must be built first')
    run(['ar','rcs',archive,late,OUT/'E5HidBindings.o',touch,capture,canvas])
    report=dict(original_kernelcache_sha256=hashlib.sha256(kc).hexdigest(),original_hid_text_sha256=TEXT_HASH,
                slots=207,original_instance_bytes=208,owned_slots=owned_slots,
                bindings={name:hex(value) for name,value in bindings.items()},
                automatic_initializer_removed=True,owned_code_unchanged=True,
                source_sha256={str(p.relative_to(SOURCE)):hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in [SOURCE/'input/E5HidDevice.cpp',SOURCE/'input/E5InputProtocol.h',SOURCE/'input/E5InputCapture.cpp',SOURCE/'input/E5TouchCanvas.cpp',SOURCE/'input/E5TouchCanvas.h',SOURCE/'touch/E5Tlsc6x.cpp',SOURCE/'touch/E5I2cProtocol.h',SOURCE/'touch/E5Tlsc6xConfig.h',SOURCE/'touch/E5GpioEvents.h',SOURCE/'touch/E5GpioIrq.h',Path(__file__)]},
                runtime_qualified=False)
    (OUT/'abi-audit.json').write_text(json.dumps(report,indent=2)+'\n')
    print('e5-input: original 207-slot HID interface and deferred E5 archive verified')

if __name__=='__main__':main()
