#include "game/Rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

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
    classPowerGain_ = loadTable("classpowergain");
    animations_ = loadTable("animations");
    combatAnimations_ = loadTable("combatanimations");
    dialogAnimations_ = loadTable("dialoganimations");
    xptable_ = loadTable("xptable");
    npc_ = loadTable("npc");
    ranges_ = loadTable("ranges");
    acbonus_ = loadTable("acbonus");
    difficultyopt_ = loadTable("difficultyopt");
    forceadjust_ = loadTable("forceadjust");
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
    // Granted feats come from feat.2da (<prefix>_list == 3, <prefix>_granted <= level).
    for (const std::int32_t feat : grantedFeats(classId, sheet.level)) {
        sheet.feats.insert(feat);
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
        sheet.skillPoints += current == 1 ? chargenSkillPoints(sheet.classId, sheet.intel)
                                          : skillPointsForLevel(sheet.classId, sheet.intel);
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
    // Level-up award (levels 2+): (SkillPointBase + INT mod) / 2, at least 1. From the
    // retail level-up path; the rounding was not re-verified against the exe.
    const std::int32_t base = classSkillPointBase(classId);
    return std::max<std::int32_t>(1, (base + abilityModifier(intelligence)) / 2);
}

std::int32_t Rules::chargenSkillPoints(std::int32_t classId, std::int32_t intelligence) const
{
    // Level-1 award: four times (SkillPointBase / 2 + INT mod), never below 4.
    const std::int32_t term = classSkillPointBase(classId) / 2 + abilityModifier(intelligence);
    return std::max<std::int32_t>(4, term * 4);
}

std::int32_t Rules::classSkillPointBase(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 2;
    }
    return cellInt(*classes_, std::to_string(classId), "skillpointbase", 2);
}

std::int32_t Rules::classForceDie(std::int32_t classId) const
{
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return 0;
    }
    return cellInt(*classes_, std::to_string(classId), "forcedie", 0);
}

std::string Rules::classColumnPrefix(std::int32_t classId) const
{
    // classes.2da FeatGain names the column family used by featgain.2da, feat.2da
    // (<p>_list/_granted) and skills.2da (<p>_class): SOL, SCT, SCD, JGD, JCN, JSN.
    if (!classes_ || classId < 0
        || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return {};
    }
    const std::string value = lower(classes_->cell(std::to_string(classId), "featgain"));
    return value == "****" ? std::string{} : value;
}

std::int32_t Rules::featsForLevel(std::int32_t classId, std::int32_t level) const
{
    // The columns follow the class's own FeatGain prefix, not class id order: ids 4
    // and 5 are Consular (JCN) and Sentinel (JSN); the old table had them swapped.
    if (!featgain_) {
        return 0;
    }
    const std::string prefix = classColumnPrefix(classId);
    if (prefix.empty()) {
        return 0;
    }
    const int row = featgain_->rowIndex(std::to_string(std::max(1, std::min(level, 20)) - 1));
    if (row < 0) {
        return 0;
    }
    const std::string label = featgain_->rowLabel(static_cast<std::size_t>(row));
    return cellInt(*featgain_, label, prefix + "_reg", 0)
        + cellInt(*featgain_, label, prefix + "_bon", 0);
}

std::vector<std::int32_t> Rules::grantedFeats(std::int32_t classId, std::int32_t level) const
{
    std::vector<std::int32_t> result;
    const std::string prefix = classColumnPrefix(classId);
    if (!feat_ || prefix.empty()) {
        return result;
    }
    // Inferred from the table shape: list 3 with an empty recommendation rank is the
    // class's automatic grant at `granted`; list 3 with a rank (Power Attack and
    // similar) is a selectable signature feat. retail reads these columns by index and
    // that reader was not located, so this reading is unverified.
    for (std::size_t row = 0; row < feat_->rowCount(); ++row) {
        const std::string label = feat_->rowLabel(row);
        if (cellInt(*feat_, label, prefix + "_list", 0) != 3) {
            continue;
        }
        const std::string recom = feat_->cell(label, prefix + "_recom");
        if (!recom.empty() && recom != "****") {
            continue;
        }
        const std::int32_t at = cellInt(*feat_, label, prefix + "_granted", -1);
        if (at >= 0 && at <= level) {
            result.push_back(static_cast<std::int32_t>(row));
        }
    }
    return result;
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

std::int32_t Rules::attackImpactMs(const std::string& clip, int subAttack) const
{
    if (!animations_ || !combatAnimations_ || clip.empty()) {
        return -1;
    }
    const int nameColumn = animations_->columnIndex("name");
    if (nameColumn < 0) {
        return -1;
    }
    for (std::size_t row = 0; row < animations_->rowCount(); ++row) {
        const std::string name = animations_->cell(row, static_cast<std::size_t>(nameColumn));
        if (name.size() != clip.size()
            || !std::equal(name.begin(), name.end(), clip.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               })) {
            continue;
        }
        const std::string label = animations_->rowLabel(row);
        // 005b4f30 starts the result at 0 and keeps it when the row or the cell is missing: a clip
        // without a hit time (the blaster rows b5a1.., hits 0) strikes as its attack starts.
        if (combatAnimations_->rowIndex(label) < 0) {
            return 0;
        }
        // 00414260 reads column subAttack+1 by index (hits, hit1, hit2, hit3, parry0, ...); a
        // column past the table or an empty cell leaves 0.
        const std::size_t column = static_cast<std::size_t>(std::max(0, subAttack) + 1);
        if (column >= combatAnimations_->columnCount()) {
            return 0;
        }
        const std::string hit = combatAnimations_->cell(static_cast<std::size_t>(combatAnimations_->rowIndex(label)), column);
        return hit.empty() || hit == "****" ? 0 : std::atoi(hit.c_str());
    }
    return -1;
}

bool Rules::animationLooping(std::int32_t animationId) const
{
    if (!animations_) {
        return false;
    }
    return cellInt(*animations_, std::to_string(animationId), "looping", 0) != 0;
}

// animations.2da FireForget column, the one 0063c380 reads (name at 0074bbf8, column found at 005c3451).
bool Rules::animationFireAndForget(std::int32_t animationId) const
{
    if (!animations_) {
        return false;
    }
    return cellInt(*animations_, std::to_string(animationId), "fireforget", 0) != 0;
}

// Script animation constant 0..120 -> animations.2da row (or dialog id >= 10000), for a creature out of combat stance
// (flags at +0xc5/+0xc6 of the animation base clear). Derived from action 0x540550 and the switch at 0069f650.
static const std::int32_t kScriptAnimationTable[121] = {
    6, 7, 18, 24, 24, 25, 27, 26, 29, 28, 40, 41,
    14, 15, 32, 44, 53, 54, 74, 270, 270, 77, 76, 79,
    359, 360, 81, 370, 371, 35, 375, 384, 385, 24, 16, 19,
    10036, 10037, 25, 27, 26, 29, 28, 10043, 17, 10045, 10046, 10047,
    10048, 10049, 10050, 10051, 7, 11, 10, 12, 13, 14, 15, 40,
    41, 70, 10062, 10063, 10064, 10065, 10066, 10067, 10068, 10069, 37, 10071,
    10072, 10073, 10074, 10075, 10076, 303, 10078, 10079, 10080, 10081, 10082, 10083,
    10084, 10085, 10086, 10087, 10088, 10089, 10090, 10091, 8, 1, 10094, 10095,
    10096, 10097, 10098, 10099, 11, 10, 12, 13, 16, 19, 31, 33,
    17, 17, 17, 10111, 10112, 43, 68, 38, 72, 57, 58, 346,
    34,
};

std::int32_t Rules::scriptAnimationTarget(std::int32_t scriptId)
{
    return scriptId >= 0 && scriptId <= 120 ? kScriptAnimationTable[scriptId] : scriptId;
}

std::string Rules::scriptAnimationName(std::int32_t scriptId) const
{
    const std::int32_t target = scriptAnimationTarget(scriptId);
    return target >= 10000 ? dialogAnimationName(target) : animationName(target);
}

bool Rules::scriptAnimationLooping(std::int32_t scriptId) const
{
    const std::int32_t target = scriptAnimationTarget(scriptId);
    return target >= 10000 ? dialogAnimationLooping(target) : animationLooping(target);
}

bool Rules::scriptAnimationFireAndForget(std::int32_t scriptId) const
{
    const std::int32_t target = scriptAnimationTarget(scriptId);
    return target >= 10000 ? !dialogAnimationLooping(target) : animationFireAndForget(target);
}

namespace {
std::int32_t dialogAnimationRow(std::int32_t animationId)
{
    if (animationId >= 10000) {
        return animationId - 10000;
    }
    return animationId >= 0 && animationId < 1200 ? animationId : -1;
}
} // namespace

std::string Rules::dialogAnimationName(std::int32_t animationId) const
{
    const std::int32_t rowId = dialogAnimationRow(animationId);
    if (!dialogAnimations_ || rowId < 0) {
        return {};
    }
    const std::string row = std::to_string(rowId);
    if (dialogAnimations_->rowIndex(row) < 0) {
        return {};
    }
    std::string label = dialogAnimations_->cell(row, "name");
    std::string lowered;
    for (const char c : label) {
        if (c != '_') {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    static const std::pair<const char*, const char*> kModelNames[] = {
        {"talknormal", "tlknorm"},     {"talkpleading", "tlkplead"},
        {"talkforceful", "tlkforce"},  {"talklaughing", "tlklaugh"},
        {"talksad", "tlksad"},         {"talkinjured", "talkinj"},
        {"listeninjured", "listeninj"}, {"usecomputerlp", "usecomplp"},
        {"usecomputer", "usecomp"},    {"kneeltalkangry", "kdtlkangry"},
        {"kneeltalksad", "kdtlksad"},
    };
    for (const auto& [from, to] : kModelNames) {
        if (lowered == from) {
            return to;
        }
    }
    return lowered;
}

bool Rules::dialogAnimationLooping(std::int32_t animationId) const
{
    const std::int32_t rowId = dialogAnimationRow(animationId);
    return dialogAnimations_ && rowId >= 0
        && cellInt(*dialogAnimations_, std::to_string(rowId), "looping", 0) != 0;
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

std::int32_t Rules::adjustedForceCost(std::int32_t powerId, std::int32_t goodEvil,
                                      bool partyMember) const
{
    if (!spells_) {
        return 0;
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0) {
        return 0;
    }
    // The loader keeps forcepoints in a byte (+0x14e).
    const std::int32_t base = cellInt(*spells_, row, "forcepoints", 0) & 0xff;
    if (!partyMember) {
        return base;
    }
    const std::string alignment = spells_->cell(row, "goodevil");
    const char side = alignment.empty() ? '-' : alignment.front();
    float multiplier = 1.0f;
    if ((side == 'G' || side == 'E') && forceadjust_) {
        // Unsigned divide of the signed short alignment (0059b8a0 mul 0xcccccccd).
        const std::uint32_t adjustRow = static_cast<std::uint32_t>(goodEvil) / 10u;
        const std::string text = forceadjust_->cell(std::to_string(adjustRow),
                                                    side == 'G' ? "goodcost" : "evilcost");
        try {
            if (!text.empty() && text != "****") {
                multiplier = std::stof(text);
            }
        } catch (const std::exception&) {
            multiplier = 1.0f;
        }
    }
    return static_cast<std::int32_t>(static_cast<float>(base) * multiplier);
}

Rules::SpellTimes Rules::spellTimes(std::int32_t powerId) const
{
    SpellTimes times;
    if (!spells_) {
        return times;
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0) {
        return times;
    }
    times.conjure = std::max<std::int32_t>(0, cellInt(*spells_, row, "conjtime", 0));
    times.cast = std::max<std::int32_t>(0, cellInt(*spells_, row, "casttime", 0));
    times.catchTime = std::max<std::int32_t>(0, cellInt(*spells_, row, "catchtime", 0));
    return times;
}

std::int32_t Rules::maxForcePoints(std::int32_t classId, std::int32_t level,
                                   std::int32_t wisdom, std::int32_t charisma) const
{
    // classes.2da ForceDie per level plus the WIS and CHA modifiers per level, at
    // least 1 per level; ForceDie 0 classes have no Force points. The die is applied
    // flat (not rolled), the same way hit points are awarded here.
    const std::int32_t die = classForceDie(classId);
    if (die <= 0) {
        return 0;
    }
    const std::int32_t perLevel = std::max<std::int32_t>(
        1, die + abilityModifier(wisdom) + abilityModifier(charisma));
    return std::max<std::int32_t>(1, level) * perLevel;
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
    // Retail 004ef6e0 CSWSCreature::CalculateSpellSaveDC: total class level (005a5fd0
    // GetLevel(1), minus level loss) + WIS modifier (stats +0xf2) + CHA modifier
    // (stats +0xf4) + 5, then Force Focus feats 0x5a/0x59/0x58 add 4/2/1.
    const std::int32_t levels = sheet.level + (sheet.secondClassId >= 0 ? sheet.secondClassLevel : 0);
    return levels + static_cast<std::int32_t>(abilityModifier(sheet.wis))
        + static_cast<std::int32_t>(abilityModifier(sheet.cha)) + 5 + focus;
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
    // retail 004ed1d0: 10, each class's ACBonus entry at its level (005be770 reads the 21-entry
    // table that 005bde80 fills from acbonus.2da rows 0..20, indexed by the level itself), the
    // Dexterity modifier (005a5330: capped by the worn armour's dexbonus), the armour and effect
    // terms, and Dueling (005a9f50: +1/+2/+3 for feats 113/114/115 with one right-hand weapon of
    // weaponwield 2 or 4 and nothing in the left hand). Scoundrel's Luck and Jedi Sense are no
    // separate terms: they are the class columns of acbonus.2da (scd, jdg, jdc, jds).
    std::int32_t ac = 10 + classArmorBonus(sheet.classId, sheet.level);
    if (sheet.secondClassId >= 0 && sheet.secondClassLevel > 0) {
        ac += classArmorBonus(sheet.secondClassId, sheet.secondClassLevel);
    }
    std::int32_t dex = static_cast<std::int32_t>(abilityModifier(sheet.dex));
    const InventoryItem* right = nullptr;
    const InventoryItem* left = nullptr;
    for (const InventoryItem& item : sheet.inventory) {
        if (!item.equipped || !baseitems_) {
            continue;
        }
        right = item.slot == 4 ? &item : right;
        left = item.slot == 5 ? &item : left;
        const std::string row = std::to_string(item.baseItem);
        const std::int32_t baseAc = cellInt(*baseitems_, row, "baseac", 0);
        ac += baseAc;
        if (item.slot == 1 && baseAc > 0) {
            const std::string cap = baseitems_->cell(row, "dexbonus");
            if (!cap.empty() && cap != "****") {
                dex = std::min(dex, static_cast<std::int32_t>(std::atoi(cap.c_str())));
            }
        }
    }
    ac += dex;
    if (right != nullptr && left == nullptr) {
        const std::int32_t wield = baseItemInt(right->baseItem, "weaponwield", 0);
        if (wield == 2 || wield == 4) {
            ac += sheet.feats.contains(115) ? 3 : sheet.feats.contains(114) ? 2 : sheet.feats.contains(113) ? 1 : 0;
        }
    }
    return ac;
}

std::int32_t Rules::handAttackBonus(const CharacterSheet& sheet, bool offhand, bool doubleWeapon) const
{
    const InventoryItem* right = nullptr;
    const InventoryItem* left = nullptr;
    for (const InventoryItem& item : sheet.inventory) {
        right = item.equipped && item.slot == 4 ? &item : right;
        left = item.equipped && item.slot == 5 ? &item : left;
    }
    // 005a7770: the off hand's weapon is the left slot (0x20), the right (0x10) for a double weapon.
    const InventoryItem* weapon = offhand && !doubleWeapon ? left : right;
    // 005a7770 reads the other hand from slot 0x20, or 0x10 for a double weapon (param_5): the same
    // weapon is its own off-hand end.
    const InventoryItem* other = doubleWeapon ? right : left;
    const bool ranged = weapon != nullptr && baseItemInt(weapon->baseItem, "rangedweapon", 0) != 0;
    const std::int32_t str = static_cast<std::int32_t>(abilityModifier(sheet.str));
    const std::int32_t dex = static_cast<std::int32_t>(abilityModifier(sheet.dex));
    // 005a60d0: the attack bonus table of each class at its level.
    std::int32_t total = baseAttackBonus(sheet.classId, sheet.level);
    if (sheet.secondClassId >= 0 && sheet.secondClassLevel > 0) {
        total += baseAttackBonus(sheet.secondClassId, sheet.secondClassLevel);
    }
    if (ranged) {
        total += dex; // 005a7b60
    } else {
        // 005a7770 / 005a54f0: base items 8, 9 and 10 (00555690) take Dexterity when it is higher.
        const bool finesse = weapon != nullptr
            && (weapon->baseItem == 8 || weapon->baseItem == 9 || weapon->baseItem == 10);
        total += finesse && dex > str ? dex : str;
    }
    // Weapon Focus: the weapon's own focfeat (+0xb2); bare hands have none.
    if (weapon != nullptr) {
        const std::int32_t focus = baseItemInt(weapon->baseItem, "focfeat", 0);
        if (focus != 0 && sheet.feats.contains(focus)) {
            total += 1;
        }
    }
    // 004f3fe0: the weapon's own item properties (Enhancement, Attack Bonus) are effects on the attack.
    if (weapon != nullptr) {
        std::int32_t propertyAttack = 0;
        std::int32_t propertyDamage = 0;
        weaponPropertyBonus(*weapon, propertyAttack, propertyDamage);
        total += propertyAttack;
    }
    // Two weapons. Off hand: -10, -6 with Two-Weapon Fighting (3), -4 Advanced (9), -2 Mastery
    // (85). Main hand, when the off hand holds a weapon: -6, -4 when that weapon is light
    // (004ed190: its weaponsize against the creature's, medium here) or double (weaponwield 3),
    // then +2 for feat 9 or +4 for feat 85.
    // A double weapon (param 5) takes both: 006b9970 passes param 4 = 0 (retail equip screen: a
    // Double-Bladed Sword reads -2 left and 0 right with Two-Weapon Fighting only).
    if (offhand) {
        if (weapon != nullptr) {
            total += sheet.feats.contains(85) ? -2 : sheet.feats.contains(9) ? -4 : sheet.feats.contains(3) ? -6 : -10;
        }
    } else if (other != nullptr && baseItemInt(other->baseItem, "weapontype", 0) != 0) {
        const std::int32_t relative = baseItemInt(other->baseItem, "weaponsize", 0) - 3;
        const bool light = relative < 0 || relative > 1;
        std::int32_t penalty = light || baseItemInt(other->baseItem, "weaponwield", 0) == 3 ? -4 : -6;
        penalty += sheet.feats.contains(85) ? 4 : sheet.feats.contains(9) ? 2 : 0;
        total += penalty;
    }
    if (right != nullptr && left == nullptr) {
        const std::int32_t wield = baseItemInt(right->baseItem, "weaponwield", 0);
        if (wield == 2 || wield == 4) {
            total += sheet.feats.contains(115) ? 3 : sheet.feats.contains(114) ? 2 : sheet.feats.contains(113) ? 1 : 0;
        }
    }
    return total;
}

void Rules::handDamageRange(const CharacterSheet& sheet, bool offhand, std::int32_t& low, std::int32_t& high,
                            bool doubleWeapon) const
{
    const InventoryItem* weapon = nullptr;
    for (const InventoryItem& item : sheet.inventory) {
        weapon = item.equipped && item.slot == (offhand && !doubleWeapon ? 5 : 4) ? &item : weapon;
    }
    const std::int32_t str = static_cast<std::int32_t>(abilityModifier(sheet.str));
    if (weapon == nullptr) {
        // 005a9e10 bare hands: 1 to 1 for a medium creature (1 to 2 when small), plus Strength.
        low = 1 + str;
        high = 1 + str;
    } else {
        const std::int32_t dice = baseItemInt(weapon->baseItem, "numdice", 1);
        const std::int32_t die = baseItemInt(weapon->baseItem, "dietoroll", 1);
        low = dice;
        high = dice * die;
        if (baseItemInt(weapon->baseItem, "rangedweapon", 0) == 0) {
            low += str;
            high += str;
        }
        // Weapon Specialization: the weapon's specfeat (+0xb0, 005a67d0) adds 2.
        const std::int32_t spec = baseItemInt(weapon->baseItem, "specfeat", 0);
        if (spec != 0 && sheet.feats.contains(spec)) {
            low += 2;
            high += 2;
        }
        // 005a9e10 with the effect flag: 004f3fe0 adds the item properties' damage.
        std::int32_t propertyAttack = 0;
        std::int32_t propertyDamage = 0;
        weaponPropertyBonus(*weapon, propertyAttack, propertyDamage);
        low += propertyDamage;
        high += propertyDamage;
    }
    low = std::max<std::int32_t>(1, low);
    high = std::max<std::int32_t>(1, high);
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
    return handAttackBonus(sheet, false);
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
    // 005be770 indexes the table by the level itself: row 1 at level 1 (row 0 is never read).
    return cellInt(*acbonus_, std::to_string(std::clamp(level, 0, 20)), column, 0);
}

// Item property cost value: the row's `value` column, else its numeric label.
const kotor::TwoDa* Rules::propertyCostTable(const kotor::ItemTemplate::Property& property) const
{
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
}

std::int32_t Rules::propertyCostValue(const kotor::ItemTemplate::Property& property) const
{
    const kotor::TwoDa* table = propertyCostTable(property);
    if (table == nullptr) {
        return 0;
    }
    const std::string row = std::to_string(property.costValue);
    const std::int32_t value = cellInt(*table, row, "value", INT32_MIN);
    return value != INT32_MIN ? value : cellInt(*table, row, "label", 0);
}

// What an equipped weapon's item properties add to its own to-hit and damage (the effects
// 004f3fe0 GetTotalEffectBonus sums for the attack): Enhancement (5), Attack Bonus (38), Attack
// Penalty (8, 41), Damage (11, flat or dice average), Damage Penalty (15). A property tied to an
// upgrade slot counts only when that upgrade is installed.
void Rules::weaponPropertyBonus(const InventoryItem& item, std::int32_t& attack, std::int32_t& damage) const
{
    attack = 0;
    damage = 0;
    if (install_ == nullptr || item.resref.empty()) {
        return;
    }
    auto [cached, added] = itemTemplates_.try_emplace(item.resref);
    if (added) {
        cached->second = kotor::loadItemTemplate(*install_, item.resref);
    }
    if (!cached->second.has_value()) {
        return;
    }
    for (const kotor::ItemTemplate::Property& property : cached->second->properties) {
        if (property.upgradeType != 255
            && (property.upgradeType >= 32 || ((item.upgrades >> property.upgradeType) & 1u) == 0)) {
            continue;
        }
        switch (property.name) {
        case 5:
            attack += propertyCostValue(property);
            damage += propertyCostValue(property);
            break;
        case 38:
            attack += propertyCostValue(property);
            break;
        case 8:
        case 41:
            attack -= std::abs(propertyCostValue(property));
            break;
        case 11: {
            const kotor::TwoDa* table = propertyCostTable(property);
            if (table != nullptr) {
                const std::string cost = std::to_string(property.costValue);
                const std::int32_t dice = cellInt(*table, cost, "numdice", 0);
                const std::int32_t die = cellInt(*table, cost, "die", 0);
                damage += dice > 0 && die > 0 ? dice * (die + 1) / 2 : cellInt(*table, cost, "label", 0);
            }
            break;
        }
        case 15:
            damage -= std::abs(propertyCostValue(property));
            break;
        default:
            break;
        }
    }
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
    const auto costValue = [&](const kotor::ItemTemplate::Property& property) {
        return propertyCostTable(property);
    };
    const auto propertyValue = [&](const kotor::ItemTemplate::Property& property) {
        return propertyCostValue(property);
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
    const std::string prefix = classColumnPrefix(classId);
    if (!skills_ || prefix.empty() || skillIndex < 0
        || skillIndex >= static_cast<std::int32_t>(skills_->rowCount())) {
        return false;
    }
    const std::string row = skills_->rowLabel(static_cast<std::size_t>(skillIndex));
    return cellInt(*skills_, row, prefix + "_class", 0) != 0;
}

std::int32_t Rules::skillRankCost(std::int32_t classId, std::int32_t skillIndex) const
{
    return skillIsClassSkill(classId, skillIndex) ? 1 : 2;
}

std::int32_t Rules::maxSkillRank(std::int32_t classId, std::int32_t skillIndex,
                                 std::int32_t level) const
{
    // Class skills rise to level + 3 ranks, cross-class skills to half of that.
    const std::int32_t cap = std::max<std::int32_t>(1, level) + 3;
    return skillIsClassSkill(classId, skillIndex) ? cap : cap / 2;
}

std::array<std::int32_t, 6> Rules::recommendedAbilities(std::int32_t classId) const
{
    std::array<std::int32_t, 6> result{{8, 8, 8, 8, 8, 8}};
    if (!classes_ || classId < 0 || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return result;
    }
    const std::string row = std::to_string(classId);
    static const char* const columns[6] = {"str", "dex", "con", "int", "wis", "cha"};
    for (std::size_t i = 0; i < 6; ++i) {
        result[i] = cellInt(*classes_, row, columns[i], 8);
    }
    return result;
}

std::int32_t Rules::skillRecommendRank(std::int32_t classId, std::int32_t skillIndex) const
{
    const std::string prefix = classColumnPrefix(classId);
    if (!skills_ || prefix.empty() || skillIndex < 0 || skillIndex >= static_cast<std::int32_t>(skills_->rowCount())) {
        return 0;
    }
    return cellInt(*skills_, skills_->rowLabel(static_cast<std::size_t>(skillIndex)), prefix + "_reco", 0);
}

std::int32_t Rules::featRecommendRank(std::int32_t classId, std::int32_t featId) const
{
    const std::string prefix = classColumnPrefix(classId);
    if (!feat_ || prefix.empty() || featId < 0 || featId >= static_cast<std::int32_t>(feat_->rowCount())) {
        return 0;
    }
    return cellInt(*feat_, feat_->rowLabel(static_cast<std::size_t>(featId)), prefix + "_recom", 0);
}

namespace {

// Retail's three lookups walk one table: an exact rank wins, otherwise the walk has counted the rows
// with no rank and the (rank+1)th of them stands in (the last one when there are fewer).
template <typename RankOf>
std::int32_t rowForRecommendRank(std::int32_t rows, std::int32_t rank, RankOf rankOf)
{
    std::int32_t fallback = -1;
    std::int32_t unranked = -1;
    for (std::int32_t row = 0; row < rows; ++row) {
        const std::int32_t value = rankOf(row);
        if (value == rank) {
            return row;
        }
        if (value == 0 && unranked != rank) {
            fallback = row;
            ++unranked;
        }
    }
    return fallback;
}

} // namespace

std::int32_t Rules::skillForRecommendRank(std::int32_t classId, std::int32_t rank) const
{
    return rowForRecommendRank(skills_ ? static_cast<std::int32_t>(skills_->rowCount()) : 0, rank,
                               [&](std::int32_t row) { return skillRecommendRank(classId, row); });
}

std::int32_t Rules::featForRecommendRank(std::int32_t classId, std::int32_t rank) const
{
    // Inferred: the class's feat table holds the feats its _list column offers; its loader was not read.
    return rowForRecommendRank(feat_ ? static_cast<std::int32_t>(feat_->rowCount()) : 0, rank, [&](std::int32_t row) {
        if (!featIsAvailable(classId, row)) {
            return -1;
        }
        return featRecommendRank(classId, row);
    });
}

std::int32_t Rules::powerForRecommendRank(bool darkSide, std::int32_t rank) const
{
    // 005be6d0 walks only rows of user type 1 or 5, the offset by the alignment argument.
    return rowForRecommendRank(spells_ ? static_cast<std::int32_t>(spells_->rowCount()) : 0, rank, [&](std::int32_t row) {
        const std::string label = spells_->rowLabel(static_cast<std::size_t>(row));
        const std::int32_t userType = cellInt(*spells_, label, "usertype", 0);
        if (userType != 1 && userType != 5) {
            return -1;
        }
        return cellInt(*spells_, label, darkSide ? "dark_recom" : "light_recom", 0);
    });
}

std::int32_t Rules::primaryAbility(std::int32_t classId) const
{
    if (!classes_ || classId < 0 || classId >= static_cast<std::int32_t>(classes_->rowCount())) {
        return -1;
    }
    const std::string value = lower(classes_->cell(std::to_string(classId), "primaryabil"));
    static const char* const names[6] = {"str", "dex", "con", "int", "wis", "cha"};
    for (std::int32_t i = 0; i < 6; ++i) {
        if (value == names[i]) {
            return i;
        }
    }
    return -1;
}

std::int32_t Rules::powersForLevel(std::int32_t classId, std::int32_t level) const
{
    const std::string prefix = classColumnPrefix(classId);
    if (prefix != "jcn" && prefix != "jsn" && prefix != "jgd") {
        return 0;
    }
    return classPowerGain_ && level >= 1 && level <= static_cast<std::int32_t>(classPowerGain_->rowCount())
        ? cellInt(*classPowerGain_, classPowerGain_->rowLabel(static_cast<std::size_t>(level - 1)), prefix, 0)
        : 0;
}

bool Rules::canLearnPowerAtLevelUp(const CharacterSheet& sheet, std::int32_t powerId) const
{
    if (!spells_) {
        return false;
    }
    const std::string row = std::to_string(powerId);
    if (spells_->rowIndex(row) < 0) {
        return false;
    }
    const std::int32_t userType = cellInt(*spells_, row, "usertype", 0);
    if ((userType != 1 && userType != 5) || sheet.powers.contains(powerId)) {
        return false;
    }
    std::stringstream prerequisites(spells_->cell(row, "prerequisites"));
    std::string one;
    while (std::getline(prerequisites, one, '_')) {
        if (!one.empty() && one != "****" && !sheet.powers.contains(std::atoi(one.c_str()))) {
            return false;
        }
    }
    const std::string prefix = classColumnPrefix(sheet.classId);
    const char* column = prefix == "jgd" ? "guardian" : prefix == "jcn" ? "consular" : prefix == "jsn" ? "sentinel" : nullptr;
    return column != nullptr && sheet.level >= cellInt(*spells_, row, column, 0);
}

bool Rules::featIsAvailable(std::int32_t classId, std::int32_t featId) const
{
    const std::string prefix = classColumnPrefix(classId);
    if (!feat_ || prefix.empty() || featId < 0
        || featId >= static_cast<std::int32_t>(feat_->rowCount())) {
        return false;
    }
    const std::string row = feat_->rowLabel(static_cast<std::size_t>(featId));
    // <prefix>_list, RETAIL-PROVEN by its reader 005bd0f0 (the class's feat table): 4 leaves the feat out
    // of the class entirely, 0, 1 and 2 put it in the class's list (types 1, 3 and 2), 3 is the granted
    // type 0. So the general feats (sol_list 0: Caution, Empathy, Gearhead, Conditioning, Implant, Iron
    // Will, Lightning Reflexes) are offered too (paired frame levelup_feats shows them). A granted feat stays
    // selectable only when it carries a recommendation rank (inferred).
    const std::int32_t list = cellInt(*feat_, row, prefix + "_list", 4);
    if (list == 0 || list == 1 || list == 2) {
        return true;
    }
    if (list == 3) {
        const std::string recom = feat_->cell(row, prefix + "_recom");
        return !recom.empty() && recom != "****";
    }
    return false;
}

std::vector<std::int32_t> Rules::pcPortraitRows(std::int32_t gender) const
{
    std::vector<std::int32_t> rows;
    if (!portraits_) {
        return rows;
    }
    for (std::size_t row = 0; row < portraits_->rowCount(); ++row) {
        const std::string label = portraits_->rowLabel(row);
        if (cellInt(*portraits_, label, "forpc", 0) == 1
            && cellInt(*portraits_, label, "sex", -1) == gender) {
            rows.push_back(static_cast<std::int32_t>(row));
        }
    }
    return rows;
}

PortraitInfo Rules::portraitRow(std::int32_t row) const
{
    PortraitInfo info;
    if (!portraits_ || row < 0 || row >= static_cast<std::int32_t>(portraits_->rowCount())) {
        return info;
    }
    const std::string label = portraits_->rowLabel(static_cast<std::size_t>(row));
    info.row = row;
    info.resref = portraits_->cell(label, "baseresref");
    info.appearance = static_cast<std::uint32_t>(cellInt(*portraits_, label, "appearancenumber", 0));
    info.appearanceSmall = static_cast<std::uint32_t>(cellInt(*portraits_, label, "appearance_s", 0));
    info.appearanceLarge = static_cast<std::uint32_t>(cellInt(*portraits_, label, "appearance_l", 0));
    info.sex = cellInt(*portraits_, label, "sex", 0);
    static const char* columns[4] = {"baseresrefe", "baseresrefve", "baseresrefvve",
                                     "baseresrefvvve"};
    for (int i = 0; i < 4; ++i) {
        const std::string value = portraits_->cell(label, columns[i]);
        info.darkVariants[i] = value == "****" ? std::string{} : value;
    }
    return info;
}

std::string Rules::portraitResrefFor(const PortraitInfo& info, std::int32_t alignment) const
{
    // retail 00647e80: alignment <= 10 uses BaseResRefVVVE, 11..20 VVE, 21..30 VE,
    // 31..40 E; anything higher (or a missing variant) uses BaseResRef.
    int variant = -1;
    if (alignment < 11) {
        variant = 3;
    } else if (alignment < 21) {
        variant = 2;
    } else if (alignment < 31) {
        variant = 1;
    } else if (alignment < 41) {
        variant = 0;
    }
    if (variant >= 0 && !info.darkVariants[variant].empty()) {
        return info.darkVariants[variant];
    }
    return info.resref;
}

std::int32_t Rules::portraitAppearance(std::int32_t row, std::int32_t classId) const
{
    const PortraitInfo info = portraitRow(row);
    if (info.row < 0) {
        return -1;
    }
    switch (classId) {
    case 0: return static_cast<std::int32_t>(info.appearanceLarge);
    case 2: return static_cast<std::int32_t>(info.appearanceSmall);
    default: return static_cast<std::int32_t>(info.appearance);
    }
}

std::int32_t Rules::portraitRowForAppearance(std::int32_t appearance) const
{
    if (!portraits_ || appearance < 0) {
        return -1;
    }
    for (std::size_t row = 0; row < portraits_->rowCount(); ++row) {
        const std::string label = portraits_->rowLabel(row);
        if (cellInt(*portraits_, label, "forpc", 0) != 1) {
            continue;
        }
        for (const char* column : {"appearancenumber", "appearance_s", "appearance_l"}) {
            if (cellInt(*portraits_, label, column, -1) == appearance) {
                return static_cast<std::int32_t>(row);
            }
        }
    }
    return -1;
}

} // namespace ke
