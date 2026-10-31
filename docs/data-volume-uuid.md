# Data identity after round 60

Round 60 (`e5-xnu/dumps/uefi-report-233521.txt`) retains 68 sampled
`rw done` lines, all with `d=-1`. The startup code reached its Data block
but did not call `mount()`: the requested volume UUID was absent. Early
publication messages no longer fit in the console ring. This alone does
not establish whether the RAM-backed device was published.

The audited device checkout starts at
`b3ac7cfff34735e4970ab4f8772bab37b00ff3c0`. CI 314 builds source revision
`a0a5f1e291b93eda422eeaa945f2c71f622eaabc`; round 61 below validates its
publication and corrected UUID on hardware.

## Device observation

Read the entire existing Data window without writing the card:

```sh
adb -s TS30520241202952 exec-out /debug_ramdisk/su -c \
  'dd if=/dev/block/mmcblk1 bs=512 skip=19034112 count=65536 2>/dev/null' \
  > <e5-xnu>/uefi/build/data-publication-check/data-card.bin
```

The 33,554,432-byte readback has SHA256
`42b81cab9b044819aa69d498b2dcf67dcefdcbb9a90651a04891c30d089b00b8`.
Its first 33,533,952 bytes exactly match the local `e5-data-raw.img`.
The NXSB checksum is valid and its first 4 KiB CRC32 is `2770965a`.
The declared APFS container is 8,182 blocks of 4,096 bytes, fitting within
the exported 32 MiB window.

All five checksummed APSB objects (transactions 2 through 6) contain
volume UUID `30ADC749-351F-4FFF-998A-20E1B683859F`. Transaction 2 precedes
the role assignment; transactions 3 through 6 have Data role `0x40`.
The driver instead requested `B5A22774-89CF-41E5-A62B-5AFA6B2710BC`.
A read-only, unmounted attachment of the corresponding local GPT image
independently reports volume `Data` and UUID `30ADC749-351F-4FFF-998A-20E1B683859F`
through `diskutil info -plist`. The attachment was detached afterward.

Android remains booted on slot A (`sys.boot_completed=1`). No card, eMMC
or boot configuration writes were needed for this diagnosis. Full host
evidence is retained in `e5-xnu/uefi/build/data-publication-check/`.

## Source changes

`E5Data::uuid` now matches the actual fixture. The new image checker reads
that same header, checks the container and volume object checksums, bounds,
UUID and latest observed Data role, and records the complete image SHA256.
It deliberately accepts only this single-volume fixture; it does not
implement APFS checkpoint recovery or prove that iPadOS can mount it RW.
On the actual readback, the old header fails with both UUIDs in the error;
the corrected header passes. Recreating the fixture requires rerunning this
check before building the kernel.

Sampled NativeSd `rw done` lines now repeat the publication checkpoint
`p`, detail `x` and UUID lookup checkpoint `q`, alongside the existing
mount state `d`. The sample interval is unchanged.

| Late checkpoint | Meaning |
| --- | --- |
| `p=disabled` | The opt-in preparation path was not entered. |
| `p=bounds-failed`, `probe-read-failed`, `not-nxsb` | The image was refused before allocation; `x` preserves the bad magic for `not-nxsb`. |
| `p=object-failed`, `init-failed`, `alloc-failed`, `buffer-failed` | RAM device or buffer creation failed. |
| `p=load-read-failed` | `x` is the failing sector offset within Data. |
| `p=loaded`, `register`, `published` | Image loaded / registering / registerService returned; `x` is its initial NXSB CRC32. `published` does not prove APFS created a volume. |
| `p=attach-failed`, `start-failed` | Device publication failed before registration. |
| `q=match-alloc`, `match-build`, `waiting`, `timeout`, `bsd-missing`, `ready` | Matching allocation, dictionary creation, wait, timeout, missing device properties, or successful UUID/device-number lookup. |

## Host verification

From the device repository:

```sh
python3 -B scripts/emmc/check-data-image.py \
  <e5-xnu>/uefi/build/data-publication-check/data-card.bin --output <report.json>
python3 -B -m unittest discover -s scripts/emmc/tests -p 'test_data_image.py' -v
xcrun clang++ -std=c++17 -Wall -Wextra -Werror \
  scripts/emmc/tests/systemos-window.cpp -o <test-windows>
<test-windows>
```

Observed: the actual fixture passes; all nine image-checker tests and the
existing window tests pass. The stale UUID, corrupted checksums, short
window, oversized container, missing volume, wrong role and invalid
transaction tests fail as expected.

The production `dataCheckpoint`, `fillDataImage`, `prepareDataDevice` and
`publishData` functions are also extracted into a host IOKit mock in
`e5-xnu/uefi/build/data-publication-check/check-publication.py`. Eleven
cases compile with warnings as errors and UBSan, exercising failure
cleanup, the complete 32 MiB copy, both publication failures, success and
the 192-byte log buffer limit. The successful preparation makes 4,097
mock card reads; no buffers/devices leak and the source image is unchanged.
This is a host control-flow check, not an actual kernel ABI build.

All 81 pinned arch/SoC/OS/device patches apply without recount or fuzz;
the report is `e5-xnu/uefi/build/data-publication-check/patch-series/patch-check.json`.

## Remaining acceptance

The pinned CI build and full-readback flash are now complete. Require a
directly retained tzinit completion plus capacity, ownership and persistence
qualification. Round 62 below now establishes actual volatile Data writes.
If lookup still times out, `p/x/q` distinguish preparation refusal from
publication or APFS enumeration failure even after the early console wraps.
The RAM-backed Data copy remains volatile; persistence is a separate task.


## Round 61: original APFS unencrypted-Data policy

CI 314 compiles, links and prelinks the changed driver. Kernel SHA256 is
`732d8c942471f902c6175302e1533333d6aafc1ef13cbaf7b689167c2cb90c08`;
UUID is `4D3FCF48-05F5-38C4-970B-5877968D1BB1`. Only `kernel.macho`
changes among the nine FAT files. Firmware and all other files are retained.
The complete 62,926,860-byte payload matches device readback, SHA256
`47b5ed9533f1eeaab84de489c589e2994a1153132573cf3464921d7b0120196e`.

The recovered raw ring includes all early publication and mount messages:

```text
NativeSd: publishing Data read-write 65536 sectors at 19034112
NativeSd: Data volume 30ADC749-351F-4FFF-998A-20E1B683859F device /dev/disk4s1
E5SystemOS: data mount /dev/disk4s1 read-write enter
panic(cpu 0 caller 0xfffffff0056d1f98): "unencrypted data volume is not allowed" @apfs_vfsops.c:2276
```

The captured kernel UUID matches CI 314. Android returns on slot A under a
new boot ID, and all 32 MiB of card Data still have the original SHA256.
This validates RAM publication and UUID selection, but RW mounting has not
returned and launchd/tzinit have not started. Evidence and complete decoded
console are `docs/logs/data-volume-round61{.json,-console.txt}`.

The actual pinned APFS binary has a built-in development path. At VA
`0xfffffff0056d1ef0`, the pre-panic check queries `/filesystems` for property
`DoNotUseSEP` using the registry-property helper at `0xfffffff00566b8d0`.
Presence returns through the diagnostic `insecure_boot_enabled` before
checking Data role `0x40` and the unencrypted flag. The helper tests object
presence, not its integer value. The disassembly and resolved strings are
retained in `e5-xnu/uefi/build/data-publication-check/no-sep-gate.txt` and
`registry-property-helper.txt`. The original APFS instructions are unchanged.

Round 62 keeps the same CI-314 kernel and adds only the u32 value 1 at
`/filesystems/DoNotUseSEP` to the existing ADT using:

```sh
python3 uefi/tools/set-e5-scratch-data.py --source <baseline>/kernel.macho.adt \
  --output <round62>/kernel.macho.adt
```

This command runs in **e5-xnu**. The pinned common ADT generator only emits
`defaults`, `options`, `product` and `buttons` beyond its fixed hardware
nodes; putting `[adt.filesystems.u32]` in board.toml would be silently
ignored. The explicit payload postprocess is therefore required. Its host
checks cover a new node, an existing node, preservation of every old
property and refusal to replace an existing declaration. The experiment
uses volatile unencrypted Data and provides no SEP or data protection.

Round-62 payload SHA256 is
`2bb4f94df9923c91760e0311705098dbe8d4b5f99a5407a197390b8ed578b71f`;
its whole payload matches readback before reboot. The result is recorded below.


## Round 62: volatile Data RW passes, then ENOSPC

Report: `e5-xnu/dumps/uefi-report-002800.txt`; full evidence and decoded
console: `docs/logs/data-volume-round62{.json,-console.txt}`. Android returns
after about 665 seconds under a new boot ID and completes boot on slot A.
Only `kernel.macho.adt` differs from round 61; all eight other FAT files,
including the CI-314 kernel, and the entire firmware prefix are identical.

All five late sampled reads report:

```text
d=100 p=published x=2770965a q=ready
```

Read counts advance from 207232 through 207744. This source sets `d=100`
only after `mount()` succeeds and `/private/var` resolves. Actual APFS
transactions now reach xids 237 through 246, compared with fixture xid 6.
CommCenter, analyticsd, apsd, containermanagerd(_system), locationd and
seserviced are active. Thus this is a real writable RAM-backed filesystem.
The entire 32 MiB card readback still has SHA256
`42b81cab9b044819aa69d498b2dcf67dcefdcbb9a90651a04891c30d089b00b8`.

No panic is captured in the retained ring. It now contains later service
activity, with APFS ENOSPC at 209-211 free blocks out of 8182. The early
tzinit task/exit lines have wrapped out of both the ring and the other
available pstore records. The original critical tzinit exit-30 panic is
not captured in this run, and the later service workload is consistent
with advancing past that boot task, but an explicit tzinit exit-zero line
has **not** been preserved. Do not label that direct checkpoint verified.

CoreData also reports the seeded `/var` root as uid 501 / gid 20 and
access errors for some services; correcting and qualifying the seed's
ownership is a separate next step. The next Data experiment should address
capacity and ownership and retain a durable tzinit completion checkpoint.
Persistent card writes and a complete user interface remain unqualified.

The validated package is frozen at `e5-xnu/uefi/build/baseline-data-r62/`.
`e5-xnu/uefi/build/current-device.json` records this current board state;
Android A is recovered and boot_b is no longer armed.
