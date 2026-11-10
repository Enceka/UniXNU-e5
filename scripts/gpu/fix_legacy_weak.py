"""Scope legacy Mach-O weak flat imports to self, refusing strong gaps."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys

source, target = map(Path, sys.argv[1:3])
assert source.resolve() != target.resolve() and not target.exists()
data = bytearray(source.read_bytes())
assert struct.unpack_from('<I', data)[0] == 0xfeedfacf
count = struct.unpack_from('<I', data, 16)[0]
offset = 32
streams = []
symtab = None
libraries = []
for _ in range(count):
    cmd, size = struct.unpack_from('<II', data, offset)
    assert size >= 8
    if cmd in (0x22, 0x80000022):
        values = struct.unpack_from('<10I', data, offset + 8)
        streams = [('bind', values[2], values[3]), ('lazy', values[6], values[7])]
    elif cmd == 2:
        symtab = struct.unpack_from('<4I', data, offset + 8)
    elif cmd in (12, 0x80000018, 0x8000001f):
        at = offset + struct.unpack_from('<I', data, offset + 8)[0]
        libraries.append(data[at:data.index(0, at)].decode())
    offset += size
assert streams and symtab

bindings, flat_opcodes = [], []
for name, start, size in streams:
    at, end = start, start + size
    ordinal, symbol, weak = 0, None, False

    def leb():
        global at
        value = shift = 0
        while at < end:
            byte = data[at]; at += 1
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
            shift += 7
            assert shift < 70
        raise ValueError('unterminated LEB')

    def bind():
        assert symbol is not None
        bindings.append(dict(symbol=symbol, ordinal=ordinal, weak=weak, stream=name))

    while at < end:
        position = at
        byte = data[at]; at += 1
        op, imm = byte & 240, byte & 15
        if op == 0:
            if name == 'bind': break
            ordinal, symbol, weak = 0, None, False
        elif op == 0x10:
            ordinal = imm
        elif op == 0x20:
            ordinal = leb()
        elif op == 0x30:
            ordinal = imm - 16 if imm else 0
            if ordinal == -2: flat_opcodes.append(position)
        elif op == 0x40:
            weak = bool(imm & 1)
            stop = data.index(0, at, end)
            symbol = data[at:stop].decode(); at = stop + 1
        elif op == 0x50:
            pass
        elif op in (0x60, 0x70, 0x80):
            leb()
        elif op in (0x90, 0xb0):
            bind()
        elif op == 0xa0:
            bind(); leb()
        elif op == 0xc0:
            repeats = leb(); leb()
            if repeats: bind()
        else:
            raise ValueError(f'unsupported bind opcode {byte:#x}')

flat = [entry for entry in bindings if entry['ordinal'] == -2]
strong = [entry for entry in bindings if entry['ordinal'] < 0 and not entry['weak']]
assert flat and not strong, strong
for position in flat_opcodes:
    assert data[position] == 0x3e
    data[position] = 0x30  # SET_DYLIB_SPECIAL_IMM, self; same encoded length.

symoff, nsyms, stroff, strsize = symtab
changed = 0
for i in range(nsyms):
    at = symoff + i * 16
    strx, kind, section, desc, value = struct.unpack_from('<IBBHQ', data, at)
    if kind & 0x0e == 0 and desc & 0x40 and desc >> 8 == 0xfe:
        struct.pack_into('<H', data, at + 6, desc & 255)
        changed += 1
assert changed == len({entry['symbol'] for entry in flat})
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(data)
subprocess.run(['codesign', '-f', '-s', '-', str(target)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(target)], check=True)
text = subprocess.check_output(['nm', '-m', '-u', str(target)], text=True)
assert 'dynamically looked up' not in text
report = dict(source=str(source), target=str(target), libraries=libraries,
              source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
              sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
              bindings=len(bindings), weak_symbols_to_self=changed,
              ordinal_opcodes_changed=len(flat_opcodes), strong_unresolved=strong,
              signature_verified=True)
target.with_suffix('.link-audit.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
