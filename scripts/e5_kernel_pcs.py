#!/usr/bin/env python3
"""e5_kernel_pcs: decode kernel PCs from panic backtraces against the pinned KC.

Kernel runtime VAs are slid -0x1000000 from KC-file VAs. Prints the nearest
export symbol for each VA (kernel-wide exports()) plus family-text sub_XXX
labels for com.apple.iokit.IOMobileGraphicsFamily addresses.

Usage: e5_kernel_pcs.py <va> [<va> ...]
"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('e5_derive', ROOT / 'scripts/e5_derive_audit.py')
derive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(derive)
SLIDE = derive.SLIDE

FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'


def main():
    sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
    from native_prelink import exports, macho, backed_offset
    kc = derive.load_kc(ROOT)
    original = macho(kc)
    derive.MODULE_ID = FAMILY
    load, _ = derive.find_module(kc)
    module = macho(kc, backed_offset(original['segments'], load, 32, len(kc)))
    text = next(s for s in module['segments'] if s['name'] == '__TEXT_EXEC')
    f_lo, f_hi = text['va'] + SLIDE, text['va'] + SLIDE + text['filesize']
    sym = exports(kc, original)
    keys = sorted(sym)
    import bisect
    for arg in sys.argv[1:]:
        va = int(arg, 0)
        raw = va - SLIDE
        i = bisect.bisect_right(keys, raw) - 1
        label = '??'
        if i >= 0:
            base = keys[i]
            label = '%s+%#x' % (sym[base][0], raw - base)
        if f_lo <= va < f_hi:
            label += ' [family sub_%x+%#x]' % (va, 0)
        print('%#x -> %s' % (va, label))


if __name__ == '__main__':
    main()
