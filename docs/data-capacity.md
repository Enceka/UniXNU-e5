# Data capacity and boot-task evidence

Round 62 mounted the 32 MiB RAM Data volume and served real writes, then
later services reached ENOSPC. The seed's root inode also retained the
host uid/gid 501:20. Early tzinit text wrapped out of the console, so a
successful critical exit could not be directly qualified.

Source `519a72775fad6b6fd71c1ffbb2b68e41c4838d2e` addresses these three
items for round 63:

- Increase the Data window to 262144 sectors (128 MiB), retaining UUID
  `30ADC749-351F-4FFF-998A-20E1B683859F` and base LBA 19034112.
- Before launchd, set the mounted Data root to uid/gid 0:0 and mode 0755.
  Read back uid/gid and require success before reporting `d=100`.
  This changes only the volatile mounted copy; it preserves child owners.
- Record tzinit's raw wait status and pid after the accepted exit thread
  commits `P_LEXIT` and `p_xstat`, before `proc_prepareexit`. Sampled SD
  reads retain this as `t=pid:hex_wait_status`: `-1:ffffffff` is unseen;
  a positive pid with status `0` is direct normal-exit-zero evidence.
  Nonzero codes/signals retain their original wait encoding. The hook
  requires the explicit SystemOS development boot mode and changes no
  exit, signal or critical-process panic decisions.

The new patch is `e5-data-progress.patch`, last in the device series.
All 82 pinned patches apply without recount or fuzz. Eight actual-source
host cases pass for name/mode filtering, zero/nonzero exit, signals and
post-commit hook placement. Existing image and window checks also pass.

## Image and installation

An independent copy of the original sparsebundle was resized to 128 MiB
with `hdiutil resize`. The original volume UUID and seed objects remain.
The APFS store was extracted as a raw 128 MiB image. Host disk inspection
reports 132014080 bytes free, confirming that the APFS container grew.
The checksummed image passes the driver-header UUID and size checker:

- Raw SHA256: `5f63bfbc8e4342cb2bfd1dab8d1c8c14e4b6c514ec367cffd086792150d9616b`.
- Initial NXSB CRC32: `bb83a697`.
- Size and container size: 134217728 bytes.

In e5-xnu, `tools/e5-data-media.py plan/install` validates the raw fixture
against `SystemOSWindow.h`. Installation verifies the SD CID, capacity,
unmounted state and old 32 MiB fixture SHA256; backs up all 128 MiB of the
destination, including the extra 96 MiB; then writes only that window.
The complete new window matches readback. OS GPT/APFS and SystemOS head
and tail metadata are unchanged. The eMMC GPT is outside this operation.

The exact backup and restore commands are retained in
`e5-xnu/uefi/build/data-capacity-r63/sd-before/plan.json`.
The R62 kernel exports only 32 MiB: restore the saved Data window before
using that old kernel/payload again. The new image and new kernel are a
paired test input. Android A recovery remains available independently.

## CI and round 63

CI 315 compiles, links and prelinks the pinned source successfully.
Kernel UUID: `D79DCFE4-737E-3041-A570-388C447577D9`.
Kernel SHA256:
`ec520cfe0b5cc96f0da68d0864abf7402b5883d289ac93bc13c16b0b112e1f81`.

The R62 payload is retained except for `kernel.macho`; the ADT retains
the verified `/filesystems/DoNotUseSEP` declaration and 512 MiB memory
request. Firmware and the other eight FAT files are unchanged. All
62926860 payload bytes match flash readback before reboot, SHA256
`a402895963b3f0c4c244aa9d6a594838f47123de5a5923b41c9390a9c3be796a`.

The background board cycle uses the normal `e5-uefi-cycle.sh` workflow.
The following rounds now verify mounted Data/root ownership and a
positive tzinit pid with wait status zero. RAM Data remains volatile; persistence and
the complete UI are separate qualifications.


## Round 63: tzinit is directly verified

`docs/logs/data-capacity-round63.json` and its `-console.txt` retain
`d=100 p=published x=bb83a697 q=ready t=17:0`. This is the committed wait
status from the real tzinit exit thread, not an inference from later
services. Data root ownership is verified before d=100. The recovered
kernel UUID matches CI 315 and Android A returns under a fresh boot ID.
The complete 128 MiB card Data readback is unchanged.

The next critical failure is watchdogd[95], OS_REASON_SIGNAL (namespace 2),
SIGTRAP (subcode 5). Its core reaches offset 127455232 and consumes almost
all Data, generating ENOSPC and overwriting the original diagnostic.
This is a core-output capacity problem; it does not show that normal
service data needs more than 128 MiB. The compressor is at 0% in the panic.

## Round 64: separate the crash from its core output

The entire CI-315 boot payload is unchanged. In a new Data fixture copy,
the original empty `cores` directory is retained as `cores.disabled` and
`cores` is a symlink to `/dev/null`. This intentionally makes the normal
core-file pathname fail as ENOTDIR; the kernel's core, signal and critical
panic policies are unchanged. No OS or SystemOS executable is altered.

`docs/logs/data-capacity-round64.json` records the same watchdogd SIGTRAP.
Core opening now fails with error 20 in 0.086 seconds. The retained ring
has no ENOSPC, and 158 complete late checkpoints still show Data d=100 and
tzinit 17:0. The original watchdogd assertion string/user PC remains absent
from the console; no physical connection-branch attribution is claimed.
The whole card Data image again matches its preboot SHA256.

## Round 65: root-owned launchd policy experiment

The original watchdogd and its original launchd job were extracted from
the read-only pinned OS image. The job requires IOWatchdog and has no
ForceEnableHack. Its native connection path precedes the diagnostic
no_user_checkin boot-arg path, so that flag cannot skip a missing service.

The pinned common tooling executes original watchdogd connection branches
and the launchd policy loader offline under Unicorn with explicitly
substituted OS APIs. Missing-service/open-error cases trap; a standard
root-owned disabled.plist can disable com.apple.watchdogd without disabling
another label. These are host controls, not physical branch/driver proof.

A new Data fixture contains only the policy entry com.apple.watchdogd=true
and its disabled.migrated marker. The Data root and both policy inodes are
stored as uid/gid 0:0; policy files are 0644. The host lacks root chown
privileges, so `e5-xnu/tools/e5-data-root-owners.py` adjusts only these guest
inode ownership fields in a raw image copy, recalculates the containing
APFS node Fletcher64 checksums, and independently mounts the result
read-only with owners enabled to verify the actual stored attributes.
It reproduces the staged image byte for byte. The source image stays
read-only; no host owner privilege is acquired.

The no-core path is retained. The CI-315 kernel, ADT and complete payload
stay identical. The PMIC's 600-second test/recovery window is unchanged;
this is an explicit temporary per-service development policy, not native
IOWatchdog support. Data SHA256 is
`c1744ab42d3baac3baaf50742afbf64fc51159d13620a2f5fef4e2655fe4ec47`,
initial NXSB CRC32 `ab20a450`. Whole-window and full-payload readbacks pass.
R65 results follow below. The policy files are mode 0644 and the original
watchdogd job has no ForceEnableHack. The guest-ownership tool reproduces
the staged image byte for byte.


## Round 65 result: original SD system development baseline

`docs/logs/data-capacity-round65.json` and its complete decoded console
record 248 full sampled SD rows, all with
`d=100 p=published x=ab20a450 q=ready t=17:0`. Read counters advance from
219776 to 251392. The boot payload is byte-identical to rounds 63 and 64;
only Data configuration differs. Full payload and Data window readback
checks passed before reboot.

The retained final ring has no panic, watchdogd critical exit or ENOSPC.
Original later services continue through the fixed hardware-watchdog test
window. Android completes recovery on A under a changed boot ID, and the
entire 128 MiB card fixture remains byte-identical. The actual execution
therefore qualifies volatile Data RW/root ownership and tzinit exit zero,
and removes the critical watchdogd stop under the explicit per-job policy.
It does not qualify native IOWatchdog support or full system health.

Late logs still report audiomxd failing to connect to AppleKeyStore,
missing Bluetooth DT service/address, and noncritical liveactivitiesd
crashes with the corpse limit reached. No UI acceptance is claimed. SEP,
persistent Data and native watchdog implementation remain separate work.

The paired boot payload and Data image are frozen at
`e5-xnu/uefi/build/baseline-data-r65/`; current board state is in
`e5-xnu/uefi/build/current-device.json`. All earlier image/window backups
and restore commands are retained. Reverting to the old 32 MiB-exporting
kernel requires restoring the corresponding old Data fixture as well.
