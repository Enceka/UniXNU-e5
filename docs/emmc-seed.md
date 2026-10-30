# E5 eMMC APFS seed acceptance

The current baseline is flash #59 (2026-10-30): kernel 262/v8, a correctly
bounded 64 MiB read-only cache window, a 512 MiB memory handoff and all five
commands exiting 0. See [the report](logs/uefi-report-172018.txt) and
[image hashes](logs/emmc-flash59.json). #58 is the same storage baseline
with a 256 MiB handoff. The earlier #55-#57 controls are retained below.

Flash #55 first starts the custom v9 PID1 from the eMMC test root.
It does not boot the original iPadOS system or provide writable Data.
The full report is [uefi-report-161217.txt](logs/uefi-report-161217.txt),
with [installed binary and image hashes](logs/emmc-flash55.json).

## Flash #55 layout and result

NativeEmmc v7, built into kernel 260, uses the original matched IOKit service
to expose the existing cache partition as a block device window:

| Value | Setting |
| --- | --- |
| eMMC controller | `sprd,sdhci-r11`, physical `0x22200000` |
| Physical capacity | 61079552 sectors of 512 bytes |
| Test window | physical LBA `0xd0a800`, length `0x200000` sectors |
| Test image | 8 MiB, containing its own GPT and APFS |
| Root | `E5EMMC`, UUID `238A9EDD-6788-4B2E-BA4E-C1F70AE25B64` |
| Runtime root device | `disk3s1` in this report (not a stable device name) |
| Kernel SHA256 | `9cd61f12978ed8e8b8b089f65ef3a1ed83c3a908bcde4f6dceb89664af5d68d6` |
| Flash #55 root SHA256 | `ddda150a9be2459752f367023a423e82a79d79eae6187ca13ea68f931d55ebc5` |
| Flash #55 payload SHA256 | `1fb99a66d080eafa3ba351e513aff77d4b36502de29668dabd94ce30cca3b35d` |

The physical eMMC GPT stays unchanged. Earlier physical GPT edits were
restored across reboot; the exact boot-chain mechanism was not isolated.
Content written inside the existing test partition persists.

Flash #54 had already mounted the same UUID, but its old v8 root lacked
`/dev` and had mismatched tools/trust cache. Flash #55 preserves kernel 260,
ADT, root UUID, firmware, menu and EFI files while installing the previously
accepted v9 signed binaries and a matching trust cache.

The new report contains `mount-complete volume E5EMMC`, `INIT-START-v9`,
`CONSOLE-OPEN 0`, `TTY-GETA 0`, `TTY-ICANON 1`, `SH|READY console` and
`SH|READ-BLOCK`. The standard four commands all have raw wait status 0:

```text
/bin/ls /bin
/bin/cat /sbin/init.cmd
1
2
```

No devfs mount failure or AMFI signature rejection is present in this boot.
The root image read back after the boot is identical to the host image.
The device returned to Android slot A with `sys.boot_completed=1`.
Physical keypad/serial command entry has a separate acceptance requirement.

## Additional diagnostic and limits

A fifth command, `/bin/cat /sbin/storage-origin`, terminated with raw wait
status `0x0a` (SIGBUS). PID1 continued into the shell. This is not a successful
exit despite the older `EXIT|0` display, which only shifts the wait status.
The command's file was added to distinguish storage contents, but the root
mount and UUID already identify the eMMC path independently.

The cat probe at commit `5697037be6ae3402f36604ad9c8c01cdac6ab705` adds
initial-stack, argc, path, open/read and EOF checkpoints on fd229. It also
returns exit 67 for a failed read instead of treating it as EOF. Flash #56
uses this cat and a regenerated trust cache while keeping the kernel/root
UUID fixed. It reproduces the SIGBUS before the first `CAT|SP` checkpoint
for the long-argument command; ordinary cat calls reach EOF and exit 0.
See [the report](logs/uefi-report-162200.txt) and
[image manifest](logs/emmc-flash56.json).

The shared assembly entry saved the original SP in x0 and branched directly
into C. In pinned XNU `bsd/kern/kern_exec.c`, `exec_copyout_strings` lays out
strings and pointer arrays with pointer-width alignment, and
`osfmk/arm64/status.c:thread_setuserstack` installs that address unchanged.
For these static executables the resulting argument pointer can be 8 modulo
16. C stack accesses require 16-byte SP alignment and can fault before the
first diagnostic call. This explains why changing argument length matters.

Commit `b7e4e3797fba39e343f9aa277133395b2248b9ce` preserves the original
argument pointer in x0, rounds the working SP down to 16 bytes, then enters
C. Both ls/cat and numeric aliases are rebuilt and signed, with a new trust
cache. Flash #57 keeps the five-command batch, kernel and root UUID fixed
to test this correction.

Flash #57 passes that control: the original argument pointer is
`0x16fdffde8`, so it is indeed only 8-byte aligned. The corrected entry passes
that pointer to C while aligning the working SP; cat reads the 65-byte
storage marker, reaches EOF and exits 0. The complete batch has five raw
zero statuses, then real tty/ICANON and shell READ-BLOCK. No child exception,
AMFI rejection or devfs mount failure appears. The root and payload match
full readback after boot, and Android slot A returns normally.

The accepted root SHA256 is
`30a10449cbb80ef69b2fa928e26797a160d1ebfea1819b089a7826e453fe3173`;
payload SHA256 is
`d788adacfad21a6ce45a5373300e3c5ee7dae195d5fca94b8e99856a08743704`.
The full baseline is frozen in `e5-xnu/uefi/build/baseline-flash57/`, and
the default local payload/FAT/manifest now match it.

Storage writes from XNU remain unsupported. v8 now reports write protection
and uses the same error-first transfer predicate for initialization, GPT
and ordinary block reads. Thirteen host error/completion cases and six
window boundary/overflow cases pass; the previous normal-read predicate
fails the error-summary-plus-completion regression case.

Raw GPT and Android sysfs show cache is only **64 MiB** (`0x20000` sectors),
not the previously exported 1 GiB. v8 corrects that length and checks it
against physical capacity. Earlier seed writes were limited to the first
8 MiB. #58 publishes 131072 sectors, mounts the same APFS root and runs the
five commands with raw status 0. Only the kernel changed from #57.

#59 changes only the typed ADT memory request: the clean sidecar is validated
before/after and all other properties retain their bytes. The loader reports
`mem=20000000` and `owned=0x20000000`, then the same root, five zero command
statuses and canonical tty pass. Both #58/#59 root and boot images match
full post-boot readback. Hardware error injection/recovery, high-page stress
and original-userland memory pressure remain separate checks.

The original system is now prepared locally as described in
[the system-volume audit](system-volume-audit.md); it has not booted on E5.

## Reproduce the seed update

Use the tools in the separate `e5-xnu` repository; do not write the physical
GPT. The archived #41 RAM seed remains the independent recovery baseline.

1. Clone the existing 8 MiB test disk image and mount the clone. Mount the
   verified `uefi/build/seed-v9-tools2/seed.dmg` read-only as a source.
2. Confirm the destination UUID, create `dev`, copy `sbin/launchd`,
   `bin/{ls,cat,clock}`, and root aliases `1`/`2` from the source. Install
   the four-command batch in `sbin/init.cmd`.
3. Verify every installed signature, collect its `codesign -dvvv` CDHash,
   and generate the sorted version-2 trust cache from those actual hashes.
   Detach both images to flush metadata; retain the binary/image manifest.
4. Read and back up the current first 8 MiB of `/dev/block/by-name/cache`.
   Confirm it matches the previous test image before replacing that region.
   Read back the full replacement and compare SHA256.
5. Run `uefi/tools/repack-e5-seed.py` against the frozen payload, preserving
   its RAM seed and replacing the trust cache. This checks every baseline
   file hash and keeps kernel/ADT/EFI/configuration contents unchanged.
6. Run `uefi/tools/mk-boot-payload.py` with the generated FAT and retained
   BootShim/firmware. Compare the rebuilt image with the repacker's image.
7. Run `tools/e5-uefi-cycle.sh flash` with that payload, verify the entire
   boot_b payload by readback, then background `collect`. Accept raw wait
   status, root identity and tty checkpoints from the resulting report.

Per-run images, backups, manifests and logs remain in
`e5-xnu/uefi/build/emmc-v9-k260/`, `flash55-k260-emmc-v9/`, and
`flash56-k260-cat-probe/`, and `flash57-k260-stack-fix/`.
The current complete baseline is `e5-xnu/uefi/build/baseline-flash59/`;
`baseline-flash58/` retains the 256 MiB fallback. Default local payload,
FAT and manifest match #59.
