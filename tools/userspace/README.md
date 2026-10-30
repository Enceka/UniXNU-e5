# E5 XNU userspace harness

Minimal PID1 + probes, all raw syscalls (no libSystem), built with iphoneos SDK clang/ld + ad-hoc codesign, run under the kernel trust cache (cdhash allow-list).

- init.c   — PID1 (installed as /sbin/launchd on the APFS seed): reads /sbin/init.cmd,
             spawns each line with argv split, reports via write(229) uplink; then enters
             a persistent console shell loop (system stays up, never exits).
- cat.c    — /bin/cat, initial stack supplied by start.S
- ls.c     — /bin/ls via getdirentries64 (position pointer must be non-NULL, static i64)
- clock.c  — timer probe: gettimeofday + setitimer(ITIMER_REAL) + sigsuspend
             (SIGALRM default action terminates the process: exit 0 via signal, not 42)
- e5_tc_build.py — builds kernel.macho.trustcache (single blob, entries sorted by cdhash)
- fat-kNN.sh     — clean FAT16 rebuild (kernel.macho FIRST for contiguous clusters)

## Build and install

Build ls/cat with the assembly entry. A C entry that samples SP after its
prologue reads its own frame instead of argc/argv.
The entry also aligns the working SP to 16 bytes before entering C while
passing the original argument-area pointer in x0. XNU's static string/pointer
copyout only guarantees pointer alignment; argument lengths can otherwise
leave SP at 8 modulo 16 and fault in the first C stack access.

```sh
sh tools/userspace/build-console-tools.sh out/userspace
xcrun --sdk iphoneos clang -target arm64-apple-ios15.0 \
  -fno-stack-protector -fno-builtin -nostdlib -Wl,-static -Wl,-e,_main \
  -o out/userspace/launchd tools/userspace/init.c
codesign -f -s - out/userspace/launchd
```

The APFS root must contain `/dev` before boot: XNU mounts devfs there.
Without the directory, `devfs_kernel_mount` fails with ENOENT. The old PID1
ignored the syscall carry flag and printed errno 2 as if it were fd2; that
was not evidence of an open console. v9 normalizes syscall errors, establishes
fd0/1/2 before spawning tools, and verifies TIOCGETA and ICANON before reading.

On a cloned and mounted seed, create `dev`, install launchd into `sbin`, and
install ls/cat into `bin`. Copy the same signed ls/cat to root paths `1` and `2`:
without arguments they list `/bin` and read `/sbin/init.cmd`, respectively.
Use this batch to validate both ordinary paths and numeric commands:

```text
/bin/ls /bin
/bin/cat /sbin/init.cmd
1
2
```

Regenerate the trust cache from the actual installed launchd/ls/cat/clock
CDHashes, then detach and repack the disk. Keep the cloned APFS UUID so the
existing ADT root match remains valid. The e5-xnu repository provides
`uefi/tools/packed-ramdisk.py` and `uefi/tools/repack-e5-seed.py`; the latter
checks every baseline file hash and preserves the kernel/ADT/EFI/configuration
bytes while replacing the seed and trust cache. `fat-kNN.sh` is a legacy flow.

The clock probe is not part of this four-command acceptance batch.

Uplink channel: kernel patch e5-uplink in write_internal — write(fd=229, buf, n<=256)
with boot-arg e5-uplink echoes "E5UL|<buf>" into the panic tee → survives into pstore ring.

Syscall numbers (XNU 10063, syscalls.master): exit 1, read 3, write 4, open 5, close 6,
wait4 7, execve 59, sigsuspend 111, gettimeofday 116, getdirentries64 344, posix_spawn 244,
openat 463 (46 is sigaction — classic trap).
