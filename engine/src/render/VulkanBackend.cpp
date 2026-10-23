#include "render/BackendFactory.h"
#include "render/Backend.h"
#include "core/Log.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

#include <volk.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ke {

#if KE_ENABLE_VULKAN

namespace {

constexpr std::uint32_t kMaxFramesInFlight = 2;

std::string apiVersionString(std::uint32_t version)
{
    return concat(VK_API_VERSION_MAJOR(version), ".", VK_API_VERSION_MINOR(version), ".",
                  VK_API_VERSION_PATCH(version));
}

const char* resultName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    default:
        return "VK_ERROR_<other>";
    }
}

std::string formatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "VK_FORMAT_B8G8R8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM:
        return "VK_FORMAT_B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB:
        return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
    default:
        return concat("VkFormat(", static_cast<int>(format), ")");
    }
}

const char* presentModeName(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    default:
        return "OTHER";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*)
{
    if (data != nullptr && data->pMessage != nullptr) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            logWarn(concat("Vulkan validation: ", data->pMessage));
        } else {
            logInfo(concat("Vulkan validation: ", data->pMessage));
        }
    }
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}

VkCompositeAlphaFlagBitsKHR pickCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> candidates{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (const VkCompositeAlphaFlagBitsKHR candidate : candidates) {
        if ((supported & candidate) != 0) {
            return candidate;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

class VulkanBackend final : public IRenderBackend {
public:
    ~VulkanBackend() override { shutdown(); }

    const char* name() const noexcept override { return "vulkan"; }

    bool initialize(SDL_Window* window, const Options& options, std::string& error) override
    {
        window_ = window;
        options_ = options;

        if (volkInitialize() != VK_SUCCESS) {
            error =
                "Vulkan loader unavailable (volkInitialize failed). Install or repair the Vulkan "
                "runtime so vulkan-1.dll can be loaded.";
            return false;
        }

        if (!createInstance(error)) {
            return false;
        }
        if (!createSurface(error)) {
            return false;
        }
        if (!pickPhysicalDevice(error)) {
            return false;
        }
        if (!createDevice(error)) {
            return false;
        }
        if (!createCommandResources(error)) {
            return false;
        }
        if (!createFrameSync(error)) {
            return false;
        }
        if (!createSwapchain(error)) {
            return false;
        }

        info_.name = "vulkan";
        info_.apiVersion = concat("instance ", apiVersionString(instanceApiVersion_), " / device ",
                                  apiVersionString(deviceApiVersion_));
        info_.device = concat(deviceName_, " [", deviceTypeName_, "]");
        initialized_ = true;
        return true;
    }

    bool initializeImGui(SDL_Window* window, std::string& error) override
    {
        if (!ImGui_ImplSDL3_InitForVulkan(window)) {
            error = "ImGui_ImplSDL3_InitForVulkan failed";
            return false;
        }
        platformInitialized_ = true;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = instanceApiVersion_;
        initInfo.Instance = instance_;
        initInfo.PhysicalDevice = physicalDevice_;
        initInfo.Device = device_;
        initInfo.QueueFamily = graphicsFamily_;
        initInfo.Queue = graphicsQueue_;
        initInfo.DescriptorPool = VK_NULL_HANDLE;
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        initInfo.PipelineCache = VK_NULL_HANDLE;
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_;
        initInfo.Allocator = nullptr;
        initInfo.CheckVkResultFn = [](VkResult result) {
            if (result != VK_SUCCESS) {
                logError(concat("ImGui Vulkan backend error: ", resultName(result)));
            }
        };
        initInfo.MinAllocationSize = 1024 * 1024;

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            error = "ImGui_ImplVulkan_Init failed";
            ImGui_ImplSDL3_Shutdown();
            platformInitialized_ = false;
            return false;
        }
        imguiInitialized_ = true;
        return true;
    }

    void newFrame() override
    {
        if (imguiInitialized_) {
            ImGui_ImplVulkan_NewFrame();
        }
    }

    FrameStatus renderFrame(std::string& error) override
    {
        if (!initialized_ || device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE) {
            error = "Vulkan backend is not initialized";
            return FrameStatus::Fatal;
        }

        if (pendingRecreate_) {
            if (!recreateSwapchain(error)) {
                return FrameStatus::Fatal;
            }
        }

        int pixelWidth = 0;
        int pixelHeight = 0;
        SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight);
        if (pixelWidth <= 0 || pixelHeight <= 0) {
            return FrameStatus::Ok; // minimized; skip the frame
        }
        if (static_cast<std::uint32_t>(pixelWidth) != extent_.width
            || static_cast<std::uint32_t>(pixelHeight) != extent_.height) {
            if (!recreateSwapchain(error)) {
                return FrameStatus::Fatal;
            }
        }

        const VkFence frameFence = inFlightFences_[currentFrame_];
        VkResult result = vkWaitForFences(device_, 1, &frameFence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            error = concat("vkWaitForFences failed: ", resultName(result));
            return FrameStatus::Fatal;
        }

        std::uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            pendingRecreate_ = true;
            return FrameStatus::Ok;
        }
        if (result != VK_SUCCESS) {
            error = concat("vkAcquireNextImageKHR failed: ", resultName(result));
            return FrameStatus::Fatal;
        }

        if (vkResetFences(device_, 1, &frameFence) != VK_SUCCESS) {
            error = "vkResetFences failed";
            return FrameStatus::Fatal;
        }

        const VkCommandBuffer commandBuffer = commandBuffers_[currentFrame_];
        if (vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
            error = "vkResetCommandBuffer failed";
            return FrameStatus::Fatal;
        }
        if (!recordCommandBuffer(commandBuffer, imageIndex, error)) {
            return FrameStatus::Fatal;
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSemaphore waitSemaphore = imageAvailable_[currentFrame_];
        const VkSemaphore signalSemaphore = renderFinished_[imageIndex];

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;

        result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frameFence);
        if (result != VK_SUCCESS) {
            error = concat("vkQueueSubmit failed: ", resultName(result));
            return FrameStatus::Fatal;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &signalSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            pendingRecreate_ = true;
        } else if (result != VK_SUCCESS) {
            error = concat("vkQueuePresentKHR failed: ", resultName(result));
            return FrameStatus::Fatal;
        }

        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
        ++frameIndex_;
        return FrameStatus::Ok;
    }

    void shutdown() override
    {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        if (imguiInitialized_) {
            ImGui_ImplVulkan_Shutdown();
            imguiInitialized_ = false;
        }
        if (platformInitialized_) {
            ImGui_ImplSDL3_Shutdown();
            platformInitialized_ = false;
        }

        destroySwapchain();

        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (inFlightFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, inFlightFences_[i], nullptr);
                inFlightFences_[i] = VK_NULL_HANDLE;
            }
            if (imageAvailable_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
                imageAvailable_[i] = VK_NULL_HANDLE;
            }
        }

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (debugMessenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
            debugMessenger_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        volkFinalize();
        initialized_ = false;
        window_ = nullptr;
    }

    const BackendInfo& info() const override { return info_; }

private:
    bool createInstance(std::string& error)
    {
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion != nullptr) {
            vkEnumerateInstanceVersion(&loaderVersion);
        }
        if (loaderVersion < VK_API_VERSION_1_3) {
            error = concat("Vulkan loader supports only ", apiVersionString(loaderVersion),
                           "; Vulkan 1.3+ is required by this scaffold");
            return false;
        }
        instanceApiVersion_ = (loaderVersion >= VK_API_VERSION_1_4) ? VK_API_VERSION_1_4
                                                                     : VK_API_VERSION_1_3;

        std::uint32_t extensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        if (sdlExtensions == nullptr) {
            error = concat("SDL_Vulkan_GetInstanceExtensions failed: ", SDL_GetError());
            return false;
        }
        std::vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionCount);

        std::uint32_t availableCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
        std::vector<VkExtensionProperties> available(availableCount);
        if (availableCount > 0) {
            vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
        }

        bool enableValidation = options_.validate;
        if (enableValidation && !hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            logWarn("VK_EXT_debug_utils is not available; validation output disabled");
            enableValidation = false;
        }
        if (enableValidation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        std::vector<const char*> layers;
        if (enableValidation) {
            std::uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> layerProperties(layerCount);
            if (layerCount > 0) {
                vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data());
            }
            const bool hasKhronos = std::any_of(
                layerProperties.begin(), layerProperties.end(), [](const VkLayerProperties& layer) {
                    return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
                });
            if (hasKhronos) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
            } else {
                logWarn("VK_LAYER_KHRONOS_validation is not installed; continuing without it");
            }
        }

        VkApplicationInfo applicationInfo{};
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.pApplicationName = "kotor_engine";
        applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        applicationInfo.pEngineName = "kotor_engine";
        applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        applicationInfo.apiVersion = instanceApiVersion_;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();

        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            error = concat("vkCreateInstance failed: ", resultName(result),
                           ". Check the Vulkan loader/ICD installation.");
            return false;
        }

        volkLoadInstance(instance_);

        if (enableValidation && vkCreateDebugUtilsMessengerEXT != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = debugCallback;
            if (vkCreateDebugUtilsMessengerEXT(instance_, &messengerInfo, nullptr, &debugMessenger_)
                != VK_SUCCESS) {
                logWarn("vkCreateDebugUtilsMessengerEXT failed; continuing without messenger");
                debugMessenger_ = VK_NULL_HANDLE;
            }
        }

        logInfo(concat("Vulkan instance created (requested API ", apiVersionString(instanceApiVersion_),
                       ", loader ", apiVersionString(loaderVersion), ")"));
        return true;
    }

    bool createSurface(std::string& error)
    {
        if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
            error = concat("SDL_Vulkan_CreateSurface failed: ", SDL_GetError(),
                           ". The window may not support Vulkan (headless/dummy video?).");
            return false;
        }
        return true;
    }

    bool pickPhysicalDevice(std::string& error)
    {
        std::uint32_t deviceCount = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            error = "No Vulkan physical devices were reported by the loader/driver";
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

        int bestScore = -1;
        for (const VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);

            const auto api = properties.apiVersion;
            if (api < VK_API_VERSION_1_3) {
                logInfo(concat("Skipping '", properties.deviceName, "': API ",
                               apiVersionString(api), " < 1.3"));
                continue;
            }

            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(device, &features2);
            if (!features13.dynamicRendering) {
                logInfo(concat("Skipping '", properties.deviceName,
                               "': dynamic rendering is not supported"));
                continue;
            }

            std::uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                                     queueFamilies.data());

            std::optional<std::uint32_t> graphicsFamily;
            std::optional<std::uint32_t> presentFamily;
            for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
                if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    if (!graphicsFamily.has_value()) {
                        graphicsFamily = i;
                    }
                    VkBool32 presentSupport = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
                    if (presentSupport == VK_TRUE && !presentFamily.has_value()) {
                        presentFamily = i;
                    }
                }
            }
            if (!graphicsFamily.has_value() || !presentFamily.has_value()) {
                logInfo(concat("Skipping '", properties.deviceName,
                               "': no graphics+present queue family"));
                continue;
            }

            std::uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> deviceExtensions(extensionCount);
            if (extensionCount > 0) {
                vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                                     deviceExtensions.data());
            }
            if (!hasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                logInfo(concat("Skipping '", properties.deviceName, "': VK_KHR_swapchain missing"));
                continue;
            }

            std::uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
            std::uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                                      nullptr);
            if (formatCount == 0 || presentModeCount == 0) {
                logInfo(concat("Skipping '", properties.deviceName,
                               "': no surface formats or present modes"));
                continue;
            }

            int score = 0;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += 100;
            } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                score += 50;
            }
            if (api >= VK_API_VERSION_1_4) {
                score += 20;
            }
            logInfo(concat("Vulkan candidate: '", properties.deviceName, "' API ",
                           apiVersionString(api), " score ", score));

            if (score > bestScore) {
                bestScore = score;
                physicalDevice_ = device;
                graphicsFamily_ = graphicsFamily.value();
                presentFamily_ = presentFamily.value();
                deviceApiVersion_ = api;
                deviceName_ = properties.deviceName;
                switch (properties.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    deviceTypeName_ = "discrete GPU";
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    deviceTypeName_ = "integrated GPU";
                    break;
                default:
                    deviceTypeName_ = "other";
                    break;
                }
            }
        }

        if (physicalDevice_ == VK_NULL_HANDLE) {
            error =
                "No usable Vulkan device found (requires API 1.3+, dynamic rendering, graphics and "
                "present queues, VK_KHR_swapchain).";
            return false;
        }

        logInfo(concat("Selected Vulkan device: ", deviceName_, " (", deviceTypeName_, ", API ",
                       apiVersionString(deviceApiVersion_), ")"));
        return true;
    }

    bool createDevice(std::string& error)
    {
        const float priority = 1.0f;
        std::array<std::uint32_t, 2> families{graphicsFamily_, presentFamily_};
        const auto last = std::unique(families.begin(), families.end());

        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        for (auto it = families.begin(); it != last; ++it) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = *it;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            queueInfos.push_back(queueInfo);
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;

        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &features13;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = deviceExtensions;

        const VkResult result = vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            error = concat("vkCreateDevice failed: ", resultName(result));
            return false;
        }

        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
        return true;
    }

    bool createCommandResources(std::string& error)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsFamily_;

        VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            error = concat("vkCreateCommandPool failed: ", resultName(result));
            return false;
        }

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = kMaxFramesInFlight;
        result = vkAllocateCommandBuffers(device_, &allocateInfo, commandBuffers_.data());
        if (result != VK_SUCCESS) {
            error = concat("vkAllocateCommandBuffers failed: ", resultName(result));
            return false;
        }
        return true;
    }

    bool createFrameSync(std::string& error)
    {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            VkResult result =
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_[i]);
            if (result != VK_SUCCESS) {
                error = concat("vkCreateSemaphore(imageAvailable) failed: ", resultName(result));
                return false;
            }
            result = vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]);
            if (result != VK_SUCCESS) {
                error = concat("vkCreateFence failed: ", resultName(result));
                return false;
            }
        }
        return true;
    }

    bool createSwapchain(std::string& error)
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_,
                                                                    &capabilities);
        if (result != VK_SUCCESS) {
            error = concat("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: ",
                           resultName(result));
            return false;
        }

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (formatCount > 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                 formats.data());
        }

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount,
                                                  nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        if (presentModeCount > 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_,
                                                      &presentModeCount, presentModes.data());
        }

        if (formats.empty() || presentModes.empty()) {
            error = "Surface reports no formats or present modes";
            return false;
        }

        VkSurfaceFormatKHR chosenFormat = formats.front();
        bool formatFound = false;
        constexpr std::array<VkFormat, 4> preferredFormats{
            VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM};
        for (const VkFormat preferred : preferredFormats) {
            for (const VkSurfaceFormatKHR& candidate : formats) {
                if (candidate.format == preferred) {
                    chosenFormat = candidate;
                    formatFound = true;
                    break;
                }
            }
            if (formatFound) {
                break;
            }
        }
        if (!formatFound) {
            logWarn(concat("No preferred swapchain format found; using ",
                           formatName(formats.front().format)));
        }

        if (formatChosen_ && chosenFormat.format != format_) {
            error = concat("Surface format changed on swapchain recreation (", formatName(format_),
                           " -> ", formatName(chosenFormat.format),
                           "); ImGui pipeline was built for the original format");
            return false;
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window_, &width, &height);
            extent.width = static_cast<std::uint32_t>(std::clamp(width, 64, 16384));
            extent.height = static_cast<std::uint32_t>(std::clamp(height, 64, 16384));
            extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }

        std::uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }
        if (imageCount < 2) {
            imageCount = 2;
        }

        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!options_.vsync) {
            for (const VkPresentModeKHR mode : presentModes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    presentMode = mode;
                    break;
                }
            }
            if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
                logWarn("MAILBOX present mode unavailable; using FIFO with --no-vsync");
            }
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = chosenFormat.format;
        createInfo.imageColorSpace = chosenFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = pickCompositeAlpha(capabilities.supportedCompositeAlpha);
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            error = concat("vkCreateSwapchainKHR failed: ", resultName(result));
            return false;
        }

        format_ = chosenFormat.format;
        formatChosen_ = true;
        extent_ = extent;
        presentMode_ = presentMode;

        std::uint32_t swapchainImageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, nullptr);
        swapchainImages_.resize(swapchainImageCount);
        if (swapchainImageCount > 0) {
            vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount,
                                    swapchainImages_.data());
        }

        swapchainViews_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
        for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format_;
            viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            result = vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]);
            if (result != VK_SUCCESS) {
                error = concat("vkCreateImageView failed: ", resultName(result));
                return false;
            }
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderFinished_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
        for (std::size_t i = 0; i < renderFinished_.size(); ++i) {
            result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_[i]);
            if (result != VK_SUCCESS) {
                error = concat("vkCreateSemaphore(renderFinished) failed: ", resultName(result));
                return false;
            }
        }

        info_.swapchain =
            concat(formatName(format_), " ", extent_.width, "x", extent_.height, ", ",
                   swapchainImages_.size(), " images, ", presentModeName(presentMode_));
        logInfo(concat("Vulkan swapchain: ", info_.swapchain));
        return true;
    }

    bool recreateSwapchain(std::string& error)
    {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (!createSwapchain(error)) {
            return false;
        }
        if (imguiInitialized_) {
            ImGui_ImplVulkan_SetMinImageCount(static_cast<std::uint32_t>(swapchainImages_.size()));
        }
        pendingRecreate_ = false;
        return true;
    }

    void destroySwapchain()
    {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        for (VkSemaphore semaphore : renderFinished_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        renderFinished_.clear();

        for (VkImageView view : swapchainViews_) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, view, nullptr);
            }
        }
        swapchainViews_.clear();
        swapchainImages_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    bool recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                             std::string& error)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            error = "vkBeginCommandBuffer failed";
            return false;
        }

        VkImageMemoryBarrier toColor{};
        toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = swapchainImages_[imageIndex];
        toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toColor.subresourceRange.baseMipLevel = 0;
        toColor.subresourceRange.levelCount = 1;
        toColor.subresourceRange.baseArrayLayer = 0;
        toColor.subresourceRange.layerCount = 1;
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toColor);

        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(frameIndex_) * 0.02f);
        VkClearValue clearValue{};
        clearValue.color = {{0.06f, 0.07f + 0.05f * pulse, 0.10f + 0.08f * pulse, 1.0f}};

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainViews_[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clearValue;

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = {0, 0};
        renderingInfo.renderArea.extent = extent_;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent_.width);
        viewport.height = static_cast<float>(extent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = extent_;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        }

        vkCmdEndRendering(commandBuffer);

        VkImageMemoryBarrier toPresent = toColor;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toPresent);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            error = "vkEndCommandBuffer failed";
            return false;
        }
        return true;
    }

    SDL_Window* window_ = nullptr;
    Options options_;
    BackendInfo info_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    std::uint32_t instanceApiVersion_ = VK_API_VERSION_1_0;
    std::uint32_t deviceApiVersion_ = VK_API_VERSION_1_0;
    std::uint32_t graphicsFamily_ = 0;
    std::uint32_t presentFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool formatChosen_ = false;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkSemaphore> renderFinished_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers_{};
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
    std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};

    std::uint32_t currentFrame_ = 0;
    std::uint64_t frameIndex_ = 0;
    bool pendingRecreate_ = false;
    bool initialized_ = false;
    bool platformInitialized_ = false;
    bool imguiInitialized_ = false;

    std::string deviceName_ = "unknown";
    std::string deviceTypeName_ = "unknown";
};

} // namespace

std::unique_ptr<IRenderBackend> createVulkanBackend(std::string& error)
{
    (void)error;
    return std::make_unique<VulkanBackend>();
}

#else

std::unique_ptr<IRenderBackend> createVulkanBackend(std::string& error)
{
    error = "Vulkan backend is disabled in this build (KOTOR_ENGINE_ENABLE_VULKAN=OFF)";
    return nullptr;
}

#endif

} // namespace ke
