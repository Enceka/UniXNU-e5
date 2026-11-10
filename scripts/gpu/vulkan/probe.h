// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stddef.h>

#define E5_VK_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) \
 X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
 X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) \
 X(DestroyPipelineLayout) X(CreateComputePipelines) X(CreateGraphicsPipelines) \
 X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdPipelineBarrier) \
 X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences) \
 X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
 X(CreateFramebuffer) X(DestroyFramebuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
 X(CmdDraw) X(CmdCopyImageToBuffer)

struct e5_vk_context {
    void *library;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties memory;
#define E5_DECLARE(name) PFN_vk##name name;
    E5_VK_FUNCTIONS(E5_DECLARE)
#undef E5_DECLARE
};
extern struct e5_vk_context vk;

struct e5_buffer { VkBuffer buffer; VkDeviceMemory memory; void *cpu; VkDeviceSize size; };
void note(const char *format, ...);
uint64_t fnv(const void *data, size_t bytes);
int buffer_create(struct e5_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage);
void buffer_destroy(struct e5_buffer *b);
VkResult buffer_sync(struct e5_buffer *b, int invalidate);
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags);
VkResult command_begin(VkCommandBuffer *command);
VkResult command_submit(VkCommandBuffer command);
int graphics_test(void);

#define TRY(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { \
    note("FAIL %s rc=%d line=%d", #call, result_, __LINE__); goto out; } } while (0)
