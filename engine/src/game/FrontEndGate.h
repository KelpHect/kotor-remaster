#pragma once

// --front-end-test: drives the shipped main menu, options, load/save and movies screens
// with real SDL pointer, wheel and key events (they go through the ImGui SDL backend and
// pumpGuiInput, the same path a player's mouse takes) and asserts on the GuiSystem state,
// the settings, the painter statistics and the presented frame.

#include "game/FrontEnd.h"
#include "game/GameScreens.h"

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace ke {

// What the loading screen showed while a module loaded (filled by the application's
// presentLoadScreen while the gate is running): the painter's statistics for it and the frame.
struct LoadScreenProbe {
    int shown = 0;
    std::string module;
    std::string image; // the LoadScreenID's picture
    std::string hint;
    GuiPainter::Stats stats;
    CapturedFrame frame;
};

class FrontEndGate {
public:
    // The gate's status summary check wants the pop-up up; everywhere else in the run it would wait for an answer.
    bool statusSummaryLive() const { return statusSummaryLive_; }
    struct Context {
        IRenderBackend* backend = nullptr;
        GuiSystem* gui = nullptr;
        FrontEnd* front = nullptr;
        GameScreens* screens = nullptr;
        GuiSystem* dialogueGui = nullptr; // the conversation panel's own screen
        GuiSystem* hudGui = nullptr;      // the HUD (mipc2<size>.gui: menu bar, action bar, party, minimap)
        const GuiPainter::Stats* hudStats = nullptr; // the HUD's own paint (mipc2)
        const GuiView* hudView = nullptr;
        GuiPainter* painter = nullptr;
        std::string hudTooltip; // what the HUD's pointer box says this frame
        std::string hudReticle; // the target reticle's art this frame (Hud::reticle), empty when none
        std::string hudBark;    // the bark bubble's line this frame (Hud::bark), empty when none
        UserSettings* settings = nullptr;
        GameSession* game = nullptr;
        bool menuStaged = false; // the 3D menu scene is the backend's preview scene
        LoadScreenProbe* loadProbe = nullptr;
    };

    FrontEndGate(std::string outDir, std::filesystem::path savesDir, SDL_Window* window,
                 std::string scenario = {});

    void step(Context& context);
    // The art probe: while it is set the application paints a bright grey field (kProbeField) and
    // each of these textures (as the painter draws them) in a grid; probeCell gives one cell's corner.
    static constexpr int kProbeField = 150;
    const std::vector<std::string>& artProbe() const { return artProbe_; }
    static void probeCell(std::size_t index, float windowWidth, float& x, float& y);
    static constexpr float kProbeCell = 136.0f;
    static constexpr float kProbeBox = 128.0f;
    bool finished() const { return index_ >= ops_.size(); }
    int exitCode() const { return ok_ ? 0 : 78; }
    // Where the gate's pointer last was. The window's backend also reads the real cursor
    // while the window has focus, which would move the pointer off the gate's target.
    bool pointer(float& x, float& y) const
    {
        x = pointerX_;
        y = pointerY_;
        return pointerSet_;
    }

private:
    struct Op {
        std::string label;
        std::function<bool(Context&)> run; // true when the op is complete
    };

    void build();
    void buildStore(); // --front-end-scenario store: the shipped store screen and the loading screen
    void buildMaps();  // --front-end-scenario maps: the HUD minimap over a sample of every planet's modules
    void check(bool condition, const std::string& label);
    bool controlPoint(Context& context, const std::string& tag, float fx, float fy, float& px, float& py);
    bool rowPoint(Context& context, const std::string& tag, int row, float& px, float& py);
    void pushMotion(float x, float y);
    void pushButton(float x, float y, bool down);
    void pushWheel(float x, float y, float dy);
    void pushKey(int scancode, unsigned key);

    Op hover(const std::string& tag, float fx = 0.5f, float fy = 0.5f);
    Op click(const std::string& tag, float fx = 0.5f, float fy = 0.5f);
    Op clickRow(const std::string& tag, int row);
    Op hoverRow(const std::string& tag, int row);
    Op clickLastRow(const std::string& tag);
    // Every help row of a screen (FrontEnd::optionHelpTable): the list holds text on open, and each
    // control's own string once the pointer is over it. Fails on a screen with no help.
    void optionHelpProof(const std::string& screen);
    Op wheel(const std::string& tag, float dy);
    Op key(int scancode, unsigned key);
    Op wait(int frames);
    Op expect(const std::string& label, std::function<bool(Context&)> test);
    // Retries `test` every frame, passing as soon as it holds and failing after maxFrames.
    Op waitUntil(const std::string& label, std::function<bool(Context&)> test, int maxFrames = 1800);
    Op capture(const std::string& name, int minColours, bool fullScreen = false);
    Op hitTest(const std::string& screen, std::size_t index);
    // Layout checks every resolution must pass, each with a negative proof (see build()).
    void layoutChecks(const std::string& screen);
    // The painter's audits for the frame just captured: logs every issue, returns how many are not
    // on the expected list.
    int auditFrame(const std::string& name, const GuiPainter::Stats& stats);
    void layoutProofs();
    // Widget art on the last capture: checkbox indicators, the +/- arrows, stretched fills.
    void widgetChecks(const std::string& screen);
    void widgetProofs();
    Op screenIs(const std::string& name);
    // Waits until the window (and so the painter's layout) is `accept`ed; fullscreen -1 ignores it.
    Op windowIs(const std::string& label, std::function<bool(int, int)> accept, int fullscreen = -1);
    Op setWindow(int width, int height, bool fullscreen);
    Op selectResolution(const std::string& text);

    GuiSystem& pointerGui(Context& context) const
    {
        if (pointToHud_ && context.hudGui != nullptr) {
            return *context.hudGui;
        }
        return pointToDialogue_ && context.dialogueGui != nullptr ? *context.dialogueGui : *context.gui;
    }

    std::string outDir_;
    std::string scenario_;
    std::filesystem::path savesDir_;
    SDL_Window* window_ = nullptr;
    std::vector<Op> ops_;
    std::size_t index_ = 0;
    int age_ = 0;
    bool ok_ = true;
    bool statusSummaryLive_ = false;
    std::uint64_t gateFrames_ = 0;
    int focusChanges_ = 0;
    bool focusSeen_ = false;
    bool focusWas_ = false;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    bool pointerSet_ = false;
    float px_ = 0.0f;
    float py_ = 0.0f;
    bool armed_ = false;
    bool pointToDialogue_ = false; // pointer ops target the conversation panel
    bool pointToHud_ = false;      // pointer ops target the HUD
    CapturedFrame worldFrame_;     // the world as it was just before the pause menu opened
    CapturedFrame lastFrame_;      // the last capture() and what the painter had drawn for it
    GuiPainter::Stats lastStats_;
    GuiView lastView_;
    std::vector<std::string> artProbe_;
};

} // namespace ke
