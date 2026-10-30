# E5 Unisoc Wi-Fi inventory and porting plan

Stock Android was read on 2026-10-30. The structured capture is
[android-wcn-20261030.json](logs/android-wcn-20261030.json); extracted firmware
and ELF modules remain local in `e5-xnu/dumps/android-wcn-private/`.
This inventory does not establish XNU Wi-Fi support.

## Observed hardware and driver

| Item | Stock Android evidence |
| --- | --- |
| WLAN driver | `/vendor/lib/modules/sprd_wlan_combo.ko`, platform driver `wlan` |
| WCN BSP | `/vendor/lib/modules/wcn_bsp.ko`, platform driver `sprd_wcn` |
| Parent / child | `/sprd-marlin3`: `unisoc,marlin3lite_sdio`; child `wlan`: `sprd,sc2355-sdio-wifi` |
| Chip | WCN boot reports `chipid=2355b001`; WLAN reports `chip_model:0x2355` |
| Transport | `mmc2`, SDIO function 1, SDR104 card at RCA `0x8800` during boot |
| Host | `sprd,sdhci-r11`, physical `0x22220000`, length `0x1000`, 4-bit bus |
| Host IRQ | DT `<0 0xca 4>`: SPI 202, absolute GIC INTID 234, level high |
| Firmware | `MARLIN3_20A_RLS2_W24.17.7`, project `sc2355_marlin3_lite`, 2024-04-28 15:41:09 |
| Board selection | Marlin3 Lite chip index 2, `IPD(1)`, `wifi_board_config.xpe.ini` |
| TX protocol | `TX_WITH_CREDIT`; `CMD_SYNC_VERSION`, `CMD_DOWNLOAD_INI`, `CMD_GET_INFO` observed |

The host's raw `reg=<0x02220000 0x1000>` is a child-bus address.
`/soc/ap-ahb/ranges` adds `0x20000000`, giving physical `0x22220000`.
The existing eMMC controller is at `0x22200000`; its block-read support does
not implement SDIO CMD52/CMD53, function enumeration or WCN packet channels.
An empty `/sys/bus/sdio/devices` after the chip powers down is consistent with
Android's logged card removal; the boot log records actual enumeration.

The combo ELF contains SC2332 SIPC and SC2355 PCIe/SIPC/SDIO aliases.
Its dependencies include `cfg80211`, `wcn_bsp`, and `sipc-core`; the BSP also
imports `trusty-ipc`. Those combined build dependencies do not identify this
board's bus. Its bound child and runtime messages identify SC2355 **SDIO**.
Ath12kWifi is not a driver for this device and is not selected in `board.toml`.

## Board resources from the live DT

`e5-xnu/dumps/live-fdt.dts` records the following WCN properties. GPIO numbers
are the offsets in the referenced controller, with their original flags;
verify vendor sequencing before using them as an electrical specification.

| Property | DT value |
| --- | --- |
| `enable-gpios` | controller `0x9e`, offset 171, flags 1 |
| `reset-gpios` | controller `0x9e`, offset 172, flags 1 |
| `m2-wakeup-ap-gpios` | controller `0x9e`, offset 128, flags 1 |
| `m2-to-ap-irq-gpios` | controller `0x9e`, offset 173, flags 1 |
| Clocks | `clk_32k`, `source`, `enable` |
| `avdd33-supply` | `LDO_VDDWIFIPA`, phandle `0x167` |
| `avdd12-supply` | `LDO_VDDWCN`, phandle `0x170` |
| `dcxo18-supply` | `LDO_VDDSIM2`, phandle `0x164` (retain vendor mapping) |
| DMA hints | `adma-rx`, `adma-tx`; host `sprd,sdio-adma` |

The parent also owns `sprd-mtty` (`ttyBT`), FM and GNSS children. Power, reset,
sleep/wake and recovery require shared ownership rather than a Wi-Fi-only
power switch. DMA flags do not prove an address width, coherency policy or an
XNU-compatible ADMA descriptor implementation.

## Firmware and calibration inputs

The actual `wcnmodem.bin` is `/odm/firmware/wcnmodem.bin`, 947120 bytes,
SHA256 `780c448afbaa614efdb1ffc71ffb6bc05b91d34f752d7994569d5c2dbf1c2a41`.
Its size matches Android's successful firmware download. The DT's
`/dev/block/by-name/wcnmodem` does not exist; `/system/etc/firmware/` printed
in BSP log messages also does not exist. The kernel firmware search path is
`/vendor/firmware,/odm/firmware`.

The selected `wifi_board_config.xpe.ini` is 7395 bytes, SHA256
`3ff60f8e6756e839cf51531d52965dc43446492d843c749da60f1ad70ac7fde6`.
Other default/AA INIs are present, but selection must follow the observed
chip/efuse logic. The capture includes all four Wi-Fi INIs, the combo/GNSS
firmware, XPE BT PSKEY/RF files and both kernel modules, with sizes and hashes.

`/vendor/firmware/tsx_data` points at missing
`/mnt/vendor/productinfo/wcn/tsx_bt_data.txt`; Android logs a default `0xffff`.
`/mnt/vendor/wcn/` is empty at capture. Per-unit address paths are
`/mnt/vendor/wifimac.txt` and `/mnt/vendor/btmac.txt`; their contents were not
included in the shareable inventory. Audit efuse reads and calibration backup
handling before deciding which inputs a port requires.

Android reports command-version differences and an `UNKNOWN_ERROR` response
for the third `CMD_DOWNLOAD_INI` section. Preserve and interpret these results
when comparing firmware behavior; successful parsing alone is insufficient.

## Source comparison and implementation order

The extracted module metadata is saved in
[android-wcn-modules-20261030.log](logs/android-wcn-modules-20261030.log).
WLAN reports `scmversion=g13f11a6756dc-dirty`; BSP reports
`scmversion=gf0c1c2c751e6-dirty`. Both advertise a 5.15.119 Android 13 kernel
ABI. WLAN DWARF paths name `wcn/wlan/wlan_combo/sc2355/{sdio,cmdevt,tx,rx,hw_param,api_version}.c`
and `common/cfg80211.c`. These paths guide source comparison; they do not
prove that another vendor branch matches the installed binaries.

The existing local `e5-linux` port identifies
[`sprd-linux-devs/linux`](https://github.com/sprd-linux-devs/linux), branch
`e5-6.18`, minimum revision `b93bbc3a36fd3c1acae0e44b93d909f7fe942182`,
as a candidate. Its WLAN path is `drivers/unisoc_platform/sprd_wlan_combo`.
Compare its SC2355 framing, command versions, INI selection and WCN bus APIs
against the stock binaries before pinning it as a porting input. Retain
license notices and review GPL reuse boundaries for an XNU implementation.

Six relevant files were read at that exact revision and hashed in
[the candidate manifest](logs/android-wcn-source-candidate.json). Its
[SDIO definitions](https://github.com/sprd-linux-devs/linux/blob/b93bbc3a36fd3c1acae0e44b93d909f7fe942182/drivers/unisoc_platform/sprd_wlan_combo/sc2355/sdio.h)
assign receive command/event, packet-log and data channels 22/23/24, and
transmit command/data channels 8/10. These are WCN bus channel identifiers,
not SDIO function numbers. Its
[board selection](https://github.com/sprd-linux-devs/linux/blob/b93bbc3a36fd3c1acae0e44b93d909f7fe942182/drivers/unisoc_platform/sprd_wlan_combo/sc2355/hw_param.c)
also handles the observed XPE INI selection. Matching these interfaces is
useful evidence for a candidate; exact vendor equivalence remains unproven.

1. Resolve power/clock/reset/wake and SDIO host resources; implement function
   enumeration, CMD52/CMD53, interrupts, transfer bounds and cache ownership.
2. Implement WCN boot/download and shared subsystem ownership. Reproduce the
   firmware version checkpoint and sleep/wake behavior using the extracted
   stock inputs.
3. Implement SC2355 command/event framing, version negotiation, credits,
   board/efuse/regulatory configuration and packet queues.
4. Audit the target iPadOS networking/802.11 ABI and add an XNU interface.
   Linux `.ko` files, cfg80211 and the Android Wi-Fi HAL cannot load into XNU.
5. Verify scan, association/key installation, DHCP and bidirectional traffic,
   then repeated stop/start and recovery. XNU support remains `planned` until
   corresponding implementation and device evidence exist.

The record follows the [UniXNU component selection](https://unixnu.com/reference/components)
and [porting evidence](https://unixnu.com/porting/records) formats; these common
documents do not supply E5 hardware facts.
