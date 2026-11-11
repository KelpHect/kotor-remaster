#pragma once

#include "kotor/Formats.h"
#include "render/Backend.h"
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
    // Uploads only the textures and entity meshes it has not seen and drops those that are gone.
    // The caller has waited for the device: a dropped mesh may have been in flight.
    bool syncMeshes(const kotor::Scene& scene, std::string& error);
    void refreshEntityMeshes(const kotor::Scene& scene);
    // Re-uploads vertices [first, first + count) of the area mesh (the grass wind moves them).
    void refreshSceneVertices(const kotor::Scene& scene, std::size_t first, std::size_t count);
    // What shows where no geometry is: the fog colour when the area has fog, as on GL.
    bool hasScene() const { return scene_ != nullptr; }
    kotor::Vec3 clearColour() const { return fogOn_ ? fogColor_ : kotor::Vec3{}; }
    void resize(VkExtent2D extent, std::string& error);
    void prepareDepth(VkCommandBuffer commandBuffer);
    // Outside the render pass: copies animated textures that changed. `frameSlot` is the index of
    // the frame in flight this command buffer belongs to, below `frameSlots`.
    void updateProceduralTextures(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, std::uint32_t frameSlots);
    void draw(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent);
    // ProgFX 1800: these entities leave the normal draw; drawDistortion draws them again, inside an
    // active pass with the scene's depth, each pixel the frame copy (`frameCopy`, read-only layout)
    // moved by the ring texture at `phase` (see scene.frag).
    void setDistortion(const std::vector<std::uint32_t>& entities, float phase)
    {
        distortEntities_ = entities;
        distortPhase_ = phase;
    }
    bool distortionVisible() const;
    void drawDistortion(VkCommandBuffer commandBuffer, const SceneCamera& camera, VkExtent2D extent,
                        VkImageView frameCopy);
    void shutdown();

    bool uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                          std::string& error);
    void destroyMovieFrame();
    VkImageView movieFrameView() const { return movieFrame_.view; }
    VkSampler sampler() const { return sampler_; }

    VkImageView depthView() const { return depthView_; }
    // Depth with an 8-bit stencil, for the stencil shadows (D32_S8 where the device has it).
    VkFormat depthFormat() const { return depthFormat_; }
    // Stencil shadows (Scene::RenderShadows 00451a50): the options' gate and the scene focus.
    void setShadows(bool enabled, kotor::Vec3 focus)
    {
        shadowsOn_ = enabled;
        shadowFocus_ = focus;
    }
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

    // 3D GUI preview support. A preview instance is initialize()d for its pipelines
    // and descriptors but never resize()d: it draws into the swapchain's depth.
    // Preview scenes may have no static geometry.
    void setPreviewMode(bool enabled) { previewMode_ = enabled; }
    // Frees the uploaded scene (meshes, textures); the caller waits for the device.
    void clearScene();
    // Inside an active dynamic-rendering pass (colour + depth attachment): clears the
    // view rectangle (top-left origin, clamped to the window) in colour and depth,
    // draws the view's entities, then restores the full-window viewport and scissor.
    void drawView(VkCommandBuffer commandBuffer, const PreviewView& view, VkExtent2D windowExtent);

    // Independent RGBA8 images for ImGui; the caller registers the view with ImGui.
    struct UiImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool createUiImage(int width, int height, const std::uint8_t* rgba, UiImage& out,
                       std::string& error);
    void destroyUiImage(UiImage& image);

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
        bool visible = true;
        bool fx = false; // an effect model: the distortion pass never takes it
        std::uint64_t uid = 0;
        std::size_t indexCount = 0;
    };

    bool addTexture(const kotor::Texture& source, GpuTexture& texture, std::string& error);

    // Matches shaders/scene.vert and scene.frag.
    struct PushConstants {
        float modelRows[12]{};
        float viewProjection[16]{};
        float ambient[4]{};           // rgb, w = fog far
        float lightPositions[12]{};   // xyz, w = quadratic attenuation (< 0: a direction)
        std::uint32_t lightColours[4]{}; // RGB8 of colour / 4; the last: bump cells x, y, cell x, y (0 = none)
        float fogColor[4]{};
        float eye[4]{};
        float uv[4]{};
        float params[4]{};
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
    void drawBatch(VkCommandBuffer commandBuffer, const kotor::TextureBatch& batch, PushConstants& push,
                   const kotor::SampledLight& light, bool indexed);
    std::size_t recordMeshes(VkCommandBuffer commandBuffer, const SceneCamera& camera, float aspect,
                             const PreviewView* view);
    void destroyTextures();
    void destroyDepth();
    bool createShadowPipelines(std::string& error);
    void drawShadows(VkCommandBuffer commandBuffer, const SceneCamera& camera, const float* viewProjection);

    // Matches shaders/shadow.vert and shadow.frag.
    struct ShadowPush {
        float viewProjection[16]{};
        float fog[4]{};
        float eye[4]{};
        float range[4]{};
    };
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    bool shadowsOn_ = false;
    kotor::Vec3 shadowFocus_;
    VkPipelineLayout shadowLayout_ = VK_NULL_HANDLE;
    VkPipeline shadowVolumePipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowFanPipeline_ = VK_NULL_HANDLE;
    VkBuffer shadowBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory_ = VK_NULL_HANDLE;
    void* shadowMapped_ = nullptr;
    std::uint64_t shadowFrame_ = 0;

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
    std::size_t textureCapacity_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE; // clamp on both axes: movies and UI images
    // Scene textures repeat per axis unless their TXI clamps it; index = Texture::clamp (bit 0
    // clamps U, bit 1 clamps V). Entry 3 is sampler_ itself and is not destroyed twice.
    VkSampler wrapSamplers_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::vector<GpuMesh> meshes_;
    const kotor::Scene* scene_ = nullptr;
    std::vector<GpuTexture> textures_;
    struct ProceduralStaging {
        std::size_t texture = 0;
        std::uint32_t revision = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize slotBytes = 0;
    };
    std::vector<ProceduralStaging> proceduralStaging_;
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
    bool fogOn_ = false;
    bool previewMode_ = false;
    VkBuffer particleBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory particleMemory_ = VK_NULL_HANDLE;
    void* particleMapped_ = nullptr;
    std::uint64_t particleFrame_ = 0;
    std::vector<std::uint32_t> distortEntities_;
    float distortPhase_ = 0.0f;
    VkDescriptorPool distortPool_ = VK_NULL_HANDLE;
    VkDescriptorSet distortSet_ = VK_NULL_HANDLE;
    VkImageView distortView_ = VK_NULL_HANDLE;
    bool distorted(const GpuMesh& mesh) const;
};

} // namespace ke
