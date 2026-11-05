#pragma once

#include "game/AudioSystem.h"
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
    std::int32_t camVidEffect = 0;
    float camHeightOffset = 0.0f;
    float tarHeightOffset = 0.0f;
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

enum class UiPanel { None, Character, Inventory, Party, Journal, Merchant, Loot };
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

class GameSession : public script::ActionHost {
    friend struct ScriptApi;
    friend struct ItemOps;
    friend struct CombatOps;

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
    SceneCamera camera() const;
    SceneCamera cinematicCamera(std::int32_t cameraId) const;
    SceneCamera presetCamera(std::int32_t angle, float camHeight, float targetHeight,
                             kotor::Vec3 speaker, kotor::Vec3 target) const;
    void setDialogueCameraForTest(std::int32_t cameraId) {
        dialogueView_.active = true;
        dialogueView_.cameraId = cameraId;
    }
    bool setDialogueCameraAnimationForTest(const std::string& model, const std::string& clip);
    std::string interactionPrompt() const;
    bool interact();
    bool testTransition(std::string& from, std::string& to);
    // Queues the module the next update() enters, through the same path as the
    // StartNewModule action (new game: chargen hands the character here).
    void requestModule(const std::string& module) { pendingModule_ = module; }
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);
    // Original KotOR save folder (SAVEGAME.sav + sidecars). The reader maps
    // the saved player/party/inventory/globals and overlays the saved GIT
    // object state; the writer emits a complete original slot.
    bool loadOriginalSave(const std::filesystem::path& folder);
    bool saveOriginalSave(const std::filesystem::path& folder,
                          const std::string& saveName = "Engine Save");
    const kotor::GameProfile& gameProfile() const { return install_.profile(); }

    kotor::Vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float elapsedSeconds() const { return elapsed_; }
    bool grounded() const { return grounded_; }
    float floorHeightAt(float x, float y) const;

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
    bool playMovie(const std::string& name);
    void stopMovie();
    bool queueMovie(const std::string& name, bool playNow);
    bool playMovieQueue();
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
    bool startPazaak(std::int32_t opponentDeck, std::string endScript,
                     std::int32_t maxWager, bool tutorial, std::uint32_t opponent);
    PazaakGame* pazaak() { return pazaak_.get(); }
    const PazaakGame* pazaak() const { return pazaak_.get(); }
    bool pazaakActive() const { return pazaak_ != nullptr; }
    int lastPazaakResult() const { return lastPazaakResult_; }
    void resolvePazaakIfOver();
    void stopPazaak() { pazaak_.reset(); pazaakEndScript_.clear(); pazaakWager_ = 0; }
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
        return character_.attributePoints > 0 || character_.skillPoints > 0 || character_.featPoints > 0;
    }
    void addPlayerClass(std::int32_t classId);
    AudioSystem* audioOrNull() { return audio_.get(); }
    bool setDialoguePlaceableCamera(std::int32_t cameraId);
    bool gameOverPanelVisible() const;
    bool creditsActive() const;
    std::int32_t creditsPageStrref() const; // -1 when no credits are showing.
    float creditsOpacity() const;
    bool creditsTransparent() const;
    void skipCredits();
    // A tutorial window a script asked for, or -1; dismiss clears it.
    std::int32_t pendingTutorialWindow() const;
    void dismissTutorialWindow();
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
    bool galaxyMapOpen() const { return sim_->galaxy.open; }
    // The UI/bot picks a planet: it must be available and selectable. Runs the
    // engine's galaxy map script and closes the map.
    bool selectPlanet(std::int32_t planet);
    void closeGalaxyMap() { sim_->galaxy.open = false; }
    std::string substituteTokens(std::string text) const;
    std::vector<std::pair<std::int32_t, std::string>> galaxyPlanetNames();
    bool mapPinEnabled(const std::string& waypointTag) const;
    bool mapRevealed(kotor::Vec3 point) const;
    bool areaUnescapable() const { return sim_->areaUnescapable; }

    void attackNearestHostile();
    std::uint32_t combatTarget() const { return combatTarget_; }
    std::string combatTargetName() const;
    std::int32_t playerHp() const { return playerHp_; }
    std::int32_t playerMaxHp() const { return playerMaxHp_; }
    bool playerDead() const { return playerDead_; }
    const std::vector<std::string>& combatLog() const { return combatLog_; }
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
    // The item object standing for character_.inventory[index].
    std::uint32_t inventoryObject(std::size_t index);
    bool equipItemObject(std::uint32_t creature, std::uint32_t item, std::int32_t slot = -1);
    bool unequipItemObject(std::uint32_t creature, std::uint32_t item);
    // Uses an item as `user`: applies its CastSpell item properties through the spell's
    // impact script, raises the module activation event and spends a charge or one of
    // the stack. The quest bot and the UI both call this.
    bool useItem(std::uint32_t item, std::uint32_t user,
                 std::uint32_t target = script::kObjectInvalid);
    // Gold held by a creature (the party's for the PC and party members).
    std::int32_t creatureGold(std::uint32_t creature) const;
    void addCreatureGold(std::uint32_t creature, std::int32_t amount);
    bool addPartyMember(const std::string& tag);
    bool removePartyMember(const std::string& tag);
    bool learnFeat(std::int32_t featId);
    bool learnPower(std::int32_t powerId);
    bool spendSkillPoint(const std::string& skill);
    bool spendAttributePoint(std::int32_t index);
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
    void stopAnimation(std::size_t entityIndex);
    std::string animationName(std::size_t entityIndex) const;
    float animationTime(std::size_t entityIndex) const;
    std::uint32_t animationRevision() const { return animationRevision_; }
    bool entityHasFacialPose(std::size_t entityIndex) const;
    void initAnimations();
    void updateAnimations(float dt);
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
    };

    bool loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor);
    const kotor::Entity* nearestDoor(float maxDistance) const;
    const kotor::Entity* nearestConversation(float maxDistance) const;
    bool loadDialogue(const std::string& resref, kotor::Dlg& dlg, int speakerGender = 0);
    void showDialogueEntry(std::uint32_t entryIndex);
    bool startConversation(std::uint32_t owner, const std::string& resref);
    void finishDialogue(bool aborted);
    void clearDialogueState();
    ScriptContext dialogueContext() const;
    bool dialogueLinkPasses(const std::string& linkActive, const std::string& nodeActive);
    template <typename Node>
    bool applyDialogueNode(const Node& node, std::uint32_t generation);
    std::vector<std::uint32_t> passingReplies(const kotor::DlgEntry& entry);
    std::uint32_t firstPassingEntry(const kotor::DlgReply& reply);
    void loadJournalData();
    void loadPlotTable();
    const JournalCategory* journalCategory(const std::string& plotId);
    std::int32_t plotXp(std::int32_t row, double fraction);
    std::int32_t plotXpByLabel(const std::string& label, double fraction);
    ActorStep stepActorToward(std::uint32_t object, kotor::Vec3 target, float speed, float dt,
                              float stopDistance);
    bool lineOfSight(kotor::Vec3 from, kotor::Vec3 to) const;
    std::shared_ptr<const std::vector<script::Instruction>> decodedScript(const std::string& resref);
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
    float creatureRadius(std::uint32_t object);
    float creatureApproachRange(std::uint32_t self, std::uint32_t target, float range);
    kotor::Vec3 slidePlayer(kotor::Vec3 from, kotor::Vec3 to, bool& moved, float radius = -1.0f);
    kotor::Vec3 formationSlot(kotor::Vec3 anchor, float anchorYaw, int slot, float baseDistance);
    void followLeader(std::uint32_t follower, kotor::Vec3 anchor, kotor::Vec3 slot, float dt);
    void placePartyAtEntry();
    friend int runPathfindingFamilyTest(GameSession& game);
    friend int runCompanionFamilyTest(GameSession& game);
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
    void syncPlayerToScriptWorld();
    void spawnPlayerAvatar();
    void updatePlayerAvatar(float dt, bool moving);
    void updateFacialMeshes();
    void syncEntityToScriptWorld(std::size_t index);
    std::string scriptSource(const std::string& resref);

    std::string installPath_;
    std::filesystem::path rsvRoot_;
    std::string module_;
    kotor::Install install_;
    std::unique_ptr<kotor::Localization> localization_;
    std::uint32_t languageId_ = 0;
    std::unique_ptr<AudioSystem> audio_;
    kotor::Scene scene_;
    kotor::Vec3 position_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    std::size_t playerEntity_ = static_cast<std::size_t>(-1);
    std::uint32_t playerAppearance_ = 92;
    std::string playerName_ = "Player";
    float playerBearing_ = 0.0f;
    bool thirdPerson_ = false;
    mutable bool playerVisible_ = true;
    bool cameraNoClip_ = false;
    float verticalVelocity_ = 0.0f;
    bool grounded_ = false;
    float elapsed_ = 0.0f;

    std::optional<kotor::Tlk> tlk_;
    std::optional<ActiveDialogue> activeDialogue_;
    DialogueView dialogueView_;
    VideoEffectController videoEffects_;
    std::shared_ptr<kotor::Mdl> dialogueCameraModel_;
    std::string dialogueCameraClip_;
    float dialogueCameraTime_ = 0.0f;
    float dialogueCameraDuration_ = 0.0f;
    std::optional<kotor::LipSync> activeLip_;
    float lipTime_ = 0.0f;
    FacialRig facialRig_;
    FactionSystem factions_;
    bool moviePlaying_ = false;
    std::string movieName_;
    float movieTime_ = 0.0f;
    float movieDuration_ = 0.0f;
    std::vector<std::string> movieQueue_;
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

    std::int32_t playerHp_ = 100;
    std::int32_t playerMaxHp_ = 100;
    std::uint32_t combatTarget_ = UINT32_MAX;
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
    std::vector<std::string> combatLog_;
    bool playerDead_ = false;
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
    std::map<std::string, std::string> globalKinds_; // KotOR GVT category
    std::map<std::string, std::array<float, 6>> locationGlobals_;
    std::string originalAreaName_;
    std::string originalPortrait_;
    std::int32_t originalTimePlayed_ = 0;
    std::vector<std::uint8_t> originalRepute_;
    std::vector<kotor::SavedResource> originalOuterExtras_;
    std::vector<kotor::SavedResource> originalPreservedModules_;
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
    std::uint32_t dialogueGeneration_ = 0;
    std::vector<std::uint32_t> entityObjects_;
    std::map<std::string, std::vector<std::uint8_t>> ncsCache_;
    std::vector<std::string> scriptLog_;
    std::set<std::uint32_t> spawnRan_;
    std::set<std::string> missingScripts_;
    std::string pendingModule_;
    double heartbeatTimer_ = 0.0;
    std::uint32_t nextItemObject_ = 0x10000;
};

} // namespace ke
