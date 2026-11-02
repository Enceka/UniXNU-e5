# R128: distinguish process readiness from an idle display gateway

R127 qualifies the native power-request/event/wait path but still observes
zero original DefaultSurface/SwapBegin/SwapEnd calls. Successful calls alone
do not establish that SpringBoard is running, unsuspended, or rendering.

Add a bounded late census to the existing E5TW checkpoint. For PIDs 1..511,
retain each process while sampling its name and selected task scalar fields.
Record SpringBoard, backboardd, runningboardd, frontboardd, liveactivitiesd,
launchd and WindowServer. Active/suspended, thread counts and stop counts
are observations, not a coherent task snapshot or proof of forward progress.
An absent match applies only to the printed PID range. Do not take task/map
locks, read user memory, suspend processes or send signals from this storage
checkpoint.

Sample published IOKit services through nonwaiting matches. The service
mask bits 0..5 are IOGPU, IOAccelerator, AppleKeyStore, IOSurfaceRoot,
IOMobileFramebuffer and a name match for IOCoreSurfaceRoot. The process
mask bits 0..6 follow the process list above. Dictionaries and retained
service references are released after the snapshot.

Preserve the R127 event implementation, firmware, ADT, enabled-SB Data and
e5-no-vc control. The change adds observation only; it does not manufacture
surface calls, enable a GPU, publish a replacement KeyStore or alter scene
state. CI and R128 hardware evidence are pending. Physical SpringBoard UI
remains the acceptance criterion.

The retained R126 liveactivitiesd trap is independently located in
SessionSyncEngine Keybag.shared: MKBEventsRegister returned NULL before a
Swift force-unwrap breakpoint. One shared-cache slide matches all seven
return call sites and the original BRK instruction. MobileKeyBag looks up
AppleKeyStore, creates an IONotificationPort and registers IOGeneralInterest;
the specific failing stage remains unobserved. The service census can test
the first prerequisite without changing this userland code.
