// Object queries: locations, tags, area iteration, nearest searches, creature
// stats read from the UTC, the game clock and the debug print actions.
// Retail addresses in comments name the engine function a rule was read from
// (disassembly; the local exports have no pseudocode for this family). Where
// the evidence is ambiguous the comment says so. The gate is runObjectsFamilyTest.
#include "game/ScriptHost.h"

#include "core/Log.h"
#include "game/GameSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>

namespace ke {

namespace {

constexpr float kDegrees = 180.0f / 3.14159265358979f;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
// GameObject::area of every object in the loaded area.
constexpr std::int64_t kAreaId = 100;
constexpr std::int64_t kTypeCreature = 1;
constexpr std::int64_t kTypeItem = 2;
constexpr std::int64_t kTypeAreaOfEffect = 16;
constexpr std::int64_t kAllTypes = 32767;
// World cursors. The area cursor is one per area (retail 0053c390), so a nested
// iteration clobbers the outer one exactly as it does there.
constexpr const char* kAreaCursor = "world_sim_area";
constexpr const char* kShapeCursor = "objects_shape";
constexpr const char* kPersistentCursor = "objects_persistent";
// An area-of-effect object without a stored radius covers 5 m (engine value, no
// retail value recovered).
constexpr float kDefaultAoeRadius = 5.0f;
constexpr std::int32_t kRaceHuman = 6;

using Cells = std::optional<std::vector<script::Value>>;

std::string lowerText(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// nwscript OBJECT_TYPE_* for an entity kind.
std::int64_t kindMask(kotor::EntityKind kind)
{
    switch (kind) {
    case kotor::EntityKind::Creature: return 1;
    case kotor::EntityKind::Trigger: return 4;
    case kotor::EntityKind::Door: return 8;
    case kotor::EntityKind::Waypoint: return 32;
    case kotor::EntityKind::Placeable: return 64;
    case kotor::EntityKind::Store: return 128;
    case kotor::EntityKind::Encounter: return 256;
    case kotor::EntityKind::Sound: return 512;
    case kotor::EntityKind::Unknown: break;
    }
    return 0;
}

std::int64_t attrInt(const script::GameObject& object, const char* key, std::int64_t fallback = 0)
{
    const auto it = object.attrs.find(key);
    if (it == object.attrs.end()) {
        return fallback;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&it->second)) {
        return *integer;
    }
    if (const auto* real = std::get_if<double>(&it->second)) {
        return static_cast<std::int64_t>(*real);
    }
    return fallback;
}

double attrReal(const script::GameObject& object, const char* key, double fallback)
{
    const auto it = object.attrs.find(key);
    if (it == object.attrs.end()) {
        return fallback;
    }
    if (const auto* real = std::get_if<double>(&it->second)) {
        return *real;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&it->second)) {
        return static_cast<double>(*integer);
    }
    return fallback;
}

// The object-type mask of anything a script can hold: the PC is a creature,
// entities follow their kind, items and spawned objects carry object_type, and
// the area and module are 0 (not positioned in the world).
std::int64_t typeMaskOf(const ScriptApi& api, std::uint32_t id)
{
    if (id == api.pc()) {
        return kTypeCreature;
    }
    const script::GameObject* object = api.world().get(id);
    if (object == nullptr) {
        return 0;
    }
    const std::size_t entity = api.entityFor(id);
    if (entity != kNone) {
        return kindMask(api.scene().entities[entity].kind);
    }
    if (object->area != kAreaId) {
        return 0;
    }
    return attrInt(*object, "object_type");
}

// Anything with a place in the area: the PC, entities, spawned objects. Items
// held in an inventory carry area 100 but no position, so they are excluded.
bool isSpatial(const ScriptApi& api, std::uint32_t id)
{
    if (id == api.pc() || api.entityFor(id) != kNone) {
        return true;
    }
    const std::int64_t type = typeMaskOf(api, id);
    return type != 0 && type != kTypeItem;
}

float distance3(kotor::Vec3 a, kotor::Vec3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float distance2(kotor::Vec3 a, kotor::Vec3 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

int factionOf(const ScriptApi& api, std::uint32_t object)
{
    if (object == api.pc()) {
        return 0;
    }
    const std::size_t entity = api.entityFor(object);
    if (entity != kNone) {
        return static_cast<int>(api.scene().entities[entity].faction);
    }
    return api.session.factionOfObject(object);
}

// ---- the area's object list ---------------------------------------------------

struct AreaEntry {
    std::uint32_t id = 0;
    std::int64_t type = 0;
    std::size_t entity = kNone;
    bool destroyed = false;
};

// Every positioned object of the loaded area in creation order, destroyed ones
// included so a cursor index stays valid while a script destroys what it visits.
// Held items and the third-person avatar entity are not area objects.
std::vector<AreaEntry> scanArea(const ScriptApi& api)
{
    const std::vector<kotor::Entity>& entities = api.scene().entities;
    std::unordered_map<std::uint32_t, std::size_t> entityOf;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const std::uint32_t object = api.objectFor(i);
        if (object != 0 && object != script::kObjectInvalid) {
            entityOf.emplace(object, i);
        }
    }
    const std::size_t avatar = api.session.playerEntity();
    std::vector<AreaEntry> out;
    for (const auto& [id, object] : api.world().objects) {
        if (object.area != kAreaId) {
            continue;
        }
        AreaEntry entry;
        entry.id = id;
        entry.destroyed = object.attrs.contains("destroyed");
        if (id == api.pc()) {
            entry.type = kTypeCreature;
        } else if (const auto found = entityOf.find(id); found != entityOf.end()) {
            entry.entity = found->second;
            entry.type = kindMask(entities[found->second].kind);
        } else {
            entry.type = attrInt(object, "object_type");
        }
        if (entry.type == 0 || entry.type == kTypeItem) {
            continue;
        }
        if (entry.entity != kNone && entry.entity == avatar) {
            continue;
        }
        out.push_back(entry);
    }
    return out;
}

std::string tagOf(const ScriptApi& api, const AreaEntry& entry)
{
    if (entry.entity != kNone) {
        return api.scene().entities[entry.entity].tag;
    }
    const script::GameObject* object = api.world().get(entry.id);
    return object != nullptr ? object->tag : std::string{};
}

// ---- creature data -----------------------------------------------------------

struct Sheet {
    std::int32_t race = 0;
    std::int32_t subRace = 0;
    std::int32_t gender = 0;
    std::int32_t appearance = 0;
    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    std::int32_t faction = 0;
    float challengeRating = 0.0f;
    std::array<std::int32_t, 6> abilities{10, 10, 10, 10, 10, 10};
    std::vector<std::pair<std::int32_t, std::int32_t>> classes;
    std::vector<std::int32_t> skills;
};

const CompanionState* companionFor(const ScriptApi& api, const kotor::Entity& entity)
{
    const std::string tag = lowerText(entity.tag);
    for (const CompanionState& companion : api.session.companions()) {
        if (lowerText(companion.tag) == tag) {
            return &companion;
        }
    }
    return nullptr;
}

void applySheet(const ScriptApi& api, const CharacterSheet& source, Sheet& out)
{
    out.abilities = {source.str, source.dex, source.con, source.intel, source.wis, source.cha};
    out.classes.assign(1, {source.classId, source.level});
    out.skills.clear();
    const Rules& rules = api.session.rules();
    for (std::int32_t i = 0; i < rules.skillCount(); ++i) {
        const auto found = source.skills.find(rules.skillName(i));
        out.skills.push_back(found != source.skills.end() ? found->second : 0);
    }
}

// The player's gender is the portrait the player picked (portraits.2da sex).
std::int32_t playerGender(const ScriptApi& api)
{
    const Rules& rules = api.session.rules();
    const std::uint32_t appearance = api.session.playerAppearance();
    for (std::int32_t i = 0; i < rules.portraitCount(); ++i) {
        const PortraitInfo info = rules.portrait(i);
        if (info.appearance == appearance) {
            return info.sex;
        }
    }
    return 0;
}

// What a creature is, from the UTC (entities) or the live character (the player
// and companions, whose level, abilities and skills are the sheet's). Nullopt for
// anything that is not a creature.
std::optional<Sheet> sheetOf(const ScriptApi& api, std::uint32_t id)
{
    Sheet out;
    if (id == api.pc()) {
        const CharacterSheet& character = api.session.character();
        applySheet(api, character, out);
        out.race = kRaceHuman; // The player is always human in K1.
        out.gender = playerGender(api);
        out.appearance = static_cast<std::int32_t>(api.session.playerAppearance());
        out.hp = api.session.playerHp();
        out.maxHp = api.session.playerMaxHp();
        return out;
    }
    const std::size_t index = api.entityFor(id);
    if (index == kNone || api.scene().entities[index].kind != kotor::EntityKind::Creature) {
        return std::nullopt;
    }
    const kotor::Entity& entity = api.scene().entities[index];
    const kotor::CreatureStats& stats = entity.stats;
    out.race = stats.race;
    out.subRace = stats.subRace;
    out.gender = stats.gender;
    out.appearance = static_cast<std::int32_t>(entity.appearance);
    out.hp = entity.dead ? 0 : std::max<std::int32_t>(0, entity.hitPoints);
    out.maxHp = entity.maxHitPoints;
    out.faction = static_cast<std::int32_t>(entity.faction);
    out.challengeRating = stats.challengeRating;
    out.abilities = stats.abilities;
    out.classes = stats.classes;
    out.skills = stats.skills;
    if (const CompanionState* companion = companionFor(api, entity); companion != nullptr) {
        applySheet(api, companion->sheet, out);
    }
    return out;
}

std::int32_t abilityModifier(std::int32_t score)
{
    return static_cast<std::int32_t>(std::floor((static_cast<double>(score) - 10.0) / 2.0));
}

std::int32_t totalLevels(const Sheet& sheet)
{
    std::int32_t total = 0;
    for (const auto& entry : sheet.classes) {
        total += entry.second;
    }
    return total;
}

// ---- skills ------------------------------------------------------------------

const kotor::TwoDa* skillsTable(const ScriptApi& api)
{
    static std::optional<kotor::TwoDa> table;
    static bool tried = false;
    if (!tried) {
        tried = true;
        try {
            auto data = api.install().find("skills", 2017);
            if (data.has_value()) {
                table = kotor::TwoDa::parse(std::move(*data), "skills");
            }
        } catch (const kotor::FormatError&) {
            table.reset();
        }
    }
    return table.has_value() ? &*table : nullptr;
}

struct SkillInfo {
    bool known = false;
    bool untrained = true;
    std::int32_t ability = -1; // 0..5 in Str Dex Con Int Wis Cha order
};

SkillInfo skillInfo(const ScriptApi& api, std::int64_t skill)
{
    SkillInfo info;
    const kotor::TwoDa* table = skillsTable(api);
    if (table == nullptr || skill < 0 || skill >= static_cast<std::int64_t>(table->rowCount())) {
        return info;
    }
    info.known = true;
    const std::size_t row = static_cast<std::size_t>(skill);
    const int untrainedColumn = table->columnIndex("untrained");
    if (untrainedColumn >= 0) {
        info.untrained = table->cell(row, static_cast<std::size_t>(untrainedColumn)) != "0";
    }
    const int abilityColumn = table->columnIndex("keyability");
    if (abilityColumn >= 0) {
        const std::string key = lowerText(table->cell(row, static_cast<std::size_t>(abilityColumn)));
        static const char* const kAbilityNames[6] = {"str", "dex", "con", "int", "wis", "cha"};
        for (std::int32_t i = 0; i < 6; ++i) {
            if (key.rfind(kAbilityNames[i], 0) == 0) {
                info.ability = i;
            }
        }
    }
    return info;
}

// retail 005aa570: -1 for a non-creature; a skill past the table is 0; an
// untrained skill the table does not allow is 0; otherwise the rank plus the key
// ability modifier. The player and companions add their feat bonuses
// (Rules::skillBonus); item, effect and armour penalties are not modelled here
// (evidence gap).
Cells getSkillRank(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int64_t skill = call.intArg(0);
    const std::uint32_t target = call.objectArg(1);
    const std::optional<Sheet> sheet = sheetOf(api, target);
    if (!sheet.has_value()) {
        return ActionCall::retInt(-1);
    }
    const SkillInfo info = skillInfo(api, skill);
    if (!info.known) {
        return ActionCall::retInt(0);
    }
    const std::size_t slot = static_cast<std::size_t>(skill);
    const std::int32_t rank = slot < sheet->skills.size() ? sheet->skills[slot] : 0;
    if (rank == 0 && !info.untrained) {
        return ActionCall::retInt(0);
    }
    std::int32_t total = rank;
    if (info.ability >= 0) {
        total += abilityModifier(sheet->abilities[static_cast<std::size_t>(info.ability)]);
    }
    const Rules& rules = api.session.rules();
    if (rules.loaded()) {
        const std::string name = rules.skillName(static_cast<std::int32_t>(skill));
        if (target == api.pc()) {
            total += rules.skillBonus(api.session.character(), name);
        } else if (const std::size_t index = api.entityFor(target); index != kNone) {
            if (const CompanionState* companion = companionFor(api, api.scene().entities[index]);
                companion != nullptr) {
                total += rules.skillBonus(companion->sheet, name);
            }
        }
    }
    return ActionCall::retInt(total);
}

// GetHasSkill(skill, creature): true when the creature has ranks in it. No
// disassembly was recovered for this one; the rule follows nwscript.nss.
Cells getHasSkill(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int64_t skill = call.intArg(0);
    const std::optional<Sheet> sheet = sheetOf(api, call.objectArg(1));
    if (!sheet.has_value() || skill < 0
        || skill >= static_cast<std::int64_t>(sheet->skills.size())) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(sheet->skills[static_cast<std::size_t>(skill)] > 0 ? 1 : 0);
}

// ---- per-creature getters ------------------------------------------------------

// retail 0053cba0: the creature's race; 0 for anything else. (nwscript.nss says
// RACIAL_TYPE_INVALID; the disassembly returns 0.)
Cells getRacialType(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    return ActionCall::retInt(sheet.has_value() ? sheet->race : 0);
}

// retail 0053d5a0: the creature's gender; -1 for anything else.
Cells getGender(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    return ActionCall::retInt(sheet.has_value() ? sheet->gender : -1);
}

// retail 00546d90: the creature's subrace. For a valid non-creature object the
// original returns the raw object id (a quirk, reproduced); an invalid one is 0.
Cells getSubRace(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    const std::optional<Sheet> sheet = sheetOf(api, object);
    if (sheet.has_value()) {
        return ActionCall::retInt(sheet->subRace);
    }
    return ActionCall::retInt(api.world().get(object) != nullptr
                                  ? static_cast<std::int64_t>(object)
                                  : 0);
}

// retail 00536630: the creature's appearance type; 0 for anything else.
Cells getAppearanceType(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    return ActionCall::retInt(sheet.has_value() ? sheet->appearance : 0);
}

// GetChallengeRating: the UTC ChallengeRating. The player has none (0.0); no
// disassembly was recovered.
Cells getChallengeRating(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    return ActionCall::retFloat(sheet.has_value() ? static_cast<double>(sheet->challengeRating)
                                                  : 0.0);
}

// retail 00539570: the total of the creature's class levels; 0 for a non-creature.
Cells getHitDice(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    return ActionCall::retInt(sheet.has_value() ? totalLevels(*sheet) : 0);
}

// retail 00536ce0: positions are 1-based; past the creature's classes it is
// CLASS_TYPE_INVALID (255); an invalid object is 0.
Cells getClassByPosition(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int64_t position = call.intArg(0);
    const std::uint32_t object = call.objectArg(1);
    if (api.world().get(object) == nullptr) {
        return ActionCall::retInt(0);
    }
    const std::optional<Sheet> sheet = sheetOf(api, object);
    if (!sheet.has_value() || position < 1
        || position > static_cast<std::int64_t>(sheet->classes.size())) {
        return ActionCall::retInt(255);
    }
    const std::int32_t classId = sheet->classes[static_cast<std::size_t>(position - 1)].first;
    return ActionCall::retInt(classId > 0x20 ? 255 : classId);
}

Cells getLevelByPosition(ActionCall& call)
{
    const std::int64_t position = call.intArg(0);
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(1));
    if (!sheet.has_value() || position < 1
        || position > static_cast<std::int64_t>(sheet->classes.size())) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(sheet->classes[static_cast<std::size_t>(position - 1)].second);
}

// K1 creatures carry at most two class slots.
Cells getLevelByClass(ActionCall& call)
{
    const std::int64_t classId = call.intArg(0);
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(1));
    if (!sheet.has_value()) {
        return ActionCall::retInt(0);
    }
    for (std::size_t i = 0; i < sheet->classes.size() && i < 2; ++i) {
        if (sheet->classes[i].first == classId) {
            return ActionCall::retInt(sheet->classes[i].second);
        }
    }
    return ActionCall::retInt(0);
}

// GetAbilityScore(creature, ability): the base score; item and effect bonuses are
// not added (evidence gap).
Cells getAbilityScore(ActionCall& call)
{
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(0));
    const std::int64_t ability = call.intArg(1);
    if (!sheet.has_value() || ability < 0 || ability > 5) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(sheet->abilities[static_cast<std::size_t>(ability)]);
}

Cells getAbilityModifier(ActionCall& call)
{
    const std::int64_t ability = call.intArg(0);
    const std::optional<Sheet> sheet = sheetOf(call.api(), call.objectArg(1));
    if (!sheet.has_value() || ability < 0 || ability > 5) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(abilityModifier(sheet->abilities[static_cast<std::size_t>(ability)]));
}

// retail 00539610: 0 for an invalid object; creatures report their own hit points;
// a door or placeable reports the entity's stored value (UTD/UTP hit points are
// not parsed, so it is the entity default: evidence gap).
Cells hitPoints(ActionCall& call, bool maximum)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (object == api.pc()) {
        return ActionCall::retInt(std::max(0, maximum ? api.session.playerMaxHp()
                                                      : api.session.playerHp()));
    }
    const std::size_t index = api.entityFor(object);
    const script::GameObject* entry = api.world().get(object);
    if (index == kNone || entry == nullptr || entry->attrs.contains("destroyed")) {
        return ActionCall::retInt(0);
    }
    const kotor::Entity& entity = api.scene().entities[index];
    if (maximum) {
        return ActionCall::retInt(std::max<std::int32_t>(0, entity.maxHitPoints));
    }
    return ActionCall::retInt(entity.dead ? 0 : std::max<std::int32_t>(0, entity.hitPoints));
}

Cells getCurrentHitPoints(ActionCall& call)
{
    return hitPoints(call, false);
}

Cells getMaxHitPoints(ActionCall& call)
{
    return hitPoints(call, true);
}

// GetAC: the player's and companions' armour class from the rules; any other
// creature uses the defense the combat family derived; -1 otherwise.
Cells getAC(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    const Rules& rules = api.session.rules();
    if (object == api.pc()) {
        return ActionCall::retInt(rules.loaded() ? rules.armorClass(api.session.character()) : 10);
    }
    const std::size_t index = api.entityFor(object);
    if (index == kNone || api.scene().entities[index].kind != kotor::EntityKind::Creature) {
        return ActionCall::retInt(-1);
    }
    const kotor::Entity& entity = api.scene().entities[index];
    if (const CompanionState* companion = companionFor(api, entity);
        companion != nullptr && rules.loaded()) {
        return ActionCall::retInt(rules.armorClass(companion->sheet));
    }
    return ActionCall::retInt(entity.defense);
}

// GetFoundEnemyCreature (retail 00546e70) reads a combat-state field the engine
// does not track yet; the combat family records it as context "foundEnemy".
Cells getFoundEnemyCreature(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (api.world().get(object) == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    const ActorState& state = api.actor(object);
    const auto found = state.last.find("foundEnemy");
    return ActionCall::retObject(found != state.last.end() ? found->second
                                                           : script::kObjectInvalid);
}

// ---- tags, names, simple identity ------------------------------------------------

Cells getModule(ActionCall& call)
{
    return ActionCall::retObject(call.api().sim().moduleObject);
}

std::string localizedField(const ScriptApi& api, const kotor::Gff& gff, const char* label)
{
    const kotor::GffField* field = gff.field(gff.rootIndex(), label);
    if (field == nullptr) {
        return {};
    }
    if (field->type != kotor::GffType::LocString) {
        return gff.text(*field);
    }
    const std::uint32_t strref = gff.locStringStrref(*field);
    const kotor::Localization& localization = api.session.localization();
    if (strref != UINT32_MAX && localization.loaded()) {
        std::string text = localization.tlk().text(strref);
        if (!text.empty()) {
            return text;
        }
    }
    return gff.locString(*field);
}

std::string templateName(const ScriptApi& api, const std::string& resref, std::uint32_t type,
                         const char* label)
{
    if (resref.empty()) {
        return {};
    }
    try {
        auto data = api.install().find(resref, type);
        if (!data.has_value()) {
            return {};
        }
        const kotor::Gff gff = kotor::Gff::parse(std::move(*data), resref);
        return localizedField(api, gff, label);
    } catch (const kotor::FormatError&) {
        return {};
    }
}

// retail 0053c080: the display name of a positioned object. Creatures are first
// and last name; doors and placeables read the template LocName (the entity does
// not keep it); the rest use the entity name. Items and non-spatial objects fall
// through to the legacy action.
Cells getName(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (object == api.pc()) {
        return ActionCall::retString(api.session.character().name);
    }
    const std::size_t index = api.entityFor(object);
    if (index == kNone) {
        return std::nullopt;
    }
    const kotor::Entity& entity = api.scene().entities[index];
    switch (entity.kind) {
    case kotor::EntityKind::Creature: {
        std::string name = entity.name;
        const std::string last = templateName(api, entity.templateResref, 2027, "LastName");
        if (!last.empty()) {
            name += name.empty() ? last : " " + last;
        }
        return ActionCall::retString(std::move(name));
    }
    case kotor::EntityKind::Door: {
        const std::string name = templateName(api, entity.templateResref, 2042, "LocName");
        return ActionCall::retString(name.empty() ? entity.name : name);
    }
    case kotor::EntityKind::Placeable: {
        const std::string name = templateName(api, entity.templateResref, 2044, "LocName");
        return ActionCall::retString(name.empty() ? entity.name : name);
    }
    default:
        break;
    }
    return ActionCall::retString(entity.name);
}

// retail 0053f1c0 (via 004b2a50): true only for the player object.
Cells getIsPC(ActionCall& call)
{
    ScriptApi api = call.api();
    return ActionCall::retInt(call.objectArg(0) == api.pc() ? 1 : 0);
}

// retail 00540170: the object id in lower-case hex.
Cells objectToString(ActionCall& call)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%x", static_cast<unsigned>(call.objectArgOrInvalid(0)));
    return ActionCall::retString(buffer);
}

Cells getObjectType(ActionCall& call)
{
    ScriptApi api = call.api();
    return ActionCall::retInt(typeMaskOf(api, call.objectArg(0)));
}

// retail 0053bec0 (0xd2): the module name without any path prefix.
Cells getModuleFileName(ActionCall& call)
{
    return ActionCall::retString(call.session.module());
}

// retail 0053b730: set while a saved game is being restored.
Cells getLoadFromSaveGame(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().restoringSave ? 1 : 0);
}

// retail 0053d610: the TLK text. The original also picks dialogf.tlk for a female
// player; this build has one table (evidence gap).
Cells getStringByStrRef(ActionCall& call)
{
    const kotor::Localization& localization = call.session.localization();
    if (!localization.loaded()) {
        return ActionCall::retString({});
    }
    const std::int64_t strref = call.intArg(0);
    if (strref < 0) {
        return ActionCall::retString({});
    }
    return ActionCall::retString(localization.tlk().text(static_cast<std::uint32_t>(strref)));
}

// retail 00543840: sets the "ship build" flag and returns 1.
Cells shipBuild(ActionCall&)
{
    return ActionCall::retInt(1);
}

// GetIsInConversation (retail 0053eec0): the object is in the conversation the
// session is running (the player, the speaker, the listener, or the owner).
Cells getIsInConversation(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (!api.dialogueActive()) {
        return ActionCall::retInt(0);
    }
    const DialogueView& view = call.session.dialogue();
    bool inside = object == api.pc() || object == view.speakerObject || object == view.listenerObject;
    if (!inside) {
        const std::size_t index = api.entityFor(object);
        inside = index != kNone && !view.npcTag.empty()
            && lowerText(api.scene().entities[index].tag) == lowerText(view.npcTag);
    }
    return ActionCall::retInt(inside ? 1 : 0);
}

// GetTrapBaseType: the UTD/UTP/UTT trap type row; 0 when the object has none.
Cells getTrapBaseType(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::size_t index = api.entityFor(call.objectArg(0));
    return ActionCall::retInt(index == kNone ? 0 : api.scene().entities[index].trapType);
}

// ---- distances and facing -----------------------------------------------------

// GetDistanceToObject: -1.0 when either object is not in the area (the legacy
// handler measured from the player instead of the running script's object).
Cells distanceToObject(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t from = call.self();
    const std::uint32_t to = call.objectArg(0);
    if (!isSpatial(api, from) || !isSpatial(api, to)) {
        return ActionCall::retFloat(-1.0);
    }
    return ActionCall::retFloat(distance3(api.position(from), api.position(to)));
}

Cells distanceToObject2D(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t from = call.self();
    const std::uint32_t to = call.objectArg(0);
    if (!isSpatial(api, from) || !isSpatial(api, to)) {
        return ActionCall::retFloat(-1.0);
    }
    return ActionCall::retFloat(distance2(api.position(from), api.position(to)));
}

// GetDistanceBetween: 0.0 on failure.
Cells distanceBetween(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t a = call.objectArg(0);
    const std::uint32_t b = call.objectArg(1);
    if (!isSpatial(api, a) || !isSpatial(api, b)) {
        return ActionCall::retFloat(0.0);
    }
    return ActionCall::retFloat(distance3(api.position(a), api.position(b)));
}

Cells distanceBetween2D(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t a = call.objectArg(0);
    const std::uint32_t b = call.objectArg(1);
    if (!isSpatial(api, a) || !isSpatial(api, b)) {
        return ActionCall::retFloat(0.0);
    }
    return ActionCall::retFloat(distance2(api.position(a), api.position(b)));
}

// retail 00537fe0: the facing in degrees, 0..360; -1.0 for an object without one.
Cells getFacing(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t target = call.objectArg(0);
    if (!isSpatial(api, target)) {
        return ActionCall::retFloat(-1.0);
    }
    float degrees = std::fmod(api.facing(target) * kDegrees, 360.0f);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return ActionCall::retFloat(degrees);
}

// ---- locations (Wave 1) --------------------------------------------------------

Cells getLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (!call.world.valid(object) && api.entityFor(object) == static_cast<std::size_t>(-1)) {
        return ActionCall::retEngine(0);
    }
    LocationData location;
    location.position = api.position(object);
    location.facing = api.facing(object) * kDegrees;
    location.area = api.sim().areaObject;
    return ActionCall::retEngine(api.sim().engine.add(location));
}

Cells makeLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    LocationData location;
    location.position = call.vectorArg(0);
    location.facing = static_cast<float>(call.floatArg(1));
    location.area = api.sim().areaObject;
    return ActionCall::retEngine(api.sim().engine.add(location));
}

Cells positionFromLocation(ActionCall& call)
{
    const LocationData* location = call.api().sim().engine.get<LocationData>(call.engineArg(0));
    return ActionCall::retVector(location != nullptr ? location->position : kotor::Vec3{});
}

Cells facingFromLocation(ActionCall& call)
{
    const LocationData* location = call.api().sim().engine.get<LocationData>(call.engineArg(0));
    return ActionCall::retFloat(location != nullptr ? location->facing : 0.0f);
}

// The K1 action table has no GetAreaFromLocation (TSL added it); a location's
// area is kept on the record so a later family can read it.
Cells getArea(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (object == api.sim().areaObject) {
        return ActionCall::retObject(object);
    }
    if (object == api.sim().moduleObject || call.world.get(object) == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(api.sim().areaObject);
}

// Creature templates only here: kotor::spawnCreature is the one spawn path the
// format layer has. Other object types get a script-visible object with no
// geometry until the object family gives them one.
Cells createObject(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int64_t type = call.intArg(0);
    const std::string resref = call.stringArg(1);
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(2));
    if (resref.empty() || location == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    if (type == 1) {
        return ActionCall::retObject(
            api.spawnCreature(resref, location->position, location->facing / kDegrees));
    }
    script::GameObject& object = call.world.addObject(resref, 100, 0, true);
    object.position = {location->position.x, location->position.y, location->position.z};
    object.attrs["object_type"] = type;
    return ActionCall::retObject(object.id);
}

// Saved as x y z + the facing direction, the layout of the original GVT record.
Cells getGlobalLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::array<float, 6>* saved = api.globalLocation(call.stringArg(0));
    if (saved == nullptr) {
        return ActionCall::retEngine(0);
    }
    LocationData location;
    location.position = {(*saved)[0], (*saved)[1], (*saved)[2]};
    location.facing = std::atan2((*saved)[4], (*saved)[3]) * kDegrees;
    location.area = api.sim().areaObject;
    return ActionCall::retEngine(api.sim().engine.add(location));
}

Cells setGlobalLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(1));
    if (location != nullptr) {
        const float radians = location->facing / kDegrees;
        api.setGlobalLocation(call.stringArg(0),
                              {location->position.x, location->position.y, location->position.z,
                               std::cos(radians), std::sin(radians), 0.0f});
    }
    return ActionCall::none();
}

// ---- area iteration (retail 0053c390) ---------------------------------------------

// GetFirstObjectInArea / GetNextObjectInArea. One cursor per area, so a nested
// loop leaves the outer one exhausted, as in the original. An invalid area means
// the caller's own; the module has none. Corpses are visited, destroyed objects
// and held items are not.
Cells nextInArea(ActionCall& call, bool first)
{
    ScriptApi api = call.api();
    const std::uint32_t area = call.objectArgOrInvalid(0);
    const std::int64_t mask = call.intArg(1, 1);
    std::uint32_t resolved = area;
    if (resolved == script::kObjectInvalid || call.world.get(resolved) == nullptr) {
        resolved = call.self() == api.sim().moduleObject ? script::kObjectInvalid
                                                         : api.sim().areaObject;
    }
    if (resolved != api.sim().areaObject) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    const std::vector<AreaEntry> entries = scanArea(api);
    std::int64_t position = first ? 0 : call.world.cursor(kAreaCursor);
    for (; position < static_cast<std::int64_t>(entries.size()); ++position) {
        const AreaEntry& entry = entries[static_cast<std::size_t>(position)];
        if (entry.destroyed || (entry.type & mask) == 0) {
            continue;
        }
        call.world.setCursor(kAreaCursor, position + 1);
        return ActionCall::retObject(entry.id);
    }
    call.world.setCursor(kAreaCursor, position);
    return ActionCall::retObject(script::kObjectInvalid);
}

Cells firstObjectInArea(ActionCall& call)
{
    return nextInArea(call, true);
}

Cells nextObjectInArea(ActionCall& call)
{
    return nextInArea(call, false);
}

// ---- nearest searches (retail 0054b550) --------------------------------------------

struct Criterion {
    std::int64_t type = -1;
    std::int64_t value = -1;
};

bool hasSpellEffect(const ScriptApi& api, std::uint32_t object, std::int64_t spell)
{
    const std::vector<kotor::ActiveEffect>* effects = nullptr;
    if (object == api.pc()) {
        effects = &api.session.character().effects;
    } else if (const std::size_t index = api.entityFor(object); index != kNone) {
        effects = &api.scene().entities[index].effects;
    }
    if (effects == nullptr) {
        return false;
    }
    return std::any_of(effects->begin(), effects->end(), [spell](const kotor::ActiveEffect& effect) {
        return effect.spellId == spell;
    });
}

// Seen and heard come from the perception lists the combat family keeps per
// creature (ActorState::perceived: bit 0 seen, bit 1 heard; retail 00517a20),
// refreshed by its perception pass each tick. Before that pass has run, nothing
// has been perceived.
bool perceptionPasses(const ScriptApi& api, std::uint32_t perceiver, std::uint32_t candidate,
                      std::int64_t mode)
{
    const std::map<std::uint32_t, std::uint8_t>& perceived = api.actor(perceiver).perceived;
    const auto found = perceived.find(candidate);
    const std::uint8_t bits = found != perceived.end() ? found->second : std::uint8_t{0};
    const bool seen = (bits & 1) != 0;
    const bool heard = (bits & 2) != 0;
    switch (mode) {
    case 0: return seen && heard;   // PERCEPTION_SEEN_AND_HEARD
    case 1: return !seen && !heard; // PERCEPTION_NOT_SEEN_AND_NOT_HEARD
    case 2: return !seen && heard;  // PERCEPTION_HEARD_AND_NOT_SEEN
    case 3: return seen && !heard;  // PERCEPTION_SEEN_AND_NOT_HEARD
    case 4: return !heard;          // PERCEPTION_NOT_HEARD
    case 5: return heard;           // PERCEPTION_HEARD
    case 6: return !seen;           // PERCEPTION_NOT_SEEN
    case 7: return seen;            // PERCEPTION_SEEN
    default: return true;
    }
}

// One CREATURE_TYPE_* test. IS_ALIVE passes everything: the disassembly shows no
// body for it, and dead creatures are excluded by the search itself.
bool criterionPasses(const ScriptApi& api, std::uint32_t perceiver, const AreaEntry& candidate,
                     const Criterion& criterion)
{
    switch (criterion.type) {
    case 0: { // CREATURE_TYPE_RACIAL_TYPE: ALL (7) matches, INVALID (8) never does
        if (criterion.value == 7) {
            return true;
        }
        if (criterion.value == 8) {
            return false;
        }
        const std::optional<Sheet> sheet = sheetOf(api, candidate.id);
        return sheet.has_value() && sheet->race == criterion.value;
    }
    case 1: // CREATURE_TYPE_PLAYER_CHAR
        return (candidate.id == api.pc()) == (criterion.value != 0);
    case 2: { // CREATURE_TYPE_CLASS
        const std::optional<Sheet> sheet = sheetOf(api, candidate.id);
        return sheet.has_value()
            && std::any_of(sheet->classes.begin(), sheet->classes.end(),
                           [&criterion](const auto& entry) { return entry.first == criterion.value; });
    }
    case 3: { // CREATURE_TYPE_REPUTATION: the searcher's standing toward the candidate
        const int standing = api.session.factions().reputation(factionOf(api, perceiver),
                                                               factionOf(api, candidate.id));
        if (criterion.value == 0) {
            return standing >= 90;
        }
        if (criterion.value == 1) {
            return standing <= 10;
        }
        return standing > 10 && standing < 90;
    }
    case 4: // CREATURE_TYPE_IS_ALIVE
        return true;
    case 5: // HAS_SPELL_EFFECT
        return hasSpellEffect(api, candidate.id, criterion.value);
    case 6: // DOES_NOT_HAVE_SPELL_EFFECT
        return !hasSpellEffect(api, candidate.id, criterion.value);
    case 7: // CREATURE_TYPE_PERCEPTION
        return perceptionPasses(api, perceiver, candidate.id, criterion.value);
    default:
        return true;
    }
}

// The nth object (1-based) of the mask nearest to `from` by squared distance, the
// origin object excluded. Ties keep creation order.
template <typename Accept>
std::uint32_t nearestMatching(const ScriptApi& api, kotor::Vec3 from, std::uint32_t exclude,
                              std::int64_t mask, std::int64_t nth, Accept accept)
{
    struct Hit {
        float squared;
        std::uint32_t id;
    };
    std::vector<Hit> hits;
    if (nth < 1) {
        return script::kObjectInvalid;
    }
    for (const AreaEntry& entry : scanArea(api)) {
        if (entry.destroyed || entry.id == exclude || (entry.type & mask) == 0 || !accept(entry)) {
            continue;
        }
        const kotor::Vec3 at = api.position(entry.id);
        const float dx = at.x - from.x;
        const float dy = at.y - from.y;
        const float dz = at.z - from.z;
        hits.push_back({dx * dx + dy * dy + dz * dz, entry.id});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) { return a.squared < b.squared; });
    if (nth > static_cast<std::int64_t>(hits.size())) {
        return script::kObjectInvalid;
    }
    return hits[static_cast<std::size_t>(nth - 1)].id;
}

// GetNearestObject(mask, target, nth)
Cells nearestObject(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t origin = call.objectArg(1);
    if (!isSpatial(api, origin)) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(nearestMatching(api, api.position(origin), origin,
                                                 call.intArg(0, kAllTypes), call.intArg(2, 1),
                                                 [](const AreaEntry&) { return true; }));
}

// GetNearestObjectToLocation(mask, location, nth)
Cells nearestObjectToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(1));
    if (location == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(nearestMatching(api, location->position, script::kObjectInvalid,
                                                 call.intArg(0, kAllTypes), call.intArg(2, 1),
                                                 [](const AreaEntry&) { return true; }));
}

// GetNearestObjectByTag(tag, target, nth). The original compares tags byte for
// byte; this compares case-insensitively like every other tag lookup here.
Cells nearestObjectByTag(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t origin = call.objectArg(1);
    if (!isSpatial(api, origin)) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    const std::string tag = lowerText(call.stringArg(0));
    return ActionCall::retObject(nearestMatching(
        api, api.position(origin), origin, kAllTypes, call.intArg(2, 1),
        [&](const AreaEntry& entry) { return lowerText(tagOf(api, entry)) == tag; }));
}

std::uint32_t creatureSearch(ActionCall& call, const ScriptApi& api, kotor::Vec3 from,
                             std::uint32_t exclude, std::uint32_t perceiver)
{
    const std::array<Criterion, 3> criteria{
        Criterion{call.intArg(0, -1), call.intArg(1, -1)},
        Criterion{call.intArg(4, -1), call.intArg(5, -1)},
        Criterion{call.intArg(6, -1), call.intArg(7, -1)}};
    return nearestMatching(api, from, exclude, kTypeCreature, call.intArg(3, 1),
                           [&](const AreaEntry& entry) {
                               if (!api.isAlive(entry.id)) {
                                   return false;
                               }
                               for (const Criterion& criterion : criteria) {
                                   if (criterion.type >= 0
                                       && !criterionPasses(api, perceiver, entry, criterion)) {
                                       return false;
                                   }
                               }
                               return true;
                           });
}

// GetNearestCreature(type1, value1, target, nth, type2, value2, type3, value3)
Cells nearestCreature(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t origin = call.objectArg(2);
    if (!isSpatial(api, origin)) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(creatureSearch(call, api, api.position(origin), origin, origin));
}

// GetNearestCreatureToLocation(type1, value1, location, nth, ...). The reputation
// and perception tests need a searcher; the running script's self stands in.
Cells nearestCreatureToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(2));
    if (location == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(creatureSearch(call, api, location->position,
                                                script::kObjectInvalid, call.self()));
}

// ---- shapes and persistent objects -----------------------------------------------

float triangleEdge(kotor::Vec3 a, kotor::Vec3 b, float px, float py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

bool insideTriangles(const std::vector<kotor::Vec3>& vertices,
                     const std::vector<std::uint32_t>& indices, float x, float y)
{
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size()
            || indices[i + 2] >= vertices.size()) {
            continue;
        }
        const kotor::Vec3 a = vertices[indices[i]];
        const kotor::Vec3 b = vertices[indices[i + 1]];
        const kotor::Vec3 c = vertices[indices[i + 2]];
        const float d1 = triangleEdge(a, b, x, y);
        const float d2 = triangleEdge(b, c, x, y);
        const float d3 = triangleEdge(c, a, x, y);
        const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
        const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
        if (!(negative && positive)) {
            return true;
        }
    }
    return false;
}

// Objects standing inside a trigger or encounter volume (its polygon, in plan) or
// inside an area-of-effect object (a disc around it). Creatures that are dead are
// not residents. Retail 00549360 walks the persistent object's own resident list;
// the engine has none, so membership is geometric (evidence gap).
std::vector<std::uint32_t> persistentMembers(const ScriptApi& api, std::uint32_t persistent,
                                             std::int64_t mask)
{
    const kotor::Scene& scene = api.scene();
    const std::size_t entity = api.entityFor(persistent);
    const std::vector<kotor::Vec3>* vertices = nullptr;
    const std::vector<std::uint32_t>* indices = nullptr;
    bool disc = false;
    kotor::Vec3 centre{};
    float radius = 0.0f;
    if (entity != kNone) {
        for (const kotor::TriggerVolume& volume : scene.triggers) {
            if (volume.entity == entity) {
                vertices = &volume.vertices;
                indices = &volume.indices;
                break;
            }
        }
        if (vertices == nullptr) {
            for (const kotor::EncounterVolume& volume : scene.encounters) {
                if (volume.entity == entity) {
                    vertices = &volume.vertices;
                    indices = &volume.indices;
                    break;
                }
            }
        }
    } else if (typeMaskOf(api, persistent) == kTypeAreaOfEffect) {
        disc = true;
        centre = api.position(persistent);
        radius = static_cast<float>(
            attrReal(*api.world().get(persistent), "radius", kDefaultAoeRadius));
    }
    std::vector<std::uint32_t> members;
    if (vertices == nullptr && !disc) {
        return members;
    }
    for (const AreaEntry& entry : scanArea(api)) {
        if (entry.destroyed || entry.id == persistent || (entry.type & mask) == 0) {
            continue;
        }
        if (entry.type == kTypeCreature && !api.isAlive(entry.id)) {
            continue;
        }
        const kotor::Vec3 at = api.position(entry.id);
        const bool inside = disc ? distance2(at, centre) <= radius
                                 : insideTriangles(*vertices, *indices, at.x, at.y);
        if (inside) {
            members.push_back(entry.id);
        }
    }
    return members;
}

Cells nextInPersistent(ActionCall& call, bool first)
{
    ScriptApi api = call.api();
    const std::uint32_t persistent = call.objectArg(0);
    const std::vector<std::uint32_t> members =
        persistentMembers(api, persistent, call.intArg(1, kTypeCreature));
    const std::int64_t position = first ? 0 : call.world.cursor(kPersistentCursor);
    if (position < 0 || position >= static_cast<std::int64_t>(members.size())) {
        call.world.setCursor(kPersistentCursor, static_cast<std::int64_t>(members.size()));
        return ActionCall::retObject(script::kObjectInvalid);
    }
    call.world.setCursor(kPersistentCursor, position + 1);
    return ActionCall::retObject(members[static_cast<std::size_t>(position)]);
}

Cells firstInPersistent(ActionCall& call)
{
    return nextInPersistent(call, true);
}

Cells nextInPersistentObject(ActionCall& call)
{
    return nextInPersistent(call, false);
}

// SHAPE_SPELLCYLINDER 0, CONE 1, CUBE 2, SPELLCONE 3, SPHERE 4. Sphere and cube
// are exact (point tests). A cylinder runs from the origin to the target with the
// size as its radius; a cone widens linearly from the origin to the size at the
// target. The line-of-sight flag is not applied (evidence gap).
bool insideShape(std::int64_t shape, float size, kotor::Vec3 centre, kotor::Vec3 origin,
                 kotor::Vec3 at)
{
    switch (shape) {
    case 4:
        return distance3(centre, at) <= size;
    case 2:
        return std::fabs(at.x - centre.x) <= size && std::fabs(at.y - centre.y) <= size
            && std::fabs(at.z - centre.z) <= size;
    case 0:
    case 1:
    case 3: {
        const float ax = centre.x - origin.x;
        const float ay = centre.y - origin.y;
        const float length2 = ax * ax + ay * ay;
        if (length2 <= 0.0f) {
            return distance2(at, centre) <= size;
        }
        const float along = ((at.x - origin.x) * ax + (at.y - origin.y) * ay) / length2;
        if (along < 0.0f || along > 1.0f) {
            return false;
        }
        const float px = origin.x + ax * along;
        const float py = origin.y + ay * along;
        const float lateral = std::hypot(at.x - px, at.y - py);
        return shape == 0 ? lateral <= size : lateral <= size * along;
    }
    default:
        return false;
    }
}

Cells nextInShape(ActionCall& call, bool first)
{
    ScriptApi api = call.api();
    const std::int64_t shape = call.intArg(0);
    const float size = static_cast<float>(call.floatArg(1));
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(2));
    const std::int64_t mask = call.intArg(4, kTypeCreature);
    if (location == nullptr) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    const kotor::Vec3 origin = call.vectorArg(5);
    std::vector<std::uint32_t> members;
    for (const AreaEntry& entry : scanArea(api)) {
        if (!entry.destroyed && (entry.type & mask) != 0
            && insideShape(shape, size, location->position, origin, api.position(entry.id))) {
            members.push_back(entry.id);
        }
    }
    const std::int64_t position = first ? 0 : call.world.cursor(kShapeCursor);
    if (position < 0 || position >= static_cast<std::int64_t>(members.size())) {
        call.world.setCursor(kShapeCursor, static_cast<std::int64_t>(members.size()));
        return ActionCall::retObject(script::kObjectInvalid);
    }
    call.world.setCursor(kShapeCursor, position + 1);
    return ActionCall::retObject(members[static_cast<std::size_t>(position)]);
}

Cells firstObjectInShape(ActionCall& call)
{
    return nextInShape(call, true);
}

Cells nextObjectInShape(ActionCall& call)
{
    return nextInShape(call, false);
}

// ---- the game clock ----------------------------------------------------------------

// Retail counts a day as 24 * Mod_MinPerHour real minutes. The time of day is
// `clockBaseMs` plus the world time in milliseconds, modulo that day. The hour is
// the minutes into the day divided by MinPerHour, the minute is the remainder,
// and seconds and milliseconds are the real ones (retail 004ae260 / 0x543670).
struct ModuleClock {
    std::int32_t minPerHour = 2;
    std::int32_t dawn = 0;
    std::int32_t dusk = 0;
    std::int32_t startHour = 13;
    std::int32_t startMinute = 0;
    std::int32_t startSecond = 0;
    std::int32_t startMs = 0;
};

ModuleClock readModuleClock(const ScriptApi& api)
{
    ModuleClock out;
    try {
        auto data = api.install().find("module", 2014);
        if (!data.has_value()) {
            return out;
        }
        const kotor::Gff ifo = kotor::Gff::parse(std::move(*data), "module.ifo");
        const std::uint32_t root = ifo.rootIndex();
        const auto read = [&](const char* label, std::int32_t fallback) {
            const kotor::GffField* field = ifo.field(root, label);
            return field != nullptr ? ifo.integer(*field) : fallback;
        };
        out.minPerHour = read("Mod_MinPerHour", out.minPerHour);
        out.dawn = read("Mod_DawnHour", out.dawn);
        out.dusk = read("Mod_DuskHour", out.dusk);
        out.startHour = read("Mod_StartHour", out.startHour);
        out.startMinute = read("Mod_StartMinute", out.startMinute);
        out.startSecond = read("Mod_StartSecond", out.startSecond);
        out.startMs = read("Mod_StartMiliSec", out.startMs);
    } catch (const kotor::FormatError&) {
        // An unreadable IFO leaves the defaults.
    }
    return out;
}

double dayLengthMs(const WorldSim& sim)
{
    return 24.0 * static_cast<double>(sim.clockMinPerHour) * 60000.0;
}

double wrapDay(double ms, double day)
{
    double wrapped = std::fmod(ms, day);
    if (wrapped < 0.0) {
        wrapped += day;
    }
    return wrapped;
}

// Reads the module's hour length and dawn and dusk hours the first time the clock
// is used and again after a module change. A module with MinPerHour 0 (a few
// shipped ones) uses 2, an engine fallback. The clock starts at the first module's
// Mod_StartHour; across modules the fraction of the day is kept.
void ensureClock(const ScriptApi& api)
{
    WorldSim& sim = api.sim();
    const std::string& module = api.session.module();
    if (sim.clockReady && sim.clockModule == module) {
        return;
    }
    const ModuleClock ifo = readModuleClock(api);
    const std::int32_t minPerHour = ifo.minPerHour > 0 ? ifo.minPerHour : 2;
    const double elapsedMs = api.world().time * 1000.0;
    if (!sim.clockReady) {
        sim.clockMinPerHour = minPerHour;
        sim.clockBaseMs = static_cast<double>(ifo.startHour) * minPerHour * 60000.0
            + static_cast<double>(ifo.startMinute) * 60000.0
            + static_cast<double>(ifo.startSecond) * 1000.0 + static_cast<double>(ifo.startMs);
        sim.clockBaseMs = wrapDay(sim.clockBaseMs, 24.0 * minPerHour * 60000.0);
        sim.clockReady = true;
    } else if (!sim.clockModule.empty()) {
        const double oldDay = dayLengthMs(sim);
        const double fraction = wrapDay(sim.clockBaseMs + elapsedMs, oldDay) / oldDay;
        sim.clockMinPerHour = minPerHour;
        sim.clockBaseMs = fraction * dayLengthMs(sim) - elapsedMs;
    } else {
        // Restored from a save: the saved base is already in this module's day.
        sim.clockMinPerHour = minPerHour;
    }
    sim.clockDawnHour = ifo.dawn;
    sim.clockDuskHour = ifo.dusk;
    sim.clockModule = module;
}

struct ClockParts {
    std::int32_t hour = 0;
    std::int32_t minute = 0;
    std::int32_t second = 0;
    std::int32_t millisecond = 0;
};

ClockParts clockParts(const ScriptApi& api)
{
    ensureClock(api);
    const WorldSim& sim = api.sim();
    const double ms = wrapDay(sim.clockBaseMs + api.world().time * 1000.0, dayLengthMs(sim));
    const std::int64_t whole = static_cast<std::int64_t>(ms);
    const std::int64_t minutes = whole / 60000;
    ClockParts parts;
    parts.hour = static_cast<std::int32_t>(minutes / sim.clockMinPerHour);
    parts.minute = static_cast<std::int32_t>(minutes % sim.clockMinPerHour);
    parts.second = static_cast<std::int32_t>((whole / 1000) % 60);
    parts.millisecond = static_cast<std::int32_t>(whole % 1000);
    return parts;
}

Cells getTimeHour(ActionCall& call)
{
    return ActionCall::retInt(clockParts(call.api()).hour);
}

Cells getTimeMinute(ActionCall& call)
{
    return ActionCall::retInt(clockParts(call.api()).minute);
}

Cells getTimeSecond(ActionCall& call)
{
    return ActionCall::retInt(clockParts(call.api()).second);
}

Cells getTimeMillisecond(ActionCall& call)
{
    return ActionCall::retInt(clockParts(call.api()).millisecond);
}

// SetTime(hour, minute, second, millisecond): refused when any argument is
// negative; carries run ms to s to m to h and the hour wraps at 24. The time of
// day becomes the target; the calendar day is not tracked here.
Cells setTime(ActionCall& call)
{
    ScriptApi api = call.api();
    std::int64_t hour = call.intArg(0);
    std::int64_t minute = call.intArg(1);
    std::int64_t second = call.intArg(2);
    std::int64_t millisecond = call.intArg(3);
    if (hour < 0 || minute < 0 || second < 0 || millisecond < 0) {
        return ActionCall::none();
    }
    ensureClock(api);
    WorldSim& sim = api.sim();
    second += millisecond / 1000;
    millisecond %= 1000;
    minute += second / 60;
    second %= 60;
    hour += minute / 60;
    minute %= 60;
    hour %= 24;
    const double target = static_cast<double>(hour) * sim.clockMinPerHour * 60000.0
        + static_cast<double>(minute) * 60000.0 + static_cast<double>(second) * 1000.0
        + static_cast<double>(millisecond);
    sim.clockBaseMs = wrapDay(target - api.world().time * 1000.0, dayLengthMs(sim));
    return ActionCall::none();
}

// retail 004c6210: 1 day, 2 night, 3 dawn (the dawn hour), 4 dusk (the dusk hour).
// Equal dawn and dusk hours (a module with neither set) is always day.
std::int32_t dayState(std::int32_t hour, std::int32_t dawn, std::int32_t dusk)
{
    if (dawn == dusk) {
        return 1;
    }
    if (hour == dawn) {
        return 3;
    }
    if (hour == dusk) {
        return 4;
    }
    if (dawn < dusk) {
        return hour < dawn || hour > dusk ? 2 : 1;
    }
    return hour >= dusk && hour <= dawn ? 2 : 1;
}

Cells timeOfDayIs(ActionCall& call, std::int32_t state)
{
    ScriptApi api = call.api();
    const ClockParts parts = clockParts(api);
    const WorldSim& sim = api.sim();
    return ActionCall::retInt(dayState(parts.hour, sim.clockDawnHour, sim.clockDuskHour) == state
                                  ? 1
                                  : 0);
}

Cells getIsDay(ActionCall& call)
{
    return timeOfDayIs(call, 1);
}

Cells getIsNight(ActionCall& call)
{
    return timeOfDayIs(call, 2);
}

Cells getIsDawn(ActionCall& call)
{
    return timeOfDayIs(call, 3);
}

Cells getIsDusk(ActionCall& call)
{
    return timeOfDayIs(call, 4);
}

// ---- debug output ---------------------------------------------------------------------

// retail 005417c0 writes the string and a newline to the log; AurPostString
// (005cb350) posts on-screen text. Both go to the script log here.
Cells printString(ActionCall& call)
{
    call.api().log("PrintString: " + call.stringArg(0));
    return ActionCall::none();
}

Cells auroraPostString(ActionCall& call)
{
    call.api().log("AurPostString: " + call.stringArg(0));
    return ActionCall::none();
}

Cells printFloat(ActionCall& call)
{
    const int width = static_cast<int>(std::clamp<std::int64_t>(call.intArg(1, 18), 0, 64));
    const int decimals = static_cast<int>(std::clamp<std::int64_t>(call.intArg(2, 9), 0, 20));
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, "%*.*f", width, decimals, call.floatArg(0));
    call.api().log(std::string("PrintFloat: ") + buffer);
    return ActionCall::none();
}

Cells printInteger(ActionCall& call)
{
    call.api().log("PrintInteger: " + std::to_string(call.intArg(0)));
    return ActionCall::none();
}

Cells printObject(ActionCall& call)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%x", static_cast<unsigned>(call.objectArgOrInvalid(0)));
    call.api().log(std::string("PrintObject: ") + buffer);
    return ActionCall::none();
}

Cells printVector(ActionCall& call)
{
    const kotor::Vec3 value = call.vectorArg(0);
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "(%f, %f, %f)", static_cast<double>(value.x),
                  static_cast<double>(value.y), static_cast<double>(value.z));
    call.api().log(std::string("PrintVector: ") + buffer);
    return ActionCall::none();
}

// Cheat codes, credits and the meta-magic feat have no state in this build.
Cells alwaysZero(ActionCall&)
{
    return ActionCall::retInt(0);
}

} // namespace

void registerObjectActions(HandlerTable& table)
{
    table.add(1, printString);
    table.add(2, printFloat);
    table.add(4, printInteger);
    table.add(5, printObject);
    table.add(12, setTime);
    table.add(16, getTimeHour);
    table.add(17, getTimeMinute);
    table.add(18, getTimeSecond);
    table.add(19, getTimeMillisecond);
    table.add(24, getArea);
    table.add(28, getFacing);
    table.add(38, nearestCreature);
    table.add(41, distanceToObject);
    table.add(49, getCurrentHitPoints);
    table.add(50, getMaxHitPoints);
    table.add(93, firstObjectInArea);
    table.add(94, nextObjectInArea);
    table.add(106, getObjectType);
    table.add(107, getRacialType);
    table.add(116, getAC);
    table.add(128, firstObjectInShape);
    table.add(129, nextObjectInShape);
    table.add(139, getAbilityScore);
    table.add(141, printVector);
    table.add(151, distanceBetween);
    table.add(166, getHitDice);
    table.add(210, getModuleFileName);
    table.add(213, getLocation);
    table.add(215, makeLocation);
    table.add(217, getIsPC);
    table.add(223, positionFromLocation);
    table.add(225, facingFromLocation);
    table.add(226, nearestCreatureToLocation);
    table.add(227, nearestObject);
    table.add(228, nearestObjectToLocation);
    table.add(229, nearestObjectByTag);
    table.add(239, getStringByStrRef);
    table.add(242, getModule);
    table.add(243, createObject);
    table.add(251, getLoadFromSaveGame);
    table.add(253, getName);
    table.add(262, firstInPersistent);
    table.add(263, nextInPersistentObject);
    table.add(272, objectToString);
    table.add(286, getHasSkill);
    table.add(315, getSkillRank);
    table.add(319, distanceBetween2D);
    table.add(331, getAbilityModifier);
    table.add(335, distanceToObject2D);
    table.add(341, getClassByPosition);
    table.add(342, getLevelByPosition);
    table.add(343, getLevelByClass);
    table.add(358, getGender);
    table.add(405, getIsDay);
    table.add(406, getIsNight);
    table.add(407, getIsDawn);
    table.add(408, getIsDusk);
    table.add(445, getIsInConversation);
    table.add(494, getChallengeRating);
    table.add(495, getFoundEnemyCreature);
    table.add(497, getSubRace);
    table.add(519, alwaysZero);
    table.add(524, getAppearanceType);
    table.add(531, getTrapBaseType);
    table.add(582, auroraPostString);
    table.add(692, getGlobalLocation);
    table.add(693, setGlobalLocation);
    table.add(761, shipBuild);
    table.add(764, alwaysZero);
}

// ---- the family gate: --family-test objects -------------------------------------------
//
// Runs on module end_m01aa. Creature numbers are compared with the UTC read
// straight from the install; object results are compared with a brute-force walk
// of the scene; the time and nearest-by-tag checks run shipped-format NCS through
// the real VM and read the answers back from script globals.

namespace {

// Just enough NCS assembly for an action call whose result goes to a global.
class MiniNcs {
public:
    MiniNcs& constInt(std::int32_t value)
    {
        op(0x04, 0x03);
        be32(static_cast<std::uint32_t>(value));
        return *this;
    }
    MiniNcs& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    MiniNcs& constObject(std::uint32_t value)
    {
        op(0x04, 0x06);
        be32(value);
        return *this;
    }
    MiniNcs& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    std::vector<std::uint8_t> bytes()
    {
        op(0x20, 0x00); // RETN
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

struct UtcFacts {
    std::int32_t race = 0;
    std::int32_t subRace = 0;
    std::int32_t gender = 0;
    std::int32_t appearance = 0;
    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    std::int32_t firstClass = -1;
    std::int32_t firstLevel = 0;
    std::int32_t levels = 0;
    float challengeRating = 0.0f;
    std::array<std::int32_t, 6> abilities{};
    std::vector<std::int32_t> skills;
};

std::optional<UtcFacts> readUtcFacts(ScriptApi& api, const std::string& resref)
{
    try {
        auto data = api.install().find(resref, 2027);
        if (!data.has_value()) {
            return std::nullopt;
        }
        const kotor::Gff utc = kotor::Gff::parse(std::move(*data), resref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        const auto number = [&](std::uint32_t structure, const char* label, std::int32_t fallback) {
            const kotor::GffField* field = utc.field(structure, label);
            return field != nullptr ? utc.integer(*field) : fallback;
        };
        UtcFacts facts;
        facts.race = number(root, "Race", 0);
        facts.subRace = number(root, "SubraceIndex", 0);
        facts.gender = number(root, "Gender", 0);
        facts.appearance = number(root, "Appearance_Type", 0);
        facts.hp = number(root, "HitPoints", 1);
        facts.maxHp = number(root, "MaxHitPoints", facts.hp);
        if (facts.maxHp <= 0) {
            facts.maxHp = facts.hp;
        }
        if (const kotor::GffField* field = utc.field(root, "ChallengeRating"); field != nullptr) {
            facts.challengeRating = utc.number(*field);
        }
        constexpr const char* kAbilities[6] = {"Str", "Dex", "Con", "Int", "Wis", "Cha"};
        for (std::size_t i = 0; i < 6; ++i) {
            facts.abilities[i] = number(root, kAbilities[i], 10);
        }
        if (const kotor::GffField* list = utc.field(root, "ClassList"); list != nullptr) {
            for (const std::uint32_t entry : utc.list(*list)) {
                const std::int32_t classId = number(entry, "Class", 0);
                const std::int32_t level = number(entry, "ClassLevel", 0);
                if (facts.firstClass < 0) {
                    facts.firstClass = classId;
                    facts.firstLevel = level;
                }
                facts.levels += level;
            }
        }
        if (const kotor::GffField* list = utc.field(root, "SkillList"); list != nullptr) {
            for (const std::uint32_t entry : utc.list(*list)) {
                facts.skills.push_back(number(entry, "Rank", 0));
            }
        }
        return facts;
    } catch (const kotor::FormatError&) {
        return std::nullopt;
    }
}

std::string hexOf(std::uint32_t id)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%x", static_cast<unsigned>(id));
    return buffer;
}

} // namespace

int runObjectsFamilyTest(GameSession& game)
{
    using Value = script::Value;
    using Args = std::vector<std::vector<Value>>;
    bool ok = true;
    const auto check = [&ok](bool condition, const char* label) {
        logInfo(concat("objects: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    ScriptApi api{game};
    script::Vm dummy({});
    const std::uint32_t pc = game.scriptWorld().pc;
    const std::uint32_t invalid = script::kObjectInvalid;

    const auto call = [&](std::uint16_t id, Args args) {
        const auto result = game.action(id, args, game.scriptWorld(), dummy);
        return result.value_or(std::vector<Value>{});
    };
    const auto obj = [](std::uint32_t id) { return std::vector<Value>{Value::makeObject(id)}; };
    const auto num = [](std::int64_t value) { return std::vector<Value>{Value::makeInt(value)}; };
    const auto real = [](double value) { return std::vector<Value>{Value::makeFloat(value)}; };
    const auto text = [](const std::string& value) {
        return std::vector<Value>{Value::makeString(value)};
    };
    const auto intOf = [&](std::uint16_t id, Args args) {
        const std::vector<Value> out = call(id, std::move(args));
        return out.empty() ? std::int64_t{-777777} : out.front().asInt();
    };
    const auto objOf = [&](std::uint16_t id, Args args) {
        const std::vector<Value> out = call(id, std::move(args));
        return out.empty() ? std::uint32_t{0} : static_cast<std::uint32_t>(out.front().integer);
    };
    const auto floatOf = [&](std::uint16_t id, Args args) {
        const std::vector<Value> out = call(id, std::move(args));
        return out.empty() ? -777777.0 : out.front().asFloat();
    };
    const auto stringOf = [&](std::uint16_t id, Args args) {
        const std::vector<Value> out = call(id, std::move(args));
        return out.empty() ? std::string{"<none>"} : out.front().asString();
    };

    // Runs a script with the given self and reads one result back from a global.
    const auto vmInt = [&](std::uint32_t self, const std::function<void(MiniNcs&)>& build) {
        MiniNcs script;
        build(script);
        script.constString("objects_out_i").action(581, 2); // SetGlobalNumber
        game.scriptWorld().setGlobal("objects_out_i", Value::makeInt(-999999));
        game.runScriptNcsForTest("objects_gate_int", script.bytes(), self);
        return game.scriptWorld().getGlobal("objects_out_i", script::ValueType::Int).asInt();
    };
    const auto vmString = [&](std::uint32_t self, const std::function<void(MiniNcs&)>& build) {
        MiniNcs script;
        build(script);
        script.constString("objects_out_s").action(160, 2); // SetGlobalString
        game.scriptWorld().setGlobal("objects_out_s", Value::makeString("<unset>"));
        game.runScriptNcsForTest("objects_gate_string", script.bytes(), self);
        return game.scriptWorld().getGlobal("objects_out_s", script::ValueType::String).asString();
    };
    // A float result goes through FloatToString(value, width 0, 6 decimals).
    const auto vmFloat = [&](std::uint32_t self, const std::function<void(MiniNcs&)>& build) {
        const std::string out = vmString(self, [&](MiniNcs& script) {
            script.constInt(6).constInt(0);
            build(script);
            script.action(3, 3);
        });
        return out == "<unset>" ? -888888.0 : std::atof(out.c_str());
    };

    const std::vector<kotor::Entity>& entities = game.scene().entities;
    const auto findCreature = [&](const std::string& name) {
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (entities[i].kind == kotor::EntityKind::Creature
                && (lowerText(entities[i].templateResref) == name || lowerText(entities[i].tag) == name)) {
                return i;
            }
        }
        return kNone;
    };
    const std::size_t indexA = findCreature("c_drdastro001");
    const std::size_t indexB = findCreature("end_bridgerep1");
    check(indexA != kNone && indexB != kNone, "fixture_creatures_found");
    if (indexA == kNone || indexB == kNone) {
        return 40;
    }
    const std::uint32_t objectA = game.objectForEntityIndex(indexA);
    const std::uint32_t objectB = game.objectForEntityIndex(indexB);
    const std::optional<UtcFacts> utcA = readUtcFacts(api, entities[indexA].templateResref);
    const std::optional<UtcFacts> utcB = readUtcFacts(api, entities[indexB].templateResref);
    check(utcA.has_value() && utcB.has_value(), "fixture_utc_read");
    if (!utcA.has_value() || !utcB.has_value()) {
        return 41;
    }

    // ---- per-creature numbers against the UTC ----
    const auto creatureChecks = [&](const char* name, std::uint32_t object, const UtcFacts& utc) {
        const std::string label = name;
        check(intOf(107, {obj(object)}) == utc.race, (label + "_race").c_str());
        check(intOf(497, {obj(object)}) == utc.subRace, (label + "_subrace").c_str());
        check(intOf(358, {obj(object)}) == utc.gender, (label + "_gender").c_str());
        check(intOf(524, {obj(object)}) == utc.appearance, (label + "_appearance").c_str());
        check(intOf(49, {obj(object)}) == utc.hp, (label + "_current_hp").c_str());
        check(intOf(50, {obj(object)}) == utc.maxHp, (label + "_max_hp").c_str());
        check(intOf(166, {obj(object)}) == utc.levels, (label + "_hit_dice").c_str());
        check(intOf(341, {num(1), obj(object)}) == utc.firstClass, (label + "_class_by_position").c_str());
        check(intOf(342, {num(1), obj(object)}) == utc.firstLevel, (label + "_level_by_position").c_str());
        check(intOf(343, {num(utc.firstClass), obj(object)}) == utc.firstLevel,
              (label + "_level_by_class").c_str());
        check(intOf(343, {num(31), obj(object)}) == 0, (label + "_level_by_other_class").c_str());
        check(intOf(341, {num(5), obj(object)}) == 255, (label + "_class_past_end").c_str());
        bool abilities = true;
        for (std::int64_t i = 0; i < 6; ++i) {
            abilities = abilities
                && intOf(139, {obj(object), num(i)}) == utc.abilities[static_cast<std::size_t>(i)]
                && intOf(331, {num(i), obj(object)})
                    == abilityModifier(utc.abilities[static_cast<std::size_t>(i)]);
        }
        check(abilities, (label + "_abilities").c_str());
        check(std::fabs(floatOf(494, {obj(object)}) - static_cast<double>(utc.challengeRating)) < 1e-4,
              (label + "_challenge_rating").c_str());
    };
    creatureChecks("droid", objectA, *utcA);
    creatureChecks("human", objectB, *utcB);
    check(utcA->hp != utcB->hp && utcA->levels != utcB->levels && utcA->gender != utcB->gender,
          "fixtures_differ_in_hp_level_gender");
    check(intOf(49, {obj(objectA)}) != intOf(49, {obj(objectB)})
              && intOf(166, {obj(objectA)}) != intOf(166, {obj(objectB)})
              && intOf(358, {obj(objectA)}) != intOf(358, {obj(objectB)}),
          "handlers_differ_between_creatures");
    // The legacy switch answered for the player; each creature now answers for itself.
    check(intOf(166, {obj(pc)}) == game.character().level
              && intOf(341, {num(1), obj(pc)}) == game.character().classId,
          "player_level_and_class_from_sheet");
    check(intOf(107, {obj(pc)}) == kRaceHuman, "player_is_human");
    check(intOf(217, {obj(pc)}) == 1 && intOf(217, {obj(objectA)}) == 0, "get_is_pc");

    // Skills: rank plus the key-ability modifier, -1 for a non-creature, 0 past the table.
    {
        const kotor::TwoDa* skills = skillsTable(api);
        bool skillsOk = skills != nullptr;
        std::size_t trained = 0;
        for (std::size_t skill = 0; skills != nullptr && skill < skills->rowCount()
                                    && skill < utcB->skills.size();
             ++skill) {
            const std::int32_t rank = utcB->skills[skill];
            const SkillInfo info = skillInfo(api, static_cast<std::int64_t>(skill));
            const std::int64_t got = intOf(315, {num(static_cast<std::int64_t>(skill)), obj(objectB)});
            std::int64_t want = 0;
            if (rank > 0 || info.untrained) {
                want = rank
                    + (info.ability >= 0
                           ? abilityModifier(utcB->abilities[static_cast<std::size_t>(info.ability)])
                           : 0);
            }
            skillsOk = skillsOk && got == want;
            trained += rank > 0 ? 1u : 0u;
            skillsOk = skillsOk
                && intOf(286, {num(static_cast<std::int64_t>(skill)), obj(objectB)}) == (rank > 0 ? 1 : 0);
        }
        check(skillsOk, "skill_rank_matches_utc");
        logInfo(concat("objects: trained_skills_checked=", trained));
        check(intOf(315, {num(99), obj(objectB)}) == 0, "skill_past_table_is_zero");
        std::size_t doorIndex = kNone;
        for (std::size_t i = 0; i < entities.size() && doorIndex == kNone; ++i) {
            doorIndex = entities[i].kind == kotor::EntityKind::Door ? i : kNone;
        }
        if (doorIndex != kNone) {
            check(intOf(315, {num(0), obj(game.objectForEntityIndex(doorIndex))}) == -1,
                  "skill_rank_of_door_is_minus_one");
        }
    }

    // Names and types.
    check(stringOf(253, {obj(pc)}) == game.character().name, "name_of_player");
    check((!entities[indexA].name.empty() || !entities[indexB].name.empty())
              && stringOf(253, {obj(objectA)}).rfind(entities[indexA].name, 0) == 0
              && stringOf(253, {obj(objectB)}).rfind(entities[indexB].name, 0) == 0,
          "name_of_creature_starts_with_first_name");
    check(intOf(106, {obj(objectA)}) == 1 && intOf(106, {obj(pc)}) == 1 && intOf(106, {obj(invalid)}) == 0
              && intOf(106, {obj(game.areaObject())}) == 0,
          "object_type_creature_invalid_area");
    {
        bool typesOk = true;
        std::size_t kindsSeen = 0;
        bool seen[5] = {false, false, false, false, false};
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const kotor::EntityKind kind = entities[i].kind;
            const std::int64_t want = kind == kotor::EntityKind::Door ? 8
                : kind == kotor::EntityKind::Placeable                 ? 64
                : kind == kotor::EntityKind::Waypoint                  ? 32
                : kind == kotor::EntityKind::Trigger                   ? 4
                : kind == kotor::EntityKind::Creature                  ? 1
                                                                        : -1;
            const std::size_t slot = want == 8 ? 0u : want == 64 ? 1u : want == 32 ? 2u : want == 4 ? 3u : 4u;
            if (want < 0 || i == game.playerEntity()) {
                continue;
            }
            typesOk = typesOk && intOf(106, {obj(game.objectForEntityIndex(i))}) == want;
            if (!seen[slot]) {
                seen[slot] = true;
                ++kindsSeen;
            }
        }
        check(typesOk && kindsSeen >= 3, "object_type_per_entity_kind");
    }

    // ---- GetStringByStrRef against the TLK ----
    {
        const kotor::Tlk& tlk = game.localization().tlk();
        bool same = true;
        std::size_t nonEmpty = 0;
        std::uint32_t firstNonEmpty = 0;
        for (const std::uint32_t strref : {0u, 1u, 2u, 100u, 5000u, 33000u, 40000u, 4000000000u}) {
            const std::string expected = tlk.text(strref);
            // 4000000000 does not fit a script int; it arrives as a negative number.
            const std::int64_t asScript = static_cast<std::int32_t>(strref);
            const std::string got = stringOf(239, {num(asScript)});
            same = same && (strref > 0x7FFFFFFFu ? got.empty() : got == expected);
            if (!expected.empty() && strref <= 0x7FFFFFFFu) {
                ++nonEmpty;
                firstNonEmpty = firstNonEmpty == 0 ? strref : firstNonEmpty;
            }
        }
        check(same && nonEmpty >= 2, "strref_text_matches_tlk");
        check(stringOf(239, {num(-1)}).empty(), "strref_minus_one_is_empty");
        const std::string viaVm = vmString(pc, [&](MiniNcs& script) {
            script.constInt(static_cast<std::int32_t>(firstNonEmpty)).action(239, 1);
        });
        check(!viaVm.empty() && viaVm == tlk.text(firstNonEmpty), "strref_text_through_the_vm");
        logInfo(concat("objects: strref ", firstNonEmpty, " = ", viaVm.substr(0, 40)));
    }

    // ---- area iteration ----
    {
        std::vector<std::uint32_t> expectedCreatures{pc};
        std::vector<std::uint32_t> expectedDoors;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (i == game.playerEntity()) {
                continue;
            }
            if (entities[i].kind == kotor::EntityKind::Creature) {
                expectedCreatures.push_back(game.objectForEntityIndex(i));
            } else if (entities[i].kind == kotor::EntityKind::Door) {
                expectedDoors.push_back(game.objectForEntityIndex(i));
            }
        }
        const auto iterate = [&](std::int64_t mask) {
            std::vector<std::uint32_t> visited;
            std::uint32_t current = objOf(93, {obj(invalid), num(mask)});
            for (int guard = 0; current != invalid && guard < 20000; ++guard) {
                visited.push_back(current);
                current = objOf(94, {obj(invalid), num(mask)});
            }
            return visited;
        };
        const auto sameSet = [](std::vector<std::uint32_t> a, std::vector<std::uint32_t> b) {
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            return a == b;
        };
        const auto distinct = [](std::vector<std::uint32_t> v) {
            std::sort(v.begin(), v.end());
            return std::adjacent_find(v.begin(), v.end()) == v.end();
        };
        const std::vector<std::uint32_t> creatures = iterate(1);
        logInfo(concat("objects: area creatures visited=", creatures.size(),
                       " expected=", expectedCreatures.size()));
        check(distinct(creatures) && sameSet(creatures, expectedCreatures),
              "area_iteration_visits_each_creature_once");
        const std::vector<std::uint32_t> doors = iterate(8);
        check(!doors.empty() && distinct(doors) && sameSet(doors, expectedDoors),
              "area_iteration_visits_each_door_once");
        const std::vector<std::uint32_t> everything = iterate(32767);
        check(distinct(everything) && everything.size() > creatures.size() + doors.size(),
              "area_iteration_all_types_distinct");
        // Nested iteration: the area has one cursor, so an inner loop that runs to
        // the end leaves the outer loop with nothing more, as in the original.
        const std::uint32_t outerFirst = objOf(93, {obj(invalid), num(1)});
        const std::vector<std::uint32_t> inner = iterate(1);
        const std::uint32_t outerNext = objOf(94, {obj(invalid), num(1)});
        check(outerFirst == creatures.front() && sameSet(inner, expectedCreatures)
                  && outerNext == invalid,
              "nested_area_iteration_clobbers_outer_cursor");
        // A partial inner loop moves the outer cursor on by exactly what it consumed.
        objOf(93, {obj(invalid), num(1)});
        objOf(94, {obj(invalid), num(1)});
        const std::uint32_t third = objOf(94, {obj(invalid), num(1)});
        check(creatures.size() >= 3 && third == creatures[2], "area_iteration_cursor_advances");
        check(objOf(93, {obj(game.moduleObject()), num(1)}) == invalid, "area_iteration_of_module_is_invalid");
    }

    // ---- nearest by tag, with the Nth parameter ----
    const kotor::Vec3 origin = game.actorPosition(pc);
    const std::string cloneResref = entities[indexA].templateResref;
    const std::string cloneTag = entities[indexA].tag;
    std::vector<std::uint32_t> clones;
    for (const float offset : {3.0f, 7.0f, 11.0f}) {
        clones.push_back(api.spawnCreature(cloneResref, {origin.x + offset, origin.y, origin.z}, 1.5f));
    }
    check(std::none_of(clones.begin(), clones.end(), [&](std::uint32_t id) { return id == invalid; }),
          "fixture_clones_spawned");
    const auto taggedByDistance = [&](kotor::Vec3 from, std::uint32_t exclude) {
        std::vector<std::pair<float, std::uint32_t>> found;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (i == game.playerEntity() || lowerText(entities[i].tag) != lowerText(cloneTag)) {
                continue;
            }
            const std::uint32_t id = game.objectForEntityIndex(i);
            if (id == exclude) {
                continue;
            }
            const kotor::Vec3 at = entities[i].position;
            found.emplace_back((at.x - from.x) * (at.x - from.x) + (at.y - from.y) * (at.y - from.y)
                                   + (at.z - from.z) * (at.z - from.z),
                               id);
        }
        std::stable_sort(found.begin(), found.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::uint32_t> ids;
        for (const auto& entry : found) {
            ids.push_back(entry.second);
        }
        return ids;
    };
    {
        const std::vector<std::uint32_t> fromPlayer = taggedByDistance(origin, invalid);
        check(fromPlayer.size() >= 4, "several_instances_share_the_tag");
        bool nth = !fromPlayer.empty();
        for (std::size_t n = 1; n <= fromPlayer.size(); ++n) {
            nth = nth && objOf(229, {text(cloneTag), obj(pc), num(static_cast<std::int64_t>(n))})
                    == fromPlayer[n - 1];
        }
        check(nth, "nearest_by_tag_orders_all_instances_by_distance");
        check(objOf(229, {text(cloneTag), obj(pc), num(static_cast<std::int64_t>(fromPlayer.size() + 1))})
                  == invalid
                  && objOf(229, {text(cloneTag), obj(pc), num(0)}) == invalid,
              "nearest_by_tag_past_the_last_is_invalid");
        check(objOf(229, {text("no_such_tag_here"), obj(pc), num(1)}) == invalid, "nearest_by_unknown_tag");
        check(objOf(229, {text(cloneTag), obj(invalid), num(1)}) == invalid, "nearest_by_tag_from_invalid");
        // From another object the origin itself is skipped and distances change.
        const std::vector<std::uint32_t> fromClone = taggedByDistance(game.actorPosition(clones[2]), clones[2]);
        check(!fromClone.empty() && objOf(229, {text(cloneTag), obj(clones[2]), num(1)}) == fromClone[0]
                  && objOf(229, {text(cloneTag), obj(clones[2]), num(2)}) == fromClone[1],
              "nearest_by_tag_from_another_object_skips_itself");
        check(objOf(229, {text(cloneTag), obj(pc), num(2)}) == fromPlayer[1]
                  && fromPlayer[1] != fromPlayer[0] && fromPlayer[0] != fromClone[0],
              "nearest_by_tag_results_differ_by_nth_and_origin");
        const std::string viaVmText = vmString(pc, [&](MiniNcs& script) {
            script.constInt(2).constObject(0).constString(cloneTag).action(229, 3).action(272, 1); // 0 is OBJECT_SELF
        });
        logInfo(concat("objects: nearest_by_tag vm text=", viaVmText, " expected=", hexOf(fromPlayer[1])));
        const std::uint32_t viaVm = static_cast<std::uint32_t>(std::strtoul(viaVmText.c_str(), nullptr, 16));
        check(viaVm == fromPlayer[1], "nearest_by_tag_nth_through_the_vm");
    }

    // ---- other nearest searches ----
    {
        std::vector<std::pair<float, std::uint32_t>> doors;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (entities[i].kind == kotor::EntityKind::Door) {
                const kotor::Vec3 at = entities[i].position;
                doors.emplace_back((at.x - origin.x) * (at.x - origin.x)
                                       + (at.y - origin.y) * (at.y - origin.y)
                                       + (at.z - origin.z) * (at.z - origin.z),
                                   game.objectForEntityIndex(i));
            }
        }
        std::stable_sort(doors.begin(), doors.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        check(doors.size() >= 2 && objOf(227, {num(8), obj(pc), num(1)}) == doors[0].second
                  && objOf(227, {num(8), obj(pc), num(2)}) == doors[1].second,
              "nearest_object_by_type_mask");
        const std::vector<Value> here = call(215, {{Value::makeFloat(origin.x + 7.0),
                                                    Value::makeFloat(origin.y),
                                                    Value::makeFloat(origin.z)},
                                                   real(0.0)});
        const std::int64_t location = here.empty() ? 0 : here.front().integer;
        check(objOf(228, {num(1), {Value::makeEngine(location)}, num(1)}) == clones[1],
              "nearest_object_to_location");

        // GetNearestCreature criteria, against a walk of the scene.
        struct Candidate {
            float squared;
            std::uint32_t id;
        };
        const auto creaturesBy = [&](kotor::Vec3 from, const std::function<bool(const kotor::Entity&)>& keep) {
            std::vector<Candidate> found;
            for (std::size_t i = 0; i < entities.size(); ++i) {
                const kotor::Entity& entity = entities[i];
                if (i == game.playerEntity() || entity.kind != kotor::EntityKind::Creature || entity.dead
                    || !keep(entity)) {
                    continue;
                }
                found.push_back({(entity.position.x - from.x) * (entity.position.x - from.x)
                                     + (entity.position.y - from.y) * (entity.position.y - from.y)
                                     + (entity.position.z - from.z) * (entity.position.z - from.z),
                                 game.objectForEntityIndex(i)});
            }
            std::stable_sort(found.begin(), found.end(),
                             [](const Candidate& a, const Candidate& b) { return a.squared < b.squared; });
            return found;
        };
        const auto droids = creaturesBy(origin, [](const kotor::Entity& e) { return e.stats.race == 5; });
        check(droids.size() >= 2 && objOf(38, {num(0), num(5), obj(pc), num(1)}) == droids[0].id
                  && objOf(38, {num(0), num(5), obj(pc), num(2)}) == droids[1].id,
              "nearest_creature_by_race");
        const auto notPlayer = creaturesBy(origin, [](const kotor::Entity&) { return true; });
        check(objOf(38, {num(1), num(0), obj(pc), num(3)}) == notPlayer[2].id
                  && objOf(38, {num(1), num(1), obj(objectA), num(1)}) == pc,
              "nearest_creature_by_player_flag");
        const auto classBy = creaturesBy(origin, [&](const kotor::Entity& e) {
            return !e.stats.classes.empty() && e.stats.classes.front().first == utcA->firstClass;
        });
        check(!classBy.empty() && objOf(38, {num(2), num(utcA->firstClass), obj(pc), num(1)}) == classBy[0].id,
              "nearest_creature_by_class");
        const auto both = creaturesBy(origin, [&](const kotor::Entity& e) {
            return e.stats.race == 5 && !e.stats.classes.empty()
                && e.stats.classes.front().first == utcA->firstClass;
        });
        check(!both.empty()
                  && objOf(38, {num(0), num(5), obj(pc), num(1), num(2), num(utcA->firstClass)}) == both[0].id
                  && objOf(38, {num(0), num(6), obj(pc), num(1), num(2), num(utcA->firstClass)}) == invalid,
              "nearest_creature_with_two_criteria");
        check(objOf(38, {num(0), num(7), obj(pc), num(1)}) == notPlayer[0].id,
              "nearest_creature_racial_all_matches");
        check(objOf(38, {num(0), num(8), obj(pc), num(1)}) == invalid
                  && objOf(38, {num(0), num(5), obj(invalid), num(1)}) == invalid,
              "nearest_creature_invalid_race_and_origin");
        const auto hereCreature = creaturesBy(origin, [](const kotor::Entity&) { return true; });
        check(objOf(226, {num(1), num(0), {Value::makeEngine(location)}, num(1)})
                  == creaturesBy(kotor::Vec3{origin.x + 7.0f, origin.y, origin.z},
                                 [](const kotor::Entity&) { return true; })
                         .front()
                         .id
                  && !hereCreature.empty(),
              "nearest_creature_to_location");
    }

    // ---- distances, facing, identity ----
    // In compiled scripts CONSTO 0 is OBJECT_SELF and 1 is OBJECT_INVALID, so the
    // scripts below name other objects by their ids and run with a clone as self.
    {
        const kotor::Vec3 at = game.actorPosition(clones[0]);
        const kotor::Vec3 other = game.actorPosition(clones[1]);
        const double flat = std::hypot(static_cast<double>(at.x - origin.x), static_cast<double>(at.y - origin.y));
        const double full = std::sqrt(flat * flat + static_cast<double>((at.z - origin.z) * (at.z - origin.z)));
        const double flatBetween = std::hypot(static_cast<double>(other.x - at.x), static_cast<double>(other.y - at.y));
        const double fullBetween = std::sqrt(flatBetween * flatBetween
                                             + static_cast<double>((other.z - at.z) * (other.z - at.z)));
        const double vm2d = vmFloat(clones[0], [&](MiniNcs& script) { script.constObject(clones[1]).action(335, 1); });
        const double vm3d = vmFloat(clones[0], [&](MiniNcs& script) { script.constObject(clones[1]).action(41, 1); });
        const double vmOther = vmFloat(clones[2], [&](MiniNcs& script) { script.constObject(clones[1]).action(335, 1); });
        logInfo(concat("objects: distance vm 2d=", vm2d, " 3d=", vm3d, " from third=", vmOther,
                       " expected flat=", flatBetween, " full=", fullBetween));
        check(std::fabs(vm2d - flatBetween) < 1e-3, "distance_2d_from_self_through_the_vm");
        check(std::fabs(vm3d - fullBetween) < 1e-3, "distance_3d_from_self_through_the_vm");
        check(std::fabs(vmOther - 4.0) < 0.05 && std::fabs(vm2d - 4.0) < 0.05,
              "distance_follows_the_running_object");
        check(vmFloat(pc, [&](MiniNcs& script) { script.constObject(1).action(335, 1); }) == -1.0
                  && vmFloat(pc, [&](MiniNcs& script) { script.constObject(game.areaObject()).action(41, 1); }) == -1.0,
              "distance_to_invalid_or_area_is_minus_one");
        check(std::fabs(floatOf(151, {obj(pc), obj(clones[0])}) - full) < 1e-3
                  && std::fabs(floatOf(319, {obj(pc), obj(clones[0])}) - flat) < 1e-3
                  && floatOf(151, {obj(pc), obj(invalid)}) == 0.0,
              "distance_between_objects");
        check(std::fabs(floatOf(28, {obj(clones[0])}) - 1.5 * 180.0 / 3.14159265358979) < 0.01
                  && floatOf(28, {obj(invalid)}) == -1.0,
              "facing_in_degrees");
        check(stringOf(272, {obj(0x1abc)}) == "1abc" && stringOf(272, {obj(invalid)}) == "7f000000",
              "object_to_string_is_lower_hex");
        check(stringOf(210, {}) == game.module(), "module_file_name");
        check(objOf(242, {}) == game.moduleObject() && intOf(251, {}) == 0, "module_object_and_load_flag");
        check(intOf(761, {}) == 1, "ship_build_returns_one");
        const std::size_t before = game.scriptLog().size();
        call(1, {text("objects gate print")});
        call(582, {text("objects gate post"), num(1), num(2), real(3.0)});
        call(4, {num(42)});
        bool printed = false;
        bool posted = false;
        bool counted = false;
        for (std::size_t i = before; i < game.scriptLog().size(); ++i) {
            printed = printed || game.scriptLog()[i] == "PrintString: objects gate print";
            posted = posted || game.scriptLog()[i] == "AurPostString: objects gate post";
            counted = counted || game.scriptLog()[i] == "PrintInteger: 42";
        }
        check(printed && posted && counted, "print_actions_log");
        check(intOf(445, {obj(pc)}) == 0, "not_in_conversation_outside_dialogue");
    }

    // ---- shapes ----
    {
        const kotor::Vec3 at = game.actorPosition(clones[1]);
        const std::vector<Value> spot = call(215, {{Value::makeFloat(at.x), Value::makeFloat(at.y),
                                                    Value::makeFloat(at.z)},
                                                   real(0.0)});
        const std::int64_t location = spot.empty() ? 0 : spot.front().integer;
        const auto shapeVisit = [&](std::int64_t shape, double size) {
            std::vector<std::uint32_t> visited;
            std::uint32_t current = objOf(128, {num(shape), real(size), {Value::makeEngine(location)}, num(0), num(1)});
            for (int guard = 0; current != invalid && guard < 1000; ++guard) {
                visited.push_back(current);
                current = objOf(129, {num(shape), real(size), {Value::makeEngine(location)}, num(0), num(1)});
            }
            return visited;
        };
        const std::vector<std::uint32_t> sphere = shapeVisit(4, 0.5);
        const std::vector<std::uint32_t> cube = shapeVisit(2, 0.5);
        logInfo(concat("objects: shape sphere=", sphere.size(), " cube=", cube.size(), " clone1=", hexOf(clones[1]),
                       " first=", sphere.empty() ? std::string{"-"} : hexOf(sphere[0]), " location=", location,
                       " cube ids=", cube.empty() ? std::string{"-"} : hexOf(cube[0]), ",",
                       cube.size() > 1 ? hexOf(cube[1]) : std::string{"-"}));
        // Expected sets from a walk of the scene: creatures within 0.5 m (sphere) or
        // within 0.5 m on every axis (cube) of the clone.
        std::vector<std::uint32_t> wantSphere;
        std::vector<std::uint32_t> wantCube;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (i == game.playerEntity() || entities[i].kind != kotor::EntityKind::Creature) {
                continue;
            }
            const kotor::Vec3 p = entities[i].position;
            const double dx = static_cast<double>(p.x - at.x);
            const double dy = static_cast<double>(p.y - at.y);
            const double dz = static_cast<double>(p.z - at.z);
            const std::uint32_t id = game.objectForEntityIndex(i);
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 0.5) {
                wantSphere.push_back(id);
            }
            if (std::fabs(dx) <= 0.5 && std::fabs(dy) <= 0.5 && std::fabs(dz) <= 0.5) {
                wantCube.push_back(id);
            }
        }
        std::sort(wantSphere.begin(), wantSphere.end());
        std::sort(wantCube.begin(), wantCube.end());
        std::vector<std::uint32_t> gotSphere = sphere;
        std::vector<std::uint32_t> gotCube = cube;
        std::sort(gotSphere.begin(), gotSphere.end());
        std::sort(gotCube.begin(), gotCube.end());
        check(gotSphere == wantSphere && gotCube == wantCube
                  && std::find(sphere.begin(), sphere.end(), clones[1]) != sphere.end()
                  && std::find(cube.begin(), cube.end(), clones[1]) != cube.end(),
              "shape_sphere_and_cube_match_the_scene");
        const std::vector<std::uint32_t> wide = shapeVisit(4, 5000.0);
        std::vector<std::uint32_t> sorted = wide;
        std::sort(sorted.begin(), sorted.end());
        check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end() && wide.size() > 5
                  && std::find(wide.begin(), wide.end(), pc) != wide.end(),
              "shape_wide_sphere_visits_each_creature_once");
    }

    // ---- persistent objects: a trigger's occupants ----
    {
        const kotor::Scene& scene = game.scene();
        std::size_t triggerEntity = kNone;
        kotor::Vec3 inside{};
        for (const kotor::TriggerVolume& volume : scene.triggers) {
            if (volume.indices.size() >= 3 && volume.indices[0] < volume.vertices.size()
                && volume.indices[1] < volume.vertices.size() && volume.indices[2] < volume.vertices.size()) {
                const kotor::Vec3 a = volume.vertices[volume.indices[0]];
                const kotor::Vec3 b = volume.vertices[volume.indices[1]];
                const kotor::Vec3 c = volume.vertices[volume.indices[2]];
                inside = {(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
                triggerEntity = volume.entity;
                break;
            }
        }
        check(triggerEntity != kNone, "fixture_trigger_found");
        if (triggerEntity != kNone) {
            const std::uint32_t trigger = game.objectForEntityIndex(triggerEntity);
            game.setPositionForTest(inside);
            std::vector<std::uint32_t> residents;
            std::uint32_t current = objOf(262, {obj(trigger), num(1), num(0)});
            for (int guard = 0; current != invalid && guard < 1000; ++guard) {
                residents.push_back(current);
                current = objOf(263, {obj(trigger), num(1), num(0)});
            }
            std::vector<std::uint32_t> sorted = residents;
            std::sort(sorted.begin(), sorted.end());
            check(std::find(residents.begin(), residents.end(), pc) != residents.end()
                      && std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
                  "persistent_object_lists_occupants_once");
            game.setPositionForTest({inside.x + 100000.0f, inside.y, inside.z});
            std::vector<std::uint32_t> awayResidents;
            std::uint32_t awayCurrent = objOf(262, {obj(trigger), num(1), num(0)});
            for (int guard = 0; awayCurrent != invalid && guard < 1000; ++guard) {
                awayResidents.push_back(awayCurrent);
                awayCurrent = objOf(263, {obj(trigger), num(1), num(0)});
            }
            check(std::find(awayResidents.begin(), awayResidents.end(), pc) == awayResidents.end(),
                  "persistent_object_drops_the_player_when_away");
            game.setPositionForTest(origin);
        }
    }

    // ---- the clock ----
    {
        const ModuleClock ifo = readModuleClock(api);
        const std::int32_t perHour = ifo.minPerHour > 0 ? ifo.minPerHour : 2;
        logInfo(concat("objects: ifo dawn=", ifo.dawn, " dusk=", ifo.dusk, " min_per_hour=", ifo.minPerHour));
        const auto hour = [&] { return vmInt(pc, [](MiniNcs& s) { s.action(16, 0); }); };
        const auto minute = [&] { return vmInt(pc, [](MiniNcs& s) { s.action(17, 0); }); };
        const auto second = [&] { return vmInt(pc, [](MiniNcs& s) { s.action(18, 0); }); };
        const auto milli = [&] { return vmInt(pc, [](MiniNcs& s) { s.action(19, 0); }); };
        const auto setTimeVm = [&](int h, int m, int s, int ms) {
            MiniNcs script;
            script.constInt(ms).constInt(s).constInt(m).constInt(h).action(12, 4);
            game.runScriptNcsForTest("objects_gate_settime", script.bytes(), pc);
        };
        const auto totalMs = [&] {
            return ((hour() * perHour + minute()) * 60000) + second() * 1000 + milli();
        };
        setTimeVm(5, 0, 0, 0);
        check(hour() == 5 && minute() == 0 && second() == 0 && milli() == 0, "set_time_through_the_vm");
        game.scriptWorld().time += 61.5;
        const std::int64_t want = static_cast<std::int64_t>(5) * perHour * 60000 + 61500;
        check(hour() == want / 60000 / perHour && minute() == (want / 60000) % perHour
                  && second() == (want / 1000) % 60 && milli() == want % 1000,
              "time_getters_follow_world_time");
        // A real update advances the clock by what the world advanced.
        const std::int64_t before = totalMs();
        const double worldBefore = game.scriptWorld().time;
        for (int i = 0; i < 20; ++i) {
            game.update(0.05f, InputState{});
        }
        const std::int64_t advanced = totalMs() - before;
        const double worldAdvanced = (game.scriptWorld().time - worldBefore) * 1000.0;
        logInfo(concat("objects: clock advanced ", advanced, " ms for ", worldAdvanced, " ms of world time"));
        check(worldAdvanced > 0.0 && std::fabs(static_cast<double>(advanced) - worldAdvanced) < 3.0,
              "clock_advances_with_update");
        // The hour carries over when the minutes run out.
        setTimeVm(7, 0, 0, 0);
        game.scriptWorld().time += perHour * 60.0;
        check(hour() == 8 && minute() == 0, "hour_carries_after_min_per_hour_minutes");
        setTimeVm(23, 0, 0, 0);
        game.scriptWorld().time += perHour * 60.0;
        check(hour() == 0, "day_wraps_at_midnight");
        setTimeVm(30, 0, 0, 0);
        check(hour() == 6 && minute() == 0, "set_time_hour_wraps_mod_24");
        setTimeVm(1, 0, 0, 0);
        const std::int64_t refused = totalMs();
        MiniNcs negative;
        negative.constInt(0).constInt(0).constInt(0).constInt(-1).action(12, 4);
        game.runScriptNcsForTest("objects_gate_settime_negative", negative.bytes(), pc);
        check(totalMs() == refused, "set_time_refuses_negative_arguments");

        // Day, night, dawn and dusk from the module's own hours.
        const auto state = [&] {
            const std::int64_t day = vmInt(pc, [](MiniNcs& s) { s.action(405, 0); });
            const std::int64_t night = vmInt(pc, [](MiniNcs& s) { s.action(406, 0); });
            const std::int64_t dawn = vmInt(pc, [](MiniNcs& s) { s.action(407, 0); });
            const std::int64_t dusk = vmInt(pc, [](MiniNcs& s) { s.action(408, 0); });
            return std::array<std::int64_t, 4>{day, night, dawn, dusk};
        };
        if (ifo.dawn != ifo.dusk && ifo.dawn < ifo.dusk && ifo.dawn > 0 && ifo.dusk < 23) {
            setTimeVm(ifo.dawn, 0, 0, 0);
            check(state() == std::array<std::int64_t, 4>{0, 0, 1, 0}, "dawn_hour_is_dawn");
            setTimeVm(ifo.dusk, 0, 0, 0);
            check(state() == std::array<std::int64_t, 4>{0, 0, 0, 1}, "dusk_hour_is_dusk");
            setTimeVm((ifo.dawn + ifo.dusk) / 2, 0, 0, 0);
            check(state() == std::array<std::int64_t, 4>{1, 0, 0, 0}, "midday_is_day");
            setTimeVm(ifo.dusk + 1, 0, 0, 0);
            check(state() == std::array<std::int64_t, 4>{0, 1, 0, 0}, "after_dusk_is_night");
            setTimeVm(0, 0, 0, 0);
            check(state() == std::array<std::int64_t, 4>{0, 1, 0, 0}, "midnight_is_night");
        } else {
            logInfo("objects: module dawn and dusk hours not usable for the day and night checks");
            check(false, "module_has_usable_dawn_and_dusk");
        }
    }

    // ---- perception criteria read the combat family's perception lists ----
    {
        std::uint32_t observer = invalid;
        for (std::size_t i = 0; i < entities.size() && observer == invalid; ++i) {
            if (entities[i].kind != kotor::EntityKind::Creature || entities[i].dead) {
                continue;
            }
            const std::uint32_t id = game.objectForEntityIndex(i);
            for (const auto& [target, bits] : api.actor(id).perceived) {
                if ((bits & 1) != 0 && api.isCreature(target) && api.isAlive(target)) {
                    observer = id;
                    break;
                }
            }
        }
        if (observer == invalid) {
            logInfo("objects: no creature has seen anything yet; perception criteria not exercised");
        } else {
            const kotor::Vec3 from = api.position(observer);
            std::uint32_t best = invalid;
            float bestSquared = 0.0f;
            std::uint32_t bestUnseen = invalid;
            float bestUnseenSquared = 0.0f;
            for (const AreaEntry& entry : scanArea(api)) {
                if (entry.type != 1 || entry.id == observer || !api.isAlive(entry.id)) {
                    continue;
                }
                const kotor::Vec3 at = api.position(entry.id);
                const float squared = (at.x - from.x) * (at.x - from.x) + (at.y - from.y) * (at.y - from.y)
                    + (at.z - from.z) * (at.z - from.z);
                const auto found = api.actor(observer).perceived.find(entry.id);
                const bool seen = found != api.actor(observer).perceived.end() && (found->second & 1) != 0;
                if (seen && (best == invalid || squared < bestSquared)) {
                    best = entry.id;
                    bestSquared = squared;
                }
                if (!seen && (bestUnseen == invalid || squared < bestUnseenSquared)) {
                    bestUnseen = entry.id;
                    bestUnseenSquared = squared;
                }
            }
            check(best != invalid && objOf(38, {num(7), num(7), obj(observer), num(1)}) == best,
                  "nearest_creature_perception_seen");
            check(objOf(38, {num(7), num(6), obj(observer), num(1)}) == bestUnseen,
                  "nearest_creature_perception_not_seen");
        }
    }

    logInfo(concat("objects: ", ok ? "all checks passed" : "FAILED"));
    return ok ? 0 : 1;
}

} // namespace ke
