// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <mach/vm_map.h>
#include <mach/memory_object_types.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../E5MaliG1Abi.h"
#include "../E5MaliG1Ext.h"

static void note(const char *format, ...) {
    char line[252];
    int prefix = snprintf(line, sizeof(line), "E5MTL ");
    va_list args; va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 2, format, args);
    va_end(args);
    size_t size = strlen(line); line[size++] = '\n';
    if (write(229, line, size) != (ssize_t)size) (void)write(2, line, size);
}

static uint64_t fnv(const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data; uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 0x100000001b3ULL;
    return hash;
}

static bool complete(id<MTLCommandBuffer> command, const char *label) {
    if (!command) { note("%s FAIL no command buffer", label); return false; }
    [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        note("%s FAIL status=%lu error=%s", label, (unsigned long)command.status,
             command.error.description.UTF8String ?: "none");
        return false;
    }
    return true;
}

static bool compute(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    NSError *error = nil;
    id<MTLFunction> function = [library newFunctionWithName:@"e5_square"];
    if (!function) { note("COMPUTE FAIL no function"); return false; }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) { note("COMPUTE FAIL pipeline %s", error.description.UTF8String); return false; }
    id<MTLBuffer> output = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (!output || !output.contents) { note("COMPUTE FAIL buffer"); return false; }
    memset(output.contents, 0xa5, output.length);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) { note("COMPUTE FAIL encoder"); return false; }
    [encoder setComputePipelineState:pipeline]; [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    if (!complete(command, "COMPUTE")) return false;
    uint32_t *values = (uint32_t *)output.contents; unsigned mismatch = 0;
    for (unsigned i = 0; i < 1024; ++i) mismatch += values[i] != i * i + 17;
    note("COMPUTE %s mismatch=%u first=%u last=%u fnv=%llx", mismatch ? "FAIL" : "PASS", mismatch,
         values[0], values[1023], (unsigned long long)fnv(values, 4096));
    return mismatch == 0;
}

static bool graphics(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    NSError *error = nil;
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"e5_triangle"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"e5_yellow"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) { note("GRAPHICS FAIL functions"); return false; }
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) { note("GRAPHICS FAIL pipeline %s", error.description.UTF8String); return false; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:320 height:480 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLBuffer> readback = [device newBufferWithLength:320 * 480 * 4 options:MTLResourceStorageModeShared];
    if (!target || !readback || !readback.contents) { note("GRAPHICS FAIL resources"); return false; }
    memset(readback.contents, 0xa5, readback.length);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { note("GRAPHICS FAIL encoder"); return false; }
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:(MTLViewport){0, 0, 320, 480, 0, 1}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) { note("GRAPHICS FAIL blit encoder"); return false; }
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(320, 480, 1) toBuffer:readback destinationOffset:0
        destinationBytesPerRow:1280 destinationBytesPerImage:320 * 480 * 4];
    [blit endEncoding];
    if (!complete(command, "GRAPHICS")) return false;
    const uint8_t *pixels = (const uint8_t *)readback.contents;
    unsigned mismatch = 0, inside = 0;
    for (int y = 0; y < 480; ++y) for (int x = 0; x < 320; ++x) {
        // Independent edge equations at doubled pixel centres; none lie on an edge.
        int px = 2 * x + 1, py = 2 * y + 1;
        bool yellow = py > 120 && 9 * px - 2 * py > 1200 && 9 * px + 2 * py < 4560;
        inside += yellow;
        const uint8_t *p = pixels + (y * 320 + x) * 4;
        mismatch += p[0] != 255 || p[1] != (yellow ? 255 : 0) || p[2] != 0 || p[3] != 255;
    }
    uint64_t hash = fnv(pixels, readback.length);
    bool ok = mismatch == 0 && inside == 28800 && hash == 0x417503084e2ac225ULL;
    note("GRAPHICS %s mismatch=%u inside=%u fnv=%llx", ok ? "PASS" : "FAIL", mismatch, inside, (unsigned long long)hash);
    return ok;
}

static char *hex64(char *p, uint64_t value) {
    for (int n = 60; n >= 0; n -= 4) *p++ = "0123456789abcdef"[(value >> n) & 15];
    return p;
}

static bool framebuffer_fetch(id<MTLDevice> device, id<MTLLibrary> library,
                              id<MTLCommandQueue> queue, bool integer, unsigned control = 0) {
    NSError *error = nil;
    const char *label = control == 2 ? "VARY constant" : control == 1 ? "VARY vertex-id" : integer ? "FETCH uint" : "FETCH unorm";
    MTLPixelFormat format = integer ? MTLPixelFormatRGBA8Uint : MTLPixelFormatRGBA8Unorm;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:control == 2 ? @"e5_vary_constant" : @"e5_fetch_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:control ? @"e5_vary_uint" : integer ? @"e5_fetch_uint" : @"e5_fetch_unorm"];
    pd.colorAttachments[0].pixelFormat = format;
    if (!pd.vertexFunction || !pd.fragmentFunction) return false;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) { note("%s FAIL pipeline %s", label, error.description.UTF8String); return false; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLBuffer> output = [device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    if (!target || !output || !output.contents) return false;
    memset(output.contents, 0xa5, output.length);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return false;
    [encoder setRenderPipelineState:pipeline]; [encoder setCullMode:MTLCullModeNone];
    [encoder setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
    // Eight fully overlapping primitives within each draw, two draws in the
    // same pass. The recurrence is non-commutative, so both ordering scopes
    // affect the expected result.
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:24];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:24 vertexCount:24];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) return false;
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(64, 64, 1) toBuffer:output destinationOffset:0
        destinationBytesPerRow:256 destinationBytesPerImage:16384];
    [blit endEncoding];
    if (!complete(command, "FETCH")) return false;
    unsigned red = 0;
    for (unsigned step = 0; step < 16; ++step)
        red = integer ? (red * 5 + step + 1) & 255 : (red * 32 + 255 * (step + 1) + 32) / 64;
    if (control) red = control == 2 ? 37 : 15;
    const unsigned green = control ? red : integer ? 15 : 0;
    const uint8_t *pixels = (const uint8_t *)output.contents; unsigned mismatch = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        const uint8_t *p = pixels + i * 4;
        mismatch += p[0] != red || p[1] != green || p[2] != 0 || p[3] != 255;
    }
    note("%s %s mismatch=%u expected=%u actual=%u,%u,%u,%u primitives=16 draws=2 fnv=%llx",
         label, mismatch ? "FAIL" : "PASS", mismatch, red,
         pixels[0], pixels[1], pixels[2], pixels[3], (unsigned long long)fnv(pixels, 16384));
    return mismatch == 0;
}

static bool import_rejections() {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
    io_connect_t connection = 0;
    if (!service) { note("IMPORT-REJECT FAIL no service"); return false; }
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (result) { note("IMPORT-REJECT FAIL open=%x", result); return false; }
    E5MaliInfo info = {}; size_t bytes = sizeof(info);
    bool ok = !IOConnectCallStructMethod(connection, kSelGetVersion, nullptr, 0, &info, &bytes) &&
              bytes == sizeof(info) && info.abiMajor == 1 && info.abiMinor >= 2;
    void *readonly = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    E5MaliImportUser requests[] = {
        {0, 4096, 0, 0}, {4096, 0, 0, 0}, {4097, 4096, 0, 0},
        {UINT64_MAX - 4095, 8192, 0, 0}, {4096, 4096, 2, 0},
        {(uintptr_t)readonly, 4096, 0, 0},
    };
    unsigned rejected = 0;
    if (ok && readonly != MAP_FAILED) for (const auto &request : requests) {
        E5MaliCreateBoOut made = {}; bytes = sizeof(made);
        result = IOConnectCallStructMethod(connection, kSelImportUser, &request, sizeof(request), &made, &bytes);
        if (result && !made.handle) ++rejected;
        else {
            note("IMPORT-REJECT unexpected rc=%x handle=%u", result, made.handle);
            if (made.handle) {
                E5MaliDestroyBo drop = {made.handle, 0};
                (void)IOConnectCallStructMethod(connection, kSelDestroyBo, &drop, sizeof(drop), nullptr, nullptr);
            }
        }
    }
    if (readonly != MAP_FAILED) munmap(readonly, 4096);
    E5MaliStats stats = {}; bytes = sizeof(stats);
    ok &= !IOConnectCallStructMethod(connection, kSelStats, nullptr, 0, &stats, &bytes) &&
          !stats.activeAtoms && !stats.mappedBytes && !stats.liveBoBytes && rejected == 6;
    IOServiceClose(connection);
    note("IMPORT-REJECT %s rejected=%u/6", ok ? "PASS" : "FAIL", rejected);
    return ok;
}

static bool no_copy(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue, bool native) {
    void *memory = nullptr;
    const size_t allocation = (size_t)getpagesize();
    if (allocation < 4096 || posix_memalign(&memory, allocation, allocation)) return false;
    io_connect_t alias = 0;
    if (native) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
        kern_return_t result = service ? IOServiceOpen(service, mach_task_self(), 0, &alias) : KERN_FAILURE;
        if (service) IOObjectRelease(service);
        E5MaliImportUser request = {(uintptr_t)memory, allocation, 0, 0};
        E5MaliCreateBoOut imported = {}; size_t bytes = sizeof(imported);
        if (!result) result = IOConnectCallStructMethod(alias, kSelImportUser, &request, sizeof(request), &imported, &bytes);
        if (result || !imported.handle) {
            if (alias) IOServiceClose(alias);
            free(memory); note("NO-COPY ALIAS FAIL rc=%x", result); return false;
        }
    }
    dispatch_semaphore_t released = dispatch_semaphore_create(0);
    bool ok = false;
    @autoreleasepool {
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:allocation
            options:MTLResourceStorageModeShared deallocator:^(void *p, NSUInteger) {
                free(p); dispatch_semaphore_signal(released);
            }];
        if (!buffer) { if (alias) IOServiceClose(alias); free(memory); note("NO-COPY FAIL buffer"); return false; }
        if (alias) { IOServiceClose(alias); alias = 0; note("NO-COPY ALIAS first connection closed"); }
        NSError *error = nil;
        id<MTLFunction> function = [library newFunctionWithName:@"e5_import_roundtrip"];
        id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (pipeline && buffer.contents == memory) {
            memset(memory, 0xa5, allocation);
            ok = true;
            for (unsigned round = 0; round < 2 && ok; ++round) {
                uint32_t *values = (uint32_t *)memory;
                for (unsigned i = 0; i < 1024; ++i) values[i] = i * 3 + round * 11 + 5;
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                if (!encoder) { ok = false; break; }
                [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:0 atIndex:0];
                [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [encoder endEncoding];
                if (!complete(command, "NO-COPY")) { ok = false; break; }
                unsigned mismatch = 0;
                for (unsigned i = 0; i < 1024; ++i) mismatch += values[i] != (i * 3 + round * 11 + 5) * 7 + 17;
                for (size_t i = 4096; i < allocation; ++i) mismatch += ((const uint8_t *)memory)[i] != 0xa5;
                ok = mismatch == 0;
                note("NO-COPY ROUND %u %s mismatch=%u fnv=%llx", round, ok ? "PASS" : "FAIL", mismatch,
                     (unsigned long long)fnv(memory, 4096));
            }
        } else note("NO-COPY FAIL pipeline/identity %s", error.description.UTF8String ?: "none");
    }
    bool freed = dispatch_semaphore_wait(released, dispatch_time(DISPATCH_TIME_NOW, 5000000000LL)) == 0;
    note("NO-COPY %s mismatch=%u rounds=2 identity=1", ok ? "PASS" : "FAIL", ok ? 0u : 1u);
    note("NO-COPY RELEASE %s", freed ? "PASS" : "FAIL");
    return ok && freed;
}

static bool cache_released() {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
    io_connect_t connection = 0;
    kern_return_t result = service ? IOServiceOpen(service, mach_task_self(), 0, &connection) : KERN_FAILURE;
    if (service) IOObjectRelease(service);
    if (result) return false;
    E5MaliStats stats = {}; size_t bytes = sizeof(stats);
    result = IOConnectCallStructMethod(connection, kSelStats, nullptr, 0, &stats, &bytes);
    IOServiceClose(connection);
    bool ok = !result && bytes == sizeof(stats) && stats.userCachePages == 0;
    note("CACHE-RELEASE %s held_pages=%llu", ok ? "PASS" : "FAIL", (unsigned long long)stats.userCachePages);
    return ok;
}

static bool atomic_shared(id<MTLDevice> device, id<MTLLibrary> library,
                          id<MTLCommandQueue> queue, bool native) {
    // The bitmap header and texels share VM pages, but occupy distinct cache
    // lines, just like CA::Render::Shmem's reference count and image bytes.
    if (native) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
        io_connect_t connection = 0;
        kern_return_t rc = service ? IOServiceOpen(service, mach_task_self(), 0, &connection) : KERN_FAILURE;
        if (service) IOObjectRelease(service);
        E5MaliInfo info = {}; size_t bytes = sizeof(info);
        if (!rc) rc = IOConnectCallStructMethod(connection, kSelGetVersion, nullptr, 0, &info, &bytes);
        if (connection) IOServiceClose(connection);
        if (rc || info.abiMinor < 5) { note("ATOMIC-SHARED FAIL needs ABI1.5 rc=%x", rc); return false; }
    }
    const size_t allocation = (size_t)getpagesize() * 2;
    vm_address_t original = 0, alias = 0;
    static_assert(sizeof(vm_address_t) == 8, "64-bit VM address API");
    memory_object_size_t sharedSize = allocation;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t rc = vm_allocate(mach_task_self(), &original, allocation,
                                       VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
    if (!rc) rc = mach_make_memory_entry_64(mach_task_self(), &sharedSize, original,
        VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_VM_SHARE | MAP_MEM_USE_DATA_ADDR,
        &entry, MACH_PORT_NULL);
    if (!rc) rc = vm_map(mach_task_self(), &alias, allocation, 0, VM_FLAGS_ANYWHERE,
        entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (entry) mach_port_deallocate(mach_task_self(), entry);
    if (rc || !original || !alias || alias == original) {
        if (alias) vm_deallocate(mach_task_self(), alias, allocation);
        if (original) vm_deallocate(mach_task_self(), original, allocation);
        note("ATOMIC-SHARED FAIL shared mapping rc=%x", rc); return false;
    }
    void *memory = (void *)(uintptr_t)original;
    memset(memory, 0xa5, allocation); __atomic_store_n((uint32_t *)memory, 0u, __ATOMIC_SEQ_CST);
    dispatch_semaphore_t released = dispatch_semaphore_create(0);
    bool ok = false, wrapped = false;
    @autoreleasepool {
        NSError *error = nil;
        id<MTLFunction> function = [library newFunctionWithName:@"e5_import_roundtrip"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)alias length:allocation
            options:MTLResourceStorageModeShared deallocator:^(void *p, NSUInteger n) {
                vm_deallocate(mach_task_self(), (uintptr_t)p, n);
                vm_deallocate(mach_task_self(), original, n);
                dispatch_semaphore_signal(released);
            }];
        wrapped = buffer != nil;
        ok = pipeline && buffer && buffer.contents == (void *)(uintptr_t)alias;
        if (ok) {
            // This exact LDAXR/STLXR access must remain legal while imported.
            ok = __atomic_fetch_add((uint32_t *)memory, 1u, __ATOMIC_SEQ_CST) == 0;
            note("ATOMIC-SHARED HEADER %s imported=1 shared_alias=1 purgeable=1", ok ? "PASS" : "FAIL");
        }
        for (unsigned round = 0; ok && round < 2; ++round) {
            uint32_t *texels = (uint32_t *)((uint8_t *)memory + 64);
            for (unsigned i = 0; i < 1024; ++i) texels[i] = i * 13 + round * 7919;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) { ok = false; break; }
            [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:64 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding]; [command commit];
            for (unsigned i = 0; i < 10000; ++i)
                __atomic_fetch_add((uint32_t *)memory, 1u, __ATOMIC_SEQ_CST);
            [command waitUntilCompleted];
            ok = command.status == MTLCommandBufferStatusCompleted && !command.error;
            unsigned mismatch = 0, guards = 0;
            if (ok) for (unsigned i = 0; i < 1024; ++i)
                mismatch += ((volatile const uint32_t *)texels)[i] != (i * 13 + round * 7919) * 7 + 17;
            for (size_t i = 4; i < 64; ++i) guards += ((const uint8_t *)memory)[i] != 0xa5;
            for (size_t i = 4160; i < allocation; ++i) guards += ((const uint8_t *)memory)[i] != 0xa5;
            uint32_t count = __atomic_load_n((uint32_t *)memory, __ATOMIC_SEQ_CST);
            ok &= !mismatch && !guards && count == 1 + 10000 * (round + 1);
            note("ATOMIC-SHARED ROUND %u %s mismatch=%u guards=%u count=%u explicit_flush=0",
                 round, ok ? "PASS" : "FAIL", mismatch, guards, count);
        }
        if (ok) {
            note("BUFFER-IMAGE BEGIN offset=64 row=256 size=64x16");
            uint32_t *texels = (uint32_t *)((uint8_t *)memory + 64);
            for (unsigned i = 0; i < 1024; ++i)
                texels[i] = 0xff000000u | (((i * 11) & 255) << 16) | (((i * 7) & 255) << 8) | (i & 255);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:64 height:16 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture = [buffer newTextureWithDescriptor:td offset:64 bytesPerRow:256];
            id<MTLFunction> reader = [library newFunctionWithName:@"e5_read_buffer_image"];
            id<MTLComputePipelineState> readPipeline = [device newComputePipelineStateWithFunction:reader error:&error];
            id<MTLBuffer> output = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            ok = texture && readPipeline && output && output.contents;
            note("BUFFER-IMAGE CREATED %s", ok ? "PASS" : "FAIL");
            if (ok) {
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                [encoder setComputePipelineState:readPipeline]; [encoder setTexture:texture atIndex:0];
                [encoder setBuffer:output offset:0 atIndex:0];
                [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [encoder endEncoding];
                note("BUFFER-IMAGE ENCODED");
                ok = complete(command, "BUFFER-IMAGE");
                unsigned mismatch = 0, guards = 0;
                if (ok) for (unsigned i = 0; i < 1024; ++i) {
                    uint32_t expected = 0xff000000u | (((i * 11) & 255) << 16) | (((i * 7) & 255) << 8) | (i & 255);
                    mismatch += ((volatile const uint32_t *)output.contents)[i] != expected;
                }
                for (size_t i = 4; i < 64; ++i) guards += ((const uint8_t *)memory)[i] != 0xa5;
                for (size_t i = 4160; i < allocation; ++i) guards += ((const uint8_t *)memory)[i] != 0xa5;
                uint32_t count = __atomic_add_fetch((uint32_t *)memory, 1u, __ATOMIC_SEQ_CST);
                ok &= !mismatch && !guards && count == 20002;
                note("BUFFER-IMAGE %s mismatch=%u guards=%u count=%u offset=64", ok ? "PASS" : "FAIL", mismatch, guards, count);
            }
        }
    }
    bool freed = !wrapped || dispatch_semaphore_wait(released, dispatch_time(DISPATCH_TIME_NOW, 5000000000LL)) == 0;
    if (!wrapped) {
        vm_deallocate(mach_task_self(), alias, allocation);
        vm_deallocate(mach_task_self(), original, allocation);
    }
    note("ATOMIC-SHARED %s release=%d", ok && freed ? "PASS" : "FAIL", freed);
    return ok && freed;
}

static bool readonly_compute(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    const size_t allocation = (size_t)getpagesize();
    void *memory = mmap(nullptr, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return false;
    dispatch_semaphore_t released = dispatch_semaphore_create(0);
    bool ok = true, wrapped = false;
    @autoreleasepool {
        NSError *error = nil;
        id<MTLFunction> function = [library newFunctionWithName:@"e5_readonly"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        id<MTLBuffer> input = nil;
        id<MTLBuffer> output = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        ok = pipeline && output && output.contents;
        for (unsigned round = 0; ok && round < 2; ++round) {
            ok = !mprotect(memory, allocation, PROT_READ | PROT_WRITE);
            if (!ok) break;
            for (unsigned i = 0; i < 1024; ++i) ((uint32_t *)memory)[i] = i * 13 + round * 7919;
            ok = !mprotect(memory, allocation, PROT_READ);
            if (!ok) break;
            if (!input) {
                input = [device newBufferWithBytesNoCopy:memory length:allocation
                    options:MTLResourceStorageModeShared deallocator:^(void *p, NSUInteger n) {
                        munmap(p, n); dispatch_semaphore_signal(released);
                    }];
                wrapped = input != nil;
            }
            ok = input && input.contents == memory;
            if (!ok) break;
            memset(output.contents, 0xa5, 4096);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) { ok = false; break; }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:output offset:0 atIndex:1];
            [encoder dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding];
            // Deliberately no CPU cache flush; submission must clean the RO source.
            ok = complete(command, "READONLY");
            unsigned mismatch = 0;
            if (ok) for (unsigned i = 0; i < 1024; ++i)
                mismatch += ((const uint32_t *)output.contents)[i] != (i * 13 + round * 7919) * 7 + 17;
            ok &= mismatch == 0;
            note("READONLY ROUND %u %s mismatch=%u identity=1 explicit_flush=0", round, ok ? "PASS" : "FAIL", mismatch);
        }
    }
    bool freed = !wrapped || dispatch_semaphore_wait(released, dispatch_time(DISPATCH_TIME_NOW, 5000000000LL)) == 0;
    if (!wrapped) munmap(memory, allocation);
    note("READONLY COMPUTE %s release=%d", ok && freed ? "PASS" : "FAIL", freed);
    return ok && freed;
}

// Run only after the Metal runtime closes: the expected MMU fault resets the GPU.
static bool readonly_write_fault() {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
    io_connect_t connection = 0;
    kern_return_t result = service ? IOServiceOpen(service, mach_task_self(), 0, &connection) : KERN_FAILURE;
    if (service) IOObjectRelease(service);
    if (result) return false;
    const size_t allocation = (size_t)getpagesize();
    void *memory = mmap(nullptr, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    E5MaliCreateBoOut source = {}, job = {};
    mach_vm_address_t cpu = 0; mach_vm_size_t cpuBytes = 0;
    E5MaliStats before = {}, after = {}; size_t bytes = sizeof(before);
    bool ok = false;
    do {
        E5MaliInfo info = {}; bytes = sizeof(info);
        if (IOConnectCallStructMethod(connection, kSelGetVersion, nullptr, 0, &info, &bytes) || info.abiMinor < 4) break;
        if (memory == MAP_FAILED) break;
        memset(memory, 0x35, allocation);
        if (mprotect(memory, allocation, PROT_READ)) break;
        E5MaliImportUser request = {(uintptr_t)memory, allocation, 0, 0}; bytes = sizeof(source);
        result = IOConnectCallStructMethod(connection, kSelImportUser, &request, sizeof(request), &source, &bytes);
        if (result != KERN_PROTECTION_FAILURE || source.handle) break;
        request.flags = E5_MALI_IMPORT_READ_ONLY; bytes = sizeof(source);
        result = IOConnectCallStructMethod(connection, kSelImportUser, &request, sizeof(request), &source, &bytes);
        if (result || source.status || !source.handle) break;
        E5MaliMapBo mapping = {source.handle, 0, 0, 0, allocation};
        E5MaliMapBoOut target = {}; bytes = sizeof(target);
        result = IOConnectCallStructMethod(connection, kSelMapBo, &mapping, sizeof(mapping), &target, &bytes);
        if (result != kIOReturnNotPermitted || target.va) break;
        mapping.flags = 2; bytes = sizeof(target);
        if (IOConnectCallStructMethod(connection, kSelMapBo, &mapping, sizeof(mapping), &target, &bytes) || target.status) break;
        E5MaliSubmit noexec = {}; noexec.jc = target.va; noexec.slot = 1; noexec.boHandles[0] = source.handle;
        E5MaliSubmitOut rejected = {}; bytes = sizeof(rejected);
        result = IOConnectCallStructMethod(connection, kSelSubmit, &noexec, sizeof(noexec), &rejected, &bytes);
        if (result != kIOReturnNotPermitted || rejected.atomId) break;
        note("READONLY POLICY PASS default_reject=2 explicit_import=1 executable_reject=1");
        E5MaliCreateBo create = {4096, 0}; bytes = sizeof(job);
        if (IOConnectCallStructMethod(connection, kSelCreateBo, &create, sizeof(create), &job, &bytes) || !job.handle) break;
        if (IOConnectMapMemory64(connection, job.handle, mach_task_self(), &cpu, &cpuBytes, kIOMapAnywhere) || cpuBytes < 64) break;
        mapping = {job.handle, 0, 0, 0, 4096}; E5MaliMapBoOut chain = {}; bytes = sizeof(chain);
        if (IOConnectCallStructMethod(connection, kSelMapBo, &mapping, sizeof(mapping), &chain, &bytes)) break;
        uint32_t *words = (uint32_t *)(uintptr_t)cpu; memset(words, 0, 64);
        words[4] = (2u << 1) | (1u << 16); // WRITE_VALUE, descriptor index 1.
        words[8] = (uint32_t)target.va; words[9] = (uint32_t)(target.va >> 32);
        words[10] = 6; words[12] = 0xdeadbeef; // Immediate 32-bit write to our own RO page.
        E5MaliCleanBo clean = {job.handle, 0};
        if (IOConnectCallStructMethod(connection, kSelCleanBo, &clean, sizeof(clean), nullptr, nullptr)) break;
        bytes = sizeof(before);
        if (IOConnectCallStructMethod(connection, kSelStats, nullptr, 0, &before, &bytes) || before.activeAtoms) break;
        E5MaliSubmit submit = {}; submit.jc = chain.va; submit.slot = 1;
        submit.boHandles[0] = job.handle; submit.boHandles[1] = source.handle;
        E5MaliSubmitOut submitted = {}; bytes = sizeof(submitted);
        if (IOConnectCallStructMethod(connection, kSelSubmit, &submit, sizeof(submit), &submitted, &bytes) || !submitted.atomId) break;
        E5MaliWaitAtom wait = {submitted.atomId, 5000, 0}; E5MaliWaitAtomOut waited = {}; bytes = sizeof(waited);
        result = IOConnectCallStructMethod(connection, kSelWaitAtom, &wait, sizeof(wait), &waited, &bytes);
        if (result || (waited.state != kAtomReset && waited.state != kAtomFault)) break;
        E5MaliCacheRange invalidate = {source.handle, E5_MALI_CACHE_INVALIDATE, 0, allocation};
        if (IOConnectCallStructMethod(connection, kSelCacheRange, &invalidate, sizeof(invalidate), nullptr, nullptr)) break;
        bytes = sizeof(after);
        if (IOConnectCallStructMethod(connection, kSelStats, nullptr, 0, &after, &bytes)) break;
        unsigned mismatch = 0;
        for (size_t i = 0; i < allocation; ++i) mismatch += ((const uint8_t *)memory)[i] != 0x35;
        const unsigned code = (unsigned)(after.lastMmuFaultCode & 255);
        ok = !mismatch && code >= 0xc8 && code <= 0xcb && after.lastMmuFaultAddr == target.va &&
             after.mmuFaults == before.mmuFaults + 1 && after.timedOut == before.timedOut && !after.activeAtoms;
        note("READONLY WRITE-FAULT %s code=%x state=%u addr=%llx expected=%llx changed=%u mmu_delta=%llu timeout_delta=%llu",
             ok ? "PASS" : "FAIL", code, waited.state, (unsigned long long)after.lastMmuFaultAddr,
             (unsigned long long)target.va, mismatch, (unsigned long long)(after.mmuFaults - before.mmuFaults),
             (unsigned long long)(after.timedOut - before.timedOut));
    } while (false);
    if (cpu) ok &= !IOConnectUnmapMemory64(connection, job.handle, mach_task_self(), cpu);
    const uint32_t handles[] = {job.handle, source.handle};
    for (uint32_t handle : handles) if (handle) {
        E5MaliDestroyBo drop = {handle, 0};
        ok &= !IOConnectCallStructMethod(connection, kSelDestroyBo, &drop, sizeof(drop), nullptr, nullptr);
    }
    bytes = sizeof(after);
    ok &= !IOConnectCallStructMethod(connection, kSelStats, nullptr, 0, &after, &bytes) &&
          !after.activeAtoms && !after.mappedBytes && !after.liveBoBytes && !after.userCachePages;
    IOServiceClose(connection);
    if (memory != MAP_FAILED) munmap(memory, allocation);
    note("READONLY NEGATIVE %s cleanup_bo=%u mapped=%u active=%u expected_mmu_faults=1", ok ? "PASS" : "FAIL",
         after.liveBoBytes, after.mappedBytes, after.activeAtoms);
    return ok;
}

static bool surface_test(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @64, (__bridge NSString *)kIOSurfaceHeight: @64,
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4, (__bridge NSString *)kIOSurfaceBytesPerRow: @256,
        (__bridge NSString *)kIOSurfaceAllocSize: @16384, (__bridge NSString *)kIOSurfacePixelFormat: @(0x42475241u),
    };
    struct Surface {
        IOSurfaceRef value;
        ~Surface() { if (value) CFRelease(value); }
    } surface{IOSurfaceCreate((__bridge CFDictionaryRef)properties)};
    if (!surface.value) { note("IOSURFACE FAIL create"); return false; }
    if (IOSurfaceLock(surface.value, 0, nullptr)) return false;
    void *memory = IOSurfaceGetBaseAddress(surface.value);
    if (memory) memset(memory, 0xa5, 16384);
    IOSurfaceUnlock(surface.value, 0, nullptr);
    if (!memory) return false;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:64 height:64 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surface.value plane:0];
    if (!texture) { note("IOSURFACE FAIL Metal texture"); return false; }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return false;
    [encoder endEncoding];
    if (!complete(command, "IOSURFACE")) return false;
    if (IOSurfaceLock(surface.value, kIOSurfaceLockReadOnly, nullptr)) return false;
    const uint8_t *pixels = (const uint8_t *)IOSurfaceGetBaseAddress(surface.value);
    unsigned mismatch = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        const uint8_t *p = pixels + i * 4;
        mismatch += p[0] != 0 || p[1] != 255 || p[2] != 0 || p[3] != 255;
    }
    uint64_t hash = fnv(pixels, 16384);
    IOSurfaceUnlock(surface.value, kIOSurfaceLockReadOnly, nullptr);
    note("IOSURFACE %s mismatch=%u fnv=%llx", mismatch ? "FAIL" : "PASS", mismatch, (unsigned long long)hash);
    return mismatch == 0;
}
static void fatal(int sig, siginfo_t *, void *context) {
    ucontext_t *uc = (ucontext_t *)context;
    char line[160] = "E5MTL FATAL sig="; char *p = line + strlen(line);
    p = hex64(p, sig); memcpy(p, " pc=", 4); p += 4;
    p = hex64(p, __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
    memcpy(p, " lr=", 4); p += 4;
    p = hex64(p, __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss)); *p++ = '\n';
    if (write(229, line, p - line) != p - line) (void)write(2, line, p - line);
    if (getpid() != 1) _exit(128 + sig);
    for (;;) pause();
}

int main(int argc, char **argv) {
    struct sigaction sa = {}; sa.sa_sigaction = fatal; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
    for (int sig : signals) sigaction(sig, &sa, NULL);
    bool reference = argc == 3 && !strcmp(argv[1], "--reference");
#ifdef E5_MTL_FETCH_TEST_ONLY
    const bool fetchOnly = true;
#else
    const bool fetchOnly = false;
#endif
#ifdef E5_MTL_READONLY_TEST_ONLY
    const bool readonlyOnly = true;
#else
    const bool readonlyOnly = false;
#endif
#ifdef E5_MTL_ATOMIC_TEST_ONLY
    const bool atomicOnly = true;
#else
    const bool atomicOnly = false;
#endif
    note("ENTRY pid=%d reference=%d", getpid(), reference);
    void *plugin = nullptr; bool (*shutdown)() = nullptr; size_t (*status)(char *, size_t) = nullptr;
    void (*trim)() = nullptr;
    bool ok = false;
    @autoreleasepool {
        @try {
            id<MTLDevice> device = nil;
            const char *metallib = reference ? argv[2] : "/private/var/mobile/e5gpu-stage/probe.metallib";
            if (reference) device = MTLCreateSystemDefaultDevice();
            else {
                setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
                if (fetchOnly) setenv("PANVK_E5_VARY_READBACK", "1", 1);
                const char *path = "/private/var/mobile/e5gpu-stage/NativeE5Metal.bundle/NativeE5Metal";
                plugin = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                if (!plugin) note("DLOPEN FAIL %s", dlerror());
                else {
                    typedef id (*Open)(const char *, const char *, unsigned);
                    Open open = (Open)dlsym(plugin, "native_e5_metal_open_for_test");
                    shutdown = (bool (*)())dlsym(plugin, "native_e5_metal_shutdown_for_test");
                    status = (size_t (*)(char *, size_t))dlsym(plugin, "native_bridge_metal_status");
                    trim = (void (*)())dlsym(plugin, "native_bridge_metal_trim_surface_imports");
                    NSString *cache = @"/private/var/mobile/e5gpu-stage/metal-cache";
                    NSError *error = nil;
                    bool ready = [[NSFileManager defaultManager] createDirectoryAtPath:cache
                        withIntermediateDirectories:YES attributes:nil error:&error];
                    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
                    if (open && shutdown && ready && service)
                        device = open("/private/var/mobile/e5gpu-stage/lib/libvulkan_panfrost.dylib", cache.fileSystemRepresentation, service);
                    else note("OPEN FAIL entry=%d shutdown=%d cache=%d service=%u", !!open, !!shutdown, ready, service);
                    if (service) IOObjectRelease(service);
                }
            }
            if (device) {
                note("DEVICE %s", device.name.UTF8String);
                NSError *error = nil;
                id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error];
                id<MTLCommandQueue> queue = [device newCommandQueue];
                if (library && queue) {
                    if (atomicOnly) ok = atomic_shared(device, library, queue, !reference) && surface_test(device, queue);
                    else if (readonlyOnly) ok = readonly_compute(device, library, queue);
                    else if (fetchOnly) {
                        const bool computeOK = compute(device, library, queue);
                        const bool constantOK = framebuffer_fetch(device, library, queue, true, 2);
                        const bool varyingOK = framebuffer_fetch(device, library, queue, true, 1);
                        const bool uintOK = framebuffer_fetch(device, library, queue, true);
                        const bool unormOK = framebuffer_fetch(device, library, queue, false);
                        ok = computeOK && constantOK && varyingOK && uintOK && unormOK;
                    }
                    else ok = (reference || import_rejections()) && compute(device, library, queue) &&
                        graphics(device, library, queue) && no_copy(device, library, queue, !reference) && surface_test(device, queue);
                }
                else note("LIBRARY/QUEUE FAIL %s", error.description.UTF8String ?: "none");
            } else note("OPEN FAIL no Metal device");
        } @catch (NSException *error) {
            note("EXCEPTION %s %s", error.name.UTF8String, error.reason.UTF8String);
        }
    }
    if (trim) trim();
    if (status) { char message[220] = {}; status(message, sizeof(message)); note("STATUS %s", message); }
    if (shutdown) { bool closed = shutdown(); note("CLOSE %s", closed ? "PASS" : "FAIL"); ok &= closed; }
    if (!reference) ok &= cache_released();
    if (readonlyOnly && !reference && ok) ok = readonly_write_fault();
    // Objective-C classes registered by the plug-in remain until exec; keep its image mapped.
    note("VERDICT %s", ok ? "PASS" : "FAIL");
    if (getpid() != 1) return ok ? 0 : 1;
    extern char **environ; char *args[] = {(char *)"/sbin/launchd", nullptr};
    note("HANDOFF launchd status=%d", ok ? 0 : 1); execve(args[0], args, environ);
    note("HANDOFF FAIL errno=%d", errno); for (;;) pause();
}
