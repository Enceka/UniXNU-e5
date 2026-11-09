// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// ABI 1.1 additions; selectors 0..11 and their layouts remain unchanged.
#define E5_MALI_CACHE_RANGE 12u
#define E5_MALI_GPU_PROPERTIES 13u
#define E5_MALI_SUBMIT_MANY 14u
#define E5_MALI_MAX_SUBMIT_BOS 64u
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
