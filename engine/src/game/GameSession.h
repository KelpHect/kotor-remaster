#pragma once

#include "game/AudioSystem.h"
#include "game/DialogueShots.h"
#include "game/Effects.h"
#include "game/FacialRig.h"
#include "game/Factions.h"
#include "game/MiniGame3D.h"
#include "game/Minigames.h"
#include "game/Party.h"
#include "game/Rules.h"
#include "game/VideoEffectController.h"
#include "game/ScriptHost.h"
#include "kotor/BinkDecoder.h"
#include "kotor/Localization.h"
#include "kotor/Media.h"
#include "kotor/Formats.h"
#include "kotor/SaveGame.h"
#include "render/SceneView.h"
#include "script/ActionRuntime.h"
#include "script/World.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ke {

struct InputState {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool run = false;
    bool jump = false;
    float mouseDx = 0.0f;
    float mouseDy = 0.0f;
};

struct DialogueOption {
    std::string text;
    std::uint32_t reply = UINT32_MAX;
};

struct DialogueView {
    bool active = false;
    // The conversation's type is Computer (DLG ConversationType 1, retail reader 005a2ae0): the
    // terminal screen (computer.gui) shows it instead of the letterboxed dialog panel.
    bool computer = false;
    std::string npcTag;
    std::string speaker;
    // The object the current line is spoken by (DLG Speaker tag, else the
    // conversation owner) and addressed to (Listener tag, else the PC).
    std::uint32_t speakerObject = script::kObjectInvalid;
    std::uint32_t listenerObject = script::kObjectInvalid;
    std::uint32_t delay = 0xFFFFFFFFu;
    std::uint32_t waitFlags = 0;
    std::string text;
    std::vector<DialogueOption> options;
    std::uint32_t entry = UINT32_MAX;
    std::int32_t cameraId = -1;
    std::int32_t cameraAngle = -1;
    // A line whose replies are on screen is shot on the one who answers (see showDialogueEntry).
    bool shotOnListener = false;
    std::int32_t replyCameraAngle = 0; // the angle the reply list is shot from, once it is up
    std::int32_t camVidEffect = 0;
    float camHeightOffset = 0.0f;
    float tarHeightOffset = 0.0f;
    float camFieldOfView = -1.0f; // the node's CamFieldOfView, -1 for the scene's view angle
};

// One floating line over an object (retail 00688e10, kept per object by 0068b7c0).
struct FloatingText {
    std::uint32_t object = 0;
    std::string text;
    kotor::Vec3 color{1.0f, 1.0f, 1.0f};
    float seconds = 1.5f;
    float age = 0.0f;
    int line = 0; // lines above the newest one
};

// What retail 0062eeb0 gathers until the world is free (0062ef90): the status summary's rows.
struct StatusSummary {
    bool journal = false;
    bool creditsBothWays = false; // retail bit 0x100
    std::int32_t credits = 0;
    std::int32_t xp = 0;
    std::int32_t stealthXp = 0;
    std::int32_t light = 0;
    std::int32_t dark = 0;
    bool received = false;
    bool lost = false;
    bool any() const
    {
        return journal || credits != 0 || creditsBothWays || xp != 0 || stealthXp != 0 || light != 0 || dark != 0 || received || lost;
    }
};

// The explored bits of an area's map (retail 00578c60: MapResX columns, at most 88, 0 meaning 88; rows in the 440x256
// view's proportion; 005061d0 keeps them in the area's AreaMap).
struct MapFog {
    int cols = 0;
    int rows = 0;
    std::vector<char> seen;
    // The saved words as retail holds them (00578fc0 copies them whole; 00578fa0 sets every byte
    // to 0xff), padding bits past the last cell included; written back under the seen cells.
    std::vector<std::uint32_t> words;
    std::size_t seenCount() const { return static_cast<std::size_t>(std::count(seen.begin(), seen.end(), char{1})); }
};

struct DialogueChoiceEvent {
    std::string module;
    std::string resref;
    std::uint32_t entry = UINT32_MAX;
    std::size_t option = 0;
    std::string text;
};

struct ModuleTransitionEvent {
    std::string from;
    std::string to;
};

enum class UiPanel { None, Character, Inventory, Party, Journal, Merchant, Loot, Equipment, Abilities, Messages, Map, LevelUp };
const char* uiPanelName(UiPanel panel);

enum class CombatMode { Normal, PowerAttack, Flurry, CriticalStrike, Whirlwind };
const char* combatModeName(CombatMode mode);

// One loaded module plus the player state. All KotOR data comes through the
// C++ kotor::Install/VFS; this class never calls an external tool. Scripts run
// on the engine's own NCS VM and mutate the same world the renderer uses.
namespace nav {
class NavMesh;
struct Runtime;
} // namespace nav
namespace quest {
struct QuestTraceOptions;
int runQuestTrace(GameSession& game, const QuestTraceOptions& options);
struct FlowOptions;
int runConversationFlow(GameSession& game, const FlowOptions& options);
int runPlotReplay(GameSession& game, const FlowOptions& options);
} // namespace quest

class StateTrace; // --state-trace writer (game/StateTrace.h): reads session state, never changes it

class GameSession : public script::ActionHost {
    friend class StateTrace;
    friend struct ScriptApi;
    friend struct ItemOps;
    friend struct CombatOps;
    friend class EffectDirector;
    friend struct QuestTraceOps;
    friend struct QuestFlowOps;

public:
    GameSession(std::string installPath, std::string module, int language = -1,
                bool traceActions = false);
    ~GameSession() override;

    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }
    const std::string& module() const { return module_; }
    const std::string& installPath() const { return installPath_; }
    kotor::Install& install() { return install_; }
    const std::string& buildSha256() const { return install_.buildSha256(); }
    const Rules& rules() const { return rules_; }
    const kotor::Localization& localization() const { return *localization_; }
    std::uint32_t languageId() const { return languageId_; }
    AudioSystem& audio() { return *audio_; }
    const AudioSystem& audio() const { return *audio_; }
    FacialRig& facialRig() { return facialRig_; }
    const FacialRig& facialRig() const { return facialRig_; }
    FactionSystem& factions() { return factions_; }
    const FactionSystem& factions() const { return factions_; }

    int reputation(int factionA, int factionB) const {
        return factions_.reputation(factionA, factionB);
    }
    bool setReputation(int factionA, int factionB, int value) {
        const bool changed = factions_.setReputation(factionA, factionB, value);
        refreshHostility();
        return changed;
    }
    bool adjustReputation(int factionA, int factionB, int delta) {
        const bool changed = factions_.adjustReputation(factionA, factionB, delta);
        refreshHostility();
        return changed;
    }
    int factionOfObject(std::uint32_t object) const;
    void setRsvRootForTest(std::filesystem::path root) { rsvRoot_ = std::move(root); }

    void update(float dt, const InputState& input);
    // A frame the world does not tick (pause, a screen over the world): retail's server loop
    // (004babb0) still runs TempDeathUpdate (004b6da0) on the real clock.
    void tickWhilePaused(float realDt);
    void runPendingModule();
    SceneCamera camera() const;
    SceneCamera cinematicCamera(std::int32_t cameraId) const;
    SceneCamera presetCamera(std::int32_t angle, float camHeight, float targetHeight,
                             kotor::Vec3 speaker, kotor::Vec3 target, std::int32_t side = 1) const;
    void setDialogueCameraForTest(std::int32_t cameraId) {
        dialogueView_.active = true;
        dialogueView_.cameraId = cameraId;
    }
    bool setDialogueCameraAnimationForTest(const std::string& model, const std::string& clip);
    std::string interactionPrompt() const;
    bool interact();
    // Retail 00620350 HandleMouseClickInWorld: ndcX and ndcY are the pointer in -1..1 (y up), aspect the view
    // width over height. False when no selectable object is under the pointer.
    bool clickWorld(float ndcX, float ndcY, float aspect);
    std::size_t pickEntity(kotor::Vec3 eye, kotor::Vec3 dir) const;
    bool performDefaultAction(std::size_t index);
    bool activateSelected();
    bool testTransition(std::string& from, std::string& to);
    // Queues the module the next update() enters, through the same path as the
    // StartNewModule action (new game: chargen hands the character here).
    void requestModule(const std::string& module) { pendingModule_ = module; }
    // New Game: the module the menu stood on is a backdrop, not a place the player has been.
    // Retail's new game starts from an empty server world, so the next update() enters `module`
    // fresh (its spawn and OnEnter scripts run) with no globals, journal or module snapshots
    // carried over, and without the backdrop's OnExit/OnClientLeave hooks.
    void startNewGame(const std::string& module);
    // Called with the destination module just before update() loads it: the application
    // shows the loading screen there (the load itself blocks the frame loop).
    void setModuleLoadHook(std::function<void(const std::string&)> hook) { moduleLoadHook_ = std::move(hook); }
    // --state-trace: null (the default) costs one pointer test at each of the three hook sites.
    void setStateTrace(StateTrace* trace) { stateTrace_ = trace; }
    // A combat round started for `owner` (the attack-round entry of the combat system).
    void traceRound(std::int32_t number, std::uint32_t owner, bool ended = false);
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);
    // Original KotOR save folder (SAVEGAME.sav + sidecars). The reader maps
    // the saved player/party/inventory/globals and overlays the saved GIT
    // object state; the writer emits a complete original slot.
    // asPlayer: play the loaded game as the PC (avatar and follow camera) before the module start
    // scripts run, so a conversation they start finds the PLAYER (WALK_DIFFS W29/W38).
    bool loadOriginalSave(const std::filesystem::path& folder, bool asPlayer = false);
    bool saveOriginalSave(const std::filesystem::path& folder,
                          const std::string& saveName = "Engine Save");
    // The frame the next original save stores as Screen.tga (taken before the pause menu opened).
    void setSaveScreenshot(std::vector<std::uint8_t> tga) { saveScreenTga_ = std::move(tga); }
    const kotor::GameProfile& gameProfile() const { return install_.profile(); }

    kotor::Vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float elapsedSeconds() const { return elapsed_; }
    bool grounded() const { return grounded_; }
    float floorHeightAt(float x, float y) const;
    // The walkmesh height below (x, y) under maxZ; false where there is no walkmesh.
    bool floorBelow(float x, float y, float maxZ, float& height) const;

    bool startDialogue(std::size_t entityIndex);
    bool startDialogue(const std::string& tag);
    void setDialogueForTest(kotor::Dlg dlg, std::string resref, std::string speaker);
    bool lipSyncActive() const { return activeLip_.has_value(); }
    float lipTime() const { return lipTime_; }
    std::uint8_t lipViseme() const;
    void setLipSyncForTest(kotor::LipSync lip) {
        activeLip_ = std::move(lip);
        lipTime_ = 0.0f;
        dialogueView_.active = true;
    }
    void clearLipSyncForTest() {
        activeLip_.reset();
        lipTime_ = 0.0f;
        dialogueView_.active = false;
    }
    // PlayMovie plays at once (skippable: its handler 00540ed0 carries no flag). The queue
    // follows QueueMovie(name, bSkippable) / PlayMovieQueue(bAllowSeparateSkips): queuing
    // plays nothing; Escape (skipMovie) ends a skippable movie, and with
    // bAllowSeparateSkips FALSE ends the whole queue.
    bool playMovie(const std::string& name);
    void stopMovie();
    bool queueMovie(const std::string& name, bool skippable = true);
    bool playMovieQueue(bool allowSeparateSkips = true);
    bool skipMovie();
    // The movie clock, audio cursor and queue; update() calls it.
    void advanceMovie(float dt);
    // Retail runs a movie inside a blocking loop (00602af0): the world and its scripts wait.
    // The frontend turns this on; a test driving update() directly keeps its clock.
    void setMovieHoldsWorld(bool hold) { movieHoldsWorld_ = hold; }
    bool movieHoldsWorld() const { return movieHoldsWorld_; }
    std::size_t queuedMovies() const { return movieQueue_.size(); }
    bool movieSkippable() const { return movieSkippable_; }
    // Retail BeginIntro (00602dc0): leclogo, biologo, legal, each skippable on its own.
    bool playStartupMovies();
    bool moviePlaying() const { return moviePlaying_; }
    const std::string& movieName() const { return movieName_; }
    float movieTime() const { return movieTime_; }
    float movieDuration() const { return movieDuration_; }
    int movieAudioVoice() const { return movieAudioVoice_; }
    const kotor::BinkVideoFrame* movieFrame() const {
        return movieFrame_.rgba.empty() ? nullptr : &movieFrame_;
    }
    int movieFrameIndex() const { return movieFrameIndex_; }
    bool seekMovie(float seconds);
    // PlayPazaak (action 364) opens the shipped screens in order: pazaakwager (when a wager is asked),
    // pazaaksetup (the side deck), pazaakgame. The screens (GameScreensPazaak.cpp) only show this model.
    // Retail: StartPazaakGame 005f3810, the start panel 00681a90, EndPazaakGame 005f3950.
    enum class PazaakStage { None, Wager, Setup, Game };
    bool startPazaak(std::int32_t opponentDeck, std::string endScript,
                     std::int32_t maxWager, bool tutorial, std::uint32_t opponent);
    PazaakGame* pazaak() { return pazaak_.get(); }
    const PazaakGame* pazaak() const { return pazaak_.get(); }
    PazaakStage pazaakStage() const { return pazaakStage_; }
    bool pazaakActive() const { return pazaakStage_ != PazaakStage::None; }
    int lastPazaakResult() const { return lastPazaakResult_; }
    std::uint32_t pazaakOpponent() const { return pazaakOpponent_; }
    // LBL_NPCNAME: the opponent creature's name when there is one (the game panel 006808a0).
    std::string pazaakOpponentName() const;
    bool pazaakTutorial() const { return pazaakTutorial_; }
    // The side deck panel's help box (only with bShowTutorial).
    PazaakHelp& pazaakSetupHelp() { return pazaakSetupHelp_; }
    const PazaakHelp& pazaakSetupHelp() const { return pazaakSetupHelp_; }
    // The match is over and its outcome has been shown: EndPazaakGame with bWon = more sets.
    void resolvePazaakIfOver();
    // Escape and OK on the quit question: before the table (0067d3d0) nothing is wagered; at the table
    // (0067db50) the match is forfeit and the wager lost. Both end the game with a result of 0.
    void quitPazaak();
    void forfeitPazaak();
    // EndPazaakGame 005f3950: keeps the result, settles the wager, runs the end script with an invalid
    // OBJECT_SELF, every time the game ends.
    void endPazaakGame(bool won, std::int32_t wager);
    void stopPazaak();
    // The wager popup (0067f000, 0067e150): it opens at the maximum (the script's, cut to the gold
    // held) and steps by one, never under 1.
    int pazaakMaxWager() const;
    int pazaakWager() const { return pazaakWagerChosen_; }
    int pazaakWagerStep() const { return 1; }
    void setPazaakWager(int wager);
    void confirmPazaakWager();
    // The setup screen: 18 card kinds (0..5 are +1..+6, 6..11 are -1..-6, 12..17 are +/-1..+/-6). The
    // cards owned are PARTYTABLE counts (ClearTable 00563200 starts a game with +1..+5 twice each); the
    // side deck is exactly ten (ShowDisabledButtons 0067e400) and is kept in PARTYTABLE (00681890).
    static constexpr std::size_t kPazaakDeckSize = 10;
    int pazaakCardsOwned(int kind) const;
    const std::vector<int>& pazaakChosen() const { return pazaakChosen_; }
    bool pazaakChoose(int kind);
    bool pazaakUnchoose(std::size_t slot);
    bool pazaakPlay();
    static PazaakCard pazaakCardOfKind(int kind);
    std::int32_t pazaakWagerAtStake() const { return pazaakWager_; }
    // CItemRepository::AddItem 0055d330: a pazaak card item (base item 86) given to the party becomes a
    // PARTYTABLE count (AddPazaakCard 004efb40, kind = (ModelVariation + 11) % 18) and the first one
    // brings the sideboard item. True when the item was taken that way.
    bool absorbPazaakCard(const std::string& resref, std::int32_t baseItem, std::int32_t stack);
    // Shipped minigame modules. Entering one is a normal module load; these are the
    // player-level input and the state a window or a bot reads (game/MiniGame3D.h).
    bool miniGameActive() const;
    bool enterMiniGameModule(const std::string& module) { return loadModule(module, nullptr, {}); }
    void miniGameSetInput(const MiniGameInput& input);
    MiniGameView miniGameView() const;
    MiniGameWorld* miniGame();
    const MiniGameWorld* miniGame() const;
    void setMiniGameSeed(std::uint32_t seed);
    void setMiniGameMeshesHidden(bool hidden);
    // Writes the minigame's poses into the scene's meshes before a frame is drawn.
    void bakeMiniGame();
    // Drops the conversation without firing its end scripts (test fixtures).
    void clearDialogueForTest() { clearDialogueState(); }
    void chooseDialogueOption(std::size_t optionIndex);
    void endDialogue();
    const DialogueView& dialogue() const { return dialogueView_; }
    // Cutscene-style playback of a conversation: when on, an entry that only continues (one
    // link without a player line, or no link) advances by itself once its voice-over has
    // finished (or, with no voice, after its Delay or a reading time). The application turns
    // it on; fixtures leave it off and advance by choosing.
    void setDialogueAutoAdvance(bool enabled) { dialogueAutoAdvance_ = enabled; }
    bool dialogueAutoAdvance() const { return dialogueAutoAdvance_; }
    // Seconds the current entry has been shown.
    float dialogueEntryAge() const { return dialogueEntryAge_; }
    std::size_t dialogueStuntCount() const { return dialogueStunts_.size(); }
    // True while an ActionPauseConversation holds the conversation; options are
    // refused until a resume runs or nothing is left in flight.
    bool dialoguePaused() const { return sim_->dialoguePaused; }
    // The ending (ScriptWorldState.cpp). gameEnded: EndGame finished and the world is
    // torn down; the UI goes back to the main menu.
    bool gameEnded() const;
    void finishGameEnding();
    // One action with `self` as the caller, as a family gate needs it (ScriptWorldState.cpp).
    std::optional<std::vector<script::Value>> actionAsForTest(
        std::uint32_t self, std::uint16_t id, const std::vector<std::vector<script::Value>>& args);
    // Level-up points waiting to be spent. Levels themselves arrive with the XP.
    bool canLevelUp() const {
        return character_.attributePoints > 0 || character_.skillPoints > 0 || character_.featPoints > 0
            || character_.powerPoints > 0;
    }
    void addPlayerClass(std::int32_t classId);
    AudioSystem* audioOrNull() { return audio_.get(); }
    bool setDialoguePlaceableCamera(std::int32_t cameraId);
    bool gameOverPanelVisible() const;
    bool creditsActive() const;
    std::int32_t creditsPageStrref() const; // -1 when no credits are showing.
    float creditsOpacity() const;
    // The last credits page scrolls instead of fading: 0 at its start, 1 when it has gone.
    bool creditsOnLastPage() const;
    float creditsScrollProgress() const;
    // The front end took the finished ending (it went back to the main menu).
    void acknowledgeGameEnd();
    bool creditsTransparent() const;
    void skipCredits();
    // The tutorial popup waiting to be shown, as a tutorial.2da row, or -1; dismiss clears it. A
    // script's ShowTutorialWindow(0..2) maps to rows 9, 40 and 42 (retail 00547340, nothing for
    // other numbers); the game raises the rest by event (showTutorial), once per row.
    std::int32_t pendingTutorialWindow() const;
    void dismissTutorialWindow();
    void showTutorial(std::int32_t row);
    // The whole party is down: the player's character and every companion. The game over message
    // follows at once (retail 004b6da0 -> 005f7200 RunDeathSequence) and the game ends after the
    // 12 s hold and 1 s fade (005f7330), or at OK.
    bool partyWiped() const;
    bool gameOverMessage() const { return gameOverShown_; }
    // OK on the game over message: the world ends and the UI returns to the main menu, where
    // Load Game is one click away (the same end EndGame gives, retail 00602eb0).
    void acknowledgeGameOver();
    void killPartyForTest();

    // ---- the upgrade bench (UpgradeBench.cpp) ----
    struct UpgradeSlot {
        int type = 0;           // upgrade.2da upgradetype: 0 saber crystals, 1..3 melee, 4..7 ranged, 8..9 armor
        std::vector<int> rows;  // the upgrade.2da rows this item takes in the slot
    };
    struct UpgradeRow {
        std::string label;
        std::string templ;      // the upgrade item's template (lower case)
        int type = 0;
    };
    const std::vector<UpgradeRow>& upgradeRows();
    int upgradeRowOf(const std::string& resref); // -1 when the template is not an upgrade item
    std::vector<UpgradeSlot> upgradeSlots(const InventoryItem& item);
    // 0 lightsaber, 1 ranged, 2 melee, 3 armor, -1 when the item takes no upgrades.
    int upgradeCategory(const InventoryItem& item);
    int upgradeInstalled(const InventoryItem& item, int slotType); // the row installed in the slot, or -1
    // Installs `rowPerType[slotType]` (-1 empties the slot) into the item: the new upgrade item is
    // spent and the replaced one returns to the pack. True when anything changed.
    bool assembleUpgrades(std::size_t inventoryIndex, const std::map<int, int>& rowPerType);
    bool upgradeScreenOpen() const;           // ShowUpgradeScreen is waiting for the player
    std::uint32_t upgradeTarget() const;      // its item argument (invalid for the bench)
    void closeUpgradeScreen();
    void openUpgradeScreenForTest(std::uint32_t item = 0x7f000000);
    const kotor::ItemTemplate* upgradeTemplate(const std::string& resref);
    // Pushes the pack's rows into their item objects (what a row changed directly must do to stay).
    void syncInventoryObjects();
    // The gates turn the popups off except where they test them, so a first-time tutorial does not
    // cover a screen under test; on also forgets which rows were shown.
    void setTutorialWindowsEnabledForTest(bool enabled);
    // retail 004b6da0 runs the death sequence (005f7200) on the first update with every
    // party member down; the sequence holds 12 s, fades to black over 1 s, then ends the game.
    static constexpr float kPartyWipeSeconds = 0.0f;
    static constexpr float kDeathSequenceSeconds = 13.0f;
    bool scriptPauseActive() const;
    void clearScriptPause();
    // The last start attempt found no starting link whose Active passed. This is
    // retail behaviour for such a conversation, not a load failure.
    bool dialogueHadNoStart() const { return sim_->noStartLink; }
    // Dialogue CamVidEffect values the install cannot apply (no such
    // videoeffects.2da row or no enabled pass). Retail ignores them too.
    int unsupportedCamVidEffects() const { return videoEffects_.rejectedCount(); }
    // Video effects (ACT_508/510 and dialogue CamVidEffect; render/VideoEffect.h).
    void enableVideoEffect(std::int32_t row) { videoEffects_.enable(install_, row); }
    void disableVideoEffect() { videoEffects_.disable(); }
    const VideoEffectState& videoEffect() const { return videoEffects_.state(); }
    const VideoEffectController& videoEffectController() const { return videoEffects_; }
    const std::vector<script::WorldEvent>& actionTrace() const;
    bool probeAction(std::uint16_t actionId);
    const std::vector<DialogueChoiceEvent>& dialogueChoiceTrace() const {
        return dialogueChoices_;
    }
    const std::vector<ModuleTransitionEvent>& transitionTrace() const {
        return transitions_;
    }
    void clearTraces();
    bool usePlaceable(std::size_t entityIndex);

    // ---- doors, traps, cinematics, galaxy map (ScriptWorldState.cpp) --------
    // A click on a door, placeable or transition trigger, with retail's rules.
    // The player must be within reach by walkmesh path; the bot walks first.
    UseOutcome useObject(std::size_t entityIndex);
    // Same rules without the reach and control checks (scripted ActionInteractObject).
    UseOutcome useObjectImpl(std::size_t entityIndex, bool checkReach);
    // actor 0 is the player. A locked door opens only with its key; otherwise OnFailToOpen runs.
    UseOutcome openDoor(std::size_t entityIndex, std::uint32_t actor = 0);
    bool closeDoor(std::size_t entityIndex);
    bool unlockDoor(std::size_t entityIndex, std::uint32_t actor = 0); // key first, then Security (d20 + rank)
    bool lockDoor(std::size_t entityIndex);
    bool bashDoor(std::size_t entityIndex);        // melee damage less Hardness; not Plot
    bool detectTrap(std::size_t entityIndex);      // Awareness check
    void updateMineChecks(float dt);               // retail 004fa390 UpdateMineCheck, party members
    bool disarmTrap(std::size_t entityIndex);      // Security check; failure springs it
    bool playerHasKey(const std::string& keyTag);
    // Length of the walkmesh path from the player to an entity; negative when
    // the straight walk is blocked by a wall or leaves the walkmesh.
    float pathDistanceTo(std::size_t entityIndex) const;
    bool inReach(std::size_t entityIndex, float range = 2.8f) const;
    // ChangeToStandardFaction / ChangeFaction: sets the faction, refreshes hostility.
    bool setObjectFaction(std::uint32_t object, int faction);
    bool controlLocked() const;
    void releaseControl();  // clears NoClicksFor and restrict mode, ends the fade
    const FadeState& fade() const { return sim_->fade; }
    const GalaxyMapState& galaxyMap() const { return sim_->galaxy; }
    const PresentationState& presentation() const { return sim_->presentation; }
    bool galaxyMapOpen() const { return sim_->galaxy.open; }
    // The UI/bot picks a planet: it must be available and selectable. Runs the
    // engine's galaxy map script and closes the map.
    bool selectPlanet(std::int32_t planet);
    void closeGalaxyMap() { sim_->galaxy.open = false; }
    // ShowGalaxyMap / SetPlanetAvailable / SetPlanetSelectable without a script (the screen gates).
    void showGalaxyMap(std::int32_t highlighted) { sim_->galaxy.open = true; sim_->galaxy.highlighted = highlighted; }
    void setPlanet(std::size_t planet, bool available, bool selectable)
    {
        if (planet < kPlanetSlots) {
            sim_->galaxy.available[planet] = available;
            sim_->galaxy.selectable[planet] = selectable;
        }
    }
    std::string substituteTokens(std::string text) const;
    std::vector<std::pair<std::int32_t, std::string>> galaxyPlanetNames();
    bool mapPinEnabled(const std::string& waypointTag) const;
    bool mapRevealed(kotor::Vec3 point) const;
    bool areaUnescapable() const { return sim_->areaUnescapable; }
    // The SetAreaUnescapable script action without a script (the screen gates).
    void setAreaUnescapableForTest(bool value) { sim_->areaUnescapable = value; }
    void setGoldForTest(std::int32_t value) { gold_ = value; }
    // "Planet - Area", the text a retail save and the map panel show for the current module.
    std::string areaDisplayName();

    void attackNearestHostile();
    // The target block's arrows: the next (+1, the down arrow) or previous (-1, the up arrow) living
    // enemy within reach by distance, which becomes the attack target. Without a current target
    // the nearest is taken.
    void cycleCombatTarget(int direction);
    // Living enemies within reach of the controlled creature (the arrows show when more than one).
    std::size_t hostilesInReach();
    // The HUD's Clear buttons: drops the controlled creature's queued actions and the target.
    void cancelCombat();
    // Deselects the client's selected object, which hides the HUD target block.
    void clearSelection();
    // The queued actions of the controlled creature (the HUD's queue icons).
    std::size_t controlledQueueSize() const;
    std::uint32_t combatTarget() const { return combatTarget_; }
    std::string combatTargetName() const;
    // Hit points over maximum of the combat target, 0..1 (0 without a target).
    float combatTargetHealthRatio() const;
    // The creature the HUD's target block and reticle are on: the combat target, else the client's
    // selected object (CClientExoApp +0x188) while it is a live creature other than the controlled
    // one. 004cf490 (conversation start) writes the conversation owner to +0x188, so the speaker
    // keeps the block after the conversation (W46). NOT TRACED: the other writers of +0x188
    // (mouse pick, Tab cycling) and what clears it.
    std::uint32_t hudTarget() const;
    // CSWCCreatureStats::GetCanUseSkill(6): Security is a trained-only skill, so it takes a rank.
    bool canUseSecurity() const { return skillRank("security") > 0; }
    std::string hudTargetName() const;
    float hudTargetHealthRatio() const;
    bool hudTargetHostile() const;
    std::int32_t playerHp() const { return playerHp_; }
    std::int32_t playerMaxHp() const { return playerMaxHp_; }
    bool playerDead() const { return playerDead_; }
    // A pending autopause (CClientExoApp::SetAutoPaused reason, 0 for none), taken once by the frame loop.
    // Scripted runs take the Enemy Sighted autopause only while a check turns this on.
    bool enemySightedPauseForTest() const { return enemySightedPauseForTest_; }
    void setEnemySightedPauseForTest(bool on)
    {
        enemySightedPauseForTest_ = on;
        enemySighted_ = false; // the next hostile in sight counts as the first
    }
    int takeAutoPauseRequest()
    {
        const int reason = autoPauseRequest_;
        autoPauseRequest_ = 0;
        return reason;
    }
    // The controlled member's combat message (CSWPartyMemberData +0x84, set by 006345e0): the HUD
    // takes a raised strref once and draws it per CSWGuiMainInterface::SetCombatMessage 00687700.
    void raiseCombatMessage(std::uint32_t strref) { combatMessageRequest_ = strref; }
    std::uint32_t takeCombatMessage()
    {
        const std::uint32_t strref = combatMessageRequest_;
        combatMessageRequest_ = 0;
        return strref;
    }
    bool controlledDebilitated();
    const std::vector<std::string>& combatLog() const { return combatLog_; }
    // What the messages screen's Feedback list shows: the lines the player is told as things happen
    // (experience, credits, journal, level up), oldest first. The texts are retail's own strings.
    void feedback(const std::string& line);
    // Retail 006027c0, gated by option bit 0x10. Types as its switch: 0 damage, 1 heal, 2 miss, 3 XP, 4 level.
    void floatingText(std::uint32_t object, int type, std::int32_t value);
    const std::vector<FloatingText>& floatingTexts() const { return floating_; }
    // The point a line stands on: the object's head; false when it has no place in this area.
    bool floatingAnchor(std::uint32_t object, kotor::Vec3& at);
    // Retail 0062eeb0: 0 journal, 1 credits, 2 XP, 3 stealth XP, 4/5 alignment (sign), 7 item received, 8 item lost.
    void statusFeedback(int kind, std::int32_t value);
    // Retail keeps a bit on the item (+0x288 bit 7): 0055d330 sets it on a new arrival, 00555fd0 on leaving the holder.
    bool isNewItem(std::uint32_t object) const { return newItems_.contains(object); }
    void markNewItem(std::uint32_t object) { newItems_.insert(object); }
    void clearNewItem(std::uint32_t object) { newItems_.erase(object); }
    void clearNewItems() { newItems_.clear(); } // inferred: 00556050 empties a list the same way; its caller was not read
    // This area's explored map; null where the area has no map.
    const MapFog* mapFog() const;
    // Retail 00579210: the point's cell has been seen. True where there is no map to hide.
    bool mapPointSeen(const kotor::Vec3& at) const;
    // Retail 005792d0: the point's cell and those within `radius` (a diamond). Below zero, all of it (00578fa0).
    void markMapAt(const kotor::Vec3& at, int radius);
    void suppressStatusSummaryEntries(std::int32_t count) { statusSuppress_ += count; } // retail 0062f0c0, +0x78
    // Retail 00602eb0: a quarter second with no conversation or screen up, then 0062ef90 raises the pop-up or the icons.
    void stepFeedback(float dt, bool blocked);
    bool statusSummaryOpen() const { return statusOpen_; }
    const StatusSummary& statusSummaryShown() const { return statusShown_; }
    void dismissStatusSummary() { statusOpen_ = false; }
    const StatusSummary& statusSummaryPending() const { return statusPending_; }
    // Seconds left of a HUD feedback icon, indexed as 00686920 binds them and 00687fe0 raises them.
    float feedbackIconSeconds(std::size_t index) const { return index < feedbackIcons_.size() ? feedbackIcons_[index] : 0.0f; }
    // A run with no one to answer a pop-up gets the icons instead.
    void holdStatusSummaryForTest(bool held) { statusHeld_ = held; }
    // Retail 00631d80, 006a70a0: with subtitles off a voiced line shows no text; replies and silent lines still do.
    bool dialogueLineShown() const { return (feedbackOptions_ & 0x4u) != 0u || !(dialogueVoiceStarted_ || voicedForTest_); }
    void setDialogueVoicedForTest(bool voiced) { voicedForTest_ = voiced; }
    // The entry is still being spoken (its voice plays, or a silent entry's hold runs): the dialog
    // panel shows the line, the number keys choose nothing and a click skips it (CSWGuiDialog
    // 006a7230: state bit 0x77e&1 -> SkipPastDialog 004ae970; SetReplies 006a86a0 clears the bit).
    bool dialogueLineSpeaking() const;
    void skipDialogueLine();
    // The lines the PC has spoken since the last call, "Name: text" (a chosen reply with text is the PC's line,
    // retail 006bd260 -> AppendToDialogBuffer 00630560); the Messages dialog list takes them.
    std::vector<std::string> takeSpokenReplies() { return std::exchange(spokenReplies_, {}); }
    const std::vector<std::string>& feedbackLog() const { return feedbackLog_; }
    const std::vector<std::string>& scriptLog() const { return scriptLog_; }

    // Script/world state used by the P5 gates and the save file.
    const script::World& scriptWorld() const { return scriptWorld_; }
    script::World& scriptWorld() { return scriptWorld_; }
    const CharacterSheet& character() const { return character_; }
    CharacterSheet& character() { return character_; }
    std::int32_t gold() const { return gold_; }
    const std::vector<InventoryItem>& inventory() const { return character_.inventory; }
    const std::vector<MerchantState>& merchants() const { return merchants_; }
    const std::vector<EncounterState>& encounters() const { return encounterStates_; }
    int activeMerchant() const { return activeMerchant_; }
    bool openStore(std::size_t entityIndex);
    // Orders the controlled creature to attack entity `entity` (the attack command and its target).
    void attackEntity(std::size_t entity);
    // The store nearest the controlled creature (the M key); false when the area has none.
    bool openNearestStore();
    bool openStoreByTag(const std::string& tag);
    void closeStore();
    // OpenStore (ACT_378): bonuses are clamped to [-100, 100] and added to the
    // store's MarkUp / MarkDown while this store is open.
    bool openStoreFor(std::uint32_t store, std::uint32_t pc, std::int32_t bonusMarkUp,
                      std::int32_t bonusMarkDown);
    std::int32_t storeBonusMarkUp() const { return storeBonusMarkUp_; }
    std::int32_t storeBonusMarkDown() const { return storeBonusMarkDown_; }
    std::int32_t buyPrice(std::size_t stockIndex) const;
    std::int32_t sellPrice(std::size_t inventoryIndex) const;
    bool buyMerchantItem(std::size_t stockIndex);
    bool sellInventoryItem(std::size_t inventoryIndex);
    bool identifyInventoryItem(std::size_t inventoryIndex);
    bool openLoot(std::size_t entityIndex);
    const std::vector<InventoryItem>& loot() const { return lootItems_; }
    bool takeLootItem(std::size_t index);
    void takeAllLoot();
    // Closing a container fires its OnClosed script; remaining loot stays in it.
    void closeLoot();
    // W038: the destroy event a dead non-party creature gets (004e0e2e), and its body bag (004ce220).
    float destroyObjectDelay(const kotor::Entity& creature);
    float fadeDelayOnDeath(const kotor::Entity& creature);
    std::uint32_t spawnBodyBag(std::size_t creatureIndex);
    void updateCorpses(float dt);
    const std::vector<std::string>& partyTags() const { return partyTags_; }
    const std::vector<CompanionState>& companions() const { return companions_; }
    int selectedCompanion() const { return selectedCompanion_; }
    void selectCompanion(int index);
    const std::string& partyLeaderTag() const { return leaderTag_; }
    void setPartyLeader(const std::string& tag);

    // ---- party table (retail PARTYTABLE model, ScriptParty.cpp) -------------
    // NPC ids are the nwscript NPC_* constants (Party.h); the PC is NPC_PLAYER.
    const PartyTable& partyTable() const { return party_; }
    static int npcIdForTag(const std::string& tag); // -1 when not a companion tag
    bool addAvailableNpcByTemplate(int npc, const std::string& templateResref);
    bool addAvailableNpcByObject(int npc, std::uint32_t creature);
    bool removeAvailableNpc(int npc);
    bool isAvailableNpc(int npc) const;
    // SetAvailableNPCId: the object standing for an available NPC.
    void setAvailableNpcObject(int npc, std::uint32_t object);
    // The NPC's current sheet (the member's live one, else the stored one).
    const CharacterSheet* partySheet(int npc) const { return liveSheet(npc); }
    // Creates (or moves) the NPC's creature at a position and returns its object.
    std::uint32_t spawnAvailableNpc(int npc, kotor::Vec3 position, float facing);
    bool addPartyNpc(int npc, std::uint32_t creature);
    bool removePartyNpc(int npc);
    bool isNpcPartyMember(int npc) const;
    // The selectability byte; 255 for an NPC that is not available (retail).
    int npcSelectability(int npc) const;
    void setNpcSelectability(int npc, int value);
    bool setPartyLeaderNpc(int npc);
    bool switchPlayerCharacter(int npc);

    // ---- companion control (PartyControl.cpp) --------------------------------
    // The controlled creature receives the input, the camera and the player's
    // interactions. The PC keeps position_, playerHp_ and its combat state while
    // a companion is controlled and follows like any other member.
    bool companionControlled() const;
    // Entity index of the controlled creature; the PC's avatar entity (which may
    // be -1 outside third person) while the PC leads.
    std::size_t controlledEntity() const;
    kotor::Vec3 controlledPosition() const;
    float controlledBearing() const;
    std::int32_t controlledHp() const;
    std::int32_t controlledMaxHp() const;
    bool controlledDead() const;
    // UI and bot: choose who to control. Refused for an unavailable, unselectable
    // or dead creature, and while solo mode is on or the player has no control.
    bool selectLeaderNpc(int npc);
    // Tab (keymap.2da ChangeChar): the next selectable living creature in
    // PC-then-member order. False when nobody else can be chosen.
    bool cycleLeader();
    // The creatures the player may pick, PC first (kNpcPlayer then member ids).
    std::vector<int> selectableLeaders() const;
    void setSoloMode(bool solo);
    bool soloMode() const { return party_.solo; }
    // Refreshes the stored creature of an NPC from its live state.
    void saveNpcState(int npc);
    // The creature scripts see as GetFirstPC / party member 0, and the party in
    // GetPartyMemberByIndex order (leader first).
    std::uint32_t controlledObject() const;
    std::vector<std::uint32_t> partyObjects() const;
    int npcForObject(std::uint32_t object) const; // kNpcPlayer for the PC, -2 otherwise
    // The party-table half of an XP award (retail PartyTable::AddXP): the shared
    // pool and every member. The PC's own share is giveXp, which calls this.
    void awardPartyXp(std::int32_t amount);
    // Share of an award an NPC receives (npc.2da PercentXP * GAME_XP_General).
    float npcXpFactor(int npc);
    // ShowPartySelectionGUI. The UI and the bot drive the same state: open,
    // toggle NPCs, then accept or cancel (both run the exit script).
    const PartySelectionState& partySelection() const { return partySelection_; }
    bool openPartySelection(const std::string& exitScript, int forceNpc1, int forceNpc2);
    // The map panel's Party Selection button (retail 00693650 decides it, 0062dd20 refuses it when
    // the script did not ask): the strref of why it is refused, 0 when it is open to the player.
    std::uint32_t partySelectionRefusal();
    bool openPartySelectionFromMap();
    // The same refusal on the other UI path (the L key): the reason goes to the feedback log, since
    // no map panel is open to show it.
    bool requestPartySelectionFromUi();
    // Every party member stands within `meters` of the controlled creature.
    bool partyTogether(float meters = 40.0f);
    // A living enemy within `meters` of any party member.
    bool hostileNearParty(float meters = 11.0f);
    bool partySelectionToggle(int npc);
    bool acceptPartySelection();
    void cancelPartySelection();
    // Party death (engine rules, see ScriptParty.cpp): a member at 0 HP is down;
    // the game is lost only when the PC and every member are down.
    bool partyMemberDown(int npc) const;
    bool revivePartyMember(int npc, std::int32_t hp);
    bool partyAllDown() const;
    // SetLockHeadFollowInDialog state, read by the dialogue camera/head code.
    bool headFollowLocked(std::uint32_t object) const {
        return party_.headFollowLocked.contains(object);
    }
    void setHeadFollowLocked(std::uint32_t object, bool locked) {
        if (locked) {
            party_.headFollowLocked.insert(object);
        } else {
            party_.headFollowLocked.erase(object);
        }
    }
    // Retail save mapping: the party table into / out of an original save.
    void writePartyToOriginalSave(kotor::OriginalSave& save) const;
    void readPartyFromOriginalSave(const kotor::OriginalSave& save);
    // Engine save: the party_* lines.
    void writePartyState(std::ostream& file) const;
    bool restorePartyTable(PartyLoad&& load);
    UiPanel activePanel() const { return activePanel_; }
    void setUiPanel(UiPanel panel) { activePanel_ = panel; }
    void toggleUiPanel(UiPanel panel) {
        if (!classicBugs_ && dialogueView_.active) {
            return;
        }
        activePanel_ = activePanel_ == panel ? UiPanel::None : panel;
    }
    void setBugParityMode(bool classic) { classicBugs_ = classic; }
    bool bugParityMode() const { return classicBugs_; }
    bool applyGuiAction(const std::string& tag);
    std::int32_t journalState(const std::string& plotId) const;
    std::size_t journalSize() const { return journal_.size(); }
    const std::map<std::string, std::int32_t>& journal() const { return journal_; }
    bool addJournalEntry(const std::string& plotId, std::int32_t state);
    bool removeJournalEntry(const std::string& plotId);
    // AddJournalQuestEntry semantics (ACT_367): monotonic unless override, and a
    // state that advances awards its global.jrl plot XP.
    bool addJournalQuest(const std::string& plotId, std::int32_t state, bool allowOverrideHigher);
    std::string journalEntryText(const std::string& plotId, std::int32_t state);
    // The journal screen: a quest's title (global.jrl category Name) and whether its current
    // state ends it.
    std::string journalCategoryName(const std::string& plotId);
    bool journalQuestFinished(const std::string& plotId);
    std::int32_t journalQuestExperience(const std::string& plotId);
    // GivePlotXP (ACT_714): plot.2da row by label, xp * percent / 100.
    void givePlotXp(const std::string& plotLabel, std::int32_t percent);
    // The XP a dialogue node's PlotIndex/PlotXPPercentage pair awards.
    std::int32_t plotXpForIndex(std::int32_t plotIndex, float fraction);
    bool addInventoryItem(const std::string& resref, std::int32_t baseItem,
                          std::int32_t stack = 1, std::int32_t cost = 0);
    bool removeInventoryItem(const std::string& resref, std::int32_t stack = 1);
    bool equipItem(const std::string& resref);

    // ---- item objects (ScriptItems.cpp) ----------------------------------------
    // One model: an item anywhere is a script object (tag, resref, base item,
    // stack, identified, plot, stolen, charges, loaded from its UTI). Every
    // creature, placeable and store holds a list of them. The party shares the
    // PC's inventory (retail routes a party member's inventory to the party's),
    // and that list is character_.inventory, linked to its item objects.
    // Creates an item from a UTI and puts it in `holder`'s inventory. Returns the
    // item object (the stack it merged into, if any) or kObjectInvalid.
    std::uint32_t giveItem(const std::string& resref, std::uint32_t holder,
                           std::int32_t stack = 1,
                           std::uint32_t from = script::kObjectInvalid);
    // Moves an existing item object into `holder`'s inventory.
    bool moveItem(std::uint32_t item, std::uint32_t holder,
                  std::uint32_t from = script::kObjectInvalid);
    // Removes the item from its holder and invalidates it.
    bool destroyItem(std::uint32_t item);
    // The tagged item in the holder's inventory or worn slots (GetItemPossessedBy).
    std::uint32_t itemPossessedBy(std::uint32_t holder, const std::string& tag);
    // The creature, placeable or store holding the item (the PC for the party).
    std::uint32_t itemPossessor(std::uint32_t item);
    // Items in a holder's carried inventory, in order. Worn items are not listed.
    std::vector<std::uint32_t> itemsOf(std::uint32_t holder);
    std::vector<std::uint32_t> itemsWornBy(std::uint32_t creature);
    // The item object standing for character_.inventory[index].
    std::uint32_t inventoryObject(std::size_t index);
    bool equipItemObject(std::uint32_t creature, std::uint32_t item, std::int32_t slot = -1);
    bool unequipItemObject(std::uint32_t creature, std::uint32_t item);
    std::uint32_t wornItemObject(std::uint32_t creature, std::int32_t slot);
    // Uses an item as `user`: applies its CastSpell item properties through the spell's
    // impact script, raises the module activation event and spends a charge or one of
    // the stack. The quest bot and the UI both call this.
    bool useItem(std::uint32_t item, std::uint32_t user,
                 std::uint32_t target = script::kObjectInvalid);
    // True when the item carries an ActivateItem property the player can use.
    bool itemUsable(std::uint32_t item);
    // Gold held by a creature (the party's for the PC and party members).
    std::int32_t creatureGold(std::uint32_t creature) const;
    void addCreatureGold(std::uint32_t creature, std::int32_t amount);
    bool addPartyMember(const std::string& tag);
    bool removePartyMember(const std::string& tag);
    bool learnFeat(std::int32_t featId);
    bool learnPower(std::int32_t powerId);
    // A pick on the level-up power list: spends a power point (Rules::canLearnPowerAtLevelUp).
    bool learnLevelUpPower(std::int32_t powerId);
    bool spendSkillPoint(const std::string& skill);
    bool spendAttributePoint(std::int32_t index);
    // One rank of skills.2da row `index`: the class's cost per rank and the level's rank cap apply.
    bool spendSkillRank(std::int32_t index);
    void giveXp(std::int32_t amount);

    // Combat/Force/alignment core.
    bool useFeat(std::int32_t featId, std::uint32_t target = 0);
    bool useForcePower(std::int32_t powerId, std::uint32_t target = 0);
    CombatMode combatMode() const { return combatMode_; }
    std::int32_t forcePoints() const { return character_.fp; }
    std::int32_t maxForcePoints() const { return character_.maxFp; }
    std::int32_t alignment() const { return character_.alignment; }
    void setAlignment(std::int32_t value) { character_.alignment = std::clamp(value, 0, 100); }
    void adjustAlignment(std::int32_t delta) { setAlignment(character_.alignment + delta); }
    std::int32_t lastForcePower() const { return lastForcePower_; }
    bool lastForcePowerSuccess() const { return lastForcePowerSuccess_; }

    // Run a raw NCS payload (test fixtures and tooling) with the live action
    // runtime. Returns false when decode or execution fails.
    bool runScriptNcsForTest(const std::string& label,
                             const std::vector<std::uint8_t>& bytes,
                             std::uint32_t self = 0);

    // Run one shipped script with optional event arguments. Returns the top of
    // stack (the condition value) when the script returns a value.
    std::optional<script::Value> runScript(const std::string& resref,
                                           std::uint32_t self = 0,
                                           std::uint32_t entering = 0,
                                           std::uint32_t caller = 0,
                                           std::uint32_t trigger = 0);
    // Run a script with an explicit event context. Saves and restores the outer
    // context, so nested runs cannot corrupt the calling script.
    std::optional<script::Value> runScriptWith(const std::string& resref, std::uint32_t self,
                                               const ScriptContext& context);
    // Test fixtures: a compiled script served by resref ahead of the install.
    void addScriptForTest(const std::string& resref, std::vector<std::uint8_t> bytes);
    // Retail drops a global globalcat.2da does not declare, so a gate's read-out must say it is one.
    void declareGlobalForTest(const std::string& name);
    std::uint32_t objectForTag(const std::string& tag) const;
    std::uint32_t areaObject() const { return sim_->areaObject; }
    std::uint32_t moduleObject() const { return sim_->moduleObject; }
    // World-simulation queries used by gates and later families.
    kotor::Vec3 actorPosition(std::uint32_t object);
    std::size_t actorQueueSize(std::uint32_t object) const;
    std::int32_t runScriptVar() const { return sim_->runScriptVar; }
    // Central damage and death. Every kill path ends in onCreatureKilled, which
    // is idempotent: one ScriptDeath, one XP award.
    void damageEntity(std::size_t index, std::int32_t amount, std::uint32_t attacker);
    void onCreatureKilled(std::size_t index, std::uint32_t killer);
    // Runs an event pass now (a tick does this too).
    void flushScriptEvents();
    // Snapshot of the module with this name, or null if never left.
    const ModuleSnapshot* moduleSnapshot(const std::string& module) const;
    std::uint32_t objectForEntityIndex(std::size_t index) const { return objectForEntity(index); }
    void setCharacterForTest(const CharacterSheet& sheet) {
        character_ = sheet;
        playerHp_ = sheet.hp;
        playerMaxHp_ = sheet.maxHp;
        playerDead_ = false;
    }
    // Apply a fully-built character sheet (character generation/level-up) to
    // the live session and refresh the script world and rendered avatar.
    void applyCharacter(const CharacterSheet& sheet);
    void setPlayerAppearance(std::uint32_t appearance, const std::string& name = {});
    std::uint32_t playerAppearance() const { return playerAppearance_; }
    // The map's pins (retail 00692c70): waypoints with a note that is enabled, by entity index, in
    // area order. Still different: retail also requires the pin's fog cell to be explored.
    std::vector<std::size_t> mapPins() const;
    // The feedback options, by retail's bits of the options word (FrontEnd.h's FeedbackRow).
    std::uint32_t feedbackOptions() const { return feedbackOptions_; }
    void setFeedbackOptions(std::uint32_t bits)
    {
        if ((bits ^ feedbackOptions_) & 0x2u) {
            setTutorialWindowsEnabledForTest((bits & 0x2u) != 0u); // 0062f420 tests this bit
        }
        feedbackOptions_ = bits;
    }
    // What the PC's Body slot gives its model (retail 00697610).
    kotor::BodyVariant wornBodyVariant();
    void setThirdPerson(bool enabled) {
        thirdPerson_ = enabled;
        if (enabled) {
            spawnPlayerAvatar();
        } else if (playerEntity_ != static_cast<std::size_t>(-1)
                   && playerEntity_ < scene_.entities.size()) {
            scene_.entities[playerEntity_].dead = true;
        }
    }
    void setCameraNoClip(bool enabled) { cameraNoClip_ = enabled; }
    bool thirdPerson() const { return thirdPerson_; }
    std::size_t playerEntity() const { return playerEntity_; }
    void attackPlayerForTest(std::size_t index);
    std::int32_t forceArmorBonus() const;
    std::int32_t forceAttackBonus() const;
    std::int32_t forceDamageBonus() const;
    std::int32_t forceShield() const;
    bool hasEffect(const std::string& label) const;
    bool forceSuppressed() const { return character_.forceSuppressedTimer > 0.0f; }
    std::int32_t forceJumpBonus() const { return forceJumpBonus_; }
    bool canStealth() const { return rules_.loaded() && rules_.allowsStealth(character_); }
    bool enterStealth();
    void leaveStealth();
    bool stealthActive() const { return stealthActive_; }
    bool playAnimation(std::size_t entityIndex, const std::string& name, bool loop = true);
    // Rests a door or placeable in the clip of its state (a restored open door stands open).
    void applyObjectStateClip(std::size_t entityIndex);
    void stopAnimation(std::size_t entityIndex);
    std::string animationName(std::size_t entityIndex) const;
    float animationTime(std::size_t entityIndex) const;
    std::uint32_t animationRevision() const { return animationRevision_; }
    // Gameplay sound and visual effects (game/Effects.h).
    EffectDirector& effects() { return *effects_; }
    std::uint32_t effectsTopologyRevision() const;
    bool entityHasFacialPose(std::size_t entityIndex) const;
    void initAnimations();
    void updateAnimations(float dt);
    // The clip a creature shows for how it moves and what it holds (AnimationFamily.cpp).
    void updateCreatureStances(float dt);
    bool hidesEquippedItems(const std::string& clip);
    void playAttackClip(std::size_t entityIndex);
    // The swing playAttackClip would start next and its impact time in seconds (combatanimations.2da
    // hit1, retail 005b4f30); negative when unknown.
    float nextAttackImpact(std::size_t entityIndex, int subAttack = 0) const;
    // The PC's body is the avatar entity, which has no script object of its own.
    std::uint32_t objectOfBody(std::size_t entityIndex) const {
        return entityIndex == playerEntity_ ? scriptWorld_.pc : objectForEntity(entityIndex);
    }
    std::size_t bodyOfObject(std::uint32_t object) const {
        return object == scriptWorld_.pc ? playerEntity_ : entityOfObject(object);
    }
    void refreshHeldItems(std::uint32_t creature);
    void setAnimationEnabled(bool enabled) { animationEnabled_ = enabled; }
    bool animationEnabled() const { return animationEnabled_; }
    // Test hook invocation without moving the player; returns false if the
    // entity or hook is absent.
    bool runHookForTest(const std::string& hook, const std::string& tag,
                        std::uint32_t entering = 0);

    std::optional<std::vector<script::Value>> action(
        std::uint16_t actionId, const std::vector<std::vector<script::Value>>& args,
        script::World& world, script::Vm& vm) override;
    std::string moduleName() const override { return module_; }

    // Player-level movement: a command on the PC's action queue, walked over the
    // following frames along a walkmesh path. Nothing here teleports. moveTo returns
    // false, and queues nothing, when no route exists; a route ending at a closed
    // door still queues and finishes with MoveOutcome::DoorBlocked.
    enum class MoveOutcome { None, Moving, Arrived, DoorBlocked, Unreachable };
    bool moveTo(kotor::Vec3 target, float stopDistance, bool run = true);
    bool pathExists(kotor::Vec3 from, kotor::Vec3 to, std::size_t* blockingDoor = nullptr);
    bool movePending() const;
    MoveOutcome moveOutcome() const { return moveOutcome_; }
    std::size_t moveBlockingDoor() const { return moveBlockingDoor_; }
    // Opens a door as a player's click does. False when it is locked.
    bool openDoorAsPlayer(std::size_t entityIndex);

    // Walk tooling (app/WalkTools.cpp, game/WalkSupport.cpp). Reads of state a walk step reports; none of it
    // changes play. walkPlaceLeader is for per-module probing only (--start-wp), never the continuous run.
    std::string dialogueResref() const { return activeDialogue_ ? activeDialogue_->resref : std::string{}; }
    bool usePending() const { return pendingUse_ != UINT32_MAX; }
    // What the last default action on an object did (a UseOutcome), -1 when none ran since the last call.
    int takeLastUseOutcome() { const int outcome = lastUseOutcome_; lastUseOutcome_ = -1; return outcome; }
    bool walkPlaceLeader(kotor::Vec3 at, float yaw);

    // Deterministic helpers used by the scripted playthrough gate.
    bool walkTo(kotor::Vec3 target, float maxSeconds);
    std::size_t findEntity(const std::string& tag) const;
    void setPositionForTest(kotor::Vec3 position) { position_ = position; }
    void faceTowards(kotor::Vec3 target);
    // Finds a standable spot with a clear walkmesh line to the target. Used by
    // the demo captures so an NPC is not hidden behind a partition.
    bool findClearView(kotor::Vec3 target, float radius, kotor::Vec3& out) const;

private:
    struct ActiveDialogue {
        std::string npcTag;
        std::string speaker;
        std::string resref;
        std::uint32_t npcObject = 0;
        kotor::Dlg dlg;
        DialogueShots shots;
        // The first answer per Speaker tag holds for the conversation (0059fc30).
        std::map<std::string, std::uint32_t> speakers;
    };

    bool loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor);
    const kotor::Entity* nearestDoor(float maxDistance) const;
    const kotor::Entity* nearestConversation(float maxDistance) const;
    bool loadDialogue(const std::string& resref, kotor::Dlg& dlg, int speakerGender = 0);
    void showDialogueEntry(std::uint32_t entryIndex);
    // The voice, fade and participant animations a node carries (retail reader 0059f5f0).
    void playDialoguePresentation(const kotor::DlgNodePresentation& node,
                                  const std::string& fallbackSound);
    void restoreDialogueAnimations();
    // StuntList of a cutscene conversation: each named participant is replaced by its stunt
    // model (hidden original, shown stand-in) until the conversation ends.
    void spawnDialogueStunts();
    void removeDialogueStunts();
    std::size_t dialogueStuntFor(std::size_t participant) const;
    void tickDialogueAdvance(float dt);
    bool startConversation(std::uint32_t owner, const std::string& resref);
    void finishDialogue(bool aborted);
    void clearDialogueState();
    void completeArrival();
    ScriptContext dialogueContext() const;
    bool dialogueLinkPasses(const std::string& linkActive, const std::string& nodeActive);
    template <typename Node>
    bool applyDialogueNode(const Node& node, std::uint32_t generation);
    std::vector<std::uint32_t> passingReplies(const kotor::DlgEntry& entry);
    std::uint32_t firstPassingEntry(const kotor::DlgReply& reply);
    bool dialogueSpeakerResolves(const std::string& speaker);
    void loadJournalData();
    void loadPlotTable();
    const JournalCategory* journalCategory(const std::string& plotId);
    std::int32_t plotXp(std::int32_t row, double fraction);
    std::int32_t plotXpByLabel(const std::string& label, double fraction);
    ActorStep stepActorToward(std::uint32_t object, kotor::Vec3 target, float speed, float dt,
                              float stopDistance, bool ramp = true);
    bool lineOfSight(kotor::Vec3 from, kotor::Vec3 to) const;
    // Retail GetObstructionFactor 0050ccf0 for a sound: 0.4 for every surface of the area the line from the
    // sound to the listener crosses (surfacemat lineofsight), the line restarted a tenth of a metre past each
    // hit, 1.0 once the sum passes 1. A closed door on the line adds 0.55 and a placeable 0.4 (once per object).
    float soundObstruction(kotor::Vec3 from, kotor::Vec3 to);
    // World-space walkmesh triangles of the area's closed doors (<model>0.dwk) and placeables (<model>.pwk).
    struct SoundFacet {
        kotor::Vec3 a;
        kotor::Vec3 b;
        kotor::Vec3 c;
        std::uint32_t entity = 0;
        bool door = false;
    };
    const std::vector<SoundFacet>& soundObjectFacets();
    script::ProgramPtr decodedScript(const std::string& resref);
    ScriptContext hookContext(const std::string& hook, std::uint32_t entering) const;
    void placeAtLink(const std::string& linkedTo);
    // Module persistence, in WorldPersistence.cpp.
    void leaveModule();
    std::map<std::uint32_t, script::GameObject> collectCarried() const;
    void restoreCarried();
    ModuleSnapshot captureModuleSnapshot(bool leaving) const;
    void applyModuleSnapshot(const ModuleSnapshot& snapshot);
    void writeWorldState(std::ostream& file) const;
    bool restoreWorldFromSave(LoadedWorldState state, const std::string& module,
                              const kotor::Vec3& position);
    bool loadDialogueCameraModel(const std::string& resref);
    void setDialogueCameraClip(std::int32_t animation);
    void updateCombat(float dt);
    void updateTriggers();
    void updateScripts(float dt);
    void updateTimers(float dt);
    bool runFunctor(const script::Functor& functor, std::uint32_t target);
    void logEvent(std::string line);
    std::uint32_t random();
    bool pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const;

    void initScriptWorld();
    void syncScriptWorld();
    void refreshHostility();
    void initMerchants();
    std::int32_t scaledPrice(std::int32_t baseItem, std::int32_t cost, std::int32_t percent) const;
    void spawnEncounters();
    std::int32_t spawnEncounterCreatures(std::size_t index);
    // Encounter entry/spawn rules and trap/trigger helpers live in ScriptWorldState.cpp.
    std::int32_t spawnEncounterWave(std::size_t index);
    void updateEncounterVolumes(float dt);
    void springTrap(std::size_t entityIndex, std::uint32_t victim);
    bool keyUnlock(std::size_t entityIndex); // key in inventory unlocks; AutoRemoveKey consumes it
    std::int32_t skillRank(const char* skill) const;
    bool skillCheck(const char* skill, std::int32_t dc, const char* what);
    std::uint32_t spawnScriptCreature(const std::string& resref, kotor::Vec3 position, float facing);
    void updateEncounters(float dt);
    void updateParty(float dt);
    // Navigation, in Pathfinding.cpp.
    nav::NavMesh* navMesh();
    // Name of the LYT room under `point` (empty off the walkmesh). Drives the per-room EAX environment.
    std::string roomNameAt(kotor::Vec3 point);
    void loadMovementTables();
    float creatureMoveRate(std::uint32_t object, bool run);
    // The keyboard drive of a controlled creature (retail CSWPlayerControlCamRelative, 00679870 / vtable 00679940):
    // advances its velocity toward `scale` times the top speed along (dirX, dirY) and returns this frame's move.
    kotor::Vec3 driveStep(std::uint32_t object, float dirX, float dirY, float scale, float dt);
    void driveStop(std::uint32_t object);
    // CSWSArea::ComputeSafeLocation 004be860 for a creature of this appearance: the nearest spot to `point` within `reach`
    // metres where TestSafeLocationPoint 004be5e0 holds. Only creatures before index `before` count as in the way (module load adds them one by one).
    bool computeSafeLocation(kotor::Vec3 point, float reach, std::uint32_t appearance, bool lineTest, std::size_t before,
                             kotor::Vec3& out);
    // Whether a creature moving at `speed` shows its run clip, and the distance one cycle of the clip it shows covers.
    void locomotionOf(std::uint32_t object, std::uint32_t appearance, float speed, bool& running, float& stride);
    // The velocity the drive controller holds for `object` after a blocked step: what the walkmesh left of the move.
    void driveSetVelocity(std::uint32_t object, float vx, float vy);
    float creatureRadius(std::uint32_t object);
    float creatureApproachRange(std::uint32_t self, std::uint32_t target, float range);
    kotor::Vec3 slidePlayer(kotor::Vec3 from, kotor::Vec3 to, bool& moved, float radius = -1.0f);
    kotor::Vec3 formationSlot(kotor::Vec3 anchor, float anchorYaw, int slot, float baseDistance);
    void followLeader(std::uint32_t follower, kotor::Vec3 anchor, kotor::Vec3 slot, float dt);
    void placePartyAtEntry();
    friend int runPathfindingFamilyTest(GameSession& game);
    friend int quest::runQuestTrace(GameSession& game, const quest::QuestTraceOptions& options);
    friend int quest::runConversationFlow(GameSession& game, const quest::FlowOptions& options);
    friend int quest::runPlotReplay(GameSession& game, const quest::FlowOptions& options);
    friend int runCompanionFamilyTest(GameSession& game);
    friend int runAnimationFamilyTest(GameSession& game);
    friend void grantWeaponFeats(GameSession& game, std::uint32_t object);
    friend bool runAnimationCaptureAction(GameSession& game, const std::string& action);
    // Companion control internals (PartyControl.cpp).
    void stepControlledCompanion(float dt, const InputState& input);
    void onControlChanged(int previous, int next);
    void passControlIfDown();
    void controlPcDefence();
    void showControlledBodies();
    std::size_t ensureCompanionEntity(const CompanionState& companion);
    // Party table internals (ScriptParty.cpp).
    void tickParty(float dt);
    void syncPartyViews();
    void syncCompanionSheet(int npc);
    std::size_t ensureNpcEntity(int npc, const kotor::Vec3* position, float facing,
                                bool* created = nullptr);
    void catchUpNpcXp(int npc);
    bool loadNpcSheet(int npc);
    CompanionState* companionFor(int npc);
    void applyNpcSoloVisibility();
    void autoLevelCompanion(CharacterSheet& sheet, std::int32_t targetLevel);
    void loadPartyTables();
    std::string templateForNpc(int npc);
    std::size_t entityOfObject(std::uint32_t object) const;
    const CharacterSheet* liveSheet(int npc) const;
    void grantMemberXp(CompanionState& companion, int npc, std::int32_t amount);
    std::uint32_t objectForEntity(std::size_t index) const;
    void runHook(const std::string& hook, std::size_t entityIndex,
                 std::uint32_t entering = 0, const ScriptContext* extra = nullptr);
    bool runSceneHook(const std::string& hook, std::uint32_t entering = 0,
                      const ScriptContext* extra = nullptr);
    void runSpawnAndEnterScripts();
    void runReturnScripts();
    bool scriptValueTruthy(const std::optional<script::Value>& value) const;
public:
    bool engineValuesEqual(const script::Value& a, const script::Value& b) const;
private:
    void loadGlobalCatalog();
    void syncPlayerToScriptWorld();
    void spawnPlayerAvatar();
    std::set<std::uint32_t> newItems_;
    mutable std::map<std::string, MapFog> mapFogs_;
    std::vector<FloatingText> floating_;
    std::uint32_t xpFloatObject_ = 0xFFFFFFFFu;
    StatusSummary statusPending_;
    StatusSummary statusShown_;
    bool statusOpen_ = false;
    bool statusHeld_ = false;
    bool statusPopupOwed_ = false;
    bool voicedForTest_ = false;
    std::int32_t statusSuppress_ = 0;
    float statusWait_ = 0.0f;
    std::array<float, 9> feedbackIcons_{};
    std::uint32_t feedbackOptions_ = 0x43eu; // 0061d950's reset value of the rows' bits
    void updatePlayerAvatar(float dt, bool moving);
    void updateAudioListener();
    void updateFacialMeshes();
    // Each line's speaker and listener turn toward each other (retail 006306e0).
    void faceDialogueParticipants();
    bool appearanceRows(); // Loads appearanceHeads_.
    void tickHeadFollow(float dt);
    void syncEntityToScriptWorld(std::size_t index);
    std::string scriptSource(const std::string& resref);

    std::string installPath_;
    std::filesystem::path rsvRoot_;
    std::string module_;
    kotor::Install install_;
    std::string soundFacetKey_;
    std::vector<SoundFacet> soundFacets_;
    std::unique_ptr<kotor::Localization> localization_;
    std::uint32_t languageId_ = 0;
    std::unique_ptr<AudioSystem> audio_;
    std::unique_ptr<EffectDirector> effects_;
    kotor::Scene scene_;
    kotor::Vec3 position_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    std::size_t playerEntity_ = static_cast<std::size_t>(-1);
    std::uint32_t playerAppearance_ = 92;
    std::string playerName_ = "Player";
    std::vector<std::string> spokenReplies_;
    float playerBearing_ = 0.0f;
    bool thirdPerson_ = false;
    mutable bool playerVisible_ = true;
    bool cameraNoClip_ = false;
    float verticalVelocity_ = 0.0f;
    bool grounded_ = false;
    float elapsed_ = 0.0f;

    std::optional<kotor::Tlk> tlk_;
    // stringtokens.2da, keymap.2da and classes.2da for the dialogue token parser (0041dd30).
    mutable bool stringTokensLoaded_ = false;
    mutable std::optional<kotor::TwoDa> stringTokens_;
    mutable std::optional<kotor::TwoDa> keymapTable_;
    mutable std::optional<kotor::TwoDa> classesTable_;
    std::optional<ActiveDialogue> activeDialogue_;
    DialogueView dialogueView_;
    VideoEffectController videoEffects_;
    std::shared_ptr<kotor::Mdl> dialogueCameraModel_;
    std::string dialogueCameraClip_;
    float dialogueCameraTime_ = 0.0f;
    float dialogueCameraDuration_ = 0.0f;
    std::optional<kotor::LipSync> activeLip_;
    bool dialogueAutoAdvance_ = false;
    float dialogueEntryAge_ = 0.0f;
    float dialogueHold_ = 0.0f;       // seconds the entry waits when no voice plays
    struct HeadFollow {
        std::size_t entity = 0;
        std::size_t target = 0;
        float arcH = 0.0f; // Radians, appearance.2da head_arc_h and head_arc_v.
        float arcV = 0.0f;
    };
    std::vector<HeadFollow> headFollow_;
    std::optional<kotor::TwoDa> appearanceHeads_;
    bool dialogueVoiceStarted_ = false;
    bool dialogueLineSkipped_ = false; // skipDialogueLine on the current entry
    bool dialogueTrackStarted_ = false;
    // Entities an AnimList moved off their idle clip, with the clip to return to.
    struct SavedAnimation {
        std::size_t entity = 0;
        std::string name;
        bool loop = true;
    };
    std::vector<SavedAnimation> dialogueSavedAnimations_;
    // Nodes reached, (is a reply, index), for the sequence modes only.
    std::vector<std::pair<bool, std::uint32_t>> dialogueTrail_;
    bool traceDialogueTrail_ = false;
    struct DialogueStunt {
        std::size_t participant = 0;
        std::size_t stand = 0;
    };
    std::vector<DialogueStunt> dialogueStunts_;
    float lipTime_ = 0.0f;
    FacialRig facialRig_;
    std::map<std::string, bool> hideItemClips_;
    FactionSystem factions_;
    bool moviePlaying_ = false;
    std::string movieName_;
    float movieTime_ = 0.0f;
    float movieDuration_ = 0.0f;
    struct QueuedMovie {
        std::string name;
        bool skippable = true;
    };
    std::vector<QueuedMovie> movieQueue_;
    bool movieSkippable_ = true;
    bool movieSeparateSkips_ = true;
    bool movieHoldsWorld_ = false;
    void endCurrentMovie();
    bool startMovie(const std::string& name, bool skippable);
    std::unique_ptr<kotor::BinkPlayer> moviePlayer_;
    kotor::BinkVideoFrame movieFrame_;
    int movieFrameIndex_ = -1;
    std::shared_ptr<audio::SoundData> movieAudioData_;
    int movieAudioVoice_ = 0;
    std::unique_ptr<PazaakGame> pazaak_;
    std::string pazaakEndScript_;
    std::uint32_t pazaakOpponent_ = 0;
    std::int32_t pazaakWager_ = 0;
    int lastPazaakResult_ = -1;
    PazaakStage pazaakStage_ = PazaakStage::None;
    std::vector<PazaakCard> pazaakOpponentSide_;
    std::int32_t pazaakDeckIndex_ = 0;
    std::int32_t pazaakMaxWager_ = 0;
    std::int32_t pazaakWagerChosen_ = 0;
    std::vector<int> pazaakChosen_;
    bool pazaakTutorial_ = false;
    PazaakHelp pazaakSetupHelp_;
    std::uint32_t pazaakSeed_ = 0;
    std::uint32_t pazaakGames_ = 0;

    std::int32_t playerHp_ = 100;
    std::int32_t playerMaxHp_ = 100;
    std::uint32_t combatTarget_ = UINT32_MAX;
    std::uint32_t selectedEntity_ = UINT32_MAX; // client +0x2b4 (SetLastTarget 005f4a20) as an entity index (hudTarget)
    std::vector<std::string> feedbackLog_;
    int combatRound_ = 0;
    CombatMode combatMode_ = CombatMode::Normal;
    std::int32_t combatModeRank_ = 1;
    float combatModeTimer_ = 0.0f;
    std::int32_t lastForcePower_ = -1;
    bool lastForcePowerSuccess_ = false;
    bool stealthActive_ = false;
    std::uint32_t animationRevision_ = 0;
    bool animationEnabled_ = true;
    float footstepTimer_ = 0.0f;
    std::int32_t forceJumpBonus_ = 0;
    std::vector<bool> triggerInside_;
    bool saveModuleStartPending_ = false;
    std::vector<std::string> combatLog_;
    bool playerDead_ = false;
    int autoPauseRequest_ = 0;
    bool enemySighted_ = false;      // retail client +0x324: a hostile has been in sight
    bool enemySightedPauseForTest_ = false;
    float enemySightedTimer_ = 0.0f; // retail +0x394: 10 s after the last one
    void tickEnemySighted(float dt);
    std::uint32_t clickedEntity_ = UINT32_MAX; // the object a world click selected
    std::uint32_t pendingUse_ = UINT32_MAX;    // the object a default action walks to
    int lastUseOutcome_ = -1;                  // what finishUse's last use of an object did (UseOutcome), for walk reports
    void tickPendingUse();
    bool finishUse(std::size_t index);
    // Retail 004f2c30 (CSWSCreature::ValidNearestObjectType): the objects the client's selection scan
    // (005fa5a0 over 004fc4c0) takes into its lists. hostile is the creature's standing against the
    // controlled one; doors and placeables are never hostile here.
    bool targetableKind(std::size_t index) const;
    bool targetableEntity(std::size_t index, bool& hostile);
    struct TargetCandidate {
        std::size_t index = 0;
        float turn = 0.0f;    // counter-clockwise turn from straight ahead, degrees, 0..359
        float squared = 0.0f; // squared distance
        bool hostile = false;
    };
    // 004fc4c0 with 30 m for both ranges: angular is every candidate ascending by turn (equal turns keep
    // entity order); forward is the indices into angular of those in front of the controlled creature
    // inside the cone (see the .cpp), ascending by squared distance.
    void buildTargetLists(std::vector<TargetCandidate>& angular, std::vector<std::size_t>& forward);
    // 00617ad0 CanSee for a candidate: a closed door or placeable is the target's own geometry, which
    // retail leaves out of the ray, so the ray stops short of it.
    bool targetVisible(std::size_t index);
    float targetLosTimer_ = 0.0f; // retail +0x368: how long the selected object has been out of sight
    std::uint32_t combatMessageRequest_ = 0;
    bool steeringMessage_ = false; // 42476 raised by steering (006227e0)
    float partyWipeTimer_ = 0.0f;
    std::vector<UpgradeRow> upgradeRows_;
    bool upgradeRowsLoaded_ = false;
    bool gameOverShown_ = false;
    void updatePartyWipe(float dt);
    bool classicBugs_ = true;
    bool traceActions_ = false;
    std::vector<DialogueChoiceEvent> dialogueChoices_;
    std::vector<ModuleTransitionEvent> transitions_;
    std::uint32_t rng_ = 0x12345678u;

    Rules rules_;
    CharacterSheet character_;
    std::vector<std::string> partyTags_;
    std::vector<CompanionState> companions_;
    int selectedCompanion_ = -1;
    std::string leaderTag_ = "PC";
    PartyTable party_;
    PartySelectionState partySelection_;
    UiPanel activePanel_ = UiPanel::None;
    std::map<std::string, std::int32_t> journal_;
    bool newGameStart_ = false; // startNewGame: the next loadModule does not leave the backdrop
    std::map<std::string, std::string> globalKinds_; // KotOR GVT category
    std::map<std::string, std::array<float, 6>> locationGlobals_;
    std::string originalAreaName_;
    std::vector<std::uint8_t> saveScreenTga_;
    std::string originalPortrait_;
    std::int32_t originalTimePlayed_ = 0;
    std::vector<std::uint8_t> originalRepute_;
    std::vector<kotor::SavedResource> originalOuterExtras_;
    std::vector<kotor::SavedResource> originalPreservedModules_;
    const std::vector<std::uint8_t>* savedGit_ = nullptr; // loadOriginalSave: the save's GIT for the module being loaded
    std::int32_t gold_ = 0;
    std::vector<MerchantState> merchants_;
    std::vector<EncounterState> encounterStates_;
    int activeMerchant_ = -1;
    std::vector<InventoryItem> lootItems_;
    int lootEntity_ = -1;
    std::int32_t storeBonusMarkUp_ = 0;
    std::int32_t storeBonusMarkDown_ = 0;
    std::map<std::int32_t, float> itemCostScale_; // ChangeItemCost, by base item
    std::map<std::string, kotor::ItemTemplate> itemTemplates_;

    script::World scriptWorld_;
    std::unique_ptr<script::ActionRuntime> runtime_;
    std::unique_ptr<WorldSim> sim_;
    std::unique_ptr<nav::Runtime> nav_;
    MoveOutcome moveOutcome_ = MoveOutcome::None;
    std::size_t moveBlockingDoor_ = static_cast<std::size_t>(-1);
    bool quietWorldForTest_ = false; // The navigation gate: no triggers, scripts or combat.
    // Retail's load-complete fade (00602eb0 at 006031be) and its deferral (client +0xb98): see
    // GameSession::completeArrival.
    bool arrivalPending_ = false;
    bool deferredFadeIn_ = false;
    std::uint32_t dialogueGeneration_ = 0;
    std::vector<std::uint32_t> entityObjects_;
    std::map<std::string, std::vector<std::uint8_t>> ncsCache_;
    std::vector<std::string> scriptLog_;
    std::set<std::uint32_t> spawnRan_;
    std::set<std::string> missingScripts_;
    std::string pendingModule_;
    std::function<void(const std::string&)> moduleLoadHook_;
    StateTrace* stateTrace_ = nullptr;
    double heartbeatTimer_ = 0.0;
    std::uint32_t nextItemObject_ = 0x10000;
};

} // namespace ke
