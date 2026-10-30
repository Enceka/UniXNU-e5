# UniXNU-e5

UniXNU device repository for the **Rongyue E5** — a 5G senior bar phone
with a removable battery (Unisoc UMS9621 / Qogirn6Lite, 2x Cortex-A76 +
6x Cortex-A55, 2 GiB DRAM at 0x80000000, GICv3, PSCI SMC, eMMC, 3.5-inch
320x480 RGB565 panel).

Current state: `ArmDTPlatform` device-tree platform expert with `ArmCpu`
(GICv3 + generic timer), a RAM block device for the seed volume, the
`NativeEmmc` driver (read-only eMMC block service with GPT enumeration),
kernel framebuffer console output and the seed shell. The kernel is reached
through the board's own UEFI firmware (see `Enceka/e5-xnu`, DarwinBoot
loader).
