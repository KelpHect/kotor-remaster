#include "render/VulkanScene.h"

#include "core/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>

namespace ke {

namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Mat4 {
    float m[16]{};
};

Mat4 perspective(float fovYDegrees, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovYDegrees * 3.14159265358979323846f / 360.0f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return out;
}

Mat4 lookAt(kotor::Vec3 eye, kotor::Vec3 forward, kotor::Vec3 up)
{
    const kotor::Vec3 f = kotor::normalize(forward);
    const kotor::Vec3 s = kotor::normalize(kotor::cross(f, up));
    const kotor::Vec3 u = kotor::cross(s, f);
    Mat4 out;
    out.m[0] = s.x; out.m[4] = s.y; out.m[8] = s.z; out.m[12] = -kotor::dot(s, eye);
    out.m[1] = u.x; out.m[5] = u.y; out.m[9] = u.z; out.m[13] = -kotor::dot(u, eye);
    out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z; out.m[14] = kotor::dot(f, eye);
    out.m[3] = 0.0f; out.m[7] = 0.0f; out.m[11] = 0.0f; out.m[15] = 1.0f;
    return out;
}

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

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

} // namespace

std::uint32_t VulkanScene::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0
            && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("no Vulkan memory type for scene resource");
}

bool VulkanScene::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                               VkBuffer& buffer, VkDeviceMemory& memory, std::string& error)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    try {
        allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (vkAllocateMemory(device_, &allocate, nullptr, &memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for scene buffer";
        return false;
    }
    vkBindBufferMemory(device_, buffer, memory, 0);
    return true;
}

bool VulkanScene::initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
                             VkQueue queue, VkFormat colorFormat, std::string& error)
{
    device_ = device;
    physicalDevice_ = physicalDevice;
    commandPool_ = commandPool;
    queue_ = queue;
    colorFormat_ = colorFormat;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    if (properties.limits.maxPushConstantsSize < sizeof(PushConstants)) {
        error = concat("device maxPushConstantsSize ", properties.limits.maxPushConstantsSize,
                       " is below the scene block ", sizeof(PushConstants));
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }
    return createPipeline(error);
}

bool VulkanScene::createPipeline(std::string& error)
{
    const std::filesystem::path dir = shaderDirectory();
    const std::vector<char> vertexCode = readFile(dir / "scene.vert.spv");
    const std::vector<char> fragmentCode = readFile(dir / "scene.frag.spv");
    if (vertexCode.empty() || fragmentCode.empty()) {
        error = "scene SPIR-V shaders missing under " + dir.string();
        return false;
    }
    VkShaderModuleCreateInfo vertexInfo{};
    vertexInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexInfo.codeSize = vertexCode.size();
    vertexInfo.pCode = reinterpret_cast<const std::uint32_t*>(vertexCode.data());
    VkShaderModule vertex = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &vertexInfo, nullptr, &vertex) != VK_SUCCESS) {
        error = "vkCreateShaderModule(vertex) failed";
        return false;
    }
    VkShaderModuleCreateInfo fragmentInfo = vertexInfo;
    fragmentInfo.codeSize = fragmentCode.size();
    fragmentInfo.pCode = reinterpret_cast<const std::uint32_t*>(fragmentCode.data());
    VkShaderModule fragment = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &fragmentInfo, nullptr, &fragment) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vertex, nullptr);
        error = "vkCreateShaderModule(fragment) failed";
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

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(kotor::SceneVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, position))};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, normal))};
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, uv))};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

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
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
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
    rendering.depthAttachmentFormat = kDepthFormat;

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
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    if (result != VK_SUCCESS) {
        error = concat("vkCreateGraphicsPipelines(scene) failed: ", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanScene::createTexture(const kotor::Texture& texture, GpuTexture& gpu, std::string& error)
{
    const kotor::Image& image = texture.image;
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        error = "empty scene texture " + texture.name;
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBuffer(image.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMemory, error)) {
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMemory, 0, image.rgba.size(), 0, &mapped);
    std::memcpy(mapped, image.rgba.data(), image.rgba.size());
    vkUnmapMemory(device_, stagingMemory);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &gpu.image) != VK_SUCCESS) {
        error = "vkCreateImage failed for " + texture.name;
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, gpu.image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &allocate, nullptr, &gpu.memory);
    vkBindImageMemory(device_, gpu.image, gpu.memory, 0);

    const auto oneTime = [&](const std::function<void(VkCommandBuffer)>& body) -> bool {
        VkCommandBufferAllocateInfo allocateCmd{};
        allocateCmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateCmd.commandPool = commandPool_;
        allocateCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateCmd.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &allocateCmd, &command) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command, &begin);
        body(command);
        vkEndCommandBuffer(command);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        return true;
    };
    const bool uploaded = oneTime([&](VkCommandBuffer command) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = gpu.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 1};
        vkCmdCopyBufferToImage(command, staging, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    });
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    if (!uploaded) {
        error = "scene texture upload command failed";
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = gpu.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &viewInfo, nullptr, &gpu.view) == VK_SUCCESS;
}

void VulkanScene::destroyTextures()
{
    for (GpuTexture& texture : textures_) {
        if (texture.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, texture.view, nullptr);
        }
        if (texture.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, texture.image, nullptr);
        }
        if (texture.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, texture.memory, nullptr);
        }
    }
    textures_.clear();
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
}

VulkanScene::GpuMesh VulkanScene::uploadMesh(const std::vector<kotor::SceneVertex>& vertices,
                                             const std::vector<std::uint32_t>& indices,
                                             const std::vector<kotor::TextureBatch>& batches,
                                             std::uint32_t entity, std::string& error)
{
    GpuMesh mesh;
    mesh.entity = entity;
    mesh.batches = batches;
    if (vertices.empty() || indices.empty()) {
        return mesh;
    }
    if (!createBuffer(vertices.size() * sizeof(kotor::SceneVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      mesh.vertexBuffer, mesh.vertexMemory, error)
        || !createBuffer(indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         mesh.indexBuffer, mesh.indexMemory, error)) {
        destroyMesh(mesh);
        return {};
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, mesh.vertexMemory, 0, vertices.size() * sizeof(kotor::SceneVertex), 0, &mapped)
        != VK_SUCCESS) {
        error = "vkMapMemory(scene vertices) failed";
        destroyMesh(mesh);
        return {};
    }
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(kotor::SceneVertex));
    vkUnmapMemory(device_, mesh.vertexMemory);
    if (vkMapMemory(device_, mesh.indexMemory, 0, indices.size() * sizeof(std::uint32_t), 0, &mapped)
        != VK_SUCCESS) {
        error = "vkMapMemory(scene indices) failed";
        destroyMesh(mesh);
        return {};
    }
    std::memcpy(mapped, indices.data(), indices.size() * sizeof(std::uint32_t));
    vkUnmapMemory(device_, mesh.indexMemory);
    return mesh;
}

void VulkanScene::destroyMesh(GpuMesh& mesh)
{
    if (mesh.vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, mesh.vertexBuffer, nullptr);
        mesh.vertexBuffer = VK_NULL_HANDLE;
    }
    if (mesh.vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, mesh.vertexMemory, nullptr);
        mesh.vertexMemory = VK_NULL_HANDLE;
    }
    if (mesh.indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, mesh.indexBuffer, nullptr);
        mesh.indexBuffer = VK_NULL_HANDLE;
    }
    if (mesh.indexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, mesh.indexMemory, nullptr);
        mesh.indexMemory = VK_NULL_HANDLE;
    }
}

bool VulkanScene::upload(const kotor::Scene& scene, std::string& error)
{
    if (pipeline_ == VK_NULL_HANDLE) {
        error = "Vulkan scene pipeline is not initialized";
        return false;
    }
    scene_ = &scene;
    ambient_ = scene.ambient;
    diffuse_ = scene.diffuse;
    fogColor_ = scene.fogColor;
    fogNear_ = scene.fogNear;
    fogFar_ = scene.fogFar;

    for (GpuMesh& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
    destroyTextures();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = static_cast<std::uint32_t>(scene.textures.size());
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(scene.textures.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    textures_.resize(scene.textures.size());
    for (std::size_t i = 0; i < scene.textures.size(); ++i) {
        if (!createTexture(scene.textures[i], textures_[i], error)) {
            return false;
        }
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptorSetLayout_;
        if (vkAllocateDescriptorSets(device_, &allocate, &textures_[i].descriptor) != VK_SUCCESS) {
            error = "vkAllocateDescriptorSets failed";
            return false;
        }
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = textures_[i].view;
        imageInfo.sampler = sampler_;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = textures_[i].descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    GpuMesh room = uploadMesh(scene.vertices, scene.indices, scene.batches, UINT32_MAX, error);
    if (room.vertexBuffer == VK_NULL_HANDLE) {
        return false;
    }
    meshes_.push_back(std::move(room));
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        GpuMesh mesh = uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                  entityMesh.entity, error);
        if (mesh.vertexBuffer != VK_NULL_HANDLE) {
            meshes_.push_back(std::move(mesh));
        }
    }
    return true;
}

bool VulkanScene::createDepth(std::string& error)
{
    destroyDepth();
    if (extent_.width == 0 || extent_.height == 0) {
        return true;
    }
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent_.width, extent_.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = kDepthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &depthImage_) != VK_SUCCESS) {
        error = "vkCreateImage(depth) failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &allocate, nullptr, &depthMemory_);
    vkBindImageMemory(device_, depthImage_, depthMemory_, 0);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &depthView_) != VK_SUCCESS) {
        error = "vkCreateImageView(depth) failed";
        return false;
    }
    return true;
}

void VulkanScene::destroyDepth()
{
    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanScene::resize(VkExtent2D extent, std::string& error)
{
    if (extent.width == extent_.width && extent.height == extent_.height && depthView_ != VK_NULL_HANDLE) {
        return;
    }
    extent_ = extent;
    createDepth(error);
}

void VulkanScene::prepareDepth(VkCommandBuffer commandBuffer)
{
    if (depthImage_ == VK_NULL_HANDLE) {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanScene::draw(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent)
{
    if (pipeline_ == VK_NULL_HANDLE || meshes_.empty()) {
        return;
    }
    const float aspect = extent.height > 0 ? static_cast<float>(extent.width) / extent.height : 1.0f;
    Mat4 projection = perspective(camera.fovYDegrees, aspect, 0.05f, 500.0f);
    // The 3D pass uses the GL Y-up convention. Vulkan's viewport is Y-down,
    // so flip the projection's Y row before the view multiply; ImGui already
    // has its own Vulkan-aware projection and stays upright.
    projection.m[5] = -projection.m[5];
    const Mat4 viewProjection = multiply(projection, lookAt(camera.eye, camera.forward, camera.up));
    PushConstants push;
    std::memcpy(push.viewProjection, viewProjection.m, sizeof(push.viewProjection));
    push.ambient[0] = ambient_.x; push.ambient[1] = ambient_.y; push.ambient[2] = ambient_.z; push.ambient[3] = 1.0f;
    push.diffuse[0] = diffuse_.x; push.diffuse[1] = diffuse_.y; push.diffuse[2] = diffuse_.z; push.diffuse[3] = 1.0f;
    push.fogColor[0] = fogColor_.x; push.fogColor[1] = fogColor_.y; push.fogColor[2] = fogColor_.z; push.fogColor[3] = 1.0f;
    push.fogNearFar[0] = fogNear_;
    push.fogNearFar[1] = fogFar_;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDeviceSize offset = 0;
    std::size_t drawnMeshes = 0;
    for (const GpuMesh& mesh : meshes_) {
        float model[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f};
        if (mesh.entity != UINT32_MAX && scene_ != nullptr && mesh.entity < scene_->entities.size()) {
            const kotor::Entity& entity = scene_->entities[mesh.entity];
            if (entity.dead) {
                continue;
            }
            const float c = std::cos(entity.bearing);
            const float s = std::sin(entity.bearing);
            model[0] = c;  model[1] = s;
            model[4] = -s; model[5] = c;
            model[12] = entity.position.x;
            model[13] = entity.position.y;
            model[14] = entity.position.z;
        }
        std::memcpy(push.model, model, sizeof(push.model));
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mesh.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.texture >= textures_.size() || batch.indexCount == 0) {
                continue;
            }
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                    &textures_[batch.texture].descriptor, 0, nullptr);
            vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }
        ++drawnMeshes;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        logInfo(concat("Vulkan scene draw: meshes ", drawnMeshes, "/", meshes_.size(),
                       " textures ", textures_.size(),
                       " eye ", camera.eye.x, ", ", camera.eye.y, ", ", camera.eye.z));
    }
}

void VulkanScene::shutdown()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (GpuMesh& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
    scene_ = nullptr;
    destroyTextures();
    destroyDepth();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

} // namespace ke
