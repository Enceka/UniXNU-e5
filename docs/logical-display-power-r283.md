# R283 logical display power and framebuffer captures

R282 initial hardware output reports requested=0 and powered=1 after the
original firmware-power setter returns success. The original valid-packet
branch skips actual swap submission when requested is zero. The E5 subclass
has no PM registration, while the reference driver registers two logical
states and requests state 1 through IOPM.

The pinned original setPowerState (slot 155, 0xfffffff0057972b0) passes ordinal
and controlling driver through the original command gate to 0xfffffff00578b858.
A nonnull controlling driver selects the requested half of the state pair;
the original slot-354 setter performs the write. R283 registers the two states,
joins the provider's power tree and requests state 1. Its callback forwards
to the original body and logs the result. The framebuffer remains the firmware
allocation; physical power-off and panel sleep are not implemented by this
logical registration. Stop removes the owned PM registration.

Two RLE captures sample the actual RGB565 destination after a surface copy
and its store barrier, while the presenter owns the work loop. The first
surface and a later copy at or after number 64 are captured. Each row records
its index and run lengths; a whole-frame FNV checksum covers the little-endian
pixel bytes. The host tool tools/e5-frame-read.py in e5-xnu accepts only complete
frames with the matching checksum, and emits PNGs. This proves memory content,
not physical panel scanout. The bounded captures can perturb frame timing.

Host validation: C++ warnings-as-errors compile, exact original state/IOPM ABI
checks, 358-slot class audit, kernel link/prelink, final vtable targets and
source hashes all pass. Payload differs from R282 only in kernel.macho and
retains 720 seconds. Four host decoder tests cover a complete color capture,
missing rows, corruption/oversized runs and an interrupted capture.

Kernel: a2bf9cd38ab0e72aa27a75db171489e9e09f23c33a66ffbd0bad766be9109289.
Payload: 33dedd44478caf2f2c0481606d69cd6aa08007f7d4859ebb5ba58fe78097b26b.
R283 hardware confirms the original IOPM callback updates requested=1 and powered=1. Three real BGRA surfaces pass initial validation. The pending worker rejects each copy with reason 6, so no frame capture or retirement occurs. Android recovered and all 37 protected-window chunks passed without retained corruption/panic markers. See docs/logs/runtime-r283.json. This qualifies logical power registration, not SpringBoard presentation.
