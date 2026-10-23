#include "app/Application.h"

#include "core/Log.h"
#include "render/Backend.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ke {

namespace {

constexpr const char* kEngineVersion = "0.1.0";

struct RuntimeState {
    SDL_Window* window = nullptr;
    std::unique_ptr<IRenderBackend> backend;
    bool imguiReady = false;
    BackendKind activeBackend = BackendKind::Null;
};

SDL_WindowFlags windowFlagsFor(BackendKind kind, const Options& options)
{
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    switch (kind) {
    case BackendKind::Vulkan:
        flags |= SDL_WINDOW_VULKAN;
        break;
    case BackendKind::OpenGL:
        flags |= SDL_WINDOW_OPENGL;
        break;
    case BackendKind::Null:
    case BackendKind::Auto:
        break;
    }
    if (options.hidden || options.headless || kind == BackendKind::Null) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

std::string buildLabel(BackendKind kind)
{
    switch (kind) {
    case BackendKind::Vulkan:
        return "Vulkan";
    case BackendKind::OpenGL:
        return "OpenGL";
    case BackendKind::Null:
        return "Null";
    case BackendKind::Auto:
        return "Auto";
    }
    return "Unknown";
}

int runImGuiSelfTest()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;

    // No renderer backend exists in the self-test, so build the legacy font
    // atlas manually and assign a dummy texture id. This exercises the same
    // NewFrame/Render path used by the GPU backends.
    unsigned char* pixels = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &textureWidth, &textureHeight);
    if (pixels == nullptr || textureWidth <= 0 || textureHeight <= 0) {
        ke::logError("ImGui self-test FAILED: font atlas rasterization produced no pixels");
        ImGui::DestroyContext();
        return 7;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    ke::logInfo(concat("ImGui self-test: atlas ", textureWidth, "x", textureHeight, ", ",
                       io.Fonts->Fonts.Size, " font(s), built=", io.Fonts->IsBuilt() ? 1 : 0));

    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(210.0f, 110.0f),
                                                  IM_COL32(40, 120, 200, 255));
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 120.0f));
    const bool windowVisible = ImGui::Begin("Headless ImGui self-test");
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextUnformatted("Context, font atlas and draw-list generation are functional.");
    ImGui::End();
    ke::logInfo(concat("ImGui self-test: window visible=", windowVisible ? 1 : 0));
    ImGui::Render();

    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData != nullptr) {
        ke::logInfo(concat("ImGui self-test: drawData valid=", drawData->Valid ? 1 : 0,
                           " cmdLists=", drawData->CmdListsCount,
                           " vtx=", drawData->TotalVtxCount,
                           " idx=", drawData->TotalIdxCount,
                           " display=", drawData->DisplaySize.x, "x", drawData->DisplaySize.y));
    }
    const bool ok = drawData != nullptr && drawData->Valid && drawData->CmdListsCount > 0
        && drawData->TotalVtxCount > 0;

    if (ok) {
        logInfo(concat("ImGui self-test PASSED (ImGui ", IMGUI_VERSION, ", ", textureWidth, "x",
                       textureHeight, " font atlas, ", drawData->CmdListsCount,
                       " draw list(s), ", drawData->TotalVtxCount, " vertices)"));
    } else {
        logError("ImGui self-test FAILED: no valid draw data was produced");
    }

    ImGui::DestroyContext();
    return ok ? 0 : 7;
}

void destroyRuntime(RuntimeState& state)
{
    if (state.backend) {
        state.backend->shutdown();
        state.backend.reset();
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
    }
    if (state.window != nullptr) {
        SDL_DestroyWindow(state.window);
        state.window = nullptr;
    }
}

void drawDebugUi(const RuntimeState& state, std::uint64_t frames, double frameMs,
                 double elapsedSeconds, const std::string& exitReason)
{
    static bool showDemo = false;

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("KotOR Remaster - Engine Scaffold", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize);

    const BackendInfo& info = state.backend->info();
    ImGui::Text("Engine scaffold %s (Lane 08)", kEngineVersion);
    ImGui::Text("Backend: %s", info.name.c_str());
    ImGui::Text("API: %s", info.apiVersion.c_str());
    ImGui::Text("Device: %s", info.device.c_str());
    ImGui::Text("Swapchain: %s", info.swapchain.c_str());
    ImGui::Separator();
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frames));
    ImGui::Text("CPU frame time: %.2f ms (%.1f FPS)", frameMs,
                frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
    ImGui::Text("Elapsed: %.2f s", elapsedSeconds);
    ImGui::Text("Exit: %s", exitReason.c_str());
    ImGui::Separator();
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::Text("Vulkan 1.4 primary / OpenGL 4.5 fallback");
    ImGui::Checkbox("Show ImGui demo window", &showDemo);
    ImGui::Text("Close the window or press Esc to exit.");
    ImGui::End();

    if (showDemo) {
        ImGui::ShowDemoWindow(&showDemo);
    }
}

} // namespace

int runApplication(const Options& options)
{
    if (options.selfTest) {
        return runImGuiSelfTest();
    }

    logInfo(concat("kotor_engine ", kEngineVersion, " starting (SDL ", SDL_MAJOR_VERSION, ".",
                   SDL_MINOR_VERSION, ".", SDL_MICRO_VERSION, ")"));

    if (options.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        logInfo("Headless mode requested: forcing the SDL dummy video driver");
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        logError(concat("SDL_Init failed: ", SDL_GetError()));
        return 3;
    }

    const char* videoDriver = SDL_GetCurrentVideoDriver();
    logInfo(concat("SDL video driver: ", videoDriver != nullptr ? videoDriver : "<unknown>"));

    RuntimeState state;

    std::vector<BackendKind> candidates;
    if (options.backend == BackendKind::Auto) {
        if (options.headless) {
            candidates = {BackendKind::Null};
        } else {
            candidates = {BackendKind::Vulkan};
            if (options.allowFallback) {
                candidates.push_back(BackendKind::OpenGL);
            }
        }
    } else {
        candidates = {options.backend};
    }

    std::string error;
    for (const BackendKind kind : candidates) {
        logInfo(concat("Trying backend: ", buildLabel(kind)));
        state.window = SDL_CreateWindow(options.title.c_str(), options.width, options.height,
                                        windowFlagsFor(kind, options));
        if (state.window == nullptr) {
            logWarn(concat(buildLabel(kind), " window creation failed: ", SDL_GetError()));
            continue;
        }

        state.backend = createBackend(kind, error);
        if (!state.backend) {
            logWarn(concat(buildLabel(kind), " backend unavailable: ", error));
            SDL_DestroyWindow(state.window);
            state.window = nullptr;
            continue;
        }

        error.clear();
        if (!state.backend->initialize(state.window, options, error)) {
            logWarn(concat(buildLabel(kind), " initialization failed: ", error));
            state.backend->shutdown();
            state.backend.reset();
            SDL_DestroyWindow(state.window);
            state.window = nullptr;
            continue;
        }

        state.activeBackend = kind;
        break;
    }

    if (!state.backend || state.window == nullptr) {
        logError(concat("No usable render backend for the requested selection (",
                        backendName(options.backend), "). See the messages above."));
        SDL_Quit();
        return 5;
    }

    logInfo(concat("Backend ready: ", state.backend->info().name));
    logInfo(concat("  API:       ", state.backend->info().apiVersion));
    logInfo(concat("  Device:    ", state.backend->info().device));
    logInfo(concat("  Swapchain: ", state.backend->info().swapchain));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // deterministic, no stray imgui.ini in test runs
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (state.backend->supportsImGui()) {
        if (!state.backend->initializeImGui(state.window, error)) {
            logError(concat("ImGui backend initialization failed: ", error));
            destroyRuntime(state);
            SDL_Quit();
            return 5;
        }
        state.imguiReady = true;
        logInfo(concat("ImGui ", IMGUI_VERSION, " renderer initialized"));
    } else {
        logInfo("ImGui disabled for the null backend (use a GPU backend for the overlay)");
    }

    std::uint64_t frames = 0;
    bool loggedUiGeometry = false;
    bool running = true;
    std::string exitReason = "loop exited";
    double smoothedFrameMs = 0.0;
    const std::uint64_t startTicks = SDL_GetTicksNS();
    std::uint64_t previousTicks = startTicks;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (state.imguiReady) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                exitReason = "window closed";
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                if (!state.imguiReady || !ImGui::GetIO().WantCaptureKeyboard) {
                    running = false;
                    exitReason = "escape pressed";
                }
            }
        }

        if (!running) {
            break;
        }

        const std::uint64_t now = SDL_GetTicksNS();
        const double deltaMs = static_cast<double>(now - previousTicks) / 1'000'000.0;
        previousTicks = now;
        smoothedFrameMs = (smoothedFrameMs <= 0.0) ? deltaMs : (smoothedFrameMs * 0.9 + deltaMs * 0.1);
        const double elapsedSeconds = static_cast<double>(now - startTicks) / 1'000'000'000.0;

        if (state.imguiReady) {
            state.backend->newFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            drawDebugUi(state, frames, smoothedFrameMs, elapsedSeconds, exitReason);
            ImGui::Render();

            const ImDrawData* drawData = ImGui::GetDrawData();
            if (!loggedUiGeometry && drawData != nullptr && drawData->TotalVtxCount > 0) {
                logInfo(concat("ImGui draw data at frame ", frames, ": ",
                               drawData->CmdListsCount, " list(s), ", drawData->TotalVtxCount,
                               " vertices"));
                loggedUiGeometry = true;
            }
        }

        error.clear();
        const FrameStatus status = state.backend->renderFrame(error);
        if (status == FrameStatus::Fatal) {
            logError(concat("Frame failed: ", error));
            destroyRuntime(state);
            SDL_Quit();
            return 6;
        }

        ++frames;

        if (options.frames > 0 && frames >= static_cast<std::uint64_t>(options.frames)) {
            exitReason = concat("frame limit reached (", options.frames, ")");
            running = false;
        } else if (options.seconds > 0.0 && elapsedSeconds >= options.seconds) {
            exitReason = concat("time limit reached (", options.seconds, " s)");
            running = false;
        }

        if (state.activeBackend == BackendKind::Null && running) {
            SDL_Delay(1); // avoid a 100% CPU spin in the headless logical loop
        }
    }

    const double totalSeconds = static_cast<double>(SDL_GetTicksNS() - startTicks) / 1'000'000'000.0;
    const double averageMs = frames > 0 ? (totalSeconds * 1000.0) / static_cast<double>(frames) : 0.0;

    logInfo(concat("Exit: ", exitReason));
    logInfo(concat("Frames: ", frames, " in ", totalSeconds, " s (avg ", averageMs, " ms/frame)"));
    logInfo(concat("Backend summary: ", state.backend->info().name, " | ",
                   state.backend->info().apiVersion, " | ", state.backend->info().swapchain));

    destroyRuntime(state);
    SDL_Quit();
    return 0;
}

} // namespace ke
