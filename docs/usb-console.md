# Native E5 USB console

The board payload enables `e5-usb=1`. The ordered
`patches/xnu/e5-usb-console.patch` adds a standalone polling MUSB CDC ACM
transport at `0x64a00000`, product `E5 XNU log`, VID:PID `0525:a4a7`.
An independent 1 MiB early kernel buffer is replayed after `arm_vm_init`.
The kernel maps and initializes its own controller/PHY; no EFI pointer or
callback survives handoff. A 1 ms worker continues polling during idle.

The shared CDC and platform sources in `scripts/usb/` are exported from
e5-xnu's firmware implementation. `shared-manifest.json` records their exact
hashes. From e5-xnu, run `tools/sync-e5-usb-kernel.py --xnu <prepared baseline
before this patch>` to regenerate after a shared-source change. Apply the
patch at the end of the current E5 series and rebuild the source kernel and
prelink through the pinned UniXNU build. `board.toml` supplies the default
payload argument; a frozen payload must be repacked with that argument too.

Normal printf/vprintf output enters the native ring through the common
buffered/unbuffered console entry, before display/UART policy. Explicit E5,
kprintf and panic output remains captured. Producers do not wait for a host;
failed try-locks count losses. A bounded panic drain never allocates memory.

Hardware trial 2026110617 passed on 2026-11-06 with macOS CDC ACM. The host
captured 76,344 firmware bytes and 131,463 native kernel bytes, including
Darwin 23.6.0, VM startup, BSD root disk4s1 and background heartbeats. Closing
and reopening the serial reader delivered another 97,397 native bytes.
All 25 observed heartbeats report `sent == written` and `dropped=0`.
The 120-second watchdog trial returned to fresh Android A. The flashed
payload hash and protected last boot_b MiB match readback.

See [the verification record](logs/usb-native-20261106.json). Full captures,
payloads and the initial boot_b/misc backups are local to e5-xnu at
`uefi/build/usb-native-20261106/accepted/`. Linux hardware, physical cable
unplug/replug and forced panic delivery remain separately unqualified.
