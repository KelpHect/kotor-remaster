#pragma once

#include "app/Args.h"
#include "render/SceneView.h"
#include "render/VideoEffect.h"

#include "kotor/Formats.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

// One 3D GUI model control (retail 3D_MODEL*/LBL_HEAD): a camera plus a pixel
// rectangle of the window. Every view draws entities of the shared preview scene
// (setPreviewScene); `entities` lists which scene entity indices this view shows,
// so one scene can hold many models. Coordinates are physical window pixels with
// the origin at the top-left.
struct PreviewView {
    SceneCamera camera;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> entities;
    bool drawStatic = false; // also draw the preview scene's static geometry (room)
    kotor::Vec3 background{0.02f, 0.02f, 0.04f};
};

// The last presented frame as tightly packed RGBA8, top-left origin.
struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

constexpr int kMaxPreviewViews = 8;

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
    // Post-processing state from ACT_508/510 (see render/VideoEffect.h). Applied
    // to the 3D scene only; the UI is drawn after it and is never tinted.
    virtual void setVideoEffect(const VideoEffectState& effect) { (void)effect; }
    virtual void refreshEntityMeshes(const kotor::Scene& scene) { (void)scene; }
    virtual bool screenshot(const std::filesystem::path& path) { (void)path; return false; }

    // ---- 3D GUI previews (character generation model controls) ----------------
    // The preview scene is separate from the world scene: it is uploaded once with
    // setPreviewScene (nullptr clears it) and must stay alive until replaced. Each
    // frame the backend draws, after the world pass and before the UI, every view
    // of setPreviewViews: scissor and viewport = the view rectangle, colour and
    // depth cleared inside it only, then the entities the view lists.
    virtual bool supportsPreview() const noexcept { return false; }
    virtual bool setPreviewScene(const kotor::Scene* scene, std::string& error)
    {
        (void)scene;
        error = "3D previews are not supported by this backend";
        return false;
    }
    virtual void setPreviewViews(const std::vector<PreviewView>& views) { (void)views; }
    // Re-uploads the preview scene's entity vertices after the app re-posed them.
    virtual void refreshPreviewMeshes() {}

    // A generic RGBA8 image for ImGui::Image (portraits). textureId in/out like
    // uploadMovieFrame; several may be live at once.
    virtual bool uploadUiImage(int width, int height, const std::uint8_t* rgba,
                               std::uint64_t& textureId, std::string& error)
    {
        (void)width;
        (void)height;
        (void)rgba;
        (void)textureId;
        error = "UI images are not supported by this backend";
        return false;
    }
    virtual void releaseUiImage(std::uint64_t& textureId) { (void)textureId; }

    // The most recently presented frame, for pixel assertions.
    virtual bool captureFrame(CapturedFrame& frame) { (void)frame; return false; }

    virtual const BackendInfo& info() const = 0;
};

std::unique_ptr<IRenderBackend> createBackend(BackendKind kind, std::string& error);

} // namespace ke
