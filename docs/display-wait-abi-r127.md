# R127: correct SwapWait arguments and observe the native event source

The R126 notification-registration experiment exposed additional errors in
the earlier v27 event interpretation. This is a separate experiment based
on the same pinned original kernelcache and family-text hashes documented
in [R126](display-async-reference-r126.md).

Original UserClient wrapper `0xfffffff006777390` calls service slot 176
with **client in x1**, swap id in w2, flags in w3 and timeout in w4.
The old three-u32 prototype discarded timeout, treated the client pointer
as the mode and could return success without inspecting the actual flags.
Restore this four-argument ABI and forward to the original slot-176 code,
retaining its native Busy/error/success result. The telemetry now records
the actual scalar inputs and returned status.

The original slot-176 function at `0xfffffff00678d998` inspects the flags'
high bit. Modes 3 and 4 test a **32-bit** enable word at +0x16e0 and a
**32-bit** token at +0x16d0. R125's qword `m16d0=100000000` means token=0
and the adjacent +0x16d4 word=1; that adjacent word was explicitly written
by v23. It does not establish a token write, event_config invocation or
completed display readiness. Withdraw that earlier inference.

The original enable_event function at `0xfffffff006797224` is virtual
**slot 344**. It records its argument at +0x16dc, writes the complete
32-bit enable word and arms the inherited event source at +0x1638. Slots
260/261 are unrelated Legacy success/false stubs. Restore those inherited
stubs and instrument slot 344 while forwarding its exact arguments and
return value. Periodic sampling observes native token/completion changes
without writing tokens or clearing the enable word.

New state fields are `tok` (the actual u32 token), `ev` (slot-344 call
count, argument and result), and `wr` (native wait result). `W` retains the
packed swap-id/flags/timeout observation with the corrected positions.

Host ABI derivation and the 13-area porting record checker pass. CI and
R127 device evidence remain pending. The immediate hardware question is
whether native notification delivery lets userspace request a real event,
and whether the native token subsequently changes. No UI, frame-submission
or physical-presentation result is claimed by this source change.
