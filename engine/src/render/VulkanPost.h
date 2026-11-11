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

    // ProgFX 1601 speed blur (FrameBufferModifications 00437520 / 00435bd0, flag 0x10). blurReady:
    // the pass exists and the blur is on; otherwise call blurSkipped, which drops the last frame
    // as retail clears 007a698d. blurPrimed: a last frame is held, so recordBlur (after recordCopy,
    // in a rendering scope) draws last x ratio + frame x (1 - ratio). recordAccumulate (outside a
    // scope) then keeps the swapchain image as the last frame.
    bool blurReady() const { return pipeline_ != VK_NULL_HANDLE && accum_.view != VK_NULL_HANDLE
        && effect_.blurring(); }
    bool blurPrimed() const { return blurPrimed_; }
    void blurSkipped() { blurPrimed_ = false; }
    void recordBlur(VkCommandBuffer commandBuffer);
    void recordAccumulate(VkCommandBuffer commandBuffer, VkImage swapchainImage);
    // The frame copy recordCopy fills (read-only layout after it); ProgFX 1800 reads it.
    VkImageView copyView() const { return copy_.view; }

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
    bool prepareAccum(std::string& error);

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
    Image accum_; // the last (blurred) frame, ProgFX 1601
    bool blurPrimed_ = false;
    VkExtent2D extent_{};
    Image noise_;
    VideoEffectState effect_;
};

} // namespace ke
