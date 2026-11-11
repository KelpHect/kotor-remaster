// Gameplay sound effects and visual effects driven from the shipped 2DA tables and MDL
// animation event tables. Retail evidence (swkotor.exe 1.0.3.0), short:
//   006a1880  visual effect spawner: visualeffects.2da node columns pick the attach node
//             (headconjure / impact / ground root by creature size).
//   0060dc50  swing/clash/parry sound pick: weaponsounds.2da column + rand() % n variants,
//             played at the creature through 005d5e10.
//   00617470  hit sound pick by the target's material; 0060b400 hitground (FallDirt/Hard/
//             Metal/Water by surface); 0061ab40 the MDL event name registry.
//   0057e8c0 / 006832a0  door and placeable sound loaders (placeableobjsnds.2da).
// Anything not read off those addresses is labelled "inferred" at the line that uses it.
#include "game/Effects.h"
#include "core/Log.h"

#include "game/AudioSystem.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace ke {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr std::size_t kLogLimit = 8192;

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string clean(std::string value)
{
    return value == "****" ? std::string{} : value;
}

kotor::Vec3 rot(const kotor::Mat3& m, kotor::Vec3 v)
{
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

kotor::Mat3 yawMatrix(float bearing)
{
    const float c = std::cos(bearing);
    const float s = std::sin(bearing);
    return kotor::Mat3{{{c, -s, 0.0f}, {s, c, 0.0f}, {0.0f, 0.0f, 1.0f}}};
}

int toInt(const std::string& text, int fallback = 0)
{
    return text.empty() ? fallback : std::atoi(text.c_str());
}

float toFloat(const std::string& text, float fallback)
{
    return text.empty() ? fallback : static_cast<float>(std::atof(text.c_str()));
}

// The priority group (PriorityGroups.2da row) each effect category plays in. Retail's callers pass
// the group when they start the sound: swings, hits and parries 15 (0060ddd8 / 00617979), projectiles
// 14, Force powers 13 (the area ones 12), a PowerItem blade 21, the ground hit 0 (0060b400), doors,
// placeables and everything else a positional single shot 22.
int categoryGroup(const std::string& category)
{
    if (category == "swing" || category == "parry" || category == "hit") return AudioSystem::kGroupCombat;
    if (category == "shot") return AudioSystem::kGroupProjectile;
    if (category == "saber") return AudioSystem::kGroupSingleGlobal;
    if (category == "hitground") return AudioSystem::kGroupUnmaskable;
    if (category == "spellconjure" || category == "spellimpact" || category == "spellduration"
        || category == "spellcessation") {
        return AudioSystem::kGroupSpell;
    }
    return AudioSystem::kGroupSinglePositional;
}

} // namespace

struct EffectDirector::Tables {
    std::map<std::string, std::optional<kotor::TwoDa>> loaded;
};

EffectDirector::EffectDirector(GameSession& session) : session_(session), tables_(new Tables) {}

EffectDirector::~EffectDirector() = default;

EffectDirector::Tables& EffectDirector::tables()
{
    return *tables_;
}

std::string EffectDirector::cell2(const char* table, const std::string& row, const std::string& column)
{
    auto it = tables().loaded.find(table);
    if (it == tables().loaded.end()) {
        std::optional<kotor::TwoDa> parsed;
        if (auto data = session_.install().find(table, kotor::resType(kotor::ResType::TwoDa))) {
            try {
                parsed = kotor::TwoDa::parse(std::move(*data), table);
            } catch (const kotor::FormatError&) {
                parsed.reset();
            }
        }
        it = tables().loaded.emplace(table, std::move(parsed)).first;
    }
    if (!it->second.has_value() || it->second->rowIndex(row) < 0
        || it->second->columnIndex(column) < 0) {
        return {};
    }
    return clean(it->second->cell(row, column));
}

std::uint32_t EffectDirector::pick(std::uint32_t n)
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return n == 0 ? 0 : rng_ % n;
}

void EffectDirector::gain(const std::string& category, float& volume, float& minDistance,
                          float& maxDistance)
{
    volume = 1.0f;
    minDistance = 10.0f;
    maxDistance = 20.0f;
    const AudioSystem* audio = session_.audioOrNull();
    if (audio == nullptr) {
        return;
    }
    const int group = categoryGroup(category);
    const AudioSystem::PriorityGroup& info = audio->priorityGroup(group);
    volume = audio->groupLevel(group);
    minDistance = std::min(info.maxVolumeDist, info.minVolumeDist);
    maxDistance = std::max(info.maxVolumeDist, info.minVolumeDist);
}

int EffectDirector::emit(const char* event, const std::string& resref, const std::string& category,
                         const kotor::Vec3* position, std::size_t entity, bool loop,
                         float volumeScale)
{
    if (resref.empty() || resref == "****") {
        return 0;
    }
    SfxRecord record;
    record.event = event;
    record.resref = resref;
    AudioSystem* audio = session_.audioOrNull();
    if (audio == nullptr) {
        return 0;
    }
    const int group = categoryGroup(category);
    record.bus = AudioSystem::groupBus(group);
    gain(category, record.volume, record.minDistance, record.maxDistance);
    record.positional = position != nullptr;
    record.loop = loop;
    record.entity = entity;
    if (position != nullptr) {
        record.position = *position;
    }
    AudioSystem::GroupSound play;
    play.group = group;
    play.position = position;
    play.loop = loop;
    play.sourceVolume127 = static_cast<int>(std::lround(std::clamp(volumeScale, 0.0f, 1.0f) * 127.0f));
    if (group == AudioSystem::kGroupUnmaskable) {
        play.pitchVariance = 0.0f; // 0060b400 passes no variance
    }
    record.volume = audio->voiceLevel(group, play.sourceVolume127, false, position != nullptr);
    record.voice = audio->playGroupSound(resref, play);
    if (record.voice > 0) {
        record.gain = audio->mixer().voiceGain(record.voice);
        record.pan = audio->mixer().voicePan(record.voice);
    }
    if (logging_ && log_.size() < kLogLimit) {
        log_.push_back(record);
    }
    return record.voice > 0 ? record.voice : 0;
}

// ---- creature facts ---------------------------------------------------------------

bool EffectDirector::entityPlacement(std::size_t entity, kotor::Vec3& position, float& bearing) const
{
    const kotor::Scene& scene = session_.scene();
    if (entity >= scene.entities.size()) {
        return false;
    }
    position = scene.entities[entity].position;
    bearing = scene.entities[entity].drawBearing();
    return true;
}

int EffectDirector::surfaceAt(kotor::Vec3 at) const
{
    // Same floor test as GameSession's footstep code (barycentric over floor facets).
    const kotor::Scene& scene = session_.scene();
    int bestSurface = -1;
    float best = -1.0e30f;
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (!facet.floor) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1.0e-8f) {
            continue;
        }
        const float u = ((b.y - c.y) * (at.x - c.x) + (c.x - b.x) * (at.y - c.y)) / den;
        const float v = ((c.y - a.y) * (at.x - c.x) + (a.x - c.x) * (at.y - c.y)) / den;
        const float w = 1.0f - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) {
            continue;
        }
        const float z = u * a.z + v * b.z + w * c.z;
        if (z <= at.z + 0.5f && z > best) {
            best = z;
            bestSurface = static_cast<int>(facet.surface);
        }
    }
    return bestSurface;
}

std::int32_t EffectDirector::weaponBaseItem(std::uint32_t object, bool offhand) const
{
    GameSession& s = session_;
    if (object == s.scriptWorld().pc) {
        // Hand slots are 4 (right) and 5 (left): ItemOps::refreshModel in ScriptItems.cpp.
        for (const InventoryItem& item : s.character_.inventory) {
            if (item.equipped && item.slot == (offhand ? 5 : 4)) {
                return item.baseItem;
            }
        }
        return -1;
    }
    const std::size_t index = s.entityOfObject(object);
    if (index == kNone || index >= s.scene_.entities.size()) {
        return -1;
    }
    const kotor::CreatureStats::Weapon& weapon = offhand ? s.scene_.entities[index].stats.offHand
                                                         : s.scene_.entities[index].stats.mainHand;
    return weapon.present ? weapon.baseItem : -1;
}

int EffectDirector::weaponRowFor(std::uint32_t attackerObject, std::size_t, bool offhand)
{
    const std::int32_t base = weaponBaseItem(attackerObject, offhand);
    if (base < 0) {
        return 0; // weaponsounds.2da row 0 is "Unarmed"
    }
    // baseitems.2da weaponmattype is the weaponsounds.2da row; blank is unarmed.
    return toInt(cell2("baseitems", std::to_string(base), "weaponmattype"), 0);
}

std::string EffectDirector::armorMaterial(std::size_t targetEntity, std::uint32_t)
{
    const kotor::Scene& scene = session_.scene();
    if (targetEntity >= scene.entities.size()) {
        return "leather";
    }
    const kotor::Entity& e = scene.entities[targetEntity];
    std::string material;
    if (e.kind == kotor::EntityKind::Creature) {
        material = cell2("appearancesndset", std::to_string(e.soundAppType), "armortype");
        // HitEvent 00617470: a blank appearance cell falls back to the armortype of the base item worn in
        // the body slot, and an empty result to "leather" (0061777c).
        if (material.empty() && targetEntity == session_.playerEntity()) {
            for (const InventoryItem& item : session_.character_.inventory) {
                if (item.equipped && item.slot == 1) {
                    material = cell2("baseitems", std::to_string(item.baseItem), "armortype");
                    break;
                }
            }
        } else if (material.empty()) {
            // The same fallback for any other creature: retail reads the base item of whatever the
            // target wears in the body slot (GetEquippedItemID 00617470), here its UTC Equip_ItemList.
            for (const kotor::CreatureItem& worn : e.creatureItems) {
                if (worn.slot != 1) {
                    continue;
                }
                if (auto data = session_.install_.find(worn.resref, 2025); data.has_value()) { // UTI
                    try {
                        const kotor::Gff uti = kotor::Gff::parse(std::move(*data), worn.resref);
                        if (const kotor::GffField* field = uti.field(uti.rootIndex(), "BaseItem"); field != nullptr) {
                            material = cell2("baseitems", std::to_string(static_cast<int>(uti.number(*field))),
                                             "armortype");
                        }
                    } catch (const kotor::FormatError&) {
                    }
                }
                break;
            }
        }
    } else {
        const char* table = e.kind == kotor::EntityKind::Door ? "genericdoors" : "placeables";
        const std::string app = cell2(table, std::to_string(e.appearance), "soundapptype");
        material = cell2("placeableobjsnds", app, "armortype");
    }
    material = lower(material);
    static const char* known[] = {"cloth", "leather", "armor", "forcefield", "metal", "wood", "stone"};
    for (const char* name : known) {
        if (material == name) {
            return material;
        }
    }
    return "leather"; // 00617782: nothing else named a material
}

int EffectDirector::playWeaponColumn(const char* event, int weaponRow, const std::string& column,
                                     const char* category, std::size_t entity)
{
    const std::string resref = cell2("weaponsounds", std::to_string(weaponRow), column);
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (!entityPlacement(entity, at, bearing)) {
        return 0;
    }
    at.z += 1.5f; // 0060ddd8 / 00617979 start the sound 1.5 m above the creature
    return emit(event, resref, category, &at, entity);
}

// ---- sounds -----------------------------------------------------------------------

int EffectDirector::footstep(std::size_t entity)
{
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (!entityPlacement(entity, at, bearing)) {
        return 0;
    }
    const kotor::Entity& e = session_.scene().entities[entity];
    if (e.dead) {
        return 0;
    }
    AudioSystem* audio = session_.audioOrNull();
    if (audio == nullptr) {
        return 0;
    }
    // CSWCCreature::PlayFootstep 0061a2d0: the row is the appearance footsteptype as it stands (a blank
    // cell stays silent, a row past 3 is a real row), the column is surfacemat.2da Name + rand() % 3.
    AudioSystem::FootstepRequest request;
    request.footstepType = e.footstepType;
    request.sizeCategory = e.sizeCategory;
    request.isPlayer = entity == session_.controlledEntity();
    request.stealth = request.isPlayer && session_.stealthActive();
    request.surface = surfaceAt({at.x, at.y, at.z + 2.0f});
    request.position = at;
    std::string resref;
    const int voice = audio->playCreatureFootstep(request, &resref);
    if (logging_ && log_.size() < kLogLimit && !resref.empty()) {
        SfxRecord record;
        record.event = "footstep";
        record.resref = resref;
        record.bus = audio::Bus::Effects;
        record.positional = true;
        record.position = at;
        record.entity = entity;
        record.voice = voice;
        if (voice > 0) {
            record.gain = audio->mixer().voiceGain(voice);
            record.pan = audio->mixer().voicePan(voice);
        }
        log_.push_back(record);
    }
    return voice;
}

int EffectDirector::hitGround(std::size_t entity)
{
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (!entityPlacement(entity, at, bearing)) {
        return 0;
    }
    const int surface = surfaceAt({at.x, at.y, at.z + 2.0f});
    const char* column = nullptr;
    switch (surface) { // 0060b400's switch
    case 1: case 3: case 14: column = "falldirt"; break;
    case 4: case 5: case 9: case 15: column = "fallhard"; break;
    case 6: case 11: case 12: case 13: column = "fallwater"; break;
    case 10: column = "fallmetal"; break;
    default: break;
    }
    if (column == nullptr) {
        return 0;
    }
    const kotor::Entity& e = session_.scene().entities[entity];
    return emit("hitground", cell2("appearancesndset", std::to_string(e.soundAppType), column),
                "hitground", &at, entity);
}

int EffectDirector::swing(std::size_t attacker, std::uint32_t attackerObject, int kind)
{
    static const char* names[] = {"swingshort", "swinglong", "swingtwirl"};
    kind = std::clamp(kind, 0, 2);
    const int weaponRow = weaponRowFor(attackerObject, attacker, false);
    return playWeaponColumn("swing", weaponRow, std::string(names[kind]) + std::to_string(pick(3)),
                            "swing", attacker);
}

int EffectDirector::clash(std::size_t attacker, std::uint32_t attackerObject)
{
    const int weaponRow = weaponRowFor(attackerObject, attacker, false);
    return playWeaponColumn("clash", weaponRow, "clash" + std::to_string(pick(2)), "parry", attacker);
}

int EffectDirector::parry(std::size_t attacker, std::uint32_t attackerObject)
{
    const int weaponRow = weaponRowFor(attackerObject, attacker, false);
    return playWeaponColumn("parry", weaponRow, "parry" + std::to_string(pick(2)), "parry", attacker);
}

int EffectDirector::hit(std::size_t attacker, std::uint32_t attackerObject, std::size_t target,
                        std::uint32_t targetObject)
{
    const int weaponRow = weaponRowFor(attackerObject, attacker, false);
    const std::string column = armorMaterial(target, targetObject) + std::to_string(pick(2));
    return playWeaponColumn("hit", weaponRow, column, "hit", target);
}

int EffectDirector::shot(std::size_t attacker, std::uint32_t attackerObject, bool power)
{
    const std::int32_t base = weaponBaseItem(attackerObject, false);
    if (base < 0) {
        return 0;
    }
    const std::string ammo = cell2("baseitems", std::to_string(base), "ammunitiontype");
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (ammo.empty() || !entityPlacement(attacker, at, bearing)) {
        return 0;
    }
    at.z += 1.0f;
    // Inferred: shotsound0 is the plain bolt and shotsound1 the power attack (cb_sh_blast1
    // against cb_sh_pwrblast1).
    return emit("shot", cell2("ammunitiontypes", ammo, power ? "shotsound1" : "shotsound0"), "shot", &at,
                attacker);
}

int EffectDirector::shotImpact(std::size_t attacker, std::uint32_t attackerObject, std::size_t target,
                               bool power)
{
    (void)attacker;
    const std::int32_t base = weaponBaseItem(attackerObject, false);
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (base < 0 || !entityPlacement(target, at, bearing)) {
        return 0;
    }
    const std::string ammo = cell2("baseitems", std::to_string(base), "ammunitiontype");
    at.z += 1.0f;
    return emit("shot_impact", cell2("ammunitiontypes", ammo, power ? "impactsound1" : "impactsound0"),
                "hit", &at, target);
}

void EffectDirector::attackResolved(std::uint32_t attackerObject, std::uint32_t targetObject,
                                    int result, bool offhand)
{
    GameSession& s = session_;
    const std::size_t attacker = s.entityOfObject(attackerObject);
    const std::size_t target = s.entityOfObject(targetObject);
    if (attacker == kNone || target == kNone) {
        return;
    }
    const std::int32_t base = weaponBaseItem(attackerObject, offhand);
    const bool ranged = base >= 0 && toInt(cell2("baseitems", std::to_string(base), "weapontype")) == 4;
    if (ranged) {
        shot(attacker, attackerObject, false);
        if (result == 0 || result == 2) {
            shotImpact(attacker, attackerObject, target, false);
        }
        return;
    }
    // Melee: a clip that carries Swing*/Hit events plays these itself (animationAdvanced),
    // so only a creature without such a clip gets the sounds from the attack roll.
    if (clipCarries(attacker, "swingshort") || clipCarries(attacker, "swinglong")
        || clipCarries(attacker, "swingtwirl")) {
        return;
    }
    swing(attacker, attackerObject, 0);
    if (result == 0 || result == 2) {
        hit(attacker, attackerObject, target, targetObject);
    } else if (result == 3) {
        parry(target, targetObject);
    }
    saberUp(attackerObject);
}

bool EffectDirector::clipCarries(std::size_t entity, const char* eventName) const
{
    for (const kotor::EntityMesh& mesh : session_.scene().entityMeshes) {
        if (mesh.entity != entity || mesh.attachModel || !mesh.model || mesh.animation.empty()) {
            continue;
        }
        for (const kotor::MdlAnimEvent& event : mesh.model->animationEvents(mesh.animation)) {
            if (lower(event.name) == eventName) {
                return true;
            }
        }
        return false;
    }
    return false;
}

void EffectDirector::saberUp(std::uint32_t object)
{
    if (saberVoices_.count(object) != 0) {
        return;
    }
    const std::int32_t base = weaponBaseItem(object, false);
    if (base < 0 || toInt(cell2("baseitems", std::to_string(base), "powereditem")) == 0) {
        return;
    }
    const std::size_t entity = session_.entityOfObject(object);
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (!entityPlacement(entity, at, bearing)) {
        return;
    }
    at.z += 1.0f;
    emit("saber_powerup", cell2("baseitems", std::to_string(base), "powerupsnd"), "saber", &at, entity);
    // Inferred timing: the hum loops from the power-up until the fight ends.
    const int hum = emit("saber_hum", cell2("baseitems", std::to_string(base), "poweredsnd"), "saber",
                         &at, entity, true);
    saberVoices_[object] = hum > 0 ? hum : -1;
}

void EffectDirector::saberDown(std::uint32_t object)
{
    const auto it = saberVoices_.find(object);
    if (it == saberVoices_.end()) {
        return;
    }
    AudioSystem* audio = session_.audioOrNull();
    if (audio != nullptr && it->second > 0) {
        audio->mixer().fadeVoice(it->second, 0.0f, 0.1f, true);
    }
    const std::int32_t base = weaponBaseItem(object, false);
    saberVoices_.erase(it);
    const std::size_t entity = session_.entityOfObject(object);
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (base >= 0 && entityPlacement(entity, at, bearing)) {
        at.z += 1.0f;
        emit("saber_powerdown", cell2("baseitems", std::to_string(base), "powerdownsnd"), "saber", &at,
             entity);
    }
}

bool EffectDirector::saberHumming(std::uint32_t object) const
{
    const auto it = saberVoices_.find(object);
    return it != saberVoices_.end() && it->second > 0;
}

int EffectDirector::objectSound(std::size_t entity, ObjectSound which)
{
    const kotor::Scene& scene = session_.scene();
    if (entity >= scene.entities.size()) {
        return 0;
    }
    const kotor::Entity& e = scene.entities[entity];
    if (e.kind != kotor::EntityKind::Door && e.kind != kotor::EntityKind::Placeable) {
        return 0;
    }
    const char* table = e.kind == kotor::EntityKind::Door ? "genericdoors" : "placeables";
    const std::string app = cell2(table, std::to_string(e.appearance), "soundapptype");
    if (app.empty()) {
        return 0;
    }
    static const char* columns[] = {"opened", "closed", "destroyed", "used", "locked"};
    const char* column = columns[static_cast<int>(which)];
    const std::string resref = cell2("placeableobjsnds", app, column);
    const char* names[] = {"door_opened", "door_closed", "door_destroyed", "door_used", "door_locked"};
    // Doors and placeables play in group 22 (SingleShotPositional): the executable has no soundgain table.
    return emit(e.kind == kotor::EntityKind::Door ? names[static_cast<int>(which)]
                                                  : (std::string("placeable_") + column).c_str(),
                resref, "object", &e.position, entity);
}

void EffectDirector::spellCast(std::size_t caster, std::int32_t spell)
{
    kotor::Vec3 at;
    float bearing = 0.0f;
    if (!entityPlacement(caster, at, bearing)) {
        return;
    }
    const std::string key = std::to_string(spell);
    at.z += 1.0f;
    // Inferred category: spells.2da castsound is a conjure-phase sound.
    emit("spell_cast", cell2("spells", key, "castsound"), "spellconjure", &at, caster);
    const std::string hand = cell2("spells", key, "casthandvisual");
    if (!hand.empty()) {
        spawnModel(hand, "handconjure", caster, at, bearing);
    }
}

bool EffectDirector::voice(std::size_t entity, int firstSlot, int count, const char* event)
{
    AudioSystem* audio = session_.audioOrNull();
    if (audio == nullptr || count <= 0) {
        return false;
    }
    const int slot = firstSlot + static_cast<int>(pick(static_cast<std::uint32_t>(count)));
    const bool played = audio->playCreatureSound(static_cast<std::uint32_t>(entity), slot);
    if (logging_ && log_.size() < kLogLimit) {
        SfxRecord record;
        record.event = event;
        record.resref = "ssf:" + std::to_string(slot);
        record.bus = audio::Bus::Effects;
        record.voice = played ? 1 : 0;
        record.entity = entity;
        log_.push_back(record);
    }
    return played;
}

bool EffectDirector::speak(std::size_t entity, int slot, VoiceKind kind)
{
    AudioSystem* audio = session_.audioOrNull();
    const kotor::Scene& scene = session_.scene();
    if (audio == nullptr || entity >= scene.entities.size()) {
        return false;
    }
    const kotor::Entity& e = scene.entities[entity];
    const std::size_t leader = session_.controlledEntity();
    const bool isLeader = entity == leader;
    if (kind != VoiceKind::Direct && leader < scene.entities.size()) {
        // 004f2520: only creatures within 30 m (900 squared) of the player's creature are told.
        const kotor::Vec3& from = scene.entities[leader].position;
        const float dx = e.position.x - from.x;
        const float dy = e.position.y - from.y;
        const float dz = e.position.z - from.z;
        if (dx * dx + dy * dy + dz * dz > 900.0f) {
            return false;
        }
    }
    switch (kind) {
    case VoiceKind::BattleCry:
        if (pick(10) >= 3) {
            return false;
        }
        slot = static_cast<int>(pick(5));
        break;
    case VoiceKind::AttackGrunt:
        if (!isLeader && pick(5) != 0) {
            return false;
        }
        slot = 9 + static_cast<int>(pick(3));
        break;
    case VoiceKind::Pain:
        if (!isLeader && pick(5) != 0) {
            return false;
        }
        slot = 12 + static_cast<int>(pick(2));
        break;
    default:
        break;
    }
    AudioSystem::SoundSetVoice who;
    who.isPlayer = isLeader;
    who.sizeCategory = e.sizeCategory;
    who.dead = e.dead;
    const bool played = audio->playSoundSetSlot(static_cast<std::uint32_t>(entity), slot, who);
    if (logging_ && log_.size() < kLogLimit) {
        SfxRecord record;
        record.event = "voice";
        record.resref = "ssf:" + std::to_string(slot);
        record.bus = audio::Bus::Voice;
        record.voice = played ? 1 : 0;
        record.entity = entity;
        log_.push_back(record);
    }
    return played;
}

// ---- animation events -------------------------------------------------------------

// A rolling or floating droid (footstepsounds.2da rows 8 and 9 carry a Rolling cell) has no footstep
// events: while it plays a walk or run clip a looped sound follows it (CSWCCreature 006105b0 starts the
// stream, 00610840 picks it from the animation id, 006107c0 plays it at rate 1.0 for the walk ids
// 0x2712, 0x2713, 0x276d, 0x2795 and at PitchOffset + 1.0 for the run ids 0x2714, 0x276e; any other
// animation stops it). The animation id is 0x2712 + the animations.2da row.
void EffectDirector::rollingSound(std::size_t entity, const std::string& clipName)
{
    const kotor::Entity& e = session_.scene().entities[entity];
    AudioSystem* audio = session_.audioOrNull();
    if (audio == nullptr || e.footstepType < 0) {
        return;
    }
    const std::string row = std::to_string(e.footstepType);
    const std::string rolling = cell2("footstepsounds", row, "rolling");
    if (rolling.empty()) {
        return;
    }
    const std::string clip = lower(clipName);
    const auto named = [&](int animationRow) {
        return lower(cell2("animations", std::to_string(animationRow), "name")) == clip;
    };
    int kind = -1;
    if (!clip.empty() && !e.dead) {
        if (named(0) || named(1) || named(91) || named(131)) {
            kind = 0;
        } else if (named(2) || named(92)) {
            kind = 1;
        }
    }
    const auto found = rollingVoices_.find(entity);
    const bool live = found != rollingVoices_.end() && audio->mixer().voiceActive(found->second);
    if (kind < 0) {
        if (found != rollingVoices_.end()) {
            if (live) {
                audio->mixer().stopVoice(found->second);
            }
            rollingVoices_.erase(found);
        }
        return;
    }
    const std::string offsetCell = cell2("footstepsounds", row, "pitchoffset");
    const float rate = kind == 0 ? 1.0f : toFloat(offsetCell, 0.0f) + 1.0f;
    if (live) {
        audio->mixer().setVoicePosition(found->second, e.position);
        audio->mixer().setVoicePitch(found->second, rate);
        return;
    }
    AudioSystem::GroupSound play;
    play.group = entity == session_.controlledEntity()
        ? AudioSystem::kGroupPlayerFootstep
        : (e.sizeCategory >= 1 && e.sizeCategory <= 3 ? AudioSystem::kGroupSmallFootstep
                                                      : AudioSystem::kGroupLargeFootstep);
    play.position = &e.position;
    play.zOffset = 1.5f;
    play.loop = true;
    play.stream = true;
    play.pitchVariance = 0.0f;
    play.fixedVariance = rate;
    const int voice = audio->playGroupSound(rolling, play);
    if (voice > 0) {
        rollingVoices_[entity] = voice;
    }
}

void EffectDirector::animationAdvanced(std::size_t entity, const std::shared_ptr<kotor::Mdl>& model,
                                       const std::string& clip, float from, float to, float duration,
                                       bool looped)
{
    (void)looped;
    if (!model || clip.empty() || entity >= session_.scene().entities.size()) {
        return;
    }
    rollingSound(entity, clip);
    const std::vector<kotor::MdlAnimEvent> events = model->animationEvents(clip);
    if (events.empty()) {
        return;
    }
    const bool wrapped = to < from;
    for (const kotor::MdlAnimEvent& event : events) {
        const bool inside = wrapped ? (event.time > from && event.time <= duration) || event.time <= to
                                    : (event.time > from && event.time <= to);
        if (inside) {
            fireEvent(entity, event.name);
        }
    }
}

bool EffectDirector::fireEvent(std::size_t entity, const std::string& rawName)
{
    GameSession& s = session_;
    if (entity >= s.scene_.entities.size() || s.scene_.entities[entity].kind != kotor::EntityKind::Creature) {
        return false;
    }
    const std::string name = lower(rawName);
    const std::uint32_t object = s.objectForEntity(entity);
    const auto targetOf = [&](std::size_t& index, std::uint32_t& obj) {
        ScriptApi api{s};
        ActorCombat& combat = api.actor(object).combat;
        obj = combat.attackTarget != script::kObjectInvalid ? combat.attackTarget : combat.lastHostileTarget;
        index = obj == script::kObjectInvalid ? kNone : s.entityOfObject(obj);
        return index != kNone;
    };
    if (name == "snd_footstep") {
        return footstep(entity) > 0;
    }
    if (name == "snd_hitground") {
        return hitGround(entity) > 0;
    }
    if (name == "swingshort") {
        return swing(entity, object, 0) > 0;
    }
    if (name == "swinglong") {
        return swing(entity, object, 1) > 0;
    }
    if (name == "swingtwirl") {
        return swing(entity, object, 2) > 0;
    }
    if (name == "clash") {
        return clash(entity, object) > 0;
    }
    if (name == "hitparry") {
        return parry(entity, object) > 0;
    }
    if (name == "hit" || name == "hitdamage") {
        std::size_t target = kNone;
        std::uint32_t targetObject = 0;
        return targetOf(target, targetObject) && hit(entity, object, target, targetObject) > 0;
    }
    if (name == "contact") {
        // 0060de20: the saber contact spark is VFX 4011 at the LightSaberHook node. Held by
        // the attacker, so it follows the blade; only a creature with a lightsaber shows it.
        if (weaponBaseItem(object, false) >= 8 && weaponBaseItem(object, false) <= 10) {
            Spawn request;
            request.vfx = 4011;
            request.target = entity;
            request.targetObject = object;
            request.sounds = true;
            return !spawn(request).empty();
        }
    }
    return false;
}

// ---- visual effects ---------------------------------------------------------------

const VisualEffectRow* EffectDirector::row(std::int32_t id)
{
    const std::string key = std::to_string(id);
    if (cell2("visualeffects", key, "label").empty()) {
        return nullptr;
    }
    auto found = rows_.find(id);
    if (found != rows_.end()) {
        return &found->second;
    }
    VisualEffectRow r;
    r.id = id;
    const auto get = [&](const char* column) { return cell2("visualeffects", key, column); };
    r.label = get("label");
    r.typeFd = get("type_fd").empty() ? 'F' : get("type_fd")[0];
    r.headModel = get("imp_headcon_node");
    r.impactModel = get("imp_impact_node");
    r.rootModel[0] = get("imp_root_s_node");
    r.rootModel[1] = get("imp_root_m_node");
    r.rootModel[2] = get("imp_root_l_node");
    r.rootModel[3] = get("imp_root_h_node");
    r.cessationHead = get("ces_headcon_node");
    r.cessationImpact = get("ces_impact_node");
    r.cessationRoot[0] = get("ces_root_s_node");
    r.cessationRoot[1] = get("ces_root_m_node");
    r.cessationRoot[2] = get("ces_root_l_node");
    r.cessationRoot[3] = get("ces_root_h_node");
    r.progImpact = toInt(get("progfx_impact"));
    r.progDuration = toInt(get("progfx_duration"));
    r.progCessation = toInt(get("progfx_cessation"));
    r.soundImpact = get("soundimpact");
    r.soundDuration = get("soundduration");
    // Retail asks the row for "SoundCessation" (006a6590, 00690510), but the shipped column is spelled
    // "soundcessastion", so GetCExoStringEntry fails and the resref is blank: a cessation plays nothing.
    // Nothing else in the executable reads the misspelt name.
    r.soundCessation = get("soundcessation");
    r.shakeType = toInt(get("shaketype"));
    r.shakeDelay = toFloat(get("shakedelay"), 0.0f);
    r.shakeDuration = toFloat(get("shakeduration"), 0.0f);
    return &rows_.emplace(id, std::move(r)).first->second;
}

const FxInstance* EffectDirector::instance(std::uint32_t id) const
{
    for (const FxInstance& fx : instances_) {
        if (fx.id == id) {
            return &fx;
        }
    }
    return nullptr;
}

std::string EffectDirector::phaseClip(const FxInstance& fx) const
{
    if (!fx.mdl) {
        return {};
    }
    const char* wanted = fx.phase == FxPhase::Impact     ? "impact"
                         : fx.phase == FxPhase::Duration ? "duration"
                                                         : "cessation";
    if (fx.mdl->hasAnimationAnyCase(wanted)) {
        for (const kotor::MdlAnimationInfo& info : fx.mdl->animations()) {
            if (lower(info.name) == wanted) {
                return info.name;
            }
        }
    }
    // Fire-and-forget effects often ship one clip named "default" (inferred phase mapping).
    for (const char* fallback : {"default", "impact", "duration"}) {
        for (const kotor::MdlAnimationInfo& info : fx.mdl->animations()) {
            if (lower(info.name) == fallback) {
                return info.name;
            }
        }
    }
    return fx.mdl->animations().empty() ? std::string{} : fx.mdl->animations().front().name;
}

std::uint32_t EffectDirector::startModel(const VisualEffectRow* vrow, const std::string& model,
                                         const std::string& node, const Spawn& request)
{
    if (model.empty()) {
        return 0;
    }
    std::shared_ptr<kotor::Mdl>& cached = models_[lower(model)];
    if (!cached) {
        try {
            cached = kotor::loadWorldModel(session_.install(), model);
        } catch (const std::exception&) {
            cached.reset();
        }
    }
    if (!cached) {
        return 0;
    }
    FxInstance fx;
    fx.id = nextId_++;
    fx.vfx = vrow != nullptr ? vrow->id : -1;
    fx.model = lower(model);
    fx.node = node;
    fx.target = request.target;
    fx.targetObject = request.targetObject;
    fx.ownerEffect = request.ownerEffect;
    fx.place = request.place;
    fx.placeBearing = request.placeBearing;
    fx.mdl = cached;
    fx.typeFd = vrow != nullptr ? vrow->typeFd : 'F';
    fx.permanent = request.permanent;
    fx.hold = request.temporary ? request.seconds : 0.0f;
    fx.tag = 0x80000000u | fx.id;
    fx.phase = FxPhase::Impact;
    fx.clip = phaseClip(fx);
    // A duration effect applied for a time (or until removed) holds the duration clip after
    // the impact one; an instant application plays the impact clip once.
    if (fx.typeFd != 'D' || (!request.temporary && !request.permanent && request.ownerEffect == 0)) {
        fx.typeFd = 'F';
    }
    placeInstance(fx);
    const std::size_t knownTextures = session_.scene().textures.size();
    // Geometry slot: reuse an idle slot of this model, else bake a new one.
    std::vector<Slot>& slots = slots_[fx.model];
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].inUse) {
            slots[i].inUse = true;
            fx.slot = slots[i].mesh;
            break;
        }
    }
    if (fx.slot == kNone) {
        kotor::EntityMesh mesh = kotor::buildWorldEntityMesh(session_.scene(), session_.install(), cached);
        if (!mesh.vertices.empty()) {
            mesh.visible = false;
            session_.scene().entityMeshes.push_back(std::move(mesh));
            slots.push_back({session_.scene().entityMeshes.size() - 1, true});
            fx.slot = slots.back().mesh;
            ++topology_;
        }
    }
    ensureEmitters(fx);
    // An emitter-only model adds no mesh, so nothing else tells the renderer about its new sprite:
    // the particle batch would be skipped for a texture index it has not uploaded.
    if (session_.scene().textures.size() != knownTextures) {
        ++session_.scene().meshRevision;
    }
    bakeInstance(fx);
    instances_.push_back(std::move(fx));
    return instances_.back().id;
}

void EffectDirector::placeInstance(FxInstance& fx)
{
    kotor::Scene& scene = session_.scene();
    kotor::Vec3 base = fx.place;
    float bearing = fx.placeBearing;
    if (fx.target != kNone && fx.target < scene.entities.size()) {
        const kotor::Entity& e = scene.entities[fx.target];
        base = e.position;
        bearing = e.drawBearing();
        fx.orientation = yawMatrix(bearing);
        // The effect hangs on the matching node of the target's model (006a1880).
        const kotor::EntityMesh* mesh = nullptr;
        for (const kotor::EntityMesh& candidate : scene.entityMeshes) {
            if (candidate.entity == fx.target && !candidate.attachModel && candidate.model) {
                mesh = &candidate;
                break;
            }
        }
        std::optional<kotor::Mdl::Pose> pose;
        if (mesh != nullptr && fx.node != "root") {
            pose = mesh->model->nodePose(fx.node, mesh->animation, mesh->animationTime);
            if (!pose && fx.node == "impact") {
                pose = mesh->model->nodePose("impact", "", 0.0f);
            }
        }
        if (pose) {
            fx.position = base + rot(fx.orientation, pose->translation);
        } else if (fx.node == "root") {
            fx.position = base;
        } else {
            fx.position = base + kotor::Vec3{0.0f, 0.0f, 1.0f}; // inferred chest height
        }
        return;
    }
    fx.orientation = yawMatrix(bearing);
    fx.position = base;
}

void EffectDirector::bakeInstance(FxInstance& fx)
{
    if (fx.slot == kNone || fx.slot >= session_.scene().entityMeshes.size()) {
        return;
    }
    kotor::EntityMesh& mesh = session_.scene().entityMeshes[fx.slot];
    std::vector<kotor::MdlAnimLayer> layers;
    if (!fx.clip.empty()) {
        layers.push_back({fx.clip, fx.time});
    }
    kotor::bakeWorldEntityMesh(mesh, fx.mdl, layers, fx.orientation, fx.position);
    mesh.visible = true;
    ++session_.animationRevision_;
}

void EffectDirector::ensureEmitters(FxInstance& fx)
{
    if (fx.emitters != 0 || !fx.mdl || fx.mdl->emitters().empty()) {
        return;
    }
    kotor::Scene& scene = session_.scene();
    const std::size_t before = scene.emitters.size();
    kotor::addModelEmitters(scene, session_.install(), fx.mdl, fx.tag, fx.position, fx.orientation);
    fx.emitters = scene.emitters.size() - before;
}

kotor::Vec3 EffectDirector::beamEnd(std::size_t entity) const
{
    // The target's impact node (006a0d30 asks the target's model for a node by name).
    const kotor::Scene& scene = session_.scene();
    const kotor::Entity& e = scene.entities[entity];
    for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entity && !mesh.attachModel && mesh.model) {
            if (const std::optional<kotor::Mdl::Pose> pose = mesh.model->nodePose("impact", "", 0.0f)) {
                return e.position + rot(yawMatrix(e.bearing), pose->translation);
            }
        }
    }
    return e.position + kotor::Vec3{0.0f, 0.0f, 1.0f}; // inferred chest height
}

void EffectDirector::driveEmitters(FxInstance& fx)
{
    if (fx.emitters == 0) {
        return;
    }
    for (kotor::EmitterInstance& emitter : session_.scene().emitters) {
        if (emitter.entity != fx.tag) {
            continue;
        }
        emitter.origin = fx.position;
        emitter.orientation = fx.orientation;
        if (fx.beamTarget < session_.scene().entities.size()) {
            emitter.boltTargetSet = true;
            emitter.boltTarget = beamEnd(fx.beamTarget);
        }
        // stepParticles samples owner clips for scene entities; a tagged emitter has none,
        // so the clip and time are set here and the sampled stamp is made to match.
        if (const std::optional<kotor::Mdl::Pose> pose = emitter.model->nodePose(emitter.node, fx.clip, fx.time)) {
            emitter.worldRotation = pose->rotation;
            emitter.worldPosition = pose->translation;
        }
        emitter.params = emitter.model->fxNodeParameters(emitter.ordinal, fx.clip, fx.time);
        kotor::noteDetonateEvents(emitter, fx.clip, emitter.poseValid ? emitter.lastClipTime : -1.0f, fx.time);
        if (fx.time < emitter.lastClipTime) {
            emitter.fired = false;
        }
        emitter.lastClipTime = fx.time;
        emitter.sampledClip.clear();
        emitter.sampledTime = 0.0f;
        emitter.poseValid = true;
    }
}

std::vector<std::uint32_t> EffectDirector::spawn(const Spawn& request)
{
    std::vector<std::uint32_t> ids;
    const VisualEffectRow* r = row(request.vfx);
    if (r == nullptr) {
        return ids;
    }
    kotor::Scene& scene = session_.scene();
    const auto add = [&](const std::string& model, const char* node) {
        if (const std::uint32_t id = startModel(r, model, node, request); id != 0) {
            ids.push_back(id);
        }
    };
    add(r->headModel, "headconjure");
    add(r->impactModel, "impact");
    // Retail 006a1880 picks the ground model by the target's appearance.2da sizecategory
    // (read at 005c081c; 3 without a creature): above 4 the H node, then while still empty
    // L above 3, then M above 0. The S node is never read.
    {
        int size = 3;
        if (request.target != kNone && request.target < scene.entities.size() &&
            scene.entities[request.target].kind == kotor::EntityKind::Creature) {
            size = toInt(cell2("appearance", std::to_string(scene.entities[request.target].appearance),
                               "sizecategory"), 3);
        }
        std::string root;
        if (size > 4) {
            root = r->rootModel[3];
        }
        if (size > 3 && root.empty()) {
            root = r->rootModel[2];
        }
        if (size > 0 && root.empty()) {
            root = r->rootModel[1];
        }
        add(root, "root");
    }
    // Retail 006a54d0 runs the row's progfx values by range; only shells and beams draw here.
    const bool held = request.temporary || request.permanent || request.ownerEffect != 0;
    if (!held || r->progImpact != r->progDuration) {
        startProgFx(*r, r->progImpact, request, ids);
    }
    if (held) {
        startProgFx(*r, r->progDuration, request, ids);
    }
    kotor::Vec3 at = request.place;
    if (request.target != kNone && request.target < scene.entities.size()) {
        at = scene.entities[request.target].position + kotor::Vec3{0.0f, 0.0f, 1.0f};
    }
    if (request.sounds) {
        emit("vfx_impact", r->soundImpact, "spellimpact", &at, request.target);
    }
    if (request.sounds && !r->soundDuration.empty() && r->typeFd == 'D'
        && (request.temporary || request.permanent || request.ownerEffect != 0)) {
        // The duration sound loops until the effect ends.
        const int voiceId = emit("vfx_duration", r->soundDuration, "spellduration", &at, request.target, true);
        if (voiceId > 0) {
            bool attached = false;
            for (FxInstance& fx : instances_) {
                if (!ids.empty() && fx.id == ids.front()) {
                    fx.durationVoice = voiceId;
                    attached = true;
                }
            }
            if (!attached && session_.audioOrNull() != nullptr) {
                session_.audioOrNull()->mixer().stopVoice(voiceId); // nothing to hold it
            }
        }
    }
    if (r->shakeType != 0) {
        shake_ = {};
        shake_.type = r->shakeType;
        shake_.delay = r->shakeDelay;
        shake_.duration = r->shakeDuration;
        shake_.active = true;
    }
    return ids;
}

std::uint32_t EffectDirector::spawnModel(const std::string& model, const std::string& node,
                                         std::size_t target, kotor::Vec3 place, float bearing)
{
    Spawn request;
    request.target = target;
    request.place = place;
    request.placeBearing = bearing;
    return startModel(nullptr, model, node, request);
}

void EffectDirector::startProgFx(const VisualEffectRow& vrow, std::int32_t prog, const Spawn& request,
                                 std::vector<std::uint32_t>& ids)
{
    kotor::Scene& scene = session_.scene();
    if ((prog == 1500 || prog == 1601 || prog == 1602 || prog == 1800) && request.target < scene.entities.size()) {
        // Retail 006a54d0: 1500-1599 ApplyFOV, 1600-1699 the speed blur, 1800-1899 the flash; each acts
        // on its one id only (the `== 0x5dc`, `0x641 / 0x642`, `0x708` tests).
        startHardFx(prog, request);
        return;
    }
    if (prog >= 1401 && prog <= 1426 && request.target < scene.entities.size()) {
        // Retail 006a1220: 1401-1412 fx_tex_01-12, 1413-1425 fx_tex_14-26, 1426 fx_tex_stealth, handed
        // with 0.02 to the creature's model (0063d440). The method it reaches was not found; the additive
        // pass is measured from retail frames with and without the shell.
        char name[20];
        if (prog == 1426) {
            std::snprintf(name, sizeof(name), "fx_tex_stealth");
        } else {
            std::snprintf(name, sizeof(name), "fx_tex_%02d", prog <= 1412 ? prog - 1400 : prog - 1399);
        }
        if (kotor::ensureOptionalTexture(scene, session_.install(), name) == kotor::kNoTexture) {
            return;
        }
        kotor::MdlMeshTriangle triangle;
        triangle.texture = name;
        Shell shell;
        shell.look = kotor::makeMaterialBatch(scene, session_.install(), triangle);
        shell.id = nextId_++;
        shell.entity = request.target;
        shell.targetObject = request.targetObject;
        shell.ownerEffect = request.ownerEffect;
        shell.permanent = request.permanent;
        // A bare impact's second is inferred.
        shell.left = request.temporary ? request.seconds : 1.0f;
        // Retail 00448130: the new texture replaces whatever shell the model wore.
        for (Shell& other : shells_) {
            if (other.entity == shell.entity && other.shown) {
                removeShell(other);
                other.shown = false;
            }
        }
        shells_.push_back(shell);
        applyShell(shells_.back());
        return;
    }
    if (prog >= 1200 && prog <= 1202) {
        // Retail 006a0800 loads v_fizzle_imp for 1200/1201 and v_fresist_imp for 1202; 006a2e10 hangs
        // it on headconjure (1200), handconjure (1201) or the target's impact node (1202, turned
        // toward the source: not carried here, the target's facing is used) and plays "impact".
        const char* model = prog == 1202 ? "v_fresist_imp" : "v_fizzle_imp";
        const char* node = prog == 1200 ? "headconjure" : prog == 1201 ? "handconjure" : "impact";
        if (const std::uint32_t id = startModel(&vrow, model, node, request); id != 0) {
            ids.push_back(id);
        }
        return;
    }
    if (prog >= 1700 && prog <= 1702) {
        // Retail 006a0860 loads v_medal_dur / v_revmask1_dur / v_revmask2_dur and 006a4680 hangs it on
        // the target's medalhook / revmask1hook / revmask2hook.
        static constexpr const char* kModels[] = {"v_medal_dur", "v_revmask1_dur", "v_revmask2_dur"};
        static constexpr const char* kHooks[] = {"medalhook", "revmask1hook", "revmask2hook"};
        if (const std::uint32_t id = startModel(&vrow, kModels[prog - 1700], kHooks[prog - 1700], request); id != 0) {
            ids.push_back(id);
        }
        return;
    }
    if (prog >= 608 && prog <= 621) {
        // Retail 006a0680 names the model; 006a0d30 ties it to the source and the target.
        static constexpr const char* kBeams[] = {"v_lightns_dur", "v_lightnx_dur", "v_drddisab_dur", "v_drdkill_dur",
                                                 "v_deathfld_dur", "v_drain_dur", "v_flame_dur", "v_stunray_dur",
                                                 "v_coldray_dur", "v_ionray01_dur", "v_ionray02_dur", "v_fstorm_dur",
                                                 "v_drdstun_dur", "v_fshock_dur"};
        if (request.beamSource >= scene.entities.size() || request.target >= scene.entities.size()) {
            ke::logInfo(ke::concat("vfx beam ", vrow.id, ": no ", request.beamSource >= scene.entities.size() ? "source" : "target",
                                 " entity (source ", static_cast<long long>(request.beamSource), ", target ",
                                 static_cast<long long>(request.target), ")"));
            return;
        }
        Spawn from = request;
        from.target = request.beamSource;
        const std::uint32_t id = startModel(&vrow, kBeams[prog - 608], request.beamNode, from);
        if (id == 0) {
            return;
        }
        for (FxInstance& fx : instances_) {
            if (fx.id == id) {
                fx.beamTarget = request.target;
                fx.typeFd = 'D';
                driveEmitters(fx);
            }
        }
        ids.push_back(id);
    }
}

void EffectDirector::applyShell(const Shell& shell)
{
    kotor::Scene& scene = session_.scene();
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity != shell.entity || mesh.fx
            || std::any_of(mesh.batches.begin(), mesh.batches.end(),
                           [&](const kotor::TextureBatch& batch) { return batch.shell == shell.id; })) {
            continue;
        }
        const std::size_t count = mesh.batches.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (mesh.batches[i].shell != 0u) {
                continue;
            }
            const kotor::TextureBatch& body = mesh.batches[i];
            kotor::TextureBatch batch = shell.look;
            batch.firstIndex = body.firstIndex;
            batch.indexCount = body.indexCount;
            batch.center = body.center;
            batch.twoSided = body.twoSided;
            batch.shell = shell.id;
            // Retail 00448130 gives every mesh a second pass with the shell texture and the blend its TXI
            // names: additive SRC_ALPHA / ONE without depth writes, punchthrough ONE / ZERO with the alpha
            // test, anything else SRC_ALPHA / ONE_MINUS_SRC_ALPHA with depth writes (0047af70). The pass is
            // unlit and pushed out by 0.02 (kotor::batchShadeMode).
            const kotor::TxiInfo::Blend blending = batch.texture < scene.textures.size()
                ? scene.textures[batch.texture].txi.blending : kotor::TxiInfo::Blend::Default;
            batch.alphaRef = -1.0f;
            batch.writeDepth = true;
            if (blending == kotor::TxiInfo::Blend::Additive) {
                batch.blend = kotor::BlendMode::Additive;
                batch.writeDepth = false;
            } else if (blending == kotor::TxiInfo::Blend::PunchThrough) {
                batch.blend = kotor::BlendMode::Opaque;
                batch.alphaRef = kotor::kPunchThroughAlpha;
            } else {
                batch.blend = kotor::BlendMode::Alpha;
            }
            batch.envmap = kotor::kNoTexture;
            batch.envMask = false;
            batch.lightmap = kotor::kNoTexture;
            batch.bumpmap = kotor::kNoTexture;
            batch.showsBehind = batch.blend != kotor::BlendMode::Opaque;
            mesh.batches.push_back(batch);
        }
        ++scene.meshRevision;
    }
}

void EffectDirector::removeShell(const Shell& shell)
{
    kotor::Scene& scene = session_.scene();
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (std::erase_if(mesh.batches, [&](const kotor::TextureBatch& batch) { return batch.shell == shell.id; }) != 0u) {
            ++scene.meshRevision;
        }
    }
}

void EffectDirector::updateShells(float dt)
{
    ScriptApi api{session_};
    for (std::size_t i = 0; i < shells_.size();) {
        Shell& shell = shells_[i];
        bool done = false;
        if (shell.ownerEffect != 0 && shell.targetObject != 0) {
            const std::vector<AppliedEffect>& held = api.actor(shell.targetObject).effects;
            done = std::none_of(held.begin(), held.end(), [&](const AppliedEffect& a) { return a.effect == shell.ownerEffect; });
        } else if (!shell.permanent) {
            shell.left -= dt;
            done = shell.left <= 0.0f;
        }
        if (done) {
            // Retail 006a56f0 -> 006a1470 -> 004485c0 clears the model's shell, whichever instance set it.
            const std::size_t entity = shell.entity;
            for (Shell& other : shells_) {
                if (other.entity == entity && other.shown) {
                    removeShell(other);
                    other.shown = false;
                }
            }
            shells_.erase(shells_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (shell.shown) {
            applyShell(shell); // A body rebuilt since (a changed armour) has lost its pass.
        }
        ++i;
    }
}

void EffectDirector::release(std::uint32_t id)
{
    std::size_t at = kNone;
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].id == id) {
            at = i;
        }
    }
    if (at == kNone || instances_[at].released) {
        return;
    }
    FxInstance& held = instances_[at];
    held.released = true;
    const std::int32_t vfx = held.vfx;
    const std::string node = held.node;
    const kotor::Vec3 position = held.position;
    Spawn request;
    request.target = held.target;
    request.targetObject = held.targetObject;
    request.place = held.place;
    request.placeBearing = held.placeBearing;
    if (held.durationVoice > 0 && session_.audioOrNull() != nullptr) {
        session_.audioOrNull()->mixer().fadeVoice(held.durationVoice, 0.0f, 0.15f, true);
        held.durationVoice = 0;
    }
    // A model without a cessation clip simply ends (inferred).
    if (held.mdl->hasAnimationAnyCase("cessation")) {
        held.phase = FxPhase::Cessation;
        held.time = 0.0f;
        held.clip = phaseClip(held);
    } else {
        held.phase = FxPhase::Done;
    }
    const VisualEffectRow* r = vfx >= 0 ? row(vfx) : nullptr;
    if (r == nullptr) {
        return;
    }
    const std::string cessationHead = r->cessationHead;
    const std::string cessationImpact = r->cessationImpact;
    emit("vfx_cessation", r->soundCessation, "spellcessation", &position, request.target);
    // startModel appends to instances_: `held` is not touched past this point.
    if (!cessationHead.empty() && node == "headconjure") {
        startModel(nullptr, cessationHead, "headconjure", request);
    }
    if (!cessationImpact.empty() && node == "impact") {
        startModel(nullptr, cessationImpact, "impact", request);
    }
}

void EffectDirector::releaseOwned(std::int64_t ownerEffect)
{
    std::vector<std::uint32_t> ids;
    for (const FxInstance& fx : instances_) {
        if (fx.ownerEffect == ownerEffect && !fx.released) {
            ids.push_back(fx.id);
        }
    }
    for (const std::uint32_t id : ids) {
        release(id);
    }
}

void EffectDirector::dropInstance(std::size_t index)
{
    FxInstance& fx = instances_[index];
    kotor::Scene& scene = session_.scene();
    if (fx.emitters != 0) {
        std::erase_if(scene.emitters,
                      [&](const kotor::EmitterInstance& emitter) { return emitter.entity == fx.tag; });
    }
    if (fx.slot != kNone && fx.slot < scene.entityMeshes.size()) {
        scene.entityMeshes[fx.slot].visible = false;
        ++scene.entityMeshes[fx.slot].revision;
        ++session_.animationRevision_;
        for (Slot& slot : slots_[fx.model]) {
            if (slot.mesh == fx.slot) {
                slot.inUse = false;
            }
        }
    }
    if (fx.durationVoice > 0 && session_.audioOrNull() != nullptr) {
        session_.audioOrNull()->mixer().fadeVoice(fx.durationVoice, 0.0f, 0.1f, true);
    }
    instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
}

void EffectDirector::update(float dt)
{
    if (shake_.active) {
        shake_.elapsed += dt;
        if (shake_.elapsed >= shake_.delay + shake_.duration) {
            shake_.active = false;
        }
    }
    updateShells(dt);
    updateHardFx(dt);
    ScriptApi api{session_};
    for (std::size_t i = 0; i < instances_.size();) {
        instances_[i].age += dt;
        instances_[i].time += dt;
        // A held effect ends when the script removed its record from the target.
        if (instances_[i].ownerEffect != 0 && !instances_[i].released && instances_[i].targetObject != 0) {
            const std::int64_t owner = instances_[i].ownerEffect;
            const std::vector<AppliedEffect>& held = api.actor(instances_[i].targetObject).effects;
            const bool present = std::any_of(held.begin(), held.end(),
                                             [&](const AppliedEffect& a) { return a.effect == owner; });
            if (!present) {
                release(instances_[i].id);
            }
        }
        FxInstance& fx = instances_[i];
        float length = fx.mdl && !fx.clip.empty() ? fx.mdl->animationDurationAnyCase(fx.clip) : 0.0f;
        if (length <= 0.0f) {
            length = 1.0f; // a model with no clip shows for one second (inferred)
        }
        bool done = fx.phase == FxPhase::Done;
        bool expired = false;
        if (fx.phase == FxPhase::Impact && fx.time >= length) {
            if (fx.typeFd == 'D' && !fx.released) {
                fx.phase = FxPhase::Duration;
                fx.clip = phaseClip(fx);
                fx.time = 0.0f;
            } else {
                done = true;
            }
        } else if (fx.phase == FxPhase::Duration) {
            if (fx.time >= length) {
                fx.time = std::fmod(fx.time, length);
            }
            if (!fx.permanent && fx.ownerEffect == 0 && fx.hold > 0.0f) {
                fx.hold -= dt;
                expired = fx.hold <= 0.0f;
            }
        } else if (fx.phase == FxPhase::Cessation && fx.time >= length) {
            done = true;
        }
        if (expired) {
            release(fx.id); // may append instances: `fx` is stale afterwards
            continue;       // looked at again, now in its cessation phase
        }
        if (done) {
            dropInstance(i);
            continue;
        }
        placeInstance(fx);
        bakeInstance(fx);
        driveEmitters(fx);
        ++i;
    }
}

void EffectDirector::resetScene()
{
    instances_.clear();
    shells_.clear();
    slots_.clear();
    shake_ = {};
    for (auto& [object, voiceId] : saberVoices_) {
        (void)object;
        if (voiceId > 0 && session_.audioOrNull() != nullptr) {
            session_.audioOrNull()->mixer().stopVoice(voiceId);
        }
    }
    saberVoices_.clear();
    ++topology_;
    // The module's objects go with their effects (inferred: their removal runs as they are destroyed).
    hardFx_.clear();
    view_ = {};
    speedBlurCount_ = 0;
    speedBlurOn_ = false;
    speedEntity_ = kNone;
    distortion_.clear();
}

// ---- hard-coded camera and frame programs ---------------------------------------------------

std::optional<EffectDirector::CameraZoom> EffectDirector::cameraZoom() const
{
    if (!view_.driven) {
        return std::nullopt;
    }
    return CameraZoom{view_.value, view_.dolly};
}

float EffectDirector::currentViewAngle() const
{
    // Camera vtable +0x48 (0045c4d0) reads +0x1d0; outside a program it is the style's VIEWANGLE.
    return view_.driven ? view_.value : session_.scene().cameraStyle.viewAngle;
}

void EffectDirector::beginViewAngle(float target, float seconds)
{
    // Camera::BeginViewAngleAnimation 0045bef0: a running animation is dropped and the angle reset to
    // 45 (0x42340000) first; the new one starts from the current angle, mode 0 (linear).
    if (view_.animating) {
        view_.value = 45.0f;
    }
    view_.animating = true;
    view_.start = view_.value;
    view_.target = target;
    view_.duration = seconds;
    view_.elapsed = 0.0f;
}

void EffectDirector::startHardFx(std::int32_t prog, const Spawn& request)
{
    HardFx fx;
    fx.prog = prog;
    fx.entity = request.target;
    fx.targetObject = request.targetObject;
    fx.ownerEffect = request.ownerEffect;
    fx.permanent = request.permanent;
    fx.left = request.temporary ? request.seconds : 0.0f;
    hardFx_.push_back(fx);
    applyHardFx(hardFx_.back());
}

void EffectDirector::applyHardFx(HardFx& fx)
{
    // Retail 006a54d0 runs a program once (+0xf4) until 006a56f0 takes it off.
    if (fx.applied) {
        return;
    }
    const std::size_t player = session_.controlledEntity();
    fx.applied = true;
    if (fx.prog == 1500) {
        // ApplyFOV 006a4580: only on the player creature. The current angle is kept (+0xe4), the stick
        // camera (behaviour 0x106a) saves its distance (+0xe8) and turns on its dolly (+0x84), then the
        // angle goes to 135 over 0.75 s (vtable +0x54) and the 4750 ms timer starts.
        if (player == kNone || fx.entity != player) {
            return;
        }
        fx.savedViewAngle = currentViewAngle();
        if (!view_.driven) {
            view_.value = fx.savedViewAngle;
        }
        view_.driven = true;
        view_.dolly = true;
        beginViewAngle(135.0f, 0.75f);
        fx.restoring = false;
        fx.timerMs = 4750.0f;
        return;
    }
    if (fx.prog == 1601 || fx.prog == 1602) {
        // 006a14e0: counts every creature it lands on; the blur starts on the first one when that is
        // the player (or there is none) and no minigame runs: ratio 0.75, AurEnableSpeedBlur.
        ++speedBlurCount_;
        if ((player == kNone || fx.entity == player) && speedBlurCount_ == 1 && !session_.miniGameActive()) {
            speedBlurRatio_ = 0.75f;
            speedBlurOn_ = true;
        }
        return;
    }
    if (fx.prog == 1800) {
        // 006a15d0 -> 0063d540 -> CSWCAnimBase vtable +0xe4 (0069de50, head 0069b950, wield 0069a8f0):
        // every model of the object gets the flash byte (00449af0). ManageSceneBSP 0046ea32 then puts
        // the model in the distortion list instead of the normal buckets.
        if (std::find(distortion_.begin(), distortion_.end(), static_cast<std::uint32_t>(fx.entity)) == distortion_.end()) {
            distortion_.push_back(static_cast<std::uint32_t>(fx.entity));
        }
    }
}

void EffectDirector::removeHardFx(HardFx& fx, bool force)
{
    const std::size_t player = session_.controlledEntity();
    if (fx.prog == 1500) {
        // RemoveFOV 006a49a0: on the player (or forced by UpdateFOV): animation off (+0x58), the kept
        // angle back at once (+0x44), the stick's distance back and its dolly off.
        if (player == kNone || (!force && fx.entity != player)) {
            return;
        }
        if (view_.driven) {
            view_ = {};
        }
        return;
    }
    if (!fx.applied) {
        return;
    }
    fx.applied = false;
    if (fx.prog == 1601 || fx.prog == 1602) {
        // 006a1570: the count goes down; at zero (or below, clamped) the blur stops unless a minigame runs.
        --speedBlurCount_;
        if (speedBlurCount_ < 0) {
            speedBlurCount_ = 0;
        } else if (speedBlurCount_ != 0) {
            return;
        }
        if (!session_.miniGameActive()) {
            speedBlurOn_ = false;
        }
        return;
    }
    if (fx.prog == 1800) {
        // 006a1610 -> 0063d550 -> vtable +0xe8 clears the byte (00449b00), whichever effect set it.
        std::erase(distortion_, static_cast<std::uint32_t>(fx.entity));
    }
}

void EffectDirector::updateHardFx(float dt)
{
    // The player's speed: retail reads the client creature's movement rate (+0x3b0, set by the player
    // control at 0067a687) against its run rate (+0x21c -> +0x5c, the one 00679510 returns). Here the
    // controlled body's ground speed over the frame stands for the former (engine choice).
    const std::size_t player = session_.controlledEntity();
    const kotor::Scene& scene = session_.scene();
    playerSpeed_ = 0.0f;
    if (player != kNone && player < scene.entities.size()) {
        const kotor::Vec3 at = scene.entities[player].position;
        if (speedEntity_ == player && dt > 0.0f) {
            const float dx = at.x - speedLast_.x;
            const float dy = at.y - speedLast_.y;
            playerSpeed_ = std::sqrt(dx * dx + dy * dy) / dt;
        }
        speedEntity_ = player;
        speedLast_ = at;
    } else {
        speedEntity_ = kNone;
    }

    ScriptApi api{session_};
    const float dtMs = dt * 1000.0f;
    bool distorting = false;
    for (std::size_t i = 0; i < hardFx_.size();) {
        HardFx& fx = hardFx_[i];
        bool done = false;
        if (fx.prog == 1500) {
            // CSWCVisualEffectOnObject::Update 006a6a50: the impact timer runs down in milliseconds;
            // while it lasts UpdateFOV 006a4e70 runs, when it ends 006a56f0 removes the program.
            if (dtMs < fx.timerMs) {
                fx.timerMs -= dtMs;
                if (player == kNone || fx.entity != player) {
                    removeHardFx(fx, true);
                } else if (!fx.restoring && fx.timerMs <= 4000.0f) {
                    // At 4000 ms left (0075574c): back to the kept angle over what remains (x 0.001).
                    view_.animating = false;
                    beginViewAngle(fx.savedViewAngle, fx.timerMs * 0.001f);
                    fx.restoring = true;
                }
            } else {
                fx.timerMs = 0.0f;
                removeHardFx(fx, false);
                done = true;
            }
        } else {
            if (fx.ownerEffect != 0 && fx.targetObject != 0) {
                const std::vector<AppliedEffect>& held = api.actor(fx.targetObject).effects;
                done = std::none_of(held.begin(), held.end(), [&](const AppliedEffect& a) { return a.effect == fx.ownerEffect; });
            } else if (!fx.permanent) {
                fx.left -= dt;
                done = fx.left <= 0.0f;
            }
            if (done) {
                removeHardFx(fx, false);
            } else if (fx.prog == 1601 || fx.prog == 1602) {
                // UpdateMotionBlur 006a5920: off while the target is not the player or the player is
                // debilitated, on again afterwards; the ratio follows the player's speed over its run
                // rate, at most 1 (0073d764), times 0.75 (0073e31c).
                if (player != kNone) {
                    const std::uint32_t object = session_.objectOfBody(player);
                    if (fx.entity != player || (object != 0 && isDebilitated(api, object))) {
                        removeHardFx(fx, false);
                    } else {
                        applyHardFx(fx);
                        const float run = object != 0 ? session_.creatureMoveRate(object, true) : 0.0f;
                        const float ratio = run > 0.0f ? std::min(playerSpeed_ / run, 1.0f) : 0.0f;
                        speedBlurRatio_ = ratio * 0.75f;
                    }
                }
            } else if (fx.prog == 1800 && fx.applied) {
                distorting = true;
            }
        }
        if (done) {
            hardFx_.erase(hardFx_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }

    // Camera::Animate 0045d220: the elapsed time grows first; past the duration the animation is
    // dropped and the angle stays where the last frame left it; else linear from start to target.
    if (view_.driven && view_.animating) {
        view_.elapsed += dt;
        if (view_.duration < view_.elapsed) {
            view_.animating = false;
        } else {
            const float t = view_.duration > 0.0f ? view_.elapsed / view_.duration : 1.0f;
            view_.value = t * view_.target + (1.0f - t) * view_.start;
        }
    }

    // RingDistortTextureController::Control 00460a90 builds the texture at the current phase, then
    // adds the frame time (0078e574 x 1.0) and wraps it at 2 pi (00740c88); it runs on the frames the
    // distortion pass draws.
    distortionPhaseShown_ = distortionPhase_;
    if (distorting) {
        distortionPhase_ += dt;
        if (distortionPhase_ > 6.2831855f) {
            do {
                distortionPhase_ -= 6.2831855f;
            } while (distortionPhase_ > 6.2831855f);
        }
    }
}

} // namespace ke
