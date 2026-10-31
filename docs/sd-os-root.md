# SD OS root: GPT validation and APFS media matching

The original root-wait failure has two separate causes. A successful GPT scan
does not imply that APFS will probe the resulting partition. Later GPT edits
introduced an additional checksum failure before APFS could be reached.

## Evidence

- Round 24, kernel 285 with scanner tracing
  ([report](logs/uefi-report-095849.txt)): the SD entries CRC calculated by the
  actual prelinked IOGUIDPartitionScheme is `c07a08b6`; its header declares
  `f77abceb`. Scan returns an error. The same boot accepts the eMMC entries CRC
  `d9ec7402` and creates its partition.
- The v21 SD edit also shortened the APFS partition by seven sectors and left
  the backup table unchanged. Its per-sector and per-chunk golden CRCs did not
  verify the header's checksum of the complete table.
- Only SD LBA 1..33 were restored from the verified backup. Readback matches;
  the backup and APFS superblock remain unchanged
  ([record](logs/sd-gpt-repair-readback.json)). The reconstructed metadata is
  byte-identical to the original OS window. The restored host image SHA256 is
  `e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2`.
- Round 25, original-layout kernel 283 with the same trace: SD entries CRC
  `fa7a560c` matches; partition-used=1, corrupt=0, invalid=0; a non-null IOMedia
  is created. There are still no SD reads at the partition base. This disproves
  the earlier inference that the original GPT was rejected.

## APFS filter in the actual kernel

Inspection uses kernel 285 SHA256
`76130c8ba37112b7012ae2f9a9c6cbd0d15dd4f51bc6ab90d7147d6bb519260c`,
containing the pinned iPadOS 17.7.11 APFS executable.
AppleAPFSContainerScheme::probe at `0xfffffff0056825cc` calls a media filter
at `0xfffffff00566be34` from `0xfffffff005682684`. A true result branches to
probe failure at `0xfffffff005682688`, before opening or reading the media.
Unless an earlier exception applies, the filter rejects an `Ejectable` or
`Removable` property equal to kOSBooleanTrue.

NativeSd originally reported both properties true. NativeEmmc reports both
false. The SD card is now treated as a fixed boot card by this driver, which
does not implement hot removal. v21 had already changed both properties, but
the invalid GPT edit masked that change.

The eMMC test image really does contain GPT. `emmc-root.dmg` has a valid table
and APFS partition at LBA 40; `seed.dbr` is a separate RAM-disk wrapper. Earlier
claims that the successful eMMC boot bypassed GPT confused these two inputs.

## Current implementation

NativeSd v22 restores the original OS reference CRCs and keeps the fixed-card
media properties. It checks the GPT header checksum and streams all 16384
entry bytes through one CRC state before logging `gpt ok`. Invalid checksums
fail initialization. The e5-xnu report checker requires both complete checks,
the expected OS volume mount and a launchd attempt.

Host tests cover a known CRC vector, streaming across chunk boundaries, a
16 KiB zero array, corrupt entries, invalid backups, truncated APFS extents
and missing or mismatched board checksum evidence.

Round 26, source-built kernel 286, executes the original launchd/libignition
and prints `hello from launchd.1`. Preboot lookup then fails with entitlement
error 1, followed by missing `/usr/lib/libSystem.B.dylib` and no dyld cache.
This is beyond GPT/root-device discovery. The 12 KiB XNU trace window lost
the earlier probe/mount records. Request sampling and a 24 KiB window retain
these records in the repeat below. The window still ends at the same offset
inside the existing pstore zone.

## Board acceptance: round 27 PASS

Source revision `012c1e33250c853697c6275376947dbdb34841b7`, CI kernel 289,
unmodified source-built executable. The
[report](logs/uefi-report-104606.txt) retains:

- Header CRC `d9e4279c` and complete entries CRC `fa7a560c`, both matching.
- Four reference sectors, sixteen repeats, default-speed mode and the eMMC
  v8 service regression marker.
- `mount-complete volume DawnUpdate21H461.J120J121J207J208OS` on `disk3s1`.
- `/sbin/launchd` loading and `hello from launchd.1` in libignition.

`python3 tools/e5-sd-check.py dumps/uefi-report-104606.txt` in e5-xnu returns
[PASS](logs/sd-gpt-round27-check.log). The complete boot payload matches device
readback SHA256 `9c685162a2a5efe9ee43b7a52f49083a24fb45353e0f19a495b158e8bb8e20d4`;
SD primary/backup GPT and the APFS superblock remain unchanged
([record](logs/sd-gpt-round27-readback.json)). Android slot A recovers normally.

This qualifies SD root mounting and original launchd/libignition execution.
Complete iPadOS boot remains blocked by Preboot entitlement/role lookup and
SystemOS/cryptex/dyld-cache integration. No writable Data or original-system
service acceptance is claimed.
