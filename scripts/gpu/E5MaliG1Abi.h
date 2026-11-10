// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

enum {
    kE5MaliAbiMajor = 1,
    kE5MaliAbiMinor = 7,
};

enum E5MaliSelector {
    kSelGetVersion   = 0,   // in  -              out Info
    kSelCreateBo     = 1,   // in  CreateBo       out CreateBoOut
    kSelDestroyBo    = 2,   // in  DestroyBo      out -
    kSelMapBo        = 3,   // in  MapBo          out MapBoOut
    kSelUnmapBo      = 4,   // in  UnmapBo        out -
    kSelImportFixed  = 5,   // in  ImportFixed    out MapBoOut (diagnostic, gated)
    kSelSubmit       = 6,   // in  Submit         out SubmitOut
    kSelWaitAtom     = 7,   // in  WaitAtom       out WaitAtomOut
    kSelQueryAtom    = 8,   // in  WaitAtom       out WaitAtomOut (no sleep)
    kSelStats        = 9,   // in  -              out Stats
    kSelCleanBo      = 10,  // in  CleanBo        out -
    kSelSelfTestMmu  = 11,  // in  -              out SelfTestMmuOut
    kSelCacheRange   = 12,
    kSelGpuProperties = 13,
    kSelSubmitMany   = 14,
    kSelImportUser   = 15,
    kSelSubmitWaitMany = 16, // SubmitMany in; SubmitWaitOut out; bounded genuine wait
    kSelCount        = 17,
};

enum E5MaliAtomState {
    kAtomFree = 0,
    kAtomQueued,       // accepted, not yet on hardware
    kAtomOnHw,         // written into a slot ring
    kAtomDone,         // hardware reported DONE
    kAtomFault,        // hardware reported an error code
    kAtomTimedOut,     // watchdog fired; reset rolled the slot back
    kAtomCancelled,    // client exit / TERMINATED status
    kAtomReset,        // dropped by a reset (never re-armed)
};

struct E5MaliInfo {                  // kSelGetVersion out (72 bytes, arm64 natural)
    uint32_t abiMajor, abiMinor;
    uint32_t gpuIdRaw;               // GPU_ID
    uint32_t gpuStatus;              // driver lifecycle bitmask
    uint64_t shaderPresent, l2Present, tilerPresent;
    uint32_t mmuFeatures, asPresent, jsPresent;
    uint32_t vaBits, paBits;
    uint32_t maxSlots, maxAs, vaWindowBytes;
};
struct E5MaliCreateBo {              // in
    uint64_t size;
    uint32_t flags;                  // bit0 noExec
};
struct E5MaliCreateBoOut {           // out
    uint32_t handle;                 // 1..maxBoHandles (GpuProperties).
    uint32_t status;                 // IOReturn
    uint64_t size;
};
struct E5MaliDestroyBo {             // in
    uint32_t handle;
    uint32_t pad0;
};
struct E5MaliMapBo {                 // in (32 bytes)
    uint32_t handle;
    uint32_t flags;                  // bit0 fixed VA (vaHint), bit1 noExec
    uint64_t vaHint;                 // when flags bit0; page aligned
    uint64_t offset;                 // within BO, page multiple
    uint64_t length;                 // page multiple, 0 = rest of BO
};
struct E5MaliMapBoOut {              // out
    uint64_t va;
    uint32_t status;
    uint32_t pad0;
};
struct E5MaliUnmapBo {               // in
    uint32_t handle;
    uint32_t pad0;
    uint64_t va;
    uint64_t length;
};
struct E5MaliImportFixed {           // in (diagnostic; e5-mali-g1-import=1)
    uint64_t physAddr;               // 4 KiB aligned, inside DRAM
    uint64_t length;                 // 4 KiB multiple
    uint32_t flags;                  // bit0 noExec
    uint32_t pad0;
};
struct E5MaliSubmit {                // in
    uint64_t jc;                     // GPU VA of job chain (mapped, !NX)
    uint8_t  slot;                   // 0..maxSlots-1
    uint8_t  pad0[7];
    uint64_t udata[2];
    uint32_t boHandles[8];           // pinned until completion; 0 terminates
    uint32_t pad1;
};
struct E5MaliSubmitOut {             // out
    uint64_t atomId;                 // nonzero
    uint32_t status;
    uint32_t pad0;
};
struct E5MaliWaitAtom {              // in
    uint64_t atomId;
    uint32_t timeoutMs;              // 0 = poll only
    uint32_t pad0;
};
struct E5MaliWaitAtomOut {           // out
    uint32_t state;                  // E5MaliAtomState
    uint32_t hwStatus;               // JS_STATUS exception code
    uint64_t completionSeq;
    uint32_t status;
    uint32_t pad0;
};
struct E5MaliStats {                 // kSelStats out
    // Counter semantics (kept SEPARATE, matching hash-golden.md §2):
    //   faults    = job-slot exceptions (JS_STATUS >= 0x40, atom state Fault)
    //   mmuFaults = AS_FAULTSTATUS events (decoded code/addr below); an MMU
    //               fault does NOT increment faults — its atom retires as
    //               Reset (the engine reset it triggered).
    uint64_t submitted, completed, faults, timedOut, cancelled;
    uint64_t resets, mmuFaults, lastMmuFaultCode, lastMmuFaultAddr;
    uint64_t userCachePages; // ABI1.3: globally held imported RAM pages
    uint32_t activeAtoms, mappedBytes, liveBoBytes;
    uint32_t pad1;
};
struct E5MaliCleanBo {               // in
    uint32_t handle;
    uint32_t pad0;
};
struct E5MaliSelfTestMmuOut {        // out
    uint32_t status;
    uint32_t entriesMapped, entriesVerified, tablesAllocated, tablesFreed;
    uint64_t readbackTranstab, readbackTranscfg, readbackMemattr;
    uint32_t asUsed;
    uint32_t pad0;
};

typedef struct E5MaliInfo E5MaliInfo;
typedef struct E5MaliCreateBo E5MaliCreateBo;
typedef struct E5MaliCreateBoOut E5MaliCreateBoOut;
typedef struct E5MaliDestroyBo E5MaliDestroyBo;
typedef struct E5MaliMapBo E5MaliMapBo;
typedef struct E5MaliMapBoOut E5MaliMapBoOut;
typedef struct E5MaliUnmapBo E5MaliUnmapBo;
typedef struct E5MaliImportFixed E5MaliImportFixed;
typedef struct E5MaliSubmit E5MaliSubmit;
typedef struct E5MaliSubmitOut E5MaliSubmitOut;
typedef struct E5MaliWaitAtom E5MaliWaitAtom;
typedef struct E5MaliWaitAtomOut E5MaliWaitAtomOut;
typedef struct E5MaliStats E5MaliStats;
typedef struct E5MaliCleanBo E5MaliCleanBo;
typedef struct E5MaliSelfTestMmuOut E5MaliSelfTestMmuOut;
