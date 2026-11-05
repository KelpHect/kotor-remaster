// Party table, available NPCs, leader, solo mode, the party-selection state and
// party persistence. The model follows retail's PartyTable (recovered from the
// 1.0.3.0 binary; the handlers are ACT_11/13/574-577/694-699/708/709/712/734/
// 753/767 at 00543xxx-00547xxx and the table methods near 00562c40-00565xxx):
//   - nine "available NPC" slots indexed by the nwscript NPC_* constants, which
//     are also the npc.2da rows (Bastila 0 .. Zaalbar 8); the PC is -1;
//   - a member list of NPC ids only (the PC is not a member), at most two;
//   - a leader / controlled NPC, a solo flag, and a shared XP pool;
//   - XP goes to the pool, the PC and every member; an NPC creature scales the
//     award by its npc.2da PercentXP (80 for every companion).
// Anything the binary did not settle is marked "engine rule" or "unverified".

#include "game/GameSession.h"
#include "game/ScriptHost.h"
#include "core/Log.h"
#include "kotor/GffWrite.h"
#include "kotor/SaveGame.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ostream>

namespace ke {

namespace {

constexpr std::uint32_t kUtcType = 2027;
constexpr std::uint32_t kTwoDaType = 2017;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr float kPi = 3.14159265358979f;

std::string lowerStr(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool validNpc(int npc)
{
    return npc >= 0 && npc < kNpcCount;
}

std::vector<std::string> splitText(const std::string& value, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t cut = value.find(separator, start);
        parts.push_back(value.substr(start, cut == std::string::npos ? std::string::npos
                                                                      : cut - start));
        if (cut == std::string::npos) {
            break;
        }
        start = cut + 1;
    }
    return parts;
}

// Save fields are '|' separated, one record per line.
std::string cleanField(std::string text)
{
    for (char& c : text) {
        if (c == '|' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

std::int32_t gffInt(const kotor::Gff& gff, std::uint32_t structure, const char* label,
                    std::int32_t fallback)
{
    const kotor::GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.integer(*field) : fallback;
}

std::string gffText(const kotor::Gff& gff, std::uint32_t structure, const char* label)
{
    const kotor::GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.text(*field) : std::string{};
}

int lowestBit(std::uint32_t mask)
{
    for (int bit = 0; bit < 32 && mask != 0; ++bit) {
        if ((mask & (1u << bit)) != 0) {
            return bit;
        }
    }
    return -1;
}

std::int32_t clampI32(std::int64_t value)
{
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX));
}

} // namespace

// ---- identity and static data -------------------------------------------------

int GameSession::npcIdForTag(const std::string& tag)
{
    // The nwscript NPC_* order. The npc.2da `tag` column lists the same names
    // (row 7 reads "T3M3" while the template says T3M4); the family gate checks
    // every row against this mapping.
    static const std::pair<const char*, int> names[] = {
        {"bastila", kNpcBastila}, {"cand", kNpcCanderous},  {"canderous", kNpcCanderous},
        {"carth", kNpcCarth},     {"hk47", kNpcHk47},       {"hk-47", kNpcHk47},
        {"jolee", kNpcJolee},     {"juhani", kNpcJuhani},   {"mission", kNpcMission},
        {"t3m4", kNpcT3m4},       {"t3-m4", kNpcT3m4},      {"t3m3", kNpcT3m4},
        {"zaalbar", kNpcZaalbar},
    };
    const std::string wanted = lowerStr(tag);
    for (const auto& [name, id] : names) {
        if (wanted == name) {
            return id;
        }
    }
    return kNpcPlayer;
}

// npc.2da (PercentXP per NPC) and the default template per NPC, found by scanning
// the shipped p_* creature templates for the NPC's tag. This replaces the old
// hard-coded tag -> template map.
void GameSession::loadPartyTables()
{
    if (party_.tablesLoaded) {
        return;
    }
    party_.tablesLoaded = true;
    if (auto data = install_.find("npc", kTwoDaType); data.has_value()) {
        try {
            const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "npc");
            for (int npc = 0; npc < kNpcCount && static_cast<std::size_t>(npc) < table.rowCount();
                 ++npc) {
                const std::string percent = table.cell(static_cast<std::size_t>(npc),
                                                       static_cast<std::size_t>(
                                                           std::max(0, table.columnIndex("percentxp"))));
                if (!percent.empty() && percent != "****") {
                    party_.xpPercent[static_cast<std::size_t>(npc)] = std::atoi(percent.c_str());
                }
                const int tagColumn = table.columnIndex("tag");
                if (tagColumn >= 0) {
                    party_.npcTag[static_cast<std::size_t>(npc)] =
                        table.cell(static_cast<std::size_t>(npc),
                                   static_cast<std::size_t>(tagColumn));
                }
            }
        } catch (const kotor::FormatError&) {
        }
    }
    // Only the shipped global templates (KEY/BIF) are candidates: a module-local
    // creature such as p_carth001 resolves in one module and nowhere else, and the
    // table is filled once, whichever module happens to be mounted. The shortest
    // resref is the NPC's plain template.
    for (const kotor::KeyEntry& entry : install_.key().entries()) {
        if (entry.type != kUtcType || lowerStr(entry.resref).rfind("p_", 0) != 0) {
            continue;
        }
        const std::string name = lowerStr(entry.resref);
        auto data = install_.find(name, kUtcType);
        if (!data.has_value()) {
            continue;
        }
        try {
            const kotor::Gff utc = kotor::Gff::parse(std::move(*data), name + ".utc");
            const int npc = npcIdForTag(gffText(utc, utc.rootIndex(), "Tag"));
            if (!validNpc(npc)) {
                continue;
            }
            std::string& best = party_.defaultTemplate[static_cast<std::size_t>(npc)];
            if (best.empty() || name.size() < best.size()) {
                best = name;
            }
        } catch (const kotor::FormatError&) {
        }
    }
}

std::string GameSession::templateForNpc(int npc)
{
    if (!validNpc(npc)) {
        return {};
    }
    loadPartyTables();
    return party_.defaultTemplate[static_cast<std::size_t>(npc)];
}

// Share of an award an NPC creature receives: npc.2da PercentXP / 100. The
// GAME_XP_General row scales the base award for everybody before this (see
// Rules::killXp), so it is not applied again here.
float GameSession::npcXpFactor(int npc)
{
    if (!validNpc(npc)) {
        return 1.0f;
    }
    loadPartyTables();
    return static_cast<float>(party_.xpPercent[static_cast<std::size_t>(npc)]) * 0.01f;
}

std::size_t GameSession::entityOfObject(std::uint32_t object) const
{
    if (object == script::kObjectInvalid || object == 0) {
        return kNone;
    }
    for (std::size_t i = 0; i < entityObjects_.size() && i < scene_.entities.size(); ++i) {
        if (entityObjects_[i] == object) {
            return i;
        }
    }
    return kNone;
}

CompanionState* GameSession::companionFor(int npc)
{
    if (!validNpc(npc)) {
        return nullptr;
    }
    const std::string tag = lowerStr(party_.npcs[static_cast<std::size_t>(npc)].tag);
    for (CompanionState& companion : companions_) {
        if (lowerStr(companion.tag) == tag) {
            return &companion;
        }
    }
    return nullptr;
}

// The sheet that is current for an NPC: the member's live sheet, else the stored one.
const CharacterSheet* GameSession::liveSheet(int npc) const
{
    if (!validNpc(npc)) {
        return nullptr;
    }
    const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    const std::string tag = lowerStr(slot.tag);
    if (isNpcPartyMember(npc)) {
        for (const CompanionState& companion : companions_) {
            if (lowerStr(companion.tag) == tag) {
                return &companion.sheet;
            }
        }
    }
    return slot.hasSheet ? &slot.sheet : nullptr;
}

// A companion sheet from the NPC's UTC: class, level, abilities, skills, feats,
// powers and worn items are the template's own, not a generated character.
bool GameSession::loadNpcSheet(int npc)
{
    if (!validNpc(npc)) {
        return false;
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    if (slot.templateResref.empty()) {
        return false;
    }
    auto data = install_.find(slot.templateResref, kUtcType);
    if (!data.has_value()) {
        return false;
    }
    try {
        const kotor::Gff utc = kotor::Gff::parse(std::move(*data), slot.templateResref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        std::int32_t classId = 0;
        std::int32_t level = 0;
        std::vector<std::int32_t> powers;
        bool firstClass = true;
        if (const kotor::GffField* classes = utc.field(root, "ClassList"); classes != nullptr) {
            for (const std::uint32_t entry : utc.list(*classes)) {
                if (firstClass) {
                    classId = gffInt(utc, entry, "Class", 0);
                    firstClass = false;
                }
                level += std::max(1, gffInt(utc, entry, "ClassLevel", 1));
                if (const kotor::GffField* known = utc.field(entry, "KnownList0");
                    known != nullptr) {
                    for (const std::uint32_t spell : utc.list(*known)) {
                        powers.push_back(gffInt(utc, spell, "Spell", -1));
                    }
                }
            }
        }
        level = std::max(1, level);
        CharacterSheet sheet = rules_.loaded() ? rules_.createCharacter(classId, level)
                                               : CharacterSheet{};
        sheet.classId = classId;
        sheet.level = level;
        sheet.str = gffInt(utc, root, "Str", sheet.str);
        sheet.dex = gffInt(utc, root, "Dex", sheet.dex);
        sheet.con = gffInt(utc, root, "Con", sheet.con);
        sheet.intel = gffInt(utc, root, "Int", sheet.intel);
        sheet.wis = gffInt(utc, root, "Wis", sheet.wis);
        sheet.cha = gffInt(utc, root, "Cha", sheet.cha);
        sheet.alignment = std::clamp(gffInt(utc, root, "GoodEvil", 50), 0, 100);
        // A UTC carries no experience; the level's threshold is the floor.
        sheet.xp = rules_.loaded() ? rules_.xpForLevel(level) : 0;
        sheet.maxHp = std::max(1, gffInt(utc, root, "MaxHitPoints", 1));
        sheet.hp = std::clamp(gffInt(utc, root, "CurrentHitPoints",
                                     gffInt(utc, root, "HitPoints", sheet.maxHp)),
                              1, sheet.maxHp);
        sheet.maxFp = std::max(0, gffInt(utc, root, "ForcePoints", 0));
        sheet.fp = std::clamp(gffInt(utc, root, "CurrentForce", sheet.maxFp), 0, sheet.maxFp);
        sheet.skillPoints = 0;
        sheet.featPoints = 0;
        sheet.attributePoints = 0;
        sheet.feats.clear();
        sheet.powers.clear();
        for (const std::int32_t power : powers) {
            if (power >= 0) {
                sheet.powers.insert(power);
            }
        }
        if (const kotor::GffField* feats = utc.field(root, "FeatList"); feats != nullptr) {
            for (const std::uint32_t entry : utc.list(*feats)) {
                sheet.feats.insert(gffInt(utc, entry, "Feat", 0));
            }
        }
        if (const kotor::GffField* skills = utc.field(root, "SkillList"); skills != nullptr) {
            const std::vector<std::uint32_t> rows = utc.list(*skills);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rules_.loaded() && static_cast<std::int32_t>(i) < rules_.skillCount()) {
                    sheet.skills[rules_.skillName(static_cast<std::int32_t>(i))] =
                        gffInt(utc, rows[i], "Rank", 0);
                }
            }
        }
        std::string name;
        if (const kotor::GffField* first = utc.field(root, "FirstName"); first != nullptr) {
            const std::uint32_t ref = utc.locStringStrref(*first);
            if (ref != UINT32_MAX && tlk_.has_value()) {
                name = tlk_->text(ref);
            }
            if (name.empty()) {
                name = utc.locString(*first);
            }
        }
        slot.tag = gffText(utc, root, "Tag");
        if (slot.tag.empty()) {
            slot.tag = slot.templateResref;
        }
        sheet.name = name.empty() ? slot.tag : name;
        sheet.inventory.clear();
        if (const kotor::GffField* worn = utc.field(root, "Equip_ItemList"); worn != nullptr) {
            for (const std::uint32_t entry : utc.list(*worn)) {
                const std::string resref = gffText(utc, entry, "EquippedRes");
                if (resref.empty()) {
                    continue;
                }
                const auto templ = kotor::loadItemTemplate(install_, resref);
                InventoryItem item = rules_.loaded()
                    ? rules_.makeItem(resref, templ ? templ->baseItem : 0)
                    : InventoryItem{};
                item.resref = lowerStr(resref);
                if (templ) {
                    item.baseItem = templ->baseItem;
                    item.name = templ->name.empty() ? item.name : templ->name;
                    item.cost = templ->cost;
                }
                item.equipped = true;
                item.slot = lowestBit(utc.structure(entry).typeId);
                sheet.inventory.push_back(std::move(item));
            }
        }
        slot.sheet = std::move(sheet);
        slot.hasSheet = true;
        slot.name = slot.sheet.name;
        return true;
    } catch (const kotor::FormatError&) {
        return false;
    }
}

// ---- entities -------------------------------------------------------------------

// Finds or creates the live creature of an NPC and binds it to the NPC's script
// object. A creature the module already has under the NPC's tag is adopted; a
// stale party avatar of the same tag is retired so tag lookups stay unambiguous.
std::size_t GameSession::ensureNpcEntity(int npc, const kotor::Vec3* position, float facing,
                                         bool* created)
{
    if (created != nullptr) {
        *created = false;
    }
    if (!validNpc(npc) || !party_.npcs[static_cast<std::size_t>(npc)].available) {
        return kNone;
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    const std::string wanted = lowerStr(slot.tag);
    std::size_t index = kNone;
    if (slot.object != script::kObjectInvalid) {
        const std::size_t own = entityOfObject(slot.object);
        if (own != kNone && !scene_.entities[own].dead) {
            index = own;
        }
    }
    if (index == kNone && !wanted.empty()) {
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& candidate = scene_.entities[i];
            if (i != playerEntity_ && candidate.kind == kotor::EntityKind::Creature
                && !candidate.dead && lowerStr(candidate.tag) == wanted) {
                index = i;
                break;
            }
        }
    }
    bool spawned = false;
    if (index == kNone) {
        if (slot.templateResref.empty()) {
            return kNone;
        }
        kotor::Vec3 where;
        if (position != nullptr) {
            where = *position;
        } else {
            std::size_t rank = 0;
            for (std::size_t i = 0; i < party_.members.size(); ++i) {
                if (party_.members[i] == npc) {
                    rank = i;
                }
            }
            const kotor::Vec3 forward{std::cos(yaw_), std::sin(yaw_), 0.0f};
            const kotor::Vec3 right{-forward.y, forward.x, 0.0f};
            const float side = (rank % 2 == 0) ? -1.2f : 1.2f;
            const float row = static_cast<float>(rank / 2);
            where = position_ - forward * (2.0f + row * 1.1f) + right * side;
        }
        // A template only the earlier module could resolve (p_carth001 and the like)
        // is replaced by the NPC's global one; the stored sheet carries the state.
        if (!install_.find(slot.templateResref, kUtcType).has_value()) {
            if (const std::string global = templateForNpc(npc); !global.empty()) {
                slot.templateResref = global;
            }
        }
        index = kotor::spawnCreature(scene_, install_, slot.templateResref, where,
                                     position != nullptr ? facing : yaw_);
        if (index == kNone) {
            return kNone;
        }
        spawned = true;
        if (created != nullptr) {
            *created = true;
        }
        while (entityObjects_.size() < scene_.entities.size()) {
            entityObjects_.push_back(0);
        }
    } else if (position != nullptr) {
        scene_.entities[index].position = *position;
        scene_.entities[index].bearing = facing;
    }
    kotor::Entity& entity = scene_.entities[index];
    if (spawned) {
        entity.partyAvatar = true;
        entity.hostile = false;
        entity.faction = 0;
    }
    if (entity.tag.empty()) {
        entity.tag = slot.tag;
    }
    CharacterSheet* sheet = nullptr;
    if (CompanionState* member = companionFor(npc); member != nullptr) {
        sheet = &member->sheet;
    } else if (slot.hasSheet) {
        sheet = &slot.sheet;
    }
    if (sheet != nullptr) {
        entity.maxHitPoints = std::max(1, sheet->maxHp);
        entity.hitPoints = std::clamp(sheet->hp, 1, entity.maxHitPoints);
        if (!sheet->name.empty()) {
            entity.name = sheet->name;
        }
        slot.lastEntityHp = entity.hitPoints;
        slot.lastSheetHp = sheet->hp;
    }
    // Object binding. The id a creature had before a module change is reused.
    std::uint32_t id = script::kObjectInvalid;
    if (const std::uint32_t current = objectForEntity(index);
        current != 0 && scriptWorld_.get(current) != nullptr) {
        id = current;
    } else if (slot.object != script::kObjectInvalid && scriptWorld_.get(slot.object) != nullptr) {
        id = slot.object;
    }
    script::GameObject* object = id != script::kObjectInvalid ? scriptWorld_.get(id) : nullptr;
    if (object == nullptr) {
        object = &scriptWorld_.addObject(entity.tag.empty() ? slot.tag : entity.tag, 100, 0, true);
        id = object->id;
    }
    for (std::size_t other = 0; other < entityObjects_.size(); ++other) {
        if (other != index && entityObjects_[other] == id) {
            entityObjects_[other] = 0;
            kotor::Entity& stale = scene_.entities[other];
            if (stale.partyAvatar && stale.dead) {
                stale.tag.clear();
            }
        }
    }
    if (spawned) {
        // Older avatars of this NPC that were hidden when it left the party.
        for (std::size_t other = 0; other < scene_.entities.size(); ++other) {
            kotor::Entity& stale = scene_.entities[other];
            if (other != index && stale.partyAvatar && stale.dead
                && lowerStr(stale.tag) == wanted) {
                stale.tag.clear();
            }
        }
    }
    entityObjects_[index] = id;
    object->valid = true;
    object->tag = entity.tag.empty() ? slot.tag : entity.tag;
    object->position = {entity.position.x, entity.position.y, entity.position.z};
    object->hp = entity.hitPoints;
    object->attrs["object_type"] = static_cast<std::int64_t>(1);
    object->attrs["faction"] = static_cast<std::int64_t>(entity.faction);
    object->attrs["ac"] = static_cast<std::int64_t>(
        sheet != nullptr && rules_.loaded() ? rules_.armorClass(*sheet) : entity.defense);
    slot.object = id;
    syncEntityToScriptWorld(index);
    if (spawned) {
        logEvent("party avatar spawn " + slot.tag + " utc=" + slot.templateResref);
    }
    return index;
}

std::size_t GameSession::ensureCompanionEntity(const CompanionState& companion)
{
    const int npc = npcIdForTag(companion.tag);
    if (!validNpc(npc)) {
        return findEntity(companion.tag);
    }
    return ensureNpcEntity(npc, nullptr, yaw_);
}

// ---- available NPCs ---------------------------------------------------------------

bool GameSession::isAvailableNpc(int npc) const
{
    return validNpc(npc) && party_.npcs[static_cast<std::size_t>(npc)].available;
}

bool GameSession::addAvailableNpcByTemplate(int npc, const std::string& templateResref)
{
    if (!validNpc(npc) || isAvailableNpc(npc) || templateResref.empty()) {
        return false;
    }
    PartyNpc fresh;
    fresh.available = true;
    fresh.templateResref = lowerStr(templateResref);
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    slot = std::move(fresh);
    if (!loadNpcSheet(npc)) {
        slot = PartyNpc{};
        return false;
    }
    logEvent("party available " + slot.tag + " (" + slot.templateResref + ")");
    return true;
}

bool GameSession::addAvailableNpcByObject(int npc, std::uint32_t creature)
{
    if (!validNpc(npc) || isAvailableNpc(npc)) {
        return false;
    }
    const std::size_t index = entityOfObject(creature);
    if (index == kNone || scene_.entities[index].kind != kotor::EntityKind::Creature) {
        return false;
    }
    std::string templ = scene_.entities[index].templateResref;
    if (templ.empty() || !install_.find(templ, kUtcType).has_value()) {
        templ = templateForNpc(npc);
    }
    if (!addAvailableNpcByTemplate(npc, templ)) {
        return false;
    }
    // The creature is the NPC's body: remember it and take its current health.
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    slot.object = creature;
    const kotor::Entity& entity = scene_.entities[index];
    slot.sheet.maxHp = std::max(1, entity.maxHitPoints);
    slot.sheet.hp = std::clamp(entity.hitPoints, 1, slot.sheet.maxHp);
    return true;
}

void GameSession::setAvailableNpcObject(int npc, std::uint32_t object)
{
    if (isAvailableNpc(npc)) {
        party_.npcs[static_cast<std::size_t>(npc)].object = object;
    }
}

bool GameSession::removeAvailableNpc(int npc)
{
    if (!isAvailableNpc(npc)) {
        return false;
    }
    removePartyNpc(npc);
    party_.npcs[static_cast<std::size_t>(npc)] = PartyNpc{};
    return true;
}

// Creates the NPC's creature at a position, or moves the one it already has.
std::uint32_t GameSession::spawnAvailableNpc(int npc, kotor::Vec3 position, float facing)
{
    if (!isAvailableNpc(npc)) {
        return script::kObjectInvalid;
    }
    bool created = false;
    const std::size_t index = ensureNpcEntity(npc, &position, facing, &created);
    if (index == kNone) {
        return script::kObjectInvalid;
    }
    catchUpNpcXp(npc);
    if (created) {
        runHook("ScriptSpawn", index, scriptWorld_.pc);
    }
    return objectForEntity(index);
}

// ---- membership --------------------------------------------------------------------

bool GameSession::isNpcPartyMember(int npc) const
{
    return std::find(party_.members.begin(), party_.members.end(), npc) != party_.members.end();
}

// A non-member's XP catches up to its share of the party pool when it appears
// (retail credits the pool share a creature missed while it was away; the exact
// rounding is partly lost in the binary's float conversion - truncation here).
void GameSession::catchUpNpcXp(int npc)
{
    if (!validNpc(npc) || !rules_.loaded()) {
        return;
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    CompanionState* member = companionFor(npc);
    CharacterSheet* sheet = member != nullptr ? &member->sheet : (slot.hasSheet ? &slot.sheet : nullptr);
    if (sheet == nullptr) {
        return;
    }
    loadPartyTables();
    const std::int64_t share = static_cast<std::int64_t>(party_.xpPool)
        * party_.xpPercent[static_cast<std::size_t>(npc)] / 100;
    if (share > sheet->xp) {
        sheet->xp = clampI32(share);
    } else {
        slot.joiningXp = std::max<std::int32_t>(slot.joiningXp, sheet->xp - clampI32(share));
    }
    const std::int32_t level = rules_.levelForXp(sheet->xp);
    if (level > sheet->level) {
        autoLevelCompanion(*sheet, level);
    }
}

bool GameSession::addPartyNpc(int npc, std::uint32_t creature)
{
    if (!validNpc(npc) || isNpcPartyMember(npc)
        || static_cast<int>(party_.members.size()) >= kMaxPartyNpcs) {
        return false;
    }
    const std::size_t index = entityOfObject(creature);
    if (index == kNone || scene_.entities[index].kind != kotor::EntityKind::Creature
        || creature == scriptWorld_.pc) {
        return false;
    }
    if (!isAvailableNpc(npc)) {
        // Scripts add the NPC first; an unlisted one is made available from the
        // creature's own template rather than refused.
        if (!addAvailableNpcByObject(npc, creature)) {
            return false;
        }
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    if (!slot.hasSheet && !loadNpcSheet(npc)) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[index];
    slot.object = creature;
    slot.downed = false;
    if (entity.tag.empty()) {
        entity.tag = slot.tag;
    }
    CompanionState companion;
    companion.tag = entity.tag;
    companion.sheet = slot.sheet;
    companions_.push_back(std::move(companion));
    party_.members.push_back(npc);
    entity.hostile = false;
    entity.faction = 0;
    catchUpNpcXp(npc);
    ensureNpcEntity(npc, nullptr, yaw_);
    syncPartyViews();
    logEvent("party join " + slot.tag);
    return true;
}

bool GameSession::removePartyNpc(int npc)
{
    if (!validNpc(npc) || !isNpcPartyMember(npc)) {
        return false;
    }
    saveNpcState(npc);
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    const std::string tag = lowerStr(slot.tag);
    std::erase(party_.members, npc);
    std::erase_if(companions_, [&](const CompanionState& companion) {
        return lowerStr(companion.tag) == tag;
    });
    if (party_.leader == npc) {
        party_.leader = kNpcPlayer;
    }
    const bool wasControlled = party_.controlled == npc;
    if (wasControlled) {
        party_.controlled = kNpcPlayer;
    }
    // The creature stays in the world as an ordinary NPC (retail leaves it to the
    // scripts); it is no longer the player's avatar, so a module snapshot keeps it.
    if (const std::size_t index = entityOfObject(slot.object); index != kNone) {
        scene_.entities[index].partyAvatar = false;
    }
    syncPartyViews();
    if (wasControlled) {
        onControlChanged(npc, kNpcPlayer);
    }
    logEvent("party leave " + slot.tag);
    return true;
}

int GameSession::npcSelectability(int npc) const
{
    if (!isAvailableNpc(npc)) {
        return 255;
    }
    return party_.npcs[static_cast<std::size_t>(npc)].select;
}

void GameSession::setNpcSelectability(int npc, int value)
{
    if (isAvailableNpc(npc)) {
        party_.npcs[static_cast<std::size_t>(npc)].select = value;
    }
}

// ---- leader, control and solo mode ---------------------------------------------------

std::uint32_t GameSession::controlledObject() const
{
    const int npc = party_.controlled;
    if (npc == kNpcPlayer || !validNpc(npc)) {
        return scriptWorld_.pc;
    }
    const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    if (scriptWorld_.get(slot.object) != nullptr) {
        return slot.object;
    }
    const std::uint32_t byTag = scriptWorld_.byTag(slot.tag, 0);
    return byTag != script::kObjectInvalid ? byTag : scriptWorld_.pc;
}

std::vector<std::uint32_t> GameSession::partyObjects() const
{
    const auto objectOf = [&](int npc) -> std::uint32_t {
        if (npc == kNpcPlayer || !validNpc(npc)) {
            return scriptWorld_.pc;
        }
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        if (scriptWorld_.get(slot.object) != nullptr) {
            return slot.object;
        }
        return scriptWorld_.byTag(slot.tag, 0);
    };
    std::vector<std::uint32_t> out;
    const auto push = [&](std::uint32_t id) {
        if (id != script::kObjectInvalid && id != 0
            && std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(id);
        }
    };
    // Member 0 is always the party leader; the PC takes the next slot when an NPC leads.
    push(objectOf(party_.controlled));
    push(scriptWorld_.pc);
    for (const int member : party_.members) {
        push(objectOf(member));
    }
    return out;
}

int GameSession::npcForObject(std::uint32_t object) const
{
    if (object == scriptWorld_.pc) {
        return kNpcPlayer;
    }
    for (int npc = 0; npc < kNpcCount; ++npc) {
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        if (slot.available && slot.object != script::kObjectInvalid && slot.object == object) {
            return npc;
        }
    }
    return -2;
}

// Rebuilds the legacy views (tags, roster selection, leader tag, the script
// world's party list) from the table.
void GameSession::syncPartyViews()
{
    partyTags_.clear();
    for (const int npc : party_.members) {
        partyTags_.push_back(party_.npcs[static_cast<std::size_t>(npc)].tag);
    }
    selectedCompanion_ = -1;
    for (std::size_t i = 0; i < party_.members.size(); ++i) {
        if (party_.members[i] == party_.controlled) {
            selectedCompanion_ = static_cast<int>(i);
        }
    }
    leaderTag_ = party_.controlled == kNpcPlayer || !validNpc(party_.controlled)
        ? std::string("PC")
        : party_.npcs[static_cast<std::size_t>(party_.controlled)].tag;
    scriptWorld_.party = partyObjects();
}

// SetPartyLeader (ACT_13) and SwitchPlayerCharacter (ACT_11) both change the
// controlled character; retail keeps one "controlled" field the leader follows.
// -1 is the PC. A non-member NPC is refused.
bool GameSession::setPartyLeaderNpc(int npc)
{
    if (npc != kNpcPlayer && (!validNpc(npc) || !isNpcPartyMember(npc))) {
        return false;
    }
    const int previous = party_.controlled;
    party_.leader = npc;
    party_.controlled = npc;
    syncPartyViews();
    if (previous != npc) {
        onControlChanged(previous, npc);
    }
    return true;
}

bool GameSession::switchPlayerCharacter(int npc)
{
    return setPartyLeaderNpc(npc);
}

void GameSession::selectCompanion(int index)
{
    if (index >= 0 && index < static_cast<int>(party_.members.size())) {
        setPartyLeaderNpc(party_.members[static_cast<std::size_t>(index)]);
    } else {
        selectedCompanion_ = index;
    }
}

void GameSession::setPartyLeader(const std::string& tag)
{
    if (lowerStr(tag) == "pc") {
        setPartyLeaderNpc(kNpcPlayer);
        return;
    }
    setPartyLeaderNpc(npcIdForTag(tag));
}

// Solo mode hides the members (they stay in the table). Engine rule: retail's
// solo flag is stored and read back; how the party renders under it is not
// recovered, so members are only hidden and left alone.
void GameSession::setSoloMode(bool solo)
{
    party_.solo = solo;
    applyNpcSoloVisibility();
}

void GameSession::applyNpcSoloVisibility()
{
    for (const int npc : party_.members) {
        const std::size_t index = entityOfObject(party_.npcs[static_cast<std::size_t>(npc)].object);
        if (index == kNone) {
            continue;
        }
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == index) {
                // Solo mode hides the members that are not steered.
                mesh.visible = !party_.solo || npc == party_.controlled;
            }
        }
    }
    ++animationRevision_;
}

// ---- XP and levels -----------------------------------------------------------------

void GameSession::grantMemberXp(CompanionState& companion, int npc, std::int32_t amount)
{
    const std::int64_t share = static_cast<std::int64_t>(amount)
        * party_.xpPercent[static_cast<std::size_t>(npc)] / 100;
    companion.sheet.xp = clampI32(static_cast<std::int64_t>(companion.sheet.xp) + share);
    if (!rules_.loaded()) {
        return;
    }
    const std::int32_t level = rules_.levelForXp(companion.sheet.xp);
    if (level > companion.sheet.level) {
        autoLevelCompanion(companion.sheet, level);
    }
}

void GameSession::awardPartyXp(std::int32_t amount)
{
    if (amount <= 0) {
        return;
    }
    loadPartyTables();
    party_.xpPool = clampI32(static_cast<std::int64_t>(party_.xpPool) + amount);
    for (const int npc : party_.members) {
        if (CompanionState* companion = companionFor(npc); companion != nullptr) {
            grantMemberXp(*companion, npc, amount);
        }
    }
}

// Raises a companion to `target` level. Retail runs a package-driven auto-level
// (005b27e0) from preference tables (packages.2da is not shipped), so the
// ordering of choices is an engine heuristic: class skills are filled lowest
// first up to level+3, the lowest-id eligible feats are taken, and the highest
// ability gets the attribute point on every fourth level.
void GameSession::autoLevelCompanion(CharacterSheet& sheet, std::int32_t targetLevel)
{
    if (targetLevel <= sheet.level) {
        return;
    }
    if (!rules_.loaded()) {
        sheet.level = targetLevel;
        return;
    }
    const std::int32_t from = sheet.level;
    for (std::int32_t next = from + 1; next <= targetLevel; ++next) {
        std::int32_t points = rules_.skillPointsForLevel(sheet.classId, sheet.intel);
        while (points > 0) {
            std::string best;
            std::int32_t bestRank = INT32_MAX;
            for (std::int32_t skill = 0; skill < rules_.skillCount(); ++skill) {
                if (!rules_.skillIsClassSkill(sheet.classId, skill)) {
                    continue;
                }
                const std::string name = rules_.skillName(skill);
                const auto found = sheet.skills.find(name);
                const std::int32_t rank = found != sheet.skills.end() ? found->second : 0;
                if (rank < next + 3 && rank < bestRank) {
                    bestRank = rank;
                    best = name;
                }
            }
            if (best.empty()) {
                break;
            }
            ++sheet.skills[best];
            --points;
        }
        std::int32_t feats = rules_.featsForLevel(sheet.classId, next);
        for (std::int32_t feat = 0; feat < rules_.featCount() && feats > 0; ++feat) {
            if (!sheet.feats.contains(feat) && rules_.featIsAvailable(sheet.classId, feat)
                && rules_.canTakeFeat(sheet, feat)) {
                sheet.feats.insert(feat);
                --feats;
            }
        }
        if (next % 4 == 0) {
            std::int32_t* abilities[] = {&sheet.str, &sheet.dex, &sheet.con,
                                         &sheet.intel, &sheet.wis, &sheet.cha};
            std::int32_t* highest = abilities[0];
            for (std::int32_t* value : abilities) {
                if (*value > *highest) {
                    highest = value;
                }
            }
            ++*highest;
        }
    }
    const std::int32_t hpGain = std::max<std::int32_t>(
        0, rules_.maxHitPoints(sheet.classId, targetLevel, sheet.con)
               - rules_.maxHitPoints(sheet.classId, from, sheet.con));
    const std::int32_t fpGain = std::max<std::int32_t>(
        0, rules_.maxForcePoints(sheet.classId, targetLevel, sheet.wis, sheet.cha)
               - rules_.maxForcePoints(sheet.classId, from, sheet.wis, sheet.cha));
    sheet.level = targetLevel;
    sheet.maxHp += hpGain;
    sheet.hp = sheet.maxHp;
    sheet.maxFp += fpGain;
    sheet.fp = sheet.maxFp;
    logEvent("companion " + sheet.name + " level " + std::to_string(targetLevel));
}

// ---- state and death ------------------------------------------------------------------

// Pulls an NPC's live health into its sheet and stores the sheet in its slot, so a
// later spawn or rejoin resumes from it (retail SaveNPCState, ACT_734).
void GameSession::saveNpcState(int npc)
{
    if (!validNpc(npc) || !party_.npcs[static_cast<std::size_t>(npc)].available) {
        return;
    }
    syncCompanionSheet(npc);
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    if (const CompanionState* member = companionFor(npc); member != nullptr) {
        slot.sheet = member->sheet;
        slot.hasSheet = true;
    }
}

void GameSession::syncCompanionSheet(int npc)
{
    if (!validNpc(npc)) {
        return;
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    CompanionState* member = companionFor(npc);
    CharacterSheet* sheet = member != nullptr ? &member->sheet : (slot.hasSheet ? &slot.sheet : nullptr);
    const std::size_t index = entityOfObject(slot.object);
    if (sheet == nullptr || index == kNone) {
        return;
    }
    kotor::Entity& entity = scene_.entities[index];
    if (entity.dead) {
        sheet->hp = 0;
    } else if (entity.hitPoints != slot.lastEntityHp) {
        sheet->hp = std::clamp(entity.hitPoints, 0, std::max(1, sheet->maxHp));
    } else if (sheet->hp != slot.lastSheetHp) {
        entity.hitPoints = std::clamp(sheet->hp, 1, std::max(1, entity.maxHitPoints));
    }
    slot.lastEntityHp = entity.hitPoints;
    slot.lastSheetHp = sheet->hp;
    slot.downed = entity.dead || sheet->hp <= 0;
}

void GameSession::tickParty(float dt)
{
    (void)dt;
    for (const int npc : party_.members) {
        syncCompanionSheet(npc);
    }
    if (party_.solo) {
        applyNpcSoloVisibility();
    }
    passControlIfDown();
    controlPcDefence();
}

bool GameSession::partyMemberDown(int npc) const
{
    if (npc == kNpcPlayer) {
        return playerDead_;
    }
    return validNpc(npc) && isNpcPartyMember(npc)
        && party_.npcs[static_cast<std::size_t>(npc)].downed;
}

// Engine rule (the retail party-death handler was not found): the game is lost
// when the controlled-party PC and every member are down. See the report.
bool GameSession::partyAllDown() const
{
    if (!playerDead_) {
        return false;
    }
    for (const int npc : party_.members) {
        if (!party_.npcs[static_cast<std::size_t>(npc)].downed) {
            return false;
        }
    }
    return true;
}

bool GameSession::revivePartyMember(int npc, std::int32_t hp)
{
    if (!validNpc(npc) || !isNpcPartyMember(npc)) {
        return false;
    }
    PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
    CompanionState* member = companionFor(npc);
    if (member == nullptr) {
        return false;
    }
    member->sheet.hp = std::clamp(hp, 1, std::max(1, member->sheet.maxHp));
    if (const std::size_t index = entityOfObject(slot.object); index != kNone) {
        kotor::Entity& entity = scene_.entities[index];
        entity.dead = false;
        entity.hitPoints = member->sheet.hp;
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == index) {
                mesh.visible = !party_.solo;
            }
        }
        ++animationRevision_;
        syncEntityToScriptWorld(index);
    }
    slot.downed = false;
    slot.lastEntityHp = -1;
    slot.lastSheetHp = member->sheet.hp;
    return true;
}

// ---- party selection GUI ---------------------------------------------------------------

// ShowPartySelectionGUI (ACT_712). Only the call signature is known from the
// binary; the screen itself is the UI's. The state: forced NPCs are pre-selected
// and cannot be removed, the rest toggle among available, selectable NPCs, at
// most kMaxPartyNpcs in all. Accept applies, cancel changes nothing; both run
// the exit script.
bool GameSession::openPartySelection(const std::string& exitScript, int forceNpc1, int forceNpc2)
{
    if (partySelection_.active) {
        return false;
    }
    partySelection_ = PartySelectionState{};
    partySelection_.active = true;
    partySelection_.exitScript = exitScript;
    partySelection_.forced[0] = forceNpc1;
    partySelection_.forced[1] = forceNpc2;
    for (const int forced : {forceNpc1, forceNpc2}) {
        if (isAvailableNpc(forced) && static_cast<int>(partySelection_.chosen.size()) < kMaxPartyNpcs
            && std::find(partySelection_.chosen.begin(), partySelection_.chosen.end(), forced)
                == partySelection_.chosen.end()) {
            partySelection_.chosen.push_back(forced);
        }
    }
    for (const int member : party_.members) {
        if (static_cast<int>(partySelection_.chosen.size()) < kMaxPartyNpcs
            && std::find(partySelection_.chosen.begin(), partySelection_.chosen.end(), member)
                == partySelection_.chosen.end()) {
            partySelection_.chosen.push_back(member);
        }
    }
    return true;
}

bool GameSession::partySelectionToggle(int npc)
{
    if (!partySelection_.active || !isAvailableNpc(npc)) {
        return false;
    }
    std::vector<int>& chosen = partySelection_.chosen;
    const auto found = std::find(chosen.begin(), chosen.end(), npc);
    if (found != chosen.end()) {
        if (npc == partySelection_.forced[0] || npc == partySelection_.forced[1]) {
            return false;
        }
        chosen.erase(found);
        return true;
    }
    if (!party_.npcs[static_cast<std::size_t>(npc)].selectable()
        || static_cast<int>(chosen.size()) >= kMaxPartyNpcs) {
        return false;
    }
    chosen.push_back(npc);
    return true;
}

bool GameSession::acceptPartySelection()
{
    if (!partySelection_.active) {
        return false;
    }
    const PartySelectionState state = partySelection_;
    partySelection_ = PartySelectionState{};
    const std::vector<int> current = party_.members;
    for (const int member : current) {
        if (std::find(state.chosen.begin(), state.chosen.end(), member) == state.chosen.end()) {
            removePartyNpc(member);
        }
    }
    for (const int npc : state.chosen) {
        if (isNpcPartyMember(npc)) {
            continue;
        }
        const std::uint32_t creature = spawnAvailableNpc(npc, position_, yaw_);
        if (creature != script::kObjectInvalid) {
            addPartyNpc(npc, creature);
        }
    }
    placePartyAtEntry();
    if (!state.exitScript.empty()) {
        runScriptWith(state.exitScript, scriptWorld_.pc, {});
    }
    return true;
}

void GameSession::cancelPartySelection()
{
    if (!partySelection_.active) {
        return;
    }
    const std::string exitScript = partySelection_.exitScript;
    partySelection_ = PartySelectionState{};
    if (!exitScript.empty()) {
        runScriptWith(exitScript, scriptWorld_.pc, {});
    }
}

// ---- legacy tag API --------------------------------------------------------------------

bool GameSession::addPartyMember(const std::string& tag)
{
    const int npc = npcIdForTag(tag);
    if (!validNpc(npc) || isNpcPartyMember(npc)
        || static_cast<int>(party_.members.size()) >= kMaxPartyNpcs) {
        return false;
    }
    if (!isAvailableNpc(npc) && !addAvailableNpcByTemplate(npc, templateForNpc(npc))) {
        return false;
    }
    const kotor::Vec3 forward{std::cos(yaw_), std::sin(yaw_), 0.0f};
    const kotor::Vec3 right{-forward.y, forward.x, 0.0f};
    const float side = (party_.members.size() % 2 == 0) ? -1.2f : 1.2f;
    const kotor::Vec3 where = position_ - forward * 2.0f + right * side;
    std::uint32_t creature = spawnAvailableNpc(npc, where, yaw_);
    if (creature == script::kObjectInvalid) {
        return false;
    }
    return addPartyNpc(npc, creature);
}

// Dismissal through the API (not the script): the member leaves and its avatar is
// hidden, as before the party table existed.
bool GameSession::removePartyMember(const std::string& tag)
{
    const int npc = npcIdForTag(tag);
    if (!validNpc(npc) || !isNpcPartyMember(npc)) {
        return false;
    }
    const std::uint32_t object = party_.npcs[static_cast<std::size_t>(npc)].object;
    const std::size_t index = entityOfObject(object);
    const bool wasAvatar = index != kNone && scene_.entities[index].partyAvatar;
    if (!removePartyNpc(npc)) {
        return false;
    }
    if (wasAvatar) {
        scene_.entities[index].partyAvatar = true;
        scene_.entities[index].dead = true;
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == index) {
                mesh.visible = false;
            }
        }
        ++animationRevision_;
    }
    return true;
}

// ---- engine save ------------------------------------------------------------------------

void GameSession::writePartyState(std::ostream& file) const
{
    file << "pt=" << party_.leader << "|" << party_.controlled << "|" << (party_.solo ? 1 : 0)
         << "|" << party_.xpPool << "|";
    for (std::size_t i = 0; i < party_.members.size(); ++i) {
        file << (i == 0 ? "" : ",") << party_.members[i];
    }
    file << "\n";
    // A steered companion's own position (x/y/z of the save is the PC's body).
    if (companionControlled() && controlledEntity() != kNone) {
        const kotor::Vec3 at = controlledPosition();
        file << "pctl=" << at.x << "|" << at.y << "|" << at.z << "|" << controlledBearing() << "\n";
    }
    for (int npc = 0; npc < kNpcCount; ++npc) {
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        if (!slot.available) {
            continue;
        }
        const CharacterSheet* sheet = liveSheet(npc);
        file << "pnpc=" << npc << "|" << slot.select << "|" << slot.object << "|"
             << slot.joiningXp << "|" << (slot.downed ? 1 : 0) << "|" << (sheet != nullptr ? 1 : 0)
             << "|" << cleanField(slot.templateResref) << "|" << cleanField(slot.tag) << "|"
             << cleanField(slot.name) << "\n";
        if (sheet == nullptr) {
            continue;
        }
        file << "psheet=" << npc << "|" << sheet->classId << "|" << sheet->level << "|"
             << sheet->xp << "|" << sheet->hp << "|" << sheet->maxHp << "|" << sheet->fp << "|"
             << sheet->maxFp << "|" << sheet->str << "|" << sheet->dex << "|" << sheet->con << "|"
             << sheet->intel << "|" << sheet->wis << "|" << sheet->cha << "|" << sheet->alignment
             << "|" << sheet->skillPoints << "|" << sheet->featPoints << "|"
             << sheet->attributePoints << "\n";
        for (const auto& [name, rank] : sheet->skills) {
            file << "pskill=" << npc << "|" << cleanField(name) << "|" << rank << "\n";
        }
        for (const std::int32_t feat : sheet->feats) {
            file << "pfeat=" << npc << "|" << feat << "\n";
        }
        for (const std::int32_t power : sheet->powers) {
            file << "ppower=" << npc << "|" << power << "\n";
        }
        for (const InventoryItem& item : sheet->inventory) {
            file << "pitem=" << npc << "|" << cleanField(item.resref) << "|" << item.baseItem << "|"
                 << item.cost << "|" << item.stack << "|" << item.slot << "|"
                 << (item.equipped ? 1 : 0) << "|" << (item.identified ? 1 : 0) << "|"
                 << cleanField(item.name) << "\n";
        }
    }
    for (const std::uint32_t object : party_.headFollowLocked) {
        file << "plock=" << object << "\n";
    }
}

bool parsePartyLine(const std::string& key, const std::string& value, PartyLoad& out)
{
    if (key != "pt" && key != "pnpc" && key != "psheet" && key != "pskill" && key != "pfeat"
        && key != "ppower" && key != "pitem" && key != "plock" && key != "pctl") {
        return false;
    }
    out.present = true;
    const std::vector<std::string> p = splitText(value, '|');
    const auto num = [&](std::size_t i, std::int64_t fallback = 0) -> std::int64_t {
        return i < p.size() && !p[i].empty() ? std::stoll(p[i]) : fallback;
    };
    if (key == "pctl") {
        if (p.size() >= 4) {
            out.hasControlledPosition = true;
            for (std::size_t i = 0; i < 3; ++i) {
                out.controlledPosition[i] = std::stof(p[i]);
            }
            out.controlledBearing = std::stof(p[3]);
        }
        return true;
    }
    if (key == "pt") {
        out.table.leader = static_cast<int>(num(0, kNpcPlayer));
        out.table.controlled = static_cast<int>(num(1, kNpcPlayer));
        out.table.solo = num(2) != 0;
        out.table.xpPool = static_cast<std::int32_t>(num(3));
        out.table.members.clear();
        if (p.size() > 4 && !p[4].empty()) {
            for (const std::string& member : splitText(p[4], ',')) {
                const int id = std::stoi(member);
                if (validNpc(id)) {
                    out.table.members.push_back(id);
                }
            }
        }
        return true;
    }
    if (key == "plock") {
        out.table.headFollowLocked.insert(static_cast<std::uint32_t>(num(0)));
        return true;
    }
    const int npc = static_cast<int>(num(0, -1));
    if (!validNpc(npc)) {
        return true;
    }
    PartyNpc& slot = out.table.npcs[static_cast<std::size_t>(npc)];
    if (key == "pnpc") {
        slot.available = true;
        slot.select = static_cast<std::int32_t>(num(1, 1));
        slot.object = static_cast<std::uint32_t>(num(2));
        slot.joiningXp = static_cast<std::int32_t>(num(3));
        slot.downed = num(4) != 0;
        slot.hasSheet = num(5) != 0;
        slot.templateResref = p.size() > 6 ? p[6] : std::string{};
        slot.tag = p.size() > 7 ? p[7] : std::string{};
        slot.name = p.size() > 8 ? p[8] : std::string{};
    } else if (key == "psheet") {
        CharacterSheet& sheet = slot.sheet;
        sheet.name = slot.name;
        sheet.classId = static_cast<std::int32_t>(num(1));
        sheet.level = static_cast<std::int32_t>(num(2, 1));
        sheet.xp = static_cast<std::int32_t>(num(3));
        sheet.hp = static_cast<std::int32_t>(num(4, 1));
        sheet.maxHp = static_cast<std::int32_t>(num(5, 1));
        sheet.fp = static_cast<std::int32_t>(num(6));
        sheet.maxFp = static_cast<std::int32_t>(num(7));
        sheet.str = static_cast<std::int32_t>(num(8, 10));
        sheet.dex = static_cast<std::int32_t>(num(9, 10));
        sheet.con = static_cast<std::int32_t>(num(10, 10));
        sheet.intel = static_cast<std::int32_t>(num(11, 10));
        sheet.wis = static_cast<std::int32_t>(num(12, 10));
        sheet.cha = static_cast<std::int32_t>(num(13, 10));
        sheet.alignment = static_cast<std::int32_t>(num(14, 50));
        sheet.skillPoints = static_cast<std::int32_t>(num(15));
        sheet.featPoints = static_cast<std::int32_t>(num(16));
        sheet.attributePoints = static_cast<std::int32_t>(num(17));
    } else if (key == "pskill") {
        if (p.size() >= 3) {
            slot.sheet.skills[p[1]] = static_cast<std::int32_t>(num(2));
        }
    } else if (key == "pfeat") {
        slot.sheet.feats.insert(static_cast<std::int32_t>(num(1)));
    } else if (key == "ppower") {
        slot.sheet.powers.insert(static_cast<std::int32_t>(num(1)));
    } else if (key == "pitem") {
        InventoryItem item;
        item.resref = p.size() > 1 ? p[1] : std::string{};
        item.baseItem = static_cast<std::int32_t>(num(2));
        item.cost = static_cast<std::int32_t>(num(3));
        item.stack = static_cast<std::int32_t>(num(4, 1));
        item.slot = static_cast<std::int32_t>(num(5, -1));
        item.equipped = num(6) != 0;
        item.identified = num(7, 1) != 0;
        item.name = p.size() > 8 ? p[8] : std::string{};
        slot.sheet.inventory.push_back(std::move(item));
    }
    return true;
}

// Installs a parsed table: members get their companion entries and creatures.
bool GameSession::restorePartyTable(PartyLoad&& load)
{
    party_ = std::move(load.table);
    companions_.clear();
    partyTags_.clear();
    std::vector<int> members;
    for (const int npc : party_.members) {
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        if (slot.available && slot.hasSheet
            && static_cast<int>(members.size()) < kMaxPartyNpcs
            && std::find(members.begin(), members.end(), npc) == members.end()) {
            members.push_back(npc);
        }
    }
    party_.members = members;
    for (const int npc : party_.members) {
        PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        CompanionState companion;
        companion.tag = slot.tag;
        companion.sheet = slot.sheet;
        companions_.push_back(std::move(companion));
    }
    if ((party_.leader != kNpcPlayer && !isNpcPartyMember(party_.leader))) {
        party_.leader = kNpcPlayer;
    }
    if (party_.controlled != kNpcPlayer && !isNpcPartyMember(party_.controlled)) {
        party_.controlled = party_.leader;
    }
    for (const int npc : party_.members) {
        ensureNpcEntity(npc, nullptr, yaw_);
    }
    syncPartyViews();
    if (party_.solo) {
        applyNpcSoloVisibility();
    }
    // A steered companion stands where it was saved; the PC is at the save's x/y/z.
    if (load.hasControlledPosition && companionControlled()) {
        if (const std::size_t index = controlledEntity(); index != kNone) {
            kotor::Entity& body = scene_.entities[index];
            body.position = {load.controlledPosition[0], load.controlledPosition[1],
                             load.controlledPosition[2]};
            body.bearing = load.controlledBearing;
            syncEntityToScriptWorld(index);
        }
    }
    showControlledBodies();
    return true;
}

// ---- original save mapping -----------------------------------------------------------------

// PARTYTABLE / AVAILNPC<n>: member ids are NPC ids with the PC at -1 and outside
// the list; the slot arrays are indexed by NPC id.
void GameSession::writePartyToOriginalSave(kotor::OriginalSave& save) const
{
    save.xpPool = party_.xpPool;
    save.solo = party_.solo;
    save.controlledNpc = party_.controlled;
    save.party.clear();
    save.partyTags.clear();
    for (const int npc : party_.members) {
        save.party.emplace_back(npc, npc == party_.leader);
        save.partyTags.push_back(party_.npcs[static_cast<std::size_t>(npc)].tag);
    }
    for (int npc = 0; npc < kNpcCount; ++npc) {
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        kotor::SavedNpc& saved = save.npcs[static_cast<std::size_t>(npc)];
        saved = kotor::SavedNpc{};
        saved.available = slot.available;
        saved.select = slot.select;
        if (!slot.available) {
            continue;
        }
        const CharacterSheet* sheet = liveSheet(npc);
        if (sheet == nullptr) {
            continue;
        }
        saved.hasCreature = true;
        saved.templateResref = slot.templateResref;
        saved.tag = slot.tag;
        saved.name = sheet->name;
        saved.classId = sheet->classId;
        saved.level = sheet->level;
        saved.xp = sheet->xp;
        saved.hp = sheet->hp;
        saved.maxHp = sheet->maxHp;
        saved.forcePoints = sheet->fp;
        saved.maxForcePoints = sheet->maxFp;
        saved.str = sheet->str;
        saved.dex = sheet->dex;
        saved.con = sheet->con;
        saved.intel = sheet->intel;
        saved.wis = sheet->wis;
        saved.cha = sheet->cha;
        saved.alignment = sheet->alignment;
        saved.joiningXp = slot.joiningXp;
        saved.feats.assign(sheet->feats.begin(), sheet->feats.end());
        saved.powers.assign(sheet->powers.begin(), sheet->powers.end());
        for (std::int32_t skill = 0; skill < rules_.skillCount(); ++skill) {
            const auto found = sheet->skills.find(rules_.skillName(skill));
            saved.skills.push_back(found != sheet->skills.end() ? found->second : 0);
        }
        for (const InventoryItem& item : sheet->inventory) {
            kotor::SavedInventoryItem copy;
            copy.resref = item.resref;
            copy.name = item.name;
            copy.baseItem = item.baseItem;
            copy.cost = item.cost;
            copy.stack = item.stack;
            copy.slot = item.slot;
            copy.equipped = item.equipped;
            copy.identified = item.identified;
            saved.inventory.push_back(std::move(copy));
        }
    }
}

void GameSession::readPartyFromOriginalSave(const kotor::OriginalSave& save)
{
    loadPartyTables();
    PartyTable table;
    table.tablesLoaded = true;
    table.xpPercent = party_.xpPercent;
    table.npcTag = party_.npcTag;
    table.defaultTemplate = party_.defaultTemplate;
    party_ = std::move(table);
    companions_.clear();
    partyTags_.clear();
    party_.xpPool = save.xpPool;
    party_.solo = save.solo;
    for (int npc = 0; npc < kNpcCount; ++npc) {
        const kotor::SavedNpc& saved = save.npcs[static_cast<std::size_t>(npc)];
        const bool listed = std::any_of(save.party.begin(), save.party.end(),
                                        [&](const auto& member) { return member.first == npc; });
        if (!saved.available && !listed) {
            continue;
        }
        PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        slot.available = true;
        slot.select = saved.select;
        slot.joiningXp = saved.joiningXp;
        slot.templateResref = saved.templateResref.empty() ? templateForNpc(npc)
                                                           : lowerStr(saved.templateResref);
        if (!saved.hasCreature) {
            if (!loadNpcSheet(npc)) {
                slot = PartyNpc{};
            }
            continue;
        }
        slot.tag = saved.tag;
        CharacterSheet sheet;
        sheet.name = saved.name;
        sheet.classId = saved.classId;
        sheet.level = saved.level;
        sheet.xp = saved.xp;
        sheet.hp = saved.hp;
        sheet.maxHp = saved.maxHp;
        sheet.fp = saved.forcePoints;
        sheet.maxFp = saved.maxForcePoints;
        sheet.str = saved.str;
        sheet.dex = saved.dex;
        sheet.con = saved.con;
        sheet.intel = saved.intel;
        sheet.wis = saved.wis;
        sheet.cha = saved.cha;
        sheet.alignment = saved.alignment;
        sheet.feats.insert(saved.feats.begin(), saved.feats.end());
        sheet.powers.insert(saved.powers.begin(), saved.powers.end());
        for (std::size_t skill = 0; skill < saved.skills.size(); ++skill) {
            if (rules_.loaded() && static_cast<std::int32_t>(skill) < rules_.skillCount()) {
                sheet.skills[rules_.skillName(static_cast<std::int32_t>(skill))] = saved.skills[skill];
            }
        }
        for (const kotor::SavedInventoryItem& source : saved.inventory) {
            InventoryItem item;
            item.resref = source.resref;
            item.name = source.name;
            item.baseItem = source.baseItem;
            item.cost = source.cost;
            item.stack = std::max(1, source.stack);
            item.slot = source.slot;
            item.equipped = source.equipped;
            item.identified = source.identified;
            sheet.inventory.push_back(std::move(item));
        }
        slot.sheet = std::move(sheet);
        slot.hasSheet = true;
        slot.name = slot.sheet.name;
        if (slot.tag.empty()) {
            slot.tag = slot.sheet.name;
        }
    }
    for (const auto& [npc, leads] : save.party) {
        if (validNpc(npc) && party_.npcs[static_cast<std::size_t>(npc)].available
            && party_.npcs[static_cast<std::size_t>(npc)].hasSheet && !isNpcPartyMember(npc)
            && static_cast<int>(party_.members.size()) < kMaxPartyNpcs) {
            party_.members.push_back(npc);
            if (leads) {
                party_.leader = npc;
            }
        }
    }
    party_.controlled = isNpcPartyMember(save.controlledNpc) ? save.controlledNpc : party_.leader;
    if (party_.leader == kNpcPlayer && party_.controlled != kNpcPlayer) {
        party_.leader = party_.controlled;
    }
    for (const int npc : party_.members) {
        PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        CompanionState companion;
        companion.tag = slot.tag;
        companion.sheet = slot.sheet;
        companions_.push_back(std::move(companion));
        ensureNpcEntity(npc, nullptr, yaw_);
    }
    syncPartyViews();
}

// ---- script handlers --------------------------------------------------------------------------

namespace {

using Cells = std::optional<std::vector<script::Value>>;

int npcArg(const ActionCall& c, std::size_t index)
{
    return static_cast<int>(c.intArg(index, kNpcPlayer));
}

// ACT_699 IsNPCPartyMember
Cells isNpcPartyMemberAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.isNpcPartyMember(npcArg(c, 0)) ? 1 : 0);
}

// ACT_13 SetPartyLeader
Cells setPartyLeaderAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.setPartyLeaderNpc(npcArg(c, 0)) ? 1 : 0);
}

// ACT_11 SwitchPlayerCharacter
Cells switchPlayerCharacterAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.switchPlayerCharacter(npcArg(c, 0)) ? 1 : 0);
}

// ACT_697 AddAvailableNPCByTemplate
Cells addAvailableByTemplateAction(ActionCall& c)
{
    return ActionCall::retInt(
        c.session.addAvailableNpcByTemplate(npcArg(c, 0), c.stringArg(1)) ? 1 : 0);
}

// ACT_694 AddAvailableNPCByObject
Cells addAvailableByObjectAction(ActionCall& c)
{
    return ActionCall::retInt(
        c.session.addAvailableNpcByObject(npcArg(c, 0), c.objectArgOrInvalid(1)) ? 1 : 0);
}

// ACT_695 RemoveAvailableNPC
Cells removeAvailableAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.removeAvailableNpc(npcArg(c, 0)) ? 1 : 0);
}

// ACT_696 IsAvailableCreature
Cells isAvailableAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.isAvailableNpc(npcArg(c, 0)) ? 1 : 0);
}

// ACT_698 SpawnAvailableNPC(nNPC, lPosition)
Cells spawnAvailableAction(ActionCall& c)
{
    const LocationData* where = c.api().sim().engine.get<LocationData>(c.engineArg(1));
    kotor::Vec3 position = c.api().position(c.api().pc());
    float facing = 0.0f;
    if (where != nullptr) {
        position = where->position;
        facing = where->facing * kPi / 180.0f;
    }
    return ActionCall::retObject(c.session.spawnAvailableNpc(npcArg(c, 0), position, facing));
}

// ACT_574 AddPartyMember(nNPC, oCreature)
Cells addPartyMemberAction(ActionCall& c)
{
    return ActionCall::retInt(
        c.session.addPartyNpc(npcArg(c, 0), c.objectArgOrInvalid(1)) ? 1 : 0);
}

// ACT_575 RemovePartyMember(nNPC): leaves the creature in the world.
Cells removePartyMemberAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.removePartyNpc(npcArg(c, 0)) ? 1 : 0);
}

// ACT_572 / ACT_573 are the multiplayer party calls ("only works on two PCs").
// A single-player game has no second PC, so they do nothing.
Cells addToPartyAction(ActionCall&)
{
    return ActionCall::none();
}

// ACT_576 IsObjectPartyMember
Cells isObjectPartyMemberAction(ActionCall& c)
{
    const std::uint32_t object = c.objectArgOrInvalid(0);
    if (object == script::kObjectInvalid) {
        return ActionCall::retInt(0);
    }
    const int npc = c.session.npcForObject(object);
    if (npc == kNpcPlayer) {
        return ActionCall::retInt(1);
    }
    return ActionCall::retInt(npc >= 0 && c.session.isNpcPartyMember(npc) ? 1 : 0);
}

// ACT_126 GetPartyMemberCount: the PC counts.
Cells partyMemberCountAction(ActionCall& c)
{
    return ActionCall::retInt(static_cast<std::int64_t>(c.session.partyTable().members.size()) + 1);
}

// ACT_577 GetPartyMemberByIndex: member 0 is the leader.
Cells partyMemberByIndexAction(ActionCall& c)
{
    const std::vector<std::uint32_t> party = c.session.partyObjects();
    const std::int64_t index = c.intArg(0);
    if (index < 0 || index >= static_cast<std::int64_t>(party.size())) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(party[static_cast<std::size_t>(index)]);
}

// ACT_548 GetFirstPC: the controlled creature (the PC while it leads).
Cells getFirstPcAction(ActionCall& c)
{
    return ActionCall::retObject(c.session.controlledObject());
}

// ACT_708 SetNPCSelectability / ACT_709 GetNPCSelectability
Cells setSelectabilityAction(ActionCall& c)
{
    c.session.setNpcSelectability(npcArg(c, 0), static_cast<int>(c.intArg(1)));
    return ActionCall::none();
}

Cells getSelectabilityAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.npcSelectability(npcArg(c, 0)));
}

// ACT_712 ShowPartySelectionGUI(sExitScript, nForceNPC1, nForceNPC2)
Cells showPartySelectionAction(ActionCall& c)
{
    c.session.openPartySelection(c.stringArg(0), static_cast<int>(c.intArg(1, kNpcPlayer)),
                                 static_cast<int>(c.intArg(2, kNpcPlayer)));
    return ActionCall::none();
}

// ACT_462 GetSoloMode / ACT_753 SetSoloMode
Cells getSoloModeAction(ActionCall& c)
{
    return ActionCall::retInt(c.session.soloMode() ? 1 : 0);
}

Cells setSoloModeAction(ActionCall& c)
{
    c.session.setSoloMode(c.intArg(0) != 0);
    return ActionCall::none();
}

// ACT_506 SetLockHeadFollowInDialog(oObject, nValue)
Cells lockHeadFollowAction(ActionCall& c)
{
    c.session.setHeadFollowLocked(c.objectArg(0), c.intArg(1) != 0);
    return ActionCall::none();
}

// ACT_734 SaveNPCState
Cells saveNpcStateAction(ActionCall& c)
{
    c.session.saveNpcState(npcArg(c, 0));
    return ActionCall::none();
}

// ACT_767 SetAvailableNPCId(nNPC, oidNPC): the object used for an available NPC.
Cells setAvailableNpcIdAction(ActionCall& c)
{
    c.session.setAvailableNpcObject(npcArg(c, 0), c.objectArgOrInvalid(1));
    return ActionCall::none();
}

// ACT_748 GetIsLiveContentAvailable(nPkg). The handler (00547ab0) answers TRUE
// for package 1 and probes a "LIVE<n>" setting for 2..6; that probe is not
// resolved, and this install ships no live content, so those answer FALSE.
Cells liveContentAction(ActionCall& c)
{
    return ActionCall::retInt(c.intArg(0) == 1 ? 1 : 0);
}

// ACT_393 GiveXPToCreature: a party creature's award is the party table's
// (pool, PC and every member). Other creatures fall through.
Cells giveXpAction(ActionCall& c)
{
    const std::uint32_t object = c.objectArg(0);
    if (c.session.npcForObject(object) == -2) {
        return std::nullopt;
    }
    c.session.giveXp(static_cast<std::int32_t>(c.intArg(1)));
    return ActionCall::none();
}

// ACT_395 GetXP for a companion reads its sheet; the PC and others fall through.
Cells getXpAction(ActionCall& c)
{
    const int npc = c.session.npcForObject(c.objectArg(0));
    if (npc < 0 || !c.session.isNpcPartyMember(npc)) {
        return std::nullopt;
    }
    const CharacterSheet* sheet = c.session.partySheet(npc);
    return sheet != nullptr ? Cells(ActionCall::retInt(sheet->xp)) : std::nullopt;
}

} // namespace

void registerPartyActions(HandlerTable& table)
{
    table.add(11, switchPlayerCharacterAction);
    table.add(13, setPartyLeaderAction);
    table.add(126, partyMemberCountAction);
    table.add(393, giveXpAction);
    table.add(395, getXpAction);
    table.add(462, getSoloModeAction);
    table.add(506, lockHeadFollowAction);
    table.add(548, getFirstPcAction);
    table.add(572, addToPartyAction);
    table.add(573, addToPartyAction);
    table.add(574, addPartyMemberAction);
    table.add(575, removePartyMemberAction);
    table.add(576, isObjectPartyMemberAction);
    table.add(577, partyMemberByIndexAction);
    table.add(694, addAvailableByObjectAction);
    table.add(695, removeAvailableAction);
    table.add(696, isAvailableAction);
    table.add(697, addAvailableByTemplateAction);
    table.add(698, spawnAvailableAction);
    table.add(699, isNpcPartyMemberAction);
    table.add(708, setSelectabilityAction);
    table.add(709, getSelectabilityAction);
    table.add(712, showPartySelectionAction);
    table.add(734, saveNpcStateAction);
    table.add(748, liveContentAction);
    table.add(753, setSoloModeAction);
    table.add(767, setAvailableNpcIdAction);
}

// ---- the family gate (--family-test party) ----------------------------------------------------

namespace {

// A tiny compiled script: constants, action calls, return. Enough for an exit script.
class PartyScript {
public:
    PartyScript& constInt(std::int32_t value)
    {
        op(0x04, 0x03);
        be32(static_cast<std::uint32_t>(value));
        return *this;
    }
    PartyScript& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    PartyScript& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    std::vector<std::uint8_t> bytes()
    {
        op(0x20, 0x00);
        const std::string magic = "NCS V1.0";
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        out.push_back(0x42);
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code_.size());
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

private:
    void op(std::uint8_t code, std::uint8_t qualifier)
    {
        code_.push_back(code);
        code_.push_back(qualifier);
    }
    void be32(std::uint32_t value)
    {
        for (const int shift : {24, 16, 8, 0}) {
            code_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    std::vector<std::uint8_t> code_;
};

// "int NAME   = value;" from nwscript.nss.
std::optional<int> nwscriptInt(const std::string& source, const std::string& name)
{
    std::size_t pos = 0;
    const std::string needle = "int " + name;
    while ((pos = source.find(needle, pos)) != std::string::npos) {
        const std::size_t after = pos + needle.size();
        pos = after;
        std::size_t cursor = after;
        while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '=') {
            continue;
        }
        return std::atoi(source.c_str() + cursor + 1);
    }
    return std::nullopt;
}

} // namespace

int runPartyFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("party family: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    script::Vm dummy({});
    using Args = std::vector<std::vector<script::Value>>;
    const auto I = [](std::int64_t v) { return std::vector<script::Value>{script::Value::makeInt(v)}; };
    const auto S = [](const std::string& v) {
        return std::vector<script::Value>{script::Value::makeString(v)};
    };
    const auto O = [](std::uint32_t v) {
        return std::vector<script::Value>{script::Value::makeObject(v)};
    };
    const auto run = [&](std::uint16_t id, Args args) {
        const auto result = game.action(id, args, game.scriptWorld(), dummy);
        return result.has_value() ? *result : std::vector<script::Value>{};
    };
    const auto ri = [&](std::uint16_t id, Args args) -> std::int64_t {
        const auto cells = run(id, std::move(args));
        return cells.empty() ? std::int64_t{-9999} : cells[0].integer;
    };
    const auto ro = [&](std::uint16_t id, Args args) -> std::uint32_t {
        const auto cells = run(id, std::move(args));
        return cells.empty() ? script::kObjectInvalid : static_cast<std::uint32_t>(cells[0].integer);
    };
    const auto xpOf = [&](const GameSession& session, int npc) -> std::int32_t {
        const CharacterSheet* sheet = session.partySheet(npc);
        return sheet != nullptr ? sheet->xp : -12345;
    };
    ScriptApi api{game};
    const std::uint32_t pc = game.scriptWorld().pc;
    const std::uint32_t invalid = script::kObjectInvalid;
    kotor::Install install(game.installPath());
    const auto logMembers = [&](const char* where) {
        std::string listed;
        for (const int m : game.partyTable().members) {
            listed += std::to_string(m) + " ";
        }
        logInfo(concat("party family: members ", where, ": ", listed));
    };

    // ---- constants against the shipped nwscript.nss ---------------------------------------
    std::string nwscript;
    if (auto data = install.find("nwscript", 2009); data.has_value()) {
        nwscript.assign(data->begin(), data->end());
    }
    const std::pair<const char*, int> constants[] = {
        {"NPC_PLAYER", kNpcPlayer},   {"NPC_BASTILA", kNpcBastila}, {"NPC_CANDEROUS", kNpcCanderous},
        {"NPC_CARTH", kNpcCarth},     {"NPC_HK_47", kNpcHk47},      {"NPC_JOLEE", kNpcJolee},
        {"NPC_JUHANI", kNpcJuhani},   {"NPC_MISSION", kNpcMission}, {"NPC_T3_M4", kNpcT3m4},
        {"NPC_ZAALBAR", kNpcZaalbar},
    };
    for (const auto& [name, value] : constants) {
        check(nwscriptInt(nwscript, name) == value, std::string("nwscript_") + name);
    }
    const auto nw = [&](const char* name) { return nwscriptInt(nwscript, name).value_or(-99); };

    // The shipped templates, read independently of the party code.
    struct Utc {
        std::int32_t classId = -1;
        std::int32_t level = 0;
        std::size_t feats = 0;
        std::size_t powers = 0;
        std::string name;
        std::string tag;
    };
    std::optional<kotor::Tlk> tlk;
    {
        std::ifstream file(std::filesystem::path(game.installPath()) / "dialog.tlk",
                           std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
        if (!bytes.empty()) {
            tlk = kotor::Tlk::parse(std::move(bytes), "dialog.tlk");
        }
    }
    const auto readUtc = [&](const std::string& resref) {
        Utc info;
        auto data = install.find(resref, kUtcType);
        if (!data.has_value()) {
            return info;
        }
        const kotor::Gff utc = kotor::Gff::parse(std::move(*data), resref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        info.tag = gffText(utc, root, "Tag");
        if (const kotor::GffField* classes = utc.field(root, "ClassList")) {
            bool first = true;
            for (const std::uint32_t entry : utc.list(*classes)) {
                if (first) {
                    info.classId = gffInt(utc, entry, "Class", -1);
                    first = false;
                }
                info.level += gffInt(utc, entry, "ClassLevel", 1);
                if (const kotor::GffField* known = utc.field(entry, "KnownList0")) {
                    info.powers += utc.list(*known).size();
                }
            }
        }
        if (const kotor::GffField* feats = utc.field(root, "FeatList")) {
            info.feats = utc.list(*feats).size();
        }
        if (const kotor::GffField* first = utc.field(root, "FirstName")) {
            const std::uint32_t ref = utc.locStringStrref(*first);
            if (ref != UINT32_MAX && tlk.has_value()) {
                info.name = tlk->text(ref);
            }
        }
        return info;
    };
    const Utc carthUtc = readUtc("p_carth");
    const Utc joleeUtc = readUtc("p_jolee");
    const Utc bastilaUtc = readUtc("p_bastilla");
    check(carthUtc.classId >= 0 && joleeUtc.classId >= 0 && bastilaUtc.classId >= 0,
          "fixture_templates");
    check(!carthUtc.name.empty() && !joleeUtc.name.empty(), "fixture_names");
    if (carthUtc.classId < 0 || joleeUtc.classId < 0 || bastilaUtc.classId < 0) {
        return 40;
    }

    // ---- AddAvailableNPCByTemplate / IsAvailableCreature ----------------------------------
    check(ri(697, {I(nw("NPC_CARTH")), S("p_carth")}) == 1, "add_available_carth");
    check(ri(697, {I(nw("NPC_CARTH")), S("p_carth")}) == 0, "add_available_twice_refused");
    check(ri(697, {I(nw("NPC_BASTILA")), S("no_such_creature")}) == 0, "add_available_bad_template");
    check(ri(696, {I(nw("NPC_CARTH"))}) == 1 && ri(696, {I(nw("NPC_BASTILA"))}) == 0,
          "is_available");
    check(ri(697, {I(nw("NPC_JOLEE")), S("p_jolee")}) == 1, "add_available_jolee");
    check(ri(697, {I(nw("NPC_BASTILA")), S("p_bastilla")}) == 1, "add_available_bastila");
    check(ri(709, {I(nw("NPC_MISSION"))}) == 255, "selectability_unavailable_255");
    // Stored sheets come from the template before anything spawns.
    {
        const PartyNpc& carth = game.partyTable().npcs[kNpcCarth];
        check(carth.hasSheet && carth.sheet.classId == carthUtc.classId
                  && carth.sheet.level == carthUtc.level && carth.sheet.name == carthUtc.name,
              "stored_sheet_from_template");
    }

    // ---- SpawnAvailableNPC: a creature with the template's class and name ------------------
    const auto here = [&](float dx) {
        LocationData at;
        at.position = game.position();
        at.position.x += dx;
        at.facing = 0.0f;
        return script::Value::makeEngine(api.sim().engine.add(at));
    };
    const std::uint32_t carth = ro(698, {I(nw("NPC_CARTH")), {here(2.0f)}});
    check(carth != invalid && carth != pc, "spawn_carth_object");
    const std::uint32_t jolee = ro(698, {I(nw("NPC_JOLEE")), {here(-2.0f)}});
    check(jolee != invalid && jolee != pc && jolee != carth, "spawn_jolee_object");
    check(ro(698, {I(nw("NPC_MISSION")), {here(1.0f)}}) == invalid, "spawn_unavailable_refused");
    {
        const std::size_t joleeEntity = api.entityFor(jolee);
        check(joleeEntity != kNone, "jolee_has_entity");
        if (joleeEntity != kNone) {
            const kotor::Entity& entity = game.scene().entities[joleeEntity];
            check(entity.templateResref == "p_jolee"
                      && lowerStr(entity.tag) == lowerStr(joleeUtc.tag),
                  "jolee_entity_from_template");
            check(entity.name == joleeUtc.name && entity.name != game.character().name,
                  "jolee_entity_name");
        }
        const CharacterSheet* sheet = game.partySheet(kNpcJolee);
        check(sheet != nullptr && sheet->classId == joleeUtc.classId
                  && sheet->level == joleeUtc.level && sheet->name == joleeUtc.name
                  && sheet->feats.size() == joleeUtc.feats && sheet->powers.size() == joleeUtc.powers,
              "jolee_sheet_class_name_feats_powers");
        check(sheet != nullptr
                  && std::any_of(sheet->inventory.begin(), sheet->inventory.end(),
                                 [](const InventoryItem& item) {
                                     return item.equipped && !item.resref.empty();
                                 }),
              "jolee_sheet_equipment");
        check(sheet != nullptr && sheet->name != game.character().name, "jolee_distinct_from_pc");
    }
    // npc.2da rows against the id mapping (loaded by now).
    {
        bool rows = true;
        for (int row = 0; row < kNpcCount; ++row) {
            rows = rows
                && GameSession::npcIdForTag(game.partyTable().npcTag[static_cast<std::size_t>(row)])
                    == row;
        }
        check(rows, "npc_2da_tags_match_ids");
    }

    // ---- AddPartyMember / IsNPCPartyMember / counts ---------------------------------------
    check(ri(699, {I(nw("NPC_CARTH"))}) == 0, "not_member_before");
    check(ri(574, {I(nw("NPC_CARTH")), O(carth)}) == 1, "add_member_carth");
    check(ri(574, {I(nw("NPC_CARTH")), O(carth)}) == 0, "add_member_twice_refused");
    check(ri(699, {I(nw("NPC_CARTH"))}) == 1, "is_member_carth");
    check(ri(576, {O(carth)}) == 1 && ri(576, {O(pc)}) == 1 && ri(576, {O(jolee)}) == 0,
          "is_object_party_member");
    check(ri(574, {I(nw("NPC_JOLEE")), O(jolee)}) == 1, "add_member_jolee");
    check(ri(126, {}) == 3, "member_count_with_pc");
    const std::uint32_t bastila = ro(698, {I(nw("NPC_BASTILA")), {here(3.0f)}});
    check(bastila != invalid && ri(574, {I(nw("NPC_BASTILA")), O(bastila)}) == 0
              && ri(699, {I(nw("NPC_BASTILA"))}) == 0,
          "third_member_refused");
    check(ro(577, {I(0)}) == pc && ro(577, {I(1)}) == carth && ro(577, {I(2)}) == jolee
              && ro(577, {I(3)}) == invalid,
          "member_by_index");

    // ---- XP: the pool, the PC and every member; a non-member gets none --------------------
    const std::int32_t pcXp = game.character().xp;
    const std::int32_t carthXp = game.partySheet(kNpcCarth)->xp;
    const std::int32_t joleeXp = game.partySheet(kNpcJolee)->xp;
    const std::int32_t bastilaXp = game.partyTable().npcs[kNpcBastila].sheet.xp;
    const std::int32_t poolBefore = game.partyTable().xpPool;
    run(393, {O(pc), I(1000)});
    check(game.character().xp == pcXp + 1000, "xp_pc_full");
    check(game.partySheet(kNpcCarth)->xp == carthXp + 800
              && game.partySheet(kNpcJolee)->xp == joleeXp + 800,
          "xp_members_80_percent");
    check(game.partyTable().npcs[kNpcBastila].sheet.xp == bastilaXp, "xp_non_member_none");
    check(game.partyTable().xpPool == poolBefore + 1000, "xp_pool");
    check(ri(395, {O(carth)}) == carthXp + 800, "get_xp_member");

    // ---- RemovePartyMember removes; HP and XP survive leave and rejoin ----------------------
    const std::size_t carthEntity = api.entityFor(carth);
    check(carthEntity != kNone, "carth_entity");
    if (carthEntity != kNone) {
        game.scene().entities[carthEntity].hitPoints = 7;
    }
    game.update(0.05f, InputState{});
    check(game.partySheet(kNpcCarth)->hp == 7, "entity_hp_reaches_sheet");
    const std::int32_t carthXpAfter = game.partySheet(kNpcCarth)->xp;
    check(ri(575, {I(nw("NPC_CARTH"))}) == 1, "remove_member_carth");
    check(ri(575, {I(nw("NPC_CARTH"))}) == 0, "remove_member_twice");
    check(ri(699, {I(nw("NPC_CARTH"))}) == 0 && ri(576, {O(carth)}) == 0, "no_longer_member");
    check(ri(126, {}) == 2 && ro(577, {I(2)}) == invalid, "count_after_remove");
    check(game.partyTable().npcs[kNpcCarth].sheet.hp == 7
              && game.partyTable().npcs[kNpcCarth].sheet.xp == carthXpAfter,
          "stored_state_after_remove");
    run(393, {O(pc), I(500)});
    check(game.partyTable().npcs[kNpcCarth].sheet.xp == carthXpAfter, "away_gains_nothing");
    const std::uint32_t carthAgain = ro(698, {I(nw("NPC_CARTH")), {here(2.0f)}});
    check(carthAgain != invalid && ri(574, {I(nw("NPC_CARTH")), O(carthAgain)}) == 1,
          "rejoin_carth");
    check(game.partySheet(kNpcCarth) != nullptr && game.partySheet(kNpcCarth)->hp == 7
              && game.partySheet(kNpcCarth)->xp == carthXpAfter,
          "rejoin_keeps_hp_and_xp");
    game.update(0.05f, InputState{});
    check(game.partySheet(kNpcCarth)->hp == 7, "rejoin_hp_stable");

    // ---- leader / controlled character ------------------------------------------------------
    check(ri(13, {I(nw("NPC_JOLEE"))}) == 1, "set_leader_jolee");
    check(ro(577, {I(0)}) == jolee && ro(548, {}) == jolee && ro(577, {I(1)}) == pc,
          "leader_updates_index0_and_first_pc");
    check(game.partyLeaderTag() == game.partyTable().npcs[kNpcJolee].tag, "leader_tag");
    check(ri(13, {I(nw("NPC_BASTILA"))}) == 0 && ro(548, {}) == jolee, "leader_non_member_refused");
    check(ri(11, {I(nw("NPC_PLAYER"))}) == 1 && ro(548, {}) == pc && ro(577, {I(0)}) == pc,
          "switch_back_to_pc");
    check(ri(11, {I(nw("NPC_CARTH"))}) == 1 && ro(548, {}) == carthAgain, "switch_to_carth");
    check(ri(13, {I(nw("NPC_PLAYER"))}) == 1 && ro(548, {}) == pc, "leader_player");

    // ---- solo mode, selectability, head-follow lock -----------------------------------------
    check(ri(462, {}) == 0, "solo_off");
    run(753, {I(1)});
    check(ri(462, {}) == 1 && game.soloMode(), "solo_on");
    run(753, {I(0)});
    check(ri(462, {}) == 0, "solo_off_again");
    run(708, {I(nw("NPC_BASTILA")), I(0)});
    check(ri(709, {I(nw("NPC_BASTILA"))}) == 0, "selectability_set_get");
    run(506, {O(carthAgain), I(1)});
    check(game.headFollowLocked(carthAgain), "head_follow_locked");
    run(506, {O(carthAgain), I(0)});
    check(!game.headFollowLocked(carthAgain), "head_follow_unlocked");

    // ---- ShowPartySelectionGUI: forced NPC, toggles, accept, exit script ---------------------
    game.addScriptForTest("party_exit",
                          PartyScript().constInt(1).constString("PARTY_EXIT").action(581, 2).bytes());
    run(712, {S("party_exit"), I(nw("NPC_BASTILA")), I(-1)});
    {
        const PartySelectionState& selection = game.partySelection();
        check(selection.active && selection.exitScript == "party_exit"
                  && selection.chosen.size() == 2 && selection.chosen[0] == kNpcBastila,
              "selection_opened_with_forced");
    }
    check(!game.partySelectionToggle(kNpcBastila), "selection_forced_stays");
    check(!game.partySelectionToggle(kNpcMission), "selection_unavailable_refused");
    // The forced NPC leaves one free slot, held by the first current member.
    const int kept = game.partySelection().chosen.size() > 1 ? game.partySelection().chosen[1]
                                                              : kNpcJolee;
    const int other = kept == kNpcJolee ? kNpcCarth : kNpcJolee;
    check(!game.partySelectionToggle(other), "selection_full_refused");
    check(game.partySelectionToggle(kept) && game.partySelectionToggle(kNpcJolee),
          "selection_swap");
    check(game.isNpcPartyMember(kNpcCarth) && !game.isNpcPartyMember(kNpcBastila),
          "selection_changes_nothing_before_accept");
    run(708, {I(nw("NPC_BASTILA")), I(1)});
    check(game.acceptPartySelection(), "selection_accepted");
    check(!game.partySelection().active && game.isNpcPartyMember(kNpcBastila)
              && game.isNpcPartyMember(kNpcJolee) && !game.isNpcPartyMember(kNpcCarth),
          "selection_applied");
    check(game.scriptWorld().getGlobal("PARTY_EXIT").integer == 1, "selection_exit_script_ran");
    check(game.partySheet(kNpcBastila) != nullptr
              && game.partySheet(kNpcBastila)->classId == bastilaUtc.classId,
          "bastila_sheet_class");
    const std::uint32_t bastilaObject = game.partyTable().npcs[kNpcBastila].object;
    check(bastilaObject != invalid && api.entityFor(bastilaObject) != kNone, "bastila_entity");

    logMembers("after selection");
    // ---- death and revive (engine rule) -------------------------------------------------------
    {
        const std::size_t joleeEntity = api.entityFor(jolee);
        if (joleeEntity != kNone) {
            game.scene().entities[joleeEntity].hitPoints = 0;
        }
        game.update(0.05f, InputState{});
        check(game.partyMemberDown(kNpcJolee) && !game.partyAllDown(), "member_down");
        check(game.revivePartyMember(kNpcJolee, 5) && !game.partyMemberDown(kNpcJolee)
                  && game.partySheet(kNpcJolee)->hp == 5,
              "member_revived");
        game.update(0.05f, InputState{});
        check(!game.partyMemberDown(kNpcJolee), "member_stays_up");
    }

    logMembers("before module");
    // ---- module transition ----------------------------------------------------------------------
    run(13, {I(nw("NPC_JOLEE"))});
    const std::string startModule = game.module();
    const std::int32_t joleeXpBefore = game.partySheet(kNpcJolee) ? game.partySheet(kNpcJolee)->xp : -1;
    const std::int32_t bastilaXpBefore = game.partySheet(kNpcBastila) ? game.partySheet(kNpcBastila)->xp : -1;
    // A Taris module: the Endar Spire entry scripts rewrite the party themselves
    // (Bastila leaves it there), which would hide what the transition does.
    const std::string nextModule = "tar_m02aa";
    run(509, {S(nextModule), S(""), S(""), S(""), S(""), S(""), S(""), S("")});
    game.update(0.05f, InputState{});
    check(game.module() != startModule, "module_changed");
    {
        std::string listed;
        for (const int m : game.partyTable().members) {
            listed += std::to_string(m) + " ";
        }
        logInfo("party family: members after module: " + listed);
    }
    check(game.isNpcPartyMember(kNpcJolee) && game.isNpcPartyMember(kNpcBastila),
          "party_survives_module");
    check(xpOf(game, kNpcJolee) == joleeXpBefore
              && xpOf(game, kNpcBastila) == bastilaXpBefore,
          "party_xp_survives_module");
    check(game.partyTable().npcs[kNpcJolee].object == jolee && api.entityFor(jolee) != kNone
              && api.entityFor(game.partyTable().npcs[kNpcBastila].object) != kNone,
          "party_objects_and_entities_survive_module");
    check(ro(548, {}) == jolee && ro(577, {I(0)}) == jolee && game.partyLeaderTag() != "PC",
          "leader_survives_module");

    // ---- engine save / load -------------------------------------------------------------------
    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_family_party.save";
    check(game.save(savePath), "engine_save_written");
    {
        GameSession restored(game.installPath(), game.module());
        check(restored.load(savePath), "engine_save_loaded");
        const PartyTable& a = game.partyTable();
        const PartyTable& b = restored.partyTable();
        const auto dump = [](const char* label, const PartyTable& t) {
            std::string text;
            for (const int m : t.members) {
                text += std::to_string(m) + ",";
            }
            logInfo(concat("party family: ", label, " members=", text, " leader=", t.leader,
                           " controlled=", t.controlled, " pool=", t.xpPool, " solo=", t.solo));
        };
        dump("saved", a);
        dump("loaded", b);
        check(a.members == b.members && a.leader == b.leader && a.controlled == b.controlled
                  && a.xpPool == b.xpPool && a.solo == b.solo,
              "engine_roundtrip_table");
        bool same = true;
        for (const int npc : a.members) {
            const CharacterSheet* before = game.partySheet(npc);
            const CharacterSheet* after = restored.partySheet(npc);
            if (before != nullptr && after != nullptr) {
                logInfo(concat("party family: sheet npc=", npc, " xp ", before->xp, "/", after->xp,
                               " hp ", before->hp, "/", after->hp, " lvl ", before->level, "/",
                               after->level, " class ", before->classId, "/", after->classId,
                               " feats ", before->feats.size(), "/", after->feats.size(),
                               " powers ", before->powers.size(), "/", after->powers.size(),
                               " name ", before->name, "/", after->name, " obj ",
                               a.npcs[static_cast<std::size_t>(npc)].object, "/",
                               b.npcs[static_cast<std::size_t>(npc)].object));
            } else {
                logInfo(concat("party family: sheet npc=", npc, " missing"));
            }
            same = same && before != nullptr && after != nullptr && before->xp == after->xp
                && before->hp == after->hp && before->level == after->level
                && before->classId == after->classId && before->feats == after->feats
                && before->powers == after->powers && before->name == after->name
                && b.npcs[static_cast<std::size_t>(npc)].object
                    == a.npcs[static_cast<std::size_t>(npc)].object;
        }
        check(same, "engine_roundtrip_sheets");
        check(b.npcs[kNpcBastila].select == a.npcs[kNpcBastila].select
                  && b.npcs[kNpcCarth].available == a.npcs[kNpcCarth].available
                  && b.npcs[kNpcCarth].sheet.hp == 7,
              "engine_roundtrip_slots");
        check(restored.controlledObject() == game.controlledObject()
                  && restored.partyObjects() == game.partyObjects(),
              "engine_roundtrip_leader_objects");
    }

    // ---- original save: party table and AVAILNPC -----------------------------------------------
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "kotor_family_party_original";
    std::error_code removeError;
    std::filesystem::remove_all(folder, removeError);
    check(game.saveOriginalSave(folder, "party gate"), "original_save_written");
    {
        const kotor::OriginalSave written = kotor::readOriginalSave(folder);
        std::vector<int> ids;
        bool leaderFlag = false;
        for (const auto& [id, leads] : written.party) {
            ids.push_back(id);
            leaderFlag = leaderFlag || (leads && id == nw("NPC_JOLEE"));
        }
        logInfo(concat("party family: original ids=", ids.size(), " leaderFlag=", leaderFlag,
                       " controlled=", written.controlledNpc, " pool=", written.xpPool,
                       " gamePool=", game.partyTable().xpPool));
        for (const int id : ids) {
            logInfo(concat("party family: original id ", id));
        }
        std::sort(ids.begin(), ids.end());
        check(ids == std::vector<int>{nw("NPC_BASTILA"), nw("NPC_JOLEE")} && leaderFlag
                  && written.controlledNpc == nw("NPC_JOLEE")
                  && written.xpPool == game.partyTable().xpPool,
              "original_party_ids_are_npc_constants");
        const kotor::SavedNpc& stored = written.npcs[kNpcJolee];
        check(stored.available && stored.hasCreature && stored.classId == joleeUtc.classId
                  && stored.templateResref == "p_jolee"
                  && stored.xp == xpOf(game, kNpcJolee),
              "original_availnpc_jolee");
        check(written.npcs[kNpcCarth].available && written.npcs[kNpcCarth].hasCreature
                  && written.npcs[kNpcCarth].hp == 7,
              "original_availnpc_nonmember_state");
    }
    {
        // A fresh session reads the engine-written slot back.
        GameSession reloaded(game.installPath(), game.module());
        check(reloaded.loadOriginalSave(folder), "original_save_loaded");
        check(reloaded.isNpcPartyMember(kNpcJolee) && reloaded.isNpcPartyMember(kNpcBastila)
                  && reloaded.partyTable().controlled == kNpcJolee
                  && xpOf(reloaded, kNpcJolee) == xpOf(game, kNpcJolee)
                  && reloaded.partySheet(kNpcCarth) != nullptr
                  && reloaded.partySheet(kNpcCarth)->hp == 7,
              "original_roundtrip");
    }
    {
        // A retail-shaped PARTYTABLE: NPC-id members, no PC entry, availability and
        // select bytes per NPC, no stored creatures. The slot is patched in place.
        const std::filesystem::path table = folder / "PARTYTABLE.res";
        std::ifstream in(table, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        in.close();
        kotor::GffNodePtr root =
            kotor::GffNode::fromGff(kotor::Gff::parse(std::move(bytes), "PARTYTABLE.res"));
        std::vector<kotor::GffNodePtr> members;
        for (const int npc : {nw("NPC_MISSION"), nw("NPC_ZAALBAR")}) {
            auto member = std::make_shared<kotor::GffNode>(0);
            member->addInt32("PT_MEMBER_ID", npc);
            member->addUInt8("PT_IS_LEADER", 0);
            members.push_back(std::move(member));
        }
        if (kotor::GffNodeField* field = root->find("PT_MEMBERS")) {
            field->list = std::move(members);
        }
        if (kotor::GffNodeField* field = root->find("PT_AVAIL_NPCS")) {
            for (std::size_t i = 0; i < field->list.size(); ++i) {
                const bool listed = static_cast<int>(i) == nw("NPC_MISSION")
                    || static_cast<int>(i) == nw("NPC_ZAALBAR");
                if (kotor::GffNodeField* avail = field->list[i]->find("PT_NPC_AVAIL")) {
                    avail->integer = listed ? 1 : 0;
                }
            }
        }
        if (kotor::GffNodeField* field = root->find("PT_CONTROLLED_NP")) {
            field->integer = nw("NPC_PLAYER");
        }
        const std::vector<std::uint8_t> patched = kotor::writeGff("PT  ", *root);
        std::ofstream out(table, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(patched.data()),
                  static_cast<std::streamsize>(patched.size()));
        out.close();
        GameSession retail(game.installPath(), game.module());
        check(retail.loadOriginalSave(folder), "retail_shaped_loaded");
        const Utc missionUtc = readUtc("p_mission");
        const Utc zaalbarUtc = readUtc("p_zaalbar");
        check(retail.partyTable().members == std::vector<int>{kNpcMission, kNpcZaalbar},
              "retail_ids_map_to_mission_zaalbar");
        const CharacterSheet* mission = retail.partySheet(kNpcMission);
        const CharacterSheet* zaalbar = retail.partySheet(kNpcZaalbar);
        check(mission != nullptr && zaalbar != nullptr && mission->classId == missionUtc.classId
                  && zaalbar->classId == zaalbarUtc.classId && mission->name == missionUtc.name
                  && zaalbar->name == zaalbarUtc.name,
              "retail_members_class_and_name");
        bool spawned = true;
        for (const int npc : {kNpcMission, kNpcZaalbar}) {
            const std::uint32_t object = retail.partyTable().npcs[static_cast<std::size_t>(npc)].object;
            bool found = false;
            for (std::size_t i = 0; i < retail.scene().entities.size(); ++i) {
                const kotor::Entity& entity = retail.scene().entities[i];
                found = found
                    || (object != invalid && retail.objectForEntityIndex(i) == object
                        && entity.templateResref == (npc == kNpcMission ? "p_mission" : "p_zaalbar"));
            }
            spawned = spawned && found;
        }
        check(spawned, "retail_members_spawned_from_templates");
        check(retail.partyTable().controlled == kNpcPlayer && !retail.isNpcPartyMember(kNpcJolee),
              "retail_leader_is_pc");
    }

    // ---- a module-local template does not travel: the stored state does --------------------
    {
        kotor::Install moduleInstall(game.installPath());
        moduleInstall.bindModule("end_m01aa");
        const bool haveLocal = moduleInstall.find("p_carth001", kUtcType).has_value();
        check(haveLocal, "xmod_fixture_local_template");
        check(game.partyTable().defaultTemplate[kNpcCarth] == "p_carth", "xmod_default_template_is_global");
        if (haveLocal) {
            GameSession fresh(game.installPath(), "end_m01aa");
            script::Vm freshVm({});
            ScriptApi freshApi{fresh};
            const auto act = [&](std::uint16_t id, Args args) {
                const auto result = fresh.action(id, args, fresh.scriptWorld(), freshVm);
                return result.has_value() ? *result : std::vector<script::Value>{};
            };
            const auto freshHere = [&]() {
                LocationData at;
                at.position = fresh.position();
                at.position.x += 2.0f;
                return script::Value::makeEngine(freshApi.sim().engine.add(at));
            };
            const auto first = [&](const std::vector<script::Value>& cells) -> std::int64_t {
                return cells.empty() ? std::int64_t{-9999} : cells[0].integer;
            };
            check(first(act(697, {I(nw("NPC_CARTH")), S("p_carth001")})) == 1, "xmod_add_local_template");
            check(fresh.partyTable().npcs[kNpcCarth].templateResref == "p_carth001",
                  "xmod_slot_holds_local_template");
            const std::uint32_t local = static_cast<std::uint32_t>(
                first(act(698, {I(nw("NPC_CARTH")), {freshHere()}})));
            check(local != invalid && first(act(574, {I(nw("NPC_CARTH")), O(local)})) == 1,
                  "xmod_spawn_and_join_local");
            act(393, {O(fresh.scriptWorld().pc), I(300)});
            const std::int32_t xpBefore = xpOf(fresh, kNpcCarth);
            const std::string nameBefore =
                fresh.partySheet(kNpcCarth) != nullptr ? fresh.partySheet(kNpcCarth)->name : "";
            act(509, {S("tar_m02aa"), S(""), S(""), S(""), S(""), S(""), S(""), S("")});
            fresh.update(0.05f, InputState{});
            check(fresh.module() != "end_m01aa" && fresh.isNpcPartyMember(kNpcCarth),
                  "xmod_member_after_module");
            const std::size_t freshCarth = freshApi.entityFor(fresh.partyTable().npcs[kNpcCarth].object);
            check(freshCarth != kNone, "xmod_entity_in_new_module");
            if (freshCarth != kNone) {
                const std::string used = lowerStr(fresh.scene().entities[freshCarth].templateResref);
                check(used == "p_carth", "xmod_global_template_spawns");
                check(fresh.scene().entities[freshCarth].name == nameBefore, "xmod_name_kept");
            }
            check(xpOf(fresh, kNpcCarth) == xpBefore && xpBefore > 0, "xmod_xp_travels");
            // Leave and rejoin in the new module: the slot's creature spawns again.
            check(first(act(575, {I(nw("NPC_CARTH"))})) == 1, "xmod_remove_in_new_module");
            const std::uint32_t again = static_cast<std::uint32_t>(
                first(act(698, {I(nw("NPC_CARTH")), {freshHere()}})));
            check(again != invalid && first(act(574, {I(nw("NPC_CARTH")), O(again)})) == 1
                      && xpOf(fresh, kNpcCarth) == xpBefore,
                  "xmod_rejoin_in_new_module");
        }
    }

    logInfo(ok ? "party family: PASS" : "party family: FAIL");
    return ok ? 0 : 10;
}

} // namespace ke
