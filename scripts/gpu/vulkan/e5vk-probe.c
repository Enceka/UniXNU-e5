// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "probe.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include "square.inc"

struct e5_vk_context vk;

void note(const char *format, ...) {
    char line[252];
    int n = snprintf(line, sizeof(line), "E5VK ");
    va_list ap; va_start(ap, format);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 2, format, ap);
    va_end(ap);
    size_t size = strlen(line); line[size++] = '\n';
    if (write(229, line, size) != (ssize_t)size) (void)write(2, line, size);
}

uint64_t fnv(const void *data, size_t bytes) {
    const uint8_t *p = data; uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

int buffer_create(struct e5_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    int ok = 0;
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    TRY(vk.CreateBuffer(vk.device, &ci, NULL, &b->buffer));
    VkMemoryRequirements req; vk.GetBufferMemoryRequirements(vk.device, b->buffer, &req);
    uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX) { note("FAIL no host-visible buffer memory"); goto out; }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type };
    TRY(vk.AllocateMemory(vk.device, &ai, NULL, &b->memory));
    TRY(vk.BindBufferMemory(vk.device, b->buffer, b->memory, 0));
    TRY(vk.MapMemory(vk.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->cpu));
    b->size = size; ok = 1;
out:
    return ok;
}

void buffer_destroy(struct e5_buffer *b) {
    if (b->cpu) vk.UnmapMemory(vk.device, b->memory);
    if (b->buffer) vk.DestroyBuffer(vk.device, b->buffer, NULL);
    if (b->memory) vk.FreeMemory(vk.device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

VkResult buffer_sync(struct e5_buffer *b, int invalidate) {
    VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = b->memory, .offset = 0, .size = VK_WHOLE_SIZE };
    return invalidate ? vk.InvalidateMappedMemoryRanges(vk.device, 1, &range)
                      : vk.FlushMappedMemoryRanges(vk.device, 1, &range);
}

VkResult command_begin(VkCommandBuffer *command) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkResult r = vk.AllocateCommandBuffers(vk.device, &ai, command);
    if (r != VK_SUCCESS) return r;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    return vk.BeginCommandBuffer(*command, &bi);
}

VkResult command_submit(VkCommandBuffer command) {
    VkResult r = vk.EndCommandBuffer(command);
    if (r != VK_SUCCESS) return r;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    r = vk.CreateFence(vk.device, &fi, NULL, &fence);
    if (r != VK_SUCCESS) return r;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    r = vk.QueueSubmit(vk.queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = vk.WaitForFences(vk.device, 1, &fence, VK_TRUE, 5000000000ULL);
    vk.DestroyFence(vk.device, fence, NULL);
    return r;
}

static int setup(void) {
    int ok = 0;
    setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
    setenv("PANVK_DEBUG", "startup", 1);
    const char *path = "/private/var/mobile/e5gpu-stage/lib/libvulkan_panfrost.dylib";
    note("DLOPEN %s", path);
    vk.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!vk.library) { note("DLOPEN FAIL %s", dlerror()); return 0; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk.library, "vk_icdGetInstanceProcAddr");
    Dl_info image = {0};
    if (gipa && dladdr((const void *)gipa, &image))
        note("ICD base=%p entry=%p", image.dli_fbase, (void *)gipa);
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    if (!create) { note("FAIL no ICD instance entry"); return 0; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "E5 native GPU qualification", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    TRY(create(&ci, NULL, &vk.instance));
#define E5_LOAD(name) vk.name = (PFN_vk##name)gipa(vk.instance, "vk" #name); \
    if (!vk.name) { note("FAIL missing vk%s", #name); goto out; }
    E5_VK_FUNCTIONS(E5_LOAD)
#undef E5_LOAD
    uint32_t count = 0;
    TRY(vk.EnumeratePhysicalDevices(vk.instance, &count, NULL));
    if (count != 1) { note("FAIL physical devices=%u", count); goto out; }
    TRY(vk.EnumeratePhysicalDevices(vk.instance, &count, &vk.physical));
    VkPhysicalDeviceProperties props; vk.GetPhysicalDeviceProperties(vk.physical, &props);
    note("DEVICE %s vendor=%x device=%x api=%x", props.deviceName, props.vendorID, props.deviceID, props.apiVersion);
    note("CACHE atom=%llu", (unsigned long long)props.limits.nonCoherentAtomSize);
    vk.GetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    uint32_t nq = 0; vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical, &nq, NULL);
    if (nq > 8) { note("FAIL too many queue families"); goto out; }
    VkQueueFamilyProperties qp[8]; vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical, &nq, qp);
    vk.family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
        if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { vk.family = i; break; }
    if (vk.family == UINT32_MAX) { note("FAIL no graphics/compute queue"); goto out; }
    float priority = 0.5f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.family, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
    TRY(vk.CreateDevice(vk.physical, &di, NULL, &vk.device));
    vk.GetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = vk.family };
    TRY(vk.CreateCommandPool(vk.device, &pi, NULL, &vk.pool));
    note("SETUP PASS"); ok = 1;
out:
    return ok;
}

static int compute_test(void) {
    int ok = 0;
    struct e5_buffer output = {0};
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!buffer_create(&output, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) goto out;
    memset(output.cpu, 0xa5, 4096); TRY(buffer_sync(&output, 0));
    VkShaderModuleCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(square_spv), .pCode = square_spv };
    TRY(vk.CreateShaderModule(vk.device, &sci, NULL, &shader));
    VkDescriptorSetLayoutBinding binding = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo sli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    TRY(vk.CreateDescriptorSetLayout(vk.device, &sli, NULL, &set_layout));
    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    TRY(vk.CreateDescriptorPool(vk.device, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
    VkDescriptorSet set; TRY(vk.AllocateDescriptorSets(vk.device, &sai, &set));
    VkDescriptorBufferInfo bi = { .buffer = output.buffer, .offset = 0, .range = 4096 };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bi };
    vk.UpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout };
    TRY(vk.CreatePipelineLayout(vk.device, &pli, NULL, &layout));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
        .layout = layout };
    TRY(vk.CreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline));
    TRY(command_begin(&command));
    vk.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vk.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vk.CmdDispatch(command, 16, 1, 1);
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    TRY(command_submit(command)); TRY(buffer_sync(&output, 1));
    unsigned mismatch = 0; uint32_t *values = output.cpu;
    for (unsigned i = 0; i < 1024; ++i) mismatch += values[i] != i * i + 17;
    ok = mismatch == 0;
    note("COMPUTE %s mismatch=%u first=%u last=%u fnv=%llx", ok ? "PASS" : "FAIL", mismatch,
         values[0], values[1023], (unsigned long long)fnv(output.cpu, 4096));
out:
    if (command) vk.FreeCommandBuffers(vk.device, vk.pool, 1, &command);
    if (pipeline) vk.DestroyPipeline(vk.device, pipeline, NULL);
    if (layout) vk.DestroyPipelineLayout(vk.device, layout, NULL);
    if (pool) vk.DestroyDescriptorPool(vk.device, pool, NULL);
    if (set_layout) vk.DestroyDescriptorSetLayout(vk.device, set_layout, NULL);
    if (shader) vk.DestroyShaderModule(vk.device, shader, NULL);
    buffer_destroy(&output);
    return ok;
}

static char *hex64(char *p, uint64_t value) {
    const char digits[] = "0123456789abcdef";
    for (int n = 60; n >= 0; n -= 4) *p++ = digits[(value >> n) & 15];
    return p;
}

static void fatal_signal(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = context;
    char line[160] = "E5VK FATAL sig="; char *p = line + 15;
    p = hex64(p, (unsigned)sig); *p++ = ' '; *p++ = 'p'; *p++ = 'c'; *p++ = '=';
    p = hex64(p, __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
    *p++ = ' '; *p++ = 'l'; *p++ = 'r'; *p++ = '=';
    p = hex64(p, __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss)); *p++ = '\n';
    (void)write(229, line, (size_t)(p - line));
    if (getpid() != 1) _exit(128 + sig);
    // Keep mappings and PID 1 alive after a fatal library fault. The board's
    // bounded watchdog recovers Android; no unsafe teardown or init exit.
    for (;;) pause();
}

int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = fatal_signal; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
    for (unsigned i = 0; i < sizeof(signals)/sizeof(signals[0]); ++i) sigaction(signals[i], &sa, NULL);
    note("ENTRY pid=%d", getpid());
    int ok = setup();
    if (ok) ok = compute_test();
    if (ok) ok = graphics_test();
    if (vk.device) {
        VkResult idle = vk.DeviceWaitIdle(vk.device);
        note("IDLE rc=%d", idle); ok &= idle == VK_SUCCESS;
        if (vk.pool) vk.DestroyCommandPool(vk.device, vk.pool, NULL);
        vk.DestroyDevice(vk.device, NULL);
    }
    if (vk.instance && vk.DestroyInstance) vk.DestroyInstance(vk.instance, NULL);
    if (vk.library) dlclose(vk.library);
    note("VERDICT %s", ok ? "PASS" : "FAIL");
    if (getpid() != 1) return ok ? 0 : 1;
    extern char **environ;
    char *args[] = { "/sbin/launchd", NULL };
    note("HANDOFF launchd status=%d", ok ? 0 : 1);
    execve(args[0], args, environ);
    note("HANDOFF FAIL errno=%d", errno);
    for (;;) pause();
}
