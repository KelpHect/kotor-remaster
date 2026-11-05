// Doors, locks, traps, plot flags, factions, map, tokens, UI and cinematic state.
// Evidence is cited by retail address where the local exports have pseudocode;
// where they do not (most of this family), the rule follows nwscript.nss and is
// called out as such. docs/recon/63_WORLD_SIM.md is the contract.
#include "game/ScriptHost.h"

#include "core/Log.h"
#include "game/GameSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ke {

namespace {

constexpr float kDegrees = 180.0f / 3.14159265358979f;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
// Engine safety values, not retail rules: nothing a script does may hold the
// player forever.
constexpr float kRestrictWatchdogSeconds = 120.0f;
constexpr float kFadeHoldWatchdogSeconds = 120.0f;
// Passive trap detection: how close the player must be for an Awareness roll.
constexpr float kTrapDetectRadius = 6.0f;
// SpeakString reach for TALKVOLUME_TALK / WHISPER. No retail value was recovered.
constexpr float kTalkRadius = 20.0f;
constexpr float kWhisperRadius = 2.0f;

using Cells = std::optional<std::vector<script::Value>>;

std::string lowerText(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// Listen patterns: `*` matches any run, `?` one character, case-insensitive.
bool globMatch(const std::string& pattern, const std::string& text)
{
    const std::string p = lowerText(pattern);
    const std::string t = lowerText(text);
    std::size_t pi = 0;
    std::size_t ti = 0;
    std::size_t star = std::string::npos;
    std::size_t mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') {
        ++pi;
    }
    return pi == p.size();
}

float distance2(kotor::Vec3 a, kotor::Vec3 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

} // namespace

// ---- GameSession: keys, skills, doors --------------------------------------

bool GameSession::playerHasKey(const std::string& keyTag)
{
    if (keyTag.empty()) {
        return false;
    }
    const std::string wanted = lowerText(keyTag);
    for (const InventoryItem& item : character_.inventory) {
        if (lowerText(item.resref) == wanted) {
            return true;
        }
        auto cached = itemTemplates_.find(item.resref);
        if (cached == itemTemplates_.end()) {
            if (auto templ = kotor::loadItemTemplate(install_, item.resref); templ.has_value()) {
                cached = itemTemplates_.emplace(item.resref, *templ).first;
            }
        }
        if (cached != itemTemplates_.end() && lowerText(cached->second.tag) == wanted) {
            return true;
        }
    }
    return false;
}

std::int32_t GameSession::skillRank(const char* skill) const
{
    const std::string wanted = lowerText(skill);
    std::int32_t rank = 0;
    for (const auto& [name, value] : character_.skills) {
        if (lowerText(name) == wanted) {
            rank = value;
            break;
        }
    }
    if (rank > 0 && rules_.loaded()) {
        rank += rules_.skillBonus(character_, wanted);
    }
    return rank;
}

// d20 + rank against the DC. An untrained skill (rank 0) cannot be used: skills.2da
// marks Security and Demolitions untrained=0. The roll-and-add form is the d20 rule
// the rest of the engine uses; no retail pseudocode for the lock check was found.
bool GameSession::skillCheck(const char* skill, std::int32_t dc, const char* what)
{
    const std::int32_t rank = skillRank(skill);
    if (rank <= 0) {
        // skills.2da `untrained`: Awareness may be tried with no ranks, Security may not.
        bool allowed = false;
        if (auto data = install_.find("skills", 2017); data.has_value()) {
            try {
                const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "skills.2da");
                for (std::size_t row = 0; row < table.rowCount(); ++row) {
                    if (lowerText(table.cell(row, static_cast<std::size_t>(table.columnIndex("label"))))
                        == lowerText(skill)) {
                        allowed = table.cell(row, static_cast<std::size_t>(table.columnIndex("untrained")))
                            == "1";
                        break;
                    }
                }
            } catch (const kotor::FormatError&) {
            }
        }
        if (!allowed) {
            logEvent(std::string(what) + ": " + skill + " untrained");
            return false;
        }
    }
    const std::int32_t roll = 1 + static_cast<std::int32_t>(random() % 20u);
    const bool ok = roll + rank >= dc;
    logEvent(std::string(what) + ": " + skill + " d20=" + std::to_string(roll) + "+"
             + std::to_string(rank) + (ok ? " >= " : " < ") + std::to_string(dc));
    return ok;
}

bool GameSession::keyUnlock(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    const std::string keyName = scene_.entities[index].keyName;
    if (!playerHasKey(keyName)) {
        return false;
    }
    const bool consume = scene_.entities[index].autoRemoveKey;
    scene_.entities[index].locked = false;
    logEvent("unlock " + scene_.entities[index].tag + " with key " + keyName);
    if (consume) {
        const std::string wanted = lowerText(keyName);
        for (const InventoryItem& item : character_.inventory) {
            auto cached = itemTemplates_.find(item.resref);
            const bool byTag = cached != itemTemplates_.end()
                && lowerText(cached->second.tag) == wanted;
            if (lowerText(item.resref) == wanted || byTag) {
                removeInventoryItem(item.resref, 1);
                break;
            }
        }
    }
    ScriptContext extra{{"lastUnlocked", scriptWorld_.pc}};
    runHook("OnUnlock", index, scriptWorld_.pc, &extra);
    return true;
}

bool GameSession::unlockDoor(std::size_t index, std::uint32_t actor)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    const kotor::EntityKind kind = scene_.entities[index].kind;
    if (kind != kotor::EntityKind::Door && kind != kotor::EntityKind::Placeable) {
        return false;
    }
    if (!scene_.entities[index].locked) {
        return true;
    }
    if (keyUnlock(index)) {
        return true;
    }
    // KeyRequired means the lock cannot be picked, only keyed or scripted.
    if (scene_.entities[index].keyRequired) {
        logEvent("unlock " + scene_.entities[index].tag + ": needs its key");
        return false;
    }
    if (!skillCheck("security", scene_.entities[index].openLockDC, "unlock")) {
        return false;
    }
    scene_.entities[index].locked = false;
    ScriptContext extra{{"lastUnlocked", actor == 0 ? scriptWorld_.pc : actor}};
    runHook("OnUnlock", index, scriptWorld_.pc, &extra);
    return true;
}

bool GameSession::lockDoor(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[index];
    if ((entity.kind != kotor::EntityKind::Door && entity.kind != kotor::EntityKind::Placeable)
        || !entity.lockable || entity.locked) {
        return false;
    }
    entity.locked = true;
    ScriptContext extra{{"lastLocked", scriptWorld_.pc}};
    runHook("OnLock", index, scriptWorld_.pc, &extra);
    return true;
}

bool GameSession::closeDoor(std::size_t index)
{
    if (index >= scene_.entities.size() || scene_.entities[index].kind != kotor::EntityKind::Door
        || !scene_.entities[index].open) {
        return false;
    }
    scene_.entities[index].open = false;
    playAnimation(index, "closing1", false);
    runHook("OnClosed", index, scriptWorld_.pc);
    return true;
}

UseOutcome GameSession::openDoor(std::size_t index, std::uint32_t actor)
{
    if (index >= scene_.entities.size() || scene_.entities[index].kind != kotor::EntityKind::Door) {
        return UseOutcome::Invalid;
    }
    const std::uint32_t who = actor == 0 ? scriptWorld_.pc : actor;
    if (scene_.entities[index].locked && !keyUnlock(index)) {
        logEvent("door " + scene_.entities[index].tag + ": locked");
        runHook("OnFailToOpen", index, who);
        return UseOutcome::Locked;
    }
    if (index < scene_.entities.size() && scene_.entities[index].trapFlag
        && !scene_.entities[index].trapDisarmed) {
        springTrap(index, who);
    }
    if (index < scene_.entities.size() && !scene_.entities[index].open) {
        scene_.entities[index].open = true;
        playAnimation(index, "opening1", false);
        runHook("OnOpen", index, who);
    }
    return UseOutcome::Opened;
}

bool GameSession::bashDoor(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[index];
    // Plot objects cannot be hurt; only a locked, intact door needs bashing.
    if (entity.kind != kotor::EntityKind::Door || entity.plot || !entity.locked
        || entity.hitPoints <= 0) {
        return false;
    }
    std::int32_t dice = 1;
    std::int32_t die = 4;
    std::int32_t bonus = 0;
    if (rules_.loaded()) {
        rules_.weaponDamage(character_, dice, die, bonus);
    }
    std::int32_t damage = bonus;
    for (std::int32_t i = 0; i < dice; ++i) {
        damage += 1 + static_cast<std::int32_t>(random() % static_cast<std::uint32_t>(std::max(1, die)));
    }
    damage = std::max(0, damage - entity.hardness);
    entity.hitPoints = std::max(0, entity.hitPoints - damage);
    logEvent("bash " + entity.tag + " for " + std::to_string(damage) + " ("
             + std::to_string(entity.hitPoints) + " left)");
    ScriptContext hit{{"lastDamager", scriptWorld_.pc}};
    runHook("OnDamaged", index, scriptWorld_.pc, &hit);
    if (index < scene_.entities.size() && scene_.entities[index].hitPoints <= 0) {
        scene_.entities[index].locked = false;
        scene_.entities[index].open = true;
        playAnimation(index, "opening1", false);
        ScriptContext killed{{"lastKiller", scriptWorld_.pc}};
        runHook("OnDeath", index, scriptWorld_.pc, &killed);
    }
    return true;
}

// ---- GameSession: traps -----------------------------------------------------

void GameSession::springTrap(std::size_t index, std::uint32_t victim)
{
    if (index >= scene_.entities.size()) {
        return;
    }
    kotor::Entity& entity = scene_.entities[index];
    if (!entity.trapFlag || entity.trapDisarmed || (entity.trapSprung && entity.trapOneShot)) {
        return;
    }
    entity.trapSprung = true;
    entity.trapDetected = true;
    const std::string tag = entity.tag;
    const std::int32_t trapType = entity.trapType;
    const bool oneShot = entity.trapOneShot;
    logEvent("trap " + tag + " sprung");
    const auto own = entity.scripts.find("OnTrapTriggered");
    if (own != entity.scripts.end() && !own->second.empty()) {
        runHook("OnTrapTriggered", index, victim);
    } else if (auto data = install_.find("traps", 2017); data.has_value()) {
        // No script on the object: the trap kind's default script from traps.2da.
        try {
            const kotor::TwoDa traps = kotor::TwoDa::parse(std::move(*data), "traps.2da");
            const std::string row = std::to_string(trapType);
            if (traps.rowIndex(row) >= 0) {
                const std::string script = traps.cell(row, "trapscript");
                if (!script.empty() && script != "****") {
                    ScriptContext context{{"entering", victim}, {"caller", victim},
                                          {"trigger", objectForEntity(index)}};
                    runScriptWith(script, objectForEntity(index), context);
                }
            }
        } catch (const kotor::FormatError&) {
        }
    }
    if (oneShot && index < scene_.entities.size()) {
        scene_.entities[index].trapDisarmed = true;
    }
}

bool GameSession::detectTrap(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[index];
    if (!entity.trapFlag || !entity.trapDetectable || entity.trapDisarmed) {
        return false;
    }
    if (entity.trapDetected) {
        return true;
    }
    if (!skillCheck("awareness", entity.trapDetectDC, "detect trap")) {
        return false;
    }
    entity.trapDetected = true;
    scriptWorld_.attrs["last_trap_detected"] = static_cast<std::int64_t>(objectForEntity(index));
    return true;
}

bool GameSession::disarmTrap(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[index];
    if (!entity.trapFlag || !entity.trapDisarmable || !entity.trapDetected || entity.trapDisarmed) {
        return false;
    }
    if (!skillCheck("security", entity.trapDisarmDC, "disarm trap")) {
        springTrap(index, scriptWorld_.pc);
        return false;
    }
    entity.trapDisarmed = true;
    ScriptContext extra{{"lastDisarmed", scriptWorld_.pc}};
    runHook("OnDisarm", index, scriptWorld_.pc, &extra);
    return true;
}

// ---- GameSession: click handling -------------------------------------------

UseOutcome GameSession::useObject(std::size_t index)
{
    return useObjectImpl(index, true);
}

UseOutcome GameSession::useObjectImpl(std::size_t index, bool checkReach)
{
    if (index >= scene_.entities.size()) {
        return UseOutcome::Invalid;
    }
    const kotor::EntityKind kind = scene_.entities[index].kind;
    if (kind != kotor::EntityKind::Door && kind != kotor::EntityKind::Placeable
        && kind != kotor::EntityKind::Store) {
        return UseOutcome::Invalid;
    }
    if (checkReach && (controlLocked() || playerDead_ || dialogueView_.active)) {
        return UseOutcome::Refused;
    }
    if (checkReach && !inReach(index, kind == kotor::EntityKind::Placeable ? 2.5f : 3.0f)) {
        return UseOutcome::OutOfRange;
    }
    const std::uint32_t pc = scriptWorld_.pc;
    if (kind == kotor::EntityKind::Store) {
        if (!openStore(index)) {
            return UseOutcome::Invalid;
        }
        setUiPanel(UiPanel::Merchant);
        return UseOutcome::Used;
    }
    runHook("OnClick", index, pc);
    if (index >= scene_.entities.size()) {
        return UseOutcome::Used;
    }
    if (kind == kotor::EntityKind::Door) {
        // Copy before any hook runs: a script may add entities and move the vector.
        const std::string linkedModule = scene_.entities[index].linkedModule;
        const std::string linkedTo = scene_.entities[index].linkedTo;
        if (linkedModule.empty()) {
            if (scene_.entities[index].open) {
                closeDoor(index);
                return UseOutcome::Closed;
            }
            return openDoor(index);
        }
        // A transition door is crossed by clicking it, and only while unlocked.
        if (scene_.entities[index].locked && !keyUnlock(index)) {
            logEvent("door " + scene_.entities[index].tag + ": locked, no transition");
            runHook("OnFailToOpen", index, pc);
            return UseOutcome::Locked;
        }
        if (sim_->areaUnescapable) {
            logEvent("door " + scene_.entities[index].tag + ": area is unescapable");
            return UseOutcome::Refused;
        }
        runHook("OnOpen", index, pc);
        return loadModule(linkedModule, nullptr, linkedTo) ? UseOutcome::Transition
                                                           : UseOutcome::Refused;
    }
    // Placeable.
    kotor::Entity& entity = scene_.entities[index];
    if (!entity.useable || entity.dead) {
        return UseOutcome::Invalid;
    }
    if (entity.locked && !keyUnlock(index)) {
        logEvent("use " + entity.tag + ": locked");
        runHook("OnFailToOpen", index, pc);
        return UseOutcome::Locked;
    }
    if (index < scene_.entities.size() && scene_.entities[index].trapFlag
        && !scene_.entities[index].trapDisarmed) {
        springTrap(index, pc);
    }
    logEvent("use " + scene_.entities[index].tag);
    runHook("OnUsed", index, pc);
    if (index >= scene_.entities.size()) {
        return UseOutcome::Used;
    }
    // Only a placeable with an inventory is a container.
    if (scene_.entities[index].hasInventory && openLoot(index)) {
        return UseOutcome::Looted;
    }
    if (!scene_.entities[index].conversation.empty() && !dialogueView_.active) {
        startDialogue(index);
    }
    return UseOutcome::Used;
}

// ---- GameSession: control, tokens, factions, galaxy map ---------------------

bool GameSession::controlLocked() const
{
    return sim_->noClicksRemaining > 0.0f || sim_->restrictMode;
}

void GameSession::releaseControl()
{
    sim_->noClicksRemaining = 0.0f;
    sim_->restrictMode = false;
    sim_->restrictHeld = 0.0f;
    sim_->fade = FadeState{};
}

std::string GameSession::substituteTokens(std::string text) const
{
    std::size_t at = 0;
    while ((at = text.find('<', at)) != std::string::npos) {
        const std::size_t close = text.find('>', at);
        if (close == std::string::npos) {
            break;
        }
        const std::string name = lowerText(text.substr(at + 1, close - at - 1));
        if (name.rfind("custom", 0) == 0 && name.size() > 6
            && std::all_of(name.begin() + 6, name.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; })) {
            const auto it = sim_->customTokens.find(std::atoi(name.c_str() + 6));
            const std::string value = it != sim_->customTokens.end() ? it->second : std::string{};
            text.replace(at, close - at + 1, value);
            at += value.size();
        } else {
            at = close + 1;
        }
    }
    return text;
}

bool GameSession::setObjectFaction(std::uint32_t object, int faction)
{
    if (faction < 0) {
        return false;
    }
    ScriptApi api{*this};
    const std::size_t index = api.entityFor(object);
    if (index == kNone || scene_.entities[index].kind != kotor::EntityKind::Creature) {
        return false;
    }
    scene_.entities[index].faction = static_cast<std::uint32_t>(faction);
    // A scripted change replaces the hostile override a spawn may carry.
    scene_.entities[index].hostileOverride = false;
    if (script::GameObject* entry = scriptWorld_.get(object); entry != nullptr) {
        entry->attrs["faction"] = static_cast<std::int64_t>(faction);
    }
    refreshHostility();
    logEvent("faction " + scene_.entities[index].tag + " -> " + std::to_string(faction)
             + (scene_.entities[index].hostile ? " (hostile)" : ""));
    return true;
}

bool GameSession::selectPlanet(std::int32_t planet)
{
    GalaxyMapState& map = sim_->galaxy;
    if (!map.open || planet < 0 || static_cast<std::size_t>(planet) >= kPlanetSlots
        || !map.available[static_cast<std::size_t>(planet)]
        || !map.selectable[static_cast<std::size_t>(planet)]) {
        return false;
    }
    map.selected = planet;
    map.open = false;
    logEvent("galaxy map: selected planet " + std::to_string(planet));
    // The engine names this script itself (k_sup_galaxymap is a literal in the
    // binary at 0075497c, referenced from 00695980); planetary.2da has no script column.
    runScriptWith("k_sup_galaxymap", scriptWorld_.pc, ScriptContext{});
    return true;
}

std::vector<std::pair<std::int32_t, std::string>> GameSession::galaxyPlanetNames()
{
    std::vector<std::pair<std::int32_t, std::string>> out;
    auto data = install_.find("planetary", 2017);
    if (!data.has_value()) {
        return out;
    }
    try {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "planetary.2da");
        for (std::size_t row = 0; row < table.rowCount() && row < kPlanetSlots; ++row) {
            std::string name = table.cell(row, static_cast<std::size_t>(table.columnIndex("label")));
            const std::string strref = table.cell(row, static_cast<std::size_t>(table.columnIndex("name")));
            if (tlk_.has_value() && !strref.empty() && strref != "****") {
                const std::string resolved = tlk_->text(static_cast<std::uint32_t>(std::atoi(strref.c_str())));
                if (!resolved.empty()) {
                    name = resolved;
                }
            }
            out.emplace_back(static_cast<std::int32_t>(row), std::move(name));
        }
    } catch (const kotor::FormatError&) {
    }
    return out;
}

bool GameSession::mapPinEnabled(const std::string& tag) const
{
    return sim_->mapPinsEnabled.contains(lowerText(tag));
}

bool GameSession::mapRevealed(kotor::Vec3 point) const
{
    for (const MapRevealRecord& record : sim_->mapReveals) {
        if (record.module != module_) {
            continue;
        }
        if (record.radius < 0 || distance2(record.point, point) <= static_cast<float>(record.radius)) {
            return true;
        }
    }
    return false;
}

// ---- GameSession: encounters ------------------------------------------------

namespace {

// Per-object stats come from the creature's UTC, not from constants.
void applyCreatureTemplate(script::GameObject& object, kotor::Install& install,
                           const std::string& resref, const kotor::Entity& entity)
{
    object.attrs["fortitude"] = static_cast<std::int64_t>(entity.fortSave);
    object.attrs["will"] = static_cast<std::int64_t>(entity.willSave);
    object.attrs["reflex"] = static_cast<std::int64_t>(entity.refSave);
    auto data = install.find(resref, 2027);
    if (!data.has_value()) {
        return;
    }
    try {
        kotor::Gff utc = kotor::Gff::parse(std::move(*data), resref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        if (const kotor::GffField* f = utc.field(root, "ChallengeRating"); f != nullptr) {
            object.attrs["challenge_rating"] = static_cast<std::int64_t>(utc.number(*f));
        }
        if (const kotor::GffField* f = utc.field(root, "GoodEvil"); f != nullptr) {
            object.attrs["good_evil"] = static_cast<std::int64_t>(utc.integer(*f));
        }
        std::int64_t level = 0;
        std::int64_t firstClass = -1;
        if (const kotor::GffField* list = utc.field(root, "ClassList"); list != nullptr) {
            for (const std::uint32_t item : utc.list(*list)) {
                if (const kotor::GffField* f = utc.field(item, "ClassLevel"); f != nullptr) {
                    level += utc.integer(*f);
                } else if (const kotor::GffField* g = utc.field(item, "Level"); g != nullptr) {
                    level += utc.integer(*g);
                }
                if (firstClass < 0) {
                    if (const kotor::GffField* f = utc.field(item, "Class"); f != nullptr) {
                        firstClass = utc.integer(*f);
                    }
                }
            }
        }
        if (level > 0) {
            object.attrs["level"] = level;
        }
        if (firstClass >= 0) {
            object.attrs["class_id"] = firstClass;
        }
    } catch (const kotor::FormatError&) {
    }
}

bool allDead(const kotor::Scene& scene, const EncounterState& state)
{
    for (const std::size_t entity : state.entities) {
        if (entity < scene.entities.size() && !scene.entities[entity].dead) {
            return false;
        }
    }
    return true;
}

} // namespace

// One wave: RecCreatures (capped by MaxCreatures) drawn from the template's
// creature list in order, placed on the volume's spawn points. The encounter's
// Faction overrides the creature's own so a hostile encounter is hostile.
// Gap: the CR/difficulty budget (encdifficulty.2da) that retail applies to the
// creature mix was not recovered from the binary.
std::int32_t GameSession::spawnEncounterWave(std::size_t encounterIndex)
{
    if (encounterIndex >= scene_.encounters.size() || encounterIndex >= encounterStates_.size()) {
        return 0;
    }
    const kotor::EncounterVolume volume = scene_.encounters[encounterIndex];
    const kotor::EncounterTemplate& encounter = volume.templateData;
    EncounterState& state = encounterStates_[encounterIndex];
    if (!encounter.active || encounter.creatures.empty() || volume.spawnPoints.empty()) {
        return 0;
    }
    std::int32_t count = encounter.recCreatures > 0 ? encounter.recCreatures
                                                    : encounter.maxCreatures;
    if (count <= 0) {
        count = static_cast<std::int32_t>(encounter.creatures.size());
    }
    if (encounter.maxCreatures > 0) {
        count = std::min(count, encounter.maxCreatures);
    }
    ScriptApi api{*this};
    std::set<std::string> singleSpawnUsed;
    std::int32_t spawnedCount = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        const kotor::EncounterCreature& creature =
            encounter.creatures[static_cast<std::size_t>(i) % encounter.creatures.size()];
        if (creature.singleSpawn && singleSpawnUsed.contains(creature.resref)) {
            continue;
        }
        const kotor::EncounterSpawnPoint& point =
            volume.spawnPoints[static_cast<std::size_t>(i) % volume.spawnPoints.size()];
        const std::uint32_t object = spawnScriptCreature(creature.resref, point.position,
                                                         point.bearing);
        const std::size_t entityIndex = api.entityFor(object);
        if (entityIndex == kNone) {
            continue;
        }
        if (encounter.faction > 0) {
            scene_.entities[entityIndex].faction = static_cast<std::uint32_t>(encounter.faction);
        }
        scene_.entities[entityIndex].hostileOverride = false;
        if (script::GameObject* entry = scriptWorld_.get(object); entry != nullptr) {
            entry->attrs["faction"] = static_cast<std::int64_t>(scene_.entities[entityIndex].faction);
            entry->attrs["encounter_creature"] = static_cast<std::int64_t>(1);
            applyCreatureTemplate(*entry, install_, creature.resref, scene_.entities[entityIndex]);
        }
        if (creature.singleSpawn) {
            singleSpawnUsed.insert(creature.resref);
        }
        state.entities.push_back(entityIndex);
        ++spawnedCount;
    }
    state.spawnedCount += spawnedCount;
    state.spawned = state.spawnedCount > 0;
    state.resetTimer = 0.0f;
    if (spawnedCount > 0) {
        state.exhausted = false;
        refreshHostility();
    }
    return spawnedCount;
}

namespace {

bool pointInEncounter(const kotor::EncounterVolume& volume, kotor::Vec3 point)
{
    // Even-odd test on the XY polygon.
    bool inside = false;
    const std::vector<kotor::Vec3>& v = volume.vertices;
    for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) {
        if ((v[i].y > point.y) != (v[j].y > point.y)
            && point.x < (v[j].x - v[i].x) * (point.y - v[i].y) / (v[j].y - v[i].y) + v[i].x) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace

// Encounters spawn when the player (or, for !PlayerOnly, a party member) walks
// into the volume, not at module load. SpawnOption 1 is single shot; a Reset
// encounter re-arms ResetTime seconds after its creatures are dead, Respawns
// times (-1 forever), and spawns again at once if the player is still inside.
void GameSession::updateEncounterVolumes(float dt)
{
    for (std::size_t i = 0; i < encounterStates_.size() && i < scene_.encounters.size(); ++i) {
        EncounterState& state = encounterStates_[i];
        const kotor::EncounterVolume& volume = scene_.encounters[i];
        if (volume.vertices.size() < 3) {
            continue;
        }
        bool inside = pointInEncounter(volume, position_);
        if (!inside && !volume.templateData.playerOnly) {
            for (const kotor::Entity& entity : scene_.entities) {
                if (entity.partyAvatar && !entity.dead && pointInEncounter(volume, entity.position)) {
                    inside = true;
                    break;
                }
            }
        }
        const bool entered = inside && !state.inside;
        state.inside = inside;
        const kotor::EncounterTemplate& data = scene_.encounters[i].templateData;
        const auto enter = [&]() {
            if (!data.active || state.exhausted) {
                return;
            }
            const bool continuous = data.spawnOption == 0;
            if (state.triggered && !(continuous && allDead(scene_, state))) {
                return;
            }
            const std::size_t entityIndex = volume.entity;
            const std::string resref = data.resref;
            const std::int32_t spawned = spawnEncounterWave(i);
            if (spawned > 0) {
                state.triggered = true;
                logEvent("encounter " + resref + " spawned " + std::to_string(spawned));
                runHook("OnEntered", entityIndex, scriptWorld_.pc);
            }
        };
        if (entered) {
            enter();
        }
        if (!state.triggered || !data.reset || !allDead(scene_, state) || state.respawnsRemaining == 0) {
            continue;
        }
        state.resetTimer += dt;
        if (state.resetTimer < state.resetTime) {
            continue;
        }
        state.resetTimer = 0.0f;
        if (state.respawnsRemaining > 0) {
            --state.respawnsRemaining;
        }
        state.triggered = false;
        if (state.inside) {
            enter();
        }
    }
}

// ---- action handlers --------------------------------------------------------

namespace {

int factionOf(const ScriptApi& api, std::uint32_t object)
{
    if (object == api.pc()) {
        return 0;
    }
    const std::size_t index = api.entityFor(object);
    if (index != kNone) {
        return static_cast<int>(api.scene().entities[index].faction);
    }
    return api.session.factionOfObject(object);
}

kotor::Entity* entityOf(ActionCall& call, std::size_t arg, std::size_t* indexOut = nullptr)
{
    ScriptApi api = call.api();
    const std::size_t index = api.entityFor(call.objectArg(arg));
    if (indexOut != nullptr) {
        *indexOut = index;
    }
    return index == kNone ? nullptr : &api.scene().entities[index];
}

// ---- listening and speech
Cells setListening(ActionCall& call)
{
    WorldSim& sim = call.api().sim();
    const std::uint32_t object = call.objectArg(0);
    if (call.intArg(1) != 0) {
        sim.listening.insert(object);
    } else {
        sim.listening.erase(object);
    }
    return ActionCall::none();
}

Cells getIsListening(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().listening.contains(call.objectArg(0)) ? 1 : 0);
}

Cells setListenPattern(ActionCall& call)
{
    auto& patterns = call.api().sim().listenPatterns[call.objectArg(0)];
    const std::string pattern = call.stringArg(1);
    const std::int32_t number = static_cast<std::int32_t>(call.intArg(2));
    for (auto& existing : patterns) {
        if (existing.second == number) {
            existing.first = pattern;
            return ActionCall::none();
        }
    }
    patterns.emplace_back(pattern, number);
    return ActionCall::none();
}

void speak(ActionCall& call, const std::string& text, std::int64_t volume)
{
    ScriptApi api = call.api();
    WorldSim& sim = api.sim();
    const std::uint32_t speaker = call.self();
    api.log("speak " + std::to_string(speaker) + ": " + text);
    // TALKVOLUME_TALK 0, WHISPER 1, SHOUT 2, SILENT_TALK 3, SILENT_SHOUT 4, PARTY 5.
    const bool shout = volume == 2 || volume == 4;
    const float reach = volume == 1 ? kWhisperRadius : kTalkRadius;
    const std::vector<std::uint32_t> listeners(sim.listening.begin(), sim.listening.end());
    for (const std::uint32_t listener : listeners) {
        if (listener == speaker || !api.isAlive(listener)) {
            continue;
        }
        if (volume == 5 && std::find(api.world().party.begin(), api.world().party.end(), listener)
                == api.world().party.end()) {
            continue;
        }
        if (!shout && distance2(api.position(speaker), api.position(listener)) > reach) {
            continue;
        }
        const auto found = sim.listenPatterns.find(listener);
        if (found == sim.listenPatterns.end()) {
            continue;
        }
        for (const auto& [pattern, number] : found->second) {
            if (globMatch(pattern, text)) {
                api.raiseHook(listener, "Dialogue",
                              {{"lastSpeaker", speaker},
                               {"listenPattern", static_cast<std::uint32_t>(number)}});
                break;
            }
        }
    }
}

Cells speakString(ActionCall& call)
{
    speak(call, call.stringArg(0), call.intArg(1));
    return ActionCall::none();
}

// ---- factions
Cells changeToStandardFaction(ActionCall& call)
{
    call.session.setObjectFaction(call.objectArg(0), static_cast<int>(call.intArg(1)));
    return ActionCall::none();
}

Cells changeFaction(ActionCall& call)
{
    ScriptApi api = call.api();
    call.session.setObjectFaction(call.objectArg(0), factionOf(api, call.objectArg(1)));
    return ActionCall::none();
}

Cells changeFactionByFaction(ActionCall& call)
{
    ScriptApi api = call.api();
    const auto from = static_cast<std::uint32_t>(call.intArg(0));
    const int to = static_cast<int>(call.intArg(1));
    for (std::size_t i = 0; i < api.scene().entities.size(); ++i) {
        const kotor::Entity& entity = api.scene().entities[i];
        if (entity.kind == kotor::EntityKind::Creature && entity.faction == from) {
            call.session.setObjectFaction(api.objectFor(i), to);
        }
    }
    return ActionCall::none();
}

Cells getFactionEqual(ActionCall& call)
{
    ScriptApi api = call.api();
    return ActionCall::retInt(factionOf(api, call.objectArg(0)) == factionOf(api, call.objectArg(1)));
}

Cells getStandardFaction(ActionCall& call)
{
    return ActionCall::retInt(factionOf(call.api(), call.objectArg(0)));
}

// GetIsEnemy(target, source=self): the source's standing toward the target.
// Enemy is the engine's hostile standing (< 50, used by combat); friend is 90 and up.
int standing(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t target = call.objectArg(0);
    const std::uint32_t source = call.hasArg(1) ? call.objectArg(1) : call.self();
    return call.session.factions().reputation(factionOf(api, source), factionOf(api, target));
}

Cells getIsEnemy(ActionCall& call)
{
    return ActionCall::retInt(FactionSystem::hostileStanding(standing(call)) ? 1 : 0);
}

Cells getIsFriend(ActionCall& call)
{
    return ActionCall::retInt(standing(call) >= 90 ? 1 : 0);
}

Cells getIsNeutral(ActionCall& call)
{
    const int value = standing(call);
    return ActionCall::retInt(!FactionSystem::hostileStanding(value) && value < 90 ? 1 : 0);
}

// ---- locks, doors, plot
Cells setLocked(ActionCall& call)
{
    // retail 00543240: stores the flag on the door (+0x2c4) or placeable (+0x260), nothing else.
    if (kotor::Entity* entity = entityOf(call, 0); entity != nullptr
        && (entity->kind == kotor::EntityKind::Door || entity->kind == kotor::EntityKind::Placeable)) {
        entity->locked = call.intArg(1) != 0;
    }
    return ActionCall::none();
}

Cells getLocked(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->locked ? 1 : 0);
}

Cells getIsOpen(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->open ? 1 : 0);
}

Cells lockKeyRequired(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->keyRequired ? 1 : 0);
}

Cells lockLockable(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->lockable ? 1 : 0);
}

Cells lockUnlockDc(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr ? entity->openLockDC : 0);
}

Cells lockLockDc(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr ? entity->closeLockDC : 0);
}

Cells getPlotFlag(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->plot ? 1 : 0);
}

Cells setPlotFlag(ActionCall& call)
{
    if (kotor::Entity* entity = entityOf(call, 0); entity != nullptr) {
        entity->plot = call.intArg(1) != 0;
    }
    return ActionCall::none();
}

// DOOR_ACTION_OPEN 0, UNLOCK 1, BASH 2.
Cells doDoorAction(ActionCall& call)
{
    std::size_t index = kNone;
    if (entityOf(call, 0, &index) == nullptr) {
        return ActionCall::none();
    }
    const std::uint32_t actor = call.self();
    switch (call.intArg(1)) {
    case 0: call.session.openDoor(index, actor); break;
    case 1: call.session.unlockDoor(index, actor); break;
    case 2: call.session.bashDoor(index); break;
    default: break;
    }
    return ActionCall::none();
}

Cells getIsDoorActionPossible(ActionCall& call)
{
    std::size_t index = kNone;
    const kotor::Entity* entity = entityOf(call, 0, &index);
    if (entity == nullptr || entity->kind != kotor::EntityKind::Door) {
        return ActionCall::retInt(0);
    }
    switch (call.intArg(1)) {
    case 0: return ActionCall::retInt(!entity->locked || call.session.playerHasKey(entity->keyName));
    case 1: return ActionCall::retInt(entity->locked && (!entity->keyRequired
                                                          || call.session.playerHasKey(entity->keyName)));
    case 2: return ActionCall::retInt(entity->locked && !entity->plot && entity->hitPoints > 0);
    default: return ActionCall::retInt(0);
    }
}

Cells actionInteractObject(ActionCall& call)
{
    std::size_t index = kNone;
    if (entityOf(call, 0, &index) != nullptr) {
        call.session.useObjectImpl(index, false);
    }
    return ActionCall::none();
}

Cells getTransitionTarget(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    if (entity == nullptr || entity->linkedTo.empty()) {
        return ActionCall::retObject(script::kObjectInvalid);
    }
    return ActionCall::retObject(call.session.objectForTag(entity->linkedTo));
}

// ---- traps
Cells isTrapped(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && entity->trapFlag && !entity->trapDisarmed ? 1 : 0);
}

template <bool kotor::Entity::*Flag>
Cells entityFlag(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr && (entity->*Flag) ? 1 : 0);
}

template <std::int32_t kotor::Entity::*Field>
Cells entityInt(ActionCall& call)
{
    const kotor::Entity* entity = entityOf(call, 0);
    return ActionCall::retInt(entity != nullptr ? entity->*Field : 0);
}

Cells setTrapDetectedBy(ActionCall& call)
{
    kotor::Entity* entity = entityOf(call, 0);
    if (entity == nullptr || !entity->trapFlag) {
        return ActionCall::retInt(0);
    }
    entity->trapDetected = true;
    return ActionCall::retInt(1);
}

Cells setTrapDisabled(ActionCall& call)
{
    if (kotor::Entity* entity = entityOf(call, 0); entity != nullptr) {
        entity->trapDisarmed = true;
    }
    return ActionCall::none();
}

Cells nearestTrap(ActionCall& call)
{
    ScriptApi api = call.api();
    const kotor::Vec3 from = api.position(call.objectArg(0));
    const bool detectedOnly = call.intArg(1, 1) != 0;
    float best = 1.0e9f;
    std::size_t found = kNone;
    for (std::size_t i = 0; i < api.scene().entities.size(); ++i) {
        const kotor::Entity& entity = api.scene().entities[i];
        if (!entity.trapFlag || entity.trapDisarmed || (detectedOnly && !entity.trapDetected)) {
            continue;
        }
        const float distance = distance2(from, entity.position);
        if (distance < best) {
            best = distance;
            found = i;
        }
    }
    return ActionCall::retObject(found == kNone ? script::kObjectInvalid : api.objectFor(found));
}

// ---- encounters
std::size_t encounterOf(ActionCall& call, std::size_t arg)
{
    ScriptApi api = call.api();
    const std::size_t entity = api.entityFor(call.objectArg(arg));
    if (entity == kNone) {
        return kNone;
    }
    for (std::size_t i = 0; i < api.scene().encounters.size(); ++i) {
        if (api.scene().encounters[i].entity == entity) {
            return i;
        }
    }
    return kNone;
}

Cells getEncounterActive(ActionCall& call)
{
    const std::size_t i = encounterOf(call, 0);
    return ActionCall::retInt(i != kNone && call.api().scene().encounters[i].templateData.active);
}

Cells setEncounterActive(ActionCall& call)
{
    if (const std::size_t i = encounterOf(call, 1); i != kNone) {
        call.api().scene().encounters[i].templateData.active = call.intArg(0) != 0;
    }
    return ActionCall::none();
}

Cells getEncounterSpawnsMax(ActionCall& call)
{
    const std::size_t i = encounterOf(call, 0);
    return ActionCall::retInt(i != kNone ? call.api().scene().encounters[i].templateData.maxCreatures : 0);
}

Cells setEncounterSpawnsMax(ActionCall& call)
{
    if (const std::size_t i = encounterOf(call, 1); i != kNone) {
        call.api().scene().encounters[i].templateData.maxCreatures = static_cast<std::int32_t>(call.intArg(0));
    }
    return ActionCall::none();
}

Cells getEncounterSpawnsCurrent(ActionCall& call)
{
    const std::size_t i = encounterOf(call, 0);
    if (i == kNone || i >= call.session.encounters().size()) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(call.session.encounters()[i].spawnedCount);
}

Cells getEncounterDifficulty(ActionCall& call)
{
    const std::size_t i = encounterOf(call, 0);
    return ActionCall::retInt(i != kNone ? call.api().scene().encounters[i].templateData.difficulty : 0);
}

Cells setEncounterDifficulty(ActionCall& call)
{
    if (const std::size_t i = encounterOf(call, 1); i != kNone) {
        call.api().scene().encounters[i].templateData.difficulty = static_cast<std::int32_t>(call.intArg(0));
    }
    return ActionCall::none();
}

Cells getIsEncounterCreature(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::size_t entity = api.entityFor(call.objectArg(0));
    for (const EncounterState& state : call.session.encounters()) {
        if (std::find(state.entities.begin(), state.entities.end(), entity) != state.entities.end()) {
            return ActionCall::retInt(1);
        }
    }
    return ActionCall::retInt(0);
}

// ---- cinematic and UI state
FadeState startFade(ActionCall& call, FadeState::Mode mode)
{
    FadeState fade;
    fade.mode = mode;
    fade.wait = static_cast<float>(std::max(0.0, call.floatArg(0)));
    fade.length = static_cast<float>(std::max(0.0, call.floatArg(1)));
    fade.color = {static_cast<float>(call.floatArg(2)), static_cast<float>(call.floatArg(3)),
                  static_cast<float>(call.floatArg(4))};
    // Out starts clear; In starts from the solid colour.
    fade.opacity = mode == FadeState::Mode::In ? 1.0f : 0.0f;
    return fade;
}

Cells fadeOut(ActionCall& call)
{
    call.api().sim().fade = startFade(call, FadeState::Mode::Out);
    return ActionCall::none();
}

Cells fadeIn(ActionCall& call)
{
    call.api().sim().fade = startFade(call, FadeState::Mode::In);
    return ActionCall::none();
}

Cells noClicksFor(ActionCall& call)
{
    call.api().sim().noClicksRemaining =
        std::max(call.api().sim().noClicksRemaining, static_cast<float>(call.floatArg(0)));
    return ActionCall::none();
}

Cells setPlayerRestrictMode(ActionCall& call)
{
    WorldSim& sim = call.api().sim();
    sim.restrictMode = call.intArg(0) != 0;
    sim.restrictHeld = 0.0f;
    return ActionCall::none();
}

Cells getPlayerRestrictMode(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().restrictMode ? 1 : 0);
}

Cells setCustomToken(ActionCall& call)
{
    call.api().sim().customTokens[static_cast<std::int32_t>(call.intArg(0))] = call.stringArg(1);
    return ActionCall::none();
}

Cells setAreaUnescapable(ActionCall& call)
{
    call.api().sim().areaUnescapable = call.intArg(0) != 0;
    return ActionCall::none();
}

Cells getAreaUnescapable(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().areaUnescapable ? 1 : 0);
}

Cells setCameraMode(ActionCall& call)
{
    call.api().sim().cameraMode = static_cast<std::int32_t>(call.intArg(1));
    return ActionCall::none();
}

// Anticlockwise degrees from east, the player's yaw convention.
Cells setCameraFacing(ActionCall& call)
{
    ScriptApi api = call.api();
    api.sim().cameraFacing = static_cast<float>(call.floatArg(0));
    api.sim().cameraFacingSet = true;
    api.setPosition(api.pc(), api.position(api.pc()), api.sim().cameraFacing / kDegrees);
    return ActionCall::none();
}

Cells setLockOrientationInDialog(ActionCall& call)
{
    WorldSim& sim = call.api().sim();
    if (call.intArg(1) != 0) {
        sim.lockOrientation.insert(call.objectArg(0));
    } else {
        sim.lockOrientation.erase(call.objectArg(0));
    }
    return ActionCall::none();
}

Cells setFacing(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t self = call.self();
    api.setPosition(self, api.position(self), static_cast<float>(call.floatArg(0)) / kDegrees);
    return ActionCall::none();
}

Cells setFacingPoint(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t self = call.self();
    const kotor::Vec3 from = api.position(self);
    const kotor::Vec3 to = call.vectorArg(0);
    api.setPosition(self, from, std::atan2(to.y - from.y, to.x - from.x));
    return ActionCall::none();
}

Cells faceObjectAwayFrom(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t self = call.objectArg(0);
    const kotor::Vec3 from = api.position(self);
    const kotor::Vec3 away = api.position(call.objectArg(1));
    api.setPosition(self, from, std::atan2(from.y - away.y, from.x - away.x));
    return ActionCall::none();
}

// ---- map
Cells revealMap(ActionCall& call)
{
    ScriptApi api = call.api();
    MapRevealRecord record;
    record.module = call.session.module();
    record.point = call.vectorArg(0);
    record.radius = static_cast<std::int32_t>(call.intArg(1, -1));
    api.sim().mapReveals.push_back(std::move(record));
    return ActionCall::none();
}

Cells setMapPinEnabled(ActionCall& call)
{
    ScriptApi api = call.api();
    const script::GameObject* pin = call.world.get(call.objectArg(0));
    std::string tag = pin != nullptr ? pin->tag : std::string{};
    if (tag.empty()) {
        if (const kotor::Entity* entity = entityOf(call, 0); entity != nullptr) {
            tag = entity->tag;
        }
    }
    if (!tag.empty()) {
        if (call.intArg(1) != 0) {
            api.sim().mapPinsEnabled.insert(lowerText(tag));
        } else {
            api.sim().mapPinsEnabled.erase(lowerText(tag));
        }
    }
    return ActionCall::none();
}

// ---- galaxy map
bool validPlanet(std::int64_t planet)
{
    return planet >= 0 && static_cast<std::size_t>(planet) < kPlanetSlots;
}

Cells showGalaxyMap(ActionCall& call)
{
    GalaxyMapState& map = call.api().sim().galaxy;
    map.open = true;
    map.highlighted = static_cast<std::int32_t>(call.intArg(0));
    return ActionCall::none();
}

Cells setPlanetSelectable(ActionCall& call)
{
    if (validPlanet(call.intArg(0))) {
        call.api().sim().galaxy.selectable[static_cast<std::size_t>(call.intArg(0))] = call.intArg(1) != 0;
    }
    return ActionCall::none();
}

Cells getPlanetSelectable(ActionCall& call)
{
    return ActionCall::retInt(validPlanet(call.intArg(0))
        && call.api().sim().galaxy.selectable[static_cast<std::size_t>(call.intArg(0))]);
}

Cells setPlanetAvailable(ActionCall& call)
{
    if (validPlanet(call.intArg(0))) {
        call.api().sim().galaxy.available[static_cast<std::size_t>(call.intArg(0))] = call.intArg(1) != 0;
    }
    return ActionCall::none();
}

Cells getPlanetAvailable(ActionCall& call)
{
    return ActionCall::retInt(validPlanet(call.intArg(0))
        && call.api().sim().galaxy.available[static_cast<std::size_t>(call.intArg(0))]);
}

Cells getSelectedPlanet(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().galaxy.selected);
}

} // namespace

void registerWorldStateActions(HandlerTable& table)
{
    table.add(10, setFacing);
    table.add(14, setAreaUnescapable);
    table.add(15, getAreaUnescapable);
    table.add(45, setCameraFacing);
    table.add(58, setPlayerRestrictMode);
    table.add(83, getPlayerRestrictMode);
    table.add(143, setFacingPoint);
    table.add(172, getFactionEqual);
    table.add(173, changeFaction);
    table.add(174, getIsListening);
    table.add(175, setListening);
    table.add(176, setListenPattern);
    table.add(198, getTransitionTarget);
    table.add(221, speakString);
    table.add(235, getIsEnemy);
    table.add(236, getIsFriend);
    table.add(237, getIsNeutral);
    table.add(276, getEncounterActive);
    table.add(277, setEncounterActive);
    table.add(278, getEncounterSpawnsMax);
    table.add(279, setEncounterSpawnsMax);
    table.add(280, getEncounterSpawnsCurrent);
    table.add(284, setCustomToken);
    table.add(296, setEncounterDifficulty);
    table.add(297, getEncounterDifficulty);
    table.add(324, setLocked);
    table.add(325, getLocked);
    table.add(329, actionInteractObject);
    table.add(337, getIsDoorActionPossible);
    table.add(338, doDoorAction);
    table.add(386, setMapPinEnabled);
    table.add(409, getIsEncounterCreature);
    table.add(412, changeToStandardFaction);
    table.add(443, getIsOpen);
    table.add(455, getPlotFlag);
    table.add(456, setPlotFlag);
    table.add(488, nearestTrap);
    table.add(504, setCameraMode);
    table.add(505, setLockOrientationInDialog);
    table.add(515, revealMap);
    table.add(527, entityFlag<&kotor::Entity::trapDisarmable>);
    table.add(528, entityFlag<&kotor::Entity::trapDetectable>);
    table.add(530, entityFlag<&kotor::Entity::trapFlag>);
    table.add(532, entityFlag<&kotor::Entity::trapOneShot>);
    table.add(535, entityInt<&kotor::Entity::trapDisarmDC>);
    table.add(536, entityInt<&kotor::Entity::trapDetectDC>);
    table.add(537, lockKeyRequired);
    table.add(539, lockLockable);
    table.add(540, lockUnlockDc);
    table.add(541, lockLockDc);
    table.add(550, setTrapDetectedBy);
    table.add(551, isTrapped);
    table.add(553, faceObjectAwayFrom);
    table.add(555, setTrapDisabled);
    table.add(719, fadeIn);
    table.add(720, fadeOut);
    table.add(737, changeFactionByFaction);
    table.add(713, getStandardFaction);
    table.add(739, showGalaxyMap);
    table.add(740, setPlanetSelectable);
    table.add(741, getPlanetSelectable);
    table.add(742, setPlanetAvailable);
    table.add(743, getPlanetAvailable);
    table.add(744, getSelectedPlanet);
    table.add(759, noClicksFor);
}

// ---- per-tick ---------------------------------------------------------------

void tickWorldState(ScriptApi api, float dt)
{
    WorldSim& sim = api.sim();
    GameSession& session = api.session;

    FadeState& fade = sim.fade;
    if (fade.active()) {
        fade.elapsed += dt;
        const float into = fade.elapsed - fade.wait;
        const float progress = into <= 0.0f ? 0.0f
            : (fade.length > 0.0f ? std::min(1.0f, into / fade.length) : 1.0f);
        if (fade.mode == FadeState::Mode::Out) {
            fade.opacity = progress;
            // A black screen no script ever lifts is cleared rather than kept forever.
            if (into > fade.length + kFadeHoldWatchdogSeconds) {
                api.log("fade: held solid too long, cleared");
                fade = FadeState{};
            }
        } else {
            fade.opacity = 1.0f - progress;
            if (progress >= 1.0f) {
                fade = FadeState{};
            }
        }
    }

    sim.noClicksRemaining = std::max(0.0f, sim.noClicksRemaining - dt);

    if (sim.restrictMode) {
        // Time spent in a conversation or movie is the script's own doing; time
        // spent with nothing driving the cutscene is a stuck script.
        const bool driven = api.dialogueActive() || session.moviePlaying()
            || !api.actor(api.pc()).queue.empty();
        sim.restrictHeld = driven ? 0.0f : sim.restrictHeld + dt;
        if (sim.restrictHeld >= kRestrictWatchdogSeconds) {
            api.log("restrict mode: released by watchdog");
            sim.restrictMode = false;
            sim.restrictHeld = 0.0f;
        }
    }

    // Passive detection of nearby traps, once a second (Awareness roll each time).
    sim.trapScanTimer += dt;
    if (sim.trapScanTimer >= 1.0f) {
        sim.trapScanTimer = 0.0f;
        const kotor::Vec3 pc = api.position(api.pc());
        for (std::size_t i = 0; i < api.scene().entities.size(); ++i) {
            const kotor::Entity& entity = api.scene().entities[i];
            if (entity.trapFlag && entity.trapDetectable && !entity.trapDetected
                && !entity.trapDisarmed && distance2(pc, entity.position) <= kTrapDetectRadius) {
                session.detectTrap(i);
            }
        }
    }
}


// ---- family gate: --family-test worldstate ---------------------------------
//
// Every assertion is a before/after state check on shipped modules, driven
// through the script VM (GameSession::action) or the public interaction API.
namespace {

using Args = std::vector<std::vector<script::Value>>;

std::int64_t callAction(GameSession& game, std::uint16_t id, Args args)
{
    script::Vm vm({});
    const auto result = game.action(id, args, game.scriptWorld(), vm);
    return result.has_value() && !result->empty() ? result->front().asInt() : -9999;
}

std::vector<script::Value> obj(std::uint32_t id) { return {script::Value::makeObject(id)}; }
std::vector<script::Value> num(std::int64_t v) { return {script::Value::makeInt(v)}; }
std::vector<script::Value> real(double v) { return {script::Value::makeFloat(v)}; }
std::vector<script::Value> text(std::string v) { return {script::Value::makeString(std::move(v))}; }

bool logHas(const GameSession& game, const std::string& needle)
{
    for (const std::string& line : game.scriptLog()) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void tickFor(GameSession& game, float seconds)
{
    for (float t = 0.0f; t < seconds; t += 0.05f) {
        game.update(0.05f, InputState{});
    }
}

// The opening cutscene of a module chains conversations; let it play out (ending
// each one) until the session has been quiet for a few seconds.
void settleIntro(GameSession& game)
{
    float quiet = 0.0f;
    for (int step = 0; step < 400 && quiet < 4.0f; ++step) {
        if (game.dialogue().active) {
            game.endDialogue();
            quiet = 0.0f;
        }
        tickFor(game, 0.25f);
        quiet += game.dialogue().active ? -quiet : 0.25f;
    }
    game.endDialogue();
    game.releaseControl();
}

kotor::Vec3 triggerCentre(GameSession& game, std::size_t entity)
{
    kotor::Vec3 sum{};
    std::size_t count = 0;
    for (const kotor::TriggerVolume& volume : game.scene().triggers) {
        if (volume.entity == entity) {
            for (const kotor::Vec3& vertex : volume.vertices) {
                sum = sum + vertex;
                ++count;
            }
        }
    }
    return count > 0 ? sum * (1.0f / static_cast<float>(count)) : kotor::Vec3{};
}

kotor::Vec3 encounterCentre(const kotor::EncounterVolume& volume)
{
    kotor::Vec3 sum{};
    for (const kotor::Vec3& vertex : volume.vertices) {
        sum = sum + vertex;
    }
    return volume.vertices.empty() ? kotor::Vec3{}
                                   : sum * (1.0f / static_cast<float>(volume.vertices.size()));
}

} // namespace

int runWorldStateFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("worldstate: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::string installPath = game.installPath();
    constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    // ---- the opening conversation, played the way a player would (module end_m01aa) ----
    // Every line is advanced with option 0 and the conversation is never aborted;
    // after it ends on its own it must not start again.
    {
        GameSession intro(installPath, "end_m01aa");
        int starts = 0;
        bool wasActive = false;
        float clock = 0.0f;
        float lastAdvance = 0.0f;
        float stageDoneAt = -1.0f;
        int nagsAfterStage = 0;
        for (int step = 0; step < 3600; ++step) { // 180 s of play
            intro.update(0.05f, InputState{});
            clock += 0.05f;
            const bool active = intro.dialogue().active;
            if (active && !wasActive) {
                ++starts;
                const auto& log = intro.scriptLog();
                std::string recent;
                for (std::size_t i = log.size() > 3 ? log.size() - 3 : 0; i < log.size(); ++i) {
                    recent += " [" + log[i] + "]";
                }
                if (stageDoneAt >= 0.0f && recent.find("end_trask01 entry=51") != std::string::npos) {
                    ++nagsAfterStage;
                }
                logInfo(concat("worldstate: intro conversation #", starts, " t=", clock, " owner=",
                               intro.dialogue().npcTag, " lines before:", recent));
            }
            if (!active && wasActive) {
                logInfo(concat("worldstate: intro conversation ended on its own at t=", clock));
            }
            wasActive = active;
            if (active && clock - lastAdvance >= 0.4f) {
                lastAdvance = clock;
                intro.chooseDialogueOption(0);
            }
            // The tutorial stage Trask nags about ends when the player opens the
            // footlocker (the module's chest script advances END_TRASK_DLG).
            if (stageDoneAt < 0.0f && clock >= 30.0f && !active) {
                const std::size_t locker = intro.findEntity("end_locker01");
                if (locker != kMissing) {
                    intro.setPositionForTest(intro.scene().entities[locker].position);
                    intro.useObject(locker);
                    intro.takeAllLoot(); // taking from the footlocker fires its OnInvDisturbed
                    stageDoneAt = clock;
                    logInfo(concat("worldstate: intro locker used at t=", clock));
                    wasActive = intro.dialogue().active;
                }
            }
        }
        logInfo(concat("worldstate: intro starts=", starts, " stage_done=", stageDoneAt >= 0.0f ? 1 : 0,
                       " nags_after_stage=", nagsAfterStage, " still_active=", wasActive ? 1 : 0));
        check(starts >= 1 && stageDoneAt >= 0.0f && !wasActive && nagsAfterStage == 0,
              "intro_nag_stage_advances_with_progress");
    }

    // ---- fades and control lock (module end_m01aa) ----
    {
        settleIntro(game); // the module's own cutscene and entry fade-in finish
        check(!game.fade().active() && game.fade().opacity == 0.0f, "fade_clear_at_start");
        callAction(game, 720, {real(0.5), real(1.0), real(0.0), real(0.0), real(0.0)});
        check(game.fade().mode == FadeState::Mode::Out, "fade_out_set");
        tickFor(game, 0.3f);
        check(game.fade().opacity == 0.0f, "fade_out_waits");
        tickFor(game, 0.7f);
        const float mid = game.fade().opacity;
        check(mid > 0.1f && mid < 0.9f, "fade_out_mid");
        tickFor(game, 1.0f);
        check(game.fade().opacity == 1.0f, "fade_out_solid");
        callAction(game, 719, {real(0.0), real(0.5), real(0.0), real(0.0), real(0.0)});
        check(game.fade().mode == FadeState::Mode::In && game.fade().opacity > 0.9f, "fade_in_set");
        tickFor(game, 0.8f);
        check(!game.fade().active() && game.fade().opacity == 0.0f, "fade_in_clears");
        callAction(game, 720, {real(0.0), real(0.0), real(1.0), real(0.0), real(0.0)});
        tickFor(game, 0.1f);
        check(game.fade().opacity == 1.0f && game.fade().color[0] == 1.0f, "fade_out_colour");
        game.releaseControl();
        check(!game.fade().active(), "release_clears_fade");

        // A module without a running cutscene or tutorial nag: a conversation
        // legitimately counts as the cutscene driving restrict mode.
        GameSession quiet(installPath, "korr_m35aa");
        settleIntro(quiet);
        check(!quiet.controlLocked(), "control_free");
        callAction(quiet, 759, {real(0.5)});
        const bool lockedNow = quiet.controlLocked();
        const bool clicked = quiet.interact();
        logInfo(concat("worldstate: lockedNow=", lockedNow ? 1 : 0, " clicked=", clicked ? 1 : 0));
        check(lockedNow && !clicked, "no_clicks_locks");
        tickFor(quiet, 0.7f);
        check(!quiet.controlLocked(), "no_clicks_expires");
        callAction(quiet, 58, {num(1)});
        check(callAction(quiet, 83, {obj(quiet.scriptWorld().pc)}) == 1 && quiet.controlLocked(),
              "restrict_mode_set");
        tickFor(quiet, 125.0f);
        logInfo(concat("worldstate: after watchdog wait: locked=", quiet.controlLocked() ? 1 : 0,
                       " dialogue=", quiet.dialogue().active ? 1 : 0,
                       " movie=", quiet.moviePlaying() ? 1 : 0, " pcQueue=",
                       ScriptApi{quiet}.actor(ScriptApi{quiet}.pc()).queue.size(), " firstKind=",
                       ScriptApi{quiet}.actor(ScriptApi{quiet}.pc()).queue.empty()
                           ? -1
                           : static_cast<int>(ScriptApi{quiet}.actor(ScriptApi{quiet}.pc()).queue.front().kind)));
        check(!quiet.controlLocked(), "restrict_watchdog_releases");
        callAction(quiet, 58, {num(1)});
        quiet.releaseControl();
        check(!quiet.controlLocked(), "release_control");
    }

    // ---- factions: a friendly creature becomes an enemy and fights ----
    {
        const std::size_t trask = game.findEntity("end_bridgerep1");
        check(trask != kMissing, "fixture_creature_present");
        if (trask != kMissing) {
            const std::uint32_t pc = game.scriptWorld().pc;
            const std::uint32_t creature = game.objectForEntityIndex(trask);
            check(callAction(game, 235, {obj(pc), obj(creature)}) == 0, "not_enemy_before");
            check(!game.scene().entities[trask].hostile, "not_hostile_before");
            callAction(game, 412, {obj(creature), num(1)}); // STANDARD_FACTION_HOSTILE_1
            check(callAction(game, 713, {obj(creature)}) == 1, "standard_faction_set");
            check(callAction(game, 235, {obj(pc), obj(creature)}) == 1, "is_enemy_after");
            check(game.scene().entities[trask].hostile, "hostile_after");
            const kotor::Vec3 near = game.scene().entities[trask].position;
            game.setPositionForTest({near.x + 1.2f, near.y, near.z});
            const std::int32_t hpBefore = game.playerHp();
            bool fought = false;
            for (int step = 0; step < 240 && !fought; ++step) { // up to 12 s
                tickFor(game, 0.05f);
                fought = game.playerHp() < hpBefore || game.playerDead()
                    || callAction(game, 320, {obj(creature)}) == 1; // GetIsInCombat
            }
            logInfo(concat("worldstate: fight hp ", hpBefore, " -> ", game.playerHp(),
                           " dialogue=", game.dialogue().active ? 1 : 0,
                           " locked=", game.controlLocked() ? 1 : 0));
            check(fought, "converted_creature_fights");
            callAction(game, 412, {obj(creature), num(2)}); // STANDARD_FACTION_FRIENDLY_1
            check(callAction(game, 235, {obj(pc), obj(creature)}) == 0
                      && !game.scene().entities[trask].hostile,
                  "friendly_again");
        }
    }

    // ---- locked door, skill and OnFailToOpen (end_m01aa) ----
    {
        const std::size_t door = game.findEntity("end_door02");
        check(door != kMissing && game.scene().entities[door].locked
                  && !game.scene().entities[door].keyRequired,
              "pickable_door_data");
        if (door != kMissing) {
            game.setPositionForTest(game.scene().entities[door].position);
            check(callAction(game, 325, {obj(game.objectForEntityIndex(door))}) == 1, "get_locked");
            check(game.openDoor(door) == UseOutcome::Locked, "locked_door_refuses");
            check(game.scene().entities[door].locked && !game.scene().entities[door].open,
                  "locked_door_stays_shut");
            check(logHas(game, "OnFailToOpen end_door02"), "fail_to_open_ran");
            check(!game.unlockDoor(door), "untrained_security_fails");
            game.character().skills["Security"] = 6;
            check(game.unlockDoor(door), "security_unlocks");
            check(!game.scene().entities[door].locked, "unlocked_by_skill");
            check(game.openDoor(door) == UseOutcome::Opened && game.scene().entities[door].open,
                  "opens_after_unlock");
            callAction(game, 324, {obj(game.objectForEntityIndex(door)), num(1)});
            check(game.scene().entities[door].locked, "set_locked");
        }
        const std::size_t keyed = game.findEntity("end_door05");
        if (keyed != kMissing && game.scene().entities[keyed].keyRequired) {
            game.character().skills["Security"] = 50;
            check(!game.unlockDoor(keyed), "key_required_cannot_be_picked");
        }
    }

    // ---- galaxy map round trip ----
    {
        check(callAction(game, 743, {num(3)}) == 0 && callAction(game, 741, {num(3)}) == 0,
              "planet_off_at_start");
        callAction(game, 742, {num(3), num(1)});
        callAction(game, 740, {num(3), num(1)});
        check(callAction(game, 743, {num(3)}) == 1 && callAction(game, 741, {num(3)}) == 1,
              "planet_available_selectable");
        check(!game.selectPlanet(3), "select_needs_open_map");
        callAction(game, 739, {num(3)});
        check(game.galaxyMapOpen() && game.galaxyMap().highlighted == 3, "map_open");
        check(!game.selectPlanet(5), "unavailable_planet_refused");
        check(callAction(game, 744, {}) == -1, "no_selection_yet");
        check(game.selectPlanet(3), "select_planet");
        check(callAction(game, 744, {}) == 3, "get_selected_planet");
        check(!game.galaxyMapOpen(), "map_closed_after_select");
        check(!game.galaxyPlanetNames().empty(), "planetary_2da_names");
    }

    // ---- map data ----
    {
        callAction(game, 515, {{script::Value::makeFloat(1.0), script::Value::makeFloat(2.0),
                                script::Value::makeFloat(0.0)},
                               num(5)});
        check(game.mapRevealed({1.0f, 2.0f, 0.0f}) && !game.mapRevealed({60.0f, 60.0f, 0.0f}),
              "reveal_map_radius");
    }

    // ---- locked transition door (module danm14ad) ----
    {
        GameSession other(installPath, "danm14ad");
        const std::size_t door = other.findEntity("dan14ad_door02");
        check(door != kMissing, "transition_door_present");
        if (door != kMissing) {
            const kotor::Entity& entity = other.scene().entities[door];
            check(entity.locked && entity.keyRequired && !entity.linkedModule.empty()
                      && !entity.keyName.empty(),
                  "transition_door_data");
            const std::string key = entity.keyName;
            other.setPositionForTest(entity.position);
            check(other.useObject(door) == UseOutcome::Locked, "locked_transition_refused");
            check(other.module() == "danm14ad", "locked_door_does_not_transition");
            other.character().skills["Security"] = 50;
            check(!other.unlockDoor(door), "skill_cannot_replace_key");
            check(other.addInventoryItem(key, 0, 1), "key_added");
            check(other.useObject(door) == UseOutcome::Transition, "key_opens_transition");
            check(other.module() != "danm14ad", "module_changed_after_key");
        }
    }

    // ---- key door (module korr_m35aa) ----
    {
        GameSession other(installPath, "korr_m35aa");
        const std::size_t door = other.findEntity("k35_door_uthar");
        check(door != kMissing, "key_door_present");
        if (door != kMissing) {
            other.setPositionForTest(other.scene().entities[door].position);
            const std::string key = other.scene().entities[door].keyName;
            check(!key.empty() && other.scene().entities[door].locked, "key_door_data");
            check(other.openDoor(door) == UseOutcome::Locked, "no_key_no_open");
            check(other.addInventoryItem(key, 0, 1), "key_item_added");
            check(other.openDoor(door) == UseOutcome::Opened && other.scene().entities[door].open
                      && !other.scene().entities[door].locked,
                  "key_opens_door");
        }
    }

    // ---- skill door, bash and traps (module korr_m37aa) ----
    {
        GameSession other(installPath, "korr_m37aa");
        std::size_t first = kMissing;
        std::size_t second = kMissing;
        for (std::size_t i = 0; i < other.scene().entities.size(); ++i) {
            if (other.scene().entities[i].tag == "k37_doorlocked") {
                (first == kMissing ? first : second) = i;
            }
        }
        check(first != kMissing, "skill_door_present");
        if (first != kMissing) {
            const kotor::Entity& entity = other.scene().entities[first];
            check(entity.locked && !entity.keyRequired && entity.openLockDC >= 26, "skill_door_data");
            other.setPositionForTest(entity.position);
            other.character().skills["Security"] = 2;
            bool everOpened = false;
            for (int attempt = 0; attempt < 40; ++attempt) {
                everOpened = everOpened || other.unlockDoor(first);
            }
            check(!everOpened, "low_security_never_beats_dc");
            other.character().skills["Security"] = 40;
            check(other.unlockDoor(first) && !other.scene().entities[first].locked,
                  "high_security_beats_dc");
        }
        if (second != kMissing) {
            kotor::Entity& entity = other.scene().entities[second];
            other.setPositionForTest(entity.position);
            check(entity.locked && !entity.plot, "bash_door_data");
            // The shipped door has Hardness 5 against a 1d4 hit; a fixture removes
            // the hardness and most hit points so the bash path itself is exercised.
            entity.hardness = 0;
            entity.hitPoints = 1;
            check(other.bashDoor(second) && other.scene().entities[second].hitPoints == 0
                      && !other.scene().entities[second].locked && other.scene().entities[second].open,
                  "bash_opens_door");
        }
        // Trap triggers: one that can be found and disarmed, and one that springs.
        std::size_t trapA = kMissing;
        std::size_t trapB = kMissing;
        for (std::size_t i = 0; i < other.scene().entities.size(); ++i) {
            const kotor::Entity& candidate = other.scene().entities[i];
            if (candidate.kind != kotor::EntityKind::Trigger || !candidate.trapFlag) {
                continue;
            }
            if (trapA == kMissing && candidate.trapDetectable && candidate.trapDisarmable) {
                trapA = i;
            } else if (trapB == kMissing) {
                trapB = i;
            }
        }
        check(trapA != kMissing && trapB != kMissing, "traps_present");
        if (trapA != kMissing && trapB != kMissing) {
            const kotor::Entity& a = other.scene().entities[trapA];
            check(a.trapDetectDC > 0 && a.trapDisarmDC > 0, "trap_data");
            check(!other.disarmTrap(trapA), "cannot_disarm_undetected");
            other.character().skills["Awareness"] = 40;
            check(other.detectTrap(trapA) && other.scene().entities[trapA].trapDetected,
                  "trap_detected");
            other.character().skills["Security"] = 40;
            check(other.disarmTrap(trapA) && other.scene().entities[trapA].trapDisarmed,
                  "trap_disarmed");
            other.setPositionForTest(triggerCentre(other, trapA));
            tickFor(other, 0.3f);
            check(!other.scene().entities[trapA].trapSprung, "disarmed_trap_stays_quiet");
            other.setPositionForTest(triggerCentre(other, trapB));
            tickFor(other, 0.3f);
            check(other.scene().entities[trapB].trapSprung, "armed_trap_springs");
        }
    }

    // ---- encounters spawn on entry (module tar_m03ab) ----
    {
        GameSession other(installPath, "tar_m03ab");
        std::int32_t atLoad = 0;
        for (const EncounterState& state : other.encounters()) {
            atLoad += state.spawnedCount;
        }
        check(!other.encounters().empty() && atLoad == 0, "no_spawn_at_load");
        other.setPositionForTest({-900.0f, -900.0f, 0.0f}); // well outside every volume
        tickFor(other, 1.0f);
        std::int32_t afterIdle = 0;
        for (const EncounterState& state : other.encounters()) {
            afterIdle += state.spawnedCount;
        }
        check(afterIdle == 0 || other.encounters().empty(), "no_spawn_while_player_outside");
        std::size_t volume = kMissing;
        for (std::size_t i = 0; i < other.scene().encounters.size(); ++i) {
            const kotor::EncounterTemplate& data = other.scene().encounters[i].templateData;
            if (data.active && data.recCreatures > 0 && !data.creatures.empty()
                && !other.scene().encounters[i].spawnPoints.empty()) {
                volume = i;
                break;
            }
        }
        check(volume != kMissing, "encounter_volume_found");
        if (volume != kMissing) {
            const kotor::EncounterTemplate data = other.scene().encounters[volume].templateData;
            other.setPositionForTest(encounterCentre(other.scene().encounters[volume]));
            tickFor(other, 0.3f);
            const EncounterState& state = other.encounters()[volume];
            const std::int32_t expected = data.maxCreatures > 0
                ? std::min(data.recCreatures, data.maxCreatures)
                : data.recCreatures;
            logInfo(concat("worldstate: encounter ", data.resref, " spawned=", state.spawnedCount,
                           " expected=", expected));
            check(state.spawnedCount == expected, "retail_spawn_count");
            bool fromUtc = !state.entities.empty();
            for (const std::size_t entity : state.entities) {
                const kotor::Entity& creature = other.scene().entities[entity];
                fromUtc = fromUtc && creature.hitPoints > 0 && creature.maxHitPoints > 0
                    && !creature.templateResref.empty();
                check(creature.faction == static_cast<std::uint32_t>(data.faction), "encounter_faction");
            }
            check(fromUtc, "creatures_from_utc");
            const std::int32_t once = other.encounters()[volume].spawnedCount;
            tickFor(other, 0.5f);
            check(other.encounters()[volume].spawnedCount == once, "no_respawn_while_inside");
        }
    }

    // ---- custom tokens in a real dialogue (module lev_m40aa) ----
    {
        GameSession other(installPath, "lev_m40aa");
        // The shipped conversation is bound to the first creature that has none of
        // its own; the dialogue file, its token and its text are all real data.
        std::size_t speaker = kMissing;
        for (std::size_t i = 0; i < other.scene().entities.size(); ++i) {
            const kotor::Entity& candidate = other.scene().entities[i];
            if (candidate.kind == kotor::EntityKind::Creature && candidate.conversation.empty()) {
                speaker = i;
                break;
            }
        }
        check(speaker != kMissing, "token_dialogue_present");
        if (speaker != kMissing) {
            other.scene().entities[speaker].conversation = "lev40_brigcompn";
            callAction(other, 284, {num(31), text("7")});
            check(other.substituteTokens("(<CUSTOM31> spike(s))") == "(7 spike(s))", "token_substituted");
            other.character().skills["Computer Use"] = 30;
            check(other.addInventoryItem("g_i_progspike01", 0, 12), "spikes_added");
            check(other.startDialogue(speaker), "dialogue_started");
            bool replaced = false;
            bool leftover = false;
            for (int step = 0; step < 12 && other.dialogue().active; ++step) {
                std::string all = other.dialogue().text;
                for (const DialogueOption& option : other.dialogue().options) {
                    all += " | " + option.text;
                }
                logInfo(concat("worldstate: dialogue step ", step, ": ", all));
                const std::size_t unit = all.find("spike(s)");
                replaced = replaced
                    || (unit != std::string::npos && unit >= 2
                        && std::isdigit(static_cast<unsigned char>(all[unit - 2])) != 0);
                leftover = leftover || all.find("<CUSTOM") != std::string::npos;
                if (other.dialogue().options.empty()) {
                    break;
                }
                if (replaced) {
                    break;
                }
                std::size_t pick = 0;
                for (std::size_t i = 0; i < other.dialogue().options.size(); ++i) {
                    if (other.dialogue().options[i].text.find("Slice") != std::string::npos) {
                        pick = i;
                        break;
                    }
                }
                other.chooseDialogueOption(pick);
            }
            check(replaced, "token_in_dialogue_line");
            check(!leftover, "no_raw_tokens_in_dialogue");
        }
    }

    logInfo(ok ? "worldstate: PASS" : "worldstate: FAIL");
    return ok ? 0 : 10;
}

} // namespace ke
