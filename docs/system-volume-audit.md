# Original system inputs and storage layout

Updated 2026-10-30. The selected iPadOS 17.7.11 / 21H461 inputs are downloaded,
checked against the OS profile, inspected read-only and assembled locally.
This prepares the original system; it does not establish an E5 boot of that
userland. Detailed hashes and geometry are in
[system-volume-audit.json](logs/system-volume-audit.json).

## Verified system inputs

The Erase identity is `j120ap`, product `iPad7,1`, chip `0x8011`, board `0x0c`.
Both DMG SHA256 values match the pinned profile, and extraction also checks
their ZIP CRCs. Common tools are pinned at
`066c8d8315f83749d28bcae2faf3595718646be5`.

| Window | IPSW member | Stored file bytes | Raw disk bytes | APFS volume UUID |
| --- | --- | --- | --- | --- |
| OS | `044-76040-038.dmg` | 5157406069 | 6077580800 | `04769D26-2CA0-4825-B443-E2E5F88300F2` |
| SystemOS | `044-76034-038.dmg` | 3303014400 | 3303014400 | `8846FE78-2910-4297-AF97-D59BB6F8FC16` |

Original launchd SHA256 is
`8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25`;
dyld SHA256 is
`f8df3c58fae6e409db740df22c98aa699aee110b73523541a63f1bd5e23cf705`.
Their CDHashes are present in the original OS trust cache. Neither binary
was changed or re-signed.

The IM4P-wrapped caches decode to two version-2 modules with 2478 and 76
entries. The 61356-byte two-module bundle has SHA256
`7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7`,
which matches the selected OS profile's existing allow-list.

## Assembled root image

The common `unixnu.userdata` builder reads the verified local DMGs and
assembles the unchanged raw disks behind a SHNGROOT manifest:

| Region | Offset | Raw bytes |
| --- | --- | --- |
| Header and padding | 0 | 1048576 |
| OS | 1048576 | 6077580800 |
| SystemOS | 6079643648 | 3303014400 |

The resulting image is **9382658048 bytes (8.74 GiB)**, SHA256
`fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093`.
The manifest checksum, window bounds, 512-byte block units, read-only flags
and per-window hashes were independently checked. Its local path is
`e5-xnu/uefi/build/system-audit/userdata/userdata-root.img`.

NativeEmmc v8 currently publishes a GPT-wrapped seed disk. It does not yet
parse SHNGROOT or publish these two original-system windows. The complete
image remains on the host.

The e5-xnu repository includes a reproducible host-only builder/verifier
(Python 3.11 or newer). It rechecks the identity and pinned source DMGs,
then verifies the full image, header and each window independently:

```sh
python3 tools/e5-system-root.py --unixnu-source /path/to/UniXNU \
  --cache uefi/build/ipsw/21H461 \
  --manifest uefi/build/system-audit/BuildManifest.plist \
  --output uefi/build/system-audit --capacity-sectors 45834240 \
  --verify-existing
```

Omit `--verify-existing` and use a new output directory to build it from the
verified local inputs. The capacity argument bounds a host image; it does
not write to or select a physical device.

## Measured media and recovery constraints

Android sysfs and a CRC-validated physical GPT agree on these 512-byte units:

| eMMC partition | First LBA | Sectors | Capacity |
| --- | --- | --- | --- |
| cache | `0xd0a800` | `0x20000` | 64 MiB |
| blackbox | `0xd2a800` | 1024000 | 500 MiB |
| userdata | `0xe89000` | 45834240 | 23467130880 bytes |

The old cache export of `0x200000` sectors (1 GiB) extended beyond its actual
partition. v8 corrects it to 64 MiB and rejects windows beyond card capacity.
Earlier seed writes and readback checks were limited to the first 8 MiB.

Android uses userdata as its active F2FS `/data`. Replacing it with SHNGROOT
would remove that filesystem and disrupt the current Android/adb recovery
workflow. The current plan preserves that recovery path.

An SD card is present on `mmc1` at host `0x22210000`. Its existing
`e5-openwrt` ext4 partition has 15484301312 filesystem bytes and initially
reported 14682820608 free bytes, including any root-reserved blocks.
Only its superblock was read. File allocation must account for the ext4
reserve and fresh free-space measurements.

A prospective 3 GiB Data allocation plus a 1 GiB log reserve and the root
image totals 13677625344 bytes. That is a capacity plan, not an installed
layout. A preservation-first SD route needs a native SD host read path,
ordinary image-file allocation with stable extents, bounded extent-backed
block windows and independent readback before original-userland acceptance.
The existing OpenWrt files and filesystem remain intact.

## Data and memory preparation

The original launchd hash satisfies the common owner-Data fixture's source
check. A local 32 MiB APFS candidate was made from the original initial
`private/var` tree, excluding staged apps. The common offline preparation
adds empty user-management/runtime directories and verifies that existing
files and symlinks did not change. Its volume UUID is
`1DB37EE6-CDBE-493F-B292-2EE96C77EF19`.

This is an offline test input. It has no owner keybag or user records and is
not a provisioned or hardware-qualified persistent Data store. The original
system/seed files and signatures remain unchanged. The candidate is at
`e5-xnu/uefi/build/system-audit/data-candidate/data-32m.cdr`.
Its 1208400-byte packed `data-32m.dbr` also passes a full reconstruction/hash
check against the dense image; this prepares a test input, not persistence.

The board declares 2 GiB DRAM, while #57/#58 actually hand XNU only 256 MiB.
The firmware's High RAM donor is `0xc4000000..0xf0000000`, separate from
the framebuffer, LK initrd and top-of-RAM logs. A 512 MiB request fits this
donor. Flash #59 changes only `/chosen/darwinboot-memory-size`, preserving
all other ADT properties and all kernel/seed/EFI bytes to qualify this step.
It passes: loader `mem=20000000` / `owned=0x20000000`, E5EMMC root, all five
raw command statuses 0, tty ICANON and shell READ-BLOCK. See
[the report](logs/uefi-report-172018.txt) and [image record](logs/emmc-flash59.json).
This is handoff/seed coverage; high-page stress, DMA coverage and memory
pressure under original userland still need measurement.

The layout follows the common [userdata format](https://unixnu.com/guide/userdata).
Its example geometry and deployment command do not select a storage target
for this board; the measured E5 geometry and recovery requirements above do.
