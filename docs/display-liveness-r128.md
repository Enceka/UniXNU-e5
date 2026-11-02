# R128: distinguish process readiness from an idle display gateway

R127 qualifies the native power-request/event/wait path but still observes
zero original DefaultSurface/SwapBegin/SwapEnd calls. Successful calls alone
do not establish that SpringBoard is running, unsuspended, or rendering.

Add a bounded late census to the existing E5TW checkpoint. For PIDs 1..511,
retain each process while sampling its name and selected task scalar fields.
Record SpringBoard, backboardd, runningboardd, frontboardd, liveactivitiesd,
launchd and WindowServer. Active/suspended are observations from the
available task getters, not proof of forward progress.
An absent match applies only to the printed PID range. Do not take task/map
locks, read user memory, suspend processes or send signals from this storage
checkpoint.

CI388 rejected the first diagnostic because exported IOKit headers leave
`struct task` incomplete. Remove direct thread/stop-count field access;
the revised census uses the available state getters. CI388 was not flashed.

Sample published IOKit services through nonwaiting matches. The service
mask bits 0..5 are IOGPU, IOAccelerator, AppleKeyStore, IOSurfaceRoot,
IOMobileFramebuffer and a name match for IOCoreSurfaceRoot. The process
mask bits 0..6 follow the process list above. Dictionaries and retained
service references are released after the snapshot.

Preserve the R127 event implementation, firmware, ADT, enabled-SB Data and
e5-no-vc control. The census adds observation only; it does not manufacture
surface calls, enable a GPU, publish a replacement KeyStore or alter scene
state. CI and R128 hardware evidence are pending. Physical SpringBoard UI
remains the acceptance criterion.

## Correct the optional FrameInfo capability response

Original selector 74 has one scalar input, two scalar outputs and a 64-byte
structure output. Its wrapper at raw `0xfffffff00677a718` enters
`0xfffffff006778688`, which calls slot 213 with index, count pointer, key
buffer and transport pointer. The transport is cached at **client**+0x3c8,
not service+0x3c8. The previous count=1/transport=1 stub left the key
uninitialized and advertised a shared queue, contradicting slot 336's Mach
fallback. A successful return from that stub did not qualify the ABI.

Use the typed four-argument signature, zero every output and return
Unsupported for this optional capability until actual frame reports are
implemented. This matches the no-FrameInfo firmware/CPU path in
RigoLigoRLC/unixnu-nativemobiledisplay at
31833aa2ad8b1faae9c3fa9752830d4271d46979. Its transport-zero and shared-queue
semantics are independently consistent with the pinned E5 family wrapper.
Runtime compatibility of the optional rejection remains to be tested.

The retained R126 liveactivitiesd trap is independently located in
SessionSyncEngine Keybag.shared: MKBEventsRegister returned NULL before a
Swift force-unwrap breakpoint. One shared-cache slide matches all seven
return call sites and the original BRK instruction. MobileKeyBag looks up
AppleKeyStore, creates an IONotificationPort and registers IOGeneralInterest;
the specific failing stage remains unobserved. The service census can test
the first prerequisite without changing this userland code.
