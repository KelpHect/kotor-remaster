#pragma once

#include "game/Rules.h"
#include "kotor/Formats.h"
#include "render/SceneView.h"
#include "script/ActionRuntime.h"
#include "script/World.h"

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
};

// One loaded module plus the player state. All KotOR data comes through the
// C++ kotor::Install/VFS; this class never calls an external tool. Scripts run
// on the engine's own NCS VM and mutate the same world the renderer uses.
class GameSession : public script::ActionHost {
public:
    GameSession(std::string installPath, std::string module);
    ~GameSession() override;

    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }
    const std::string& module() const { return module_; }
    const std::string& installPath() const { return installPath_; }
    const std::string& buildSha256() const { return install_.buildSha256(); }
    const Rules& rules() const { return rules_; }

    void update(float dt, const InputState& input);
    SceneCamera camera() const;
    std::string interactionPrompt() const;
    bool interact();
    bool testTransition(std::string& from, std::string& to);
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);

    kotor::Vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float elapsedSeconds() const { return elapsed_; }
    bool grounded() const { return grounded_; }
    float floorHeightAt(float x, float y) const;

    bool startDialogue(std::size_t entityIndex);
    bool startDialogue(const std::string& tag);
    void chooseDialogueOption(std::size_t optionIndex);
    void endDialogue();
    const DialogueView& dialogue() const { return dialogueView_; }
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
    const std::vector<std::string>& partyTags() const { return partyTags_; }
    std::int32_t journalState(const std::string& plotId) const;
    std::size_t journalSize() const { return journal_.size(); }
    bool addJournalEntry(const std::string& plotId, std::int32_t state);
    bool removeJournalEntry(const std::string& plotId);
    bool addInventoryItem(const std::string& resref, std::int32_t baseItem,
                          std::int32_t stack = 1);
    bool removeInventoryItem(const std::string& resref, std::int32_t stack = 1);
    bool equipItem(const std::string& resref);
    bool addPartyMember(const std::string& tag);
    bool removePartyMember(const std::string& tag);
    void giveXp(std::int32_t amount);

    // Run one shipped script with optional event arguments. Returns the top of
    // stack (the condition value) when the script returns a value.
    std::optional<script::Value> runScript(const std::string& resref,
                                           std::uint32_t self = 0,
                                           std::uint32_t entering = 0,
                                           std::uint32_t caller = 0,
                                           std::uint32_t trigger = 0);
    std::uint32_t objectForTag(const std::string& tag) const;

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
    void updateCombat(float dt);
    void updateTriggers();
    void updateScripts(float dt);
    void playerAttack(std::size_t target);
    void npcAttack(std::size_t attacker);
    void logEvent(std::string line);
    std::uint32_t random();
    bool pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const;

    void initScriptWorld();
    void syncScriptWorld();
    std::uint32_t objectForEntity(std::size_t index) const;
    void runHook(const std::string& hook, std::size_t entityIndex,
                 std::uint32_t entering = 0);
    void runSceneHook(const std::string& hook, std::uint32_t entering = 0);
    void runSpawnAndEnterScripts();
    bool scriptValueTruthy(const std::optional<script::Value>& value) const;
    void syncPlayerToScriptWorld();
    void syncEntityToScriptWorld(std::size_t index);
    std::string scriptSource(const std::string& resref);

    std::string installPath_;
    std::string module_;
    kotor::Install install_;
    kotor::Scene scene_;
    kotor::Vec3 position_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float verticalVelocity_ = 0.0f;
    bool grounded_ = false;
    float elapsed_ = 0.0f;

    std::optional<kotor::Tlk> tlk_;
    std::optional<ActiveDialogue> activeDialogue_;
    DialogueView dialogueView_;

    std::int32_t playerHp_ = 100;
    std::int32_t playerMaxHp_ = 100;
    std::uint32_t combatTarget_ = UINT32_MAX;
    float playerAttackTimer_ = 0.0f;
    float combatRoundTimer_ = 0.0f;
    int combatRound_ = 0;
    std::vector<float> npcAttackTimers_;
    std::vector<bool> triggerInside_;
    std::vector<std::string> combatLog_;
    bool playerDead_ = false;
    std::uint32_t rng_ = 0x12345678u;

    Rules rules_;
    CharacterSheet character_;
    std::vector<std::string> partyTags_;
    std::map<std::string, std::int32_t> journal_;
    std::int32_t gold_ = 0;

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
