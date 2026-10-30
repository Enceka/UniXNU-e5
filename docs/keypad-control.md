# Keypad controls and tty acceptance

The original binary-layout and boot-argument-content diagnoses are withdrawn.
The detailed experiments remain in `e5-xnu/docs/keypad-mmio-investigation.md`.

With the same v8 seed held fixed, flash #35 with original kernel 242 failed;
flash #36 with only its two address-forming instructions corrected booted.
The AON gate address was `0x06490000`, corrected to `0x64900000`; the GPIO
child offset `0x00170000` translates through AON ranges to `0x64170000`.
Kernel 244 built from the corrected source also booted (#37). The control
with an `e5-keys` boot argument and kernel 241 booted (#38).

The 241/243 differences previously described as moved data pointers were
principally `N_OSO` build timestamps in `__LINKEDIT`. Defined section symbols
and segment layout did not move. This does not support a general claim that
new source changes are unsafe or that an `e5-key*` token alone kills boot.

The keypad-to-tty bridge is already implemented in
[`e5-keys2.patch`](../patches/xnu/e5-keys2.patch). It polls from the serial
keyboard path and calls `cons_cinput`. `E5KINIT` reports sane registers after
the address correction. Physical command entry still needs acceptance.

Flash #41 (kernel 244, v9 seed) is the first valid tty acceptance:
`CONSOLE-OPEN 0`, `TTY-GETA 0`, `TTY-ICANON 1`, four `EXITH|0` results and
`SH|READ-BLOCK`. See [the collected report](logs/uefi-report-083924.txt).
The source commit is `bb2e590` for the seed fixes and `331ec89` for the kernel
address fixes. The command was `sh tools/e5-uefi-cycle.sh collect` in e5-xnu.

Earlier seeds lacked `/dev`, so devfs mounting failed with ENOENT. v8 did not
check the syscall carry flag and printed errno 2 as `CONSOLE-FD 2`; this did
not prove a console was open. v9 creates the mount point and stdio, handles
errors and checks tty attributes. Canonical line input through a physical key
or serial command has not yet been accepted.

Stock Android enumerates `tlsc6x_touch`, `sprd-keypad` and `gpio-keys` in
`/proc/bus/input/devices`. Do not infer absence of touch from the bar-phone
form factor; binding, coordinates, interrupt handling and HID work remain.
