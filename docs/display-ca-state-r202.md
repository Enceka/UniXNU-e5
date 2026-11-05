# R202: observe the actual CoreAnimation display state

R200 restored writable Data and late SpringBoard/backboardd liveness after the
SystemOS cache repair. R201 disables benchmark preferences and video-console
drawing; the user still observes a small white block at the upper left.

`e5-ca-state` enables a bounded read-only diagnostic. The media-side census
only schedules a separate thread_call every 16 backboardd observations; it
never waits for task/map locks or page faults. The worker retains the task,
gets TASK_DYLD_INFO, and reads through XNU's `vm_map_read_user`. Failed reads
stop that sample. No task is suspended and no user memory is written.

The cache UUID, QuartzCore UUID, NSArrayM class/layout, CAWindowServerDisplay
class and original server/display vtables qualify the chain. Other array or
display classes are reported as unsupported rather than interpreted. Exact
21H461 instructions establishing the fields are archived in e5-xnu's
`uefi/build/systemos-read-repair-20261105/` and `/tmp/e5-ca-object-layout-20261105.txt`.

`E5CA` records the read phase, result, slide, server, display array and C++
display pointers. `E5CAX` records current packed mode, display flags, enable
and power words, effective benchmark byte, software context/renderer, Metal
renderer, timer and event-source pointers. Atomic generation-checked snapshots
are repeated in E5TW so wrapping does not erase the latest result.

These snapshots identify the next gate; they do not prove rendering or
physical presentation. Compile/link and device acceptance are pending.

R201 later faults at `e5_dpu_tick+0x3c4`: `ldp x8,x9,[x0]` reads the expired
stack registration pointer returned by `e5_display_async_reg3`. Copy its four
words during registration and publish only that static diagnostic snapshot.
The node words used by the timer are also copied during registration; live
event delivery retains its existing separate pointer and behavior. No claim
about callback execution follows from the old sender-success counts.
