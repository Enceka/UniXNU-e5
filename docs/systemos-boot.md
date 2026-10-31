# SystemOS and original dyld bring-up

Round 27 uses the seed's 128-byte trust bundle even while booting the original
OS. Both original launchd and dyld are rejected by AMFI. Launchd's signed
entitlements include `com.apple.apfs.get-dev-by-role`, but the subsequent
Preboot query fails EPERM. The first control replaces only the trust bundle
with the two unchanged original modules (2554 entries, bundle SHA256
`7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7`).
No entitlement or signature is fabricated.

The original SystemOS is a bare APFS container, 3303014400 bytes, SHA256
`b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f`.
It contains the complete arm64 dyld cache. Its volume UUID is
`8846FE78-2910-4297-AF97-D59BB6F8FC16`; it has no Preboot role.

The `e5-systemos` argument publishes a second read-only IOBlockStorageDevice
over SD sectors 12582912..19034111 (6 GiB plus 3150 MiB). The host tool
`tools/e5-systemos-media.py` in e5-xnu verifies the whole input, backs up that
exact destination and checks the full installed readback. The original OS
image and both copies of its GPT stay outside this window. The driver checks
the pinned 4 KiB NXSB CRC, capacity and every request's window bounds. All
reads share the original host's lock, DMA buffer and failure handling.

The additional `e5-systemos-boot` argument enables a DEVELOPMENT/DEBUG-only
startup experiment, requiring `e5-systemos` and the existing
`e5-rootauth-off` opt-out. It finds the actual APFS volume by UUID, mounts it
read-only as a native union over the OS root, checks the resulting root graph
and dev_t, reads the original arm64 cache header, then sets dyld's existing
skipIgnition bit 3. Missing media, failed mounting or an unexpected cache
stops startup before that bit is set. Neither switch is enabled by default.

This explicit development path is separate from provisioning a real Preboot
volume and executing native libignition. It does not establish Apple root
authentication, persistent Data, a complete system boot or a working UI.
Device acceptance and the next observed failure must be recorded separately.
