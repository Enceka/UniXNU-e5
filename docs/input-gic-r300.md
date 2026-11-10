# GPIO/GIC callbacks and lifetime

The isolated ArmCpu candidate adds masked registration, owner-qualified line
management and callback gate/drain/detach. A timeout retains closing ownership
and caller resources. The SPI ID stays immutable and detached slots can reuse
that ID. Host ASan/UBSan verifies concurrent context retirement and reuse.

GPIO13 uses a falling edge with shared SPI40/INTID72 configured level high.
The primary callback checks enabled/pending sources in all16 banks, acknowledges
only GPIO13 and signals a providerless IOInterruptEventSource on the owned HID
workloop. Foreign sources are contained without clearing their pending bits.
I2C, decoding, diagnostics and HID submission run in deferred work. Callback
tokens authorize reads; coalescing is explicit, and recovery preserves tokens.

R300 refuses before GPIO enable because its group readback check incorrectly
requires IGROUPR visibility in the nonsecure view. Mask/detach/drain succeed.
R301 checks visibility through GICD_CTLR.DS and verifies accessible enable bits.
Linux irq-gic-v3.c documents the single-security-state restriction. The local
candidate requests Group1 aliases, while this board still reads CTLR0x12.

R301 has386 callbacks/386 valid reads,341 event-work reads and45 timer reads
of existing callback tokens. No transport/recovery/foreign-source errors occur.
158 hardware changes include two complete dual-contact sequences, final0,
operator-confirmed tracking/lift and four original-checksum canvas frames.
The idle mask/detach/drain/reattach cycle succeeds, but there are no new
post-reuse callbacks; operator observation of reuse is missing. Overall reuse
qualification remains INCOMPLETE. Both rounds return Android A and three37/37
protected-window checks pass. See [R300](logs/input-gic-r300.json) and
[R301](logs/input-gic-r301.json). Original SpringBoard physical UI is the next
acceptance stage; a canvas result does not replace it.
