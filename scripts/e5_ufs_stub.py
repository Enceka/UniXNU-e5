#!/usr/bin/env python3
"""E5 bring-up: satisfy the optional RamBlock persistence-store symbols.

RamBlock references native_ufs_store_* as weak imports ("a build without the
provider keeps the volatile disk"), but the strict XNU kernel link rejects the
undefined symbols. E5 has no SM8550 ShengUfs transport; this stub defines the
six symbols so the link resolves, with mode()/gpt_check() returning 0 so the
runtime persistence path stays disabled and the pure RAM disk is kept.
Deployed as a board.toml archives entry (cwd = unixnu tree root), after the
RamBlock component and its bindings step.
"""
import subprocess
import sys
from pathlib import Path

TEXT = """\
.text
.globl _native_ufs_store_mode
_native_ufs_store_mode:
    mov w0, #0
    ret
.globl _native_ufs_store_gpt_check
_native_ufs_store_gpt_check:
    mov w0, #0
    ret
.globl _native_ufs_store_open
_native_ufs_store_open:
    mov w0, #-1
    ret
.globl _native_ufs_store_read
_native_ufs_store_read:
    mov w0, #-1
    ret
.globl _native_ufs_store_write
_native_ufs_store_write:
    mov w0, #-1
    ret
.globl _native_ufs_store_sync
_native_ufs_store_sync:
    mov w0, #-1
    ret
"""


def main():
    ctx = sys.argv[1] if len(sys.argv) > 1 else '.'
    out = Path(ctx).resolve() / 'build' / 'ram-block'
    out.mkdir(parents=True, exist_ok=True)
    asm = out / 'e5-ufs-stub.S'
    asm.write_text(TEXT)
    obj = out / 'e5-ufs-stub.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                    '-Wno-incompatible-sysroot', '-c', asm, '-o', obj], check=True)
    subprocess.run(['xcrun', 'ar', 'rcs', out / 'libRamBlock.a', obj], check=True)
    print('e5-ufs-stub: store provider stubs archived into %s' % (out / 'libRamBlock.a'))


if __name__ == '__main__':
    main()
