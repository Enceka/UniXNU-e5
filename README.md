# UniXNU-e5

UniXNU device repository for the **Rongyue E5** e-reader (Unisoc UMS9621,
8x Cortex-A53, 2 GiB DRAM at 0x80000000, GICv3, PSCI SMC).

First-bring-up configuration: the shared `ArmDTPlatform` device-tree platform
expert with `ArmCpu` (GICv3 + generic timer) and a RAM block device. Storage,
display and input drivers are intentionally absent; the kernel is reached
through the board's own UEFI firmware (see `Enceka/e5-xnu`, DarwinBoot loader).
