#pragma once

// --soak: leak detection inside the real main loop, so a GPU backend is exercised as in play.
//   idle     one module, fixed frames per cycle
//   modules  the same module reloaded, then rounds through a varied set
//   ui       movies, GUI screens, dialogue and saves opened and closed
// Every cycle samples this process only. After warm-up the sustained growth per cycle
// (median of pairwise slopes, so one plateau step does not count) must stay under the limits.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ke {

class GameSession;
class GuiSystem;

struct ProcessSample {
    std::int64_t privateBytes = 0;
    std::int64_t workingSet = 0;
    std::int64_t heapAllocated = 0; // live bytes in the process heap, -1 when unknown
    std::int64_t gpuBytes = -1;     // this process's video memory by DXGI, -1 when unknown
    std::int64_t handles = 0;
    std::int64_t gdiObjects = 0;
    std::int64_t userObjects = 0;
};

ProcessSample sampleProcess();

// Median of the slopes between every pair of points (Theil-Sen); 0 under two points.
double sustainedSlope(const std::vector<double>& values);

// Index of the first sample the fit uses: a third is warm-up, at least one and at most all but two.
std::size_t soakWarmup(std::size_t samples);

class SoakGate {
public:
    struct Settings {
        std::string scenario;
        std::string module;
        std::vector<std::string> modules; // the varied set
        std::filesystem::path out;
        int cycles = 0; // 0 takes the scenario's default
    };

    explicit SoakGate(Settings settings);

    void step(GameSession& game, GuiSystem* gui);
    bool finished() const { return finished_; }
    int exitCode() const { return exitCode_; }
    // The scene was rebuilt under the same module name: the backend must take it again.
    bool takeSceneRebuilt();

private:
    struct Series {
        std::string name;
        std::vector<ProcessSample> samples;
    };

    void stepIdle();
    void stepModules(GameSession& game);
    void stepUi(GameSession& game, GuiSystem* gui);
    void sample(Series& series, GameSession& game);
    void finish();
    int judge(const Series& series);

    Settings settings_;
    std::ofstream log_;
    Series first_;
    Series second_;
    Series tour_;
    bool finished_ = false;
    int exitCode_ = 0;
    bool sceneRebuilt_ = false;
    int frame_ = 0;
    int cycle_ = 0;
    std::size_t stage_ = 0;
    std::vector<std::string> schedule_;
    std::vector<std::string> movies_;
};

} // namespace ke
