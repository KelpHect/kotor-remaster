#pragma once

// --state-trace <file>: the engine's side of the shared state trace (format and tolerances: TRACE_SPEC.md in the
// scratchpad's trace folder, record by record notes: ENGINE_NOTES.md there). The retail recorder writes the same
// TSV from the original game; compare.py reads both.
//
// The writer only READS game state (GameSession is a friend for that and nothing else): it never calls a
// non-const game function that changes anything. The flag costs nothing when absent: the writer is not
// created, GameSession::stateTrace_ stays null (one pointer test at three sites) and the mixer has no observer.
// The file holds numbers, resrefs, tags and strrefs only: no dialogue text, no asset bytes.

#include "audio/Audio.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ke {

class GameSession;
class GuiSystem;
namespace kotor {
struct GuiScreen;
}

// What Application hands in once per presented frame (after the frame was drawn).
struct StateTraceFrame {
    GameSession* game = nullptr;
    const GuiSystem* menuGui = nullptr;     // the menu / game screens' GuiSystem (state.guiSystem)
    const GuiSystem* dialogueGui = nullptr; // the conversation panel's GuiSystem
    const GuiSystem* hudGui = nullptr;      // the HUD's GuiSystem (its screen lists the HUD controls)
    const GuiSystem* pauseGui = nullptr;    // pause.gui while the pause panel is shown (retail lists it as `pause`)
    std::string hudLayout;                  // the HUD layout drawn this frame; empty when no HUD was drawn
    bool userPause = false;
    bool uiPause = false;                   // pause menu or a game screen over the world
    int windowWidth = 0;
    int windowHeight = 0;
};

class StateTrace {
public:
    StateTrace();
    ~StateTrace();
    StateTrace(const StateTrace&) = delete;
    StateTrace& operator=(const StateTrace&) = delete;

    // Opens `file` and writes the header. `samp`: state records are sampled every samp-th presented frame.
    bool open(const std::filesystem::path& file, int samp, std::string& error);
    bool isOpen() const { return open_; }

    // Hooks the session (module, dialogue, script actions) and its mixer (sounds). `menuBackdrop`: the session's
    // first module is only the main menu's backdrop; retail has no module at that point, so it is not reported.
    void attach(GameSession& game, bool menuBackdrop);
    void detach();

    // Once per presented frame, after the draw.
    void step(const StateTraceFrame& frame);
    // Screenshot files due this frame, taken after step(); the caller writes them (BMP).
    std::vector<std::filesystem::path> takeShots();
    // A frame the input script took (--input-script shot): written as a `shot` row named like the retail driver's
    // (`settle_vis`, `walk`), so the pair lines up by the state at that frame. No file is taken here.
    void noteShot(const std::string& name, const std::string& file);

    // ---- hooks called by the session (all on the game thread) ----
    void onModuleReady(GameSession& game);
    void onDialogueLine(GameSession& game, std::uint32_t entry);
    // An entry with no text that the conversation passes through: retail's client still receives it
    // (CSWSDialog::SendDialogEntryNode 005a13d0 -> CGuiInGame::HandleDialogEntry 00631d80), so it is a row too.
    void onDialogueControlEntry(GameSession& game, std::uint32_t entry, std::int32_t cameraAngle);
    void onAction(std::uint16_t actionId);
    void onRound(GameSession& game, std::int32_t number, std::uint32_t owner, bool ended = false);
    // The loading screen the module hook presents: reported as a panel that opens now and closes at `mod ready`.
    void onLoadScreen(const std::string& name, int x, int y, int w, int h);

private:
    struct Panel {
        std::string name;
        int mode = 0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        const kotor::GuiScreen* screen = nullptr;
    };
    // fade.gui, parsed once for the fade panel's control row.
    std::unique_ptr<kotor::GuiScreen> fadeScreen_;
    bool fadeScreenTried_ = false;
    struct Pending {
        std::uint64_t due = 0;
        bool control = false; // true: write `lines`; false: a shot named `name`
        std::string name;
        std::vector<std::string> lines;
    };
    struct Voice {
        int id = 0;
        std::vector<std::string> tail; // the columns after `start|stop`
        // A looped 3D sample (retail CExoSoundSourceInternal, not a streaming source) that is farther from the
        // listener than its max distance is not played: it waits in the non-playing looping list and
        // CExoSoundInternal::Render (005d9590) calls its Play (005db4d0) again on every frame.
        bool loopedSample3d = false;
        bool waiting = false;
    };

    void write(const char* kind, const std::vector<std::string>& fields);
    void state(const char* kind, const std::string& key, const std::vector<std::string>& fields);
    void flushNow();
    void resetWorld();
    void trackGui(const StateTraceFrame& frame);
    void trackSounds();
    void onPlay(const audio::Mixer::PlayInfo& info);
    void writeSound(const std::vector<std::string>& fields);
    void sampleState(const StateTraceFrame& frame);
    void trackDialogueCamera(GameSession& game);
    void schedule(std::uint64_t afterFrames, const std::string& shotName);
    std::string keyOf(std::size_t entity) const;
    std::string keyFor(GameSession& game, std::size_t index, std::uint32_t& idOut);
    std::string keyOfObject(GameSession& game, std::uint32_t object);

    bool open_ = false;
    std::ofstream out_;
    std::filesystem::path shotDir_;
    std::recursive_mutex mutex_;
    int samp_ = 3;
    std::uint64_t frame_ = 0;
    std::uint64_t startTicks_ = 0;
    std::uint64_t now_ = 0;
    GameSession* game_ = nullptr;

    std::unordered_map<std::string, std::string> last_; // class + key -> the last written payload
    std::unordered_map<std::uint32_t, std::string> keyById_;
    std::unordered_map<std::string, int> seen_;          // tag|template -> objects met
    std::vector<std::string> entityKey_;                 // entity index -> key, this sample
    std::unordered_map<std::string, std::string> objectVisible_; // object id -> the visible column outside a minigame
    std::unordered_map<std::string, std::string> objects_; // id text -> key, to write obj_del
    std::vector<Panel> panels_;
    std::string reported_;                               // the module `mod ready` last named
    bool hudSeen_ = false;
    bool backdrop_ = false; // the session only stands behind the main menu: its world is not reported
    bool inPlay_ = false;
    std::uint32_t dlgCount_ = 0;
    bool dlgcamDirty_ = false;
    std::string dlgcamLast_;
    std::string camMode_;
    std::string dlgResref_;
    std::string loadScreen_;
    std::vector<Pending> pending_;
    std::vector<std::filesystem::path> shotsOut_;
    std::uint32_t shotSeq_ = 0;
    std::vector<Voice> voices_;
    std::unordered_map<std::string, std::uint64_t> soundWritten_; // snd dedupe key -> SDL ticks of the last write
    bool attached_ = false;
    double simBase_ = 0.0; // GameSession::elapsed_ when the module became ready
};

} // namespace ke
