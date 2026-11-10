# R297 CPU touch canvas and reset entry control

The unchanged R296 operator repeat failed physical acceptance: six malformed
packets, only one simultaneous XNU contact, and no desktop unlock. Android
subsequently reported two concurrent slots in two complete finger sequences.
The original device logs also show SpringBoard keyboard focus buffering timing
out, leaving the automatic key sequence without a destination. See the
[repeat evidence](https://github.com/Enceka/e5-xnu/blob/ed9b72182da973f0e2ea16c631425f126ce5fef4/docs/r296-reflash-result.json).

`e5-touch-canvas=1` requires `e5-hid=1 e5-touch=3 e5-no-vc`. It selects an
exclusive CPU diagnostic at the validated firmware framebuffer: physical
`0xbe000000`, 320x480 RGB565, stride 640, exactly 307200 bytes. The ordinary
presenter does not map/write scanout or accept compositor swaps in this mode;
synthetic input and timed UI captures are disabled. No new DPU/DSI/PWM or
interrupt programming is introduced.

The sensor feeds a bounded queue independently of the original HID client.
One worker draws stable-ID trails, active rings, coordinates, error count,
Home matrix state and a heartbeat. Hardware lifts remove rings while leaving
ink. Errors clear active contacts and break paths; queue overflow is explicit
and cannot connect a later touch to an unobserved old path. This canvas proves
the raw sensor-to-scanout path; original HID/UI actions remain separate checks.

Before milestone captures 200/201/202/203/204 (ready, one contact, two contacts,
all-up after two contacts, 600 seconds), the sole writer compares every physical
pixel with its raster, then uses the existing RLE/checksum encoder. Stop disables
production, waits for the worker, clears contact state and releases resources.

`e5-touch-reset=2` performs one qualified GPIO14 high10ms/low10ms/high60ms pulse
before identification. The vendor `tlsc6x_resume_work` uses hardware reset for
the observed flash-backed 3536; A5 wake-only is its `needKeepRamCode` case.
Mode 1 retains the existing single conditional reset after four failed identity
reads. The new mode must still pass firmware/configuration and normal-mode
checks. It is an experiment, not a proven explanation of the R296 failures.
No firmware is downloaded or programmed.

`e5-touch-raw=1` retains bounded raw changed packets, including separate budgets
for declared/decoded two-contact frames, and GPIO13 level before/after reads.
The I2C transfer algorithm and strict packet decoder are unchanged. The
previously compiled `e5-touch-irq-probe=1` may collect the shared-bank census;
it still does not attach an IRQ.

Host checks cover raster bounds, inclusive edge coordinates, swapped contact
slots, lifts, cancellation, overflow and independent key state. Source/kernel
compile, link/prelink and physical canvas/reset results must be recorded before
claiming device qualification. GPIO interrupt ownership, physical display
power recovery, Home UI routing and GPU remain subsequent work.

The host checks and full native compile/link/prelink pass for device revision
`170f6df5eb980b1d8dc40b36c3ad6dfee54e7008`; the kernel SHA-256 is
`68c47267f294b9fc0a46ccf2903a4943514c0b21ef5aa328de9702f12b901ab1`. The 207-slot original HID ABI, 358-slot display
ABI and source hashes match, and the canvas has no automatic C++ initializer.
See [host evidence](logs/input-canvas-r297-host.json).

R297 runtime records 1,088 changed hardware frames, 12 complete two-contact
sequences and final active count zero. The operator confirms five targets,
drag tracking, two rings, lift removal and the Home indicator. There are also
18 malformed packets and 18 recoveries, so transport acceptance is FAIL.
The startup GPIO census reports no other enabled/pending sources across all
16 banks; this does not qualify a GIC callback. Android A recovery and all
three 37/37 protected-window checks pass. The full runtime summary is in
`e5-xnu/docs/r297-canvas-result.json`. R298 isolates strict event-gated packet
reads; see [the timing trial](input-gpio-events-r298.md).
