// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// ABI 1.1 additions; selectors 0..11 and their layouts remain unchanged.
#define E5_MALI_CACHE_RANGE 12u
#define E5_MALI_GPU_PROPERTIES 13u
#define E5_MALI_SUBMIT_MANY 14u
#define E5_MALI_SUBMIT_WAIT_MANY 16u
#define E5_MALI_MAX_BO_HANDLES 256u
#define E5_MALI_MAX_SUBMIT_BOS 256u
#define E5_MALI_LEGACY_SUBMIT_MANY_BYTES 288u
#define E5_MALI_CACHE_CLEAN 0u
#define E5_MALI_CACHE_INVALIDATE 1u

typedef struct E5MaliCacheRange {
    uint32_t handle, operation;
    uint64_t offset, length; // 64-byte aligned; length=0 means remainder.
} E5MaliCacheRange;

typedef struct E5MaliGpuProperties {
    uint32_t gpuId, l2Features, tilerFeatures, memFeatures;
    uint32_t mmuFeatures, threadMaxThreads, threadMaxWorkgroup, threadMaxBarrier;
    uint32_t threadFeatures, threadTlsAlloc, textureFeatures[4];
    uint64_t shaderPresent, l2Present, tilerPresent;
    uint64_t vaBase, vaBytes;
    uint32_t maxBoHandles, maxSubmitBos;
} E5MaliGpuProperties;

typedef struct E5MaliSubmitMany {
    uint64_t jc;
    uint64_t udata[2];
    uint32_t slot, handleCount;
    uint32_t handles[E5_MALI_MAX_SUBMIT_BOS];
} E5MaliSubmitMany;

// ABI1.7 combines SubmitMany and the existing bounded 500ms WaitAtom into
// one transport call. All legacy selectors/layouts remain available. A
// successful transport return alone does not imply a terminal DONE state.
typedef struct E5MaliSubmitWaitOut {
    uint64_t atomId;
    uint32_t state, hwStatus;
    uint64_t completionSeq;
    uint32_t status, reserved;
} E5MaliSubmitWaitOut;

// ABI1.6 keeps the 32-byte header and accepts both the old 64-handle packet
// and this 256-handle packet. A count must fit the supplied packet's array.
static inline int e5_mali_submit_many_size_supported(uint32_t bytes) {
    return bytes == E5_MALI_LEGACY_SUBMIT_MANY_BYTES || bytes == sizeof(E5MaliSubmitMany);
}
static inline int e5_mali_submit_many_count_valid(uint32_t bytes, uint32_t count) {
    return e5_mali_submit_many_size_supported(bytes) && count != 0 && count <= (bytes - 32u) / 4u;
}

// ABI 1.2: pin an existing writable range in the connection owner's task.
// No physical address input; MapBo must request non-executable GPU access.
// ABI 1.3 holds write-combining per imported RAM page and restores the old
// cache policy after the last import. ABI1.5 instead preserves CPU cache
// attributes and synchronizes shared imports at Submit/Wait completion; CPU
// atomic metadata in the same allocation must remain valid.
#define E5_MALI_IMPORT_USER 15u
#define E5_MALI_IMPORT_READ_ONLY 1u // ABI1.4: CPU-readable managed RAM, GPU RO+NX.
typedef struct E5MaliImportUser {
    uint64_t address, length; // 4 KiB aligned, 0 < length <= 256 MiB.
    uint32_t flags, reserved; // flags: E5_MALI_IMPORT_READ_ONLY or zero; reserved zero.
} E5MaliImportUser;
