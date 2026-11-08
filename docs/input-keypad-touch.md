# E5 keypad, Home and TLSC6x input

The target remains iPadOS 17.7.11 / 21H461. The original IOHIDFamily
2031.120.4 executable hash is checked before compiling a 207-slot
`IOHIDDevice` subclass. Only qualified report/lifecycle methods are called.
The owned metaclass is registered after the original family has initialized;
the original family instructions stay unchanged.

The Linux reference is `sprd-linux-devs/linux@6b355f728578c9070da794773cb212346379f790`,
locally at `/Volumes/Workspace/e5-linux/linux-lts-e5` after the workspace move.
The vendor reference is `63b8011056a5060e274fc1dcc9051c94d9a7bab8`.
HID descriptor/lifecycle organization is adapted from
`UniXNU/ShengHidInput@71a40e76daeae1bb4127546f96ffa7339c7a06ff`;
its license is retained in `scripts/input/LICENSE`. E5 bus and sensor code
is an independent implementation of the documented hardware protocol.
[Source inventory](logs/input-source-inventory.json).

## Key mapping

The real FDT supplies 20 keys in a 4-row, 7-column matrix at PA `0x641b0000`.
The existing serial keyboard poller remains the sole reader/acknowledger of
`INT_RAW`. It forwards every state transition into a bounded 64-entry SPSC
queue. The HID workloop drains that queue, preserving a press and release
that occur within one scan. Queue overflow reconciles the latest held set;
stopping releases the reports. Release coordinates may be the idle `0x77`,
so the decoder remembers the key assigned to each of four hardware slots.

| Matrix position | Linux key | iOS report |
| --- | --- | --- |
| row 3, col 0 | KEY_MENU | Command-H, keyboard H + Left GUI; R292 candidate for Home |
| row 0, col 0 | KEY_BACK | Keyboard Escape |
| row 1, col 0 | KEY_SELECT | Keyboard Return |
| row 3, col 1 | KEY_PHONE | Keyboard Return/confirm |
| row 0, col 5/6; row 1, col 1/2 | right/up/left/down | Standard keyboard arrows |
| remaining populated cells | 0–9 | Standard digit key usages |
| row 0, col 4 | KEY_NUMERIC_POUND | Shift + 3 (`#`) |
| row 3, col 6 | KEY_KPASTERISK | Keypad multiply (`*`) |
| GPIO127, bank 7 bit 15, active low | volume down | Consumer Volume Decrement |

Keyboard usage 0x4a is a navigation Home key and is not used for iOS Home.
GPIO127's data mask, input direction and input-enable are set after the AON
set-mirror clock writes. PMIC volume-up/power keys are not connected yet.
Physical Menu now uses the standard Command-H shortcut; GUI/H release follows
the key release and other held keys are preserved. Consumer Menu remains an
independent diagnostic pulse, so this mapping change does not relabel tests.
The R292 candidate still needs its board trial and physical Home confirmation.

`e5-hid=1` enables publication. The default profile selects it; the saved
R285 payload needs the argument appended when preparing the new trial.
`e5-hid-test=1` emits one Consumer Menu press and release through the original
`handleReport` path after an event driver attaches and the default 450-second
deadline. `e5-hid-test-at=N` selects 30..600 seconds. Test reports are explicitly
labeled `selftest-home`; they are not physical-key evidence.
`e5-hid-test=2` isolates Command-H and `=3` isolates Return, each with one
down/up pair. Mode4 sends Consumer Menu, Command-H, Return, Command-H.
Before/after snapshots and the single-action modes isolate UI effects before
changing the physical Menu mapping.

## TLSC6x stages

The sensor is on I²C3 at PA **`0x22270000`**, address 0x2e. The parent AP-AHB
range translates the FDT's 0x02270000 offset. AP-AHB gate bit13 is set through
0x20001000 before controller access. The AP-I2C3 parent mux is at 0x200100a0,
its divider at 0x2001009c; the first implementation selects ext-26m and 400kHz.
GPIO13 is IRQ and GPIO14 reset at AON GPIO PA 0x64170000. GPIO clocks are enabled
before access. Working Android pinmux/reset/firmware are preserved.

| Boot argument | Operation |
| --- | --- |
| absent | No touch MMIO |
| `e5-touch=1` | Map/gate/clock qualification and controller version/pin observations |
| `e5-touch=2` | Bounded combined register reads, A3 ESD identity tag and ordinary A5 wake |
| `e5-touch=3` | Read/decode 18-byte packets and submit two-contact digitizer reports |

Transfers use a repeated START, FIFO servicing, NACK checks and a 10ms bound.
Failure telemetry retains the pre-abort status, write/read phase and elapsed
time. `e5-touch-probe=1` enables pin-eb (AON_APB_EB1 bit11) before reading
the FDT-derived pinctrl at0x642e0000, pads0xa0/0xa4/0xa8/0xac and IIC route0x20.
Pad mux is not changed. `e5-touch-reset=1` optionally applies one vendor GPIO14
high10ms/low10ms/high60ms pulse after four failed identity reads, only if the
probe qualified GPIO14's func4 pad. It is refused without that qualification.
The default preserves runtime firmware; reset of a RAM-only part may require
a later volatile reload. No firmware programming is added.
For isolated mux trials, `e5-touch-pinmux=1` selects the live-FDT GPIO13/14
func4 states; `=2` also selects the dedicated SDA3/SCL3 func1 states inferred
from the Qogirn6 convention. Only bits[5:4] change, with before/after readback;
the previous function fields are restored on stop. No IIC matrix-route write
is made. Both modes require touch>=2 and enable the pin clock before access.
Sensor reset is still refused unless GPIO14 reads back as func4.
STOP suppression is bit20 **set**; bit20 clear permits STOP. On error the
controller is reset, polling backs off 500ms, and active HID contacts are lifted.
Hardware contact IDs are 0/1 and map to stable HID IDs 1/2. Coordinates cover the observed inclusive 0..320 and
0..480 axes and normalize to 0..32767. Vanished IDs receive explicit lift
records with confidence retained. `DigitizerCollectionDispatch` groups the
fingers into one frame. The R292 adapter reads packets every16ms instead of
waiting up to500ms when a GPIO pulse is missed; GPIO IRQ routing,
hardware contacts, firmware/config identity and suspend/reset recovery remain
separate qualification tasks. No sensor firmware-flash update is implemented.

## Verification

`scripts/input/tests/protocol.cpp` passes ASan/UBSan checks for key wiring,
Home/chords/fast releases, malformed packets, stable IDs, explicit lifts,
repeated-START/FIFO ordering, NACK, timeout and STOP recovery. The mock rejects
FIFO reads before the data-ready notification. The actual source-kernel compile,
native link and prelink also pass. [HID ABI audit](logs/input-host-audit.json).

R286 passed the automated HID publication and synthetic Home down/up trial. Physical keys and finger/canvas/UI testing are
deferred at the user's request; automated device reports and physical input
will be recorded separately. Preserve the R285 Chinese Data and display path,
verify the full payload and protected windows, test Home first, then enable
touch stages individually. GPU preparation follows the display/touch plan;
these host checks do not establish its physical joint-acceptance gate.

R286 completed 15 copied/retired frames, four successful keyboard/consumer reports, no report errors/drops/stuck state, fresh Android A recovery and 37/37 postboot checks. Physical buttons and desktop effect are still pending. The next descriptor uses independent keyboard and Consumer Control Application collections. `e5-hid-snapshot=1` takes two stable, read-only scanout snapshots around the automatic Home deadline; checksum-verified PNGs are memory evidence. Temporary sensor direct mode reads the firmware/config identity and restores normal mode before packets.

R287 resource trial safely refused I2C access when a legacy-comment-derived gate at 0x20003000 read zero before/after the set write. The FDT child reg is address0/size0x3000 under syscon@20000000, so the corrected gate base is 0x20000000 (set0x20001000, reset set/clear0x20001004/0x20002004). The controller was not read on that failure; no kernel panic was observed.

The R287 snapshots100/101 decode with complete checksums and show the Chinese lock screen on both sides of Consumer Menu. The clock font is clipped at the viewport edges. R288's user test found no key/touch UI response despite 18 matrix transitions and 36 accepted reports; touch mode1 was resource-only, and the HID timer stopped progressing before the scheduled diagnostics.

R289 completes all four diagnostic actions with 16 reports and a live independent observer. Its checksummed after-snapshot shows the desktop with Chinese Settings and dock icons; at least35 display frames retire. This qualifies the sequence effect, not individual or physical Home. Touch mode2 still NACKs, so its overall identity/configuration check fails. Android A and postboot37/37 recover. R290 tests3e6a554's IRQ-qualified ACK, cached event-driver discovery and bounded queue drain without new sensor-reset/pad writes.

The corrected gate is derived with UniXNU FDT translation; a negative control rejects the old wrong base before MMIO. A separate `e5-touch-test=1` emits synthetic tap/drag/two-contact reports only after real sensor identification/packet readiness, and cancels when real contacts appear.

R291 reads3536 boot0x0240, config0x0c0e0dd0/chip0x2d5c,204 bytes with a zero
16-bit checksum and matching0x2e address. Configuration SHA256 is
`19cc5ee07b370300680e4136c0ded6225cc21ee405716da29fc8a3a196b0a820`.
The first pad read is IRQ/reset0x30 and SDA/SCL0; mux mode2 changes no function
bits and the conditional reset pulse is not needed. Pin clock and GPIO reset
initialization/settle differ from R290; their individual effects are not isolated.
All16 synthetic touch phases complete with no report error and no active contacts.
Actual fingers and the UI effect remain separate evidence.

R292 additionally rejects bad configuration checksum/address before packet mode,
using the actual R291 capture as a host-test fixture. Its synthetic tap targets
the visible Settings icon/label at274,190 and requests an additional snapshot102
after the touch window. This tests a concrete UI action instead of inferring it
from successful report return codes.

The R292 user reaches the desktop but reports no touch UI response despite
accepted hardware coordinates/moves/lifts. Initial snapshot100 is still black
at440s, and snapshots101/102 show lock screen, so450s is not a reliable UI-ready
test deadline on this boot. Real input cancels the synthetic test before it starts.

The next isolated trial uses `e5-touch-split=1`: a second instance of the same
qualified207-slot class publishes only the Touch Screen descriptor; the main
instance retains keyboard/consumer reports and exclusively owns the sensor
poller. The digitizer has its own workloop and Built-In/DisplayIntegrated/
collectionDispatch properties. Both services are registered after start.
`e5-hid-event-probe=1` reads public Keyboard/Digitizer/DebugState properties on
the independent observer, recording transducer count/grouping and LastReportTime
age. No original driver vtable or private HID event ABI is changed.

Touch input before the synthetic suite starts now defers it until all-up and2s
quiet, instead of cancelling a test several minutes in advance; input during
the suite still cancels it. Readiness requires a valid packet after recovery.
The next trial moves the keyboard deadline to550s and touch target to600s,
with snapshot102 around640s, inside the720s watchdog window.

R293's split service publishes but is selected by AppleUserHIDEventDriver,
which waits for `com.apple.driverkit.AppleUserHIDDrivers` and times out.
Its Digitizer/DebugState properties are absent; registry presence alone is
not readiness. The original kernelcache's generic native personalities do not
include a normal Touch Screen13/4 match (only a vendor-specific SafeBoot match).
`e5-touch-native=1` now adds an owned IOCatalogue personality for13/4 with
native IOHIDEventDriver/IOHIDFamily and score60000 before service publication.
It changes matching configuration only; original family code/vtables stay intact.
Driver discovery requires the public DebugState property, and probe logs include
the actual driver class to distinguish a waiting DriverKit proxy.
