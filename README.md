# UniXNU-e5

UniXNU device repository for the **Rongyue E5** — a 5G senior bar phone
with a removable battery (Unisoc UMS9621 / Qogirn6Lite, 2x Cortex-A76 +
6x Cortex-A55, 2 GiB DRAM at 0x80000000, GICv3, PSCI SMC, eMMC, 3.5-inch
320x480 RGB565 panel).

Current state: `ArmDTPlatform` device-tree platform expert with `ArmCpu`
(GICv3 + generic timer), a RAM block device for the seed volume, the
`NativeEmmc` driver (read-only eMMC block service, GPT and APFS seed root),
kernel framebuffer console output and the seed shell. The original OS on SD
now reaches launchd/libignition; Preboot, SystemOS/dyld-cache attachment and
writable Data remain pending. Wi-Fi uses Unisoc Marlin3 Lite/SC2355 SDIO;
its Android inputs are inventoried, while XNU driver work remains planned.
The authoritative progress and evidence are in [porting.toml](porting.toml),
with [Wi-Fi inputs](docs/network-bringup.md), [eMMC seed results](docs/emmc-seed.md)
and [SD OS root results](docs/sd-os-root.md).
The kernel is reached
through the board's own UEFI firmware (see `Enceka/e5-xnu`, DarwinBoot
loader).
