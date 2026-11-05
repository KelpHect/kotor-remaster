#include "render/VulkanPost.h"

#include "core/Log.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ke {

namespace {

struct PushConstants {
    float row0[4]{};
    float row1[4]{};
    float row2[4]{};
    float noiseScale[4]{};
    float noiseTranslate[4]{};
    float flags[4]{};
};

std::vector<char> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(bytes.data(), size)) {
        return {};
    }
    return bytes;
}

std::filesystem::path shaderDirectory()
{
    const char* base = SDL_GetBasePath();
    return std::filesystem::path(base != nullptr ? base : ".") / "shaders";
}

VkShaderModule makeModule(VkDevice device, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module
                                                                               : VK_NULL_HANDLE;
}

} // namespace

std::uint32_t VulkanPost::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0
            && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanPost::createImage(Image& target, std::uint32_t width, std::uint32_t height,
                             VkFormat format, VkImageUsageFlags usage, std::string& error)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &info, nullptr, &target.image) != VK_SUCCESS) {
        error = "vkCreateImage failed for the video effect pass";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, target.image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &allocate, nullptr, &target.memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for the video effect pass";
        return false;
    }
    vkBindImageMemory(device_, target.image, target.memory, 0);
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &target.view) != VK_SUCCESS) {
        error = "vkCreateImageView failed for the video effect pass";
        return false;
    }
    return true;
}

void VulkanPost::destroyImage(Image& target)
{
    if (target.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, target.view, nullptr);
    }
    if (target.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, target.image, nullptr);
    }
    if (target.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, target.memory, nullptr);
    }
    target = {};
}

bool VulkanPost::uploadNoise(std::string& error)
{
    const std::vector<std::uint8_t> pixels = makeScanNoiseTexture();
    if (!createImage(noise_, kScanNoiseSize, kScanNoiseSize, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, error)) {
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = pixels.size();
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) {
        error = "vkCreateBuffer failed for the scan-noise texture";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &allocate, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        error = "vkAllocateMemory failed for the scan-noise staging buffer";
        return false;
    }
    vkBindBufferMemory(device_, staging, stagingMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMemory, 0, pixels.size(), 0, &mapped);
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(device_, stagingMemory);

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(device_, &commandInfo, &command) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command, &begin);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = noise_.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kScanNoiseSize, kScanNoiseSize, 1};
        vkCmdCopyBufferToImage(command, staging, noise_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        vkEndCommandBuffer(command);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        ok = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
    }
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    if (!ok) {
        error = "scan-noise texture upload failed";
    }
    return ok;
}

bool VulkanPost::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                            VkCommandPool commandPool, VkQueue queue, VkFormat colorFormat,
                            std::string& error)
{
    device_ = device;
    physicalDevice_ = physicalDevice;
    commandPool_ = commandPool;
    queue_ = queue;
    colorFormat_ = colorFormat;

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed for the video effect pass";
        return false;
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed for the video effect pass";
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &clampSampler_) != VK_SUCCESS) {
        error = "vkCreateSampler failed for the video effect pass";
        return false;
    }
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &repeatSampler_) != VK_SUCCESS) {
        error = "vkCreateSampler(repeat) failed for the video effect pass";
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed for the video effect pass";
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout_;
    if (vkAllocateDescriptorSets(device_, &allocate, &descriptor_) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed for the video effect pass";
        return false;
    }

    const std::filesystem::path dir = shaderDirectory();
    const std::vector<char> vertexCode = readFile(dir / "effect.vert.spv");
    const std::vector<char> fragmentCode = readFile(dir / "effect.frag.spv");
    if (vertexCode.empty() || fragmentCode.empty()) {
        error = "effect SPIR-V shaders missing under " + dir.string();
        return false;
    }
    const VkShaderModule vertex = makeModule(device_, vertexCode);
    const VkShaderModule fragment = makeModule(device_, fragmentCode);
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, vertex, nullptr);
        }
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, fragment, nullptr);
        }
        error = "vkCreateShaderModule failed for the video effect pass";
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat_;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    const VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    if (result != VK_SUCCESS) {
        error = concat("vkCreateGraphicsPipelines(effect) failed: ", static_cast<int>(result));
        return false;
    }
    return uploadNoise(error);
}

void VulkanPost::updateDescriptor()
{
    if (descriptor_ == VK_NULL_HANDLE || copy_.view == VK_NULL_HANDLE
        || noise_.view == VK_NULL_HANDLE) {
        return;
    }
    VkDescriptorImageInfo images[2]{};
    images[0] = {clampSampler_, copy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[1] = {repeatSampler_, noise_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
}

bool VulkanPost::resize(VkExtent2D extent, std::string& error)
{
    if (device_ == VK_NULL_HANDLE || pipeline_ == VK_NULL_HANDLE) {
        return true; // effect pass unavailable; frames stay single-pass
    }
    vkDeviceWaitIdle(device_);
    destroyImage(copy_);
    extent_ = extent;
    if (extent.width == 0 || extent.height == 0) {
        return true;
    }
    if (!createImage(copy_, extent.width, extent.height, colorFormat_,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, error)) {
        return false;
    }
    updateDescriptor();
    return true;
}

void VulkanPost::recordCopy(VkCommandBuffer commandBuffer, VkImage swapchainImage)
{
    VkImageMemoryBarrier barriers[2]{};
    for (VkImageMemoryBarrier& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    // swapchain: attachment -> transfer source; copy: (discard) -> transfer destination.
    barriers[0].image = swapchainImage;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].image = copy_.image;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);
}

void VulkanPost::recordDraw(VkCommandBuffer commandBuffer)
{
    PushConstants push;
    const auto& m = effect_.matrix;
    for (int i = 0; i < 3; ++i) {
        push.row0[i] = m[static_cast<std::size_t>(i)];
        push.row1[i] = m[static_cast<std::size_t>(3 + i)];
        push.row2[i] = m[static_cast<std::size_t>(6 + i)];
        push.noiseScale[i] = effect_.noiseScale[static_cast<std::size_t>(i)];
        push.noiseTranslate[i] = effect_.noiseTranslate[static_cast<std::size_t>(i)];
    }
    push.flags[0] = effect_.saturation ? 1.0f : 0.0f;
    push.flags[1] = effect_.scanNoise ? 1.0f : 0.0f;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &descriptor_, 0, nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanPost::shutdown()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    destroyImage(copy_);
    destroyImage(noise_);
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptor_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    if (clampSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, clampSampler_, nullptr);
        clampSampler_ = VK_NULL_HANDLE;
    }
    if (repeatSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, repeatSampler_, nullptr);
        repeatSampler_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

} // namespace ke
