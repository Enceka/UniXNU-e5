# Agent instructions

This repository owns the Rongyue E5 device record: kernel patch series,
seed userland, the NativeEmmc storage driver, board identity and the
porting record. The UEFI firmware tree, flash/collect tooling and
hardware dumps live in the separate `e5-xnu` repository. The SoC feed
lives in `ums9621`. The documentation site, component catalog and common
build tools belong to `UniXNU`.

Device identity: Rongyue E5 is a 5G senior bar phone (feature-phone form
factor) with a removable battery, 3.5-inch 320x480 ST7365P RGB565 panel,
2 GiB RAM, eMMC and Unisoc UMS9621 (Qogirn6Lite) with 2x Cortex-A76 plus
6x Cortex-A55. Keep `board.toml`, `porting.toml` and the docs consistent
with these facts.

Read the following before editing:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [repository format](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Records | [Porting record](https://unixnu.com/porting/records), `porting.toml` in this repository |
| Hardware | [Device trees](https://unixnu.com/porting/device-tree), vendor live device tree dumps in `e5-xnu/dumps/` |
| Components | Selected component README, catalog metadata, build interface, relevant source |
| Kernel | Selected OS profile, XNU source, [patch reference](https://unixnu.com/xnu/source-and-patches), ordered patch series in `patches/xnu/series` |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [UEFI firmware](https://unixnu.com/porting/uefi-firmware) |
| Verification | [Building](https://unixnu.com/building), [bring-up](https://unixnu.com/porting/bring-up) |

Working rules for this device:

- The kernel is built by the `unixnu-e5-kernel-builder` CI from pinned
  sources; device patches must match the exact upstream context and be
  validated by a flash before they are trusted. Kernel binaries are
  fragile to patch deltas beyond the validated series (see the
  configuration area of `porting.toml`); record every new patch with its
  flash evidence.
- Seed binaries are ad-hoc signed; rebuild the trust cache whenever any
  seed binary changes, keep the ADT root UUID and
  `root_match_volume_uuid` in sync with the seed volume UUID, and always
  re-run `mk-boot-payload.py` after `make-e5-payload.py`.
- Do not write to the eMMC GPT area: the boot chain restores it.
  Partition-content writes persist.
- AON peripherals (keypad, AON GPIO) need their set-mirror clock writes
  before any register access.
- Flash and collect through `tools/e5-uefi-cycle.sh` in the `e5-xnu`
  repository; runs take about nine minutes and must be backgrounded.
  Reports land in `e5-xnu/dumps/uefi-report-*.txt`.

Update the affected `porting.toml` area findings or evidence with every
code change. Write concise English for this repository. Report commands,
outputs, missing inputs and hardware observations separately.
