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
| row 3, col 0 | KEY_MENU | Consumer Menu, usage page 0x0c / usage 0x40: **Home** |
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

`e5-hid=1` enables publication. The default profile selects it; the saved
R285 payload needs the argument appended when preparing the new trial.
`e5-hid-test=1` emits one Home press and release through the original
`handleReport` path after an event driver attaches and the default 450-second
deadline. `e5-hid-test-at=N` selects 30..600 seconds. Test reports are explicitly
labeled `selftest-home`; they are not physical-key evidence.

## TLSC6x stages

The sensor is on I²C3 at PA **`0x22270000`**, address 0x2e. The parent AP-AHB
range translates the FDT's 0x02270000 offset. AP-AHB gate bit13 is set through
0x20004000 before controller access. The AP-I2C3 parent mux is at 0x200100a0,
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
STOP suppression is bit20 **set**; bit20 clear permits STOP. On error the
controller is reset, polling backs off 500ms, and active HID contacts are lifted.
Contact IDs are 0/1. Coordinates cover the observed inclusive 0..320 and
0..480 axes and normalize to 0..32767. Vanished IDs receive explicit lift
records with confidence retained. `DigitizerCollectionDispatch` groups the
fingers into one frame. The first driver polls GPIO13; GPIO IRQ routing,
hardware contacts, firmware/config identity and suspend/reset recovery remain
separate qualification tasks. No sensor firmware-flash update is implemented.

## Verification

`scripts/input/tests/protocol.cpp` passes ASan/UBSan checks for key wiring,
Home/chords/fast releases, malformed packets, stable IDs, explicit lifts,
repeated-START/FIFO ordering, NACK, timeout and STOP recovery. The mock rejects
FIFO reads before the data-ready notification. The actual source-kernel compile,
native link and prelink also pass. [HID ABI audit](logs/input-host-audit.json).

Hardware trials are pending. Physical keys and finger/canvas/UI testing are
deferred at the user's request; automated device reports and physical input
will be recorded separately. Preserve the R285 Chinese Data and display path,
verify the full payload and protected windows, test Home first, then enable
touch stages individually. GPU preparation follows the display/touch plan;
these host checks do not establish its physical joint-acceptance gate.
