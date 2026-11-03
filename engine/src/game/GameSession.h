#pragma once

#include "kotor/Formats.h"
#include "render/SceneView.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
// C++ kotor::Install/VFS; this class never calls an external tool.
class GameSession {
public:
    GameSession(std::string installPath, std::string module);

    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }
    const std::string& module() const { return module_; }
    const std::string& installPath() const { return installPath_; }
    const std::string& buildSha256() const { return install_.buildSha256(); }

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

    void attackNearestHostile();
    std::uint32_t combatTarget() const { return combatTarget_; }
    std::string combatTargetName() const;
    std::int32_t playerHp() const { return playerHp_; }
    std::int32_t playerMaxHp() const { return playerMaxHp_; }
    bool playerDead() const { return playerDead_; }
    const std::vector<std::string>& combatLog() const { return combatLog_; }

    // Deterministic helpers used by the scripted playthrough gate.
    bool walkTo(kotor::Vec3 target, float maxSeconds);
    std::size_t findEntity(const std::string& tag) const;
    void setPositionForTest(kotor::Vec3 position) { position_ = position; }
    void faceTowards(kotor::Vec3 target);

private:
    struct ActiveDialogue {
        std::string npcTag;
        std::string speaker;
        kotor::Dlg dlg;
    };

    bool loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor);
    const kotor::Entity* nearestDoor(float maxDistance) const;
    const kotor::Entity* nearestConversation(float maxDistance) const;
    bool loadDialogue(const std::string& resref, kotor::Dlg& dlg);
    void showDialogueEntry(std::uint32_t entryIndex);
    void updateCombat(float dt);
    void updateTriggers();
    void playerAttack(std::size_t target);
    void npcAttack(std::size_t attacker);
    void logEvent(std::string line);
    std::uint32_t random();
    bool pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const;

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
    std::vector<float> npcAttackTimers_;
    std::vector<bool> triggerInside_;
    std::vector<std::string> combatLog_;
    bool playerDead_ = false;
    std::uint32_t rng_ = 0x12345678u;
};

} // namespace ke
