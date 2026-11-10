# R299 GPIO polling cadence control

R298's live capture has consistent masked GPIO event/read accounting and
ordered software observation/start/end timestamps. It suppresses packet
reads during long idle intervals. Two malformed packets remain, both with
GPIO13 high before/after the read and RIS13 pending after it. This correlation
does not establish a sensor update race or stale-event cause. The original
18-byte register read matches the vendor TLSC6x path; the strict decoder
continues to reject invalid headers/slots.

`e5-touch-event-ms=1..16` sets the HID workloop timer rearm delay after strict
GPIO event initialization. Default and invalid values use 16 ms, as do all
non-event modes and failed/stopped initialization. R299 selects 2 ms. The
period includes handler execution, I2C and diagnostic output; it is not a
guaranteed physical-edge response bound. The initial discovery tick remains
16 ms. `gpio-cadence` logs the actual selected delay and scope.

This control retains masked IE13, pin-only pre-read acknowledgment, strict
RIS13 authorization, startup/reset behavior, recovery delay and packet/HID
semantics. It installs no GIC callback and adds no unconditional I2C read.
Key queue draining shares this timer; timestamps/deadlines remain absolute.
Only the optional event mode changes cadence.

Verify build/link/prelink and original HID ABI, then a complete isolated
trial with an idle interval, real single/two-contact movement and all-up.
Record transport failures independently from correct gate/timestamp checks.
An idle-only run cannot qualify event delivery. Compare error incidence only
for matched physical workloads; raw counts from R297/R298 differ in activity
and read policy. GIC delivery is a subsequent change.

Native compile/link/prelink, original207-slot HID ABI, frozen source hashes
and all15 porting record areas pass. Kernel SHA-256 is
`c66b289989861b038c6176106b46c8c24965ba0e64687f80992dd01c63b8612d`.
The candidate uses the default-policy600s watchdog. Only the HID timer caller
and TLSC6x cadence option differ from R298's frozen source manifest.
R299 completes with 301 valid packets, zero errors/recoveries, 274433 GPIO
polls and 274132 skips. GPIO accounting, idle suppression, 65 sampled timing
orders and actual 2ms selection pass. Hardware has 232 changed frames, three
complete single-contact drags, one complete two-contact sequence and final
active zero, with no error-terminated sequence. The operator confirms rings
track fingers and disappear on lift. 244 HID reports have no failure/drop.

Canvas frames 200..203 pass the original checksums. Optional frame204 was not
captured; the canvas starts after the kernel watchdog, so its 600-second
post-start milestone is not required by this 600-second configuration. Full
payload readback, all three37/37 protected-window checks, Android A return
and retained panic/integrity checks pass. See [runtime evidence](logs/input-gpio-cadence-r299.json)
and `e5-xnu/uefi/build/sb-r299-gpio-cadence`. This is CPU canvas/packet-path
acceptance, not original SpringBoard/GIC qualification.

The selected ArmCpu9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b exposes attach/unmask
and callback count but no mask/detach/inflight wait. Add these lifecycle
operations before GPIO callback/deferred-work delivery, then disable the
canvas and verify physical actions in original SpringBoard. No GIC callback
is installed by this revision.
