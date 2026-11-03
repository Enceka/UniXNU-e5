# Display UI arc: power→wait→swap-band→events→D2→D3 (rounds 109–147)

Status: CONTINUING. Milestone chain from the r109 no-swap finding toward on-screen UI.

| Round | Kernel/branch | Result |
|---|---|---|
| r109 | fix/display-submit-trace-r109 (user) | 42k intercepted UC chars; DefaultSurface/SwapBegin/SwapEnd = 0; SwapWait 11.9k× Busy; RequestPowerChange Unsupported |
| r110–r112 | fix/display-power-contract-r110 | +0x8c arm (v13), power band overrides (v15): sel 6 → 3007× Timeout retry loop = client wants to wait |
| r113 | v16 E5STATE | member map: m1658=0 (wait sub-object), f1860=0000 |
| r114–r117 | v17/v18/v19/v20 | wait 176 + swap band 180/183/199/206/207/232 + 201/224 + 200/203 → **all 18 client selectors green** (r117) |
| r118 | v21 fake vsync | no token registered → no-op (safe) |
| r119–r121 | v22/v23/v24 | provider cache +0x1640 ✓, panel-online +0x1788/+0x16d4 → event token +0x16d0 first non-null; A/B no visible difference |
| r122–r123 | v25/v26 vsync cadence | 1-in-4 cadence revives wait loop (n=4 ok=1, F=30→49); denser = client idles sooner; saturated |
| r124 | v27 native wait semantics | enable path never armed; LA abort confirmed sole crasher |
| r125–r126 | v28/v28.1 async delivery + user r126 | sendAsyncResult64WithOptions 630/630 ok; **wrong-payload kind-3 FROZE the fallback drawer (F=1)** — channel works, payload semantics matter |
| r127 | user fix/display-wait-abi-r127 | native wait ABI correct; still no surface/swap; black screen + white block |
| r128 | user fix/display-liveness-r128 | sel-74 FrameInfo DECLINED → ak=00 no registrations; drawer resumed (F=32); E5LIVE census live; **0 crashes** |
| r129–r134 | e5diag userspace-agent arc | wrapper exec confirmed (G=2); fork/posix_spawn/dyld-interpose ALL blocked (EPERM/scrubbed env) — userspace telemetry wall FINAL |
| r135–r136 | v29/v29.1 family broadcast | inline marshaling stalls service path; thread_call delivery healthy; kind-3 still no swap |
| r137–r144 | D2 arc (local build from 402L) | DPU regs verified vs M0; PWM backlight pulsing (PA 0x64030000); paint crc cycles proven; RUN bit → **user SAW scrolling RGB bands = D2 VISUAL ACCEPTANCE PASS (r144)** |
| r145 | D3 v0 i2c probe | data abort: controller clock-gated (hot-state shortcut false) |
| r146 | staged (merged main) | first merged-main round: D3 v1 gate-first i2c + native wait/async lineage + D2 paint |
| r147 | staged (fix/display-frameinfo-enable-r147) | sel-74 un-declined count=1 transport=0 key='e5' — single-variable experiment for registration→link→render→SwapBegin |

## Static decode anchors (see porting.toml display area)

- sel 74 = SupportedFrameInfo-class capability (wrapper 0x6778688 → slot 213); sel 15 ≈ ReadyForSwap (slot 180);
  SwapBegin/SwapEnd (sels 4/5) never called — gated behind CADisplay link start.
- backboardd: IOMFB imports only Open/SetDebugFlags; links IOSurface+QuartzCore+Metal; renders via
  _CARenderServerRenderDisplay; overlay snapshot path = IOSurfaceCreate→render→CGImage ("BKDisplayRenderOverlayImage");
  legacy AppleH1CLCD/M2CLCD matching is a fallback, not our path.
- E5LIVE services=0x38: IOSurfaceRoot + AppleKeyStore + IOCoreSurfaceRoot present; IOGPU/IOAccelerator absent (no GPU driver).
- sel-17 0xc0c display-info = snapshot of H9G panel sub-object state (service→+0x1e88→+0x10 blob, fill via GOT memcpy
  0x6c0d638 with w2=0xc0c); zero-fill already accepted (ok=1) — content not the blocker.

## Next

1. Flash r147 → read ak/asend/aok/d/b (registration → deliveries → SwapBegin).
2. Then r146 (D3 i2c gate probe).
3. If r147 registers but no swap: decode CADisplay link start conditions in CA render server.
