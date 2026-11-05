// Unified combat: rounds, attack resolution, the damage path, perception, talents
// and the queries the shipped k_ai_master scripts make. Every creature, the PC
// included, fights through the same action and round code. Addresses are retail;
// docs/recon/63_WORLD_SIM.md is the layer's contract.
#include "game/ScriptHost.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "game/Pathfinding.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ke {

namespace {

using Cells = std::optional<std::vector<script::Value>>;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr std::uint32_t kInvalid = script::kObjectInvalid;

constexpr float kRoundSeconds = 3.0f;       // retail 004d5f70: round length 3000 ms
constexpr float kCombatStateSeconds = 8.0f; // retail 004f2610: +0x4e4 = 8000 ms
constexpr float kPerceptionInterval = 1.0f; // Retail slices this per update; assumed 1 s.
constexpr float kMeleeReach = 2.4f;         // Retail reach uses appearance hitdist; not resolved.
constexpr float kMeleeStop = 1.6f;
constexpr float kAttackStuckGiveUp = 3.0f;
constexpr std::int32_t kActionAttackObject = 3;
constexpr std::int32_t kResultHit = 1;
constexpr std::int32_t kResultCritical = 2;
constexpr std::int32_t kResultAutomatic = 3;
constexpr std::int32_t kResultMiss = 4;
constexpr std::int32_t kResultDeflected = 8;
constexpr std::int32_t kResultReflected = 9;
constexpr std::int32_t kDamageUniversal = 8;

int modifier(std::int32_t score)
{
    return static_cast<int>(std::floor((static_cast<double>(score) - 10.0) / 2.0));
}

} // namespace

// Friend of GameSession: everything here needs the player's private state.
struct CombatOps {
    struct Profile {
        std::int32_t attack = 0;
        std::int32_t dice = 1;
        std::int32_t die = 2;
        std::int32_t flat = 0;
        std::int32_t minDamage = 1;
        std::int32_t maxDamage = INT32_MAX;
        std::int32_t threat = 20;
        std::int32_t critMult = 2;
        std::int32_t sneakDice = 0;
        bool ranged = false;
    };

    GameSession& s;

    int gate();

    ScriptApi api() const { return ScriptApi{s}; }
    kotor::Entity* entity(std::uint32_t object) const
    {
        const std::size_t index = api().entityFor(object);
        return index != kNone ? &s.scene_.entities[index] : nullptr;
    }
    bool isPc(std::uint32_t object) const { return object == s.scriptWorld_.pc; }

    // The creature the player steers: no AI script runs for it. retail 004d4620
    bool playerControlled(std::uint32_t object) const
    {
        if (isPc(object)) {
            return s.leaderTag_ == "PC";
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr && found->partyAvatar && found->tag == s.leaderTag_;
    }
    bool inParty(std::uint32_t object) const
    {
        if (isPc(object)) {
            return true;
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr && found->partyAvatar;
    }
    std::string nameOf(std::uint32_t object) const
    {
        if (isPc(object)) {
            return "player";
        }
        const kotor::Entity* found = entity(object);
        if (found == nullptr) {
            return "object " + std::to_string(object);
        }
        if (found->partyAvatar || found->name.empty()) {
            return found->tag;
        }
        return found->name;
    }
    std::string tagOf(std::uint32_t object) const
    {
        const kotor::Entity* found = entity(object);
        return found != nullptr ? found->tag : nameOf(object);
    }

    std::int32_t roll(std::int32_t sides) const
    {
        return 1 + static_cast<std::int32_t>(s.random() % static_cast<std::uint32_t>(std::max(1, sides)));
    }

    std::int32_t hitPoints(std::uint32_t object) const
    {
        if (isPc(object)) {
            return s.playerHp_;
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr ? found->hitPoints : 0;
    }

    int factionOf(std::uint32_t object) const
    {
        if (inParty(object)) {
            return 0; // Party members stand in the player's faction.
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr ? static_cast<int>(found->faction) : s.factionOfObject(object);
    }

    // retail 0057cb80: same faction is 100, otherwise the repute table.
    int reputation(std::uint32_t source, std::uint32_t target) const
    {
        const int a = factionOf(source);
        const int b = factionOf(target);
        const kotor::Entity* first = entity(source);
        const kotor::Entity* second = entity(target);
        if ((first != nullptr && first->hostileOverride && inParty(target))
            || (second != nullptr && second->hostileOverride && inParty(source))) {
            return 0;
        }
        if (a == b) {
            return 100;
        }
        return std::clamp(s.factions_.reputation(a, b), 0, 100);
    }

    std::int32_t abilityDelta(std::uint32_t object, std::int32_t ability) const
    {
        std::int32_t total = 0;
        ScriptApi a = api();
        for (const AppliedEffect& held : a.actor(object).effects) {
            if (held.type != 38 && held.type != 39) {
                continue;
            }
            const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
            if (data != nullptr && data->args.size() > 1 && data->args[0].asInt() == ability) {
                const std::int32_t change = static_cast<std::int32_t>(data->args[1].asInt());
                total += held.type == 38 ? change : -change;
            }
        }
        return total;
    }

    std::int32_t effectNet(std::uint32_t object, std::int32_t increase, std::int32_t decrease) const
    {
        return effectTotal(api(), object, increase) - effectTotal(api(), object, decrease);
    }

    // retail 005ad0e0 for AC, 004ed1d0 for the debilitated adjustment.
    std::int32_t armorClass(std::uint32_t object) const
    {
        const kotor::Entity* found = entity(object);
        std::int32_t ac = 0;
        std::int32_t dexMod = 0;
        if (isPc(object)) {
            ac = s.rules_.armorClass(s.character_) + s.forceArmorBonus();
            dexMod = modifier(s.character_.dex);
        } else if (found != nullptr) {
            ac = found->defense;
            for (const kotor::ActiveEffect& active : found->effects) {
                if (active.remaining > 0.0f) {
                    ac += active.armorBonus;
                }
            }
            dexMod = modifier(found->stats.abilities[1]);
        }
        ac += effectNet(object, 46, 47);
        // Ability effects move the modifier by half; odd scores are not tracked.
        ac += abilityDelta(object, 1) / 2;
        if (isDebilitated(api(), object)) {
            ac -= 4;
            if (dexMod > 0) {
                ac -= dexMod; // Only a negative Dex counts while disabled.
            }
        }
        return ac;
    }

    // Combat feat modifiers. The PC's combat modes and an NPC's ActionUseTalent
    // feat share one table. Retail's special-attack tables 005a4ec0 and 005a4fb0
    // were not resolved; these are the numbers the engine already used for the PC
    // (retail 005a8be0 shows +5 and +10 for the two power attack ranks).
    static void applyCombatMode(CombatMode mode, std::int32_t rank, Profile& profile)
    {
        switch (mode) {
        case CombatMode::PowerAttack:
            profile.attack -= 3;
            profile.flat += rank == 1 ? 5 : rank == 2 ? 8 : 10;
            break;
        case CombatMode::Flurry:
            profile.attack -= rank == 1 ? 4 : rank == 2 ? 2 : 0;
            break;
        case CombatMode::CriticalStrike:
            profile.attack -= 2;
            profile.threat -= rank;
            break;
        case CombatMode::Whirlwind:
            profile.attack -= 4;
            break;
        case CombatMode::Normal:
            break;
        }
    }

    static std::pair<CombatMode, std::int32_t> featMode(const std::string& label)
    {
        CombatMode mode = CombatMode::Normal;
        if (label.find("POWER_ATTACK") != std::string::npos
            || label.find("POWER_BLAST") != std::string::npos) {
            mode = CombatMode::PowerAttack;
        } else if (label.find("FLURRY") != std::string::npos
                   || label.find("RAPID_SHOT") != std::string::npos) {
            mode = CombatMode::Flurry;
        } else if (label.find("CRITICAL_STRIKE") != std::string::npos
                   || label.find("SNIPER_SHOT") != std::string::npos) {
            mode = CombatMode::CriticalStrike;
        } else if (label == "WHIRLWIND_ATTACK") {
            mode = CombatMode::Whirlwind;
        }
        const std::int32_t rank = label.find("MASTER") != std::string::npos ? 3
            : label.find("IMPROVED") != std::string::npos ? 2 : 1;
        return {mode, rank};
    }

    // retail 005a7ea0 (attack) and 005a9050 (damage inputs).
    Profile profile(std::uint32_t object, bool offhand, std::int32_t featId) const
    {
        Profile result;
        const kotor::Entity* found = entity(object);
        CombatMode mode = CombatMode::Normal;
        std::int32_t rank = 1;
        if (isPc(object)) {
            std::int32_t bonus = 0;
            s.rules_.weaponDamage(s.character_, result.dice, result.die, bonus);
            result.attack = s.rules_.attackBonus(s.character_) + s.forceAttackBonus()
                + s.forceJumpBonus_;
            result.flat = bonus + s.forceDamageBonus() + s.forceJumpBonus_;
            result.sneakDice = s.rules_.sneakAttackDice(s.character_);
            if (offhand) {
                result.attack -= 4; // The -10 against -6 gap of 005a7ea0.
            }
            for (const InventoryItem& item : s.character_.inventory) {
                if (item.equipped && s.rules_.baseItemInt(item.baseItem, "numdice", 0) > 0) {
                    result.threat = 21 - s.rules_.baseItemInt(item.baseItem, "critthreat", 1);
                    result.critMult = s.rules_.baseItemInt(item.baseItem, "crithitmult", 2);
                    result.ranged = s.rules_.baseItemInt(item.baseItem, "weapontype", 0) == 4;
                    break;
                }
            }
            mode = s.combatMode_;
            rank = s.combatModeRank_;
        } else if (found != nullptr) {
            const kotor::CreatureStats::Weapon& weapon =
                offhand ? found->stats.offHand : found->stats.mainHand;
            result.attack = offhand && weapon.present ? weapon.attackBonus : found->attackBonus;
            if (found->stats.combatDerived) {
                result.dice = weapon.dice;
                result.die = weapon.die;
                result.flat = weapon.flatDamage;
                // 005a5130: threat range from baseitems critthreat, doubled by Keen.
                result.threat = 21 - weapon.critThreat * (weapon.keen ? 2 : 1);
                result.critMult = weapon.critMult;
                result.ranged = weapon.ranged || found->ranged;
            } else {
                // No UTC sheet (doors, placeholders): the entity's flat damage range.
                result.dice = 1;
                result.die = std::max(1, found->damageMax - found->damageMin + 1);
                result.flat = found->damageMin - 1;
                result.ranged = found->ranged;
            }
            if (!offhand) {
                result.minDamage = found->damageMin;
                result.maxDamage = found->damageMax;
            }
            for (const kotor::ActiveEffect& active : found->effects) {
                if (active.remaining > 0.0f) {
                    result.attack += active.attackBonus;
                    result.flat += active.damageBonus;
                }
            }
            if (featId >= 0) {
                const auto [featModeValue, featRank] = featMode(s.rules_.featName(featId));
                mode = featModeValue;
                rank = featRank;
            }
        }
        applyCombatMode(mode, rank, result);
        result.attack += effectNet(object, 40, 41);
        result.flat += effectNet(object, 42, 43);
        const std::int32_t strength = abilityDelta(object, 0) / 2;
        const std::int32_t dexterity = abilityDelta(object, 1) / 2;
        result.attack += result.ranged ? dexterity : strength;
        if (!result.ranged) {
            result.flat += strength;
        }
        return result;
    }

    bool holdsLightsaber(std::uint32_t object) const
    {
        if (isPc(object)) {
            return s.rules_.hasLightsaber(s.character_);
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr && found->stats.mainHand.present
            && found->stats.mainHand.baseItem >= 8 && found->stats.mainHand.baseItem <= 10;
    }

    // retail 005b8e50: blaster bolts against a lightsaber. 0 none, 1 deflected, 2 reflected.
    int tryDeflect(std::uint32_t target, std::int32_t total) const
    {
        if (!holdsLightsaber(target)) {
            return 0;
        }
        const std::int32_t deflection = roll(20);
        std::int32_t defense = 0;
        if (isPc(target)) {
            if (!s.rules_.jediDeflects(s.character_, total, deflection)) {
                return 0;
            }
            defense = deflection + s.rules_.jediDefenseBonus(s.character_);
        } else {
            const kotor::Entity* found = entity(target);
            if (found == nullptr) {
                return 0;
            }
            const bool master = std::find(found->stats.feats.begin(), found->stats.feats.end(), 24)
                != found->stats.feats.end();
            const bool basic = std::find(found->stats.feats.begin(), found->stats.feats.end(), 1)
                != found->stats.feats.end();
            defense = deflection + (master ? 6 : basic ? 3 : 0) + found->stats.baseAttack
                + modifier(found->stats.abilities[1]);
            if (defense < total) {
                return 0;
            }
        }
        return defense >= total + 6 ? 2 : 1;
    }

    void enterCombat(std::uint32_t object) const
    {
        ActorCombat& combat = api().actor(object).combat;
        combat.inCombat = true;
        combat.combatTimer = kCombatStateSeconds;
    }

    // retail 004f2610 with state 0: every combat field resets.
    void leaveCombat(std::uint32_t object) const
    {
        ActorCombat& combat = api().actor(object).combat;
        combat.inCombat = false;
        combat.combatTimer = 0.0f;
        combat.roundActive = false;
        combat.pendingAction = -1;
        combat.lastAttackAction = -1;
        combat.lastHostileTarget = kInvalid;
        combat.attemptedTarget = kInvalid;
        combat.attackTarget = kInvalid;
        combat.attemptedSpell = kInvalid;
    }

    // retail 005baca0: one attack roll; 005bb590 round loop; 005a9050 damage.
    void performAttack(std::uint32_t attacker, std::uint32_t target, bool offhand,
                       std::int32_t featId)
    {
        ScriptApi a = api();
        if (!a.isAlive(target) || !a.isAlive(attacker)) {
            return;
        }
        const Profile weapon = profile(attacker, offhand, featId);
        if (isPc(attacker)) {
            s.forceJumpBonus_ = 0;
        }
        const std::int32_t ac = armorClass(target);
        const std::int32_t natural = roll(20);
        const std::int32_t total = natural + weapon.attack;
        ActorState& targetState = a.actor(target);
        targetState.last["lastAttacker"] = attacker;
        targetState.combat.lastHostileActor = attacker;
        targetState.combat.attackedBy = attacker;
        enterCombat(target);
        enterCombat(attacker);
        a.raiseHook(target, "Attacked", {{"lastAttacker", attacker}});

        std::int32_t result = 0;
        bool critical = false;
        if (hasEffectOfType(a, attacker, 74)) { // EffectAssuredHit
            result = kResultAutomatic;
        } else if (weapon.ranged && !offhand) {
            const int deflected = tryDeflect(target, total);
            result = deflected == 2 ? kResultReflected : deflected == 1 ? kResultDeflected : 0;
        }
        if (result == 0) {
            if (natural == 1 || (natural != 20 && total < ac)) {
                result = kResultMiss;
            } else {
                result = kResultHit;
                if (natural >= weapon.threat && roll(20) + weapon.attack >= ac) {
                    critical = true;
                    result = kResultCritical;
                }
            }
        }
        a.actor(attacker).combat.lastAttackResult = result;

        const std::string attackerName = nameOf(attacker);
        const std::string targetName = nameOf(target);
        if (result == kResultMiss) {
            s.logEvent(attackerName + " misses " + targetName + " (" + std::to_string(total)
                       + " vs " + std::to_string(ac) + ")");
            return;
        }
        std::int32_t damage = weapon.flat;
        for (std::int32_t i = 0; i < std::max(1, weapon.dice); ++i) {
            damage += roll(weapon.die);
        }
        // An entity's damage range bounds the roll: it spans exactly the derived
        // dice unless a script or fixture narrowed it.
        damage = std::clamp(damage, std::min(weapon.minDamage, weapon.maxDamage), weapon.maxDamage);
        if (critical) {
            damage *= std::max(1, weapon.critMult);
        }
        // Sneak attack needs a target that cannot defend itself (005a9050, flag).
        std::int32_t sneak = 0;
        if (weapon.sneakDice > 0 && isDebilitated(a, target)) {
            for (std::int32_t i = 0; i < weapon.sneakDice; ++i) {
                sneak += roll(6);
            }
        }
        damage = weapon.maxDamage <= 0 ? std::max(0, damage + sneak) : std::max(1, damage + sneak);

        if (result == kResultDeflected || result == kResultReflected) {
            s.logEvent(targetName + " deflects " + tagOf(attacker));
            if (result == kResultReflected) {
                s.logEvent("deflected bolt hits " + tagOf(attacker) + " for "
                           + std::to_string(damage));
                applyScriptDamage(a, attacker, damage, kDamageUniversal, target);
            }
            return;
        }
        if (s.audio_ && !isPc(attacker)) {
            const std::size_t index = a.entityFor(attacker);
            if (index != kNone) {
                s.audio_->playCreatureSound(static_cast<std::uint32_t>(index), 9); // attack
            }
        }
        s.logEvent(attackerName + " hits " + targetName + " for " + std::to_string(damage)
                   + (sneak > 0 ? " (sneak)" : "") + (critical ? " (critical)" : ""));
        applyScriptDamage(a, target, damage, kDamageUniversal, attacker);
    }

    void startRound(std::uint32_t self, std::uint32_t target, std::int32_t featId)
    {
        ScriptApi a = api();
        ActorCombat& combat = a.actor(self).combat;
        // retail 004d5f70: one on-hand attack, plus effect attacks, plus one for the
        // two-attack specials (004d2a10), plus an off-hand attack (004d2a70).
        std::int32_t attacks = 1 + std::max(0, effectTotal(a, self, effect::kModifyAttacks));
        CombatMode mode = isPc(self) ? s.combatMode_ : CombatMode::Normal;
        if (featId >= 0 && !isPc(self)) {
            mode = featMode(s.rules_.featName(featId)).first;
            combat.lastCombatFeat = featId;
        }
        if (mode == CombatMode::Flurry) {
            ++attacks;
        }
        const bool offhandAttack = hasOffhand(self);
        if (offhandAttack) {
            ++attacks;
        }
        combat.roundActive = true;
        combat.roundTimer = 0.0f;
        combat.roundTarget = target;
        combat.roundFeat = featId;
        combat.nextAttack = 0;
        combat.attackTimes.clear();
        // Retail schedules 1500 ms into a single-attack round (0052e7f0 passes 0x5dc)
        // and paces the rest with animation data this engine does not read, so
        // several attacks are spaced evenly: an assumption.
        for (std::int32_t i = 0; i < attacks; ++i) {
            combat.attackTimes.push_back(kRoundSeconds * static_cast<float>(i + 1)
                                         / static_cast<float>(attacks + 1));
        }
        combat.pendingAction = kActionAttackObject;
        const std::int32_t number = ++a.sim().combatRoundCounter;
        if (isPc(self)) {
            s.combatRound_ = number;
            s.logEvent("round " + std::to_string(number) + ": " + nameOf(target));
        }
    }

    bool hasOffhand(std::uint32_t object) const
    {
        if (isPc(object)) {
            int weapons = 0;
            for (const InventoryItem& item : s.character_.inventory) {
                if (item.equipped && s.rules_.baseItemInt(item.baseItem, "numdice", 0) > 0) {
                    ++weapons;
                }
            }
            return weapons > 1;
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr && found->stats.offHand.present;
    }

    float reach(std::uint32_t object) const
    {
        const kotor::Entity* found = entity(object);
        if (isPc(object)) {
            return profile(object, false, -1).ranged ? 15.0f : kMeleeReach;
        }
        return found != nullptr && found->ranged ? found->attackRange : kMeleeReach;
    }

    // retail 004d4620
    void endRound(std::uint32_t self)
    {
        ScriptApi a = api();
        ActorCombat& combat = a.actor(self).combat;
        combat.roundActive = false;
        combat.lastHostileTarget = combat.attemptedTarget;
        combat.lastAttackAction = combat.pendingAction;
        if (combat.attemptedTarget != kInvalid && !a.isAlive(combat.attemptedTarget)) {
            combat.attemptedTarget = kInvalid;
        }
        if (combat.attackTarget != kInvalid && !a.isAlive(combat.attackTarget)) {
            combat.attackTarget = kInvalid;
        }
        if (!playerControlled(self) && a.isAlive(self)) {
            a.raiseHook(self, "EndRound");
        }
    }

    void tick(float dt)
    {
        ScriptApi a = api();
        std::vector<std::uint32_t> fighters;
        for (const auto& [object, actor] : a.sim().actors) {
            if (actor.combat.inCombat || actor.combat.roundActive) {
                fighters.push_back(object);
            }
        }
        for (const std::uint32_t object : fighters) {
            if (!a.isAlive(object)) {
                leaveCombat(object);
                continue;
            }
            // Reference retaken each step: an attack can raise events and spawn actors.
            if (!a.actor(object).combat.roundActive) {
                ActorCombat& combat = a.actor(object).combat;
                combat.combatTimer -= dt;
                if (combat.combatTimer <= 0.0f) {
                    leaveCombat(object);
                }
                continue;
            }
            a.actor(object).combat.roundTimer += dt;
            a.actor(object).combat.combatTimer = kCombatStateSeconds;
            while (a.actor(object).combat.nextAttack < a.actor(object).combat.attackTimes.size()
                   && a.actor(object).combat.roundTimer
                       >= a.actor(object).combat.attackTimes[a.actor(object).combat.nextAttack]) {
                ActorCombat& combat = a.actor(object).combat;
                const std::size_t index = combat.nextAttack++;
                const std::uint32_t target = combat.roundTarget;
                const std::int32_t feat = combat.roundFeat;
                const bool offhand = hasOffhand(object) && index + 1 == combat.attackTimes.size();
                if (isDebilitated(a, object)) {
                    continue;
                }
                if (isPc(object) && s.combatMode_ == CombatMode::Whirlwind) {
                    whirlwind(object);
                } else if (inReach(object, target)) {
                    performAttack(object, target, offhand, feat);
                }
            }
            if (a.actor(object).combat.roundTimer >= kRoundSeconds) {
                endRound(object);
            }
        }
    }

    bool inReach(std::uint32_t self, std::uint32_t target) const
    {
        ScriptApi a = api();
        if (!a.isAlive(target)) {
            return false;
        }
        const kotor::Vec3 from = a.position(self);
        const kotor::Vec3 to = a.position(target);
        return std::hypot(from.x - to.x, from.y - to.y) <= reach(self) + 0.5f;
    }

    void whirlwind(std::uint32_t self)
    {
        ScriptApi a = api();
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const std::uint32_t other = a.objectFor(i);
            const kotor::Entity& candidate = s.scene_.entities[i];
            if (candidate.kind == kotor::EntityKind::Creature && !candidate.dead
                && reputation(self, other) <= 10 && inReach(self, other)) {
                performAttack(self, other, false, -1);
            }
        }
    }

    // ---- damage, healing, death ------------------------------------------------

    void damage(std::uint32_t target, std::int32_t amount, std::int32_t type,
                std::uint32_t attacker)
    {
        ScriptApi a = api();
        if (target == kInvalid || !a.isAlive(target)) {
            return;
        }
        kotor::Entity* found = entity(target);
        const bool pc = isPc(target);
        if (!pc && found == nullptr) {
            return;
        }
        const bool plot = pc ? a.actor(target).combat.plot : found->plot;
        std::int32_t dealt = plot ? 0 : std::max(0, amount); // retail 004dfa40 +0xf8
        if (dealt == 0 && !plot && amount > 0) {
            return;
        }
        const bool creature = a.isCreature(target);
        if (creature && dealt > 0) {
            dealt = resistedDamage(target, dealt, type);
        }
        ActorState& state = a.actor(target);
        state.combat.lastDamage = dealt;
        state.combat.lastDamageByType[type] = dealt;
        state.last["lastDamager"] = attacker;
        state.last["lastAttacker"] = attacker;
        if (creature) {
            state.combat.lastHostileActor = attacker;
            enterCombat(target);
        }
        if (pc) {
            const bool min1 = state.combat.min1Hp;
            s.playerHp_ -= dealt;
            if (min1 && s.playerHp_ < 1) {
                s.playerHp_ = 1; // retail 004cc280 +0x200
            }
            s.character_.hp = std::max(0, s.playerHp_);
            s.logEvent("player hp " + std::to_string(std::max(0, s.playerHp_)) + "/"
                       + std::to_string(s.playerMaxHp_));
            if (s.playerHp_ <= 0) {
                killPlayer();
            }
            return;
        }
        found->hitPoints -= dealt;
        if (found->min1Hp && found->hitPoints < 1) {
            found->hitPoints = 1;
        }
        const std::size_t index = a.entityFor(target);
        if (found->hitPoints <= 0) {
            found->hitPoints = 0;
            s.syncEntityToScriptWorld(index);
            s.logEvent(nameOf(attacker) + " defeats " + nameOf(target));
            if (s.audio_) {
                s.audio_->playCreatureSound(static_cast<std::uint32_t>(index), 15); // death
            }
            s.onCreatureKilled(index, attacker);
            return;
        }
        s.syncEntityToScriptWorld(index);
        if (s.audio_ && dealt > 0) {
            s.audio_->playCreatureSound(static_cast<std::uint32_t>(index), 12); // pain
        }
        if (!playerControlled(target)) {
            a.raiseHook(target, "Damaged", {{"lastDamager", attacker}, {"lastAttacker", attacker}});
        }
    }

    // Target vcalls of 005a9050 and the absorption of 004f3830.
    std::int32_t resistedDamage(std::uint32_t target, std::int32_t dealt, std::int32_t type) const
    {
        ScriptApi a = api();
        ActorState& state = a.actor(target);
        // EffectDamageImmunity: percentage by type.
        std::int32_t immunity = 0;
        std::int32_t flatResist = 0;
        for (AppliedEffect& held : state.effects) {
            const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
            if (data == nullptr || data->args.empty()) {
                continue;
            }
            const std::int32_t arg0 = static_cast<std::int32_t>(data->args[0].asInt());
            const std::int32_t arg1 = data->args.size() > 1
                ? static_cast<std::int32_t>(data->args[1].asInt())
                : 0;
            if ((held.type == 44 || held.type == 45) && (arg0 == type || type == kDamageUniversal)) {
                immunity += held.type == 44 ? arg1 : -arg1;
            } else if (held.type == 1 && (arg0 == type || type == kDamageUniversal)) {
                flatResist = std::max(flatResist, arg1);
            } else if (held.type == 7) {
                flatResist = std::max(flatResist, arg0);
            }
        }
        immunity = std::clamp(immunity, -100, 100);
        dealt -= dealt * immunity / 100;
        dealt = std::max(0, dealt - flatResist);
        // Force Shield and friends on the shared stack.
        std::vector<kotor::ActiveEffect>* stack = nullptr;
        if (isPc(target)) {
            stack = &s.character_.effects;
        } else if (kotor::Entity* found = entity(target); found != nullptr) {
            stack = &found->effects;
        }
        if (stack != nullptr) {
            for (kotor::ActiveEffect& active : *stack) {
                if (active.remaining <= 0.0f || active.shield <= 0 || dealt <= 0) {
                    continue;
                }
                const std::int32_t absorbed = std::min(active.shield, dealt);
                dealt -= absorbed;
                active.shield -= absorbed;
            }
        }
        if (isPc(target)) {
            dealt = std::max(0, dealt - s.rules_.damageReduction(s.character_));
        }
        if (inParty(target)) {
            dealt = static_cast<std::int32_t>(static_cast<float>(dealt)
                                              * s.rules_.partyDamageMultiplier());
        }
        // Temporary hit points absorb what is left. retail 004f3830 +0xe4
        for (AppliedEffect& held : state.effects) {
            if (held.type == 9 && held.pool > 0 && dealt > 0) {
                const std::int32_t absorbed = std::min(held.pool, dealt);
                held.pool -= absorbed;
                dealt -= absorbed;
            }
        }
        return dealt;
    }

    void killPlayer()
    {
        s.playerHp_ = 0;
        s.character_.hp = 0;
        s.playerDead_ = true;
        s.combatTarget_ = UINT32_MAX;
        s.logEvent("player defeated");
        api().clearQueue(s.scriptWorld_.pc);
        leaveCombat(s.scriptWorld_.pc);
        s.runSceneHook("Mod_OnPlrDying", s.scriptWorld_.pc);
        s.runSceneHook("Mod_OnPlrDeath", s.scriptWorld_.pc);
    }

    void kill(std::uint32_t object, std::uint32_t killer)
    {
        ScriptApi a = api();
        kotor::Entity* found = entity(object);
        if (!a.isAlive(object) || (!isPc(object) && found == nullptr)) {
            return;
        }
        // retail 004e0ac0: Plot refuses it, Min1HP holds at one hit point.
        const bool plot = isPc(object) ? a.actor(object).combat.plot : found->plot;
        const bool min1 = isPc(object) ? a.actor(object).combat.min1Hp : found->min1Hp;
        if (plot) {
            return;
        }
        if (min1) {
            if (isPc(object)) {
                s.playerHp_ = 1;
                s.character_.hp = 1;
            } else {
                found->hitPoints = 1;
                s.syncEntityToScriptWorld(a.entityFor(object));
            }
            return;
        }
        if (isPc(object)) {
            killPlayer();
            return;
        }
        found->hitPoints = 0;
        s.logEvent(nameOf(killer) + " defeats " + nameOf(object));
        s.onCreatureKilled(a.entityFor(object), killer);
    }

    void heal(std::uint32_t object, std::int32_t amount)
    {
        if (isPc(object)) {
            s.playerHp_ = std::min(s.playerMaxHp_, s.playerHp_ + std::max(0, amount));
            s.character_.hp = s.playerHp_;
            return;
        }
        if (kotor::Entity* found = entity(object); found != nullptr) {
            found->hitPoints = std::min(found->maxHitPoints, found->hitPoints + std::max(0, amount));
            s.syncEntityToScriptWorld(api().entityFor(object));
        }
    }

    // EffectResurrection: back at one hit point. The amount was not traced in the
    // retail handler; shipped scripts follow it with EffectHeal.
    void resurrect(std::uint32_t object)
    {
        ScriptApi a = api();
        if (a.isAlive(object)) {
            return;
        }
        if (isPc(object)) {
            s.playerDead_ = false;
            s.playerHp_ = 1;
            s.character_.hp = 1;
            s.logEvent("player resurrected");
            return;
        }
        if (kotor::Entity* found = entity(object); found != nullptr) {
            found->dead = false;
            found->hitPoints = 1;
            s.syncEntityToScriptWorld(a.entityFor(object));
            s.logEvent(nameOf(object) + " resurrected");
        }
    }

    std::int32_t& npcForce(std::uint32_t object) const
    {
        ActorCombat& combat = api().actor(object).combat;
        if (combat.forcePoints < 0) {
            const kotor::Entity* found = entity(object);
            combat.forcePoints = found != nullptr ? found->stats.forcePoints : 0;
        }
        return combat.forcePoints;
    }

    std::int32_t forcePoints(std::uint32_t object) const
    {
        return isPc(object) ? s.character_.fp : npcForce(object);
    }

    void changeForce(std::uint32_t object, std::int32_t delta)
    {
        if (isPc(object)) {
            s.character_.fp = std::clamp(s.character_.fp + delta, 0, s.character_.maxFp);
            return;
        }
        const kotor::Entity* found = entity(object);
        const std::int32_t top = found != nullptr ? found->stats.maxForcePoints : 0;
        std::int32_t& points = npcForce(object);
        points = std::clamp(points + delta, 0, std::max(top, points));
    }

    // ---- saves -----------------------------------------------------------------

    std::int32_t saveBase(std::uint32_t object, std::int32_t which) const
    {
        std::int32_t total = 0;
        if (isPc(object)) {
            const CharacterSheet& sheet = s.character_;
            const int table = which == 1 ? 0 : which == 2 ? 1 : 2;
            total = s.rules_.savingThrow(sheet, table)
                + modifier(which == 1 ? sheet.con : which == 2 ? sheet.dex : sheet.wis);
        } else if (const kotor::Entity* found = entity(object); found != nullptr) {
            total = which == 1 ? found->fortSave : which == 2 ? found->refSave : found->willSave;
        }
        return total;
    }

    // retail 005b92b0: d20 + save + effects (capped at +20) >= DC. nSave 1 fort, 2 reflex, 3 will.
    std::int32_t savingThrow(std::uint32_t object, std::int32_t which, std::int32_t dc)
    {
        ScriptApi a = api();
        std::int32_t bonus = 0;
        for (const AppliedEffect& held : a.actor(object).effects) {
            if (held.type != 50 && held.type != 51) {
                continue;
            }
            const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
            if (data != nullptr && data->args.size() > 1
                && (data->args[0].asInt() == which || data->args[0].asInt() == 0)) {
                const std::int32_t value = static_cast<std::int32_t>(data->args[1].asInt());
                bonus += held.type == 50 ? value : -value;
            }
        }
        return roll(20) + saveBase(object, which) + std::min(bonus, 20) >= dc ? 1 : 0;
    }

    // ---- perception ------------------------------------------------------------

    // Stable for 20 s per pair, as retail re-rolls its values on that period (00502ac0).
    std::int32_t pairRoll(std::uint32_t a, std::uint32_t b, std::int32_t salt, std::int32_t sides) const
    {
        const std::uint32_t bucket = static_cast<std::uint32_t>(s.scriptWorld_.time / 20.0);
        std::uint32_t mix = (a * 2654435761u) ^ (b * 40503u) ^ (bucket * 2246822519u)
            ^ static_cast<std::uint32_t>(salt);
        mix ^= mix >> 13;
        mix *= 3266489917u;
        mix ^= mix >> 16;
        return 1 + static_cast<std::int32_t>(mix % static_cast<std::uint32_t>(sides));
    }

    std::int32_t skillOf(std::uint32_t object, int skill) const
    {
        if (isPc(object)) {
            return s.rules_.skillBonus(s.character_, skill == 2 ? "stealth" : "awareness");
        }
        const kotor::Entity* found = entity(object);
        if (found == nullptr || static_cast<std::size_t>(skill) >= found->stats.skills.size()) {
            return 0;
        }
        return found->stats.skills[static_cast<std::size_t>(skill)];
    }

    // retail 004f1fd0: a stealthy target against the observer's Awareness; passive
    // observers count half. The facing and distance adjustments are not modelled.
    bool spotsStealth(std::uint32_t observer, std::uint32_t target) const
    {
        const std::int32_t awareness = skillOf(observer, 3);
        const std::int32_t watch = api().actor(observer).combat.inCombat ? awareness : awareness / 2;
        const std::int32_t score = watch + pairRoll(target, observer, 1, 10)
            - (10 + pairRoll(observer, target, 2, 10)) - skillOf(target, 2);
        return score > 0;
    }

    bool isHidden(std::uint32_t object) const
    {
        return (isPc(object) && s.stealthActive_) || hasEffectOfType(api(), object, 56);
    }

    void notice(std::uint32_t observer, std::uint32_t target, bool seen, bool vanished, bool heard,
                bool inaudible) const
    {
        api().raiseHook(observer, "Notice",
                        {{"lastPerceived", target},
                         {"perceptionSeen", seen ? 1u : 0u},
                         {"perceptionVanished", vanished ? 1u : 0u},
                         {"perceptionHeard", heard ? 1u : 0u},
                         {"perceptionInaudible", inaudible ? 1u : 0u}});
    }

    // retail 00502ac0 per pair: sight within range with line of sight, hearing
    // within range; each change is one event.
    void perceive(float dt)
    {
        ScriptApi a = api();
        WorldSim& sim = a.sim();
        sim.perceptionTimer += dt;
        if (sim.perceptionTimer < kPerceptionInterval) {
            return;
        }
        sim.perceptionTimer = 0.0f;
        if (a.dialogueActive()) {
            return;
        }
        std::vector<std::uint32_t> creatures;
        if (!s.playerDead_) {
            creatures.push_back(a.pc());
        }
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const kotor::Entity& candidate = s.scene_.entities[i];
            if (candidate.kind == kotor::EntityKind::Creature && !candidate.dead) {
                creatures.push_back(a.objectFor(i));
            }
        }
        const auto sees = [&](std::uint32_t observer) -> std::pair<float, float> {
            // In combat a non-leader uses the PartyCombat row. retail 004efc70, 004efd20
            if (a.actor(observer).combat.inCombat && !playerControlled(observer)) {
                return {s.rules_.perceptionRange(18, false), s.rules_.perceptionRange(18, true)};
            }
            if (isPc(observer)) {
                return {s.rules_.perceptionRange(12, false), s.rules_.perceptionRange(12, true)};
            }
            const kotor::Entity* found = entity(observer);
            const std::int32_t row = found != nullptr
                ? static_cast<std::int32_t>(found->stats.perceptionRange)
                : 11;
            return {s.rules_.perceptionRange(row, false), s.rules_.perceptionRange(row, true)};
        };
        for (const std::uint32_t observer : creatures) {
            const auto [sight, hearing] = sees(observer);
            const kotor::Vec3 from = a.position(observer);
            std::map<std::uint32_t, std::uint8_t> old = std::move(a.actor(observer).perceived);
            std::map<std::uint32_t, std::uint8_t> now;
            const bool isParty = inParty(observer);
            for (const std::uint32_t target : creatures) {
                if (target == observer) {
                    continue;
                }
                std::uint8_t bits = 0;
                if (isParty && inParty(target)) {
                    bits = 3; // Party members always know of each other. retail 00502ac0
                } else {
                    const kotor::Vec3 to = a.position(target);
                    const float dx = from.x - to.x;
                    const float dy = from.y - to.y;
                    const float dz = from.z - to.z;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    const bool hidden = isHidden(target);
                    const bool spotted = !hidden
                        || (!hasEffectOfType(a, target, 56) && spotsStealth(observer, target))
                        || hasEffectOfType(a, observer, 64) || hasEffectOfType(a, observer, 65);
                    if (spotted && distance <= sight * sight
                        && a.hasLineOfSight(observer, target)) {
                        bits |= 1;
                    }
                    if (spotted && distance <= hearing * hearing) {
                        bits |= 2;
                    }
                }
                if (bits != 0) {
                    now[target] = bits;
                }
            }
            for (const auto& [target, bits] : now) {
                const auto it = old.find(target);
                const std::uint8_t before = it != old.end() ? it->second : 0;
                if ((bits & 2) != 0 && (before & 2) == 0) {
                    notice(observer, target, false, false, true, false);
                }
                if ((bits & 1) != 0 && (before & 1) == 0) {
                    notice(observer, target, true, false, false, false);
                }
                if ((bits & 1) == 0 && (before & 1) != 0) {
                    notice(observer, target, false, true, false, false);
                }
                if ((bits & 2) == 0 && (before & 2) != 0) {
                    notice(observer, target, false, false, false, true);
                }
            }
            for (const auto& [target, bits] : old) {
                if (now.contains(target) || !a.world().valid(target)) {
                    continue;
                }
                if ((bits & 1) != 0) {
                    notice(observer, target, false, true, false, false);
                }
                if ((bits & 2) != 0) {
                    notice(observer, target, false, false, false, true);
                }
            }
            a.actor(observer).perceived = std::move(now);
        }
    }

    // ---- talents -----------------------------------------------------------------

    const std::vector<std::int32_t>* npcList(std::uint32_t object, bool power) const
    {
        const kotor::Entity* found = entity(object);
        if (found == nullptr) {
            return nullptr;
        }
        return power ? &found->stats.powers : &found->stats.feats;
    }

    bool knows(std::uint32_t object, bool power, std::int32_t id) const
    {
        if (isPc(object)) {
            return power ? s.character_.powers.contains(id) : s.character_.feats.contains(id);
        }
        const std::vector<std::int32_t>* list = npcList(object, power);
        return list != nullptr && std::find(list->begin(), list->end(), id) != list->end();
    }

    std::vector<std::int32_t> talents(std::uint32_t object, bool power) const
    {
        if (isPc(object)) {
            const std::set<std::int32_t>& from = power ? s.character_.powers : s.character_.feats;
            return {from.begin(), from.end()};
        }
        const std::vector<std::int32_t>* list = npcList(object, power);
        return list != nullptr ? *list : std::vector<std::int32_t>{};
    }

    // retail 005aaa30. Type 0 Force power, 1 feat, 2 skill.
    bool hasTalent(std::uint32_t object, std::int32_t type, std::int32_t id) const
    {
        ScriptApi a = api();
        if (id < 0 || !a.isAlive(object)) {
            return false;
        }
        if (type == 0) {
            if (!knows(object, true, id)) {
                return false;
            }
            const std::optional<ForcePowerInfo> info = s.rules_.forcePower(id);
            return info.has_value() && forcePoints(object) >= info->cost;
        }
        if (type == 1) {
            return knows(object, false, id);
        }
        return type == 2 && skillOf(object, id) > 0;
    }

    // Best: closest maxcr to the limit without going over. Random: any. Retail
    // 005b23a0 flips a coin between the two talent groups, and falls back to the
    // other when one finds nothing; items are not offered as talents here.
    std::pair<std::int32_t, std::int32_t> pickTalent(std::uint32_t object, std::int32_t category,
                                                     std::int32_t crMax, bool random,
                                                     std::int32_t excludeType,
                                                     std::int32_t excludeId) const
    {
        const auto search = [&](bool power) -> std::int32_t {
            const std::int32_t type = power ? 0 : 1;
            if (excludeType == type && excludeId == -1) {
                return -1;
            }
            std::vector<std::int32_t> options;
            std::int32_t best = -1;
            std::int32_t bestCr = -1;
            for (const std::int32_t id : talents(object, power)) {
                if ((excludeType == type && excludeId == id) || !hasTalent(object, type, id)) {
                    continue;
                }
                if (s.rules_.talentInt(power, id, "category", -1) != category) {
                    continue;
                }
                const std::int32_t cr = s.rules_.talentInt(power, id, "maxcr", 0);
                if (!random && crMax >= 0 && cr > crMax) {
                    continue;
                }
                options.push_back(id);
                if (cr > bestCr) {
                    bestCr = cr;
                    best = id;
                }
            }
            if (random && !options.empty()) {
                return options[s.random() % options.size()];
            }
            return best;
        };
        const bool powerFirst = (s.random() & 1u) == 0;
        std::int32_t id = search(powerFirst);
        if (id >= 0) {
            return {powerFirst ? 0 : 1, id};
        }
        id = search(!powerFirst);
        return id >= 0 ? std::pair{powerFirst ? 1 : 0, id} : std::pair{-1, -1};
    }

    std::int64_t talentRecord(std::int32_t type, std::int32_t id) const
    {
        WorldSim& sim = api().sim();
        const auto key = std::pair{type, id};
        const auto it = sim.talents.find(key);
        if (it != sim.talents.end() && sim.engine.get<TalentData>(it->second) != nullptr) {
            return it->second;
        }
        const std::int64_t record = sim.engine.add(TalentData{type, id});
        sim.talents[key] = record;
        return record;
    }

    // ---- casting -----------------------------------------------------------------

    void finishCast(std::uint32_t self, const ActorAction& action)
    {
        ScriptApi a = api();
        ActorCombat& combat = a.actor(self).combat;
        combat.lastForcePower = action.spell;
        combat.forcePowerSuccess = true;
        combat.lastSpellTarget = action.object;
        if (action.fake) {
            return;
        }
        if (const std::optional<ForcePowerInfo> info = s.rules_.forcePower(action.spell);
            info.has_value()) {
            changeForce(self, -info->cost);
        }
        const std::string script = s.rules_.powerImpactScript(action.spell);
        if (script.empty()) {
            return;
        }
        ScriptContext context;
        context["spellTarget"] = action.object;
        context["spellId"] = static_cast<std::uint32_t>(action.spell);
        context["lastSpell"] = static_cast<std::uint32_t>(action.spell);
        context["lastSpellCaster"] = self;
        context["spellLocation"] = static_cast<std::uint32_t>(action.location);
        a.runScript(script, self, context);
    }
};

// ---- entry points ------------------------------------------------------------

void GameSession::attackPlayerForTest(std::size_t index)
{
    CombatOps{*this}.performAttack(objectForEntity(index), scriptWorld_.pc, false, -1);
}

void tickCombat(ScriptApi api, float dt)
{
    CombatOps{api.session}.tick(dt);
}

void tickPerception(ScriptApi api, float dt)
{
    CombatOps{api.session}.perceive(dt);
}

void applyScriptDamage(ScriptApi api, std::uint32_t target, std::int32_t amount,
                       std::int32_t damageType, std::uint32_t attacker)
{
    CombatOps{api.session}.damage(target, amount, damageType, attacker);
}

void healCreature(ScriptApi api, std::uint32_t object, std::int32_t amount)
{
    CombatOps{api.session}.heal(object, amount);
}

void killCreature(ScriptApi api, std::uint32_t object, std::uint32_t killer)
{
    CombatOps{api.session}.kill(object, killer);
}

void resurrectCreature(ScriptApi api, std::uint32_t object)
{
    CombatOps{api.session}.resurrect(object);
}

void changeForcePoints(ScriptApi api, std::uint32_t object, std::int32_t delta)
{
    CombatOps{api.session}.changeForce(object, delta);
}

int reputationBetween(ScriptApi api, std::uint32_t source, std::uint32_t target)
{
    return CombatOps{api.session}.reputation(source, target);
}

bool isEnemy(ScriptApi api, std::uint32_t source, std::uint32_t target)
{
    return api.isCreature(target) && reputationBetween(api, source, target) <= 10;
}

bool creatureHasPerceived(ScriptApi api, std::uint32_t observer, std::uint32_t target,
                          std::uint8_t bits)
{
    const std::map<std::uint32_t, std::uint8_t>& perceived = api.actor(observer).perceived;
    const auto it = perceived.find(target);
    return it != perceived.end() && (it->second & bits) != 0;
}

// retail 005462d0 -> 005b4880. Which effects set the +0x8ed flag was not listed
// one by one, so isDisablingEffect names the kinds whose nss comments say the
// creature cannot act, and the legacy Force timers count too.
bool isDebilitated(ScriptApi api, std::uint32_t object)
{
    for (const AppliedEffect& held : api.actor(object).effects) {
        if (isDisablingEffect(held.type)) {
            return true;
        }
    }
    const std::size_t index = api.entityFor(object);
    if (index != kNone) {
        const kotor::Entity& entity = api.scene().entities[index];
        return entity.stunTimer > 0.0f || entity.fearTimer > 0.0f;
    }
    return false;
}

bool runCutsceneAttackAction(ScriptApi api, std::uint32_t self, float dt)
{
    ActorState& state = api.actor(self);
    ActorAction& action = state.queue.front();
    if (!action.started) {
        action.started = true;
        action.remaining = 1.0f; // No retail length recovered.
        const std::size_t index = api.entityFor(self);
        const std::string name = api.animationName(action.animation);
        if (index != kNone && !name.empty()) {
            api.playAnimation(index, name, false);
        }
    }
    action.remaining -= dt;
    if (action.remaining > 0.0f) {
        return false;
    }
    const ActorAction taken = action;
    state.queue.pop_front();
    // ATTACK_RESULT_HIT_SUCCESSFUL 1, CRITICAL_HIT 2, AUTOMATIC_HIT 3.
    if (taken.result >= 1 && taken.result <= 3 && taken.amount > 0 && api.isAlive(taken.object)) {
        applyScriptDamage(api, taken.object, taken.amount, kDamageUniversal, self);
    }
    return true;
}

bool runAttackAction(ScriptApi api, std::uint32_t self, float dt)
{
    CombatOps ops{api.session};
    ActorState& state = api.actor(self);
    ActorAction& action = state.queue.front();
    const std::uint32_t target = action.object;
    if (target == kInvalid || !api.isAlive(target) || api.entityFor(target) == kNone) {
        if (target != api.pc() || !api.isAlive(target)) {
            state.combat.attackTarget = kInvalid;
            state.queue.pop_front();
            return true;
        }
    }
    ActorCombat& combat = state.combat;
    combat.attemptedTarget = target;
    combat.pendingAction = kActionAttackObject;
    ops.enterCombat(self);
    const kotor::Vec3 from = api.position(self);
    const kotor::Vec3 to = api.position(target);
    const float distance = std::hypot(from.x - to.x, from.y - to.y);
    const float reach = ops.reach(self);
    const bool ranged = reach > kMeleeReach;
    if (distance > reach || (ranged && !api.hasLineOfSight(self, target))) {
        const ActorStep step = api.step(self, to, api.moveSpeed(self, true), dt,
                                        ranged ? std::max(1.0f, reach * 0.9f) : kMeleeStop);
        action.stuck = step == ActorStep::Blocked ? action.stuck + dt : 0.0f;
        if (action.stuck >= kAttackStuckGiveUp) {
            api.log("attack gave up: no way to target");
            combat.attackTarget = kInvalid;
            state.queue.pop_front();
            return true;
        }
        return false;
    }
    combat.attackTarget = target;
    if (!combat.roundActive) {
        ops.startRound(self, target, action.spell);
    }
    return false;
}

bool runCastAction(ScriptApi api, std::uint32_t self, float dt)
{
    CombatOps ops{api.session};
    ActorState& state = api.actor(self);
    ActorAction& action = state.queue.front();
    if (action.object != kInvalid && !api.isAlive(action.object)) {
        state.queue.pop_front();
        return true;
    }
    if (!action.started) {
        const std::optional<ForcePowerInfo> info = api.session.rules().forcePower(action.spell);
        action.range = info.has_value() && info->range > 0 ? static_cast<float>(info->range) : 0.0f;
        if (action.object != kInvalid && action.object != self && action.range > 0.0f) {
            const kotor::Vec3 to = api.position(action.object);
            const kotor::Vec3 from = api.position(self);
            if (std::hypot(from.x - to.x, from.y - to.y) > action.range) {
                api.step(self, to, api.moveSpeed(self, true), dt, action.range * 0.9f);
                return false;
            }
        }
        action.started = true;
        // A cast takes a full round; retail's conjure and cast times only pace
        // animations this engine does not drive from here.
        action.remaining = kRoundSeconds / 2.0f;
        ActorCombat& combat = state.combat;
        combat.attemptedSpell = action.object;
        combat.pendingAction = 4; // ACTION_CASTSPELL
        ops.enterCombat(self);
        if (!combat.roundActive) {
            combat.roundActive = true;
            combat.roundTimer = 0.0f;
            combat.attackTimes.clear();
            combat.nextAttack = 0;
            combat.roundTarget = action.object;
        }
    }
    action.remaining -= dt;
    if (action.remaining > 0.0f) {
        return false;
    }
    const ActorAction taken = std::move(state.queue.front());
    state.queue.pop_front();
    ops.finishCast(self, taken);
    return true;
}

// ---- handlers ----------------------------------------------------------------

namespace {

// ACT_37 ActionAttack 0052e7f0
// ---- cutscene attacks and immunity queries ----------------------------------

// ACT_503 CutsceneAttack(oTarget, nAnimation, nAttackResult, nDamage) 0052e890: plays the
// animation with no attack roll and applies the damage on a hit result.
Cells actionCutsceneAttack(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::CutsceneAttack;
    action.object = call.objectArgOrInvalid(0);
    action.animation = static_cast<std::int32_t>(call.intArg(1));
    action.result = static_cast<std::int32_t>(call.intArg(2));
    action.amount = static_cast<std::int32_t>(call.intArg(3));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_169 ResistForce 00541cc0. Retail rolls a d20 plus the source's level against the
// target's spell resistance (effect 52). 0 not resisted, 1 resisted; the three-way
// result the header mentions (global immunity) was not traced.
Cells resistForce(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int32_t resistance = effectTotal(api, call.objectArg(1), 52);
    if (resistance <= 0) {
        return ActionCall::retInt(0);
    }
    const std::int32_t roll = api.world().rng.randint(1, 20) + creatureTotalLevel(api, call.objectArg(0));
    return ActionCall::retInt(roll < resistance ? 1 : 0);
}

// ACT_751 GetIsPoisoned 00547a80: holds an EffectPoison.
Cells getIsPoisoned(ActionCall& call)
{
    return ActionCall::retInt(hasEffectOfType(call.api(), call.objectArg(0), 31) ? 1 : 0);
}

// Does the creature hold an EffectImmunity (type 15) of this IMMUNITY_TYPE_*?
bool holdsImmunity(ScriptApi api, std::uint32_t object, std::int32_t immunity)
{
    for (const AppliedEffect& held : api.actor(object).effects) {
        if (held.type != 15) {
            continue;
        }
        const EffectData* data = api.sim().engine.get<EffectData>(held.effect);
        if (data != nullptr && !data->args.empty() && data->args.front().asInt() == immunity) {
            return true;
        }
    }
    return false;
}

// ACT_274 GetIsImmune 00545840.
Cells getIsImmune(ActionCall& call)
{
    return ActionCall::retInt(holdsImmunity(call.api(), call.objectArg(0),
                                            static_cast<std::int32_t>(call.intArg(1))) ? 1 : 0);
}

// IMMUNITY_TYPE_* for an effect's type, or -1 when no immunity covers it.
std::int32_t immunityForEffect(std::int32_t type)
{
    switch (type) {
    case 31: return 2;  // poison
    case 25: return 4;  // frightened
    case 27: return 6;  // paralysis
    case 29: return 12; // stun
    case 30: return 13; // sleep
    case 24: return 16; // confused
    default: return -1;
    }
}

// ACT_390 GetIsLinkImmune 005458d0: immune to the effect or to any effect linked to it.
Cells getIsLinkImmune(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t target = call.objectArg(0);
    std::vector<std::int64_t> pending{call.engineArg(1)};
    while (!pending.empty()) {
        const EffectData* data = api.sim().engine.get<EffectData>(pending.back());
        pending.pop_back();
        if (data == nullptr) {
            continue;
        }
        const std::int32_t immunity = immunityForEffect(data->type);
        if (immunity >= 0 && holdsImmunity(api, target, immunity)) {
            return ActionCall::retInt(1);
        }
        pending.insert(pending.end(), data->links.begin(), data->links.end());
    }
    return ActionCall::retInt(0);
}

// ACT_366 DisplayFeedBackText 00541560: a feedback line. Logged; the HUD reads the log.
Cells displayFeedBackText(ActionCall& call)
{
    call.api().log("feedback text " + std::to_string(call.intArg(1)));
    return ActionCall::none();
}

Cells actionAttack(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::Attack;
    action.object = call.objectArgOrInvalid(0);
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_54 CancelCombat 00546510 -> 004fdaa0
Cells cancelCombat(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArgOrInvalid(0);
    if (!api.isCreature(object)) {
        return ActionCall::none();
    }
    api.clearQueue(object);
    CombatOps{call.session}.leaveCombat(object);
    return ActionCall::none();
}

// ACT_140 GetIsDead 0053ecb0
Cells getIsDead(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArgOrInvalid(0);
    return ActionCall::retInt(api.isCreature(object) && !api.isAlive(object) ? 1 : 0);
}

// ACT_235/236/237 share 00537ce0: the bands of 0057cb80.
Cells getReputationBand(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t target = call.objectArgOrInvalid(0);
    const std::uint32_t source = call.objectArg(1);
    if (!api.isCreature(target) || !api.isCreature(source)) {
        return ActionCall::retInt(0);
    }
    const int reputation = reputationBetween(api, source, target);
    const bool result = call.id == 235 ? reputation <= 10
        : call.id == 236                ? reputation >= 90
                                        : reputation > 10 && reputation < 90;
    return ActionCall::retInt(result ? 1 : 0);
}

// ACT_289/290 0053c780: the perception list entry on the source.
Cells getObjectPerceived(ActionCall& call)
{
    return ActionCall::retInt(creatureHasPerceived(call.api(), call.objectArg(1),
                                                   call.objectArgOrInvalid(0),
                                                   call.id == 289 ? 1 : 2)
                                  ? 1
                                  : 0);
}

Cells getIsInCombat(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).combat.inCombat ? 1 : 0);
}

// Fields of the object: +0x50c, +0x504, +0x524, +0x53c, +0x520.
Cells getAttackTarget(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.objectArg(0)).combat.attackTarget);
}
Cells getAttemptedAttackTarget(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.self()).combat.attemptedTarget);
}
Cells getAttemptedSpellTarget(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.self()).combat.attemptedSpell);
}
Cells getLastHostileTarget(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.objectArg(0)).combat.lastHostileTarget);
}
Cells getGoingToBeAttackedBy(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.objectArgOrInvalid(0)).combat.attackedBy);
}
Cells getLastAttackAction(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).combat.lastAttackAction);
}
Cells getLastCombatFeatUsed(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).combat.lastCombatFeat);
}
Cells getLastAttackResult(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).combat.lastAttackResult);
}
Cells getLastAttackerOrDamager(ActionCall& call)
{
    // ACT_346 reads the named object's field.
    const ScriptContext& last = call.api().actor(call.objectArg(0)).last;
    const auto it = last.find("lastDamager");
    return ActionCall::retObject(it != last.end() ? it->second : kInvalid);
}

// The PC's Force state lives on the sheet; the legacy cases serve it.
Cells getLastForcePowerUsed(ActionCall& call)
{
    const std::uint32_t object = call.objectArg(0);
    if (object == call.api().pc()) {
        return std::nullopt;
    }
    return ActionCall::retInt(call.api().actor(object).combat.lastForcePower);
}
Cells getWasForcePowerSuccessful(ActionCall& call)
{
    const std::uint32_t object = call.objectArg(0);
    if (object == call.api().pc()) {
        return std::nullopt;
    }
    return ActionCall::retInt(call.api().actor(object).combat.forcePowerSuccess ? 1 : 0);
}
Cells setForcePowerUnsuccessful(ActionCall& call)
{
    call.api().actor(call.objectArg(1)).combat.forcePowerSuccess = call.intArg(0) == 0;
    return ActionCall::none();
}

Cells getIsDebilitated(ActionCall& call)
{
    return ActionCall::retInt(isDebilitated(call.api(), call.objectArg(0)) ? 1 : 0);
}

// ACT_456 SetPlotFlag 005433d0 and ACT_716 SetMinOneHP 00545f90 write the object
// field on any object; ScriptObjects-side entities carry them on Entity.
Cells setPlotFlag(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArgOrInvalid(0);
    const bool value = call.intArg(1) != 0;
    if (object == api.pc()) {
        api.actor(object).combat.plot = value;
    } else if (const std::size_t index = api.entityFor(object); index != kNone) {
        api.scene().entities[index].plot = value;
    } else {
        return std::nullopt; // Items keep plot on their object (ScriptItems.cpp).
    }
    return ActionCall::none();
}
Cells getPlotFlag(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (object == api.pc()) {
        return ActionCall::retInt(api.actor(object).combat.plot ? 1 : 0);
    }
    if (const std::size_t index = api.entityFor(object); index != kNone) {
        return ActionCall::retInt(api.scene().entities[index].plot ? 1 : 0);
    }
    return std::nullopt;
}
Cells setMinOneHp(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArgOrInvalid(0);
    const bool value = call.intArg(1) != 0;
    if (object == api.pc()) {
        api.actor(object).combat.min1Hp = value;
    } else if (const std::size_t index = api.entityFor(object); index != kNone) {
        api.scene().entities[index].min1Hp = value;
    }
    return ActionCall::none();
}
Cells getMinOneHp(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t object = call.objectArg(0);
    if (object == api.pc()) {
        return ActionCall::retInt(api.actor(object).combat.min1Hp ? 1 : 0);
    }
    const std::size_t index = api.entityFor(object);
    return ActionCall::retInt(index != kNone && api.scene().entities[index].min1Hp ? 1 : 0);
}

Cells getPartyAiStyle(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().partyAiStyle);
}
Cells setPartyAiStyle(ActionCall& call)
{
    call.api().sim().partyAiStyle = static_cast<std::int32_t>(call.intArg(0));
    return ActionCall::none();
}
Cells getNpcAiStyle(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).combat.npcAiStyle);
}
Cells setNpcAiStyle(ActionCall& call)
{
    call.api().actor(call.objectArg(1)).combat.npcAiStyle = static_cast<std::int32_t>(call.intArg(0));
    return ActionCall::none();
}

// ---- talents -----------------------------------------------------------------

Cells talentOf(ActionCall& call)
{
    const std::int32_t type = call.id == 301 ? 0 : call.id == 302 ? 1 : 2;
    return ActionCall::retEngine(
        CombatOps{call.session}.talentRecord(type, static_cast<std::int32_t>(call.intArg(0))));
}

Cells getIsTalentValid(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    if (talent != nullptr) {
        return ActionCall::retInt(talent->id >= 0 ? 1 : 0);
    }
    // An old int-encoded talent from a legacy path, or the zero of "no talent".
    return call.engineArg(0) != 0 ? Cells{std::nullopt} : ActionCall::retInt(0);
}

Cells getTypeFromTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    return talent != nullptr ? ActionCall::retInt(talent->type) : Cells{std::nullopt};
}

Cells getIdFromTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    return talent != nullptr ? ActionCall::retInt(talent->id) : Cells{std::nullopt};
}

// ACT_735 GetCategoryFromTalent: the table's category column.
Cells getCategoryFromTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    if (talent == nullptr || talent->type < 0 || talent->type > 1) {
        return ActionCall::retInt(-1);
    }
    return ActionCall::retInt(call.session.rules().talentInt(talent->type == 0, talent->id, "category"));
}

Cells getCreatureHasTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    if (talent == nullptr) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(
        CombatOps{call.session}.hasTalent(call.objectArg(1), talent->type, talent->id) ? 1 : 0);
}

// ACT_307/308 00536fe0 -> 005b23a0
Cells getCreatureTalent(ActionCall& call)
{
    CombatOps ops{call.session};
    const bool best = call.id == 308;
    const std::int32_t category = static_cast<std::int32_t>(call.intArg(0));
    const std::int32_t crMax = best ? static_cast<std::int32_t>(call.intArg(1)) : -1;
    const std::uint32_t object = call.objectArg(best ? 2 : 1);
    const std::int32_t excludeType = best ? static_cast<std::int32_t>(call.intArg(4, -1)) : -1;
    const std::int32_t excludeId = best ? static_cast<std::int32_t>(call.intArg(5, -1)) : -1;
    const auto [type, id] = ops.pickTalent(object, category, crMax, !best, excludeType, excludeId);
    if (id < 0) {
        return ActionCall::retEngine(0);
    }
    return ActionCall::retEngine(ops.talentRecord(type, id));
}

void queueCast(ActionCall& call, std::int32_t spell, std::uint32_t target, std::int64_t location,
               bool fake)
{
    ActorAction action;
    action.kind = ActorActionKind::CastSpell;
    action.spell = spell;
    action.object = target;
    action.location = location;
    action.fake = fake;
    call.api().enqueue(call.self(), std::move(action));
}

// ACT_48 ActionCastSpellAtObject. The PC's powers keep their engine implementation
// in GameSession::useForcePower (the legacy case); everyone else runs the shipped
// impact script from spells.2da, which is what retail does (k_sp1_generic).
Cells actionCastSpellAtObject(ActionCall& call)
{
    if (call.self() == call.api().pc()) {
        return std::nullopt;
    }
    queueCast(call, static_cast<std::int32_t>(call.intArg(0)), call.objectArgOrInvalid(1), 0, false);
    return ActionCall::none();
}

Cells actionCastSpellAtLocation(ActionCall& call)
{
    queueCast(call, static_cast<std::int32_t>(call.intArg(0)), kInvalid, call.engineArg(1), false);
    return ActionCall::none();
}

// ACT_501/502: the visuals without cost or effect.
Cells actionCastFakeSpellAtObject(ActionCall& call)
{
    queueCast(call, static_cast<std::int32_t>(call.intArg(0)), call.objectArgOrInvalid(1), 0, true);
    return ActionCall::none();
}

Cells actionCastFakeSpellAtLocation(ActionCall& call)
{
    queueCast(call, static_cast<std::int32_t>(call.intArg(0)), kInvalid, call.engineArg(1), true);
    return ActionCall::none();
}

// ACT_309/310
Cells actionUseTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    if (talent == nullptr) {
        return std::nullopt; // Int-encoded legacy talent.
    }
    const std::int32_t type = talent->type;
    const std::int32_t id = talent->id;
    const std::uint32_t target = call.id == 309 ? call.objectArgOrInvalid(1) : kInvalid;
    const std::int64_t location = call.id == 310 ? call.engineArg(1) : 0;
    if (call.self() == call.api().pc()) {
        if (type == 0) {
            call.session.useForcePower(id, target);
        } else if (type == 1) {
            call.session.useFeat(id, target);
        }
        return ActionCall::none();
    }
    if (type == 0) {
        queueCast(call, id, target, location, false);
    } else if (type == 1) {
        const std::int32_t spell = call.session.rules().talentInt(false, id, "spellid", -1);
        if (spell >= 0) {
            queueCast(call, spell, target, location, false);
        } else if (target != kInvalid) {
            ActorAction action;
            action.kind = ActorActionKind::Attack;
            action.object = target;
            action.spell = id;
            call.api().enqueue(call.self(), std::move(action));
        }
    } else {
        call.api().log("ActionUseTalent: skill talents are not simulated");
    }
    return ActionCall::none();
}

// Per-creature for everyone; the PC answers from its sheet.
Cells getHasFeat(ActionCall& call)
{
    const std::int32_t feat = static_cast<std::int32_t>(call.intArg(0));
    return ActionCall::retInt(CombatOps{call.session}.knows(call.objectArg(1), false, feat) ? 1 : 0);
}

Cells getHasSpell(ActionCall& call)
{
    const std::int32_t spell = static_cast<std::int32_t>(call.intArg(0));
    return ActionCall::retInt(CombatOps{call.session}.knows(call.objectArg(1), true, spell) ? 1 : 0);
}

Cells getCurrentForcePoints(ActionCall& call)
{
    return ActionCall::retInt(CombatOps{call.session}.forcePoints(call.objectArg(0)));
}

Cells getMaxForcePoints(ActionCall& call)
{
    const std::uint32_t object = call.objectArg(0);
    ScriptApi api = call.api();
    if (object == api.pc()) {
        return std::nullopt;
    }
    const std::size_t index = api.entityFor(object);
    return ActionCall::retInt(index != kNone ? api.scene().entities[index].stats.maxForcePoints : 0);
}

// ---- spell context -----------------------------------------------------------

Cells getSpellId(ActionCall& call)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find("spellId");
    if (it != context.end()) {
        return ActionCall::retInt(static_cast<std::int32_t>(it->second));
    }
    return ActionCall::retInt(-1);
}

Cells getSpellTargetLocation(ActionCall& call)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find("spellLocation");
    return ActionCall::retEngine(it != context.end() ? static_cast<std::int64_t>(it->second) : 0);
}

Cells getSpellTarget(ActionCall& call)
{
    return ActionCall::retObject(call.api().actor(call.objectArg(0)).combat.lastSpellTarget);
}

// ACT_111 0053d320 reads the caster's save DC through 004ef6e0; its spell-level
// input was not resolved, so the PC's formula stands in for the PC and 10 + half
// hit dice + Wisdom modifier for others. Stated in the report.
Cells getSpellSaveDc(ActionCall& call)
{
    const std::uint32_t self = call.self();
    ScriptApi api = call.api();
    if (self == api.pc()) {
        return ActionCall::retInt(call.session.rules().forcePowerSaveDC(call.session.character()));
    }
    const std::size_t index = api.entityFor(self);
    if (index == kNone) {
        return ActionCall::retInt(14);
    }
    const kotor::CreatureStats& stats = api.scene().entities[index].stats;
    std::int32_t levels = 0;
    for (const auto& [klass, level] : stats.classes) {
        levels += level;
    }
    return ActionCall::retInt(10 + levels / 2 + modifier(stats.abilities[4]));
}

// ---- saves -------------------------------------------------------------------

Cells savingThrow(ActionCall& call)
{
    const std::int32_t which = call.id == 108 ? 1 : call.id == 109 ? 2 : 3;
    return ActionCall::retInt(CombatOps{call.session}.savingThrow(
        call.objectArg(0), which, static_cast<std::int32_t>(call.intArg(1))));
}

Cells savingThrowValue(ActionCall& call)
{
    const std::int32_t which = call.id == 491 ? 1 : call.id == 493 ? 2 : 3;
    return ActionCall::retInt(CombatOps{call.session}.saveBase(call.objectArg(0), which));
}

// ACT_299: half on a Reflex success. Evasion feats are not modelled.
Cells getReflexAdjustedDamage(ActionCall& call)
{
    const std::int32_t damage = static_cast<std::int32_t>(call.intArg(0));
    const std::int32_t saved = CombatOps{call.session}.savingThrow(
        call.objectArgOrInvalid(1), 2, static_cast<std::int32_t>(call.intArg(2)));
    return ActionCall::retInt(saved != 0 ? damage / 2 : damage);
}

// ---- damage reads ------------------------------------------------------------

Cells getTotalDamageDealt(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.self()).combat.lastDamage);
}

Cells getDamageDealtByType(ActionCall& call)
{
    const auto& types = call.api().actor(call.self()).combat.lastDamageByType;
    const auto it = types.find(static_cast<std::int32_t>(call.intArg(0)));
    return ActionCall::retInt(it != types.end() ? it->second : 0);
}

// ---- surrender ---------------------------------------------------------------

// ACT_736 00546340: every creature of the faction moves to another and stops.
Cells surrenderByFaction(ActionCall& call)
{
    ScriptApi api = call.api();
    CombatOps ops{call.session};
    const std::uint32_t from = static_cast<std::uint32_t>(call.intArg(0));
    const std::uint32_t to = static_cast<std::uint32_t>(call.intArg(1));
    for (std::size_t i = 0; i < api.scene().entities.size(); ++i) {
        kotor::Entity& entity = api.scene().entities[i];
        if (entity.kind != kotor::EntityKind::Creature || entity.faction != from) {
            continue;
        }
        const std::uint32_t object = api.objectFor(i);
        call.session.setObjectFaction(object, static_cast<int>(to));
        api.clearQueue(object);
        ops.leaveCombat(object);
    }
    return ActionCall::none();
}

// ACT_476/762 00544830 -> 00518990, ACT_379 queues the same. The creature's
// reaction beyond stopping (buff handling inside 00518990) was not traced.
Cells surrenderToEnemies(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t self = call.self();
    api.clearQueue(self);
    CombatOps{call.session}.leaveCombat(self);
    return ActionCall::none();
}

// ---- stealth XP --------------------------------------------------------------

Cells getStealthXp(ActionCall& call)
{
    const WorldSim& sim = call.api().sim();
    switch (call.id) {
    case 464:
        return ActionCall::retInt(sim.stealthXpMax);
    case 474:
        return ActionCall::retInt(sim.stealthXpCurrent);
    case 481:
        return ActionCall::retInt(sim.stealthXpEnabled ? 1 : 0);
    default:
        return ActionCall::retInt(sim.stealthXpDecrement);
    }
}

Cells setStealthXp(ActionCall& call)
{
    WorldSim& sim = call.api().sim();
    const std::int32_t value = static_cast<std::int32_t>(call.intArg(0));
    switch (call.id) {
    case 468:
        sim.stealthXpMax = value;
        break;
    case 478:
        sim.stealthXpCurrent = std::clamp(value, 0, sim.stealthXpMax);
        break;
    case 482:
        sim.stealthXpEnabled = value != 0;
        break;
    default:
        sim.stealthXpDecrement = value;
        break;
    }
    return ActionCall::none();
}

// ACT_480 00546c80 -> 00508770: the area award. The per-target spend inside was
// not traced; the current pool is awarded once and spent by the decrement.
Cells awardStealthXp(ActionCall& call)
{
    WorldSim& sim = call.api().sim();
    if (sim.stealthXpEnabled && sim.stealthXpCurrent > 0) {
        call.session.giveXp(sim.stealthXpCurrent);
        sim.stealthXpCurrent = std::max(0, sim.stealthXpCurrent - sim.stealthXpDecrement);
    }
    return ActionCall::none();
}

} // namespace

// ---- the family gate ----------------------------------------------------------

int CombatOps::gate()
{
    using Value = script::Value;
    using Args = std::vector<std::vector<Value>>;
    bool ok = true;
    const auto check = [&ok](bool condition, const std::string& label) {
        logInfo(concat("combat family: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    ScriptApi a = api();
    script::Vm dummy({});
    const std::uint32_t pc = s.scriptWorld_.pc;
    const auto tickUntil = [&](float seconds, const auto& done) {
        for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f) {
            if (s.dialogueView_.active) {
                s.endDialogue();
            }
            s.update(1.0f / 30.0f, InputState{});
            if (done()) {
                return true;
            }
        }
        return false;
    };
    const auto tick = [&](float seconds) {
        tickUntil(seconds, [] { return false; });
    };
    const auto obj = [](std::uint32_t id) { return std::vector<Value>{Value::makeObject(id)}; };
    const auto num = [](std::int64_t value) { return std::vector<Value>{Value::makeInt(value)}; };
    const auto real = [](double value) { return std::vector<Value>{Value::makeFloat(value)}; };
    const auto eng = [](std::int64_t value) { return std::vector<Value>{Value::makeEngine(value)}; };
    // Every handler runs as the VM would dispatch it, with self in the context.
    const auto call = [&](std::uint32_t self, std::uint16_t id, Args args) {
        s.runtime_->setContext({{"self", self}});
        return s.action(id, args, s.scriptWorld_, dummy).value_or(std::vector<Value>{});
    };
    const auto callInt = [&](std::uint32_t self, std::uint16_t id, Args args) -> std::int64_t {
        const std::vector<Value> out = call(self, id, std::move(args));
        return out.empty() ? -9999 : out.front().asInt();
    };
    const auto makeEffect = [&](std::uint16_t id, Args args) -> std::int64_t {
        const std::vector<Value> out = call(pc, id, std::move(args));
        return out.empty() ? 0 : out.front().integer;
    };
    constexpr int kInstant = 0;
    constexpr int kTemporary = 1;
    constexpr int kPermanent = 2;
    const auto apply = [&](std::uint32_t creator, int duration, std::int64_t effectId,
                           std::uint32_t target, double seconds) {
        call(creator, 220, {num(duration), eng(effectId), obj(target), real(seconds)});
    };
    const auto logHas = [&](const std::string& text) {
        return std::any_of(s.combatLog_.begin(), s.combatLog_.end(), [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    };
    const auto scriptLogHas = [&](const std::string& text) {
        return std::any_of(s.scriptLog_.begin(), s.scriptLog_.end(), [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    };

    // A standing spot in plain sight of `center`, so a test never starts in a wall.
    const auto openSpot = [&](kotor::Vec3 center, float radius) {
        for (int step = 0; step < 16; ++step) {
            const float angle = static_cast<float>(step) * 0.3927f;
            kotor::Vec3 spot{center.x + std::cos(angle) * radius,
                             center.y + std::sin(angle) * radius, center.z};
            const nav::NavMesh* mesh = s.navMesh();
            const int tri = mesh != nullptr ? mesh->locate(spot) : -1;
            if (tri >= 0) {
                spot.z = mesh->height(tri, spot.x, spot.y);
                if (std::abs(spot.z - center.z) < 1.0f && s.lineOfSight(center, spot)) {
                    return spot;
                }
            }
        }
        return center;
    };

    // The fixture only needs a creature that a shipped UTC makes an enemy and that
    // carries the shipped perception hook.
    std::size_t hostileIndex = kNone;
    for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
        const kotor::Entity& entity = s.scene_.entities[i];
        const auto hook = entity.scripts.find("ScriptOnNotice");
        if (entity.kind == kotor::EntityKind::Creature && !entity.dead
            && hook != entity.scripts.end() && !hook->second.empty()
            && isEnemy(a, pc, a.objectFor(i))) {
            hostileIndex = i;
            break;
        }
    }
    check(hostileIndex != kNone, "hostile_with_ai_found");
    if (hostileIndex == kNone) {
        return 1;
    }
    s.playerMaxHp_ = 100000;
    s.playerHp_ = 100000;
    s.character_.maxHp = 100000;
    s.character_.hp = 100000;
    const std::uint32_t hostile = a.objectFor(hostileIndex);
    const std::string resref = s.scene_.entities[hostileIndex].templateResref;
    logInfo(concat("combat family: fixture ", s.scene_.entities[hostileIndex].tag, " resref=", resref,
                   " notice=", s.scene_.entities[hostileIndex].scripts["ScriptOnNotice"]));

    // ---- stats come from the sheet -----------------------------------------
    {
        const kotor::Entity& e = s.scene_.entities[hostileIndex];
        std::int32_t bab = 0;
        std::int32_t levels = 0;
        for (const auto& [klass, level] : e.stats.classes) {
            bab += s.rules_.baseAttackBonus(klass, level);
            levels += level;
        }
        logInfo(concat("combat family: sheet levels=", levels, " bab=", e.stats.baseAttack,
                       " attack=", e.attackBonus, " ac=", e.defense, " dmg=", e.damageMin, "-",
                       e.damageMax, " weapon=", e.stats.mainHand.baseItem, " ", e.stats.mainHand.dice,
                       "d", e.stats.mainHand.die, " cr=", e.stats.challengeRating));
        check(e.stats.combatDerived, "sheet_derived");
        check(e.stats.baseAttack == bab, "bab_from_class_tables");
        check(e.defense >= 10 + e.stats.naturalAC + std::min(0, modifier(e.stats.abilities[1])),
              "ac_from_sheet");
    }

    // ---- shipped AI: perception -> k_ai_master -> ActionAttack -> damage ----
    {
        s.setPositionForTest(openSpot(s.scene_.entities[hostileIndex].position, 4.0f));
        s.syncPlayerToScriptWorld();
        const std::int32_t before = s.playerHp_;
        bool attackQueued = false;
        const bool hurt = tickUntil(90.0f, [&] {
            for (const ActorAction& action : a.actor(hostile).queue) {
                attackQueued = attackQueued
                    || (action.kind == ActorActionKind::Attack && action.object == pc);
            }
            return s.playerHp_ < before;
        });
        check(creatureHasPerceived(a, hostile, pc, 1), "ai_perceived_pc");
        check(scriptLogHas("k_ai_master"), "ai_master_ran");
        check(attackQueued, "ai_issued_attack_on_pc");
        check(hurt, "pc_took_damage_through_real_path");
        check(logHas("hits player"), "attack_hit_logged");
        check(a.actor(hostile).combat.inCombat, "attacker_in_combat");
        check(callInt(hostile, 320, {obj(hostile)}) == 1, "get_is_in_combat");
        check(callInt(hostile, 361, {}) == pc || callInt(hostile, 721, {obj(hostile)}) == pc,
              "attempted_or_last_hostile_target");
        tickUntil(8.0f, [&] { return scriptLogHas("hook ScriptEndRound"); });
        check(scriptLogHas("hook ScriptEndRound"), "end_round_hook");
    }

    // Everyone stands down; the PC becomes unperceivable for the NPC-only tests.
    const auto standDown = [&] {
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            if (s.scene_.entities[i].kind == kotor::EntityKind::Creature) {
                call(pc, 54, {obj(a.objectFor(i))});
            }
        }
        call(pc, 54, {obj(pc)});
    };
    const std::int64_t invisibility = makeEffect(457, {num(0)});
    apply(pc, kPermanent, invisibility, pc, 0.0);
    standDown();

    // ---- two NPC factions fight ---------------------------------------------
    const kotor::Vec3 arena = s.scene_.entities[hostileIndex].position;
    s.setPositionForTest({arena.x + 60.0f, arena.y, arena.z});
    s.syncPlayerToScriptWorld();
    s.scene_.entities[hostileIndex].position.x += 200.0f; // Out of the fight.
    s.syncEntityToScriptWorld(hostileIndex);
    const std::uint32_t red = a.spawnCreature(resref, {arena.x + 1.5f, arena.y, arena.z}, 0.0f);
    const std::uint32_t blue = a.spawnCreature(resref, {arena.x - 1.5f, arena.y, arena.z}, 3.14f);
    check(red != kInvalid && blue != kInvalid, "npc_fighters_spawned");
    s.setObjectFaction(red, 1);
    s.setObjectFaction(blue, 3);
    check(s.factions_.reputation(1, 3) <= 10, "factions_hostile_in_repute");
    check(callInt(red, 235, {obj(blue), obj(red)}) == 1, "get_is_enemy_between_npcs");
    check(callInt(red, 237, {obj(blue), obj(red)}) == 0, "not_neutral_between_npcs");
    const auto hpOf = [&](std::uint32_t object) { return hitPoints(object); };
    const std::int32_t redBefore = hpOf(red);
    const std::int32_t blueBefore = hpOf(blue);
    const std::int32_t xpBeforeFight = s.character_.xp;
    const bool fought = tickUntil(120.0f, [&] {
        return hpOf(red) < redBefore || hpOf(blue) < blueBefore;
    });
    check(fought, "npc_factions_fought");
    check(a.actor(red).combat.lastHostileActor == blue
              || a.actor(blue).combat.lastHostileActor == red,
          "npc_attacked_by_other_faction");
    check(scriptLogHas("hook ScriptDamaged") || scriptLogHas("hook ScriptAttacked"),
          "damage_or_attacked_hook_ran");
    // Finish it through combat: low hit points, so a kill follows quickly.
    if (kotor::Entity* e = entity(red); e != nullptr && !e->dead) {
        e->hitPoints = std::min(e->hitPoints, 2);
    }
    if (kotor::Entity* e = entity(blue); e != nullptr && !e->dead) {
        e->hitPoints = std::min(e->hitPoints, 2);
    }
    const bool oneDied = tickUntil(180.0f, [&] { return !a.isAlive(red) || !a.isAlive(blue); });
    check(oneDied, "npc_killed_by_npc");
    (void)xpBeforeFight;
    const std::uint32_t survivor = a.isAlive(red) ? red : blue;
    const std::uint32_t corpse = a.isAlive(red) ? blue : red;
    standDown();

    // ---- plot and Min1HP -----------------------------------------------------
    {
        const std::int32_t hp = hpOf(survivor);
        call(pc, 456, {obj(survivor), num(1)});
        check(callInt(pc, 455, {obj(survivor)}) == 1, "set_plot_flag");
        apply(pc, kInstant, makeEffect(79, {num(100000), num(8), num(0)}), survivor, 0.0);
        check(a.isAlive(survivor) && hpOf(survivor) == hp, "plot_survives_lethal_damage");
        apply(pc, kInstant, makeEffect(133, {num(0), num(1)}), survivor, 0.0);
        check(a.isAlive(survivor), "plot_survives_effect_death");
        call(pc, 456, {obj(survivor), num(0)});
        call(pc, 716, {obj(survivor), num(1)});
        check(callInt(pc, 715, {obj(survivor)}) == 1, "set_min_one_hp");
        apply(pc, kInstant, makeEffect(79, {num(100000), num(8), num(0)}), survivor, 0.0);
        check(a.isAlive(survivor) && hpOf(survivor) == 1, "min1hp_holds_at_one");
        call(pc, 716, {obj(survivor), num(0)});
    }

    // ---- effects apply and expire ---------------------------------------------
    {
        const std::int32_t ac = armorClass(survivor);
        const std::int32_t attack = profile(survivor, false, -1).attack;
        const std::int64_t acUp = makeEffect(115, {num(5), num(0), num(0)});
        apply(pc, kTemporary, acUp, survivor, 2.0);
        check(armorClass(survivor) == ac + 5, "ac_effect_applies");
        const std::int64_t first = call(pc, 85, {obj(survivor)}).front().integer;
        check(callInt(pc, 88, {eng(first)}) == 1, "first_effect_valid");
        check(callInt(pc, 170, {eng(first)}) == 46, "effect_type_ac_increase");
        check(callInt(pc, 89, {eng(first)}) == kTemporary, "effect_duration_type");
        check(callInt(pc, 88, {eng(call(pc, 86, {obj(survivor)}).front().integer)}) == 0,
              "next_effect_invalid");
        apply(pc, kTemporary, makeEffect(118, {num(3), num(0)}), survivor, 2.0);
        check(profile(survivor, false, -1).attack == attack + 3, "attack_effect_applies");
        tick(3.0f);
        check(armorClass(survivor) == ac, "ac_effect_expires");
        check(profile(survivor, false, -1).attack == attack, "attack_effect_expires");

        check(callInt(pc, 732, {obj(survivor)}) == 0, "not_debilitated");
        apply(pc, kTemporary, makeEffect(161, {}), survivor, 1.0);
        check(callInt(pc, 732, {obj(survivor)}) == 1, "stun_debilitates");
        check(armorClass(survivor) <= ac - 4, "stun_lowers_ac");
        tick(1.5f);
        check(callInt(pc, 732, {obj(survivor)}) == 0, "stun_expires");

        const std::int64_t held = makeEffect(115, {num(2), num(0), num(0)});
        apply(pc, kPermanent, held, survivor, 0.0);
        check(armorClass(survivor) == ac + 2, "permanent_effect_holds");
        call(pc, 87, {obj(survivor), eng(held)});
        check(armorClass(survivor) == ac, "remove_effect");

        const std::int64_t linked = call(pc, 199, {eng(makeEffect(115, {num(1), num(0), num(0)})),
                                                   eng(makeEffect(118, {num(1), num(0)}))})
                                        .front()
                                        .integer;
        apply(pc, kPermanent, linked, survivor, 0.0);
        check(armorClass(survivor) == ac + 1 && profile(survivor, false, -1).attack == attack + 1,
              "linked_effects_apply");
        call(survivor, 710, {});
        check(armorClass(survivor) == ac, "clear_all_effects");

        entity(survivor)->hitPoints = 20;
        apply(pc, kTemporary, makeEffect(314, {num(50)}), survivor, 30.0);
        apply(pc, kInstant, makeEffect(79, {num(10), num(8), num(0)}), survivor, 0.0);
        check(hpOf(survivor) == 20, "temporary_hp_absorbs");
        call(survivor, 710, {});
    }

    // ---- saving throws --------------------------------------------------------
    check(callInt(survivor, 108, {obj(survivor), num(-1000), num(0), obj(survivor)}) == 1, "save_low_dc");
    check(callInt(survivor, 108, {obj(survivor), num(1000), num(0), obj(survivor)}) == 0, "save_high_dc");

    // ---- XP from retail's table ------------------------------------------------
    {
        // A fresh enemy; the expected award comes straight from the shipped 2DAs.
        const std::uint32_t mark = a.spawnCreature(resref, {arena.x, arena.y + 2.0f, arena.z}, 0.0f);
        s.setObjectFaction(mark, 1);
        tick(0.2f);
        const kotor::Entity& e = *entity(mark);
        s.character_.xp = 0;
        const std::int32_t level = s.rules_.levelForXp(0);
        auto table = a.install().find("xptable", 2017);
        check(table.has_value(), "xptable_loaded");
        const kotor::TwoDa xptable = kotor::TwoDa::parse(std::move(*table), "xptable");
        const std::int32_t expected = std::atoi(
            xptable.cell(std::to_string(level - 1),
                         "c" + std::to_string(static_cast<int>(e.stats.challengeRating))).c_str());
        const std::int32_t xpBefore = s.character_.xp;
        apply(pc, kInstant, makeEffect(133, {num(0), num(1)}), mark, 0.0);
        check(!a.isAlive(mark), "effect_death_kills");
        check(callInt(pc, 140, {obj(mark)}) == 1, "get_is_dead");
        logInfo(concat("combat family: xp expected=", expected, " got=", s.character_.xp - xpBefore));
        check(expected > 0 && s.character_.xp - xpBefore == expected, "xp_matches_retail_table");
        tick(0.5f);
        check(scriptLogHas("hook ScriptDeath"), "death_hook_ran");

        // Resurrection and heal.
        apply(pc, kInstant, makeEffect(82, {}), mark, 0.0);
        check(a.isAlive(mark) && callInt(pc, 140, {obj(mark)}) == 0, "effect_resurrection_revives");
        check(hpOf(mark) == 1, "resurrection_at_one_hp");
        apply(pc, kInstant, makeEffect(78, {num(5)}), mark, 0.0);
        check(hpOf(mark) == 6 || hpOf(mark) == entity(mark)->maxHitPoints, "effect_heal_heals");
        const std::int32_t xpAfter = s.character_.xp;
        // A neutral creature's death pays nothing. retail 004fb1e0
        s.setObjectFaction(mark, 5);
        apply(pc, kInstant, makeEffect(133, {num(0), num(1)}), mark, 0.0);
        check(!a.isAlive(mark) && s.character_.xp == xpAfter, "no_xp_for_non_enemy");
    }
    (void)corpse;

    // ---- the PC's own attack command --------------------------------------------
    {
        call(pc, 87, {obj(pc), eng(invisibility)});
        s.scene_.entities[hostileIndex].position = {arena.x, arena.y, arena.z};
        s.syncEntityToScriptWorld(hostileIndex);
        s.scene_.entities[hostileIndex].plot = true;
        // A shipped script on this module's soldiers calls CancelCombat on the PC, which
        // retail 004fdaa0 honours by dropping the queue. This part tests the command, so
        // creature hooks are silenced and everything else stands far off.
        for (kotor::Entity& other : s.scene_.entities) {
            if (other.kind == kotor::EntityKind::Creature) {
                other.scripts.clear();
            }
        }
        // "Nearest enemy" has to be this creature.
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            kotor::Entity& other = s.scene_.entities[i];
            if (i != hostileIndex && other.kind == kotor::EntityKind::Creature && !other.dead
                && isEnemy(a, pc, a.objectFor(i))) {
                other.position.x += 500.0f;
                s.syncEntityToScriptWorld(i);
            }
        }
        s.setPositionForTest(openSpot(arena, 5.0f));
        s.syncPlayerToScriptWorld();
        const kotor::Vec3 start = s.position_;
        s.combatLog_.clear();
        s.attackNearestHostile();
        check(s.combatTarget_ != UINT32_MAX, "attack_command_sets_target");
        const bool attacked = tickUntil(30.0f, [&] {
            return logHas("player hits") || logHas("player misses");
        });
        check(attacked, "pc_attacks_after_command");
        check(logHas("round "), "pc_round_logged");
        check(std::hypot(s.position_.x - start.x, s.position_.y - start.y) > 0.5f, "pc_walked_to_range");
        InputState steer;
        steer.forward = true;
        s.update(1.0f / 30.0f, steer);
        s.update(1.0f / 30.0f, InputState{});
        check(s.combatTarget_ == UINT32_MAX, "steering_cancels_attack");
        s.scene_.entities[hostileIndex].plot = false;
    }

    // ---- companions fight through the same path -----------------------------------
    // A party member is an ordinary target of the damage path (the party family
    // covers forming a party; a shipped module without one has no avatar to test).
    for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
        if (s.scene_.entities[i].partyAvatar && !s.scene_.entities[i].dead) {
            const std::uint32_t member = a.objectFor(i);
            apply(hostile, kInstant, makeEffect(79, {num(1000000), num(8), num(0)}), member, 0.0);
            check(!a.isAlive(member), "companion_can_be_killed");
            break;
        }
    }

    logInfo(concat("combat family: ", ok ? "PASS" : "FAIL"));
    return ok ? 0 : 1;
}

int runCombatFamilyTest(GameSession& game)
{
    return CombatOps{game}.gate();
}

void registerCombatActions(HandlerTable& table)
{
    table.add(169, resistForce);
    table.add(274, getIsImmune);
    table.add(366, displayFeedBackText);
    table.add(390, getIsLinkImmune);
    table.add(503, actionCutsceneAttack);
    table.add(751, getIsPoisoned);
    table.add(37, actionAttack);
    table.add(48, actionCastSpellAtObject);
    table.add(54, cancelCombat);
    table.add(55, getCurrentForcePoints);
    table.add(56, getMaxForcePoints);
    table.add(108, savingThrow);
    table.add(109, savingThrow);
    table.add(110, savingThrow);
    table.add(111, getSpellSaveDc);
    table.add(140, getIsDead);
    table.add(211, getGoingToBeAttackedBy);
    table.add(222, getSpellTargetLocation);
    table.add(234, actionCastSpellAtLocation);
    table.add(235, getReputationBand);
    table.add(236, getReputationBand);
    table.add(237, getReputationBand);
    table.add(248, getSpellId);
    table.add(285, getHasFeat);
    table.add(289, getObjectPerceived);
    table.add(290, getObjectPerceived);
    table.add(299, getReflexAdjustedDamage);
    table.add(301, talentOf);
    table.add(302, talentOf);
    table.add(303, talentOf);
    table.add(306, getCreatureHasTalent);
    table.add(307, getCreatureTalent);
    table.add(308, getCreatureTalent);
    table.add(309, actionUseTalent);
    table.add(310, actionUseTalent);
    table.add(316, getAttackTarget);
    table.add(320, getIsInCombat);
    table.add(344, getDamageDealtByType);
    table.add(345, getTotalDamageDealt);
    table.add(346, getLastAttackerOrDamager);
    table.add(359, getIsTalentValid);
    table.add(361, getAttemptedAttackTarget);
    table.add(362, getTypeFromTalent);
    table.add(363, getIdFromTalent);
    table.add(375, getAttemptedSpellTarget);
    table.add(377, getHasSpell);
    table.add(379, surrenderToEnemies);
    table.add(455, getPlotFlag);
    table.add(456, setPlotFlag);
    table.add(464, getStealthXp);
    table.add(468, setStealthXp);
    table.add(474, getStealthXp);
    table.add(476, surrenderToEnemies);
    table.add(478, setStealthXp);
    table.add(480, awardStealthXp);
    table.add(481, getStealthXp);
    table.add(482, setStealthXp);
    table.add(491, savingThrowValue);
    table.add(492, savingThrowValue);
    table.add(493, savingThrowValue);
    table.add(498, getStealthXp);
    table.add(499, setStealthXp);
    table.add(501, actionCastFakeSpellAtObject);
    table.add(502, actionCastFakeSpellAtLocation);
    table.add(704, getPartyAiStyle);
    table.add(705, getNpcAiStyle);
    table.add(706, setPartyAiStyle);
    table.add(707, setNpcAiStyle);
    table.add(715, getMinOneHp);
    table.add(716, setMinOneHp);
    table.add(721, getLastHostileTarget);
    table.add(722, getLastAttackAction);
    table.add(723, getLastForcePowerUsed);
    table.add(724, getLastCombatFeatUsed);
    table.add(725, getLastAttackResult);
    table.add(726, getWasForcePowerSuccessful);
    table.add(731, setForcePowerUnsuccessful);
    table.add(732, getIsDebilitated);
    table.add(735, getCategoryFromTalent);
    table.add(736, surrenderByFaction);
    table.add(752, getSpellTarget);
    table.add(762, surrenderToEnemies);
}

} // namespace ke
