#pragma once

#include "game/AudioSystem.h"
#include "game/FacialRig.h"
#include "game/Factions.h"
#include "game/Minigames.h"
#include "game/Rules.h"
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
class GameSession : public script::ActionHost {
public:
    GameSession(std::string installPath, std::string module, int language = -1,
                bool traceActions = false);
    ~GameSession() override;

    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }
    const std::string& module() const { return module_; }
    const std::string& installPath() const { return installPath_; }
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
    void startSwoop();
    void startTurret();
    void updateSwoop(float dt, float throttle, bool boost, bool brake);
    void updateTurret(float dt);
    bool swoopActive() const { return swoopActive_; }
    bool turretActive() const { return turretActive_; }
    SwoopRace& swoop() { return swoop_; }
    const SwoopRace& swoop() const { return swoop_; }
    TurretGame& turret() { return turret_; }
    const TurretGame& turret() const { return turret_; }
    void stopSwoop() { swoopActive_ = false; }
    void stopTurret() { turretActive_ = false; }
    void chooseDialogueOption(std::size_t optionIndex);
    void endDialogue();
    const DialogueView& dialogue() const { return dialogueView_; }
    int unsupportedCamVidEffects() const { return unsupportedCamVidEffects_; }
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
    void closeStore() { activeMerchant_ = -1; }
    std::int32_t buyPrice(std::size_t stockIndex) const;
    std::int32_t sellPrice(std::size_t inventoryIndex) const;
    bool buyMerchantItem(std::size_t stockIndex);
    bool sellInventoryItem(std::size_t inventoryIndex);
    bool identifyInventoryItem(std::size_t inventoryIndex);
    bool openLoot(std::size_t entityIndex);
    const std::vector<InventoryItem>& loot() const { return lootItems_; }
    bool takeLootItem(std::size_t index);
    void takeAllLoot();
    void closeLoot() { lootItems_.clear(); lootEntity_ = -1; }
    const std::vector<std::string>& partyTags() const { return partyTags_; }
    const std::vector<CompanionState>& companions() const { return companions_; }
    int selectedCompanion() const { return selectedCompanion_; }
    void selectCompanion(int index) { selectedCompanion_ = index; if (index >= 0 && index < static_cast<int>(companions_.size())) { leaderTag_ = companions_[static_cast<std::size_t>(index)].tag; } }
    const std::string& partyLeaderTag() const { return leaderTag_; }
    void setPartyLeader(const std::string& tag) { leaderTag_ = tag; selectedCompanion_ = -1; if (tag != "PC") { for (std::size_t i = 0; i < companions_.size(); ++i) { if (companions_[i].tag == tag) { selectedCompanion_ = static_cast<int>(i); break; } } } }
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
    bool addInventoryItem(const std::string& resref, std::int32_t baseItem,
                          std::int32_t stack = 1, std::int32_t cost = 0);
    bool removeInventoryItem(const std::string& resref, std::int32_t stack = 1);
    bool equipItem(const std::string& resref);
    bool addPartyMember(const std::string& tag);
    bool removePartyMember(const std::string& tag);
    bool learnFeat(std::int32_t featId);
    bool learnPower(std::int32_t powerId);
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
    std::uint32_t objectForTag(const std::string& tag) const;
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
    void attackPlayerForTest(std::size_t index) { npcAttack(index); }
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
        std::set<std::uint32_t> firedEntries;
    };

    bool loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor);
    const kotor::Entity* nearestDoor(float maxDistance) const;
    const kotor::Entity* nearestConversation(float maxDistance) const;
    bool loadDialogue(const std::string& resref, kotor::Dlg& dlg);
    void showDialogueEntry(std::uint32_t entryIndex);
    bool loadDialogueCameraModel(const std::string& resref);
    void setDialogueCameraClip(std::int32_t animation);
    void updateCombat(float dt);
    void updateTriggers();
    void updateScripts(float dt);
    void updateTimers(float dt);
    bool runFunctor(const script::Functor& functor, std::uint32_t target);
    void playerAttack(std::size_t target);
    void npcAttack(std::size_t attacker);
    void logEvent(std::string line);
    std::uint32_t random();
    bool pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const;

    void initScriptWorld();
    void syncScriptWorld();
    void refreshHostility();
    void initMerchants();
    void spawnEncounters();
    std::int32_t spawnEncounterCreatures(std::size_t index);
    void updateEncounters(float dt);
    void updateParty(float dt);
    std::uint32_t objectForEntity(std::size_t index) const;
    void runHook(const std::string& hook, std::size_t entityIndex,
                 std::uint32_t entering = 0);
    void runSceneHook(const std::string& hook, std::uint32_t entering = 0);
    void runSpawnAndEnterScripts();
    bool scriptValueTruthy(const std::optional<script::Value>& value) const;
    void syncPlayerToScriptWorld();
    void spawnPlayerAvatar();
    void updatePlayerAvatar(float dt, bool moving);
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
    int unsupportedCamVidEffects_ = 0;
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
    SwoopRace swoop_;
    TurretGame turret_;
    bool swoopActive_ = false;
    bool turretActive_ = false;
    std::string pazaakEndScript_;
    std::uint32_t pazaakOpponent_ = 0;
    std::int32_t pazaakWager_ = 0;
    int lastPazaakResult_ = -1;

    std::int32_t playerHp_ = 100;
    std::int32_t playerMaxHp_ = 100;
    std::uint32_t combatTarget_ = UINT32_MAX;
    float playerAttackTimer_ = 0.0f;
    float combatRoundTimer_ = 0.0f;
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
    std::vector<float> npcAttackTimers_;
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
    std::map<std::string, kotor::ItemTemplate> itemTemplates_;

    script::World scriptWorld_;
    std::unique_ptr<script::ActionRuntime> runtime_;
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
