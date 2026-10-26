# E5 XNU userspace harness

Minimal PID1 + probes, all raw syscalls (no libSystem), built with iphoneos SDK clang/ld + ad-hoc codesign, run under the kernel trust cache (cdhash allow-list).

- init.c   — PID1 (installed as /sbin/launchd on the APFS seed): reads /sbin/init.cmd,
             spawns each line with argv split, reports via write(229) uplink; then enters
             a persistent console shell loop (system stays up, never exits).
- cat.c    — /bin/cat, argv from sp
- ls.c     — /bin/ls via getdirentries64 (position pointer must be non-NULL, static i64)
- clock.c  — timer probe: gettimeofday + setitimer(ITIMER_REAL) + sigsuspend
             (SIGALRM default action terminates the process: exit 0 via signal, not 42)
- e5_tc_build.py — builds kernel.macho.trustcache (single blob, entries sorted by cdhash)
- fat-kNN.sh     — clean FAT16 rebuild (kernel.macho FIRST for contiguous clusters)

Uplink channel: kernel patch e5-uplink in write_internal — write(fd=229, buf, n<=256)
with boot-arg e5-uplink echoes "E5UL|<buf>" into the panic tee → survives into pstore ring.

Syscall numbers (XNU 10063, syscalls.master): exit 1, read 3, write 4, open 5, close 6,
wait4 7, execve 59, sigsuspend 111, gettimeofday 116, getdirentries64 344, posix_spawn 244,
openat 463 (46 is sigaction — classic trap).
