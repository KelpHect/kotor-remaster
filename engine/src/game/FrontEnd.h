#pragma once

// The player-facing shipped screens outside the world: main menu, options and their
// sub-screens, load/save, movies, credits and the confirm popup. FrontEnd binds each
// shipped .gui to real state (user settings, the saves folder, movies.2da, the TLK)
// and turns control actions into outcomes the application acts on. It only talks to
// GuiSystem controls, so a screen is the shipped layout with real content.

#include "game/GameSession.h"
#include "game/GuiPaint.h"
#include "game/GuiSystem.h"
#include "game/UserSettings.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace ke {

// One row of the options screens' control -> help text table (retail's panel constructors).
// optfeedback's rows (retail 006e1300): the option's name and help strrefs, the settings key, and the
// bit of the options word (+0x14) that retail keeps it in.
struct FeedbackRow {
    std::uint32_t name;
    std::uint32_t help;
    const char* key;
    std::uint32_t bit;
    bool defaultOn;
};

struct OptionHelpRow {
    const char* screen;
    const char* tag;
    std::uint32_t strref;
};

struct FrontEndOutcome {
    bool newGame = false;
    bool quit = false;
    std::filesystem::path loadFolder;  // load this save folder
    std::filesystem::path saveFolder;  // write the current game here
    std::string saveName;
    std::string playMovie;
    bool resumeGame = false;           // leave the front end and return to play
    int displayWidth = 0;              // apply this window size (0: leave it)
    int displayHeight = 0;
    int fullscreen = -1;               // 1 borderless fullscreen, 0 windowed, -1 leave it
    bool any() const
    {
        return newGame || quit || !loadFolder.empty() || !saveFolder.empty() || !playMovie.empty()
            || resumeGame || displayWidth > 0 || fullscreen >= 0;
    }
};

class FrontEnd {
public:
    FrontEnd(GuiSystem& gui, GameSession& game, GuiPainter& painter, UserSettings& settings,
             std::filesystem::path saveRoot);

    // Once per frame: binds the content of a newly opened screen, applies live settings
    // (sliders), scrolls the credits.
    void update(float dt);
    // The retail control -> help strref table of the options screens (see FrontEnd.cpp).
    static std::span<const OptionHelpRow> optionHelpTable();
    static std::span<const FeedbackRow> feedbackTable();
    // A control action (tag). True when this class consumed it.
    bool action(const std::string& tag);
    // What the application must do now; cleared by the call.
    FrontEndOutcome takeOutcome();
    // The window as it is now and the desktop it sits on, for the resolution list.
    void setDisplay(int width, int height, bool fullscreen, int desktopWidth, int desktopHeight);

    struct SaveSlot {
        std::filesystem::path folder;
        std::string name;
        std::string area;
        std::string module;
        std::string pcName;
        std::int32_t playedSeconds = 0;
        std::string portrait;
        std::string portrait2; // PORTRAIT1 / PORTRAIT2: the other party members
        std::string portrait3;
        std::string screenshot; // savenfo SCREENSHOT: a texture resref (autosaves: "load_<module>")
        bool autosave = false;
        // The folder's number ("000007 - Game6" is 7): retail's slot id (+0x1c8), 0 quick, 1 auto.
        int id = -1;
    };
    // Real listing of the saves folder in directory order (ascending folder name), as retail's
    // CSWGuiSaveLoad::PopulateGameList (006cc160) takes it from GetDirectoryList.
    static std::vector<SaveSlot> listSaves(const std::filesystem::path& root);
    const std::vector<SaveSlot>& slots() const { return slots_; }
    // true: the save/load screen is saving (the in-game menu), false: loading.
    void setSaveMode(bool saving) { saveMode_ = saving; }
    void applyAudio();
    std::string screen() const { return bound_; }

private:
    void bind(const std::string& screen);
    void bindMainMenu();
    void bindOptions(const std::string& screen);
    void bindSaveLoad();
    void bindMovies();
    void bindCredits();
    void bindConfirm();
    void showSlot(int index);
    void refreshSlots();
    void pollSettings();
    void updateOptionHelp(const std::string& screen);
    void showOptionHelp(const std::string& screen, const std::string& tag);
    void bindFeedbackRows();
    void showFeedbackHelp(int row);
    int feedbackHelpRow_ = -1;
    void openConfirm(const std::string& text, const std::string& onOk);
    std::string tlk(std::uint32_t strref) const;
    std::string difficultyLabel() const;

    GuiSystem& gui_;
    GameSession& game_;
    GuiPainter& painter_;
    UserSettings& settings_;
    std::filesystem::path saveRoot_;
    std::string bound_;
    std::string helpScreen_;  // the screen the description list was last filled for
    std::string helpControl_; // the control whose help it holds
    // The options screens open in this run, oldest first, with the control whose help each list
    // holds: a screen revealed again keeps its text (W08).
    std::vector<std::pair<std::string, std::string>> helpStack_;
    std::vector<SaveSlot> slots_;
    bool saveMode_ = false;
    int shownSlot_ = -2;
    std::string thumbKey_;
    std::string confirmText_;
    std::string confirmOk_;
    std::vector<std::string> movieNames_;
    float creditsScroll_ = 0.0f; // design pixels
    float creditsHeight_ = 0.0f;
    float creditsViewport_ = 0.0f;
    FrontEndOutcome outcome_;
    struct DisplayMode {
        int width;
        int height;
    };
    int displayWidth_ = 0;
    int displayHeight_ = 0;
    bool displayFullscreen_ = false;
    int desktopWidth_ = 0;
    int desktopHeight_ = 0;
    std::vector<DisplayMode> resolutionRows_;
    // optkeymapping's filter tab (keymap.2da "page": 0 movement, 1 game, 2 mini games); the
    // screen opens on movement (006edc90 sets +0xf28 = 0 before 006ecfd0).
    int keymapPage_ = 0;
    bool mainMenuFocused_ = false; // New Game has taken the opening focus once
    struct SettingBinding {
        std::string tag;
        std::string key;
        bool slider;
    };
    std::vector<SettingBinding> bindings_;
};

} // namespace ke
