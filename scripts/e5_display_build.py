#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors.
"""Audit and build the E5 concrete Legacy construction probe.

The opaque inheritance/late-init technique and globalize helper are adapted
from Ken-u/UniXNU-evb10 e03f9880ae8fc420fa337366a444c8888b0e3fdc,
scripts/build-native-legacy-display.py, itself derived from UniXNU-sheng.
See display/LICENSE. No saved runtime observation is substituted for E5 proof.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

FAMILY_HASH = '2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b'
PURE = [211, 213, 245, 246, 306, 307, 337, 338, 345]
OWNED = sorted([0, 1, 7, 14, 84, 86, 87, 91, 141, 176, 180, 183, 184, 188, 199, 200, 201, 203, 206, 207, 208, 210, 224, 229, 232, 255, 256, 258, 288, 290, 296, 344] + PURE)
ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
SOURCE = Path(__file__).resolve().parent / 'display'
OUT = ROOT / 'build/e5-display'


def require(value, message):
    if not value:
        raise SystemExit('e5-display: ' + message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stderr.write((result.stdout + result.stderr)[-6000:])
        raise SystemExit('e5-display: command failed: ' + str(args[0]))
    return result.stdout


sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports
spec = importlib.util.spec_from_file_location('e5_derive', Path(__file__).with_name('e5_derive_audit.py'))
derive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(derive)


def read_symbols(data, image):
    at = next(p for c, p, _ in image['commands'] if c == 2)
    start, count, strings, _ = struct.unpack_from('<4I', data, at + 8)
    result = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, start + 16 * i)
        name = data[strings + ix:data.index(b'\0', strings + ix)].decode()
        result.append((name, va, kind))
    return result


def globalize(path, name):
    # Mach-O --globalize-symbol is unsupported by llvm-objcopy. Keep the local
    # and external symbol ranges contiguous and remap every affected index.
    data = bytearray(path.read_bytes())
    image = macho(bytes(data))
    sym = next(p for c, p, _ in image['commands'] if c == 2)
    dys = next(p for c, p, _ in image['commands'] if c == 0xb)
    start, count, strings, _ = struct.unpack_from('<4I', data, sym + 8)
    local, nlocal, external, nexternal = struct.unpack_from('<4I', data, dys + 8)
    found = [i for i, (n, _, _) in enumerate(read_symbols(bytes(data), image)) if n == name]
    require(len(found) == 1, 'owned initializer symbol not unique')
    index = found[0]
    require(data[start + index * 16 + 4] & 0xef == 0x0e, 'initializer is not a local N_SECT')
    require(local <= index < local + nlocal and external == local + nlocal, 'unexpected symbol ranges')
    last = local + nlocal - 1
    if index != last:
        a, b = start + index * 16, start + last * 16
        olda, oldb = bytes(data[a:a + 16]), bytes(data[b:b + 16])
        data[a:a + 16], data[b:b + 16] = oldb, olda
        remap = {index: last, last: index}
        for segment in image['segments']:
            for section in segment['sections']:
                off, length = struct.unpack_from('<2I', data, section['command_offset'] + 56)
                for i in range(length):
                    at = off + i * 8
                    address, bits = struct.unpack_from('<2I', data, at)
                    require(not address >> 31, 'unexpected scattered relocation')
                    if bits >> 27 & 1 and bits & 0xffffff in remap:
                        struct.pack_into('<I', data, at + 4, (bits & ~0xffffff) | remap[bits & 0xffffff])
        off, length = struct.unpack_from('<2I', data, dys + 56)
        for i in range(length):
            value, = struct.unpack_from('<I', data, off + i * 4)
            if value in remap:
                struct.pack_into('<I', data, off + i * 4, remap[value])
    data[start + last * 16 + 4] |= 1
    struct.pack_into('<4I', data, dys + 8, local, nlocal - 1, last, nexternal + 1)
    path.write_bytes(data)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    kc = derive.load_kc(ROOT)  # Exact original-kernelcache SHA pin; never a synthetic runtime seed.
    original = macho(kc)

    def read(va, size):
        off = backed_offset(original['segments'], va, size, len(kc))
        return kc[off:off + size]

    derive.MODULE_ID = 'com.apple.iokit.IOMobileGraphicsFamily'
    load, _ = derive.find_module(kc)
    module = macho(kc, backed_offset(original['segments'], load, 32, len(kc)))
    text = next(s for s in module['segments'] if s['name'] == '__TEXT_EXEC')
    require(digest(read(text['va'], text['filesize'])) == FAMILY_HASH, 'original family text hash mismatch')
    slide = derive.SLIDE
    vptr = 0xfffffff005f38570
    table = struct.unpack('<358Q', read(vptr - slide, 358 * 8))
    require(all(table) and read(vptr - slide + 358 * 8, 24) == bytes(24), 'original vtable extent changed')
    names = exports(kc, original)
    pure = [i for i, va in enumerate(table) if '___cxa_pure_virtual' in names.get(va, [])]
    require(pure == PURE, 'original pure virtual slots changed')
    require(table[141] + slide == 0xfffffff005789080,
            'original newUserClient slot changed')
    require(table[188] + slide == 0xfffffff005798184 and
            read(0xfffffff0057981d4 - slide, 8) == bytes.fromhex('00214bf9080040f9') and
            read(0xfffffff0057982a0 - slide, 16) == bytes.fromhex('e81740b9a9035ff8280100b9a0c35eb8'),
            'original slot-188 uint32 output / IOReturn ABI changed')
    require(table[86] + slide == 0xfffffff0057872e8 and
            read(0xfffffff005787ef8 - slide, 16) == bytes.fromhex('080040f9088144f900013fd6e0000037'),
            'original bool startHardware() call changed')
    require(read(0xfffffff005777e9c - slide, 16) == bytes.fromhex('080040f908e142f900013fd6c0000036'),
            'original bool isMainDisplay() call changed')
    getter = table[249] + slide
    require(getter == 0xfffffff00579fc8c and
            struct.unpack('<I', read(getter + 0x30 - slide, 4))[0] == 0xb956b108 and
            struct.unpack('<I', read(getter + 0x58 - slide, 4))[0] == 0xb956b508,
            'original uint32 width/height fields changed')
    for pc, target in [(0xfffffff0057865fc, 0xfffffff00577b510),
                       (0xfffffff00577b540, 0xfffffff00577b318),
                       (0xfffffff005786630, 0xfffffff00577b55c)]:
        word, = struct.unpack('<I', read(pc - slide, 4))
        require(word & 0xfc000000 == 0x94000000 and
                derive.branch_target(pc - slide, word) + slide == target, 'original C2/D2 chain changed')

    require(table[295] + slide == 0xfffffff005795b88 and
            read(0xfffffff005795b98 - slide, 16) == bytes.fromhex('e00740f9280080520101001272d3ff97'),
            'original normal current-swap completion changed')
    require(read(0xfffffff00578aa7c - slide, 24) == bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
            'original optional completion-report call changed')
    require(read(0xfffffff005790ec8 - slide, 16) == bytes.fromhex('08a144f900013fd6280080520800080a'),
            'original request-validation bool dispatch changed')
    require(read(0xfffffff005787d84 - slide, 4) == bytes.fromhex('3fb50af9') and
            read(0xfffffff005787d90 - slide, 4) == bytes.fromhex('28b90af9') and
            read(0xfffffff005786c5c - slide, 4) == bytes.fromhex('08c945f9'),
            'original pending queue head/tail/next fields changed')
    require(read(0xfffffff004ba2908 - slide, 4) == bytes.fromhex('c84e4139') and
            read(0xfffffff004ba2910 - slide, 4) == bytes.fromhex('c84e40b9'),
            'original IOSurface read-fence active/completed fields changed')
    surface_bindings = {}
    for name, address, field, wide in [
        ('descriptor', 0xfffffff004b9ead0, 48, True),
        ('width', 0xfffffff004b9ebbc, 88, True), ('height', 0xfffffff004b9ebc4, 96, True),
        ('stride', 0xfffffff004b9ebd4, 104, True), ('allocation', 0xfffffff004b9ebfc, 144, True),
        ('format', 0xfffffff004b9ebac, 128, False), ('planes', 0xfffffff004b9ebf4, 152, False)]:
        expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
        require(read(address - slide, 8) == struct.pack('<2I', expected, 0xd65f03c0),
                'original IOSurface getter changed: ' + name)
        surface_bindings['_e5_surface_' + name] = address

    header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generated opaque inheritance layout for the hash-qualified 21H461 family.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
  OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
  unsigned char originalPrivate[0x2110 - sizeof(IOService)];
public:
  void installBootDimensions(uint32_t width, uint32_t height) {
    auto bytes = reinterpret_cast<unsigned char *>(this);
    memcpy(bytes + 0x16b0, &width, sizeof(width));
    memcpy(bytes + 0x16b4, &height, sizeof(height));
  }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
  unsigned lifecycleResourceMask() const {
    const unsigned offsets[] = {0x1608, 0x1610, 0x1638, 0x1648, 0x17f8, 0x1868};
    auto bytes = reinterpret_cast<const unsigned char *>(this);
    unsigned mask = 0;
    for (unsigned i = 0; i != 6; ++i) {
      const void *resource = nullptr;
      memcpy(&resource, bytes + offsets[i], sizeof(resource));
      if (resource) mask |= 1U << i;
    }
    return mask;
  }
  void free() override;
  bool init(OSDictionary *properties=nullptr) override;
  IOReturn setProperties(OSObject *) override;
  void systemWillShutdown(IOOptionBits) override;
  IOService *probe(IOService *, SInt32 *) override;
  bool start(IOService *) override;
  void stop(IOService *) override;
  IOWorkLoop *getWorkLoop() const override;
  IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) override;
  IOReturn setPowerState(unsigned long, IOService *) override;
'''
    for i in range(168, 358):
        if i == 184:
            header += '  virtual bool isMainDisplay();\n'
        elif i == 188:
            header += '  virtual IOReturn getDotPitch(uint32_t *);\n'
        elif i == 176:
            # The wrapper passes client in x1, then swap id, flags, timeout.
            header += '  virtual IOReturn opaque176(void *, uint32_t, uint32_t, uint32_t);\n'
        elif i == 200:
            header += '  virtual IOReturn opaque200(void *);\n'
        elif i == 203:
            header += '  virtual IOReturn opaque203(void *);\n'
        elif i == 201:
            header += '  virtual IOReturn opaque201(uint32_t, void *);\n'
        elif i == 224:
            header += '  virtual IOReturn opaque224(void *, uint32_t, uint64_t, void *, uint32_t, void *, uint32_t);\n'
        elif i == 180:
            header += '  virtual IOReturn opaque180(uint32_t, uint32_t, void *);\n'
        elif i == 183:
            header += '  virtual IOReturn opaque183(uint32_t);\n'
        elif i == 199:
            header += '  virtual IOReturn opaque199(void *);\n'
        elif i == 206:
            header += '  virtual IOReturn opaque206(void *);\n'
        elif i == 207:
            header += '  virtual IOReturn opaque207(uint32_t);\n'
        elif i == 208:
            header += '  virtual IOReturn opaque208(uint32_t, void *, void *);\n'
        elif i == 210:
            header += '  virtual IOReturn opaque210(uint32_t, void *, uint32_t);\n'
        elif i == 211:
            header += '  virtual IOReturn opaque211(uint32_t, void *, uint32_t) = 0;\n'
        elif i == 213:
            header += '  virtual IOReturn opaque213(uint32_t, uint32_t *, char *, uint32_t *) = 0;\n'
        elif i == 232:
            header += '  virtual IOReturn opaque232(uint32_t);\n'
        elif i == 229:
            # request_power_change backend (dispatcher 0x67779fc tail calls it
            # with (uint32_t power_state, void *zero)); the legacy stub only
            # forwards to the +0x1608 sub-object. Client-visible IOReturn.
            header += '  virtual IOReturn opaque229(uint32_t, void *);\n'
        elif i == 255:
            # get power state; dispatcher tests bit0 of the return (1 = on).
            header += '  virtual IOReturn opaque255();\n'
        elif i == 256:
            # set power state (arg&1); dispatcher treats 0 as success.
            header += '  virtual IOReturn opaque256(uint32_t);\n'
        elif i == 258:
            # power controllable bool; dispatcher tests bit0.
            header += '  virtual bool opaque258();\n'
        elif i == 260:
            # Original Legacy leaf is a no-argument success stub.
            header += '  virtual IOReturn opaque260();\n'
        elif i == 261:
            header += '  virtual bool opaque261();\n'
        elif i == 249:
            header += '  virtual void getDisplayDimensions(uint32_t *, uint32_t *);\n'
        elif i == 288:
            header += '  virtual bool startHardware();\n'

        elif i == 290:
            header += '  virtual void kickSwapQueue();\n'
        elif i == 295:
            header += '  virtual void completeCurrentSwap();\n'
        elif i == 296:
            header += '  virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
        elif i == 337:
            header += '  virtual IOReturn collectCompletionReports(uint32_t, void *, uint32_t *, uint32_t) = 0;\n'
        elif i == 344:
            header += '  virtual IOReturn opaque344(uint64_t, void *);\n'
        elif i in PURE:
            header += f'  virtual IOReturn opaque{i}(uint32_t, void *, void *, void *) = 0;\n'
        else:
            header += f'  virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
    header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy) == 0x2110, "Original Legacy extent (v43: panel sub-object pointer at +0x1e88 in-bounds)");\n'
    (OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
    if '--audit-only' in sys.argv[2:]:
        print(json.dumps(dict(original_kernelcache_sha256=digest(kc),
                              original_family_text_sha256=FAMILY_HASH,
                              virtual_entries=len(table), abstract_slots=pure,
                              original_constructor_chain_checked=True,
                              original_native_export_count=sum(map(len, names.values())),
                              generated_header_sha256=digest(header.encode()),
                              runtime_verified=False), indent=2))
        return
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))

    def compile_source_at(out_name, src_path):
        args, i = [], 0
        original_args = record['arguments']
        while i < len(original_args):
            arg = original_args[i]
            if arg == record['file']:
                args.append(src_path)
            elif arg == '-o':
                args += ['-o', OUT / (out_name + '.o')]
                i += 1
            elif arg in ('-MF', '-MJ'):
                i += 1
            elif arg == '-Xclang' and original_args[i + 1].startswith('-W'):
                i += 1
            elif not arg.startswith('-W') and arg not in ('-MD', '-MMD', '-MP'):
                args.append(arg)
            i += 1
        args += ['-I', OUT, '-I', ROOT / '.cache/xnu-work/osfmk', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
                 '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-incompatible-sysroot']
        (OUT / (out_name + '-command.json')).write_text(json.dumps([str(a) for a in args], indent=2) + '\n')
        run(args, cwd=record['directory'])
        return OUT / (out_name + '.o')

    def compile_source(name):
        args, i = [], 0
        original_args = record['arguments']
        while i < len(original_args):
            arg = original_args[i]
            if arg == record['file']:
                args.append(SOURCE / (name + '.cpp'))
            elif arg == '-o':
                args += ['-o', OUT / (name + '.o')]
                i += 1
            elif arg in ('-MF', '-MJ'):
                i += 1
            elif arg == '-Xclang' and original_args[i + 1].startswith('-W'):
                i += 1
            elif not arg.startswith('-W') and arg not in ('-MD', '-MMD', '-MP'):
                args.append(arg)
            i += 1
        args += ['-I', OUT, '-I', ROOT / '.cache/xnu-work/osfmk', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
                 '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-incompatible-sysroot']
        (OUT / (name + '-command.json')).write_text(json.dumps([str(a) for a in args], indent=2) + '\n')
        run(args, cwd=record['directory'])
        return OUT / (name + '.o')

    owned = compile_source('E5Display')
    blob = owned.read_bytes()
    image = macho(blob)
    symbols = read_symbols(blob, image)
    vt = next(va for name, va, _ in symbols if name == '__ZTV9E5Display')
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vt < section['va'] + section['size']:
                off, count = struct.unpack_from('<2I', blob, section['command_offset'] + 56)
                for i in range(count):
                    address, bits = struct.unpack_from('<2I', blob, off + i * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[section['va'] + address] = symbols[bits & 0xffffff][0]
    compiled = [relocations.get(vt + 16 + 8 * i) for i in range(358)]
    require(all(compiled) and vt + 16 + 358 * 8 not in relocations, 'compiled concrete vtable is not exactly 358 entries')
    bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
                '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
                '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
                '__ZN25IOMobileFramebufferLegacy13newUserClientEP4taskPvjPP12IOUserClient': table[141] + slide,
                '__ZN25IOMobileFramebufferLegacy5startEP9IOService': table[86] + slide,
                '__ZN25IOMobileFramebufferLegacy4stopEP9IOService': table[87] + slide,
                '__ZTV25IOMobileFramebufferLegacy': vptr - 16}
    bindings['__ZN25IOMobileFramebufferLegacy4freeEv'] = table[14] + slide
    bindings['__ZN25IOMobileFramebufferLegacy9opaque176EPvjjj'] = table[176] + slide
    owned_slots, native_slots = [], []
    for i, name in enumerate(compiled):
        if name.startswith(('__ZN9E5Display', '__ZNK9E5Display')):
            owned_slots.append(i)
            require(i in OWNED, 'unexpected owned override ' + str(i))
        elif '25IOMobileFramebufferLegacy' in name:
            require(i not in PURE and text['va'] <= table[i] < text['va'] + text['filesize'], 'unqualified family binding ' + str(i))
            bindings[name] = table[i] + slide
        else:
            require(i < 168 and name in names.get(table[i], []), 'native inherited symbol mismatch at slot ' + str(i) + ': ' + name)
            native_slots.append(i)
    require(owned_slots == OWNED, 'owned dispatch set changed')
    undefined = {name for name, _, kind in symbols if kind == 1 and 'IOMobileFramebufferLegacy' in name}
    require(undefined <= set(bindings), 'unqualified original-class imports: ' + str(undefined - set(bindings)))
    bindings = {name: bindings[name] for name in undefined}
    bindings.update(surface_bindings)
    asm = OUT / 'E5DisplayBindings.S'
    asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
    run([record['arguments'][0], '--target=arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot',
         '-c', asm, '-o', OUT / 'E5DisplayBindings.o'], cwd=record['directory'])

    initializers = [s for g in image['segments'] for s in g['sections'] if s['name'] == '__mod_init_func']
    require(len(initializers) == 1 and initializers[0]['size'] == 8, 'owned initializer list changed')
    objcopy = shutil.which('llvm-objcopy')
    require(objcopy, 'llvm-objcopy unavailable')
    late = OUT / 'E5Display.late.o'
    run([objcopy, '--remove-section=__DATA,__mod_init_func', owned, late])
    globalize(late, '__GLOBAL__sub_I_E5Display.cpp')
    later = macho(late.read_bytes())
    require(not any(s['name'] == '__mod_init_func' for g in later['segments'] for s in g['sections']), 'owned automatic initializer still present')
    def code(data, info):
        sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
        return data[sec['fileoff']:sec['fileoff'] + sec['size']]
    require(code(blob, image) == code(late.read_bytes(), later), 'late-init staging modified owned instructions')
    probe = compile_source('E5DisplayProbe')
    ca_probe = compile_source('E5CAProbe')
    mali = compile_source_at('E5Mali', SOURCE.parent / 'gpu' / 'E5Mali.cpp')
    # r213: the AppleKeyStore stand-in host plus the pinned component service
    # (board.toml fetches UniXNU/AppleKeyStoreState at 42ff83e into
    # drivers/AppleKeyStoreState). Inert without the e5-aks boot argument.
    keystore = compile_source('E5KeyStore')
    component = ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
    require(component.is_file(), 'AppleKeyStoreState component source missing (check board.toml components.include)')
    keystore_component = compile_source_at('NativeKeyStoreEvents', component)
    archive = ROOT / 'build/e5-emmc/libE5Emmc.a'
    require(archive.is_file(), 'storage archive must be built first')
    run(['ar', 'rcs', archive, late, OUT / 'E5DisplayBindings.o', probe, ca_probe, mali,
         keystore, keystore_component])
    report = dict(schema=1, original_kernelcache_sha256=digest(kc), original_family_text_sha256=FAMILY_HASH,
                  original_slide=slide, legacy_instance_bytes=0x2110, virtual_entries=358,
                  original_abstract_slots=PURE, guarded_backend_slots=[245, 246, 307, 338, 345], owned_slots=owned_slots, inherited_native_slots=native_slots,
                  bindings={name: hex(va) for name, va in sorted(bindings.items())},
                  generated_header_sha256=digest(header.encode()), object_sha256=digest(blob),
                  source_sha256={p.name: digest(p.read_bytes()) for p in
                                 [SOURCE / 'E5Display.cpp', SOURCE / 'E5DisplayProbe.cpp', SOURCE / 'E5CAProbe.cpp',
                                  SOURCE / 'E5KeyStore.cpp', SOURCE / 'E5FramebufferCopy.h', SOURCE / 'E5CpuPresenter.inc', component, Path(__file__)]},
                  late_object_sha256=digest(late.read_bytes()), archive_sha256=digest(archive.read_bytes()),
                  automatic_initializer_removed=True, owned_instructions_unchanged=True,
                  runtime_registration_verified=False, runtime_construction_verified=False,
                  driver_published=False, display_presented=False)
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('e5-display-report: ' + json.dumps(report, sort_keys=True))
    print('e5-display: exact 358-slot construction probe built; runtime and backend remain unqualified')


if __name__ == '__main__':
    main()
