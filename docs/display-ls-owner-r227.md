# R227: identify the LaunchServices lock owner

Host candidate, not yet device-qualified. The e5-xnu audit
`docs/display-mig-static-20261105.md` corrects the previous-console replay mix:
R220/R225 native E5TW records stop at LSContextInitCommon+0x2f8, while R226
contains zero registers. The actual display MIG ID is 0x5b9173; 0x1513 is
msgh_bits. No mode-ready wait was found in the original handler.

The candidate copies saved states with ml_nofault_copy under task_lock and
reports saved-state address, copied length, flavor and failure status. Only
SpringBoard gets 24 frame returns; other processes retain six. Thread IDs
and user Mach self-port names allow offline owner matching. The LS recursive
lock at shared-cache unslid 0x1f4884878 is read before/after the thread sample.
The task cache UUID and slide are checked before reading that global.

Current thread.o DWARF verifies machine=0xf8, upcb=+0x18,
cthread_self=+0x50, thread_id=0x508, task_threads=0x428. Original pthread and
both unfair-recursive-lock variants load their port name from TPIDRRO_EL0+0x18.
The owner is a Mach port name, not ctid. No lock values are modified.

Frame status: 0 valid; 1 upcb-pointer read failed; 2 saved-state copy failed;
3 unexpected flavor; 4 invalid user PC; 5 thread-link read failed.
The old saved-state address predicate excluded addresses >=0xfffffff800000000;
R227 records actual addresses to test whether this caused R226's zero frames.
The cause remains unproven until a valid new device record is collected.

R227 device result: boot_b readback matches 72d2a6f8d5409116b053d65762981055f23c8e3a72b0fe0a187236fa69038e71.
Android returns in 660 seconds; no retained panic. All 23 retained thread states
are valid. SB main-thread port 0x103 matches stable gSessionLock word 0x102.
Its 24-frame chain includes LSCopyServerStore+0x36c and the main-queue blocking
I/O marker; the application enumerator waits on the same lock. UI remains
unqualified. R228 observes lsd and the seeding group without changing database
state or bypassing startup dependencies.

R228 also suppresses E5STATE when e5-ca-state is present. Inspection of the
actual frozen r226 payload shows that r221 repacking inherited r219 and lost
r220's e5-no-state boot argument. Only diagnostic output changes.
