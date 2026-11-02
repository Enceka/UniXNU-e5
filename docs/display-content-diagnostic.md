# R109 original IOMFB submission diagnostic

The CI362/R108 content audit supersedes the earlier compositor attribution.
All retained tiles match iso_font character `f`; serial_init returns zero,
leaving the video console selected, and the frame probe calls printf while
reading the same scanout. CRC motion alone does not establish compositor swaps.
See [original-image evidence](logs/display-content-audit-r108.json).

This diagnostic keeps the original family text, UserClient implementation,
Legacy inheritance, provider contract and display-scale policy fixed. It
adds the following observations:

- `IOUserClient::callExternalMethod` records the original IOMFB class's
  selector calls and success/last-return counters. It covers UC2022 through
  the existing gateway and preserves dispatch and locking behavior.
- `E5IMCALL` logs PID/name, selector, argument sizes and return status on
  initial calls, PID/status changes and sampled repeated calls. Only successful
  selectors 3/4 expose their first scalar output; no frame bytes are decoded.
- `E5IMSUM` retains console index/calls/suppressed calls and selector 3/4/5
  call/success/last-status fields. `E5IMSEL` periodically repeats all nonzero
  selector counters with the last PID and applicable scalar output.
- `e5-no-vc` suppresses video character painting at vcputc_options, with
  independent call and suppressed counters. It does not change v_display or
  the validated boot framebuffer geometry.
- The frame probe uses the existing E5TW byte writer instead of printf.
  Summaries run independently of CRC changes, so a static buffer stays observable.

Counters are approximate concurrent snapshots. The gateway does not observe
MIG argument or policy rejections that occur before dispatch. API success does
not establish surface mapping, DPU ownership, panel transfer or visible UI.

Keep the original 21H461 family and common source pin
`066c8d8315f83749d28bcae2faf3595718646be5`. Run the full resolved patch replay,
porting-record check and kernel CI before preparing the payload. The board
cycle must use full boot_b readback, the existing scale/provider ADT and the
same enabled-SB Data fixture, then verify Android A recovery and the full
128 MiB Data hash. Record build and board acceptance separately.

LA is a separate dependency: the retained R96 exception is a force unwrap of
NULL from MKBEventsRegister in SessionSyncEngine. AppleKeyStore lookup and
event registration need their own diagnosis; no display property is checked
at that site.

Host validation: all 86 resolved patches replay successfully against pristine XNU 10063.141.1 with the pinned common, XnuPrelink and SoC inputs. The 13-area porting-record checker passes. CI363 and R109 now pass compile/link/prelink, full flash readback and Android/Data recovery checks. See [R109 findings](display-submit-r109.md). Readiness and visible presentation remain unqualified.
