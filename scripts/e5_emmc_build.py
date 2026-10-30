#!/usr/bin/env python3
"""Compile the E5 eMMC driver against the actual source-kernel ABI.

Mirrors the RamBlock component recipe: the compile command of the real kernel
build (IOPlatformExpert.cpo.json), with the source swapped for NativeEmmc.cpp
and the storage-ABI include added, then archived into build/e5-emmc/libE5Emmc.a.
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]  # <tree>/scripts -> tree
tree = root
recipe = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
source_dir = Path(__file__).resolve().parent / 'emmc'
out = tree / 'build/e5-emmc'
out.mkdir(parents=True, exist_ok=True)
archive = out / 'libE5Emmc.a'

record = json.loads(recipe.read_text().strip().rstrip(','))
arguments = record['arguments']
warnings = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier',
                '-I', str(tree / '.cache/xnu-work/osfmk'),
                '-I', str(tree / 'build/storage-abi/include')]
objects = []
for name in ('NativeEmmc', 'NativeSd'):
    source = source_dir / (name + '.cpp')
    destination = out / (name + '.o')
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend(warnings)
    (out / (name + '-compile-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / (name + '-compile.log')).write_text(result.stdout)
    if result.returncode:
        sys.stderr.write(result.stdout[-3500:])
        raise SystemExit(name + ' compile failed')
    data = destination.read_bytes()
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit('Unexpected platform object architecture')
    objects.append(str(destination))
archive.unlink(missing_ok=True)
subprocess.run(['ar', 'rcs', str(archive), *objects], check=True)
print(f'e5-storage: {archive} ({archive.stat().st_size} bytes, eMMC + SD)')
