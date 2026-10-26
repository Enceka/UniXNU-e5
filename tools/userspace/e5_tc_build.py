#!/usr/bin/env python3
"""Build e5 trust cache: e5_tc_build.py <cdhash1> [cdhash2 ...]
Format (DarwinBoot ValidTrustCaches): U32 count, U32 offsets[count],
blob{U32 ver=2, uuid16, U32 n, entries[24B]{cdhash20, htype, flags, pad2}} sorted by cdhash."""
import struct, sys

hashes = sorted(set(h.lower() for h in sys.argv[1:]))
assert 1 <= len(hashes) <= 128, len(hashes)
UUID = bytes.fromhex('1228e6e5f2934aa29f9b0567aa8270cc')
blob = struct.pack('<I16sI', 2, UUID, len(hashes))
for h in hashes:
    b = bytes.fromhex(h)
    assert len(b) == 20, h
    blob += b + bytes([2, 0]) + b'\0\0'   # cdhash, hash_type=2, flags=0, pad
out = struct.pack('<II', 1, 8) + blob  # one blob, N sorted entries
open('/tmp/kernel.macho.trustcache', 'wb').write(out)
print(f'TC: {len(hashes)} entries, {len(out)} bytes ->', ' '.join(h[:8] for h in hashes))
