# E5 display provider contract: r85 / v11

R85 / CI340 publishes E5Display and reaches the original family's UserClient
external-method dispatcher. The first retained fault is in Legacy slot 188,
at `0xfffffff0057981d8`: a null cached provider at object offset `0x1640`.
The surrounding method reads `dot-pitch`, writes a 32-bit output through its
second argument, and returns an IOReturn. The unchanged family executable
hash is `2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b`.

## What the fault establishes

The original Legacy start stores a dynamic cast of its provider to
AppleARMIODevice at `0x1640` (`0xfffffff005787810..0xfffffff005787828`).
The E5 lcd nub is generic, and v8's owned start bypasses that entire method.
Calling the original start alone would still leave an incompatible provider;
fabricating a private pointer would conceal that mismatch.

The panicked task is **MobileGestaltHel**, not an observed WindowServer thread.
The retained backtrace includes the family UserClient external-method
dispatcher (`0xfffffff005778f34`), its wrapper (`0xfffffff005778194`), and
Legacy slot 188. Consequently `o=0` is not evidence of no IOServiceOpen.
It counts IOService::handleOpen, which this read-only UserClient path need not
call. R85 proves an external-method invocation; the ADT count gate and this
particular MobileGestalt request are not demonstrated to be causally linked.

## Bounded v11 repair

The generated ABI replaces opaque slot 188 with
`IOReturn getDotPitch(uint32_t *)`. The builder checks its original vtable
target, the cached-provider load and the output/return instructions before
accepting this prototype. The owned override uses `getProvider()` and reads
only a four-byte OSData `dot-pitch` property. Missing property returns the
original `kIOReturnNotFound`; null output, wrong type or wrong size returns
`kIOReturnBadArgument`; absent provider returns `kIOReturnNotReady`.
Valid outputs are initialized to zero on error. No guessed pitch or successful
hardware initialization is advertised.

The original UserClient factory remains in use. A slot-141 wrapper records
entry count, successful non-null client results, last type and full return
status. NativeSd retains `u=entries:successes:type:status` (first three decimal,
status hexadecimal) and `g=pitch_calls:pitch_status`. `o` continues to mean
handleOpen calls only. These counters are atomic observations, not a coherent
snapshot of concurrent calls. The logging buffer grows to 256 bytes so the
new fields do not truncate existing evidence.

Only owned slots 0, 1, 7, 86, 91, 141, 188 and the nine existing guarded leaves
may differ from the original 358-entry dispatch. All other bindings and late
initialization rules remain audited.

## Validation and remaining work

Host command (run against the exact cached original):

```sh
E5_KERNELCACHE=/path/to/original-kernelcache \
  python3 scripts/e5_display_build.py /path/to/prepared/tree --audit-only
```

This passes on the host; CI and a board round are required for runtime trust.
R86 must preserve the r85 ADT, firmware, seed and Data fixture, replace only
the kernel, verify the entire flash readback, and recover a fresh Android A
boot. Acceptance requires a nonzero pitch-call count with a defined return,
actual UserClient accounting and absence of the r85 null-provider panic.

This repair does **not** initialize Legacy's workloop, command gate, locks,
mode state or swap queues. Those are separate prerequisites for a working
system display. Audit remaining provider-dependent getters and qualify an E5
start/backend path using the actual RGB565 boot scanout before claiming frames
or enabling hardware operations. The nine backend guards remain in place.
