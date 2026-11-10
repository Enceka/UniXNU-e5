# R298 masked GPIO event timing

R297 records 1,088 changed hardware frames, 12 complete two-contact releases
and final active count zero. Its physical canvas observations pass, but 18
malformed packets and 18 recoveries prevent transport acceptance. The startup
census reports no other enabled or pending GPIO sources in 16 banks. These
results motivate an isolated packet timing experiment.

`e5-touch-gpio-events=1` with `e5-touch=3` configures GPIO13 for single falling
edges (IS/IBE/IEV bit13 clear) after firmware identification restores normal
mode. IE bit13 stays masked; the shared GIC SPI40/INTID72 has no new callback.
An already enabled GPIO13 is refused. Startup clears stale RIS through the
pin-only IC write-one-to-clear action. Stop restores only the saved GPIO13
trigger fields; unrelated pin changes survive and IC is never restored.

The existing HID timer polls RIS. Only RIS bit13 permits an 18-byte packet
read. A high idle line, low line without a latch, first packet and recovery
all obey the same gate. Mandatory identity/configuration transactions precede
this packet-event epoch. There is no unconditional baseline or timeout read.
Contacts already held at configuration may require a new edge before being
observed; the operator should leave the panel untouched until ready.

IC acknowledges the observed edge before I2C, with no serial logging in
between. An edge during the transaction remains latched for the next poll.
Several edges before acknowledgment can coalesce in RIS; its count must not
be described as the physical edge count or lossless delivery. The timer is
rearmed 16 ms after work completes, so its period includes execution time.

`E5TOUCH events` records timer polls, authorized reads, consumed latches,
skipped polls and low-without-edge observations. Within this epoch,
`polls = reads + skipped` and `reads = edges`. `E5TOUCH status` retains packet
attempt/good/bad/recovery counters. `event-read` records the first 64 reads,
every 256th read, and all failures, with sequence/poll indices and raw
`mach_absolute_time` ticks for observation, read start and read end. It also
records before/after GPIO levels and pending RIS after the transaction without
clearing it. Observation is a software sampling time, not the physical edge
timestamp; only observation-to-read and read duration can be derived.

The physical trial keeps R297's reset=2, canvas, native split digitizer and
strict packet decoder. Check a quiet interval first, then single taps/drags,
two-contact moves and hardware all-up. Require increasing timer polls with
unchanged reads at idle, consistent event/read counters, ordered timestamps,
zero malformed/failed packets, complete lifts and zero final active contacts.
Keep firmware identity, payload readback, protected windows and Android return
as separate checks. Any transport failure remains a failed trial even if
the canvas responds. GIC delivery follows in a subsequent experiment.

Host tests cover short pulses ending before DATA sampling, idle/held-low
suppression, pre-read acknowledgment, edges during a read, rejected ownership
and trigger readback, restart and pin-only restoration. ASan/UBSan GPIO, protocol and I2C-tail tests pass, as does native compile,
link and prelink. Kernel SHA-256 is
`41e677be89e40b98017629318ccd2ae9c469abbeb12211cdb5f63bf3589f09ca`.
The build audits the original 207-slot HID ABI, rejects automatic C++
initializers in the touch/canvas objects and verifies the source hashes.
The first build attempt used system Python without tomllib; rerunning with
the existing Python 3.15/LLVM host tools passed.

`e5-xnu/tools/e5-gpio-events-check.py` checks event/packet accounting, mask
state, quiet intervals and sampled timestamp ordering. Its negative controls
reject malformed traces, counter regressions, packet errors and ungated reads.
R298 runtime completes with Android A return and three37/37 protected-window
checks. GPIO accounting/idle suppression and all67 sampled timestamp orders
pass. Last periodic records show40961 timer polls,343 consumed latches/reads
and40618 skips,IE0. Eighteen quiet windows have no new reads over36864 polls.
341 packets are valid, but two format errors/recoveries keep transportFAIL.
Both failed reads have pending_after1, also observed in successful reads;
no sensor-update/stale-edge cause is established.

Hardware records contain276 changed frames,two complete two-contact releases
and final active0. Two other sequences end at sensor errors, which do not
supply hardware lifts.306 HID reports have zero reported failure/drop.
All five canvas captures200..204 pass their original checksum; four require
removal of complete interleaved diagnostics. Operator/UI/GIC qualification
is separate. Results are in [the runtime record](logs/input-gpio-events-r298.json)
and `e5-xnu/uefi/build/sb-r298-gpio-events`.

The subsequent [R299 control](input-gpio-cadence-r299.md) selects2ms rearming
while retaining this GPIO/I2C/decoder path. Different workloads forbid
describing the raw18-to2 error counts as a qualified error-rate improvement.
