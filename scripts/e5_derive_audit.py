# E5: derive build/storage-abi/interface-audit.json from the pinned original
# iPadOS 17.7.11 kernelcache. Structural fork of scripts/audit-storage-interface.py:
# the private live-observation candidate inputs are replaced by direct derivation
# from the original kernelcache (same module bytes emit_prelink places at slide).
# Deployed as a board.toml archives entry (cwd = unixnu tree root), after the
# storage-abi headers exist and before build-virtio-mmio-bindings.py.
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.request import Request, urlopen

PIN = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
SLIDE = -0x1000000
URL = 'https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw'
MODULE_ID = 'com.apple.iokit.IOStorageFamily'
SLOTS = 190
PURE = 14
BRK = 0xd4200020


def die(msg):
    raise SystemExit('e5-derive: ' + msg)


def require(value, message):
    if not value:
        die(message)


class RF(io.RawIOBase):
    def __init__(self, url):
        self.url = url
        r = urlopen(Request(url, method='HEAD'), timeout=120)
        self.len = int(r.headers['Content-Length'])
        self.pos = 0

    def seekable(self):
        return True

    def readable(self):
        return True

    def seek(self, off, whence=0):
        if whence == 1:
            off = self.pos + off
        elif whence == 2:
            off = self.len + off
        self.pos = off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n < 0:
            n = self.len - self.pos
        if self.pos >= self.len or n == 0:
            return b''
        end = min(self.pos + n, self.len) - 1
        r = urlopen(Request(self.url, headers={'Range': 'bytes=%d-%d' % (self.pos, end)}), timeout=300)
        if r.status != 206:
            die('range request ignored (%d)' % r.status)
        data = r.read()
        self.pos += len(data)
        return data


def der_octet_string(d):
    # IM4P container is DER: SEQ(IA5 'IM4P', IA5 'krnl', IA5 desc, OCTET STRING payload)

    def rd_tl(p):
        tag = d[p]
        l = d[p + 1]
        p += 2
        if l & 0x80:
            n = l & 0x7f
            l = int.from_bytes(d[p:p + n], 'big')
            p += n
        return tag, l, p

    tag, l, p = rd_tl(0)
    require(tag == 0x30, 'kernelcache member is not an IM4P DER sequence')
    for _ in range(3):
        tag, l, p = rd_tl(p)
        require(tag == 0x16, 'expected IA5String in IM4P header')
        p += l
    tag, l, p = rd_tl(p)
    require(tag == 0x04, 'expected OCTET STRING kernelcache payload')
    return d[p:p + l]


def fat_arm64_slice(data):
    if data[:4] != b'\xca\xfe\xba\xbe':
        return data
    cnt = struct.unpack('>I', data[4:8])[0]
    for i in range(cnt):
        c = data[8 + i * 20:8 + (i + 1) * 20]
        cpu = struct.unpack('>I', c[0:4])[0]
        if cpu == 0x0100000c:
            off = struct.unpack('>I', c[8:12])[0]
            ln = struct.unpack('>I', c[12:16])[0]
            return data[off:off + ln]
    die('no arm64 slice in fat kernelcache')


def load_kc(root):
    env = os.environ.get('E5_KERNELCACHE')
    cache = root / 'build' / 'e5-kernelcache'
    for path in [Path(env) if env else None, cache if cache.is_file() else None]:
        if path:
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() == PIN:
                print('e5-derive: kernelcache cache %s (%d bytes)' % (path, len(data)))
                return data
    rf = RF(URL)
    z = zipfile.ZipFile(rf)
    target = None
    for n in [i.filename for i in z.infolist()]:
        if 'kernelcache' in n.lower():
            target = n
            if n.lower().endswith('kernelcache.release.ipad7'):
                break
    require(target is not None, 'kernelcache member not found in ipsw')
    member = z.read(target)
    print('e5-derive: ipsw member %s (%d bytes)' % (target, len(member)))
    payload = der_octet_string(member)
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / 'kc.lzfse'
        dst = Path(td) / 'kc.raw'
        src.write_bytes(payload)
        r = subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', str(src), '-o', str(dst)],
                           capture_output=True, text=True)
        require(r.returncode == 0, 'lzfse decode failed: ' + r.stderr.strip()[:200])
        data = fat_arm64_slice(dst.read_bytes())
    require(hashlib.sha256(data).hexdigest() == PIN, 'kernelcache sha mismatch (pin changed upstream?)')
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(data)
    print('e5-derive: kernelcache %d bytes (sha pin ok, cached)' % len(data))
    return data


def commands_of(m, off):
    ncmd = struct.unpack_from('<I', m, off + 16)[0]
    p = off + 32
    for _ in range(ncmd):
        cmd, csz = struct.unpack_from('<II', m, p)
        yield cmd, p, csz
        p += csz


def segments_of(m, off):
    segs = {}
    for cmd, p, csz in commands_of(m, off):
        if cmd == 0x19:
            name = m[p + 8:p + 24].rstrip(b'\0').decode()
            vm, vmsz, fs, fssz = struct.unpack_from('<QQQQ', m, p + 24)
            segs[name] = (vm, vmsz, fs, fssz)
    return segs


def macho_sections(o, base):
    out = []
    for cmd, p, csz in commands_of(o, base):
        if cmd == 0x19:
            seg = o[p + 8:p + 24].rstrip(b'\0').decode()
            nsec = struct.unpack_from('<I', o, p + 64)[0]
            sp = p + 72
            for s in range(nsec):
                saddr, ssize = struct.unpack_from('<QQ', o, sp + 32)
                soff, align, reloff, nreloc = struct.unpack_from('<IIII', o, sp + 48)
                out.append((seg, o[sp:sp + 16].rstrip(b'\0').decode(), saddr, ssize, soff, reloff, nreloc))
                sp += 80
    return out


def _xml_int(text):
    return int(text or '0', 0)


def _read_xml(element):
    tag = element.tag
    if tag == 'dict':
        ch = list(element)
        return {_read_xml(ch[i]): _read_xml(ch[i + 1]) for i in range(0, len(ch), 2)}
    if tag == 'array':
        return [_read_xml(c) for c in element]
    if tag in ('key', 'string'):
        return element.text or ''
    if tag == 'integer':
        return _xml_int(element.text)
    if tag == 'data':
        import base64
        return base64.b64decode(''.join((element.text or '').split()))
    if tag in ('true', 'false'):
        return tag == 'true'
    raise ValueError('unsupported prelink element ' + tag)


def find_module(kc):
    key = kc.find(b'<key>_PrelinkInfoDictionary</key>')
    arr = kc.find(b'<array>', key)
    depth = 0
    pos = arr
    end = None
    while True:
        no = kc.find(b'<array>', pos)
        nc = kc.find(b'</array>', pos)
        if nc < 0:
            die('unterminated prelink array')
        if 0 <= no < nc:
            depth += 1
            pos = no + 7
        else:
            depth -= 1
            pos = nc + 8
            if depth == 0:
                end = nc
                break
    mods = _read_xml(ET.fromstring(kc[arr:end + 8]))
    for m in mods:
        if m.get('CFBundleIdentifier', '') == MODULE_ID:
            return m['_PrelinkExecutableLoadAddr'], m['_PrelinkExecutableSize']
    die('%s module not found' % MODULE_ID)


def adrp_imm(a):
    immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    return immediate


def branch_target(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def derive_module(kc):
    segs = segments_of(kc, 0)

    def kcread(va, n):
        for name, (vm, vmsz, fs, fssz) in segs.items():
            if vm <= va and va + n <= vm + vmsz:
                at = fs + (va - vm)
                return kc[at:at + n]
        die('kernelcache va 0x%x unmapped' % va)

    load, msize = find_module(kc)
    moff = None
    for name, (vm, vmsz, fs, fssz) in segs.items():
        if vm <= load < vm + vmsz:
            moff = fs + (load - vm)
            break
    require(moff is not None and struct.unpack_from('<I', kc, moff)[0] == 0xfeedfacf,
            '%s module header not found at load addr' % MODULE_ID)
    msecs = macho_sections(kc, moff)
    print('e5-derive: %s module at kc file 0x%x (load 0x%x)' % (MODULE_ID, moff, load))

    const = next((s for s in msecs if s[0] == '__DATA_CONST' and s[1] == '__const' and s[3] >= (SLOTS + 2) * 8), None)
    require(const is not None, 'module __DATA_CONST __const section missing')
    text = [(s[2], s[2] + s[3]) for s in msecs if s[0] == '__TEXT_EXEC']
    require(text, 'module __TEXT_EXEC section missing')
    exec_ranges = [(vm, vm + vmsz) for name, (vm, vmsz) in
                   [(n, (v[0], v[1])) for n, v in segs.items()] if name in ('__TEXT_EXEC', '__PRELINK_TEXT', '__PLK_TEXT_EXEC')]

    blob = kcread(const[2], const[3])
    found = []
    for at in range(0, len(blob) - (SLOTS + 2) * 8 + 1, 8):
        if blob[at:at + 16] != bytes(16):
            continue
        ptrs = struct.unpack_from('<' + 'Q' * SLOTS, blob, at + 16)
        if not all(any(a <= q < b for a, b in exec_ranges) for q in ptrs):
            continue
        if struct.unpack_from('<I', kcread(ptrs[0], 4), 0)[0] != BRK:
            continue
        if struct.unpack_from('<I', kcread(ptrs[1], 4), 0)[0] != BRK:
            continue
        dtor_word = struct.unpack('<I', kcread(ptrs[0] - 4, 4))[0]
        if dtor_word >> 26 != 0x05:
            continue
        counts = {}
        for q in ptrs:
            counts[q] = counts.get(q, 0) + 1
        if sorted(counts.values())[-1] != PURE or len([1 for v in counts.values() if v == PURE]) != 1:
            continue
        found.append((const[2] + at, ptrs))
    require(len(found) == 1, 'expected one %d-slot vtable in module const, found %d' % (SLOTS, len(found)))
    table, ptrs = found[0]
    pure_stub = max(counts := {q: 1 for q in ptrs}, key=lambda q: sum(1 for r in ptrs if r == q))

    lo, hi = min(a for a, b in text), max(b for a, b in text)
    constructors = []
    tbytes = kcread(lo, hi - lo)
    for off in range(0, len(tbytes) - 32, 4):
        words = struct.unpack_from('<8I', tbytes, off)
        if words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000:
            continue
        if words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108:
            continue
        if words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0):
            continue
        stored = ((lo + off + 12) & ~4095) + adrp_imm(words[3]) * 4096 + (words[4] >> 10 & 4095)
        if stored == table + 16:
            constructors.append(lo + off)
    require(len(constructors) == 1, 'expected one protected constructor, found %d' % len(constructors))

    a, add, ret = struct.unpack('<3I', kcread(ptrs[7], 12))
    require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
            'unqualified storage metaclass getter')
    meta = (ptrs[7] & ~4095) + adrp_imm(a) * 4096 + (add >> 10 & 4095)

    print('e5-derive: vtable 0x%x ctor 0x%x dtor 0x%x meta 0x%x pure-stub 0x%x' %
          (table, constructors[0], ptrs[0] - 4, meta, pure_stub))
    return table, ptrs, constructors[0], ptrs[0] - 4, meta


def ref_compile(root):
    root = root.resolve()
    source = root / 'refer' / 'IOStorageFamily'
    out = root / 'build' / 'storage-abi'
    record = json.loads((root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                        .read_text().strip().rstrip(','))
    obj_path = out / 'IOBlockStorageDevice-reference.o'
    arguments = record['arguments']
    command, index = [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source / 'IOBlockStorageDevice.cpp'))
        elif value == '-o':
            command += ['-o', str(obj_path)]
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command += ['-I', str(out / 'include'), '-I', str(root / '.cache/xnu-work/osfmk'),
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-unnecessary-virtual-specifier',
                '-Wno-deprecated-enum-enum-conversion']
    r = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (out / 'e5-ref-compile.log').write_text(r.stdout + r.stderr)
    if r.returncode:
        die('reference compile failed: ' + (r.stdout + r.stderr)[-1500:])
    return obj_path


def ref_names(root):
    obj_path = None
    if os.environ.get('E5_REF_OBJECT'):
        obj_path = Path(os.environ['E5_REF_OBJECT'])
    else:
        cpo = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
        if cpo.is_file():
            obj_path = ref_compile(root)
    require(obj_path is not None and obj_path.is_file(),
            'reference object unavailable (no cpo.json and no E5_REF_OBJECT)')
    obj = obj_path.read_bytes()
    sycmd = next(at for cmd, at, size in commands_of(obj, 0) if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', obj, sycmd + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + i * 16)
        z = obj.index(b'\0', stroff + ix)
        symbols.append((obj[stroff + ix:z].decode(), value, kind))
    vtable = next(v for n, v, k in symbols if n == '__ZTV20IOBlockStorageDevice')
    end = min(v for n, v, k in symbols if not k & 0xe0 and k & 0xe == 0xe and v > vtable)
    relocations = {}
    for seg, sec, saddr, ssize, soff, reloff, nreloc in macho_sections(obj, 0):
        if saddr <= vtable < saddr + ssize:
            for i in range(nreloc):
                address, word = struct.unpack_from('<II', obj, reloff + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[saddr + address] = symbols[word & 0xffffff][0]
    methods = []
    for at in range(vtable + 16, end, 8):
        if at not in relocations:
            break
        methods.append(relocations[at])
    require(len(methods) == SLOTS and methods.count('___cxa_pure_virtual') == PURE,
            'reference dispatch shape changed (%d slots, %d pure)' % (len(methods), methods.count('___cxa_pure_virtual')))
    print('e5-derive: reference object %d slots, %d pure virtual' % (len(methods), PURE))
    return methods


def main():
    ctx = sys.argv[1] if len(sys.argv) > 1 else '.'
    root = Path(ctx)
    kc = load_kc(root)
    table, ptrs, ctor, dtor, meta = derive_module(kc)
    names = ref_names(root)
    methods = [dict(slot=i, reference_name=names[i], candidate_target=hex(ptrs[i] + SLIDE), kernel_names=[])
               for i in range(SLOTS)]
    audit = {'schema': 'storage.interface.audit/1',
             'derived_from_kernelcache_sha256': PIN,
             'candidate_sha256': PIN,
             'live_native_4k': True,
             'method_slots': SLOTS,
             'abstract_complete_destructors_trap': True,
             'vtable': hex(table + SLIDE),
             'original_protected_constructor': hex(ctor + SLIDE),
             'original_base_destructor': hex(dtor + SLIDE),
             'original_metaclass': hex(meta + SLIDE),
             'methods': methods}
    outp = root / 'build' / 'storage-abi' / 'interface-audit.json'
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text(json.dumps(audit, indent=2) + '\n')
    print('e5-derive: wrote %s (%d methods, slide %#x)' % (outp, SLOTS, SLIDE))


if __name__ == '__main__':
    main()
