#pragma once

#include "app/Args.h"
#include "render/SceneView.h"

#include "kotor/Formats.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>

namespace ke {

struct BackendInfo {
    std::string name;
    std::string apiVersion;
    std::string device;
    std::string swapchain;
};

enum class FrameStatus {
    Ok,
    Recreate, // Backend handled or requests swapchain recreation; caller continues
    Fatal
};

// Rendering backends only own graphics resources and their ImGui renderer
// backend. The application owns the SDL window, the ImGui context and the
// per-frame UI construction.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual const char* name() const noexcept = 0;
    virtual bool supportsImGui() const noexcept { return true; }

    virtual bool initialize(SDL_Window* window, const Options& options, std::string& error) = 0;
    virtual bool initializeImGui(SDL_Window* window, std::string& error) = 0;

    // Called once per frame before ImGui::NewFrame().
    virtual void newFrame() = 0;

    // Clears/presents one frame and draws the ImGui draw data.
    virtual FrameStatus renderFrame(std::string& error) = 0;

    // Releases GPU resources and the ImGui renderer/platform backends.
    virtual void shutdown() = 0;

    // Movie overlay: uploads one decoded RGBA frame and returns a backend
    // texture identifier suitable for ImGui::Image. Callers release it with
    // releaseMovieFrame before backend shutdown.
    virtual bool uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                                  std::uint64_t& textureId, std::string& error)
    {
        (void)width;
        (void)height;
        (void)rgba;
        (void)textureId;
        error = "movie overlay is not supported by this backend";
        return false;
    }
    virtual void releaseMovieFrame(std::uint64_t& textureId) { (void)textureId; }

    // Scene pass hooks. Backends that cannot draw a scene keep the defaults;
    // the application still runs its logical loop and HUD.
    virtual void setScene(const kotor::Scene* scene) { (void)scene; }
    virtual void setCamera(const SceneCamera& camera) { (void)camera; }
    virtual void refreshEntityMeshes(const kotor::Scene& scene) { (void)scene; }
    virtual bool screenshot(const std::filesystem::path& path) { (void)path; return false; }

    virtual const BackendInfo& info() const = 0;
};

std::unique_ptr<IRenderBackend> createBackend(BackendKind kind, std::string& error);

} // namespace ke
