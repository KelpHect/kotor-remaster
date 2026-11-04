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

    std::int32_t portraitIndex() const { return portraitIndex_; }
    PortraitInfo portrait() const;
    void cyclePortrait(std::int32_t delta);

    const std::string& name() const { return name_; }
    void setName(std::string name);

    CharacterSheet build() const;

private:
    static std::int32_t attributeCost(std::int32_t score);
    void refreshDerived(bool resetSelections);
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
    std::string name_ = "Player";
    CharacterSheet base_;
};

} // namespace ke
