#pragma once

#include "game/Rules.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ke {

// The body every chargen model wears: retail 006dc3c0 builds each classsel creature with the first short of
// its body struct set to 0x0102 (body variation 2, texture variation 1) before 006134c0 makes the model, and
// that creature is the one carried through the rest of chargen (portcust, MAINCG 006ea9f0 copy it unchanged).
// The same pair as g_a_clothes01's BodyVariation 2 / TextureVar 1: the class's own clothes, model column b.
inline kotor::BodyVariant chargenBodyVariant() { return kotor::BodyVariant{2, 1}; }

// Deterministic KotOR 1 character-generation state machine. The UI and the
// headless gate drive the same object; finishing it produces a CharacterSheet
// plus the portrait/appearance used by the player avatar.
class Chargen {
public:
    enum class Step { Class, Abilities, Skills, Feats, Portrait, Name, Done };

    explicit Chargen(const Rules& rules);

    void reset();
    // Scores back to 8 and no skills or feats, class, gender and portrait kept (retail 006eb010:
    // the hub going back to Quick or Custom, or the custom steps back to the first).
    void resetBuild();
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
    // The portrait row (portraits.2da) each classsel slot wears. Retail 006dc3c0 draws one at random per
    // slot, without repeats inside a gender, from the rows with ForPC == 1 and the slot's Sex; the portcust
    // panel then opens on that slot's portrait (006f90f0 marks the row equal to the creature's portrait id).
    // A fresh Chargen takes the first row of each gender; shuffleSlotPortraits is the retail draw.
    std::int32_t slotPortraitRow(int slot) const;
    // `seed` feeds the draw (retail seeds the CRT rand from GetTickCount, 005f8d68); 0 keeps the first rows.
    // `forced`, when it holds six rows, replaces the draw outright (the --chargen-portrait-rows option).
    void shuffleSlotPortraits(std::uint32_t seed, const std::vector<std::int32_t>& forced = {});
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
    const std::set<std::int32_t>& ownedFeats() const { return base_.feats; }
    const std::set<std::int32_t>& selectedFeats() const { return feats_; }
    std::vector<std::int32_t> availableFeats() const;
    bool toggleFeat(std::int32_t featId);
    // The Recommended button of the abilities, skills and feats steps: the class's own row of the shipped
    // tables (Rules::recommendedAbilities and the two recommend ranks). Skill ranks go to the highest priority
    // skill until it can take no more, then the next (retail 006491a0: by rank 1.., 00648d50 until false).
    bool applyRecommended(Step step);

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
    std::int32_t portraitIndexOfRow(std::int32_t row) const;
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
    std::array<std::int32_t, kSlotCount> slotPortrait_{{-1, -1, -1, -1, -1, -1}};
    std::vector<std::int32_t> portraitRows_;
    std::string name_ = "Player";
    CharacterSheet base_;
};

} // namespace ke
