#pragma once

#include "render/VideoEffect.h"

#include <volk.h>

#include <string>

namespace ke {

// Vulkan video-effect pass (ACT_508/510). When the effect is active the backend
// splits the frame: the scene is drawn into the swapchain image, copied here,
// and a second pass redraws it through effect.frag before the UI. With no
// effect the backend keeps its single-pass path and this class is idle.
class VulkanPost {
public:
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
                    VkQueue queue, VkFormat colorFormat, std::string& error);
    bool resize(VkExtent2D extent, std::string& error);
    void shutdown();

    void setEffect(const VideoEffectState& effect) { effect_ = effect; }
    const VideoEffectState& effect() const { return effect_; }
    // True when a frame should take the two-pass path.
    bool needed() const { return pipeline_ != VK_NULL_HANDLE && copy_.view != VK_NULL_HANDLE
        && effect_.active(); }

    // Copies the swapchain image (COLOR_ATTACHMENT_OPTIMAL on entry and exit).
    void recordCopy(VkCommandBuffer commandBuffer, VkImage swapchainImage);
    // Draws the full-screen effect triangle inside an active rendering scope.
    void recordDraw(VkCommandBuffer commandBuffer);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool createImage(Image& target, std::uint32_t width, std::uint32_t height,
                     VkFormat format, VkImageUsageFlags usage, std::string& error);
    void destroyImage(Image& target);
    bool uploadNoise(std::string& error);
    void updateDescriptor();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkSampler clampSampler_ = VK_NULL_HANDLE;
    VkSampler repeatSampler_ = VK_NULL_HANDLE;
    Image copy_;
    VkExtent2D extent_{};
    Image noise_;
    VideoEffectState effect_;
};

} // namespace ke
