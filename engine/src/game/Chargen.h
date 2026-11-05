#pragma once

#include "game/Rules.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ke {

// Deterministic KotOR 1 character-generation state machine. The UI and the
// headless gate drive the same object; finishing it produces a CharacterSheet
// plus the portrait/appearance used by the player avatar.
class Chargen {
public:
    enum class Step { Class, Abilities, Skills, Feats, Portrait, Name, Done };

    explicit Chargen(const Rules& rules);

    void reset();
    Step step() const { return step_; }
    bool next();
    bool back();
    void setStep(Step step) { step_ = step; }

    bool selectClass(std::int32_t classId);
    std::int32_t classId() const { return classId_; }

    // classsel's six buttons (BTN_SEL1..6): male Scoundrel, Scout, Soldier, then
    // female Soldier, Scout, Scoundrel. retail 006dc3c0 / initialiser 0073b690.
    static constexpr int kSlotCount = 6;
    static std::int32_t slotClass(int slot);
    static std::int32_t slotGender(int slot);
    bool selectSlot(int slot);
    int slot() const { return slot_; }
    std::int32_t gender() const { return gender_; }
    // Switching gender restarts the portrait list at its first entry.
    bool setGender(std::int32_t gender);
    std::int32_t classCount() const { return rules_.playableClassCount(); }

    std::int32_t pointsRemaining() const;
    std::int32_t attribute(std::int32_t index) const;
    bool adjustAttribute(std::int32_t index, std::int32_t delta);

    std::int32_t skillPointsRemaining() const;
    std::int32_t skillRank(std::int32_t index) const;
    bool adjustSkill(std::int32_t index, std::int32_t delta);

    std::int32_t featPointsRemaining() const { return featPool_ - static_cast<std::int32_t>(feats_.size()); }
    bool featSelected(std::int32_t featId) const { return feats_.contains(featId); }
    std::vector<std::int32_t> availableFeats() const;
    bool toggleFeat(std::int32_t featId);

    // Portraits offered to this gender (Sex == gender, ForPC == 1); the index is a
    // position in that list.
    std::int32_t portraitIndex() const { return portraitIndex_; }
    std::int32_t portraitListSize() const { return static_cast<std::int32_t>(portraitRows_.size()); }
    PortraitInfo portrait() const;
    void cyclePortrait(std::int32_t delta);
    // appearance.2da row for the chosen portrait and class (retail 006f90f0).
    std::int32_t appearanceId() const;
    std::int32_t skillRankCap(std::int32_t index) const;

    const std::string& name() const { return name_; }
    void setName(std::string name);

    CharacterSheet build() const;

private:
    static std::int32_t attributeCost(std::int32_t score);
    void refreshDerived(bool resetSelections);
    void refreshPortraits();
    void applyAttributes(CharacterSheet& sheet) const;

    const Rules& rules_;
    Step step_ = Step::Class;
    std::int32_t classId_ = 0;
    std::array<std::int32_t, 6> attributes_{{8, 8, 8, 8, 8, 8}};
    std::vector<std::int32_t> skills_;
    std::set<std::int32_t> feats_;
    std::int32_t skillPool_ = 0;
    std::int32_t featPool_ = 0;
    std::int32_t portraitIndex_ = 0;
    std::int32_t gender_ = 0;
    int slot_ = 2;
    std::vector<std::int32_t> portraitRows_;
    std::string name_ = "Player";
    CharacterSheet base_;
};

} // namespace ke
