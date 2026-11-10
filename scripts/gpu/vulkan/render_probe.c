// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "probe.h"
#include <string.h>
#include "triangle.inc"
#include "yellow.inc"

static int inside(unsigned x, unsigned y) {
    const int v[3][2] = {{80,60}, {240,60}, {160,420}};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned j = (i + 1) % 3;
        int edge = (v[j][0] - v[i][0]) * ((int)y * 2 + 1 - v[i][1] * 2)
                 - (v[j][1] - v[i][1]) * ((int)x * 2 + 1 - v[i][0] * 2);
        if (edge < 0) return 0;
    }
    return 1;
}

int graphics_test(void) {
    int ok = 0;
    struct e5_buffer readback = {0};
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    const unsigned width = 320, height = 480;
    if (!buffer_create(&readback, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) goto out;
    memset(readback.cpu, 0xa5, (size_t)readback.size); TRY(buffer_sync(&readback, 0));
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width,height,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    TRY(vk.CreateImage(vk.device, &ici, NULL, &image));
    VkMemoryRequirements req; vk.GetImageMemoryRequirements(vk.device, image, &req);
    uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { note("FAIL no device-local image memory"); goto out; }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type };
    TRY(vk.AllocateMemory(vk.device, &ai, NULL, &memory));
    TRY(vk.BindImageMemory(vk.device, image, memory, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ici.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    TRY(vk.CreateImageView(vk.device, &vi, NULL, &view));
    VkAttachmentDescription attachment = { .format = ici.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference color = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color };
    VkSubpassDependency dependencies[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT }
    };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 2, .pDependencies = dependencies };
    TRY(vk.CreateRenderPass(vk.device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = width, .height = height, .layers = 1 };
    TRY(vk.CreateFramebuffer(vk.device, &fi, NULL, &framebuffer));
    VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(triangle_spv), .pCode = triangle_spv };
    TRY(vk.CreateShaderModule(vk.device, &si, NULL, &vertex));
    si.codeSize = sizeof(yellow_spv); si.pCode = yellow_spv;
    TRY(vk.CreateShaderModule(vk.device, &si, NULL, &fragment));
    VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    TRY(vk.CreatePipelineLayout(vk.device, &li, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main" }
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = {0,0,(float)width,(float)height,0,1};
    VkRect2D scissor = { .extent = {width,height} };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo blending = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &vp, .pRasterizationState = &raster,
        .pMultisampleState = &ms, .pColorBlendState = &blending,
        .layout = layout, .renderPass = pass, .subpass = 0 };
    TRY(vk.CreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
    TRY(command_begin(&command));
    VkClearValue clear = { .color = { .float32 = {1,0,0,1} } };
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = scissor,
        .clearValueCount = 1, .pClearValues = &clear };
    vk.CmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vk.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk.CmdDraw(command, 3, 1, 0, 0);
    vk.CmdEndRenderPass(command);
    VkBufferImageCopy copy = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent = {width,height,1} };
    vk.CmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    TRY(command_submit(command)); TRY(buffer_sync(&readback, 1));
    unsigned mismatch = 0, expected_inside = 0, actual_yellow = 0;
    const uint8_t *pixels = readback.cpu;
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned index = (y * width + x) * 4;
        int in = inside(x, y); expected_inside += (unsigned)in;
        uint8_t expected[4] = {255,in ? 255 : 0,0,255};
        mismatch += memcmp(pixels + index, expected, 4) != 0;
        actual_yellow += pixels[index] == 255 && pixels[index + 1] == 255 && pixels[index + 2] == 0 && pixels[index + 3] == 255;
    }
    uint64_t hash = fnv(pixels, (size_t)readback.size);
    ok = mismatch == 0 && expected_inside == 28800 && actual_yellow == 28800 && hash == 0x417503084e2ac225ULL;
    note("TRIANGLE %s mismatch=%u inside=%u yellow=%u fnv=%llx", ok ? "PASS" : "FAIL",
         mismatch, expected_inside, actual_yellow, (unsigned long long)hash);
out:
    if (command) vk.FreeCommandBuffers(vk.device, vk.pool, 1, &command);
    if (pipeline) vk.DestroyPipeline(vk.device, pipeline, NULL);
    if (layout) vk.DestroyPipelineLayout(vk.device, layout, NULL);
    if (vertex) vk.DestroyShaderModule(vk.device, vertex, NULL);
    if (fragment) vk.DestroyShaderModule(vk.device, fragment, NULL);
    if (framebuffer) vk.DestroyFramebuffer(vk.device, framebuffer, NULL);
    if (pass) vk.DestroyRenderPass(vk.device, pass, NULL);
    if (view) vk.DestroyImageView(vk.device, view, NULL);
    if (image) vk.DestroyImage(vk.device, image, NULL);
    if (memory) vk.FreeMemory(vk.device, memory, NULL);
    buffer_destroy(&readback);
    return ok;
}
