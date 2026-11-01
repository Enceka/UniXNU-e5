# Display family probe v6: registration and allocation are different

The r77/r79 `c=12` census used `allocClassWithName()`. It does not prove the
display metaclasses are absent: in CI 332, Framebuffer, Service and Legacy have
metaclass `alloc()` entries that unconditionally return NULL. This matches
XNU's abstract-class factory macro. See `docs/logs/display-family-static-k332.json`
for the input hash, recovered constructor/vtable sites and instruction words.

The same KC declares IOMobileGraphicsFamily executable, has neither interface
nor kernel-resource metadata, and carries ten module initializer pointers.
XNU `OSKext::load()` reaches its startup gate through the `loaded:` label even
when `isLoaded()` is true. Zero personalities does not prevent an explicit load
from initializing a family. Actual runtime state still needs measurement.

CI 333 / v5 failed to compile: OSPtr is a retained raw return type in this archive
header mode, so it cannot be a local shared pointer or use `.get()`; `start()` is
private. V6 uses a released raw lookup reference and the public locked load API,
with startup allowed and catalogue matching excluded. It does not force C++
reinitialization or call private entry points.

V6 progress fields are persistent in sampled NativeSd reads:

| Field | Meaning |
| --- | --- |
| `c` | Prior allocation mask and low byte of load result; same four-class order |
| `m` | Registered metaclass mask: Service=1, Legacy=2, IOMedia=4, AppleARMIODevice=8 |
| `k` | Hex state: record=01, loaded=02, started=04, CPP initialized=08, declares executable=10, executable=20, interface=40, kernel component=80 |
| `l` | Full hexadecimal OSReturn from `loadKextWithIdentifier()` |
| `a` | **V6 semantics:** lcd nub=1, concrete E5Display metaclass=2, published E5Display service=4 |

`c=12 m=15 k=3f l=0 a=1` is consistent with a started family and registered
abstract display classes, while the concrete platform driver is still absent.
R80 observes this exact combination in all 202 complete retained samples.
The concrete platform driver is the next missing implementation. If a later
round loses `m` bits 0/1, use `k` and `l` to investigate the actual loader/C++
state before changing it.
V5 high bits of `a` are not reused: the old start() experiment never built.
They also could not survive the call sequence: v5 ORed them into the initial
`a=-999`, then `e5_display_start()` reset `a` to zero. V6 keeps loader state in
its own initialized `k` field instead.

V6 removes both abstract Legacy personalities (lcd and IOResources). The next
implementation must be a concrete E5Display subclass with the exact Legacy ABI
and platform overrides, matching the real lcd nub and driving verified E5
scanout. Do not fake an Apple scaler node to run an Apple hardware driver.

Host audit command, run from e5-xnu:

```sh
python3 tools/e5-display-family-audit.py \
  --kernel uefi/build/e5-kernel-artifact-332/kernel.macho
```

CI 335 passes the complete archive, kernel link and prelink at device revision
`9db769180fcf76dc0043f2436e9436aaa5a39901` with the common source pinned to
`066c8d8315f83749d28bcae2faf3595718646be5`. See
`docs/logs/display-family-ci335.json`. The r80 board cycle
confirms family startup/registration separately, as recorded in
`docs/logs/display-family-round80.json` and its 202-sample console. All samples
show `c=12 m=15 k=3f l=0 a=1`, `d=100`, published/ready and `t=17:0`; completed
request counters run 221312..247040. The retained ring contains no
panic/E5EXC/ENOSPC. Fresh Android A returns and the complete 128 MiB card Data
fixture hash remains unchanged. The headless fixture keeps SpringBoard and
liveactivitiesd disabled; this does not retest their abort or qualify UI.
The original host wait process ended before collection; pstore was harvested
read-only after fresh Android identity checks, without another reboot.
The standalone host audit reproduces the three NULL factories on CI 321 and
332. The pre-existing R71 storage note is normalized from the unsupported
`board-cycle` kind/short revision to the standard `device` kind, full historical
revision and log reference. This is metadata repair, not a new R71 board run.
