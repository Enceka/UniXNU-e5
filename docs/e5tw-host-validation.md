# E5TW console capture: host and board validation (2026-10-31)

CI kernel 305 failed before compilation with `corrupt patch ...:186`.
The committed tee patch had an incorrect hunk size; the subsequent local
repair also removed closing `@@` delimiters. The replacement patch was
exported with `git diff` from the reconstructed CI baseline.

## Exact inputs

| Input | Revision |
| --- | --- |
| UniXNU | `066c8d8315f83749d28bcae2faf3595718646be5` |
| Apple XNU | `d8b80295118ef25ac3a784134bcf95cd8e88109f` (`xnu-10063.141.1`) |
| UMS9621 | `f5a9ef4457910fe1b2309d466de5a25f5c90bde7` |
| XnuPrelink | `f876e3ee62e18fc06653dd75ed7dafcfb3950ac0` |
| Device | `54347d444c71064ba254a8cb9abd787264cd007a` |
| UEFI reader | e5-xnu `b6949f4` |

Applying the 61 arch/SoC/OS prerequisites reproduces every incoming tee
blob hash. The apparent difference from pristine Apple source comes from
these prerequisites, including `zzz-boot-progress.patch`.

| File | Reconstructed pre-tee Git blob |
| --- | --- |
| `osfmk/arm/arm_init.c` | `7187c153334593049e1f4fd330912eb35cd172af` |
| `osfmk/console/serial_console.c` | `715c545ff65ea501e84b80fc7ccbec6e556924ca` |
| `osfmk/kern/debug.c` | `e9daeed833924a653c2243f46615527a3e9b4195` |
| `osfmk/kern/printf.c` | `aeac9d3a4a0ec15ce18c14d5629a745b98cb4336` |
| `pexpert/arm/pe_kprintf.c` | `3c440ec77564608647d50a85d534df528e2d88c5` |

This follows the documented [ordered patch workflow](https://unixnu.com/xnu/source-and-patches).
The checker uses this pinned UniXNU's own `unixnu.patches.plan` resolver.

## Header and persistence

The 0x6000-byte window at PA `0xFFF81F1B` contains the literal bytes
`E5TW` (`0x57543545` little-endian), a little-endian u32 next-write
position, and a 0x5FF8-byte data ring starting at +8. The old draft's
`0x57453545` spells `E5EW`; the matching UEFI reader is corrected too.

The header is unaligned. All header accesses in the kernel writer use
volatile bytes, avoiding unaligned word stores through the phase-1 Device
mapping. Each console character publishes its position, including a short
panic after a 64-byte boundary. Barriers order data before the position;
phase 0 additionally cleans both data and header cache lines.

BootShim still copies the complete window before UEFI can overwrite it.
E5BringupDxe recognizes a full-length E5TW rescue and prints the 4096 bytes
ending at the saved position, with wrap handling. Deploy the repaired
kernel and matching UEFI reader together.

## Commands and observed output

Run from e5-xnu with Python 3.11+; this machine uses
`/opt/homebrew/bin/python3`. Inputs and output are under the persistent
`uefi/build/e5tw-validation/` directory. The first two source directories
were extracted from GitHub archives at the revisions above; the SoC checkout
is pinned by this device's `feeds.toml`.

```sh
python3 tools/e5-xnu-patch-check.py \
  --unixnu uefi/build/e5tw-validation/UniXNU-066c8d8315f83749d28bcae2faf3595718646be5 \
  --prelink uefi/build/e5tw-validation/XnuPrelink-f876e3ee62e18fc06653dd75ed7dafcfb3950ac0 \
  --soc /Volumes/Projects/unixnu-e5-repos/ums9621 \
  --device /Volumes/Projects/unixnu-e5-repos/e5-device \
  --xnu-source .work/xnu-xnu-10063.141.1 \
  --output uefi/build/e5tw-validation/full-series
```

Observed: `PASS: 81 patches`. No `--recount`, fuzz or whitespace repair
was used. `patch-check.json` records every applied patch's SHA256. Use a
new output directory when repeating the check. Add
`--stop-before e5-ram-console-tee.patch` to prepare the incoming baseline;
stage that baseline, edit source files, and export with `git diff`.

```sh
python3 tools/e5-console-tee-check.py \
  --xnu-source uefi/build/e5tw-validation/full-series/xnu \
  --output uefi/build/e5tw-validation/tee-tests
```

Observed: 24 actual writer/reader cases pass, covering both mapping phases,
short output, 64-byte boundaries, ring boundaries and multiple wraps. The
last panic byte is retained. CRLF, invalid position and truncated rescue
checks also pass. The harness compiles the real source functions with
UBSan; it replaces physical addresses and privileged cache instructions.
An ARM64 compilation separately checks byte-store instructions because
UBSan does not instrument volatile Device accesses. Negative controls
restore an unaligned word store or the 64-byte position update; both are
rejected by the respective checks.

The matching reader also builds and links with the actual EDK2 platform:

```sh
. /Users/enceka/e5uefi/env.sh
build -a AARCH64 -t CLANGDWARF -b DEBUG -p e5Pkg/e5.dsc \
  -m UMS9621Pkg/Drivers/E5BringupDxe/E5BringupDxe.inf -n 8 \
  -D FD_BASE=0x80800000 -D FD_SIZE=0x400000 -D FD_BLOCKS=0x400
```

Observed: exit 0, `- Done -`; log retained as
`uefi/build/e5tw-validation/uefi-reader-build.log` in e5-xnu.

## Remaining evidence

The first board run below recovers a new, UUID-matched panic from the raw
pstore E5TW window. BootShim-only printed-tail retention is a separate
checkpoint; the direct pstore decoder provides the usable capture path.
The whole-record check initially exposed an existing shortened storage
evidence revision. Expanding `e3fe21c` to its uniquely resolved full commit
fixes that metadata error; `porting-record.py check` now passes all 13 areas.
The bytewise position is not atomic across reset, and existing concurrent
console writers are not serialized by this change. Warm-reset DRAM
survival and pstore retention of the printed tail require a board run.


## CI 306 and first board attempt

Kernel 306 completed successfully using device commit
`54347d444c71064ba254a8cb9abd787264cd007a` and the UniXNU pin above.
The final prelink SHA256 matches the downloaded 52,068,352-byte kernel:
`3e472ad5e4a4b92a3d456f8b3f8d2bd4b74efed4bf73b31fc18778d5d0f691f0`.
Its UUID is `F6A3666A-8868-3E30-BC01-5D1A69866445`. Disassembly of the
actual CI kernel confirms byte stores for all eight header bytes and the
data character, with cache clean/barrier instructions intact.

The matching complete UEFI firmware built successfully from e5-xnu
`b6949f4`; FD SHA256 is
`04c8d35c3125e68eb36fb1081fbe21a60f322be929d61b197d8f78d662e53732`.
The first experiment replaces only the kernel among the nine FAT files.
ADT, seed, original trust cache, EFI programs and verbose SystemOS args
retain their verified baseline hashes. Complete payload SHA256:
`c0d3329116d9d57efe6d44bce6edf5ac1c22b9457b6348898367571c1d1ec375`.

The device's original 64 MiB boot_b and 4 KiB misc were backed up. Its old
payload exactly matches the frozen local baseline. All 62,926,860 bytes of
the new payload were read back and matched before reboot at approximately
11:33:52 UTC. The cycle runs in the background with a 1500-second deadline
for two 600-second watchdog windows and Android recovery. Collection now
requires a changed Android boot ID, avoiding the race where ADB is still
online immediately after a reboot request. A mock-ADB regression also
checks that absent optional pstore files do not stop later-zone collection.

Private relay run 5 could not start a runner because of GitHub's existing
account Billing & plans restriction. The kernel, ADT, hardware DTB, action
logs and build-log archive were recovered locally before deleting the
public run. Local kernel staging is `uefi/build/e5-kernel-artifact-306/`;
evidence and the running cycle are under `uefi/build/e5tw-validation/`.


## Board result: panic captured

ADB returned after approximately 665 seconds. The Android boot ID changed
from `689a39a1-7672-497f-87dd-f421dfb61737` to
`06d92d70-7aac-4e6c-90ff-bf3f01001aa3`; Android completed boot on slot A.
The first collector process had already parsed its old function when the
optional-pstore-zone fix was installed, so it stopped at a missing zone.
Collection was completed directly without another reboot.

The raw 32741-byte dmesg-ramoops-0 contains `E5TW` at offset 0x1F00 and
next-write position 16307. All 24568 ring bytes can be read and rotated.
The captured kernel UUID exactly matches CI 306.

```text
panic(cpu 0 caller 0xfffffff0079e9860): mount[3] exited -- no exit reason available -- (signal 0, exit status 66 )
```

Before the panic, mount-phase-1 prints `failed to get boot-manifest-hash`
and `failed to get boot manifest hash - No such file or directory`. Missing
Data is explicitly reported as not required in environment 0. The actual
original `/sbin/mount` was copied from the read-only original OS image and
disassembled: helper 0x100004CB0 reads IODeviceTree:/chosen and the manifest
hash property, returning ENOENT on missing input; its caller takes the errx
exit path. XNU kern_exit.c confirms that a spawn-configured critical child
can trigger this panic when it exits nonzero.

Existing console hooks repeat each panic character three times. The last
4 KiB holds only the report tail; the main panic line is approximately
8.6 KiB before the ring end. Recover the whole ordered ring, preserving raw
evidence and normalizing only the identified tripled panic section:

```sh
python3 tools/e5-console-tee-read.py \
  uefi/build/e5tw-validation/board-r1/dmesg-ramoops-0.bin \
  --output uefi/build/e5tw-validation/board-r1/decoded
```

Device-repository evidence: `docs/logs/e5tw-r1-console.txt` and
`docs/logs/e5tw-r1-summary.json`. Direct raw-ring recovery resolves the
previous inability to retain the panic reason.

## Next controlled experiment: chosen image-set identity

The same kernel and complete firmware prefix are retained. Only the ADT
changes among the nine FAT files, adding two chosen properties.
`boot-manifest-hash` is the 48-byte SHA-384 of an explicit development owner
domain plus role-tagged SHA-256 digests of the actual OS and SystemOS image
bytes. The companion `owner-boot-manifest-domain` is
`E5.Bringup.ImageSet.v1`. This is development image-set metadata without
firmware personalization; the helper rejects an existing identity.

Both large input files were rehashed and matched the previously verified
21H461 pins. Every pre-existing ADT property retains its bytes. The helper
and decoder pass tests for wrap handling, invalid inputs, preservation of
ordinary log digits and refusal to replace an existing identity.

Identity: `9c4e12788051dccdc1667e61f9bf2e10c6467360ae8b26edd34016c862f39ee373187181aa308e137345c334b1207f5c`.
ADT SHA256: `a003018eed7f6130de628655b707f6e8ccc5c92c76f752b72f094a0e2e114bac`.
Payload SHA256: `ea82675bae2de666a1c8846d4b971c9e40bd3d7a1ec99c56ed36ca6da72ad4a8`.
Its complete flash readback matched before reboot at approximately
11:53:11 UTC. The normal collector returned after about 660 seconds,
verified a new Android boot ID, and completed successfully. The report is
`e5-xnu/dumps/uefi-report-200430.txt`.

## Second board result: mount blocker resolved

The ring position is 19018, kernel UUID still matches the unchanged kernel,
and boot session UUID is `20F7FD8F-0582-4955-93AB-350EC9DEE361`.
Android recovered to slot A, boot ID
`0606012d-6cbd-42b1-b968-53022bbc9df6`.

The fatal mount exit 66 is gone. Original launchd proceeds through
mount-phase-2, init-with-data-volume, fips, keybag and usermanagerd boot
tasks, then reaches tzinit. These are observed boot-task markers; the
retained ring does not qualify every task's complete functionality.
The next actual panic is:

```text
panic(cpu 0 caller 0xfffffff0079e9860): tzinit[17] exited -- no exit reason available -- (signal 0, exit status 30 )
```

Its preceding stderr says it cannot locate Data and that
`force_system_dataset()` fails with `Read-only file system`. Earlier
runtime-directory creation and log-socket operations also return EROFS.
Writable Data is now the concrete next boot requirement; no panic policy
or original userspace binary was changed for this experiment.

The new chosen metadata is retained in board.toml. The pinned common ADT
generator, using the recorded random seed, reproduces the exact
board-tested sidecar byte for byte. Both controlled runs preserve the
original read-only SD images. Device evidence is archived in
`docs/logs/e5tw-r2-console.txt` and `docs/logs/e5tw-r2-summary.json`.

The second report also observes the actual BootShim reader branch:
`E5-PREV-CONSOLE: E5TW pos=16307 tail 4096`. Thus the prior run's saved
position reaches UEFI successfully; the direct raw-ring decoder supplies
the earlier main panic line that the tripled 4 KiB tail cannot contain.
