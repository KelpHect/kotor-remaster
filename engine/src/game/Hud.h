#pragma once

// The in-world HUD: the shipped PC interface (mipc2<size>.gui, the family the retail HUD
// constructor 0068c100 opens: mipc28x6, mipc210x7, mipc212x9, mipc216x12) bound to the live
// session. It carries the menu bar (equipment ... options, the minimap button), the pause,
// solo and stealth toggles, the party portraits with their vitality and force bars, the
// action bar with its up/down arrows, the action queue with its Clear buttons, and the
// target block. Controls the shipped file keeps for events that have not happened (level-up
// badges, item received, combat effects) stay hidden.

#include "game/GameScreens.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ke {

class Hud {
public:
    // What a control asks the application for, beyond what the session can do itself.
    enum class Request { None, Pause, Map, TogglePause };

    // One entry of an action slot: what BTN_ACTIONn would do and how it is drawn.
    struct Option {
        enum class Kind { Attack, Mode, Power, Item, Bash, Unlock };
        Kind kind = Kind::Attack;
        std::int32_t id = 0;        // feat id or spells.2da row
        std::uint32_t item = 0;     // script object of the item
        std::string name;
        std::string icon;
        std::string detail;         // cost line under the name
        bool usable = true;
    };

    Hud(GuiSystem& gui, GameSession& game, GameScreens* screens);

    // The shipped layout for a window (always the 800x600 file, see the definition); `designWidth`
    // and `designHeight` return its size.
    static std::string layoutFor(float windowWidth, float windowHeight, float& designWidth, float& designHeight);
    // The HUD's scale at a window size: one at retail's own sizes, else max(1, height / 720).
    static float layoutScale(float windowWidth, float windowHeight);

    // Binds the screen to the session; once per frame before painting.
    void update(double timeSeconds, bool userPaused);
    // A clicked control (tag). Returns what the application must do, if anything.
    Request activate(const std::string& tag);
    // Retail 0040b490 with 00418a90's text: what the control under the pointer shows, empty for none or with bit 0x400 off.
    std::string tooltip() const;
    // Portrait texture resref drawn over a control (LBL_CHAR1..3), empty when none.
    std::string portraitFor(const std::string& tag) const;

    // What the HUD shows, for the tests.
    int memberCount() const { return members_; }
    bool targetShown() const { return targetShown_; }
    // keymap.2da row 225 ToolTips (T): while held retail sets the tooltip delay to 0 and hides the
    // action description line (005f4be0 -> 006856e0); on release it restores both (00685710).
    void setInstantToolTips(bool held) { instantToolTips_ = held; }
    bool instantToolTips() const { return instantToolTips_; }
    const std::vector<Option>& options(int slot) const { return options_[static_cast<std::size_t>(slot)]; }
    int optionIndex(int slot) const { return index_[static_cast<std::size_t>(slot)]; }
    const std::vector<Option>& targetOptions(int slot = 0) const { return targetOptions_[static_cast<std::size_t>(slot)]; }
    // The window the HUD is drawn in, its scale and whether it is edge-anchored (GuiView), so the
    // target block can be placed on the target. Call before update().
    void setView(float windowWidth, float windowHeight, float scale, bool anchored)
    {
        viewWidth_ = windowWidth;
        viewHeight_ = windowHeight;
        viewScale_ = scale;
        viewAnchored_ = anchored;
    }
    // The control that stands for an action slot / target slot, for the pointer gates.
    static std::string actionTag(int slot) { return "BTN_ACTION" + std::to_string(slot); }
    // Arrow of the minimap in radians the leader faces (0 = up).
    float mapArrowAngle() const;
    // The target reticle (CSWGuiMainInterface 0068a310, its code-built control at +0x5a2c): the art,
    // the square's top left and edge in HUD design units, and its turn in degrees (+0x5aa4).
    struct Reticle {
        bool visible = false;
        std::string art;
        float x = 0.0f;
        float y = 0.0f;
        float size = 0.0f;
        float angleDegrees = 0.0f;
    };
    const Reticle& reticle() const { return reticle_; }

    // The bark bubble (barkbubble.gui, CSWGuiBarkBubble 006a9770): BarkString's line in a box across
    // the top of the screen. Runs whether or not the HUD itself is up (the turret minigame barks).
    struct Bark {
        bool visible = false;
        std::string text;
    };
    void updateBark(double timeSeconds);
    const Bark& bark() const { return bark_; }

private:
    void hideEventControls();
    void rebuildOptions();
    void bindActionSlots();
    // The file's rects of the action description controls (SetMessageText 00685560 refits them).
    bool descFitKnown_ = false;
    const void* descScreen_ = nullptr;
    kotor::GuiExtent descBackground_;
    kotor::GuiExtent descLabel_;
    void bindTarget(double timeSeconds);
    void bindReticle(float dt);
    const Option* current(int slot) const;
    const Option* currentTarget(int slot) const;
    std::string itemIcon(const InventoryItem& item) const;

    GuiSystem& gui_;
    GameSession& game_;
    GameScreens* screens_ = nullptr;
    std::map<std::string, std::string> portraits_;
    std::array<std::vector<Option>, 4> options_;
    std::array<int, 4> index_{0, 0, 0, 0};
    // The target's three slots (00619c20): Attack and the combat modes, hostile powers, grenades.
    std::array<std::vector<Option>, 3> targetOptions_;
    std::array<int, 3> targetIndex_{0, 0, 0};
    // The target block's last place in design units (00686090 keeps it while the target is not projected).
    float blockX_ = 0.0f;
    float blockY_ = 0.0f;
    bool blockPlaced_ = false;
    bool pointerOnBlock_ = false; // retail +0x1aec bit 2 (hit test 00285f..: the pointer is over the block)
    float blockHold_ = 0.0f;      // retail +0x1ae0: 0.5 s after the pointer leaves, the block stays put
    double blockTime_ = -1.0;
    float viewWidth_ = 0.0f;
    float viewHeight_ = 0.0f;
    float viewScale_ = 0.0f;
    bool viewAnchored_ = false;
    double lastRebuild_ = -1.0e9;
    bool tablesLoaded_ = false;
    bool instantToolTips_ = false;
    // The combat line (00687700 / 00686ba0): the shown strref, the transient's seconds left (-1 none).
    std::uint32_t combatStrref_ = 48208;
    float combatTransient_ = -1.0f;
    double lastUpdate_ = -1.0;
    std::optional<kotor::TwoDa> feats_;
    std::optional<kotor::TwoDa> spells_;
    std::optional<kotor::TwoDa> baseItems_;
    std::optional<kotor::TwoDa> skills_;
    std::uint32_t optionsTarget_ = UINT32_MAX; // the entity the target slots were built for
    int members_ = 0;
    bool targetShown_ = false;
    Reticle reticle_;
    float reticlePulse_ = -1.0f; // retail +0x5cb0: the combat reticle's 0.5 s shrink, -1 before it starts
    std::uint32_t reticleTarget_ = UINT32_MAX;
    Bark bark_;
    bool barkActive_ = false;
    std::uint32_t barkSerial_ = 0;
    std::uint32_t barkSpeaker_ = 0x7F000000u;
    float barkLeft_ = 0.0f;  // retail +0x1b4
    float barkSound_ = 0.0f; // the line's sound still playing (retail asks the source, +0x1b8)
    bool barkSoundPlayed_ = false; // its Play succeeded (+0x1c4): when it ends the bubble goes
    double barkClock_ = -1.0;
};

} // namespace ke
