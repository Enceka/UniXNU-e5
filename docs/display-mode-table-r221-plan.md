
## r253/r253b: the "338s wall" was the 300s watchdog, not a failure (Nov 6 ~20:45)

r253 journal (gen12): scratch armed, dyld cache header verified, launchd ACTIVE
(E5PROC pid=1 active=1), shared_region check_np start_address failed ×2
(= no first mapping yet → ENOMEM; dyld falls back, NON-FATAL), heartbeats to
~339s, then 689+ sequential 1MiB SystemOS reads (dyld cache full read at
~3.4MB/s delivered) cut mid-chunk by **e5-wdt-seconds=300** (unixnu.toml args;
+~38s UEFI lead-in = the uniform 338-339s across gen9-12). Zero 'rv unstable'
records — verify-retry eliminated r251's wrong-data class.

r253b = same payload, net-zero toml patch: e5-wdt-seconds 300→3600
(zz-pad absorbed 1 space). /private/tmp/e5-payload-r253b/e5-uefi.bin
sha 24a5eac5… 62926860B. Expect cache read to complete (~700s at this pace),
launchd to finish loading, SB/daemons to spawn — first REAL LS-seeding
observation window.

Journal location truth: INTERNAL eMMC boot_b (/dev/block/by-name/boot_b,
mmcblk0p37), 61MiB partition-relative: dd bs=4096 skip=15616 count=512.
(e5-boot-log-read.py needs the full 2MiB window. mmcblk1 = SD = payload+images,
NOT the journal.)
