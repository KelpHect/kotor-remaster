#pragma once

// --input-script: a timed walk of mouse, key and screenshot steps, read from the same text file the
// retail driver (SC\paired\walk\walk.py) plays against the original, so both sides see one route at
// one resolution and their frames pair by name. Verification tooling, not a player feature.
//
// Lines (times in milliseconds; '#' starts a comment):
//   section <name>      names the frames that follow
//   every <ms>          also shoot a frame every <ms> of script time (0 stops)
//   wait <ms>           advance the script clock
//   move <x> <y>        pointer to window pixel x,y
//   click <x> <y> [right]
//   key <Name>          tap a key (SDL scancode name: Escape, Return, W, I, F1, ...)
//   hold <Name> <ms>    press, advance the clock by <ms>, release
//   shot <name>         one frame
//   shotas <name>       one frame named exactly <name>.bmp (pairs with a retail frame of that name)
//   script <file.ncs>   run a compiled script with the live runtime (the retail walk's override scripts)
//   camera ex ey ez fx fy fz ux uy uz fovy   draw the world from this camera (retail's recorded one)
//   camera off          back to the game's camera
//   waitfor <condition> [timeout ms]   hold the script clock until the game is in that state (or the
//                       timeout passes, default 60000; logged either way); every later step moves by the
//                       time waited. Conditions (Application decides them): nogui | gui <screen> |
//                       panel <screen> | nopanel | dialogue | nodialogue | entry <n> | replies | status |
//                       nostatus
//   goto|use|talk|dialogue|autodialogue|fight|module|expect|idle|partyselect|note|list|loot|equip|leader|unlock ...
//                       walk steps (app/WalkTools.h): the script plays the game through the player's own paths and
//                       holds every later step until the step reports done; a failed step ends the script. The
//                       grammar is in engine/tests/walk/STEPS.md.
//   if <condition> <step>   the step (click, key, shot, ...) runs only if the condition holds when it comes due;
//                       the conditions are the waitfor ones plus `paused`
//   end                 quit after this point

#include <SDL3/SDL.h>

#include <array>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace ke {

class InputScript {
public:
    struct Step {
        std::uint64_t atMs = 0;
        enum class Kind { Move, Down, Up, KeyDown, KeyUp, Shot, Script, Camera, WaitFor, Walk, End } kind = Kind::Move;
        float x = 0.0f;
        float y = 0.0f;
        int button = SDL_BUTTON_LEFT;
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        std::string name;
        std::array<float, 10> camera{}; // eye, forward, up, fovy; fovy <= 0 turns the override off
        std::uint64_t timeoutMs = 60000; // WaitFor
        int group = 0;                   // `if <condition> <step>`: the steps of one line, run only when the condition holds
        std::string onlyIf;
    };
    // WaitFor: true once the game is in the named state (the step's name, e.g. "entry 4").
    using Condition = std::function<bool(const std::string&)>;
    // Walk: called every frame the step is pending, with the step's whole line and whether this is the first
    // call. Returns 0 while pending, 1 when done, -1 when it failed.
    using WalkHook = std::function<int(const std::string&, bool)>;
    struct Camera {
        bool on = false;
        float eye[3]{};
        float forward[3]{};
        float up[3]{};
        float fovy = 55.0f;
    };

    // Reads and expands the file; false with error() on a bad line.
    bool load(const std::string& path, const std::string& outDir);
    const std::string& error() const { return error_; }
    bool active() const { return !steps_.empty(); }

    // Pushes every input step due by nowMs (script clock) and returns the frames to shoot after this
    // frame is drawn, as full paths. finished() turns true once the end step is reached.
    // A WaitFor step holds every later step until `condition` says yes or its timeout passes.
    std::vector<std::string> advance(std::uint64_t nowMs, SDL_Window* window, const Condition& condition = {},
                                     const WalkHook& walk = {});
    // True when the file has walk steps (the application turns the walk reporting on).
    bool hasWalkSteps() const;
    // A failed walk step ends the script unless this is off.
    void setAbortOnFail(bool abort) { abortOnFail_ = abort; }
    // Ends the script now (the walk gave up).
    void abort() { finished_ = true; }
    // The last WaitFor that ended: its condition, the time waited and whether it timed out.
    struct WaitResult {
        std::string condition;
        std::uint64_t waitedMs = 0;
        bool timedOut = false;
        bool fresh = false;
    };
    WaitResult takeWaitResult() { WaitResult out = waitResult_; waitResult_.fresh = false; return out; }
    bool finished() const { return finished_; }

    // The SDL keyboard state with the script's held keys added, for code that polls keys.
    const bool* keyboardState();

    const std::vector<Step>& steps() const { return steps_; }

    // Scripts (file paths) due since the last call, to run before the frame; and the camera override.
    std::vector<std::string> takeScripts() { std::vector<std::string> out; out.swap(scripts_); return out; }
    const Camera& camera() const { return camera_; }

private:
    std::vector<Step> steps_;
    std::size_t next_ = 0;
    std::string outDir_;
    std::string error_;
    bool finished_ = false;
    std::array<bool, SDL_SCANCODE_COUNT> held_{};
    std::array<bool, SDL_SCANCODE_COUNT> merged_{};
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    std::vector<std::string> scripts_;
    Camera camera_;
    std::uint64_t offsetMs_ = 0;   // added to every step's time: the time spent in WaitFor steps
    bool waiting_ = false;
    bool abortOnFail_ = true;
    int group_ = 0;        // the `if` line being run or skipped
    bool groupRun_ = true;
    std::uint64_t waitStartMs_ = 0;
    WaitResult waitResult_;
};

} // namespace ke
