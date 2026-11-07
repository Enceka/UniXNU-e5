# R284 retain the accepted composition plan

R283's original IOPM callback sets requested=1, powered=1 and returns success.
The first three actual requests now reach E5's validator: BGRA (0x42475241),
zero planes, full-frame rectangles and valid backing. All are accepted.
The redundant inspectRequest call in the copy worker then fails with reason 6,
so no copy or retirement occurs and SwapBegin/SwapEnd stop at three.

The original family writes request+796 after validation: original addresses
0xfffffff006791c50..0xfffffff006791c58 and 0xfffffff006791cbc..0xfffffff006791cc4
OR per-layer bits into the word. The reference NativeLegacyDisplay validates
composition once, then its pending worker checks the actual surface and memory
layout without applying the original input mask predicate again.

R284 stores the accepted request/surface identity. Before copying, it requires
that same retained surface and checks its actual IOSurface type, linear BGRA
format, dimensions, stride, allocation and descriptor length. The validated
copy rectangle remains the fixed 320x480 full frame. Null surfaces are allowed
only by the initially accepted blank plan. Failed validation clears a matching
plan; reused request addresses receive a fresh plan. The original request owns
the surface and fence. The existing active-fence wait, mapped copy, store
barrier, queue activation and original completion are retained.

Pending masks are logged for the first sixteen copies, so hardware can verify
whether the rejected predicate was the changed mask rather than surface type.
Full compile, ABI checks, link/prelink, final vtable audit and source hashes
pass. Payload changes only kernel.macho, with the same 720-second watchdog and
preserved Data. R284 hardware acceptance and actual panel output remain pending.

Kernel: 565d2f5ac1bafbdf44c8056a9b5f8381e88c7e4319f2b36b42f5cfb37223bfd6
Payload: 19676c4c8bef31b163b1bba584cccb0da3305a62f928d516fb6bece014b627b0
