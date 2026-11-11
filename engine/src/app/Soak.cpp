#include "app/Soak.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#endif

namespace ke {
namespace {

constexpr int kIdleCycleFrames = 600;
constexpr int kModuleSettleFrames = 30;
constexpr int kUiSettleFrames = 5;
constexpr int kDefaultIdleCycles = 60; // sound caches take about 25 cycles to warm up
constexpr int kDefaultModuleCycles = 12;
constexpr int kDefaultUiCycles = 12;
// The sound and script caches fill as game time passes and reach their ceiling only after a dozen rounds (the
// sound cache then trades a few MB between tracks); judged over fewer rounds, that fill reads as a leak.
// A run of 48 rounds stayed flat at 342 MB (slope 18 KiB per cycle against the unchanged 256 KiB limit).
constexpr std::size_t kModuleLoadsInRounds = 120; // shared out over at least three rounds

// Gate limits on sustained growth per cycle. Heap bytes are the strict leak measure and follow
// what a cycle does: 10 game seconds (idle, 32 KiB is about 12 MB an hour), one whole module, or
// a handful of open and close. Private bytes also see the allocator keep and return free
// segments, which swings them by megabytes with a flat heap, so their limit is wider.
constexpr double kKiB = 1024.0;
constexpr double kIdleHeapLimit = 32.0 * kKiB;
constexpr double kModuleHeapLimit = 256.0 * kKiB;
constexpr double kUiHeapLimit = 64.0 * kKiB;
constexpr double kIdlePrivateLimit = 64.0 * kKiB;
constexpr double kBusyPrivateLimit = 2048.0 * kKiB;
constexpr double kGpuLimit = 1024.0 * kKiB;
constexpr double kHandleLimit = 0.5;
constexpr double kGuiObjectLimit = 0.25;

const char* const kScreens[] = {"mainmenu", "optionsmain", "saveload"};

#ifdef _WIN32
std::int64_t gpuBytesInUse()
{
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    static IDXGIFactory1* factory = []() -> IDXGIFactory1* {
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (dxgi == nullptr) {
            return nullptr;
        }
        const auto create = reinterpret_cast<CreateFactory>(
            reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        IDXGIFactory1* made = nullptr;
        if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&made)))) {
            return nullptr;
        }
        return made;
    }();
    if (factory == nullptr) {
        return -1;
    }
    std::int64_t total = -1;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            for (const DXGI_MEMORY_SEGMENT_GROUP group :
                 {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, group, &info))) {
                    total = std::max<std::int64_t>(total, 0) + static_cast<std::int64_t>(info.CurrentUsage);
                }
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    return total;
}
#endif

double metric(const ProcessSample& sample, int which)
{
    switch (which) {
    case 0: return static_cast<double>(sample.privateBytes);
    case 1: return static_cast<double>(sample.heapAllocated);
    case 2: return static_cast<double>(sample.gpuBytes);
    case 3: return static_cast<double>(sample.handles);
    case 4: return static_cast<double>(sample.gdiObjects);
    default: return static_cast<double>(sample.userObjects);
    }
}

} // namespace

ProcessSample sampleProcess()
{
    ProcessSample out;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        out.privateBytes = static_cast<std::int64_t>(memory.PrivateUsage);
        out.workingSet = static_cast<std::int64_t>(memory.WorkingSetSize);
    }
    HEAP_SUMMARY heap{};
    heap.cb = sizeof(heap);
    out.heapAllocated = HeapSummary(GetProcessHeap(), 0, &heap) ? static_cast<std::int64_t>(heap.cbAllocated) : -1;
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        out.handles = handles;
    }
    out.gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    out.gpuBytes = gpuBytesInUse();
#endif
    return out;
}

double sustainedSlope(const std::vector<double>& values)
{
    std::vector<double> slopes;
    for (std::size_t a = 0; a < values.size(); ++a) {
        for (std::size_t b = a + 1; b < values.size(); ++b) {
            slopes.push_back((values[b] - values[a]) / static_cast<double>(b - a));
        }
    }
    if (slopes.empty()) {
        return 0.0;
    }
    const std::size_t middle = slopes.size() / 2;
    std::nth_element(slopes.begin(), slopes.begin() + static_cast<std::ptrdiff_t>(middle), slopes.end());
    return slopes[middle];
}

std::size_t soakWarmup(std::size_t samples)
{
    if (samples <= 2) {
        return 0;
    }
    return std::min(std::max<std::size_t>(samples / 3, 1), samples - 2);
}

SoakGate::SoakGate(Settings settings) : settings_(std::move(settings))
{
    std::error_code ignored;
    std::filesystem::create_directories(settings_.out, ignored);
    log_.open(settings_.out / ("soak_" + settings_.scenario + ".jsonl"), std::ios::trunc);
    first_.name = settings_.scenario == "modules" ? "same" : settings_.scenario;
    second_.name = "rounds";
    tour_.name = "tour";
    if (settings_.cycles <= 0) {
        settings_.cycles = settings_.scenario == "idle" ? kDefaultIdleCycles
            : settings_.scenario == "modules"           ? kDefaultModuleCycles
                                                        : kDefaultUiCycles;
    }
    if (settings_.scenario == "modules") {
        for (int i = 0; i < settings_.cycles; ++i) {
            schedule_.push_back(settings_.module);
        }
        const std::size_t rounds = std::max<std::size_t>(3, kModuleLoadsInRounds / (settings_.modules.size() + 1));
        for (std::size_t round = 0; round < rounds; ++round) {
            for (const std::string& module : settings_.modules) {
                schedule_.push_back(module);
            }
            schedule_.push_back(settings_.module);
        }
    }
}

bool SoakGate::takeSceneRebuilt()
{
    const bool value = sceneRebuilt_;
    sceneRebuilt_ = false;
    return value;
}

void SoakGate::sample(Series& series, GameSession& game)
{
    const ProcessSample sampled = sampleProcess();
    series.samples.push_back(sampled);
    if (log_) {
        log_ << "{\"series\":\"" << series.name << "\",\"cycle\":" << series.samples.size()
             << ",\"module\":\"" << game.module() << "\",\"private\":" << sampled.privateBytes
             << ",\"ws\":" << sampled.workingSet << ",\"heap\":" << sampled.heapAllocated
             << ",\"gpu\":" << sampled.gpuBytes << ",\"handles\":" << sampled.handles
             << ",\"gdi\":" << sampled.gdiObjects << ",\"user\":" << sampled.userObjects << "}\n";
        log_.flush();
    }
}

void SoakGate::step(GameSession& game, GuiSystem* gui)
{
    if (finished_) {
        return;
    }
    ++frame_;
    if (settings_.scenario == "idle") {
        if (frame_ >= kIdleCycleFrames) {
            frame_ = 0;
            sample(first_, game);
            stepIdle();
        }
    } else if (settings_.scenario == "modules") {
        stepModules(game);
    } else if (settings_.scenario == "ui") {
        stepUi(game, gui);
    } else {
        logError(concat("soak: unknown scenario ", settings_.scenario));
        exitCode_ = 2;
        finished_ = true;
    }
}

void SoakGate::stepIdle()
{
    if (static_cast<int>(first_.samples.size()) >= settings_.cycles) {
        finish();
    }
}

void SoakGate::stepModules(GameSession& game)
{
    if (frame_ < kModuleSettleFrames) {
        return;
    }
    frame_ = 0;
    // The settled state of what the previous step asked for.
    if (stage_ > 0 && stage_ <= static_cast<std::size_t>(settings_.cycles)) {
        sample(first_, game);
    } else if (stage_ > static_cast<std::size_t>(settings_.cycles)) {
        // Only the return to the base module compares like with like; the rest is the curve.
        sample(schedule_[stage_ - 1] == settings_.module ? second_ : tour_, game);
    }
    if (stage_ >= schedule_.size()) {
        finish();
        return;
    }
    game.requestModule(schedule_[stage_]);
    sceneRebuilt_ = true;
    ++stage_;
}

void SoakGate::stepUi(GameSession& game, GuiSystem* gui)
{
    if (frame_ < kUiSettleFrames) {
        return;
    }
    frame_ = 0;
    constexpr std::size_t kScreenCount = sizeof(kScreens) / sizeof(kScreens[0]);
    const std::size_t stages = 6 + kScreenCount * 2;
    const std::size_t stage = stage_ % stages;
    ++stage_;
    if (movies_.empty()) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(game.installPath()) / "movies", error)) {
            if (entry.path().extension() == ".bik" || entry.path().extension() == ".BIK") {
                movies_.push_back(entry.path().stem().string());
            }
        }
        std::sort(movies_.begin(), movies_.end());
        movies_.resize(std::min<std::size_t>(movies_.size(), 1));
        movies_.push_back({}); // the list is never empty again, and an empty name is skipped
    }
    if (stage == 0) {
        if (!movies_.front().empty() && !game.playMovie(movies_.front())) {
            logWarn(concat("soak: movie did not start: ", movies_.front()));
        }
    } else if (stage == 1) {
        game.stopMovie();
    } else if (stage == 2) {
        for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
            if (game.startDialogue(i)) {
                break;
            }
        }
    } else if (stage == 3) {
        game.endDialogue();
    } else if (stage == 4) {
        const std::filesystem::path slot = settings_.out / "soak_save";
        std::error_code error;
        std::filesystem::create_directories(slot, error);
        if (!game.saveOriginalSave(slot, "soak")) {
            logWarn("soak: save failed");
        }
    } else if (stage == 5) {
        if (!game.loadOriginalSave(settings_.out / "soak_save")) {
            logWarn("soak: load failed");
        }
        sceneRebuilt_ = true;
    } else if (gui != nullptr) {
        const std::size_t index = (stage - 6) / 2;
        if ((stage - 6) % 2 == 0) {
            if (!gui->open(kScreens[index])) {
                logWarn(concat("soak: screen did not open: ", kScreens[index]));
            }
        } else {
            gui->close();
        }
    }
    if (stage == stages - 1) {
        sample(first_, game);
        if (static_cast<int>(first_.samples.size()) >= settings_.cycles) {
            finish();
        }
    }
}

int SoakGate::judge(const Series& series)
{
    if (series.samples.size() < 3) {
        return 0;
    }
    const double privateLimit = settings_.scenario == "idle" ? kIdlePrivateLimit : kBusyPrivateLimit;
    const double heapLimit = settings_.scenario == "idle" ? kIdleHeapLimit
        : settings_.scenario == "modules"                 ? kModuleHeapLimit
                                                          : kUiHeapLimit;
    const struct {
        const char* name;
        double limit;
    } metrics[] = {{"private", privateLimit}, {"heap", heapLimit}, {"gpu", kGpuLimit},
                   {"handles", kHandleLimit}, {"gdi", kGuiObjectLimit}, {"user", kGuiObjectLimit}};
    const std::size_t from = soakWarmup(series.samples.size());
    int failures = 0;
    for (int which = 0; which < 6; ++which) {
        std::vector<double> values;
        for (std::size_t i = from; i < series.samples.size(); ++i) {
            values.push_back(metric(series.samples[i], which));
        }
        const double slope = sustainedSlope(values);
        const bool known = values.front() >= 0.0;
        const bool pass = !known || slope <= metrics[which].limit;
        failures += pass ? 0 : 1;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "soak %s %s: first=%.0f last=%.0f slope/cycle=%.1f limit=%.1f samples=%zu %s",
                      series.name.c_str(), metrics[which].name, values.front(), values.back(), slope,
                      metrics[which].limit, values.size(), !known ? "UNKNOWN" : pass ? "PASS" : "FAIL");
        logInfo(line);
    }
    return failures;
}

void SoakGate::finish()
{
    const int failures = judge(first_) + judge(second_);
    logInfo(concat("soak ", settings_.scenario, ": ", failures == 0 ? "PASS" : "FAIL"));
    exitCode_ = failures == 0 ? 0 : 1;
    finished_ = true;
    try {
        std::error_code ignored;
        std::filesystem::remove_all(settings_.out / "soak_save", ignored);
    } catch (const std::exception&) {
    }
}

} // namespace ke
