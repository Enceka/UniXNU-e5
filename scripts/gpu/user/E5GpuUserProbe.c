// SPDX-License-Identifier: BSD-2-Clause-Patent
// Normal LC_MAIN entry; every result also uses the bounded E5 uplink.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../E5MaliG1Abi.h"
#include "../E5MaliG1Ext.h"

_Static_assert(sizeof(E5MaliInfo) == 72, "info ABI");
_Static_assert(sizeof(E5MaliSubmit) == 72, "submit ABI");
_Static_assert(sizeof(E5MaliSubmitMany) == 1056, "submit-many ABI1.6");
_Static_assert(sizeof(E5MaliGpuProperties) == 104, "properties ABI");
static io_connect_t connection;

static void note(const char *format, ...) {
    char line[252];
    int prefix = snprintf(line, sizeof(line), "E5GPUU ");
    va_list ap; va_start(ap, format);
    vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 2, format, ap);
    va_end(ap);
    size_t length = strlen(line); line[length++] = '\n';
    if (write(229, line, length) != (ssize_t)length) (void)write(2, line, length);
}

static kern_return_t call(uint32_t selector, const void *in, size_t in_size, void *out, size_t out_size) {
    size_t size = out_size;
    kern_return_t result = IOConnectCallStructMethod(connection, selector, in, in_size, out, out ? &size : NULL);
    if (result == KERN_SUCCESS && out && size != out_size) return kIOReturnUnderrun;
    return result;
}

struct buffer { uint32_t handle; uint64_t gpu, size; mach_vm_address_t cpu; mach_vm_size_t mapped; };

static int make_buffer(struct buffer *b, uint64_t size, int executable) {
    E5MaliCreateBo create = { .size = size, .flags = executable ? 0u : 1u };
    E5MaliCreateBoOut made = {0};
    kern_return_t rc = call(kSelCreateBo, &create, sizeof(create), &made, sizeof(made));
    if (rc || made.status || !made.handle) { note("CREATE FAIL rc=%x status=%x", rc, made.status); return 0; }
    b->handle = made.handle; b->size = made.size;
    E5MaliMapBo map = { .handle = b->handle, .flags = executable ? 0u : 2u };
    E5MaliMapBoOut mapped = {0};
    rc = call(kSelMapBo, &map, sizeof(map), &mapped, sizeof(mapped));
    if (rc || mapped.status) { note("MAPGPU FAIL rc=%x status=%x", rc, mapped.status); return 0; }
    b->gpu = mapped.va;
    rc = IOConnectMapMemory64(connection, b->handle, mach_task_self(), &b->cpu, &b->mapped, kIOMapAnywhere);
    if (rc || !b->cpu || b->mapped < b->size) { note("MAPCPU FAIL rc=%x size=%llu", rc, (unsigned long long)b->mapped); return 0; }
    note("BUFFER PASS handle=%u gpu=%llx bytes=%llu", b->handle, (unsigned long long)b->gpu, (unsigned long long)b->size);
    return 1;
}

static int cache(struct buffer *b, uint32_t operation) {
    E5MaliCacheRange in = { .handle = b->handle, .operation = operation, .offset = 0, .length = b->size };
    kern_return_t rc = call(kSelCacheRange, &in, sizeof(in), NULL, 0);
    if (rc) note("CACHE FAIL handle=%u op=%u rc=%x", b->handle, operation, rc);
    return rc == KERN_SUCCESS;
}

static int release_buffer(struct buffer *b) {
    int ok = 1;
    if (b->gpu) {
        E5MaliUnmapBo in = { .handle = b->handle, .va = b->gpu, .length = b->size };
        kern_return_t rc = call(kSelUnmapBo, &in, sizeof(in), NULL, 0);
        if (rc) { note("UNMAP FAIL handle=%u rc=%x", b->handle, rc); return 0; }
        b->gpu = 0;
    }
    if (b->cpu) {
        kern_return_t rc = IOConnectUnmapMemory64(connection, b->handle, mach_task_self(), b->cpu);
        ok &= rc == KERN_SUCCESS;
        if (!rc) b->cpu = 0;
    }
    if (b->handle) {
        E5MaliDestroyBo in = { .handle = b->handle };
        kern_return_t rc = call(kSelDestroyBo, &in, sizeof(in), NULL, 0);
        ok &= rc == KERN_SUCCESS;
        if (!rc) b->handle = 0;
    }
    return ok;
}

static uint64_t fnv64(const uint8_t *data, size_t bytes) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ data[i]) * 0x100000001b3ULL;
    return hash;
}

// Exact R339/R342 clear encoding: v9 fragment job, 128B framebuffer,
// RGB565 render target with tilebuffer-internal red (not memory RGB565).
static void clear_descriptors(struct buffer *desc, struct buffer *fb) {
    uint32_t *job = (uint32_t *)(uintptr_t)desc->cpu;
    uint32_t *fbd = (uint32_t *)((uint8_t *)job + 0x80);
    uint32_t *rt = (uint32_t *)((uint8_t *)job + 0x100);
    memset(job, 0, (size_t)desc->size);
    job[4] = (9u << 1) | (1u << 16);
    job[9] = 19u | (29u << 16);
    uint64_t tagged = (desc->gpu + 0x80) | 1u;
    job[10] = (uint32_t)tagged; job[11] = (uint32_t)(tagged >> 32);
    fbd[8] = fbd[10] = 319u | (479u << 16);
    fbd[11] = 0x01009000;
    rt[0] = 5u << 26;
    rt[1] = 1u | (21u << 3) | (2u << 8) | (1u << 15) | (0x80au << 16) | (1u << 31);
    rt[8] = (uint32_t)fb->gpu; rt[9] = (uint32_t)(fb->gpu >> 32);
    rt[10] = rt[11] = 640;
    for (unsigned i = 12; i < 16; ++i) rt[i] = 0x3e0;
}

static int run_probe(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("E5MaliG1"));
    if (!service) { note("OPEN FAIL service missing"); return 10; }
    kern_return_t rc = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (rc) { note("OPEN FAIL rc=%x", rc); return 11; }
    note("OPEN PASS");
    int ok = 1;
    struct buffer desc = {0}, fb = {0};
    struct buffer extra[62] = {{0}};
    E5MaliInfo info = {0};
    rc = call(kSelGetVersion, NULL, 0, &info, sizeof(info));
    ok = !rc && info.abiMajor == 1 && info.abiMinor >= 1 && info.gpuIdRaw == 0x90910010 && info.shaderPresent == 5;
    note("VERSION %s abi=%u.%u gpu=%08x shader=%llx", ok ? "PASS" : "FAIL", info.abiMajor, info.abiMinor, info.gpuIdRaw, (unsigned long long)info.shaderPresent);
    if (!ok) goto done;
    const size_t submit_bytes = info.abiMinor >= 6 ? sizeof(E5MaliSubmitMany) : E5_MALI_LEGACY_SUBMIT_MANY_BYTES;
    E5MaliGpuProperties properties = {0};
    rc = call(kSelGpuProperties, NULL, 0, &properties, sizeof(properties));
    ok = !rc && properties.gpuId == info.gpuIdRaw && properties.mmuFeatures == info.mmuFeatures && properties.maxSubmitBos >= 64;
    note("PROPS %s l2=%x tiler=%x mem=%x threads=%u wg=%u barrier=%u features=%x tls=%u", ok ? "PASS" : "FAIL", properties.l2Features, properties.tilerFeatures, properties.memFeatures, properties.threadMaxThreads, properties.threadMaxWorkgroup, properties.threadMaxBarrier, properties.threadFeatures, properties.threadTlsAlloc);
    note("TEXTURE features=%x:%x:%x:%x", properties.textureFeatures[0], properties.textureFeatures[1], properties.textureFeatures[2], properties.textureFeatures[3]);
    if (!ok || !make_buffer(&desc, 4096, 1) || !make_buffer(&fb, 320u * 480u * 2u, 0)) { ok = 0; goto done; }
    for (unsigned i = 0; i < 62; ++i)
        if (!make_buffer(&extra[i], 4096, 0)) { ok = 0; goto done; }
    E5MaliCacheRange invalid = { .handle = fb.handle, .operation = E5_MALI_CACHE_INVALIDATE,
                                .offset = fb.size, .length = 64 };
    rc = call(kSelCacheRange, &invalid, sizeof(invalid), NULL, 0);
    ok = rc == kIOReturnBadArgument;
    note("CACHE-BOUNDS %s rc=%x", ok ? "PASS" : "FAIL", rc);
    if (!ok) goto done;
    E5MaliSubmitMany too_many = { .jc = desc.gpu, .slot = 0, .handleCount = E5_MALI_MAX_SUBMIT_BOS + 1 };
    E5MaliSubmitOut rejected = {0};
    rc = call(kSelSubmitMany, &too_many, submit_bytes, &rejected, sizeof(rejected));
    ok = rc == kIOReturnBadArgument && !rejected.atomId;
    note("SUBMIT-BOUNDS %s rc=%x", ok ? "PASS" : "FAIL", rc);
    if (!ok) goto done;
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
        clear_descriptors(&desc, &fb);
        memset((void *)(uintptr_t)fb.cpu, 0x5a, (size_t)fb.size);
        if (!cache(&desc, E5_MALI_CACHE_CLEAN) || !cache(&fb, E5_MALI_CACHE_CLEAN)) { ok = 0; break; }
        E5MaliSubmitMany submit = { .jc = desc.gpu, .slot = 0, .handleCount = 64, .handles = { desc.handle, fb.handle } };
        for (unsigned i = 0; i < 62; ++i) submit.handles[i + 2] = extra[i].handle;
        E5MaliSubmitOut submitted = {0};
        rc = call(kSelSubmitMany, &submit, submit_bytes, &submitted, sizeof(submitted));
        if (rc || submitted.status || !submitted.atomId) { note("SUBMIT FAIL rc=%x status=%x", rc, submitted.status); ok = 0; break; }
        E5MaliDestroyBo pinned = { .handle = extra[61].handle };
        rc = call(kSelDestroyBo, &pinned, sizeof(pinned), NULL, 0);
        if (rc != kIOReturnBusy) { note("PIN64 FAIL rc=%x", rc); ok = 0; break; }
        E5MaliWaitAtom wait = { .atomId = submitted.atomId, .timeoutMs = 500 };
        E5MaliWaitAtomOut result = {0};
        rc = call(kSelWaitAtom, &wait, sizeof(wait), &result, sizeof(result));
        if (rc || result.status || result.state != kAtomDone || result.hwStatus != 1) { note("WAIT FAIL rc=%x status=%x state=%u hw=%x", rc, result.status, result.state, result.hwStatus); ok = 0; break; }
        if (!cache(&fb, E5_MALI_CACHE_INVALIDATE) || !cache(&desc, E5_MALI_CACHE_INVALIDATE)) { ok = 0; break; }
        const uint16_t *pixels = (const uint16_t *)(uintptr_t)fb.cpu;
        unsigned mismatch = 0;
        for (unsigned i = 0; i < 320u * 480u; ++i) mismatch += pixels[i] != 0xf800;
        uint64_t hash = fnv64((const uint8_t *)pixels, (size_t)fb.size);
        unsigned header = *(const uint32_t *)(uintptr_t)desc.cpu;
        ok = mismatch == 0 && hash == 0xe2c50ed21b7e2325ULL && header == 1;
        note("CLEAR %s repeat=%u mismatch=%u fnv=%016llx header=%x", ok ? "PASS" : "FAIL", repeat + 1, mismatch, (unsigned long long)hash, header);
        if (!ok) break;
    }
done:
    ok = release_buffer(&desc) && ok;
    ok = release_buffer(&fb) && ok;
    for (unsigned i = 0; i < 62; ++i) ok = release_buffer(&extra[i]) && ok;
    E5MaliStats stats = {0};
    rc = call(kSelStats, NULL, 0, &stats, sizeof(stats));
    ok = ok && !rc && stats.submitted == 8 && stats.completed == 8 && !stats.faults && !stats.timedOut && !stats.resets && !stats.mmuFaults && !stats.activeAtoms && !stats.mappedBytes && !stats.liveBoBytes;
    note("STATS sub=%llu comp=%llu fault=%llu tmo=%llu reset=%llu mmu=%llu active=%u mapped=%u bo=%u", (unsigned long long)stats.submitted, (unsigned long long)stats.completed, (unsigned long long)stats.faults, (unsigned long long)stats.timedOut, (unsigned long long)stats.resets, (unsigned long long)stats.mmuFaults, stats.activeAtoms, stats.mappedBytes, stats.liveBoBytes);
    IOServiceClose(connection);
    note("VERDICT %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 20;
}

int main(int argc, char **argv) {
    note("ENTRY pid=%d uid=%u argc=%d argv0=%s", getpid(), getuid(), argc, argc ? argv[0] : "none");
    int status = run_probe();
    if (getpid() != 1) return status;

    // The opt-in pre-launch probe must preserve PID 1 on every test outcome.
    // execve keeps the original kernel-supplied environment and never forks.
    extern char **environ;
    char *launchd_argv[] = { "/sbin/launchd", NULL };
    note("HANDOFF launchd probe_status=%d", status);
    execve(launchd_argv[0], launchd_argv, environ);
    note("HANDOFF FAIL errno=%d; retaining PID 1 until board watchdog", errno);
    for (;;) pause();
}
