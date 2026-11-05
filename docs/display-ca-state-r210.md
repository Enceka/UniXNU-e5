# Display census repair and 21H461 mode audit

2026-11-05, Rongyue E5, iPadOS 17.7.11 build 21H461.

The R206-R208 diagnostic extension identifies native versus virtual IOMFB
connections by scanning bounded display-object words for conn+0x880 pointers
inside the UUID-qualified IOMFB text range. It records connection slots,
kind-5 callback state, SignalType (`tt`), ModeInfo count (`mc`), and the
server's disabled/shutdown/timer state. The E5Display line also records the
service registry ID. All user-memory access is read-only through
`vm_map_read_user` on the retained backboardd task.

R208 panicked before publishing its new fields. The exact kernel's
`e5_ca_probe_dump` reserved 0x1a60 bytes after 0x60 saved-register bytes.
Its stack-check helper faulted on the guard page at
`0xfffffff007976a58`, called from `0xfffffff0079933a4`.
This is diagnostic stack exhaustion, not evidence for a display mode failure.

Worker and dump snapshots now live in separate static scratch areas. The
worker uses its existing request busy flag; a separate compare/exchange
serializes dump scratch. Generation checks still discard interrupted copies.
The rebuilt dump reserves only 0x120 bytes. This repair changes no display
state, notification behavior, current-mode response, or watchdog deadline.

R210 also corrects the render-gate read. The original code at
`0x18951de78..0x18951de9c` tests display+0x28 for nonzero, then reads bit 5
of display+0x30. The R208/R209 probe instead read the unrelated address
`[display+0x28]+0x30`; its g28 value must not be interpreted as that gate.

The 600-second PMIC load already is correct: LOAD_HIGH=0x012c, LOAD_LOW=0 at
32768 Hz. R208's bootloader log contains one B attempt from 11:38:58 to the
rollback boot at 11:49:39. B tries=2 is the qualified one-attempt setting;
tries=1 triggers LK rollback. The e5-xnu collector's old 475-second host
timeout was shorter than this run. It now waits up to 780 seconds and offers
`harvest` to recover existing pstore without rebooting.

The static mode audit qualifies the complete QuartzCore and IOMFB text hashes.
It establishes these limits on the previous root-cause hypothesis:

- `display+0x66b8` counts the ModeInfo hash map. It is separate from ModeSet.
- SignalType=1 obtains top-level ColorElements/TimingElements from the service
  registry properties. Missing arrays, or <=1 color and <=1 timing, explicitly
  produce a ModeSet containing the -1 sentinel. Thus md=-1 alone is not proof
  that mode construction caused missing surface submissions.
- The mc>=2 comparison is in power-on multi-mode reselection. It is absent
  from the early `render_for_time` entry gates.
- IOMobileFramebufferGetDigitalOutMode requests GetBlock(0x40), 44 bytes;
  color ID is at +4 and timing ID at +8. A success-only sel-79 implementation
  cannot supply an explicit current ID pair. The other block fields have not
  been qualified, and no genuine iPad registry dump is available.

The reproducible audit and raw instruction evidence live in the e5-xnu
repository: `tools/e5-ca-mode-audit.py`,
`docs/display-mode-contract-21H461-r209.json`, and
`docs/display-mode-watchdog-r208-r210.md`.

Build verification uses the existing resolved target tree with the pinned
original kernelcache and XNU d8b80295118ef25ac3a784134bcf95cd8e88109f:

```sh
PATH=/opt/homebrew/bin:$PATH /opt/homebrew/bin/python3 /path/to/UniXNU/unixnu \
  kernel -D /tmp/e5-device-ca-state-r202 --out /tmp/e5-local-build-v43 \
  --kernelcache /tmp/e5-local-build-v43/ipsw/21H461/kernelcache.decompressed
```

R210 compiles, links and prelinks successfully. Kernel SHA256:
`60b25611cbec2697c5681c7cc67f01820129d5744c73b59d9ebf70756c2a1ced`.
The payload retains the baseline firmware, ADT, seed and trust cache;
only the kernel file and its FAT chain/size metadata are replaced.
Board acceptance and current mode/timer/notification measurements follow in
the board evidence record after capture.
