# Data identity after round 60

Round 60 (`e5-xnu/dumps/uefi-report-233521.txt`) retains 68 sampled
`rw done` lines, all with `d=-1`. The startup code reached its Data block
but did not call `mount()`: the requested volume UUID was absent. Early
publication messages no longer fit in the console ring. This alone does
not establish whether the RAM-backed device was published.

The audited device checkout starts at
`b3ac7cfff34735e4970ab4f8772bab37b00ff3c0`. The changes described below
are local source changes; no new CI kernel or boot result is claimed.

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

Build and prelink the changed driver through the pinned kernel CI,
repack and fully read back the payload, then use the normal backgrounded
`e5-uefi-cycle.sh` flash/collect workflow. Require the real UUID device,
successful RW mount on `/private/var`, and tzinit completion in that boot.
If lookup still times out, `p/x/q` distinguish preparation refusal from
publication or APFS enumeration failure even after the early console wraps.
The RAM-backed Data copy remains volatile; persistence is a separate task.
