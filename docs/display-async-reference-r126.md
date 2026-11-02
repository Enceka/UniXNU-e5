# R125 / R126: restore the native event registration ABI

R125 completed at 2026-11-02 09:50 UTC and returned to Android A. The
62,926,860-byte payload has SHA256
`509001a2703ab5879498eb89a298e279eb035cc797fb14323b80d5275e32791b`.
The exact requested CI run is 41421099587; GitHub reports number 384 and
artifact `e5-kernel-main-384`, although the local directory was named 385.
Its kernel SHA256 is
`f5c9382d1583e2b907fa44321aeeb413f3b27791a3ebf409dbb201571f927e1b`.
Fresh complete boot_b readback, independent mtools reads of all nine FAT
files, and the live retained pstore reproduce the completed R125 evidence.

All retained R125 event samples show `ak=08 asend=0 aok=0`. Selector 6 has
one successful call, while original selectors 3/4/5 still have zero calls.
The current F counter is affected by video-console painting; it is not
evidence of SpringBoard presentation.

## Pinned-source findings

The original kernelcache has SHA256
`f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`;
IOMobileGraphicsFamily text retains SHA256
`2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b`.
Addresses below are raw original kernelcache addresses (before the -16 MiB
runtime slide).

- UserClient register wrapper `0xfffffff006778460` passes kind, event-node
  pointer and a registration record in x1/x2/x3 to virtual slot 208.
  The record is `{client, reserved, callback, refcon}`.
- Slot 208 `0xfffffff006795e14` enters the native command gate. Its action
  `0xfffffff006795ec0` initializes an async reference at **node+0x18** and
  links the node at service+`0x1588 + kind*16`. Node+0 is a next pointer,
  explaining why the v28 sender sees zero for its first registration.
- Slot 336 `0xfffffff0067a023c` returns true. For kind 3 this causes the
  native registration action to skip async-reference initialization and
  select a shared-queue backend that E5 has not implemented. V29 explicitly
  selects the family's existing Mach-notification fallback with false.
- The framework's kind 3 callback is FrameInfoNotifyFunc, not vsync.
  Its first two words are a property mask and status. An empty property
  mask reports no property changes; it does not claim a rendered frame.
- Kind 5 is vsync. Original broadcaster `0xfffffff006796720` sends six
  words: three 64-bit values split into low/high 32-bit halves. The
  framework callback at `0x1b63da464` reads all six. V28's two-word payload
  would be invalid if any vsync registration reached the sender.

## V29 experiment

Preserve native registration, reference setup, list insertion and native
unregistration. Correct the slot-208 prototype to three arguments and the
slot-211 arm backend to `(kind, node, on)`. Use the original slot-210 gate
wrapper. Iterate native event lists through a nonblocking command-gate
attempt so media reads cannot wait on a display gate that needs media I/O.
Sample kind 3 with an empty change mask and kind 5 with six-word timestamps.
The `ap`, `ar` and `am` fields distinguish initialized reply ports, actual
send return status and explicit arm calls.

Host ABI derivation passes against the pinned original: 358 virtual slots,
nine original abstract leaves, unchanged constructor and family-text pins.
CI compile/link/prelink, R126 flash and physical presentation remain pending.
The acceptance criterion remains the user's observation of SpringBoard UI
on the physical panel. Successful notification delivery alone is insufficient.
