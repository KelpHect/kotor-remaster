#include "render/BackendFactory.h"
#include "render/Backend.h"
#include "core/Log.h"

#include <string>

namespace ke {

namespace {

// Headless logical backend: validates window/event/loop plumbing without
// requiring a GPU or a real video driver. ImGui is intentionally disabled here;
// --self-test exercises the ImGui core separately.
class NullBackend final : public IRenderBackend {
public:
    const char* name() const noexcept override { return "null"; }
    bool supportsImGui() const noexcept override { return false; }

    bool initialize(SDL_Window* window, const Options& options, std::string& error) override
    {
        (void)error;
        window_ = window;
        info_.name = "null";
        info_.apiVersion = "none (headless logical loop)";
        info_.device = "no GPU device requested";
        info_.swapchain = ke::concat(options.width, "x", options.height, " (no swapchain)");
        return true;
    }

    bool initializeImGui(SDL_Window* window, std::string& error) override
    {
        (void)window;
        (void)error;
        return true;
    }

    void newFrame() override {}

    FrameStatus renderFrame(std::string& error) override
    {
        (void)error;
        return FrameStatus::Ok;
    }

    void shutdown() override { window_ = nullptr; }

    const BackendInfo& info() const override { return info_; }

private:
    SDL_Window* window_ = nullptr;
    BackendInfo info_;
};

} // namespace

std::unique_ptr<IRenderBackend> createNullBackend(std::string& error)
{
    (void)error;
    return std::make_unique<NullBackend>();
}

} // namespace ke
