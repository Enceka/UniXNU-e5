#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors.
"""Identify the semantics of the opaque Legacy framebuffer vtable slots.

Scans the original IOMobileGraphicsFamily __TEXT_EXEC for virtual dispatches
(ldr vtable-ptr; ldr slot; blr) and reports, per slot, the enclosing named
functions. Pure virtual slots [211,213,245,246,306,307,337,338,345] are the
concrete-platform-driver backend methods E5Display must implement.
"""
import importlib.util
import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

FAMILY_HASH = '2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b'
PURE = [211, 213, 245, 246, 306, 307, 337, 338, 345]
ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else 'build/e5-display/slots-report.json')

sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports  # noqa: E402

spec = importlib.util.spec_from_file_location('e5_derive', Path(__file__).with_name('e5_derive_audit.py'))
derive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(derive)

kc = derive.load_kc(ROOT)
original = macho(kc)


def read(va, size):
    off = backed_offset(original['segments'], va, size, len(kc))
    return kc[off:off + size]


derive.MODULE_ID = 'com.apple.iokit.IOMobileGraphicsFamily'
load, _ = derive.find_module(kc)
module = macho(kc, backed_offset(original['segments'], load, 32, len(kc)))
text = next(s for s in module['segments'] if s['name'] == '__TEXT_EXEC')
import hashlib
assert hashlib.sha256(read(text['va'], text['filesize'])).hexdigest() == FAMILY_HASH
slide = derive.SLIDE
vptr = 0xfffffff005f38570
table = struct.unpack('<358Q', read(vptr - slide, 358 * 8))
names = exports(kc, original)

# Function map: named addresses inside family text (KC slid VAs).
named = sorted((va + slide, n) for va, ns in names.items() for n in ns
               if text['va'] <= va + slide < text['va'] + text['filesize'])
print('family text va %#x size %#x, %d named addrs' % (text['va'], text['filesize'], len(named)))


def enclosing(pc):
    lo, hi = 0, len(named)
    while lo < hi:
        mid = (lo + hi) // 2
        if named[mid][0] <= pc:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    return named[lo - 1]


md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
md.detail = False
code = read(text['va'], text['filesize'])
calls = {}  # slot -> {caller_key: count}
WINDOW = 8
insns = list(md.disasm(code, text['va']))
print('disassembled %d insns' % len(insns))

# Simple reg-def tracking: reg -> (insn_index, kind, info)
defs = {}
for idx, ins in enumerate(insns):
    m = ins.mnemonic
    ops = ins.op_str
    if m == 'ldr' and ops.startswith('x'):
        # ldr Xt, [Xn{, #imm}]
        dst = ops.split(',')[0].strip()
        mem = ops[ops.find('['):]
        parts = mem.strip('[]').split(',')
        src = parts[0].strip()
        imm = 0
        if len(parts) > 1 and parts[1].strip().startswith('#'):
            imm = int(parts[1].strip().lstrip('#'), 0)
        defs[dst] = (idx, 'ldr', src, imm)
    elif m == 'blr':
        target = ops.strip()
        d = defs.get(target)
        if not d or d[1] != 'ldr':
            continue
        i1, _, src1, imm1 = d
        d0 = defs.get(src1)
        if not d0 or d0[1] != 'ldr':
            continue
        i0, _, src0, imm0 = d0
        if idx - i1 > WINDOW or i1 - i0 > WINDOW:
            continue
        if imm0 != 0:
            continue  # vtable pointer comes from offset 0 of the object
        slot = imm1 // 8
        if slot < 168:
            continue
        fn = enclosing(ins.address)
        key = (fn[1] if fn else 'sub_%x' % ins.address), fn[0] if fn else ins.address
        calls.setdefault(slot, {})
        kname = '%s@%#x' % key
        calls[slot][kname] = calls[slot].get(kname, 0) + 1

report = {'pure': PURE, 'slots': {}}
print('\n== virtual dispatches into Legacy-specific slots (>=168) ==')
for slot in sorted(calls):
    callers = calls[slot]
    tag = ' PURE' if slot in PURE else ''
    print('slot %3d%s : %s' % (slot, tag, ', '.join('%s x%d' % kv for kv in sorted(callers.items()))))
    report['slots'][str(slot)] = callers
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(json.dumps(report, indent=1) + '\n')
print('\nwrote', OUT)
print('PURE slots with NO in-family callers:', [s for s in PURE if s not in calls])
