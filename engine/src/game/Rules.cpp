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
    baseitems_ = loadTable("baseitems");
    featgain_ = loadTable("featgain");
    spells_ = loadTable("spells");
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
    sheet.maxHp = maxHitPoints(sheet.classId, sheet.level, sheet.con);
    sheet.hp = sheet.maxHp;
    sheet.maxFp = 0;
    sheet.fp = 0;
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
    return cellInt(it->second, std::to_string(std::max(1, std::min(level, 20)) - 1), column, 0);
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
    (void)sheet;
    return item.slot >= 0 || equipSlotFor(item.baseItem) >= 0;
}

std::int32_t Rules::armorClass(const CharacterSheet& sheet) const
{
    std::int32_t ac = 10 + static_cast<std::int32_t>(abilityModifier(sheet.dex));
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped || !baseitems_) {
            continue;
        }
        const std::string row = std::to_string(item.baseItem);
        ac += cellInt(*baseitems_, row, "baseac", 0);
    }
    return ac;
}

std::int32_t Rules::attackBonus(const CharacterSheet& sheet) const
{
    return baseAttackBonus(sheet.classId, sheet.level)
        + static_cast<std::int32_t>(abilityModifier(sheet.str));
}

std::int32_t Rules::damageBonus(const CharacterSheet& sheet) const
{
    return static_cast<std::int32_t>(abilityModifier(sheet.str));
}

std::int32_t Rules::initiativeBonus(const CharacterSheet& sheet) const
{
    return static_cast<std::int32_t>(abilityModifier(sheet.dex));
}

AttackResult Rules::resolveAttack(const CharacterSheet& attacker, std::int32_t targetAc,
                                  std::uint32_t& rng, std::int32_t damageDice,
                                  std::int32_t damageDie, std::int32_t damageBonus) const
{
    AttackResult result;
    result.roll = rollDie(rng, 20);
    result.total = result.roll + attackBonus(attacker);
    if (result.roll == 1) {
        return result;
    }
    result.hit = result.roll == 20 || result.total >= targetAc;
    if (!result.hit) {
        return result;
    }
    if (result.roll == 20) {
        const int confirm = rollDie(rng, 20) + attackBonus(attacker);
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

} // namespace ke
