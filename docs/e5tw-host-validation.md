# E5TW console tail: host validation (2026-10-31)

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
| Device | `9aafc5b` plus the E5TW repair in this working tree |
| UEFI reader | e5-xnu `40f5590` plus the matching magic/length check |

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

These are host results. The repaired kernel has not yet been compiled by
CI or flashed. No new panic text or hardware success is claimed.
The whole-record check initially exposed an existing shortened storage
evidence revision. Expanding `e3fe21c` to its uniquely resolved full commit
fixes that metadata error; `porting-record.py check` now passes all 13 areas.
The bytewise position is not atomic across reset, and existing concurrent
console writers are not serialized by this change. Warm-reset DRAM
survival and pstore retention of the printed tail require a board run.
