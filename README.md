# UniXNU-e5

UniXNU device repository for the **Rongyue E5** — a 5G senior bar phone
with a removable battery (Unisoc UMS9621 / Qogirn6Lite, 2x Cortex-A76 +
6x Cortex-A55, 2 GiB DRAM at 0x80000000, GICv3, PSCI SMC, eMMC, 3.5-inch
320x480 RGB565 panel).

Current state: the original 21H461 system boots from the E5 eMMC images with
writable Data. Native keypad/touch input and GPU compute/rendering are implemented.
The display accepts BGRA and linear two-plane `b3a8` IOSurfaces. R379 selects
SDR BGRA for the physical RGB565 panel and captures the original GPU-rendered
desktop. R390 completes Settings functional validation: the full navigation
(open, subpage, Back, scroll, Home, reopen) renders with captured frames, and
the FrameInfo count1 advertisement is proven load-bearing for the desktop
composite (R388/R389 controls with count0 never composited). Interaction
latency remains high; first working-session measurements attribute it to
synchronous queue submit waits and multi-second shader recompiles
(`docs/logs/gpu-r390-evidence.json`). Wi-Fi uses Unisoc Marlin3 Lite /
SC2355 SDIO; its Android inputs are inventoried, while XNU driver work is planned.
The authoritative progress and evidence are in [porting.toml](porting.toml),
with [Wi-Fi inputs](docs/network-bringup.md), [eMMC seed results](docs/emmc-seed.md)
and [SD OS root results](docs/sd-os-root.md).
The kernel is reached
through the board's own UEFI firmware (see `Enceka/e5-xnu`, DarwinBoot
loader).


## Reproducing the source inputs

`scripts/gpu/user-stack-sources.json` pins Mesa, MaliMetal, Metal2Vulkan and
ArmCpu, including the SHA-256 of each ordered patch. The ArmCpu lifetime changes
are carried here because the shared upstream is maintained separately.

```sh
python3 scripts/prepare-e5-gpu-sources.py --output /path/to/e5-sources
```

The output includes independent checkouts and `prepared.json` with each patched
Git tree. Existing checkouts are never edited. `--reference NAME=PATH` can reuse
local Git objects while producing an independent result.

Build the kernel from the pinned UniXNU environment using the prepared ArmCpu:

```sh
./unixnu build kernel -D /path/to/UniXNU-e5 \
  --kernelcache /path/to/original-21H461-kernelcache \
  --local-module driver:ArmCpu=/path/to/e5-sources/armcpu
```

The Metal frontend entry and bundle metadata are in `scripts/gpu/metal/`.
The bridge's Cargo lockfile, iOS deployment target, frontend flags and Mesa
options are retained in the source manifest and `scripts/gpu/build-options.json`.
The user stack uses Rust 1.98, LLVM 23 and the iPhoneOS SDK. iOS PanVK linking
requires `scripts/gpu/fix_legacy_weak.py`; it refuses strong unresolved symbols
and signs the result. Binary staging, trust-cache rebuilding and device boot
remain separate operations in the `e5-xnu` repository.

Targeted host checks include `scripts/display/tests/rgb565.cpp`,
`scripts/gpu/tests/test_mmu.cpp` and
`python3 scripts/gpu/tests/test_user_cache.py`,
`python3 scripts/gpu/tests/test_submit_wait.py --mesa /path/to/e5-sources/mesa`,
and `python3 scripts/display/tests/test_async_delivery.py`. Hardware and recovery evidence
remain in `porting.toml` and `docs/logs/`.

For native GPU UI, enable `e5-mali-g1=1 e5-metal=1` and pre-stage
`CAEnableDeepFramebuffer=false` for backboardd with the `e5-xnu` tool
`tools/e5-data-display-policy.py`. It creates a verified Data copy; install it
through the guarded image workflow with rollback and complete readback. The
preference chooses the panel's SDR compositor path without modifying Apple
binaries. Input test mode6 sends two Home presses five seconds apart; mode4
Settings navigation accepts explicit icon coordinates and a navigation-row Y,
stops when no new frame arrives, and yields to hardware touch. Keep diagnostic
input disabled during normal use.

ABI 1.7 retains the existing submission APIs and adds a bounded combined
submit/wait call. The E5 Mesa backend negotiates this API and retains the older
two-call path. Imported-memory synchronization checks each page and cache line,
while reusing contiguous physical descriptor segments. The native display
selects Mach event transport and walks live registrations under the original
command gate. Current candidate qualification is recorded in `porting.toml`.
