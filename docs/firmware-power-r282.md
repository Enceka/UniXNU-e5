# R282 firmware power report and presenter diagnostics

R281 completed 8,096 SwapBegin/SwapEnd calls without a changing panel CRC.
Android recovered and all 37 protected-window chunks passed. There were no
CAFEBABE, APFS checksum/corruption or kernel panic markers. The E5CPU helper
was suppressed by e5-ca-state, so missing rows cannot locate the lost frame.

R282 separates bounded presenter messages from the suppressed state helper.
It prints validation reasons, request rectangles, surface dimensions and
allocation bounds. The pinned original slot 354 takes a state request whose
skipRequested/requested words control display+5836 and skipPowered/powered
words control display+5840. Exact original loads/stores are checked by the
build recipe. The original valid SwapEnd branch bypasses submission if +5836
is zero. This branch is distinct from the missing default-surface fallback.

After the full start chain validates retained firmware scanout, R282 uses
that original setter with {skipRequested=1, requested=0, skipPowered=0,
powered=1, changed=1}. It preserves the original client's requested state and
logs original setter transitions without replacing their results. No new
hardware power sequence or physical vblank is claimed. This follows the
NativeLegacyDisplay firmware-power contract; E5 runtime acceptance is pending.

Full C++ compilation, pinned ABI checks, kernel link/prelink and linked-vtable
inspection passed. With -fapple-kext the qualified base call uses the original
base vtable; its slot 354 remains 0xfffffff0057970cc. R282's owned slot 354 is
the logging wrapper. Payload comparison against R281 changes only kernel.macho;
the 720-second watchdog, firmware, EFI files, ADT, trustcache and seed match.
Artifacts: e5-xnu/uefi/build/sb-r282. Runtime result is not yet available.
