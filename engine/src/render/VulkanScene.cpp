#include "render/VulkanScene.h"
#include "render/VulkanShadowShaders.h"

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
#include <unordered_map>
#include <vector>

namespace ke {

namespace {

struct Mat4 {
    float m[16]{};
};

Mat4 perspective(float fovYDegrees, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovYDegrees * 3.14159265358979323846f / 360.0f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[5] = f;
    // Vulkan clip depth runs 0..1. The GL form (-1..1) would clip at about twice the near plane
    // and give different depth values, so the backends would disagree on near-coplanar faces.
    out.m[10] = farPlane / (nearPlane - farPlane);
    out.m[11] = -1.0f;
    out.m[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
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
    // The stencil shadows need 8 stencil bits beside the depth (retail asks GL for them,
    // AurGetAvailableStencilBits 0045fd00). D32 keeps the depth the scene had before.
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties support{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, candidate, &support);
        if ((support.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            depthFormat_ = candidate;
            break;
        }
    }

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
    const VkDescriptorSetLayout setLayouts[4] = {descriptorSetLayout_, descriptorSetLayout_, descriptorSetLayout_,
                                                 descriptorSetLayout_};
    pipelineLayoutInfo.setLayoutCount = 4;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
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
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }
    // Retail repeats a texture unless its TXI clamps an axis (retail 00424980, 00424dd0).
    for (int flags = 0; flags < 3; ++flags) {
        samplerInfo.addressModeU = (flags & 1) != 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                                     : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = (flags & 2) != 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                                     : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (vkCreateSampler(device_, &samplerInfo, nullptr, &wrapSamplers_[flags]) != VK_SUCCESS) {
            error = "vkCreateSampler (wrap) failed";
            return false;
        }
    }
    if (!createPipeline(error)) {
        return false;
    }
    std::string shadowError;
    if (!createShadowPipelines(shadowError)) {
        logWarn(concat("Stencil shadows unavailable on Vulkan: ", shadowError));
    }
    return true;
}

bool VulkanScene::createShadowPipelines(std::string& error)
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(ShadowPush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &shadowLayout_) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout(shadow) failed";
        return false;
    }
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = sizeof(kShadowVertSpv);
    moduleInfo.pCode = kShadowVertSpv;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &moduleInfo, nullptr, &vertex);
    moduleInfo.codeSize = sizeof(kShadowFragSpv);
    moduleInfo.pCode = kShadowFragSpv;
    vkCreateShaderModule(device_, &moduleInfo, nullptr, &fragment);
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, vertex, nullptr);
        }
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, fragment, nullptr);
        }
        error = "vkCreateShaderModule(shadow) failed";
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
    VkVertexInputBindingDescription binding{0, 4u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    // Culling as the scene's (back faces, counter-clockwise front); the volume passes change which
    // winding is the front (00426660's glFrontFace) through the dynamic state.
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // 00451a50: depth test on (GL_LEQUAL), no depth writes, stencil always passing with 0xff written;
    // the volume passes set the depth-fail op (GL_INCR, then GL_DECR) through the dynamic state.
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth.stencilTestEnable = VK_TRUE;
    depth.front.failOp = VK_STENCIL_OP_KEEP;
    depth.front.passOp = VK_STENCIL_OP_KEEP;
    depth.front.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth.front.compareMask = 0x0Fu;
    depth.front.writeMask = 0xFFu;
    depth.front.reference = 0x0Fu;
    depth.back = depth.front;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0; // glColorMask(0, 0, 0, 0)
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState volumeStates[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_STENCIL_OP};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 4;
    dynamic.pDynamicStates = volumeStates;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat_;
    rendering.depthAttachmentFormat = depthFormat_;
    rendering.stencilAttachmentFormat = depthFormat_;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = shadowLayout_;
    const VkResult volumeResult = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
                                                            &shadowVolumePipeline_);
    // 00450730: the fan, stencil GL_LEQUAL against 1 << (bits - 1) with nothing written (glStencilMask(0)),
    // depth test off, SRC_ALPHA / ONE_MINUS_SRC_ALPHA, as a fan.
    VkPipelineInputAssemblyStateCreateInfo fanAssembly = assembly;
    fanAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    VkPipelineDepthStencilStateCreateInfo fanDepth = depth;
    fanDepth.depthTestEnable = VK_FALSE;
    fanDepth.front.depthFailOp = VK_STENCIL_OP_KEEP;
    fanDepth.front.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    fanDepth.front.compareMask = 0xFFFFFFFFu;
    fanDepth.front.writeMask = 0u;
    fanDepth.front.reference = 0x80u;
    fanDepth.back = fanDepth.front;
    VkPipelineColorBlendAttachmentState fanAttachment{};
    fanAttachment.blendEnable = VK_TRUE;
    fanAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    fanAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    fanAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    fanAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    fanAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    fanAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    fanAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo fanBlend = blend;
    fanBlend.pAttachments = &fanAttachment;
    VkPipelineDynamicStateCreateInfo fanDynamic = dynamic;
    fanDynamic.dynamicStateCount = 2;
    VkGraphicsPipelineCreateInfo fanInfo = info;
    fanInfo.pInputAssemblyState = &fanAssembly;
    fanInfo.pDepthStencilState = &fanDepth;
    fanInfo.pColorBlendState = &fanBlend;
    fanInfo.pDynamicState = &fanDynamic;
    const VkResult fanResult = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &fanInfo, nullptr,
                                                         &shadowFanPipeline_);
    vkDestroyShaderModule(device_, vertex, nullptr);
    vkDestroyShaderModule(device_, fragment, nullptr);
    if (volumeResult != VK_SUCCESS || fanResult != VK_SUCCESS) {
        error = concat("vkCreateGraphicsPipelines(shadow) failed: ", static_cast<int>(volumeResult), " ",
                       static_cast<int>(fanResult));
        return false;
    }
    return true;
}

void VulkanScene::drawShadows(VkCommandBuffer commandBuffer, const SceneCamera& camera, const float* viewProjection)
{
    if (!shadowsOn_ || scene_ == nullptr || shadowVolumePipeline_ == VK_NULL_HANDLE
        || shadowFanPipeline_ == VK_NULL_HANDLE || extent_.width == 0 || extent_.height == 0) {
        return;
    }
    kotor::ShadowView view;
    view.enabled = true;
    view.focus = shadowFocus_;
    view.eye = camera.eye;
    view.forward = camera.forward;
    view.farClip = camera.farClip;
    const kotor::ShadowFrame frame = kotor::buildShadowFrame(*scene_, view);
    if (!frame.active()) {
        return;
    }
    constexpr VkDeviceSize kSliceBytes = 4u * 1024u * 1024u;
    constexpr VkDeviceSize kSlices = 4u;
    if (shadowBuffer_ == VK_NULL_HANDLE) {
        std::string error;
        if (!createBuffer(kSliceBytes * kSlices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          shadowBuffer_, shadowMemory_, error)) {
            return;
        }
        vkMapMemory(device_, shadowMemory_, 0, VK_WHOLE_SIZE, 0, &shadowMapped_);
    }
    if (shadowMapped_ == nullptr) {
        return;
    }
    const std::size_t pointCount = frame.volume.size() + frame.disk.size();
    if (pointCount * 4u * sizeof(float) > kSliceBytes) {
        return;
    }
    const VkDeviceSize sliceOffset = kSliceBytes * (shadowFrame_++ % kSlices);
    float* out = reinterpret_cast<float*>(static_cast<char*>(shadowMapped_) + sliceOffset);
    for (const kotor::Vec3& point : frame.volume) {
        *out++ = point.x;
        *out++ = point.y;
        *out++ = point.z;
        *out++ = 0.0f;
    }
    for (std::size_t i = 0; i < frame.disk.size(); ++i) {
        *out++ = frame.disk[i].x;
        *out++ = frame.disk[i].y;
        *out++ = frame.disk[i].z;
        *out++ = i == 0 ? frame.diskAlpha : 0.0f;
    }
    ShadowPush push;
    std::memcpy(push.viewProjection, viewProjection, sizeof(push.viewProjection));
    push.fog[0] = fogColor_.x;
    push.fog[1] = fogColor_.y;
    push.fog[2] = fogColor_.z;
    push.fog[3] = fogOn_ ? 1.0f : 0.0f;
    push.eye[0] = camera.eye.x;
    push.eye[1] = camera.eye.y;
    push.eye[2] = camera.eye.z;
    push.eye[3] = fogNear_;
    push.range[0] = fogFar_;
    // Scene::Render clears the stencil to 2^(bits-1)-1 before RenderShadows (0045172e).
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil = {1.0f, 0x7Fu};
    VkClearRect rect{};
    rect.rect.extent = extent_;
    rect.layerCount = 1;
    vkCmdClearAttachments(commandBuffer, 1, &clear, 1, &rect);
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &shadowBuffer_, &sliceOffset);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowVolumePipeline_);
    vkCmdPushConstants(commandBuffer, shadowLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    const VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
    for (const kotor::ShadowFrame::Run& run : frame.runs) {
        // 00426660: glFrontFace(GL_CW) with glStencilOp(KEEP, INCR, KEEP), then GL_CCW with DECR.
        vkCmdSetFrontFace(commandBuffer, VK_FRONT_FACE_CLOCKWISE);
        vkCmdSetStencilOp(commandBuffer, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                          VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_COMPARE_OP_ALWAYS);
        vkCmdDraw(commandBuffer, run.count, 1, run.first, 0);
        vkCmdSetFrontFace(commandBuffer, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetStencilOp(commandBuffer, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                          VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_COMPARE_OP_ALWAYS);
        vkCmdDraw(commandBuffer, run.count, 1, run.first, 0);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowFanPipeline_);
    vkCmdPushConstants(commandBuffer, shadowLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frame.disk.size()), 1,
              static_cast<std::uint32_t>(frame.volume.size()), 0);
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
    VkVertexInputAttributeDescription attributes[5]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, position))};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, normal))};
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, uv))};
    attributes[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, uv1))};
    attributes[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(kotor::SceneVertex, selfIllum))};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 5;
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
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // GL_LEQUAL in retail (004513a6)
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[5] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                             VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 5;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat_;
    rendering.depthAttachmentFormat = depthFormat_;
    rendering.stencilAttachmentFormat = depthFormat_;

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
    auto createBlend = [&](VkPipeline& target, VkBlendFactor source, VkBlendFactor destination) {
        VkPipelineColorBlendAttachmentState attachment = blendAttachment;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = source;
        attachment.dstColorBlendFactor = destination;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = source;
        attachment.dstAlphaBlendFactor = destination;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blendState = blend;
        blendState.pAttachments = &attachment;
        VkGraphicsPipelineCreateInfo variant = info;
        variant.pColorBlendState = &blendState;
        return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &variant, nullptr, &target);
    };
    const VkResult alphaResult = createBlend(alphaPipeline_, VK_BLEND_FACTOR_SRC_ALPHA,
                                             VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    const VkResult additiveResult = createBlend(additivePipeline_, VK_BLEND_FACTOR_SRC_ALPHA,
                                                VK_BLEND_FACTOR_ONE);
    (void)alphaResult;
    (void)additiveResult;
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    if (result != VK_SUCCESS) {
        error = concat("vkCreateGraphicsPipelines(scene) failed: ", static_cast<int>(result));
        return false;
    }
    return true;
}

namespace {

// Level 0 holds the new texels in TRANSFER_DST. Builds the mip chain, each level blitted from the
// one above, as glGenerateMipmap does on GL, and leaves every level ready for the shaders.
void recordMipChain(VkCommandBuffer command, VkImage image, std::int32_t width, std::int32_t height,
                    std::uint32_t mipLevels)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::int32_t mipWidth = width;
    std::int32_t mipHeight = height;
    for (std::uint32_t level = 1; level < mipLevels; ++level) {
        barrier.subresourceRange.baseMipLevel = level - 1u;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        const std::int32_t nextWidth = std::max(mipWidth / 2, 1);
        const std::int32_t nextHeight = std::max(mipHeight / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
        vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }
    barrier.subresourceRange.baseMipLevel = mipLevels - 1u;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

// An animated texture rewrote its image (retail 00462520 and 00461820 upload through the texture's
// vfunc +0xac). One staging slot per frame in flight: the fence of this slot's frame has been waited
// on, so nothing still reads it. The barrier orders the copy after the fragment reads of the frame
// before.
void VulkanScene::updateProceduralTextures(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                                           std::uint32_t frameSlots)
{
    if (scene_ == nullptr || scene_->procedurals.empty()) {
        return;
    }
    for (const kotor::ProceduralTexture& state : scene_->procedurals) {
        if (state.texture + 1u >= textures_.size() || state.texture >= scene_->textures.size()) {
            continue;
        }
        const kotor::Texture& texture = scene_->textures[state.texture];
        ProceduralStaging* staging = nullptr;
        for (ProceduralStaging& held : proceduralStaging_) {
            staging = held.texture == state.texture ? &held : staging;
        }
        if (staging == nullptr) {
            ProceduralStaging created;
            created.texture = state.texture;
            created.slotBytes = texture.image.rgba.size();
            std::string error;
            if (!createBuffer(created.slotBytes * frameSlots, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              created.buffer, created.memory, error)) {
                logWarn(concat("animated texture ", texture.name, ": ", error));
                continue;
            }
            vkMapMemory(device_, created.memory, 0, created.slotBytes * frameSlots, 0, &created.mapped);
            proceduralStaging_.push_back(created);
            staging = &proceduralStaging_.back();
        }
        if (staging->revision == texture.revision || staging->slotBytes != texture.image.rgba.size()) {
            continue;
        }
        staging->revision = texture.revision;
        const VkDeviceSize offset = staging->slotBytes * frameSlot;
        std::memcpy(static_cast<char*>(staging->mapped) + offset, texture.image.rgba.data(), staging->slotBytes);
        std::uint32_t mipLevels = 1;
        for (int size = std::max(texture.image.width, texture.image.height); size > 1; size /= 2) {
            ++mipLevels;
        }
        const VkImage image = textures_[state.texture].image;
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(texture.image.width),
                            static_cast<std::uint32_t>(texture.image.height), 1};
        vkCmdCopyBufferToImage(commandBuffer, staging->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        recordMipChain(commandBuffer, image, texture.image.width, texture.image.height, mipLevels);
    }
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
    std::uint32_t mipLevels = 1;
    for (int size = std::max(image.width, image.height); size > 1; size /= 2) {
        ++mipLevels;
    }
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 1};
        vkCmdCopyBufferToImage(command, staging, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        recordMipChain(command, gpu.image, image.width, image.height, mipLevels);
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
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    return vkCreateImageView(device_, &viewInfo, nullptr, &gpu.view) == VK_SUCCESS;
}

void VulkanScene::destroyTextures()
{
    for (ProceduralStaging& staging : proceduralStaging_) {
        vkDestroyBuffer(device_, staging.buffer, nullptr);
        vkFreeMemory(device_, staging.memory, nullptr);
    }
    proceduralStaging_.clear();
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
    mesh.vertexBytes = vertices.size() * sizeof(kotor::SceneVertex);
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

void VulkanScene::refreshSceneVertices(const kotor::Scene& scene, std::size_t first, std::size_t count)
{
    if (scene_ != &scene || meshes_.empty() || meshes_.front().vertexMemory == VK_NULL_HANDLE
        || meshes_.front().vertexBytes != scene.vertices.size() * sizeof(kotor::SceneVertex)
        || first + count > scene.vertices.size() || count == 0) {
        return;
    }
    void* mapped = nullptr;
    const VkDeviceSize offset = first * sizeof(kotor::SceneVertex);
    const VkDeviceSize bytes = count * sizeof(kotor::SceneVertex);
    if (vkMapMemory(device_, meshes_.front().vertexMemory, offset, bytes, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, scene.vertices.data() + first, bytes);
        vkUnmapMemory(device_, meshes_.front().vertexMemory);
    }
}

void VulkanScene::refreshEntityMeshes(const kotor::Scene& scene)
{
    std::size_t index = 1;
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        if (index >= meshes_.size()) {
            break;
        }
        GpuMesh& mesh = meshes_[index++];
        if (mesh.entity != entityMesh.entity) {
            continue;
        }
        mesh.visible = entityMesh.visible;
        mesh.fx = entityMesh.fx;
        if (mesh.vertexMemory == VK_NULL_HANDLE
            || entityMesh.vertices.size() * sizeof(kotor::SceneVertex) != mesh.vertexBytes) {
            continue;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, mesh.vertexMemory, 0, mesh.vertexBytes, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(mapped, entityMesh.vertices.data(), mesh.vertexBytes);
            vkUnmapMemory(device_, mesh.vertexMemory);
        }
    }
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
    fogOn_ = scene.fogOn;

    for (GpuMesh& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
    destroyTextures();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // Room for what spawned creatures and changed weapons bring; past it syncMeshes starts over.
    textureCapacity_ = scene.textures.size() + 1u + 256u;
    poolSize.descriptorCount = static_cast<std::uint32_t>(textureCapacity_);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(textureCapacity_);
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    textures_.resize(scene.textures.size() + 1u);
    // Last entry: bound in the lightmap and environment slots when a batch has neither.
    kotor::Texture white;
    white.name = "__white";
    white.image.width = 1;
    white.image.height = 1;
    white.image.rgba = {255u, 255u, 255u, 255u};
    for (std::size_t i = 0; i < textures_.size(); ++i) {
        const kotor::Texture& source = i < scene.textures.size() ? scene.textures[i] : white;
        if (!addTexture(source, textures_[i], error)) {
            return false;
        }
    }

    GpuMesh room = uploadMesh(scene.vertices, scene.indices, scene.batches, UINT32_MAX, error);
    // A module can have no static geometry at all: M12ab's rooms all move with scriptloop clips
    // (00646180), so everything it draws is in entity meshes. Draws skip a mesh without buffers.
    if (room.vertexBuffer == VK_NULL_HANDLE && !scene.vertices.empty() && !scene.indices.empty()) {
        return false;
    }
    meshes_.push_back(std::move(room));
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        // An empty mesh keeps its place too: refreshEntityMeshes walks both lists in step.
        GpuMesh mesh = uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                  entityMesh.entity, error);
        mesh.visible = entityMesh.visible;
        mesh.fx = entityMesh.fx;
        mesh.uid = entityMesh.uid;
        mesh.indexCount = entityMesh.indices.size();
        meshes_.push_back(std::move(mesh));
    }
    return true;
}

bool VulkanScene::addTexture(const kotor::Texture& source, GpuTexture& texture, std::string& error)
{
    if (!createTexture(source, texture, error)) {
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocate, &texture.descriptor) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texture.view;
    const std::uint8_t wrap = static_cast<std::uint8_t>(source.clamp & 3u);
    imageInfo.sampler = wrap == 3 ? sampler_ : wrapSamplers_[wrap];
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = texture.descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool VulkanScene::syncMeshes(const kotor::Scene& scene, std::string& error)
{
    if (scene_ != &scene || meshes_.empty() || textures_.empty() || textures_.size() - 1u > scene.textures.size()
        || scene.textures.size() + 1u > textureCapacity_
        || meshes_.front().vertexBytes != scene.vertices.size() * sizeof(kotor::SceneVertex)) {
        return upload(scene, error);
    }
    const std::size_t knownTextures = textures_.size() - 1u;
    for (std::size_t i = knownTextures; i < scene.textures.size(); ++i) {
        GpuTexture texture;
        if (!addTexture(scene.textures[i], texture, error)) {
            return false;
        }
        // The white stand-in stays last.
        textures_.insert(textures_.end() - 1, texture);
    }
    std::unordered_map<std::uint64_t, std::size_t> held;
    held.reserve(meshes_.size());
    for (std::size_t i = 1; i < meshes_.size(); ++i) {
        held.emplace(meshes_[i].uid, i);
    }
    std::vector<GpuMesh> next;
    next.reserve(scene.entityMeshes.size() + 1u);
    next.push_back(std::move(meshes_.front()));
    meshes_.front() = GpuMesh{};
    std::size_t reused = 0;
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        const auto found = held.find(entityMesh.uid);
        if (found != held.end() && meshes_[found->second].uid == entityMesh.uid
            && meshes_[found->second].vertexBytes == entityMesh.vertices.size() * sizeof(kotor::SceneVertex)
            && meshes_[found->second].indexCount == entityMesh.indices.size()) {
            next.push_back(std::move(meshes_[found->second]));
            meshes_[found->second] = GpuMesh{};
            next.back().batches = entityMesh.batches;
            next.back().entity = entityMesh.entity;
            ++reused;
        } else {
            next.push_back(uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                      entityMesh.entity, error));
            next.back().uid = entityMesh.uid;
            next.back().indexCount = entityMesh.indices.size();
        }
        next.back().visible = entityMesh.visible;
        next.back().fx = entityMesh.fx;
    }
    for (GpuMesh& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_ = std::move(next);
    logInfo(concat("scene sync: meshes reused ", reused, " uploaded ", scene.entityMeshes.size() - reused,
                   " textures uploaded ", scene.textures.size() - knownTextures));
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
    imageInfo.format = depthFormat_;
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
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
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
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

namespace {
// The column-major model matrix as three rows: its last row is always 0 0 0 1.
void setModelRows(float* rows, const float* m)
{
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            rows[row * 4 + column] = m[column * 4 + row];
        }
    }
}
} // namespace

void VulkanScene::drawBatch(VkCommandBuffer commandBuffer, const kotor::TextureBatch& batch, PushConstants& push,
                            const kotor::SampledLight& light, bool indexed)
{
    if (batch.texture + 1u >= textures_.size() || batch.indexCount == 0) {
        return;
    }
    const kotor::SampledLight used = kotor::materialLight(batch, batch.bakedLight ? batch.baked : light);
    push.ambient[0] = used.ambient.x; push.ambient[1] = used.ambient.y; push.ambient[2] = used.ambient.z;
    for (std::size_t i = 0; i < used.lights.size(); ++i) {
        const kotor::SampledLight::Light& one = used.lights[i];
        push.lightPositions[i * 4u] = one.position.x;
        push.lightPositions[i * 4u + 1u] = one.position.y;
        push.lightPositions[i * 4u + 2u] = one.position.z;
        push.lightPositions[i * 4u + 3u] = one.quadratic;
        const auto byte = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(value * 0.25f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        push.lightColours[i] = byte(one.colour.x) | (byte(one.colour.y) << 8u) | (byte(one.colour.z) << 16u);
    }
    const kotor::BatchUv uv = kotor::batchUvAt(batch, scene_ != nullptr ? scene_->clock : 0.0f);
    push.uv[0] = uv.scaleU; push.uv[1] = uv.scaleV; push.uv[2] = uv.offsetU; push.uv[3] = uv.offsetV;
    const std::size_t realTextures = textures_.size() - 1u;
    const bool lightmap = batch.lightmap < realTextures;
    const bool envmap = batch.envmap < realTextures && batch.envMask;
    const bool bump = batch.bumpmap < realTextures;
    push.lightColours[3] = 0u;
    if (bump) {
        const kotor::BumpCell cell = kotor::bumpCellAt(batch, scene_ != nullptr ? scene_->clock : 0.0f);
        push.lightColours[3] = static_cast<std::uint32_t>(batch.bumpCycleX)
            | (static_cast<std::uint32_t>(batch.bumpCycleY) << 5u)
            | (static_cast<std::uint32_t>(cell.x) << 10u) | (static_cast<std::uint32_t>(cell.y) << 15u)
            | (static_cast<std::uint32_t>(std::clamp(batch.bumpSpecular, 0.0f, 1.0f) * 255.0f + 0.5f) << 20u);
    }
    push.params[0] = kotor::batchAlphaRef(batch, {push.eye[0], push.eye[1], push.eye[2]});
    push.params[1] = lightmap ? 1.0f : 0.0f;
    push.params[2] = !envmap ? 0.0f : batch.envOnlyAlpha >= 0.0f ? 2.0f + batch.envOnlyAlpha : batch.envReflectionUnlit ? 1.25f : 1.0f;
    push.params[3] = kotor::batchShadeMode(batch);
    const kotor::Vec3 fog = kotor::batchFogColour(batch, fogColor_);
    push.fogColor[0] = fog.x; push.fogColor[1] = fog.y; push.fogColor[2] = fog.z;
    const float fogOnForFrame = push.fogColor[3];
    if (batch.noFog) {
        push.fogColor[3] = 0.0f;
    }
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    push.fogColor[3] = fogOnForFrame;
    VkPipeline activePipeline = pipeline_;
    if (batch.blend == kotor::BlendMode::Additive && additivePipeline_ != VK_NULL_HANDLE) {
        activePipeline = additivePipeline_;
    } else if (batch.blend == kotor::BlendMode::Alpha && alphaPipeline_ != VK_NULL_HANDLE) {
        activePipeline = alphaPipeline_;
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);
    vkCmdSetCullMode(commandBuffer, batch.twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    vkCmdSetDepthWriteEnable(commandBuffer, batch.writeDepth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthTestEnable(commandBuffer, batch.depthTest ? VK_TRUE : VK_FALSE);
    const VkDescriptorSet white = textures_.back().descriptor;
    const VkDescriptorSet sets[4] = {textures_[batch.texture].descriptor,
                                     lightmap ? textures_[batch.lightmap].descriptor : white,
                                     envmap ? textures_[batch.envmap].descriptor : white,
                                     bump ? textures_[batch.bumpmap].descriptor : white};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 4, sets, 0, nullptr);
    if (indexed) {
        vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
    } else {
        vkCmdDraw(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0);
    }
}

std::size_t VulkanScene::recordMeshes(VkCommandBuffer commandBuffer, const SceneCamera& camera, float aspect,
                                      const PreviewView* view)
{
    Mat4 projection = perspective(camera.fovYDegrees, aspect, camera.nearClip, camera.farClip);
    // The 3D pass uses the GL Y-up convention. Vulkan's viewport is Y-down,
    // so flip the projection's Y row before the view multiply; ImGui already
    // has its own Vulkan-aware projection and stays upright.
    projection.m[5] = -projection.m[5];
    const Mat4 viewProjection = multiply(projection, lookAt(camera.eye, camera.forward, camera.up));
    PushConstants push;
    std::memcpy(push.viewProjection, viewProjection.m, sizeof(push.viewProjection));
    push.ambient[3] = fogFar_;
    push.fogColor[3] = fogOn_ && view == nullptr ? 1.0f : 0.0f;
    push.eye[0] = camera.eye.x; push.eye[1] = camera.eye.y; push.eye[2] = camera.eye.z; push.eye[3] = fogNear_;
    const VkDeviceSize offset = 0;
    // Solid surfaces first, then everything that blends, far to near (see GlScene::drawMeshes).
    struct Pending {
        const GpuMesh* mesh;
        const kotor::TextureBatch* batch;
        std::array<float, 16> model;
        kotor::SampledLight light;
        float distance;
    };
    std::vector<Pending> blended;
    std::size_t drawnMeshes = 0;
    const std::vector<std::uint8_t>* rooms = scene_ != nullptr && view == nullptr && !scene_->visibleRooms.empty()
        ? &scene_->visibleRooms : nullptr;
    for (const GpuMesh& mesh : meshes_) {
        if (!mesh.visible || mesh.vertexBuffer == VK_NULL_HANDLE || mesh.indexBuffer == VK_NULL_HANDLE) {
            continue;
        }
        // ProgFX 1800: a flashing model leaves the normal buckets (ManageSceneBSP 0046ea32).
        if (view == nullptr && distorted(mesh)) {
            continue;
        }
        std::array<float, 16> model = {1.0f, 0.0f, 0.0f, 0.0f,
                                       0.0f, 1.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 1.0f, 0.0f,
                                       0.0f, 0.0f, 0.0f, 1.0f};
        kotor::SampledLight light;
        kotor::Vec3 meshCenter{};
        if (mesh.entity == UINT32_MAX) {
            if (view != nullptr && !view->drawStatic) {
                continue;
            }
            if (scene_ != nullptr) {
                light = kotor::sampleSceneLight(*scene_, camera.eye);
            }
        } else {
            if (view != nullptr
                && std::find(view->entities.begin(), view->entities.end(), mesh.entity) == view->entities.end()) {
                continue;
            }
            if (scene_ != nullptr && mesh.entity < scene_->entities.size()) {
                const kotor::Entity& entity = scene_->entities[mesh.entity];
                if (entity.dead && !entity.corpse()) {
                    continue;
                }
                const float c = std::cos(entity.drawBearing());
                const float s = std::sin(entity.drawBearing());
                model[0] = c;  model[1] = s;
                model[4] = -s; model[5] = c;
                model[12] = entity.position.x;
                model[13] = entity.position.y;
                model[14] = entity.position.z;
                meshCenter = entity.position + entity.lightOffset;
                light = kotor::objectSceneLight(*scene_, mesh.entity, meshCenter);
            }
        }
        setModelRows(push.modelRows, model.data());
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mesh.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.room >= 0 && rooms != nullptr && static_cast<std::size_t>(batch.room) < rooms->size()
                && (*rooms)[static_cast<std::size_t>(batch.room)] == 0u) {
                continue;
            }
            if (batch.maxDistance > 0.0f && kotor::length(batch.center - camera.eye) > batch.maxDistance) {
                continue;
            }
            if (batch.blend != kotor::BlendMode::Opaque) {
                const kotor::Vec3 center = mesh.entity == UINT32_MAX ? batch.center : meshCenter;
                blended.push_back({&mesh, &batch, model, light, kotor::length(center - camera.eye)});
                continue;
            }
            drawBatch(commandBuffer, batch, push, light, true);
        }
        ++drawnMeshes;
        // Scene::Render: RenderStaticGeometry, then RenderShadows, then the dynamic geometry.
        if (view == nullptr && &mesh == &meshes_.front()) {
            drawShadows(commandBuffer, camera, viewProjection.m);
        }
    }
    std::stable_sort(blended.begin(), blended.end(), [](const Pending& a, const Pending& b) {
        return kotor::blendedDrawsBefore(*a.batch, a.distance, *b.batch, b.distance);
    });
    for (const Pending& pending : blended) {
        setModelRows(push.modelRows, pending.model.data());
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &pending.mesh->vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, pending.mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        drawBatch(commandBuffer, *pending.batch, push, pending.light, true);
    }
    // Particles: rebuilt per frame into one slice of a host-visible ring.
    if ((view == nullptr || view->drawParticles) && scene_ != nullptr && !scene_->particleMesh.vertices.empty()) {
        constexpr VkDeviceSize kSliceBytes = 8u * 1024u * 1024u;
        constexpr VkDeviceSize kSlices = 4u;
        if (particleBuffer_ == VK_NULL_HANDLE) {
            std::string error;
            if (createBuffer(kSliceBytes * kSlices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             particleBuffer_, particleMemory_, error)) {
                vkMapMemory(device_, particleMemory_, 0, VK_WHOLE_SIZE, 0, &particleMapped_);
            }
        }
        if (particleMapped_ != nullptr) {
            const VkDeviceSize bytes = std::min<VkDeviceSize>(
                scene_->particleMesh.vertices.size() * sizeof(kotor::SceneVertex), kSliceBytes);
            const VkDeviceSize sliceOffset = kSliceBytes * (particleFrame_++ % kSlices);
            std::memcpy(static_cast<char*>(particleMapped_) + sliceOffset, scene_->particleMesh.vertices.data(),
                        static_cast<std::size_t>(bytes));
            const std::uint32_t available = static_cast<std::uint32_t>(bytes / sizeof(kotor::SceneVertex));
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &particleBuffer_, &sliceOffset);
            const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            setModelRows(push.modelRows, identity);
            const kotor::SampledLight none;
            for (const kotor::TextureBatch& batch : scene_->particleMesh.batches) {
                if (batch.firstIndex + batch.indexCount <= available) {
                    drawBatch(commandBuffer, batch, push, none, false);
                }
            }
        }
    }
    return drawnMeshes;
}

void VulkanScene::draw(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent)
{
    if (pipeline_ == VK_NULL_HANDLE || meshes_.empty()) {
        return;
    }
    const float aspect = extent.height > 0 ? static_cast<float>(extent.width) / extent.height : 1.0f;
    const std::size_t drawnMeshes = recordMeshes(commandBuffer, camera, aspect, nullptr);
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::size_t alphaBatches = 0;
        std::size_t additiveBatches = 0;
        for (const GpuMesh& mesh : meshes_) {
            for (const kotor::TextureBatch& batch : mesh.batches) {
                if (batch.blend == kotor::BlendMode::Alpha) {
                    ++alphaBatches;
                } else if (batch.blend == kotor::BlendMode::Additive) {
                    ++additiveBatches;
                }
            }
        }
        logInfo(concat("Vulkan scene draw: meshes ", drawnMeshes, "/", meshes_.size(),
                       " textures ", textures_.size(), " alpha batches ", alphaBatches,
                       " additive batches ", additiveBatches, " alpha pipeline ",
                       alphaPipeline_ != VK_NULL_HANDLE ? "ready" : "missing",
                       " additive pipeline ",
                       additivePipeline_ != VK_NULL_HANDLE ? "ready" : "missing",
                       " eye ", camera.eye.x, ", ", camera.eye.y, ", ", camera.eye.z));
    }
}

bool VulkanScene::uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                                   std::string& error)
{
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        error = "invalid movie frame";
        return false;
    }
    kotor::Texture texture;
    texture.name = "movie";
    texture.image.width = width;
    texture.image.height = height;
    texture.image.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
    // The last frame submitted may still be sampling the current image. createTexture ends with
    // vkQueueWaitIdle on the queue frames are submitted to, so the old image goes only after it.
    GpuTexture next;
    if (!createTexture(texture, next, error)) {
        UiImage partial;
        partial.image = next.image;
        partial.memory = next.memory;
        partial.view = next.view;
        destroyUiImage(partial);
        return false;
    }
    destroyMovieFrame();
    movieFrame_ = next;
    return true;
}

void VulkanScene::destroyMovieFrame()
{
    if (movieFrame_.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, movieFrame_.view, nullptr);
        movieFrame_.view = VK_NULL_HANDLE;
    }
    if (movieFrame_.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, movieFrame_.image, nullptr);
        movieFrame_.image = VK_NULL_HANDLE;
    }
    if (movieFrame_.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, movieFrame_.memory, nullptr);
        movieFrame_.memory = VK_NULL_HANDLE;
    }
}

void VulkanScene::clearScene()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (GpuMesh& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
    destroyTextures();
    scene_ = nullptr;
}

void VulkanScene::drawView(VkCommandBuffer commandBuffer, const PreviewView& view,
                           VkExtent2D windowExtent)
{
    if (pipeline_ == VK_NULL_HANDLE || view.width <= 0 || view.height <= 0
        || windowExtent.width == 0 || windowExtent.height == 0) {
        return;
    }
    // Scissor / clear rectangle: the view rectangle clamped to the window. Vulkan
    // framebuffer coordinates are already top-left, so no vertical conversion.
    const int windowWidth = static_cast<int>(windowExtent.width);
    const int windowHeight = static_cast<int>(windowExtent.height);
    const int left = std::max(view.x, 0);
    const int top = std::max(view.y, 0);
    const int right = std::min(view.x + view.width, windowWidth);
    const int bottom = std::min(view.y + view.height, windowHeight);
    if (right <= left || bottom <= top) {
        return;
    }
    VkRect2D clampedRect{};
    clampedRect.offset = {left, top};
    clampedRect.extent = {static_cast<std::uint32_t>(right - left),
                          static_cast<std::uint32_t>(bottom - top)};

    VkClearAttachment clears[2]{};
    clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clears[0].colorAttachment = 0;
    clears[0].clearValue.color = {{view.background.x, view.background.y, view.background.z, 1.0f}};
    clears[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clears[1].clearValue.depthStencil = {1.0f, 0};
    VkClearRect clearRect{};
    clearRect.rect = clampedRect;
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;
    vkCmdClearAttachments(commandBuffer, 2, clears, 1, &clearRect);

    // The viewport keeps the full (unclamped) rectangle so a view that sticks out of
    // the window is cropped instead of squashed; the scissor limits writes.
    VkViewport viewport{};
    viewport.x = static_cast<float>(view.x);
    viewport.y = static_cast<float>(view.y);
    viewport.width = static_cast<float>(view.width);
    viewport.height = static_cast<float>(view.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &clampedRect);

    if (!meshes_.empty()) {
        recordMeshes(commandBuffer, view.camera, static_cast<float>(view.width) / static_cast<float>(view.height), &view);
    }

    // Restore the full-window state for whatever draws next (ImGui sets its own too).
    VkViewport full{};
    full.width = static_cast<float>(windowExtent.width);
    full.height = static_cast<float>(windowExtent.height);
    full.minDepth = 0.0f;
    full.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &full);
    VkRect2D fullScissor{};
    fullScissor.extent = windowExtent;
    vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);
}

bool VulkanScene::createUiImage(int width, int height, const std::uint8_t* rgba, UiImage& out,
                                std::string& error)
{
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        error = "invalid UI image";
        return false;
    }
    kotor::Texture texture;
    texture.name = "ui image";
    texture.image.width = width;
    texture.image.height = height;
    texture.image.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
    GpuTexture gpu;
    const bool created = createTexture(texture, gpu, error);
    out.image = gpu.image;
    out.memory = gpu.memory;
    out.view = gpu.view;
    if (!created) {
        destroyUiImage(out);
        return false;
    }
    return true;
}

void VulkanScene::destroyUiImage(UiImage& image)
{
    if (device_ == VK_NULL_HANDLE) {
        image = {};
        return;
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image.memory, nullptr);
    }
    image = {};
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
    destroyMovieFrame();
    destroyDepth();
    if (particleMapped_ != nullptr) {
        vkUnmapMemory(device_, particleMemory_);
        particleMapped_ = nullptr;
    }
    if (particleBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, particleBuffer_, nullptr);
        particleBuffer_ = VK_NULL_HANDLE;
    }
    if (particleMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, particleMemory_, nullptr);
        particleMemory_ = VK_NULL_HANDLE;
    }
    if (shadowMapped_ != nullptr) {
        vkUnmapMemory(device_, shadowMemory_);
        shadowMapped_ = nullptr;
    }
    if (shadowBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, shadowBuffer_, nullptr);
        shadowBuffer_ = VK_NULL_HANDLE;
    }
    if (shadowMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, shadowMemory_, nullptr);
        shadowMemory_ = VK_NULL_HANDLE;
    }
    if (shadowVolumePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, shadowVolumePipeline_, nullptr);
        shadowVolumePipeline_ = VK_NULL_HANDLE;
    }
    if (shadowFanPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, shadowFanPipeline_, nullptr);
        shadowFanPipeline_ = VK_NULL_HANDLE;
    }
    if (shadowLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, shadowLayout_, nullptr);
        shadowLayout_ = VK_NULL_HANDLE;
    }
    for (VkSampler& wrapSampler : wrapSamplers_) {
        if (wrapSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, wrapSampler, nullptr);
            wrapSampler = VK_NULL_HANDLE;
        }
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (alphaPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, alphaPipeline_, nullptr);
        alphaPipeline_ = VK_NULL_HANDLE;
    }
    if (additivePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, additivePipeline_, nullptr);
        additivePipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (distortPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, distortPool_, nullptr);
        distortPool_ = VK_NULL_HANDLE;
        distortSet_ = VK_NULL_HANDLE;
        distortView_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

bool VulkanScene::distorted(const GpuMesh& mesh) const
{
    return !mesh.fx && mesh.entity != UINT32_MAX
        && std::find(distortEntities_.begin(), distortEntities_.end(), mesh.entity) != distortEntities_.end();
}

bool VulkanScene::distortionVisible() const
{
    if (distortEntities_.empty()) {
        return false;
    }
    return std::any_of(meshes_.begin(), meshes_.end(),
                       [&](const GpuMesh& mesh) { return mesh.visible && distorted(mesh); });
}

void VulkanScene::drawDistortion(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent,
                                 VkImageView frameCopy)
{
    // The GL twin is GlScene::applyDistortion (retail 004331d0 / 00432d70 after 004503a0).
    if (pipeline_ == VK_NULL_HANDLE || frameCopy == VK_NULL_HANDLE || scene_ == nullptr || textures_.empty()
        || extent.width == 0 || extent.height == 0) {
        return;
    }
    if (distortPool_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &distortPool_) != VK_SUCCESS) {
            return;
        }
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = distortPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptorSetLayout_;
        if (vkAllocateDescriptorSets(device_, &allocate, &distortSet_) != VK_SUCCESS) {
            return;
        }
    }
    if (distortView_ != frameCopy) {
        // The copy is remade on a resize, after the device has gone idle.
        VkDescriptorImageInfo image{sampler_, frameCopy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = distortSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        distortView_ = frameCopy;
    }
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    Mat4 projection = perspective(camera.fovYDegrees, aspect, camera.nearClip, camera.farClip);
    projection.m[5] = -projection.m[5];
    const Mat4 viewProjection = multiply(projection, lookAt(camera.eye, camera.forward, camera.up));
    PushConstants push;
    std::memcpy(push.viewProjection, viewProjection.m, sizeof(push.viewProjection));
    push.eye[0] = camera.eye.x; push.eye[1] = camera.eye.y; push.eye[2] = camera.eye.z;
    push.lightPositions[0] = 1.0f / static_cast<float>(extent.width);
    push.lightPositions[1] = 1.0f / static_cast<float>(extent.height);
    push.lightPositions[2] = distortPhase_;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetDepthWriteEnable(commandBuffer, VK_TRUE);
    vkCmdSetDepthTestEnable(commandBuffer, VK_TRUE);
    const VkDeviceSize offset = 0;
    const VkDescriptorSet white = textures_.back().descriptor;
    for (const GpuMesh& mesh : meshes_) {
        if (!mesh.visible || !distorted(mesh) || mesh.vertexBuffer == VK_NULL_HANDLE
            || mesh.indexBuffer == VK_NULL_HANDLE || mesh.entity >= scene_->entities.size()) {
            continue;
        }
        const kotor::Entity& entity = scene_->entities[mesh.entity];
        if (entity.dead && !entity.corpse()) {
            continue;
        }
        const float c = std::cos(entity.drawBearing());
        const float s = std::sin(entity.drawBearing());
        const float model[16] = {c, s, 0.0f, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                 entity.position.x, entity.position.y, entity.position.z, 1.0f};
        setModelRows(push.modelRows, model);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mesh.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.shell != 0u || batch.texture + 1u >= textures_.size() || batch.indexCount == 0) {
                continue;
            }
            const kotor::BatchUv uv = kotor::batchUvAt(batch, scene_->clock);
            push.uv[0] = uv.scaleU; push.uv[1] = uv.scaleV; push.uv[2] = uv.offsetU; push.uv[3] = uv.offsetV;
            push.params[0] = kotor::batchAlphaRef(batch, camera.eye);
            push.params[1] = 0.0f;
            push.params[2] = 0.0f;
            push.params[3] = 4.0f;
            push.lightColours[3] = 0u;
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdSetCullMode(commandBuffer, batch.twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            const VkDescriptorSet sets[4] = {textures_[batch.texture].descriptor, white, distortSet_, white};
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 4, sets, 0,
                                    nullptr);
            vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }
    }
}

} // namespace ke
