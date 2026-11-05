#include "game/Rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

int abilityModifier(std::int32_t score)
{
    return static_cast<int>(std::floor((static_cast<double>(score) - 10.0) / 2.0));
}

std::uint32_t nextRandom(std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int rollDie(std::uint32_t& state, int sides)
{
    if (sides <= 1) {
        return 1;
    }
    return 1 + static_cast<int>(nextRandom(state) % static_cast<std::uint32_t>(sides));
}

} // namespace

Rules::Rules(kotor::Install& install)
{
    const auto loadTable = [&install](const char* name) -> std::optional<kotor::TwoDa> {
        auto data = install.find(name, 2017);
        if (!data.has_value()) {
            return std::nullopt;
        }
        try {
            return kotor::TwoDa::parse(std::move(*data), name);
        } catch (const kotor::FormatError&) {
            return std::nullopt;
        }
    };
    classes_ = loadTable("classes");
    exptable_ = loadTable("exptable");
    skills_ = loadTable("skills");
    feat_ = loadTable("feat");
    portraits_ = loadTable("portraits");
    baseitems_ = loadTable("baseitems");
    featgain_ = loadTable("featgain");
    spells_ = loadTable("spells");
    animations_ = loadTable("animations");
    xptable_ = loadTable("xptable");
    npc_ = loadTable("npc");
    ranges_ = loadTable("ranges");
    acbonus_ = loadTable("acbonus");
    difficultyopt_ = loadTable("difficultyopt");
    costTables_ = loadTable("iprp_costtable");
    install_ = &install;
    if (!classes_ || !exptable_ || !baseitems_) {
        error_ = "missing classes/exptable/baseitems 2DA";
        return;
    }
    for (std::size_t row = 0; row < classes_->rowCount(); ++row) {
        const std::string attack = lower(classes_->cell(std::to_string(row), "attackbonustable"));
        if (!attack.empty() && !attackTables_.contains(attack)) {
            if (auto table = loadTable(attack.c_str()); table.has_value()) {
                attackTables_[attack] = std::move(*table);
            }
        }
        const std::string save = lower(classes_->cell(std::to_string(row), "savingthrowtable"));
        if (!save.empty() && !saveTables_.contains(save)) {
            if (auto table = loadTable(save.c_str()); table.has_value()) {
                saveTables_[save] = std::move(*table);
            }
        }
    }
    loaded_ = true;
}

std::int32_t Rules::cellInt(const kotor::TwoDa& table, const std::string& row,
                            const std::string& column, std::int32_t fallback) const
{
    const std::string text = table.cell(row, column);
    if (text.empty() || text == "****") {
        return fallback;
    }
    try {
        const bool hex = text.size() > 2 && text[0] == '0'
            && (text[1] == 'x' || text[1] == 'X');
        const long value = std::stol(text, nullptr, hex ? 16 : 10);
        if (value > std::numeric_limits<std::int32_t>::max()
            || value < std::numeric_limits<std::int32_t>::min()) {
            return fallback;
        }
        return static_cast<std::int32_t>(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

CharacterSheet Rules::createCharacter(std::int32_t classId, std::int32_t level,
                                      std::int32_t xp) const
{
    CharacterSheet sheet;
    sheet.classId = classId;
    sheet.level = std::max(1, std::min(level, 20));
    sheet.xp = std::max(0, xp);
    if (classes_ && classId >= 0
        && classId < static_cast<std::int32_t>(classes_->rowCount())) {
        const std::string row = std::to_string(classId);
        sheet.str = cellInt(*classes_, row, "str", 10);
        sheet.dex = cellInt(*classes_, row, "dex", 10);
        sheet.con = cellInt(*classes_, row, "con", 10);
        sheet.intel = cellInt(*classes_, row, "int", 10);
        sheet.wis = cellInt(*classes_, row, "wis", 10);
        sheet.cha = cellInt(*classes_, row, "cha", 10);
        sheet.name = classes_->cell(row, "label");
    }
    switch (classId) {
    case 0: // Soldier
        sheet.feats = {4, 5, 6, 39, 40, 42, 44};
        break;
    case 1: // Scout
        sheet.feats = {5, 39, 40, 44};
        break;
    case 2: // Scoundrel
        sheet.feats = {5, 39, 44};
        break;
    case 3:
    case 4:
    case 5: // Jedi Guardian/Consular/Sentinel
        sheet.feats = {5, 43, 44, 55, 116};
        break;
    default:
        break;
    }
    sheet.maxHp = maxHitPoints(sheet.classId, sheet.level, sheet.con);
    sheet.hp = sheet.maxHp;
    sheet.alignment = 50;
    sheet.maxFp = maxForcePoints(sheet.classId, sheet.level, sheet.wis, sheet.cha);
    sheet.fp = sheet.maxFp;
    if (skills_) {
        for (std::size_t row = 0; row < skills_->rowCount(); ++row) {
            sheet.skills[skills_->rowLabel(row)] = 0;
        }
    }
    sheet.skillPoints = 0;
    sheet.featPoints = 0;
    for (std::int32_t current = 1; current <= sheet.level; ++current) {
        sheet.skillPoints += skillPointsForLevel(sheet.classId, sheet.intel);
        sheet.featPoints += featsForLevel(sheet.classId, current);
    }
    return sheet;
}

std::int32_t Rules::levelForXp(std::int32_t xp) const
{
    if (!exptable_ || xp <= 0) {
        return 1;
    }
    std::int32_t level = 1;
    for (std::size_t row = 0; row < exptable_->rowCount(); ++row) {
        const std::int32_t threshold = cellInt(*exptable_, exptable_->rowLabel(row), "xp", -1);
        if (threshold >= 0 && xp >= threshold) {
            level = std::max(level, cellInt(*exptable_, exptable_->rowLabel(row), "level",
                                            static_cast<std::int32_t>(row + 1)));
        }
    }
    return level;
}

std::int32_t Rules::xpForLevel(std::int32_t level) const
{
    if (!exptable_) {
        return 0;
    }
    for (std::size_t row = 0; row < exptable_->rowCount(); ++row) {
        if (cellInt(*exptable_, exptable_->rowLabel(row), "level", -1) == level) {
            return cellInt(*exptable_, exptable_->rowLabel(row), "xp", 0);
        }
    }
    return 0;
}

std::int32_t Rules::baseAttackBonus(std::int32_t classId, std::int32_t level) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 0;
    }
    const std::string tableName = lower(classes_->cell(std::to_string(classId), "attackbonustable"));
    const auto it = attackTables_.find(tableName);
    if (it == attackTables_.end()) {
        return level;
    }
    return cellInt(it->second, std::to_string(std::max(1, std::min(level, 20)) - 1), "bab", 0);
}

std::int32_t Rules::savingThrow(std::int32_t classId, std::int32_t level, int which) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 0;
    }
    const std::string tableName = lower(classes_->cell(std::to_string(classId), "savingthrowtable"));
    const auto it = saveTables_.find(tableName);
    if (it == saveTables_.end()) {
        return 0;
    }
    const char* column = which == 0 ? "fortsave" : which == 1 ? "refsave" : "willsave";
    return cellInt(it->second, std::to_string(std::max(1, std::min(level, 20)) - 1),
                   column, 0);
}

std::int32_t Rules::savingThrow(const CharacterSheet& sheet, int which) const
{
    std::int32_t bonus = savingThrow(sheet.classId, sheet.level, which);
    if (hasFeatLabel(sheet, "MASTER_CONDITIONING")) {
        bonus += 3;
    } else if (hasFeatLabel(sheet, "IMPROVED_CONDITIONING")) {
        bonus += 2;
    } else if (hasFeatLabel(sheet, "CONDITIONING")) {
        bonus += 1;
    }
    return bonus;
}

std::int32_t Rules::maxHitPoints(std::int32_t classId, std::int32_t level,
                                 std::int32_t constitution) const
{
    const std::int32_t hitDie = classHitDie(classId);
    const std::int32_t modifier = static_cast<std::int32_t>(abilityModifier(constitution));
    return std::max<std::int32_t>(1, level * std::max<std::int32_t>(1, hitDie + modifier));
}

std::int32_t Rules::skillPointsForLevel(std::int32_t classId, std::int32_t intelligence) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 1;
    }
    const std::int32_t base = cellInt(*classes_, std::to_string(classId), "skillpointbase", 1);
    return std::max<std::int32_t>(1, base + static_cast<std::int32_t>(abilityModifier(intelligence)));
}

std::int32_t Rules::featsForLevel(std::int32_t classId, std::int32_t level) const
{
    if (!featgain_) {
        return 0;
    }
    static const char* columns[] = {"sol_reg", "sct_reg", "scd_reg", "jgd_reg", "jsn_reg", "jcn_reg"};
    if (classId < 0 || classId >= 6) {
        return 0;
    }
    const int row = featgain_->rowIndex(std::to_string(std::max(1, std::min(level, 20)) - 1));
    if (row < 0) {
        return 0;
    }
    return cellInt(*featgain_, featgain_->rowLabel(static_cast<std::size_t>(row)),
                   columns[classId], 0);
}

bool Rules::canTakeFeat(const CharacterSheet& sheet, std::int32_t featId) const
{
    if (!feat_) {
        return false;
    }
    const std::string row = std::to_string(featId);
    if (feat_->rowIndex(row) < 0 || cellInt(*feat_, row, "mincharlevel", 0) > sheet.level) {
        return false;
    }
    if (cellInt(*feat_, row, "minstr", 0) > sheet.str
        || cellInt(*feat_, row, "mindex", 0) > sheet.dex
        || cellInt(*feat_, row, "minint", 0) > sheet.intel
        || cellInt(*feat_, row, "minwis", 0) > sheet.wis) {
        return false;
    }
    for (const char* column : {"prereqfeat1", "prereqfeat2"}) {
        const std::int32_t required = cellInt(*feat_, row, column, -1);
        if (required >= 0 && !sheet.feats.contains(required)) {
            return false;
        }
    }
    return true;
}

bool Rules::canTakePower(const CharacterSheet& sheet, std::int32_t powerId) const
{
    if (!spells_) {
        return false;
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0 || spells_->cell(row, "name").empty()
        || cellInt(*spells_, row, "usertype", 0) != 1) {
        return false;
    }
    const std::int32_t prerequisite = cellInt(*spells_, row, "prerequisites", -1);
    if (prerequisite >= 0 && !sheet.powers.contains(prerequisite)) {
        return false;
    }
    const std::int32_t master = cellInt(*spells_, row, "masterspell", -1);
    if (master >= 0 && !sheet.powers.contains(master)) {
        return false;
    }
    return true;
}

std::string Rules::featName(std::int32_t featId) const
{
    if (!feat_) {
        return "feat " + std::to_string(featId);
    }
    const std::string row = std::to_string(featId);
    if (feat_->rowIndex(row) < 0) {
        return "feat " + std::to_string(featId);
    }
    return feat_->cell(row, "label");
}

std::string Rules::animationName(std::int32_t animationId) const
{
    if (!animations_) {
        return {};
    }
    const std::string row = std::to_string(animationId);
    if (animations_->rowIndex(row) < 0) {
        return {};
    }
    return animations_->cell(row, "name");
}

bool Rules::animationLooping(std::int32_t animationId) const
{
    if (!animations_) {
        return false;
    }
    return cellInt(*animations_, std::to_string(animationId), "looping", 0) != 0;
}

bool Rules::animationStationary(std::int32_t animationId) const
{
    if (!animations_) {
        return false;
    }
    return cellInt(*animations_, std::to_string(animationId), "stationary", 0) != 0;
}

std::string Rules::powerName(std::int32_t powerId) const
{
    if (!spells_) {
        return "power " + std::to_string(powerId);
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0) {
        return "power " + std::to_string(powerId);
    }
    return spells_->cell(row, "label");
}

std::optional<ForcePowerInfo> Rules::forcePower(std::int32_t powerId) const
{
    if (!spells_) {
        return std::nullopt;
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0) {
        return std::nullopt;
    }
    const std::string label = spells_->cell(row, "label");
    if (label.empty() || cellInt(*spells_, row, "usertype", 0) != 1) {
        return std::nullopt;
    }
    ForcePowerInfo info;
    info.id = powerId;
    info.label = label;
    info.cost = std::max<std::int32_t>(0, cellInt(*spells_, row, "forcepoints", 0));
    const std::string alignment = spells_->cell(row, "goodevil");
    info.alignment = alignment.empty() ? '-' : alignment.front();
    const std::string range = spells_->cell(row, "range");
    if (range == "W" || range == "T") {
        info.range = 2;
    } else if (range == "M") {
        info.range = 15;
    } else if (range == "S") {
        info.range = 25;
    } else {
        info.range = 0;
    }
    return info;
}

std::int32_t Rules::maxForcePoints(std::int32_t classId, std::int32_t level,
                                   std::int32_t wisdom, std::int32_t charisma) const
{
    // Jedi Guardian/Consular/Sentinel have force points. The engine uses a
    // transparent data-scaled rule (10 + positive WIS/CHA modifiers per level)
    // because the shipped tables do not carry a force-point progression.
    if (classId < 3 || classId > 5) {
        return 0;
    }
    const std::int32_t wis = std::max(0, static_cast<std::int32_t>(abilityModifier(wisdom)));
    const std::int32_t cha = std::max(0, static_cast<std::int32_t>(abilityModifier(charisma)));
    return std::max<std::int32_t>(1, level * (10 + wis + cha));
}

std::int32_t Rules::forcePowerSaveDC(const CharacterSheet& sheet) const
{
    std::int32_t focus = 0;
    if (hasFeatLabel(sheet, "FORCE_FOCUS_MASTERY")) {
        focus = 4;
    } else if (hasFeatLabel(sheet, "FORCE_FOCUS_ADVANCED")) {
        focus = 2;
    } else if (hasFeatLabel(sheet, "FORCE_FOCUS")) {
        focus = 1;
    }
    return 10 + sheet.level / 2
        + static_cast<std::int32_t>(abilityModifier(sheet.wis)) + focus;
}

bool Rules::hasFeatLabel(const CharacterSheet& sheet, const std::string& needle) const
{
    for (const std::int32_t feat : sheet.feats) {
        if (featName(feat).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::int32_t Rules::featAttackBonus(const CharacterSheet& sheet) const
{
    bool weaponFocus = false;
    std::int32_t dueling = 0;
    for (const std::int32_t feat : sheet.feats) {
        const std::string label = featName(feat);
        if (label.find("WEAPON_FOCUS") != std::string::npos) {
            weaponFocus = true;
        } else if (label == "DUELING") {
            dueling = std::max<std::int32_t>(dueling, 1);
        } else if (label == "ADVANCED_DUELING") {
            dueling = std::max<std::int32_t>(dueling, 2);
        } else if (label == "MASTER_DUELING") {
            dueling = std::max<std::int32_t>(dueling, 3);
        }
    }
    return (weaponFocus ? 1 : 0) + dueling;
}

std::int32_t Rules::featDamageBonus(const CharacterSheet& sheet) const
{
    for (const std::int32_t feat : sheet.feats) {
        if (featName(feat).find("WEAPON_SPEC") != std::string::npos) {
            return 2;
        }
    }
    return 0;
}

std::int32_t Rules::toughnessBonus(const CharacterSheet& sheet) const
{
    std::int32_t perLevel = 0;
    if (hasFeatLabel(sheet, "TOUGHNESS")) {
        perLevel = 1;
    }
    if (hasFeatLabel(sheet, "MASTER_TOUGHNESS")) {
        perLevel = 2;
    }
    if (hasFeatLabel(sheet, "WOOKIE_ENDURANCE")) {
        perLevel += 2;
    }
    return perLevel * sheet.level;
}

std::int32_t Rules::damageReduction(const CharacterSheet& sheet) const
{
    if (hasFeatLabel(sheet, "IMPROVED_TOUGHNESS")
        || hasFeatLabel(sheet, "MASTER_TOUGHNESS")
        || hasFeatLabel(sheet, "WOOKIE_ENDURANCE")) {
        return 2;
    }
    return 0;
}

std::int32_t Rules::sneakAttackDice(const CharacterSheet& sheet) const
{
    for (std::int32_t dice = 10; dice >= 1; --dice) {
        if (hasFeatLabel(sheet, "SNEAK_ATTACK_" + std::to_string(dice) + "D6")) {
            return dice;
        }
    }
    return 0;
}

std::int32_t Rules::forceJumpBonus(const CharacterSheet& sheet) const
{
    if (hasFeatLabel(sheet, "FORCE_JUMP_MASTERY")) {
        return 4;
    }
    if (hasFeatLabel(sheet, "FORCE_JUMP_ADVANCED")) {
        return 2;
    }
    return 0;
}

std::int32_t Rules::jediDefenseBonus(const CharacterSheet& sheet) const
{
    if (!hasLightsaber(sheet)) {
        return 0;
    }
    if (hasFeatLabel(sheet, "MASTER_JEDI_DEFENSE")) {
        return 6;
    }
    if (hasFeatLabel(sheet, "ADVANCED_JEDI_DEFENSE")) {
        return 3;
    }
    return 0;
}

bool Rules::allowsStealth(const CharacterSheet& sheet) const
{
    return hasFeatLabel(sheet, "FORCE_CAMOFLAGE");
}

bool Rules::jediDeflects(const CharacterSheet& sheet, std::int32_t attackTotal,
                         std::int32_t deflectionRoll) const
{
    if (!hasLightsaber(sheet)
        || (!hasFeatLabel(sheet, "JEDI_DEFENSE")
            && !hasFeatLabel(sheet, "ADVANCED_JEDI_DEFENSE")
            && !hasFeatLabel(sheet, "MASTER_JEDI_DEFENSE"))) {
        return false;
    }
    return deflectionRoll + jediDefenseBonus(sheet) > attackTotal;
}

bool Rules::hasLightsaber(const CharacterSheet& sheet) const
{
    if (!baseitems_) {
        return false;
    }
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped) {
            continue;
        }
        const std::string row = std::to_string(item.baseItem);
        if (baseitems_->rowIndex(row) < 0) {
            continue;
        }
        const std::string label = lower(baseitems_->cell(row, "label"));
        if (label.find("lightsaber") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::int32_t Rules::skillBonus(const CharacterSheet& sheet, const std::string& skill) const
{
    const std::string name = lower(skill);
    std::int32_t bonus = 0;
    const auto tier = [&](const char* base, const char* improved, const char* master,
                          const char* spelledMaster) {
        if (hasFeatLabel(sheet, master) || (spelledMaster != nullptr
                                            && hasFeatLabel(sheet, spelledMaster))) {
            return 3;
        }
        if (hasFeatLabel(sheet, improved)) {
            return 2;
        }
        return hasFeatLabel(sheet, base) ? 1 : 0;
    };
    if (name == "demolitions" || name == "stealth") {
        bonus = std::max(bonus, tier("CAUTIOUS", "IMPROVED_CAUTION", "MASTER_CAUTION", nullptr));
    }
    if (name == "persuade" || name == "awareness" || name == "treatinjury") {
        bonus = std::max(bonus, tier("EMPATHY", "IMPROVED_EMPATHY", "MASTER__EMPATHY", nullptr));
    }
    if (name == "repair" || name == "security" || name == "computeruse") {
        bonus = std::max(bonus, tier("GEAR_HEAD", "GEAR_HEAD_ADEPT", "GEAR_HEAD_MASTER", nullptr));
    }
    return bonus;
}

int Rules::equipSlotFor(std::int32_t baseItem) const
{
    if (!baseitems_) {
        return -1;
    }
    const std::string row = std::to_string(baseItem);
    if (baseitems_->rowIndex(row) < 0) {
        return -1;
    }
    const std::string mask = baseitems_->cell(row, "equipableslots");
    if (mask.empty() || mask == "****") {
        return -1;
    }
    try {
        const unsigned long value = std::stoul(mask, nullptr, 0);
        if (value == 0) {
            return -1;
        }
        for (int bit = 0; bit < 32; ++bit) {
            if ((value & (1UL << bit)) != 0) {
                return bit;
            }
        }
    } catch (const std::exception&) {
        return -1;
    }
    return -1;
}

int Rules::itemCost(std::int32_t baseItem) const
{
    if (!baseitems_) {
        return 0;
    }
    return cellInt(*baseitems_, std::to_string(baseItem), "basecost", 0);
}

std::int32_t Rules::baseItemInt(std::int32_t baseItem, const std::string& column,
                                std::int32_t fallback) const
{
    if (!baseitems_) {
        return fallback;
    }
    const std::string row = std::to_string(baseItem);
    if (baseitems_->rowIndex(row) < 0) {
        return fallback;
    }
    return cellInt(*baseitems_, row, column, fallback);
}

std::string Rules::baseItemString(std::int32_t baseItem, const std::string& column) const
{
    if (!baseitems_) {
        return {};
    }
    const std::string row = std::to_string(baseItem);
    if (baseitems_->rowIndex(row) < 0) {
        return {};
    }
    const std::string text = baseitems_->cell(row, column);
    return text == "****" ? std::string{} : text;
}

std::int32_t Rules::maxStack(std::int32_t baseItem) const
{
    return std::max<std::int32_t>(1, baseItemInt(baseItem, "stacking", 1));
}

std::uint32_t Rules::equipMask(std::int32_t baseItem) const
{
    if (!baseitems_) {
        return 0;
    }
    const std::string row = std::to_string(baseItem);
    if (baseitems_->rowIndex(row) < 0) {
        return 0;
    }
    const std::string mask = baseitems_->cell(row, "equipableslots");
    if (mask.empty() || mask == "****") {
        return 0;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(mask, nullptr, 0));
    } catch (const std::exception&) {
        return 0;
    }
}

InventoryItem Rules::makeItem(const std::string& resref, std::int32_t baseItem) const
{
    InventoryItem item;
    item.resref = resref;
    item.baseItem = baseItem;
    item.cost = itemCost(baseItem);
    item.slot = equipSlotFor(baseItem);
    return item;
}

bool Rules::canEquip(const CharacterSheet& sheet, const InventoryItem& item) const
{
    if (item.slot < 0 && equipSlotFor(item.baseItem) < 0) {
        return false;
    }
    if (!baseitems_) {
        return true;
    }
    const std::string row = std::to_string(item.baseItem);
    if (baseitems_->rowIndex(row) < 0) {
        return true;
    }
    for (const char* column : {"reqfeat0", "reqfeat1", "reqfeat2", "reqfeat3", "reqfeat4"}) {
        const std::int32_t required = cellInt(*baseitems_, row, column, -1);
        if (required >= 0 && !sheet.feats.contains(required)) {
            return false;
        }
    }
    return true;
}

std::int32_t Rules::armorClass(const CharacterSheet& sheet) const
{
    std::int32_t ac = 10 + static_cast<std::int32_t>(abilityModifier(sheet.dex))
        + classArmorBonus(sheet.classId, sheet.level); // retail 005be770
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped || !baseitems_) {
            continue;
        }
        const std::string row = std::to_string(item.baseItem);
        ac += cellInt(*baseitems_, row, "baseac", 0);
    }
    if (hasFeatLabel(sheet, "MASTER_SCOUNDRELS_LUCK")) {
        ac += 6;
    } else if (hasFeatLabel(sheet, "IMPROVED_SCOUNDRELS_LUCK")) {
        ac += 4;
    } else if (hasFeatLabel(sheet, "SCOUNDRELS_LUCK")) {
        ac += 2;
    }
    if (hasFeatLabel(sheet, "MASTER_SENSE")) {
        ac += 6;
    } else if (hasFeatLabel(sheet, "KNIGHT_SENSE")) {
        ac += 4;
    } else if (hasFeatLabel(sheet, "JEDI_SENSE")) {
        ac += 2;
    }
    return ac;
}

std::int32_t Rules::twoWeaponPenalty(const CharacterSheet& sheet) const
{
    if (!baseitems_) {
        return 0;
    }
    int weapons = 0;
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped) {
            continue;
        }
        const std::string row = std::to_string(item.baseItem);
        if (cellInt(*baseitems_, row, "weapontype", 0) != 0) {
            ++weapons;
        }
    }
    if (weapons < 2) {
        return 0;
    }
    if (hasFeatLabel(sheet, "TWO_WEAPON_MASTERY")) {
        return 0;
    }
    if (hasFeatLabel(sheet, "TWO_WEAPON_ADVANCED")) {
        return -2;
    }
    if (hasFeatLabel(sheet, "TWO_WEAPON_FIGHTING")) {
        return -4;
    }
    return -6;
}

std::int32_t Rules::attackBonus(const CharacterSheet& sheet) const
{
    return baseAttackBonus(sheet.classId, sheet.level)
        + static_cast<std::int32_t>(abilityModifier(sheet.str))
        + featAttackBonus(sheet)
        + twoWeaponPenalty(sheet);
}

std::int32_t Rules::damageBonus(const CharacterSheet& sheet) const
{
    return static_cast<std::int32_t>(abilityModifier(sheet.str))
        + featDamageBonus(sheet);
}

std::int32_t Rules::initiativeBonus(const CharacterSheet& sheet) const
{
    return static_cast<std::int32_t>(abilityModifier(sheet.dex));
}

AttackResult Rules::resolveAttack(const CharacterSheet& attacker, std::int32_t targetAc,
                                  std::uint32_t& rng, std::int32_t damageDice,
                                  std::int32_t damageDie, std::int32_t damageBonus,
                                  std::int32_t criticalThreat, std::int32_t attackModifier) const
{
    AttackResult result;
    result.roll = rollDie(rng, 20);
    result.total = result.roll + attackBonus(attacker) + attackModifier;
    if (result.roll == 1) {
        return result;
    }
    result.hit = result.roll == 20 || result.total >= targetAc;
    if (!result.hit) {
        return result;
    }
    if (result.roll >= std::max<std::int32_t>(2, std::min<std::int32_t>(20, criticalThreat))) {
        const int confirm = rollDie(rng, 20) + attackBonus(attacker) + attackModifier;
        result.critical = confirm >= targetAc;
    }
    std::int32_t damage = damageBonus;
    for (std::int32_t die = 0; die < std::max<std::int32_t>(1, damageDice); ++die) {
        damage += rollDie(rng, std::max<std::int32_t>(1, damageDie));
    }
    result.damage = std::max<std::int32_t>(1, result.critical ? damage * 2 : damage);
    return result;
}

void Rules::weaponDamage(const CharacterSheet& sheet, std::int32_t& dice, std::int32_t& die,
                         std::int32_t& bonus) const
{
    dice = 1;
    die = 8;
    bonus = damageBonus(sheet);
    if (!baseitems_) {
        return;
    }
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped) {
            continue;
        }
        const std::string row = std::to_string(item.baseItem);
        const std::int32_t itemDice = cellInt(*baseitems_, row, "numdice", 0);
        const std::int32_t itemDie = cellInt(*baseitems_, row, "dietoroll", 0);
        if (itemDice > 0 && itemDie > 0) {
            dice = itemDice;
            die = itemDie;
            return;
        }
    }
}

std::int32_t Rules::classArmorBonus(std::int32_t classId, std::int32_t level) const
{
    if (!classes_ || !acbonus_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 0;
    }
    const std::string column = lower(classes_->cell(std::to_string(classId), "armorclasscolumn"));
    if (column.empty() || column == "****") {
        return 0;
    }
    // One row per level, read as is. retail 005be770.
    return cellInt(*acbonus_, std::to_string(std::clamp(level, 1, 20) - 1), column, 0);
}

void Rules::deriveCreatureCombat(kotor::Entity& entity) const
{
    kotor::CreatureStats& stats = entity.stats;
    if (!stats.parsed || stats.combatDerived) {
        return;
    }
    stats.combatDerived = true;
    const int strMod = abilityModifier(stats.abilities[0]);
    const int dexMod = abilityModifier(stats.abilities[1]);
    const int conMod = abilityModifier(stats.abilities[2]);
    const int wisMod = abilityModifier(stats.abilities[4]);
    const auto hasFeat = [&](std::int32_t feat) {
        return std::find(stats.feats.begin(), stats.feats.end(), feat) != stats.feats.end();
    };
    std::int32_t classAc = 0;
    std::int32_t fort = 0;
    std::int32_t reflex = 0;
    std::int32_t will = 0;
    stats.baseAttack = 0;
    for (const auto& [classId, level] : stats.classes) {
        stats.baseAttack += baseAttackBonus(classId, level); // retail 005a60d0
        classAc += classArmorBonus(classId, level);
        fort += savingThrow(classId, level, 0);
        reflex += savingThrow(classId, level, 1);
        will += savingThrow(classId, level, 2);
    }
    // Item property cost value: the row's `value` column, else its numeric label.
    const auto costValue = [&](const kotor::ItemTemplate::Property& property) -> const kotor::TwoDa* {
        if (!costTables_ || install_ == nullptr) {
            return nullptr;
        }
        const std::string name = lower(costTables_->cell(std::to_string(property.costTable), "name"));
        if (name.empty() || name == "****") {
            return nullptr;
        }
        auto [slot, added] = propertyTables_.try_emplace(name);
        if (added) {
            if (auto data = install_->find(name, 2017); data.has_value()) {
                try {
                    slot->second = kotor::TwoDa::parse(std::move(*data), name);
                } catch (const kotor::FormatError&) {
                    slot->second.reset();
                }
            }
        }
        return slot->second.has_value() ? &*slot->second : nullptr;
    };
    const auto propertyValue = [&](const kotor::ItemTemplate::Property& property) {
        const kotor::TwoDa* table = costValue(property);
        if (table == nullptr) {
            return 0;
        }
        const std::string row = std::to_string(property.costValue);
        const std::int32_t value = cellInt(*table, row, "value", INT32_MIN);
        return value != INT32_MIN ? value : cellInt(*table, row, "label", 0);
    };
    std::int32_t armorAc = 0;
    std::int32_t dexCap = 100;
    bool hasArmorCap = false;
    const auto readWeapon = [&](kotor::CreatureStats::Weapon& weapon,
                                const kotor::ItemTemplate& item) {
        weapon.present = true;
        weapon.baseItem = item.baseItem;
        const std::string row = std::to_string(item.baseItem);
        weapon.dice = cellInt(*baseitems_, row, "numdice", 0);
        weapon.die = cellInt(*baseitems_, row, "dietoroll", 0);
        weapon.critThreat = cellInt(*baseitems_, row, "critthreat", 1);
        weapon.critMult = cellInt(*baseitems_, row, "crithitmult", 2);
        weapon.ranged = cellInt(*baseitems_, row, "rangedweapon", 0) != 0
            || cellInt(*baseitems_, row, "weapontype", 0) == 4;
        for (const kotor::ItemTemplate::Property& property : item.properties) {
            switch (property.name) {
            case 5: // Enhancement
                weapon.attackBonus += propertyValue(property);
                weapon.flatDamage += propertyValue(property);
                break;
            case 38: // AttackBonus
                weapon.attackBonus += propertyValue(property);
                break;
            case 8:  // AttackPenalty
            case 41: // ToHitPenalty
                weapon.attackBonus -= std::abs(propertyValue(property));
                break;
            case 11: { // Damage: a flat row, or dice in the numdice/die columns
                const kotor::TwoDa* table = costValue(property);
                if (table != nullptr) {
                    const std::string cost = std::to_string(property.costValue);
                    const std::int32_t dice = cellInt(*table, cost, "numdice", 0);
                    const std::int32_t die = cellInt(*table, cost, "die", 0);
                    weapon.flatDamage += dice > 0 && die > 0
                        ? dice * (die + 1) / 2
                        : cellInt(*table, cost, "label", 0);
                }
                break;
            }
            case 15: // DamagePenalty
                weapon.flatDamage -= std::abs(propertyValue(property));
                break;
            case 28: // Keen
                weapon.keen = true;
                break;
            case 51: { // Monster_damage: creature weapon dice. retail 005a9050
                const kotor::TwoDa* table = costValue(property);
                if (table != nullptr) {
                    const std::string cost = std::to_string(property.costValue);
                    weapon.dice = cellInt(*table, cost, "numdice", weapon.dice);
                    weapon.die = cellInt(*table, cost, "die", weapon.die);
                }
                break;
            }
            default:
                break;
            }
        }
    };
    stats.mainHand = {};
    stats.offHand = {};
    kotor::CreatureStats::Weapon creatureWeapon;
    if (install_ != nullptr && baseitems_) {
        for (const kotor::CreatureItem& carried : entity.creatureItems) {
            if (carried.slot < 0) {
                continue;
            }
            auto [cached, added] = itemTemplates_.try_emplace(carried.resref);
            if (added) {
                cached->second = kotor::loadItemTemplate(*install_, carried.resref);
            }
            if (!cached->second.has_value()) {
                continue;
            }
            const kotor::ItemTemplate& item = *cached->second;
            const std::string row = std::to_string(item.baseItem);
            if (carried.slot == 4) { // INVENTORY_SLOT_RIGHTWEAPON
                readWeapon(stats.mainHand, item);
            } else if (carried.slot == 5) { // INVENTORY_SLOT_LEFTWEAPON
                readWeapon(stats.offHand, item);
            } else if (carried.slot >= 14 && carried.slot <= 16) { // CWEAPON_L, R, B
                if (!creatureWeapon.present) {
                    readWeapon(creatureWeapon, item);
                }
            } else {
                // Worn items: armor base and cap from the base item, properties on top.
                armorAc += cellInt(*baseitems_, row, "baseac", 0);
                const std::int32_t cap = cellInt(*baseitems_, row, "dexbonus", -1);
                if (cellInt(*baseitems_, row, "baseac", 0) > 0 && cap >= 0) {
                    dexCap = hasArmorCap ? std::min(dexCap, cap) : cap;
                    hasArmorCap = true;
                }
                for (const kotor::ItemTemplate::Property& property : item.properties) {
                    if (property.name == 1) { // Armor
                        armorAc += propertyValue(property);
                    } else if (property.name == 20) { // DecreaseAC
                        armorAc -= std::abs(propertyValue(property));
                    }
                }
            }
        }
    }
    if (!stats.mainHand.present && creatureWeapon.present) {
        stats.mainHand = creatureWeapon;
    }
    const auto finishWeapon = [&](kotor::CreatureStats::Weapon& weapon, bool offhand) {
        // Unarmed: 005a4d90 rolls a die of 1 or 2 by a size value at creature +0x4f8
        // whose source is not resolved; 1d2 is used here.
        if (weapon.dice <= 0 || weapon.die <= 0) {
            weapon.dice = 1;
            weapon.die = 2;
        }
        std::int32_t attack = stats.baseAttack + weapon.attackBonus;
        // Ability: ranged uses Dex; melee uses Str unless finesse applies. retail 005a7ea0.
        bool finesse = false;
        if (!weapon.ranged && weapon.present) {
            const bool saber = weapon.baseItem >= 8 && weapon.baseItem <= 10;
            for (const std::int32_t feat : stats.feats) {
                const std::string label = featName(feat);
                if (label.find("FINESSE") != std::string::npos
                    && (label.find("LIGHTSABER") != std::string::npos) == saber) {
                    finesse = true;
                }
            }
        }
        attack += weapon.ranged || finesse ? dexMod : strMod;
        if (weapon.present) {
            const std::string row = std::to_string(weapon.baseItem);
            const std::int32_t focus = cellInt(*baseitems_, row, "focfeat", -1);
            if (focus >= 0 && hasFeat(focus)) {
                attack += 1; // retail 005a7ea0, base item +0xb2
            }
            const std::int32_t spec = cellInt(*baseitems_, row, "specfeat", -1);
            if (spec >= 0 && hasFeat(spec)) {
                weapon.flatDamage += 2; // retail 005a8be0, base item +0xb0
            }
        }
        if (stats.offHand.present) {
            // retail 005a7ea0: -6 on-hand, -10 off-hand, reduced by the two-weapon feats.
            if (offhand) {
                attack += hasFeat(85) ? -2 : hasFeat(9) ? -4 : hasFeat(3) ? -6 : -10;
            } else {
                attack += hasFeat(85) ? 0 : hasFeat(9) ? -2 : hasFeat(3) ? -4 : -6;
            }
        } else if (weapon.present && !weapon.ranged) {
            const std::int32_t wield = cellInt(*baseitems_, std::to_string(weapon.baseItem),
                                               "weaponwield", 0);
            if (wield == 2 || wield == 4) {
                // Dueling feats 113..115. retail 005a7ea0.
                attack += hasFeat(115) ? 3 : hasFeat(114) ? 2 : hasFeat(113) ? 1 : 0;
            }
        }
        weapon.attackBonus = attack;
        if (!weapon.ranged) {
            weapon.flatDamage += strMod; // retail 005a8be0: Str for melee only
        }
        weapon.maxDamage = weapon.dice * weapon.die + weapon.flatDamage;
    };
    finishWeapon(stats.mainHand, false);
    if (stats.offHand.present) {
        finishWeapon(stats.offHand, true);
    }
    entity.attackBonus = stats.mainHand.attackBonus;
    entity.damageMin = std::max(1, stats.mainHand.dice + stats.mainHand.flatDamage);
    entity.damageMax = std::max(entity.damageMin, stats.mainHand.maxDamage);
    if (stats.mainHand.present) {
        entity.ranged = stats.mainHand.ranged;
        if (entity.ranged) {
            entity.attackRange = static_cast<float>(cellInt(
                *baseitems_, std::to_string(stats.mainHand.baseItem), "maxattackrange", 15));
        }
    }
    // retail 005ad0e0: 10 + class + natural + armor + capped Dex.
    entity.defense = 10 + classAc + stats.naturalAC + armorAc + std::min(dexMod, dexCap);
    // Class table plus ability; the UTC value on top. The base helpers 005ab810,
    // 005ab880 and 005ab8f0 were not read to the end, so this split is an assumption.
    entity.fortSave += fort + conMod;
    entity.refSave += reflex + dexMod;
    entity.willSave += will + wisMod;
}

std::int32_t Rules::killXp(std::int32_t leaderLevel, float challengeRating,
                           std::int32_t partyCount) const
{
    if (!xptable_ || !npc_) {
        return 0;
    }
    const std::int32_t row = std::clamp(leaderLevel, 1, 20) - 1;
    const std::int32_t column = std::clamp(static_cast<std::int32_t>(challengeRating), 0, 20);
    const std::int32_t base = cellInt(*xptable_, std::to_string(row), "c" + std::to_string(column), 0);
    // npc.2da row 9 GAME_XP_General scales everything; row 10 PER_NPC_Bonus is per member.
    double award = static_cast<double>(base) * cellInt(*npc_, "9", "percentxp", 100) * 0.01;
    const std::int32_t perNpc = cellInt(*npc_, "10", "percentxp", 0);
    if (perNpc > 0) {
        award *= static_cast<double>(perNpc) * 0.01 * partyCount + 1.0;
    }
    return static_cast<std::int32_t>(award);
}

float Rules::perceptionRange(std::int32_t row, bool hearing) const
{
    if (!ranges_) {
        return 20.0f;
    }
    const std::string text = ranges_->cell(std::to_string(row),
                                           hearing ? "secondaryrange" : "primaryrange");
    try {
        return text.empty() || text == "****" ? 20.0f : std::stof(text);
    } catch (const std::exception&) {
        return 20.0f;
    }
}

float Rules::partyDamageMultiplier() const
{
    if (!difficultyopt_) {
        return 1.0f;
    }
    // Row 1 is the shipped default setting; the engine has no difficulty option yet.
    const std::string text = difficultyopt_->cell("1", "multiplier");
    try {
        return text.empty() || text == "****" ? 1.0f : std::stof(text);
    } catch (const std::exception&) {
        return 1.0f;
    }
}

std::int32_t Rules::talentInt(bool power, std::int32_t id, const std::string& column,
                              std::int32_t fallback) const
{
    const std::optional<kotor::TwoDa>& table = power ? spells_ : feat_;
    return table ? cellInt(*table, std::to_string(id), column, fallback) : fallback;
}

std::string Rules::powerImpactScript(std::int32_t powerId) const
{
    if (!spells_) {
        return {};
    }
    const std::string script = spells_->cell(std::to_string(powerId), "impactscript");
    return script == "****" ? std::string{} : script;
}

std::int32_t Rules::classHitDie(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 10;
    }
    return cellInt(*classes_, std::to_string(classId), "hitdie", 10);
}

std::string Rules::className(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return {};
    }
    return classes_->cell(std::to_string(classId), "label");
}

std::string Rules::classConstant(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return {};
    }
    return classes_->cell(std::to_string(classId), "constant");
}

std::string Rules::classDescription(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return {};
    }
    return classes_->cell(std::to_string(classId), "description");
}

std::int32_t Rules::portraitCount() const
{
    if (!portraits_) {
        return 0;
    }
    std::int32_t count = 0;
    for (std::size_t row = 0; row < portraits_->rowCount(); ++row) {
        if (cellInt(*portraits_, portraits_->rowLabel(row), "forpc", 0) == 1) {
            ++count;
        }
    }
    return count;
}

PortraitInfo Rules::portrait(std::int32_t index) const
{
    PortraitInfo info;
    if (!portraits_ || index < 0) {
        return info;
    }
    std::int32_t seen = 0;
    for (std::size_t row = 0; row < portraits_->rowCount(); ++row) {
        const std::string label = portraits_->rowLabel(row);
        if (cellInt(*portraits_, label, "forpc", 0) != 1) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        info.resref = portraits_->cell(label, "baseresref");
        info.appearance = static_cast<std::uint32_t>(
            cellInt(*portraits_, label, "appearancenumber", 0));
        info.sex = cellInt(*portraits_, label, "sex", 0);
        return info;
    }
    return info;
}

std::int32_t Rules::skillCount() const
{
    return skills_ ? static_cast<std::int32_t>(skills_->rowCount()) : 0;
}

std::string Rules::skillName(std::int32_t index) const
{
    if (!skills_ || index < 0
        || index >= static_cast<std::int32_t>(skills_->rowCount())) {
        return {};
    }
    const std::string row = skills_->rowLabel(static_cast<std::size_t>(index));
    std::string name = skills_->cell(row, "label");
    if (name.empty()) {
        name = row;
    }
    return name;
}

bool Rules::skillIsClassSkill(std::int32_t classId, std::int32_t skillIndex) const
{
    if (!skills_ || classId < 0 || classId > 2 || skillIndex < 0
        || skillIndex >= static_cast<std::int32_t>(skills_->rowCount())) {
        return false;
    }
    static const char* columns[] = {"sol_class", "sct_class", "scd_class"};
    const std::string row = skills_->rowLabel(static_cast<std::size_t>(skillIndex));
    return cellInt(*skills_, row, columns[classId], 0) != 0;
}

bool Rules::featIsAvailable(std::int32_t classId, std::int32_t featId) const
{
    if (!feat_ || classId < 0 || classId > 2 || featId < 0
        || featId >= static_cast<std::int32_t>(feat_->rowCount())) {
        return false;
    }
    const std::string row = std::to_string(featId);
    if (cellInt(*feat_, row, "allclassescanuse", 0) != 0) {
        return true;
    }
    static const char* columns[] = {"sol_list", "sct_list", "scd_list"};
    const std::string value = feat_->cell(row, columns[classId]);
    return !value.empty() && value != "****" && value != "0";
}

} // namespace ke
