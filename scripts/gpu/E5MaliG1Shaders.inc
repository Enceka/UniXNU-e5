// SPDX-License-Identifier: MIT
//
// E5MaliG1Shaders.inc — Valhall v9 fullscreen-triangle shader assets for
// the E5MaliG1 in-kernel self-test §4.2 (bit3). Compiled OFFLINE by the
// Mesa panfrost/bifrost compiler — NOT hand-assembled.
//
// Provenance (see .work/gpu-user-r310/shaders/{tri_fullscreen.inc,
// BUILD-NOTES.md, SE-DCD-FILL.md}, user-space subagent, 2026-11-09):
//   tree:  FristOneRR-Panvk-Source @ efd07bab401b263a9ac114560ecdb9a2934f5d43
//   tool:  src/panfrost/compiler/bifrost/tri_dump.c
//          (SHA256 5db293c7152e51da4c577f7729200e3066d7701adc2c44971ac9bb7a76fdd0f9)
//   build: debian:bookworm-slim, meson 1.12.1, -Dtools=panfrost
//   gpu_id 0x90910010 (R313), deterministic output
//          (dump SHA256 5436c168d4707894262f44327897c43466d0943571d0a6352a2718a17ab2e624)
//
// Semantics:
//   vs_fullscreen — IDVS position shader, 3 vertices from gl_VertexID
//     covering the 320x480 viewport (clip-space fullscreen triangle);
//     128 B, preload R59/R60, FAU 3 words.
//   fs_const — writes constant RGB(219,42,18)/255 via ATEST+BLEND
//     (real shader execution); 128 B, preload R60/R61, FAU 3 words.
//   fs_tri — edge functions on the preloaded fragment position, DISCARD
//     outside, yellow inside; 256 B, can_discard, preload R59/R60/R61,
//     FAU 6 words (R339 edge-function fix: no y flip, +0.5 pixel
//     centre, float compare).
// FAU words are PART OF THE PROGRAM: upload them to the FAU buffer the
// Shader Environment points at.
//
// Shader Program descriptor word2/word3 (binary GPU VA) are patched at
// runtime by E5MaliG1.cpp. Field citations in the source tree comments
// refer to refs/v9.xml.
#ifndef E5_MALI_G1_SHADERS_INC
#define E5_MALI_G1_SHADERS_INC

#include <stdint.h>

/* ---------------- vertex: fullscreen triangle (position/IDVS) ------------- */
static const uint32_t e5MaliVsFullscreenBin[] = {
    0x00000d7b, 0x005e8004, 0x98c0c83c, 0x00f0c200,
    0x9cc0c47c, 0x00f0c300, 0x81c0c042, 0x0150c200,
    0x80c0c043, 0x0150c300, 0x00000043, 0x0091c400,
    0x00000042, 0x0091c500, 0x00000082, 0x0091c600,
    0x000000d0, 0x0891c700, 0x79000040, 0x78614408,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
/* v9.xml "Shader Program" (8 words, align 32): word0 Type[3:0]=Shader(1)|
 * Stage[7:4] (0=Vertex,1=Fragment) | WarpLimit[9:8] | FTZ[18:17] |
 * RegAlloc[31:30]=2 (32 regs); word1 preload R48-R63 bitmap; word2-3 =
 * binary GPU VA (PATCHED by the kernel); word4-7 = 0. FAU count lives in
 * the Shader Environment, NOT here. */
static const uint32_t e5MaliVsFullscreenProg[8] = {
    0x80000138, 0x00001800, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t e5MaliVsFullscreenFau[] = {
    0x44200000, /* u0.w0 = 160.0f (viewport scale x) */
    0x44700000, /* u0.w1 = 240.0f (viewport scale y) */
    0x3f000000, /* u1.w0 = 0.5f   (viewport scale z) */
};

/* ---------------- fragment: constant colour ------------------------------- */
static const uint32_t e5MaliFsConstBin[] = {
    0x00000000, 0x4000c000, 0x00ead07c, 0x087dbc02,
    0x00000082, 0x6891c200, 0x000000d0, 0x0091c300,
    0x00000080, 0x0091c000, 0x00000081, 0x4891c100,
    0x323c00f0, 0x787f4008, 0x000000c0, 0x0110f000,
    0x0000f1c0, 0x002fc110, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t e5MaliFsConstProg[8] = {
    0x80000128, 0x00003000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t e5MaliFsConstFau[] = {
    0x3f5bdbdc, /* u0.w0 = 0.85882f (R = 219/255) */
    0x3e28a8a9, /* u0.w1 = 0.16471f (G =  42/255) */
    0x3d909091, /* u1.w0 = 0.07059f (B =  18/255) */
};

/* ---------------- fragment: probe triangle (discard outside) -------------- */
static const uint32_t e5MaliFsTriBin[] = {
    0x0015003b, 0x6890c000, 0x1015007b, 0x0090c100,
    0x1f800000, 0x0114c2c3, 0x1e000001, 0x0114c3c2,
    0x00c04380, 0x00b2c304, 0x00434281, 0x00b2c204,
    0x6e000000, 0x0114c3c2, 0xd1c00001, 0x0114c4c3,
    0x00c04482, 0x00b2c404, 0x00444383, 0x00b2c304,
    0x8bc00040, 0x0114c0c3, 0xbdc00041, 0x0114c1c3,
    0x00c04184, 0x00b2c104, 0x00414085, 0x00b2c004,
    0x00024342, 0x00a4c100, 0x00024041, 0x00a4c000,
    0x0000c040, 0x4020c004, 0x00ead07c, 0x087dbc02,
    0x000000d0, 0x0091c000, 0x000000d0, 0x0091c100,
    0x000000c0, 0x0091c200, 0x000000d0, 0x4891c300,
    0x323c00f0, 0x787f4008, 0x000000c0, 0x0110f000,
    0x0000f1c0, 0x002fc110, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t e5MaliFsTriProg[8] = {
    0x80000128, 0x00003800, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t e5MaliFsTriFau[] = {
    0xc2c80000, /* u0.w0 = -100f */
    0x43be0000, /* u0.w1 = 380f */
    0x435c0000, /* u1.w0 = 220f */
    0xc2200000, /* u1.w1 = -40f */
    0xc2f00000, /* u2.w0 = -120f */
    0xc3aa0000, /* u2.w1 = -340f */
};

#endif /* E5_MALI_G1_SHADERS_INC */
