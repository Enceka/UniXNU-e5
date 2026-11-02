# R109: console motion stops; original IOMFB waits on power readiness

2026-11-02. CI363 builds device revision
`e5b241f370628875d2a6fa68ab213fc7ed7accf2` on
`fix/display-submit-trace-r109`, with common UniXNU fixed at
`066c8d8315f83749d28bcae2faf3595718646be5`.
[Machine evidence](logs/display-submit-round109.json) and the
[retained console](logs/display-submit-round109-console.txt) archive this round.

The new gateway observation works: original IOMFB UserClient methods are
counted and a retained event identifies backboardd, PID 56. No selector
3/4/5 invocation reaches the observed gateway. The first concrete dependency
to investigate is display power/readiness: RequestPowerChange is unsupported
and backboardd repeatedly receives Busy from SwapWait.

## Changes and controls

The device series adds the generic IOUserClient gateway counter and an
explicit `e5-no-vc` character-painting suppression. The panel probe writes
directly to E5TW. CI363's linked frame probe has zero calls to printf and
does call the periodic gateway dump. Original IOMobileGraphicsFamily text
retains SHA256 `2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b`;
Legacy bindings, provider queries and backend behavior retain their existing
audit controls. No new surface, backlight or hardware power operation is added.

The actual R108 ADT is retained byte-for-byte at SHA256 `0fea719d…`, including
display-scale=1 and the provider contract. The established R62 frozen payload
supplies unchanged firmware, EFI executables, seed and trust cache. Only the
kernel, retained ADT and config argument change in that payload. Independent
mtools reads verify all nine files and both FAT tables agree. All 62,926,860
payload bytes match boot_b readback before reboot.

R109 uses the enabled-SB Data fixture `c1744ab4…`, already installed before
this experiment. The physical fixture is unchanged during R109. The R108
glyph/histogram ladder used headless controls in some rounds, so this is not
a claim that every historical round had an identical Data policy.

## Retained result

| Observation | R109 |
| --- | --- |
| Video console index | 1, still video |
| Character calls / suppressed | 42,413 / 42,413 |
| Retained scanout states | Only `F=1:557a87bd` |
| Default surface, selector 3 | 0 calls |
| SwapBegin, selector 4 | 0 calls |
| SwapEnd, selector 5 | 0 calls |
| SwapWait, selector 6 | At least 11,911 calls; 0 successes; `e00002d5` |
| RequestPowerChange, selector 12 | 1 call; 0 successes; `e00002c7` |
| Data / lifecycle / pitch | `d=100 h=4ff:3f g=2:0` |
| UserClient aggregate | `u=5:5:0:0`; no per-process-open claim |
| Backend guard | `B=0.0.0.0`; prior walk counters `C=1:0 D=1:1` |

The retained ring contains complete periodic snapshots, even though the
framebuffer stops changing. This closes the earlier feedback ambiguity:
ordinary kernel character painting was active, and suppressing it removes
the previously observed continuing motion in this controlled diagnostic.
CRC motion is not evidence of compositor swaps. There is no observed first
SwapBegin/SwapEnd through this gateway to attribute to an empty scene.

The error names come from the pinned XNU IOReturn.h: `0xe00002d5` is
**kIOReturnBusy**, and `0xe00002c7` is **kIOReturnUnsupported**.
Other optional queries also fail; those failures alone are not labeled causal.

## Original power and ready paths

In the original IOMFB framework, `_kern_SwapWait` at `0x1b63d25b4` sends
selector 6 with three scalar inputs. `_kern_SwapWaitWithTimeout` uses the
same selector. `_kern_RequestPowerChange` at `0x1b63d6f8c` sends selector 12
with one scalar input. Thus the gateway records map directly to these APIs.

QuartzCore's `IOMFBDisplay::is_ready` calls SwapWait at `0x18951087c` with
swap ID 0 and flags `0x80000003`. Only a successful result reaches
`Display::complete_powering_on` and the subsequent framebuffer update.
Its power-state update helper also contains SwapWait calls and a log message
`Wait Power On failed with error 0x%x. Retrying...`.
The repeated R109 call's exact user caller and scalar values are not yet
captured, so attribution specifically to is_ready remains an inference.

The original UserClient's request_power_change wrapper begins at
`0xfffffff0057779f8`. Depending on its state branches, it checks bit 0 at
service+`0x8c`; a clear bit reaches Unsupported at `...5777c74` or
`...5777d84`, instead of forwarding through service slot 229. The original
Legacy start explicitly clears that byte at `0xfffffff005787e58`.
E5Display currently validates the firmware handoff and dimensions; its owned
start does not implement a qualified replacement power contract.

This byte belongs to the framebuffer family's contract. It is **not** the
native IOService::PMinit initialized flag: both original and native PMinit
use IOService+`0x68`. A blanket PMinit call or a private-byte poke is not a
demonstrated fix.

The service SwapWait wrapper is virtual slot 176. Its high-bit mode-3/4
queries test the words at service+`0x16e0` and `0x16d0` and can return Busy
without entering a blocking wait. This fits the observed retry pattern,
but R109 does not directly sample these words or the request flags.

The next discriminating observation is the three scalar inputs of selector 6,
the requested value of selector 12, and the family support/readiness fields.
Then implement the native power transition, state publication and completion
notification together, against the already-running firmware display. Do not
fake a successful power request or readiness response to manufacture progress.
Once readiness completes, observe the first real 3/4/5 calls and their status
before moving to scene grants or physical presentation.

## Acceptance and limits

CI compile/link/prelink succeeds. All 86 resolved patches replay on the pinned
source inputs, the 13-area record checker passes, and six gateway decoder
tests pass. Full flash readback passes; fresh Android A returns, the entire
128 MiB Data hash remains `c1744ab4…`, and protected OS/SystemOS metadata is
unchanged. No panic is present in the retained current ring.

The user is away from the device and requests log-only assessment. Physical
panel visibility remains unobserved. Zero retained SB/LA exception text does
not prove either process's continuous liveness or LA repair. No visible UI
or physical frame presentation is qualified by this round.

From the companion e5-xnu checkout, decode this round again with:

```sh
python3 tools/e5-iomfb-trace-read.py \
  uefi/build/watchdog-policy-r65/board-r109/decoded/console-readable.txt \
  --require-vc-blocked --output /tmp/r109-gateway.json
```
