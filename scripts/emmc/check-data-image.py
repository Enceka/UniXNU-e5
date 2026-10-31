#!/usr/bin/env python3
"""Qualify the fixed, single-volume raw E5 Data fixture before a kernel build.

Checks the UUID selected by NativeSd against checksummed APFS volume objects.
This conservative fixture scan does not implement checkpoint/omap recovery or
qualify writable mounting. See Apple's Apple File System Reference, obj_phys_t,
nx_superblock_t and apfs_superblock_t. No image or device writes are performed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import uuid
import zlib


def checksum(block):
    modulus = 0xFFFFFFFF
    first = second = 0
    for (word,) in struct.iter_unpack('<I', block[8:]):
        first = (first + word) % modulus
        second = (second + first) % modulus
    low = modulus - (first + second) % modulus
    high = modulus - (first + low) % modulus
    return low | high << 32


def check(image, expected_uuid, expected_size):
    if len(image) != expected_size:
        raise ValueError(f'Data window size {len(image)} != {expected_size}')
    if image[32:36] != b'NXSB':
        raise ValueError('Data window does not begin with a raw NXSB')
    block_size, block_count = struct.unpack_from('<IQ', image, 36)
    if block_size != 4096 or not block_count or block_count > len(image) // block_size:
        raise ValueError('unsupported or truncated APFS container')
    if struct.unpack_from('<I', image, 24)[0] & 0xFFFF != 1:
        raise ValueError('invalid NXSB object type')
    if checksum(image[:block_size]) != struct.unpack_from('<Q', image)[0]:
        raise ValueError('NXSB checksum mismatch')
    expected_uuid = str(uuid.UUID(expected_uuid)).upper()
    container_xid = struct.unpack_from('<Q', image, 16)[0]
    volumes = []
    for offset in range(0, block_count * block_size, block_size):
        block = image[offset:offset + block_size]
        if block[32:36] != b'APSB' or struct.unpack_from('<I', block, 24)[0] & 0xFFFF != 13:
            continue
        if checksum(block) != struct.unpack_from('<Q', block)[0]:
            raise ValueError(f'APSB checksum mismatch at {offset:#x}')
        xid = struct.unpack_from('<Q', block, 16)[0]
        actual_uuid = str(uuid.UUID(bytes=bytes(block[0xF0:0x100]))).upper()
        role = struct.unpack_from('<H', block, 0x3C4)[0]
        if xid > container_xid or not xid:
            raise ValueError(f'APSB transaction outside this fixture at {offset:#x}')
        if actual_uuid != expected_uuid:
            raise ValueError(f'Data UUID {actual_uuid} != driver UUID {expected_uuid}')
        volumes.append(dict(offset=offset, xid=xid, uuid=actual_uuid, role=role))
    if not volumes:
        raise ValueError('no checksummed Data APSB found')
    # The fixture retains older superblocks from before its Data-role change.
    latest = max(volumes, key=lambda volume: volume['xid'])
    if latest['role'] != 0x40:
        raise ValueError(f"latest volume role {latest['role']:#x} is not Data")
    return dict(result='PASS', bytes=len(image), container_bytes=block_count * block_size,
                sha256=hashlib.sha256(image).hexdigest(),
                superblock_crc32=f'{zlib.crc32(image[:block_size]):08x}',
                volume_uuid=expected_uuid, volume_superblocks=volumes,
                limitation='Fixed fixture identity only; APFS publication, RW mount and tzinit require a board run.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='complete raw Data window or readback matching the header size')
    parser.add_argument('--window-header', type=Path, default=Path(__file__).with_name('SystemOSWindow.h'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    data_config = args.window_header.read_text().split('namespace E5Data {', 1)[1]
    expected_uuid = re.search(r'const char \*uuid = "([0-9A-F-]+)";', data_config)[1]
    expected_size = int(re.search(r'uint64_t sectors = ([0-9]+);', data_config)[1]) * 512
    report = check(args.image.read_bytes(), expected_uuid, expected_size)
    report.update(image=str(args.image), window_header=str(args.window_header))
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        raise SystemExit(str(error))
