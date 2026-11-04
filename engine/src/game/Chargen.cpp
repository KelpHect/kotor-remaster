#include "game/Chargen.h"

#include <algorithm>
#include <cctype>

namespace ke {

namespace {

constexpr std::int32_t kMinAttribute = 8;
constexpr std::int32_t kMaxAttribute = 18;
constexpr std::int32_t kAttributePool = 30;
constexpr std::int32_t kMaxSkillRank = 4; // level 1 + 3

} // namespace

Chargen::Chargen(const Rules& rules)
    : rules_(rules)
{
    reset();
}

void Chargen::reset()
{
    step_ = Step::Class;
    classId_ = 0;
    attributes_ = {8, 8, 8, 8, 8, 8};
    portraitIndex_ = 0;
    name_ = "Player";
    refreshDerived(true);
}

void Chargen::refreshDerived(bool resetSelections)
{
    base_ = rules_.createCharacter(classId_, 1);
    applyAttributes(base_);
    base_.maxHp = rules_.maxHitPoints(classId_, 1, attributes_[2]);
    base_.hp = base_.maxHp;
    base_.maxFp = rules_.maxForcePoints(classId_, 1, attributes_[4], attributes_[5]);
    base_.fp = base_.maxFp;
    skillPool_ = rules_.skillPointsForLevel(classId_, attributes_[3]);
    featPool_ = std::max<std::int32_t>(1, rules_.featsForLevel(classId_, 1));
    if (resetSelections) {
        skills_.assign(static_cast<std::size_t>(rules_.skillCount()), 0);
        feats_.clear();
    } else {
        skills_.resize(static_cast<std::size_t>(rules_.skillCount()), 0);
        for (auto it = feats_.begin(); it != feats_.end();) {
            if (!rules_.canTakeFeat(base_, *it)) {
                it = feats_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Chargen::applyAttributes(CharacterSheet& sheet) const
{
    sheet.str = attributes_[0];
    sheet.dex = attributes_[1];
    sheet.con = attributes_[2];
    sheet.intel = attributes_[3];
    sheet.wis = attributes_[4];
    sheet.cha = attributes_[5];
}

std::int32_t Chargen::attributeCost(std::int32_t score)
{
    if (score <= 14) {
        return score - kMinAttribute;
    }
    if (score <= 16) {
        return 6 + (score - 14) * 2;
    }
    return 10 + (score - 16) * 3;
}

std::int32_t Chargen::pointsRemaining() const
{
    std::int32_t spent = 0;
    for (const std::int32_t value : attributes_) {
        spent += attributeCost(value);
    }
    return kAttributePool - spent;
}

std::int32_t Chargen::attribute(std::int32_t index) const
{
    if (index < 0 || index >= static_cast<std::int32_t>(attributes_.size())) {
        return kMinAttribute;
    }
    return attributes_[static_cast<std::size_t>(index)];
}

bool Chargen::adjustAttribute(std::int32_t index, std::int32_t delta)
{
    if (index < 0 || index >= static_cast<std::int32_t>(attributes_.size())
        || (delta != 1 && delta != -1)) {
        return false;
    }
    const std::int32_t current = attributes_[static_cast<std::size_t>(index)];
    const std::int32_t next = current + delta;
    if (next < kMinAttribute || next > kMaxAttribute) {
        return false;
    }
    const std::int32_t cost = attributeCost(next) - attributeCost(current);
    if (cost > 0 && cost > pointsRemaining()) {
        return false;
    }
    attributes_[static_cast<std::size_t>(index)] = next;
    refreshDerived(false);
    return true;
}

bool Chargen::selectClass(std::int32_t classId)
{
    if (classId < 0 || classId >= rules_.playableClassCount()) {
        return false;
    }
    classId_ = classId;
    attributes_ = {8, 8, 8, 8, 8, 8};
    refreshDerived(true);
    return true;
}

std::int32_t Chargen::skillPointsRemaining() const
{
    std::int32_t spent = 0;
    for (std::size_t i = 0; i < skills_.size(); ++i) {
        const std::int32_t cost = rules_.skillIsClassSkill(classId_, static_cast<std::int32_t>(i))
            ? 1 : 2;
        spent += skills_[i] * cost;
    }
    return skillPool_ - spent;
}

std::int32_t Chargen::skillRank(std::int32_t index) const
{
    if (index < 0 || index >= static_cast<std::int32_t>(skills_.size())) {
        return 0;
    }
    return skills_[static_cast<std::size_t>(index)];
}

bool Chargen::adjustSkill(std::int32_t index, std::int32_t delta)
{
    if (index < 0 || index >= static_cast<std::int32_t>(skills_.size())
        || (delta != 1 && delta != -1)) {
        return false;
    }
    const std::int32_t cost = rules_.skillIsClassSkill(classId_, index) ? 1 : 2;
    std::int32_t& rank = skills_[static_cast<std::size_t>(index)];
    if (delta > 0) {
        if (rank >= kMaxSkillRank || skillPointsRemaining() < cost) {
            return false;
        }
        ++rank;
    } else {
        if (rank <= 0) {
            return false;
        }
        --rank;
    }
    return true;
}

std::vector<std::int32_t> Chargen::availableFeats() const
{
    std::vector<std::int32_t> result;
    for (std::int32_t id = 0; id < rules_.featCount(); ++id) {
        if (base_.feats.contains(id) || feats_.contains(id)) {
            continue;
        }
        if (!rules_.featIsAvailable(classId_, id)) {
            continue;
        }
        if (!rules_.canTakeFeat(base_, id)) {
            continue;
        }
        result.push_back(id);
    }
    return result;
}

bool Chargen::toggleFeat(std::int32_t featId)
{
    if (feats_.contains(featId)) {
        feats_.erase(featId);
        return true;
    }
    if (featPointsRemaining() <= 0 || base_.feats.contains(featId)) {
        return false;
    }
    const std::vector<std::int32_t> available = availableFeats();
    if (std::find(available.begin(), available.end(), featId) == available.end()) {
        return false;
    }
    feats_.insert(featId);
    return true;
}

PortraitInfo Chargen::portrait() const
{
    return rules_.portrait(portraitIndex_);
}

void Chargen::cyclePortrait(std::int32_t delta)
{
    const std::int32_t count = rules_.portraitCount();
    if (count <= 0) {
        portraitIndex_ = 0;
        return;
    }
    portraitIndex_ = (portraitIndex_ + delta) % count;
    if (portraitIndex_ < 0) {
        portraitIndex_ += count;
    }
}

void Chargen::setName(std::string name)
{
    for (char& ch : name) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (name.size() > 16) {
        name.resize(16);
    }
    name_ = name.empty() ? "Player" : std::move(name);
}

CharacterSheet Chargen::build() const
{
    CharacterSheet sheet = base_;
    applyAttributes(sheet);
    sheet.name = name_;
    sheet.maxHp = rules_.maxHitPoints(classId_, 1, attributes_[2]);
    sheet.hp = sheet.maxHp;
    sheet.maxFp = rules_.maxForcePoints(classId_, 1, attributes_[4], attributes_[5]);
    sheet.fp = sheet.maxFp;
    sheet.skillPoints = 0;
    sheet.featPoints = 0;
    sheet.gold = 0;
    sheet.alignment = 50;
    sheet.skills.clear();
    for (std::int32_t i = 0; i < rules_.skillCount(); ++i) {
        sheet.skills[rules_.skillName(i)] = skills_[static_cast<std::size_t>(i)];
    }
    sheet.feats.insert(feats_.begin(), feats_.end());
    return sheet;
}

bool Chargen::next()
{
    switch (step_) {
    case Step::Class: step_ = Step::Abilities; break;
    case Step::Abilities: step_ = Step::Skills; break;
    case Step::Skills: step_ = Step::Feats; break;
    case Step::Feats: step_ = Step::Portrait; break;
    case Step::Portrait: step_ = Step::Name; break;
    case Step::Name: step_ = Step::Done; break;
    case Step::Done: return false;
    }
    return true;
}

bool Chargen::back()
{
    switch (step_) {
    case Step::Class: return false;
    case Step::Abilities: step_ = Step::Class; break;
    case Step::Skills: step_ = Step::Abilities; break;
    case Step::Feats: step_ = Step::Skills; break;
    case Step::Portrait: step_ = Step::Feats; break;
    case Step::Name: step_ = Step::Portrait; break;
    case Step::Done: step_ = Step::Name; break;
    }
    return true;
}

} // namespace ke
