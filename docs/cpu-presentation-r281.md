# R281 CPU presentation candidate

R279 reached the SpringBoard UIApplication run loop and submitted 19 swaps,
but the E5 backend did not implement pixel transfer. The default-surface
fallback also failed. Comparable ports show that fallback failure alone does
not establish the cause of missing scene pixels.

The candidate implements slot 296 validation and slot 290 processing of the
original pending queue. It accepts a full-frame, untransformed, single linear
BGRA IOSurface over black, or a genuinely blank request. It validates actual
IOSurface getters, stride, allocation and descriptor length, preserves the
original read fence and waits for activation on a work-loop timer. It converts
to the existing 320x480 RGB565 buffer at 0xbe000000, then moves the actual queue
head into the empty current slot and calls original slot 295 after the stores
complete. SwapWait reads original completion state. No physical-vblank timestamp
or completed GPU renderer is claimed.

Compressed/planar surfaces and unsupported composition are rejected with a
reason. The original request owns its surface/fence through the copy. The
16ms retry does not sleep with the family gate held. Timer and mapping teardown
are explicit. Existing independent panel observation and console suppression
remain active; E5CPU offer/copy/retirement rows provide separate stages.

Contracts follow NativeLegacyDisplay 31833aa2 and EVB10's binding generator,
with original-family SHA, exact getter instructions, validation call, queue
initialization/traversal and fence field checks. The generated class retains
358 virtual entries. The final kernel links slots 290/296 to the owned code,
slot 295 to the original completion, and all seven IOSurface aliases to their
qualified original targets.

Host validation passed: C++ compilation with warnings as errors, ASan/UBSan
color/padded-row/bounds tests, complete kernel link/prelink and linked-slot
checks. The host Xcode now warns when the recorded macOS SDK is used with the
iOS target; the build used CCC_OVERRIDE_OPTIONS=+-Wno-incompatible-sysroot,
while the existing actual ARM64/iOS 17 object-format audit still ran.

Kernel SHA-256: 4b4677c1a3b954ad0f8a1925bbb4f4ba48d009b9a6582ff41bedb8c19605bafc.
Payload SHA-256: c4baec38b5716b8ce3d2b657b7374e1d5795965f5fb824640edf2d3a0bd3bf7f.
Watchdog: 720 seconds, within the user's current 900-second upper limit.

Device acceptance is pending. Require actual accepted surface/copy/retirement
and matching panel output; API success alone does not establish SpringBoard.
