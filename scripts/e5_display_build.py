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
    for pc, target in [(0xfffffff0057865fc, 0xfffffff00577b510),
                       (0xfffffff00577b540, 0xfffffff00577b318),
                       (0xfffffff005786630, 0xfffffff00577b55c)]:
        word, = struct.unpack('<I', read(pc - slide, 4))
        require(word & 0xfc000000 == 0x94000000 and
                derive.branch_target(pc - slide, word) + slide == target, 'original C2/D2 chain changed')

    header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generated opaque inheritance layout for the hash-qualified 21H461 family.
#pragma once
#include <IOKit/IOService.h>
class IOMobileFramebufferLegacy : public IOService {
  OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
  unsigned char originalPrivate[0x18a0 - sizeof(IOService)];
public:
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
        if i in PURE:
            header += f'  virtual __attribute__((noreturn)) void opaque{i}() = 0;\n'
        else:
            header += f'  virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
    header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy) == 0x18a0, "Original Legacy extent");\n'
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
                 '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion']
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
                '__ZTV25IOMobileFramebufferLegacy': vptr - 16}
    owned_slots, native_slots = [], []
    for i, name in enumerate(compiled):
        if name.startswith(('__ZN9E5Display', '__ZNK9E5Display')):
            owned_slots.append(i)
            require(i in [0, 1, 7, 86, 91] + PURE, 'unexpected owned override ' + str(i))
        elif '25IOMobileFramebufferLegacy' in name:
            require(i not in PURE and text['va'] <= table[i] < text['va'] + text['filesize'], 'unqualified family binding ' + str(i))
            bindings[name] = table[i] + slide
        else:
            require(i < 168 and name in names.get(table[i], []), 'native inherited symbol mismatch at slot ' + str(i) + ': ' + name)
            native_slots.append(i)
    require(owned_slots == sorted([0, 1, 7, 86, 91] + PURE), 'owned dispatch set changed')
    undefined = {name for name, _, kind in symbols if kind == 1 and 'IOMobileFramebufferLegacy' in name}
    require(undefined <= set(bindings), 'unqualified original-class imports: ' + str(undefined - set(bindings)))
    bindings = {name: bindings[name] for name in undefined}
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
    archive = ROOT / 'build/e5-emmc/libE5Emmc.a'
    require(archive.is_file(), 'storage archive must be built first')
    run(['ar', 'rcs', archive, late, OUT / 'E5DisplayBindings.o', probe])
    report = dict(schema=1, original_kernelcache_sha256=digest(kc), original_family_text_sha256=FAMILY_HASH,
                  original_slide=slide, legacy_instance_bytes=0x18a0, virtual_entries=358,
                  guarded_backend_slots=PURE, owned_slots=owned_slots, inherited_native_slots=native_slots,
                  bindings={name: hex(va) for name, va in sorted(bindings.items())},
                  generated_header_sha256=digest(header.encode()), object_sha256=digest(blob),
                  source_sha256={p.name: digest(p.read_bytes()) for p in
                                 [SOURCE / 'E5Display.cpp', SOURCE / 'E5DisplayProbe.cpp', Path(__file__)]},
                  late_object_sha256=digest(late.read_bytes()), archive_sha256=digest(archive.read_bytes()),
                  automatic_initializer_removed=True, owned_instructions_unchanged=True,
                  runtime_registration_verified=False, runtime_construction_verified=False,
                  driver_published=False, display_presented=False)
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('e5-display-report: ' + json.dumps(report, sort_keys=True))
    print('e5-display: exact 358-slot construction probe built; runtime and backend remain unqualified')


if __name__ == '__main__':
    main()
