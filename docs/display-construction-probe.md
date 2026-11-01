# E5Display construction probe v7

R80 establishes family startup and registered abstract display classes. V7 now
adds the concrete factory and a bounded original `init`/`release` cycle. It has
not yet passed CI or device validation.

The build reads the SHA-pinned original 21H461 kernelcache through the existing
storage audit cache. It checks the complete family text hash, C2/D2 ancestry,
358 virtual entries and nine abstract slots. The compiled E5Display vtable must
match every inherited native symbol and every bound family target. Its only
owned operational override is `start`, which returns false. All nine pure
backend slots remain noreturn diagnostic guards; their prototypes/operations
are not represented as implemented. No original executable or live vtable is
patched and no saved runtime-observation hash is substituted.

Only the owned automatic initializer entry is removed. The probe validates the
actual registered Legacy metaclass address, `0x18a0` instance size and complete
Service/Framebuffer/native-IOService ancestry before running that initializer
once inside `preModLoad(g_kernel_kmod_info.name)` / `postModLoad`. It then checks
the owned metaclass/size/parent, allocates through `allocClassWithName`, checks
the instance ancestry, calls the original `init` and releases it. Instance counts
must return to their starting value. It does not attach, start or publish an
IOService, access panel registers or map/write framebuffer memory.

Persistent NativeSd fields extend v6:

| Field | Meaning |
| --- | --- |
| `v` hex | entered=001, original parent qualified=002, owned static constructor returned=004, postModLoad success=008, owned metadata qualified=010, factory allocated=020, instance ancestry qualified=040, original init success=080, release returned with balanced instance count=100 |
| `s` hex | Actual registered original Legacy size before construction |
| `r` hex | Full postModLoad OSReturn; ffffffff before an attempt |
| `a` | lcd=1, concrete metaclass=2, published service=4; v7 should reach 3, not 7 |

The target result is `c=12 m=15 k=3f l=0 a=3 v=1ff s=18a0 r=0`, with Data
checkpoints preserved. This is an expectation, not an observation. A backend
guard panic identifies the exact unexpected slot; it must be investigated
before a hardware lifecycle is enabled.

The late-init/layout recipe is adapted from Ken-u/UniXNU-evb10 commit
`e03f9880ae8fc420fa337366a444c8888b0e3fdc`, itself derived from UniXNU-sheng.
The BSD-2-Clause-Patent notice is retained in `scripts/display/LICENSE`. No RK3588,
Goodix, VOP2 or GPU implementation is included.
