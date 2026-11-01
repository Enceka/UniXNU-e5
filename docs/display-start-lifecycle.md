# E5 Legacy start lifecycle: v12

R86 / CI341 reaches `u=1:1:0:0`: one type-0 UserClient creation succeeds.
Its later kernel-task cleanup faults on the null IORecursiveLock at `+0x1868`.
The last retained pitch count is `g=0`; greater pager depth alone does not
prove a pitch query ran. `o=0` measures handleOpen, not UserClient creation.

## Change

E5Display now calls the complete original Legacy `start(provider)`. It retains
the unchanged family initializer, command gate/workloop, event-source setup,
queue setup, slot-299 finalization and lock allocation. It does not manually
construct a partial set of locks or call unrelated numbered slots as a
replacement for the start method.

Before entering the original start, it loads IOSurface and waits at most five
seconds for IOCoreSurfaceRoot, which the family otherwise waits for itself.
The platform hook at slot 288 is now typed `bool startHardware()`. For this
firmware-scanout stage it checks the exact PE tuple: physical `0xbe000000`,
320x480, 640 bytes/row, 16 bits/pixel, no rotation. It installs only the two
mode-dimension words proven by the original slot-249 getter and reads them
back through that getter. It maps or writes no framebuffer, touches no DPU/DSI
registers and supplies no frame/power/vblank implementation. Slot 184 reports
the validated E5 panel as primary without changing `Video.v_display=0`.

The original start calls virtual registerService before returning. An owned
slot-84 override defers that call until the whole original start has returned
success and six original resource pointers have been checked. The existing
publish helper then performs the actual registration and bounded lookup.
If lookup fails after start, it calls stop before detach. Nine unqualified
backend leaves continue to panic with their slot numbers.

The original Legacy extent stays `0x18a0`; two owned boolean flags extend
E5Display to `0x18a8`. The construction probe already compares the owned
metaclass size against its compiled size independently of the original size.
Pitch reads now retain the provider's property during inspection.

## Observations

NativeSd retains `h=lifecycle:resources`, both hexadecimal. Lifecycle bits:

| Bit | Event |
| --- | --- |
| 0x001 | Entered owned start |
| 0x002 | Entered startHardware |
| 0x004 | Exact PE boot-video tuple accepted |
| 0x008 | Original mode getter confirms 320x480 |
| 0x010 | Complete original start returned true |
| 0x020 | Original resource mask is 0x3f |
| 0x040 | Original start's registration was deferred |
| 0x080 | Actual registration forwarded after readiness |
| 0x100 | Post-start readiness failed; original stop called |
| 0x200 | Owned stop entered |
| 0x400 | IOSurface loaded and IOCoreSurfaceRoot found |

Resources bits 0..5 correspond to pointers at `0x1608`, `0x1610`, `0x1638`,
`0x1648`, `0x17f8`, `0x1868`. This is a non-null readiness measurement; the
original init/start owns allocation and initialization. No pointer is fabricated.
Normal publication should retain `h=4ff:3f`.

The builder checks the unchanged family hash, original startHardware and
isMainDisplay call conventions and both uint32 dimension loads. The exact
owned dispatch is slots 0, 1, 7, 84, 86, 87, 91, 141, 184, 188, 288 plus the
nine guarded leaves; the other 338 entries remain original bindings.

Host ABI validation and CI342/run41294526511 pass at device revision `3a5d9e9`.
The final linked 358-entry dispatch differs at exactly the 20 intended slots.
Kernel SHA256 is `12f600232e4b143486317800b927db47c60699100387ff32d697ebc2dd9a9f38`.
The kernel-only r87 payload is `1764f113b90c2783f7ed722b59d8b0ae174f4d297e76b61db94adce84b7d2675`;
its ADT is the actual r86 `07e35d3d` image, retained byte for byte. R87 now qualifies the complete start lifecycle.
[Host evidence](logs/display-lifecycle-ci342.json) contains source hashes, dispatch
and independent FAT verification. The R87 procedure holds the actual
r86 ADT, firmware, seed and Data fixed, verify the final dispatch and all FAT
files, verify complete boot_b readback before reboot, then collect the retained
resource mask, UserClient results, first fault/guard (if any) and fresh Android
A recovery. Initialization qualification is separate from frame presentation.

## R87 device result

All 80 retained complete samples have `h=4ff:3f` and `a=130055` (0x1fc07).
Original start returns success, the exact boot geometry is accepted, all six
resources are non-null and publication occurs after readiness. The original
parent size remains `s=18a0`, with construction `v=1ff` and registration `r=0`.
The final sample at read #203520 has `u=2:2:0:0` and `g=2:e00002f0`: two
successful type-0 UserClients and two pitch queries returning NotFound.
`o=0` continues to demonstrate why handleOpen cannot measure UserClient creation.

The new terminal fault is the existing **slot-213 backend guard**, called by
backboardd through the original UserClient external-method path. This is the
next backend ABI checkpoint; frame presentation remains unqualified. The
r85 null-provider fault and r86 null-lock cleanup are passed on this run.

The full 62926860-byte flash readback matches. Firmware, actual r86 ADT, seed,
trust cache and other FAT files are fixed; both FAT copies match and all nine
files reproduce through independent mtools reads. Fresh Android A boot ID is
`8ba01656-7424-4136-bec9-4c737f01fa2c`. The full 128 MiB Data fixture remains
`fe0619f08c72460b5a22a4460644f265180a2f1dfdb09c9026b61999382d5fe7`.
[Machine evidence](logs/display-lifecycle-round87.json) includes the exact
artifact, counters, panic and input checks. Raw report is
`e5-xnu/dumps/uefi-report-180543.txt`; decoded retained console is under
`e5-xnu/uefi/build/watchdog-policy-r65/board-r87/decoded/`.
