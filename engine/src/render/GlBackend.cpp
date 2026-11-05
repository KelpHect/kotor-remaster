#include "render/BackendFactory.h"
#include "render/Backend.h"
#include "render/GlScene.h"
#include "core/Log.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define KE_GLAPI __stdcall
#else
#define KE_GLAPI
#endif

namespace ke {

#if KE_ENABLE_OPENGL

namespace {

using GlClearColorFn = void(KE_GLAPI*)(float, float, float, float);
using GlClearFn = void(KE_GLAPI*)(std::uint32_t);
using GlViewportFn = void(KE_GLAPI*)(int, int, int, int);
using GlGetStringFn = const unsigned char*(KE_GLAPI*)(std::uint32_t);

constexpr std::uint32_t kGlColorBufferBit = 0x00004000u;
constexpr std::uint32_t kGlVersion = 0x1F02u;
constexpr std::uint32_t kGlRenderer = 0x1F01u;
constexpr std::uint32_t kGlVendor = 0x1F00u;

class OpenGlBackend final : public IRenderBackend {
public:
    ~OpenGlBackend() override
    {
        if (initialized_) {
            shutdown();
        }
    }

    const char* name() const noexcept override { return "opengl"; }

    bool initialize(SDL_Window* window, const Options& options, std::string& error) override
    {
        window_ = window;

        if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4)
            || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5)
            || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE)
            || !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)
            || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)
            || !SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8)) {
            error = concat("SDL_GL_SetAttribute failed: ", SDL_GetError());
            return false;
        }

        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            error = concat("OpenGL 4.5 core context unavailable: ", SDL_GetError());
            return false;
        }
        if (!SDL_GL_MakeCurrent(window_, context_)) {
            error = concat("SDL_GL_MakeCurrent failed: ", SDL_GetError());
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            return false;
        }

        if (!SDL_GL_SetSwapInterval(options.vsync ? 1 : 0)) {
            logWarn(concat("SDL_GL_SetSwapInterval failed (continuing): ", SDL_GetError()));
        }

        glClearColor_ = reinterpret_cast<GlClearColorFn>(SDL_GL_GetProcAddress("glClearColor"));
        glClear_ = reinterpret_cast<GlClearFn>(SDL_GL_GetProcAddress("glClear"));
        glViewport_ = reinterpret_cast<GlViewportFn>(SDL_GL_GetProcAddress("glViewport"));
        glGetString_ = reinterpret_cast<GlGetStringFn>(SDL_GL_GetProcAddress("glGetString"));

        if (glClearColor_ == nullptr || glClear_ == nullptr || glViewport_ == nullptr
            || glGetString_ == nullptr) {
            error = "Required OpenGL entry points could not be loaded";
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            return false;
        }

        const auto* version = glGetString_(kGlVersion);
        const auto* renderer = glGetString_(kGlRenderer);
        const auto* vendor = glGetString_(kGlVendor);
        if (version == nullptr) {
            error = "glGetString(GL_VERSION) returned null";
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            return false;
        }

        info_.name = "opengl";
        info_.apiVersion = reinterpret_cast<const char*>(version);
        info_.device = concat(reinterpret_cast<const char*>(vendor), " / ",
                              reinterpret_cast<const char*>(renderer));
        info_.swapchain = "double-buffered SDL window";

        int major = 0;
        int minor = 0;
        const char* versionText = info_.apiVersion.c_str();
        char* versionEnd = nullptr;
        major = static_cast<int>(std::strtol(versionText, &versionEnd, 10));
        if (versionEnd != versionText && *versionEnd == '.') {
            minor = static_cast<int>(std::strtol(versionEnd + 1, nullptr, 10));
        }
        if (major <= 0 || (major * 10 + minor) < 45) {
            error = concat("OpenGL 4.5 is required, got: ", info_.apiVersion);
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            return false;
        }

        initialized_ = true;
        if (!sceneRenderer_.initialize(error)) {
            logWarn(concat("Scene renderer unavailable; HUD-only OpenGL: ", error));
        } else {
            sceneReady_ = true;
        }
        return true;
    }

    bool initializeImGui(SDL_Window* window, std::string& error) override
    {
        if (!ImGui_ImplSDL3_InitForOpenGL(window, context_)) {
            error = "ImGui_ImplSDL3_InitForOpenGL failed";
            return false;
        }
        platformInitialized_ = true;

        if (!ImGui_ImplOpenGL3_Init("#version 450")) {
            error = "ImGui_ImplOpenGL3_Init failed";
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
            ImGui_ImplOpenGL3_NewFrame();
        }
    }

    void setScene(const kotor::Scene* scene) override
    {
        scene_ = scene;
        if (scene_ != nullptr && sceneReady_) {
            std::string uploadError;
            if (!sceneRenderer_.upload(*scene_, uploadError)) {
                logError(concat("Scene upload failed: ", uploadError));
            }
        }
    }

    void setCamera(const SceneCamera& camera) override { camera_ = camera; }

    void setVideoEffect(const VideoEffectState& effect) override
    {
        sceneRenderer_.setVideoEffect(effect);
    }

    void refreshEntityMeshes(const kotor::Scene& scene) override
    {
        if (sceneReady_) {
            sceneRenderer_.refreshEntityMeshes(scene);
        }
    }

    bool screenshot(const std::filesystem::path& path) override
    {
        if (!sceneReady_ || scene_ == nullptr || window_ == nullptr) {
            return false;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        return sceneRenderer_.screenshot(path, width, height);
    }

    bool uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                          std::uint64_t& textureId, std::string& error) override
    {
        GlUint texture = static_cast<GlUint>(textureId);
        if (!sceneRenderer_.uploadMovieFrame(width, height, rgba, texture, error)) {
            return false;
        }
        textureId = texture;
        return true;
    }

    void releaseMovieFrame(std::uint64_t& textureId) override
    {
        GlUint texture = static_cast<GlUint>(textureId);
        sceneRenderer_.destroyMovieFrame(texture);
        textureId = texture;
    }

    bool supportsPreview() const noexcept override { return initialized_ && sceneReady_; }

    bool setPreviewScene(const kotor::Scene* scene, std::string& error) override
    {
        if (!initialized_ || !sceneReady_) {
            error = "OpenGL scene renderer is unavailable";
            return false;
        }
        if (scene == nullptr) {
            previewScene_ = nullptr;
            if (previewReady_) {
                previewRenderer_.clearScene();
            }
            return true;
        }
        if (!previewReady_) {
            if (!previewRenderer_.initialize(error)) {
                return false;
            }
            previewReady_ = true;
        }
        previewScene_ = nullptr;
        if (!previewRenderer_.upload(*scene, error)) {
            return false;
        }
        previewScene_ = scene;
        return true;
    }

    void setPreviewViews(const std::vector<PreviewView>& views) override
    {
        const std::size_t count = std::min<std::size_t>(views.size(), kMaxPreviewViews);
        previewViews_.assign(views.begin(), views.begin() + static_cast<std::ptrdiff_t>(count));
    }

    void refreshPreviewMeshes() override
    {
        if (previewReady_ && previewScene_ != nullptr) {
            previewRenderer_.refreshEntityMeshes(*previewScene_);
        }
    }

    bool uploadUiImage(int width, int height, const std::uint8_t* rgba,
                       std::uint64_t& textureId, std::string& error) override
    {
        if (!initialized_) {
            error = "OpenGL backend is not initialized";
            return false;
        }
        GlUint texture = static_cast<GlUint>(textureId);
        if (!sceneRenderer_.uploadMovieFrame(width, height, rgba, texture, error)) {
            return false;
        }
        textureId = texture;
        return true;
    }

    void releaseUiImage(std::uint64_t& textureId) override
    {
        if (!initialized_) {
            textureId = 0;
            return;
        }
        GlUint texture = static_cast<GlUint>(textureId);
        sceneRenderer_.destroyMovieFrame(texture);
        textureId = texture;
    }

    bool captureFrame(CapturedFrame& frame) override
    {
        if (!initialized_ || !sceneReady_ || window_ == nullptr) {
            return false;
        }
        // The first request arms the pre-swap read in renderFrame (the front buffer is
        // undefined after SwapWindow on some drivers); until a cached frame exists the
        // front buffer is read like screenshot() does.
        captureArmed_ = true;
        if (captureWidth_ > 0 && captureHeight_ > 0 && !capturePixels_.empty()) {
            frame.width = captureWidth_;
            frame.height = captureHeight_;
            frame.rgba = capturePixels_;
            return true;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (!sceneRenderer_.readFramePixels(false, width, height, frame.rgba)) {
            return false;
        }
        frame.width = width;
        frame.height = height;
        return true;
    }

    FrameStatus renderFrame(std::string& error) override
    {
        if (!initialized_ || window_ == nullptr) {
            error = "OpenGL backend is not initialized";
            return FrameStatus::Fatal;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (width <= 0 || height <= 0) {
            return FrameStatus::Ok; // minimized
        }

        glViewport_(0, 0, width, height);
        if (sceneReady_ && scene_ != nullptr) {
            sceneRenderer_.render(camera_, width, height);
        } else {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(frameIndex_) * 0.02f);
            glClearColor_(0.06f, 0.07f + 0.05f * pulse, 0.10f + 0.08f * pulse, 1.0f);
            glClear_(kGlColorBufferBit);
        }

        if (previewReady_ && previewScene_ != nullptr) {
            for (const PreviewView& view : previewViews_) {
                previewRenderer_.renderView(view, width, height);
            }
        }

        if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr) {
            ImGui_ImplOpenGL3_RenderDrawData(drawData);
        }

        if (captureArmed_ && sceneReady_) {
            if (sceneRenderer_.readFramePixels(true, width, height, capturePixels_)) {
                captureWidth_ = width;
                captureHeight_ = height;
            }
        }

        SDL_GL_SwapWindow(window_);
        ++frameIndex_;
        return FrameStatus::Ok;
    }

    void shutdown() override
    {
        if (imguiInitialized_) {
            ImGui_ImplOpenGL3_Shutdown();
            imguiInitialized_ = false;
        }
        if (platformInitialized_) {
            ImGui_ImplSDL3_Shutdown();
            platformInitialized_ = false;
        }
        if (previewReady_) {
            previewRenderer_.shutdown();
            previewReady_ = false;
        }
        previewScene_ = nullptr;
        previewViews_.clear();
        capturePixels_.clear();
        captureArmed_ = false;
        if (sceneReady_) {
            sceneRenderer_.shutdown();
            sceneReady_ = false;
        }
        scene_ = nullptr;
        if (context_ != nullptr) {
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }
        initialized_ = false;
        window_ = nullptr;
    }

    const BackendInfo& info() const override { return info_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    BackendInfo info_;
    bool initialized_ = false;
    bool platformInitialized_ = false;
    bool imguiInitialized_ = false;
    bool sceneReady_ = false;
    const kotor::Scene* scene_ = nullptr;
    SceneCamera camera_;
    GlScene sceneRenderer_;
    GlScene previewRenderer_;
    bool previewReady_ = false;
    const kotor::Scene* previewScene_ = nullptr;
    std::vector<PreviewView> previewViews_;
    bool captureArmed_ = false;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    std::vector<std::uint8_t> capturePixels_;
    std::uint64_t frameIndex_ = 0;

    GlClearColorFn glClearColor_ = nullptr;
    GlClearFn glClear_ = nullptr;
    GlViewportFn glViewport_ = nullptr;
    GlGetStringFn glGetString_ = nullptr;
};

} // namespace

std::unique_ptr<IRenderBackend> createOpenGlBackend(std::string& error)
{
    (void)error;
    return std::make_unique<OpenGlBackend>();
}

#else

std::unique_ptr<IRenderBackend> createOpenGlBackend(std::string& error)
{
    error = "OpenGL backend is disabled in this build (KOTOR_ENGINE_ENABLE_OPENGL=OFF)";
    return nullptr;
}

#endif

} // namespace ke
