#pragma once

#include "kotor/Formats.h"
#include "render/SceneView.h"

#include <volk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ke {

// Vulkan scene pass. The backend owns the device/swapchain; this class owns
// the scene pipeline, buffers, texture images and the depth attachment.
class VulkanScene {
public:
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
                    VkQueue queue, VkFormat colorFormat, std::string& error);
    bool upload(const kotor::Scene& scene, std::string& error);
    void refreshEntityMeshes(const kotor::Scene& scene);
    void resize(VkExtent2D extent, std::string& error);
    void prepareDepth(VkCommandBuffer commandBuffer);
    void draw(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent);
    void shutdown();

    bool uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                          std::string& error);
    void destroyMovieFrame();
    VkImageView movieFrameView() const { return movieFrame_.view; }
    VkSampler sampler() const { return sampler_; }

    VkImageView depthView() const { return depthView_; }
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

    struct GpuMesh {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkDeviceSize vertexBytes = 0;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        std::vector<kotor::TextureBatch> batches;
        std::uint32_t entity = UINT32_MAX;
    };

    struct PushConstants {
        float model[16]{};
        float viewProjection[16]{};
        float ambient[4]{};
        float diffuse[4]{};
        float fogColor[4]{};
        float fogNearFar[2]{};
        float padding[2]{};
    };

    std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory, std::string& error);
    bool createDepth(std::string& error);
    bool createPipeline(std::string& error);
    bool createTexture(const kotor::Texture& texture, GpuTexture& gpu, std::string& error);
    GpuMesh uploadMesh(const std::vector<kotor::SceneVertex>& vertices,
                       const std::vector<std::uint32_t>& indices,
                       const std::vector<kotor::TextureBatch>& batches, std::uint32_t entity,
                       std::string& error);
    void destroyMesh(GpuMesh& mesh);
    void destroyTextures();
    void destroyDepth();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<GpuMesh> meshes_;
    const kotor::Scene* scene_ = nullptr;
    std::vector<GpuTexture> textures_;
    GpuTexture movieFrame_;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat colorFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
    kotor::Vec3 ambient_{0.35f, 0.35f, 0.38f};
    kotor::Vec3 diffuse_{0.85f, 0.82f, 0.75f};
    kotor::Vec3 fogColor_{0.06f, 0.07f, 0.10f};
    float fogNear_ = 40.0f;
    float fogFar_ = 160.0f;
};

} // namespace ke
