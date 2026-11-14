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
constexpr float kPerceptionInterval = 1.0f; // stationary sampling for 004f1fd0; assumed 1 s.
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

bool holdsImmunity(ScriptApi api, std::uint32_t object, std::int32_t immunity);

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
        std::int32_t critBase = 1; // baseitems critthreat; 1 unarmed (005a5130)
        bool ranged = false;
        bool noDamage = false; // item property 31: no base dice (retail 005a9050)
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
        // 004f3fe0 case 4: the increases are capped at 20 (0x14) and the decreases at 30 (0x1e).
        std::int32_t gain = 0;
        std::int32_t loss = 0;
        ScriptApi a = api();
        for (const AppliedEffect& held : a.actor(object).effects) {
            if (held.type != 38 && held.type != 39) {
                continue;
            }
            const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
            if (data != nullptr && data->args.size() > 1 && data->args[0].asInt() == ability) {
                const std::int32_t change = static_cast<std::int32_t>(data->args[1].asInt());
                (held.type == 38 ? gain : loss) += change;
            }
        }
        return std::min(gain, 20) - std::min(loss, 30);
    }

    std::int32_t effectNet(std::uint32_t object, std::int32_t increase, std::int32_t decrease) const
    {
        return effectTotal(api(), object, increase) - effectTotal(api(), object, decrease);
    }

    // 004f3fe0 GetTotalEffectBonus, case 1 (attack): walks the effects of the increase (40) and the
    // decrease (41) type. An effect counts when its modifier type (arg 1) is ATTACK_BONUS_MISC (0)
    // or the swinging hand (1 on hand, 2 off hand). A MISC effect that names a spell (+0x1c) stacks
    // per spell: only the largest amount of each spell id counts; every other effect adds. The
    // increases and the decreases are each capped at 20 (0x14), then subtracted.
    std::int32_t attackEffectNet(std::uint32_t object, std::int32_t hand) const
    {
        ScriptApi a = api();
        const auto side = [&](std::int32_t type) {
            std::int32_t sum = 0;
            std::map<std::int32_t, std::int32_t> perSpell;
            for (const AppliedEffect& held : a.actor(object).effects) {
                if (held.type != type) {
                    continue;
                }
                const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
                if (data == nullptr || data->args.empty()) {
                    continue;
                }
                const std::int32_t amount = static_cast<std::int32_t>(data->args[0].asInt());
                const std::int32_t mode = data->args.size() > 1 ? static_cast<std::int32_t>(data->args[1].asInt()) : 0;
                if (mode != 0 && mode != hand) {
                    continue;
                }
                if (mode == 0 && held.spellId != -1) {
                    auto [it, added] = perSpell.try_emplace(held.spellId, amount);
                    if (!added) {
                        it->second = std::max(it->second, amount);
                    }
                } else {
                    sum += amount;
                }
            }
            for (const auto& [spell, amount] : perSpell) {
                sum += amount;
            }
            return std::min(sum, 20);
        };
        return side(40) - side(41);
    }

    // 004f3fe0 GetTotalEffectBonus: the increases and the decreases are each capped before they
    // are subtracted: attack at 20 (0x14), damage at 36 (0x24).
    std::int32_t effectNetCapped(std::uint32_t object, std::int32_t increase, std::int32_t decrease,
                                 std::int32_t cap) const
    {
        return std::min(effectTotal(api(), object, increase), cap) - std::min(effectTotal(api(), object, decrease), cap);
    }

    // retail 005ad0e0 for AC, 004ed1d0 for the debilitated adjustment.
    // An attacker the defender has not seen (005ad0e0: visible-list entry bit 0) takes the
    // positive Dex modifier off, as for a debilitated defender; a negative one stays.
    std::int32_t armorClass(std::uint32_t object, std::uint32_t attacker = kInvalid) const
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
        const bool unseen = attacker != kInvalid && attacker != object
            && !creatureHasPerceived(api(), object, attacker, 1);
        if (isDebilitated(api(), object)) {
            ac -= 4;
        }
        if ((isDebilitated(api(), object) || unseen) && dexMod > 0) {
            ac -= dexMod; // Only a negative Dex counts while disabled or against an unseen attacker.
        }
        return ac;
    }

    // Special attacks by feat.2da row, the numbers retail keys on the attack's feat id:
    // attack 005a4ec0, damage 005a4fb0, threat 005a5130 (adds critthreat x rank), extra
    // on-hand attack 004d2a10, and the post-attack effects of 005ba8e0 (melee) and
    // 005ba540 (ranged): a 3 s dodge AC decrease on the attacker and, for the Critical
    // Strike / Sniper Shot families, a 6 s stun on a hit unless the target makes a
    // Fortitude save against attacker level + STR (melee) or INT (ranged) modifier.
    // K1's Master Flurry is WHIRLWIND_ATTACK (53) and Master Rapid Shot MULTI_SHOT (26).
    struct SpecialAttack {
        std::int32_t attack = 0;
        std::int32_t damage = 0;
        std::int32_t threatRanks = 0;
        bool extraAttack = false;
        std::int32_t selfPenalty = 0; // dodge AC decrease for 3 s
        bool melee = false;           // which post-attack handler reacts to it
        bool ranged = false;
        bool stun = false;            // Critical Strike / Sniper Shot families
    };
    static SpecialAttack specialAttack(std::int32_t feat)
    {
        SpecialAttack out;
        switch (feat) {
        case 11: out = {-4, 0, 0, true, 4, true, false, false}; break;   // FLURRY
        case 91: out = {-2, 0, 0, true, 2, true, false, false}; break;   // IMPROVED_FLURRY
        case 53: out = {-1, 0, 0, true, 1, true, false, false}; break;   // WHIRLWIND_ATTACK (Master Flurry)
        case 30: out = {-4, 0, 0, true, 4, false, true, false}; break;   // RAPID_SHOT
        case 92: out = {-2, 0, 0, true, 2, false, true, false}; break;   // IMPROVED_RAPID_SHOT
        case 26: out = {-1, 0, 0, true, 1, false, true, false}; break;   // MULTI_SHOT
        case 28: case 29: out.attack = -3; out.damage = 5; break;        // POWER_ATTACK / BLAST
        case 17: case 18: out.attack = -3; out.damage = 8; break;        // IMPROVED_*
        case 83: case 82: out.attack = -3; out.damage = 10; break;       // MASTER_*
        case 8: out = {0, 0, 1, false, 5, true, false, true}; break;     // CRITICAL_STRIKE
        case 19: out = {0, 0, 2, false, 5, true, false, true}; break;    // IMPROVED_CRITICAL_STRIKE
        case 81: out = {0, 0, 3, false, 5, true, false, true}; break;    // MASTER_CRITICAL_STRIKE
        case 31: out = {0, 0, 1, false, 5, false, true, true}; break;    // SNIPER_SHOT
        case 20: out = {0, 0, 2, false, 5, false, true, true}; break;    // IMPROVED_SNIPER_SHOT
        case 77: out = {0, 0, 3, false, 5, false, true, true}; break;    // MASTER_SNIPER_SHOT
        case 102: out.attack = 2; out.damage = 2; break;                 // FORCE_JUMP_ADVANCED
        case 103: out.attack = 4; out.damage = 4; break;                 // FORCE_JUMP_MASTERY
        default: break;
        }
        return out;
    }

    // For the HUD's combat mode display and the legacy GetLastCombatFeatUsed reply.
    static CombatMode modeOfFeat(std::int32_t feat)
    {
        switch (feat) {
        case 28: case 29: case 17: case 18: case 82: case 83: return CombatMode::PowerAttack;
        case 11: case 91: case 30: case 92: case 26: return CombatMode::Flurry;
        case 8: case 19: case 81: case 31: case 20: case 77: return CombatMode::CriticalStrike;
        case 53: return CombatMode::Whirlwind;
        default: return CombatMode::Normal;
        }
    }

    std::int32_t abilityModifierOf(std::uint32_t object, std::size_t ability) const
    {
        if (isPc(object)) {
            const CharacterSheet& c = s.character_;
            const std::int32_t scores[6] = {c.str, c.dex, c.con, c.intel, c.wis, c.cha};
            return modifier(scores[ability]);
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr ? modifier(found->stats.abilities[ability]) : 0;
    }

    std::int32_t levelOf(std::uint32_t object) const
    {
        if (isPc(object)) {
            return s.character_.level
                + (s.character_.secondClassId >= 0 ? s.character_.secondClassLevel : 0);
        }
        return creatureTotalLevel(api(), object);
    }

    // Applies an engine-made effect record (the ApplyEffect calls inside 005ba8e0).
    void applyEngineEffect(std::uint32_t target, std::int32_t type,
                           std::vector<script::Value> args, float seconds, std::uint32_t creator)
    {
        EffectData data;
        data.type = type;
        data.args = std::move(args);
        const std::int64_t id = api().sim().engine.add(std::move(data));
        applyEffectRecord(api(), 1, id, target, seconds, creator, -1);
    }

    // retail 005a7ea0 (attack) and 005a9050 (damage inputs).
    Profile profile(std::uint32_t object, bool offhand, std::int32_t featId) const
    {
        Profile result;
        const kotor::Entity* found = entity(object);
        if (isPc(object)) {
            std::int32_t bonus = 0;
            s.rules_.weaponDamage(s.character_, result.dice, result.die, bonus);
            result.attack = s.rules_.attackBonus(s.character_) + s.forceAttackBonus()
                + s.forceJumpBonus_;
            result.flat = bonus + s.forceDamageBonus() + s.forceJumpBonus_;
            // 004f3fe0: the swinging weapon's item properties add their damage.
            for (const InventoryItem& item : s.character_.inventory) {
                if (item.equipped && item.slot == (offhand ? 5 : 4)) {
                    std::int32_t propertyAttack = 0;
                    std::int32_t propertyDamage = 0;
                    s.rules_.weaponPropertyBonus(item, propertyAttack, propertyDamage);
                    result.flat += propertyDamage;
                }
            }
            result.sneakDice = s.rules_.sneakAttackDice(s.character_);
            if (offhand) {
                // 005a7770 / 005a7b60 with the off-hand flag: the left weapon's own to-hit.
                result.attack += s.rules_.handAttackBonus(s.character_, true) - s.rules_.attackBonus(s.character_);
            }
            for (const InventoryItem& item : s.character_.inventory) {
                if (item.equipped && s.rules_.baseItemInt(item.baseItem, "numdice", 0) > 0) {
                    result.critBase = s.rules_.baseItemInt(item.baseItem, "critthreat", 1);
                    result.threat = 21 - result.critBase;
                    result.critMult = s.rules_.baseItemInt(item.baseItem, "crithitmult", 2);
                    result.ranged = s.rules_.baseItemInt(item.baseItem, "weapontype", 0) == 4;
                    break;
                }
            }
        } else if (found != nullptr) {
            const kotor::CreatureStats::Weapon& weapon =
                offhand ? found->stats.offHand : found->stats.mainHand;
            result.attack = offhand && weapon.present ? weapon.attackBonus : found->attackBonus;
            if (found->stats.combatDerived) {
                result.dice = weapon.dice;
                result.noDamage = weapon.noDamage;
                result.die = weapon.die;
                result.flat = weapon.flatDamage;
                // 005a5130: threat range from baseitems critthreat, doubled by Keen.
                result.critBase = weapon.critThreat;
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
        }
        const SpecialAttack special = specialAttack(featId);
        result.attack += special.attack;
        result.flat += special.damage;
        result.threat -= result.critBase * special.threatRanks;
        result.attack += attackEffectNet(object, offhand ? 2 : 1);
        result.flat += effectNetCapped(object, 42, 43, 36);
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
        // retail 004f2610: a creature of faction 5 (STANDARD_FACTION_NEUTRAL) never enters
        // the combat state; the call is turned into a state-0 set.
        if (factionOf(object) == 5) {
            return;
        }
        ActorCombat& combat = api().actor(object).combat;
        if (!combat.inCombat && object == s.scriptWorld_.pc) {
            s.showTutorial(21); // Hostile_Creature_Encountered, retail 005fa5a0
        }
        if (!combat.inCombat && !inParty(object)) {
            // 004f2610: entering combat gives a battle cry with a 30 percent chance, only for
            // a creature that is not a party member (+0xa88 == 0).
            const std::size_t index = api().entityFor(object);
            if (index != kNone) {
                s.effects_->speak(index, 0, EffectDirector::VoiceKind::BattleCry);
            }
        }
        if (!combat.inCombat && s.effects_) {
            s.effects_->saberUp(object);
        }
        combat.inCombat = true;
        combat.combatTimer = kCombatStateSeconds;
    }

    // retail 004f2610 with state 0: every combat field resets.
    void leaveCombat(std::uint32_t object) const
    {
        ActorCombat& combat = api().actor(object).combat;
        combat.inCombat = false;
        if (s.effects_) {
            s.effects_->saberDown(object);
        }
        combat.combatTimer = 0.0f;
        combat.roundActive = false;
        combat.pendingAction = -1;
        combat.lastAttackAction = -1;
        combat.lastHostileTarget = kInvalid;
        combat.lastHostileActor = kInvalid; // 004f2610 state 0 calls 004cc0c0(invalid), the field GetLastHostileActor reads
        combat.attemptedTarget = kInvalid;
        combat.attackTarget = kInvalid;
        combat.attemptedSpell = kInvalid;
    }

    // retail 005baca0: one attack roll; 005bb590 round loop; 005a9050 damage.
    void performAttack(std::uint32_t attacker, std::uint32_t target, bool offhand,
                       std::int32_t featId, bool firstAttack = true)
    {
        ScriptApi a = api();
        if (!a.isAlive(target) || !a.isAlive(attacker)) {
            return;
        }
        const Profile weapon = profile(attacker, offhand, featId);
        if (isPc(attacker)) {
            s.forceJumpBonus_ = 0;
        }
        const std::int32_t ac = armorClass(target, attacker);
        const std::int32_t natural = roll(20);
        const std::int32_t total = natural + weapon.attack;
        ActorState& targetState = a.actor(target);
        targetState.last["lastAttacker"] = attacker;
        targetState.combat.lastHostileActor = attacker;
        targetState.combat.attackedBy = attacker;
        enterCombat(target);
        enterCombat(attacker);
        a.raiseHook(target, "Attacked", {{"lastAttacker", attacker}});

        // Coup de grace, retail 005bb890: a melee attack on a debilitated non-party
        // creature below level 5 is an automatic hit (005baca0 result 3) and kills it
        // (005b8b00 applies EffectDeath).
        const bool coupDeGrace = !weapon.ranged && a.isCreature(target) && !inParty(target)
            && isDebilitated(a, target) && levelOf(target) < 5;
        const bool sneak = sneakAttackApplies(attacker, target, weapon);
        std::int32_t result = 0;
        bool critical = false;
        if (coupDeGrace) {
            result = kResultAutomatic;
        } else if (hasEffectOfType(a, attacker, 74)) { // EffectAssuredHit
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
                // 005baca0: a confirmed critical on a target immune to critical hits
                // (IMMUNITY_TYPE_CRITICAL_HIT 31) stays an ordinary hit.
                if (natural >= weapon.threat && roll(20) + weapon.attack >= ac
                    && !holdsImmunity(a, target, 31)) {
                    critical = true;
                    result = kResultCritical;
                }
            }
        }
        a.actor(attacker).combat.lastAttackResult = result;
        if (firstAttack && featId >= 0) {
            resolveSpecialAttack(attacker, target, featId, weapon.ranged,
                                 result != kResultMiss && result != kResultDeflected
                                     && result != kResultReflected);
        }
        if (!offhand && !std::exchange(a.actor(attacker).combat.swingStarted, false)) {
            s.playAttackClip(s.bodyOfObject(attacker));
        }
        s.effects_->attackResolved(attacker, target,
                                   result == kResultMiss ? 1
                                       : (result == kResultDeflected || result == kResultReflected) ? 3
                                       : critical ? 2 : 0,
                                   offhand);

        const std::string attackerName = nameOf(attacker);
        const std::string targetName = nameOf(target);
        if (result == kResultMiss) {
            s.floatingText(target, 2, 0); // retail 006027c0 type 2
            s.logEvent(attackerName + " misses " + targetName + " (" + std::to_string(total)
                       + " vs " + std::to_string(ac) + ")");
            return;
        }
        std::int32_t damage = weapon.flat;
        for (std::int32_t i = 0; i < (weapon.noDamage ? 0 : std::max(1, weapon.dice)); ++i) {
            damage += roll(weapon.die);
        }
        // An entity's damage range bounds the roll: it spans exactly the derived
        // dice unless a script or fixture narrowed it.
        damage = std::clamp(damage, std::min(weapon.minDamage, weapon.maxDamage), weapon.maxDamage);
        if (critical) {
            damage *= std::max(1, weapon.critMult);
        }
        std::int32_t sneakDamage = 0;
        if (sneak) {
            for (std::int32_t i = 0; i < weapon.sneakDice; ++i) {
                sneakDamage += roll(6);
            }
        }
        damage = weapon.maxDamage <= 0 ? std::max(0, damage + sneakDamage)
                                        : std::max(1, damage + sneakDamage);

        if (result == kResultDeflected || result == kResultReflected) {
            s.logEvent(targetName + " deflects " + tagOf(attacker));
            if (result == kResultReflected) {
                s.logEvent("deflected bolt hits " + tagOf(attacker) + " for "
                           + std::to_string(damage));
                applyScriptDamage(a, attacker, damage, kDamageUniversal, target);
            }
            return;
        }
        if (s.audio_) {
            // 006183d0: the attack grunt (leader always, others one in five); 005bb440: a critical
            // hit sends enum 0x11 (slot 16).
            const std::size_t index = a.entityFor(attacker);
            if (index != kNone) {
                s.effects_->speak(index, 9, EffectDirector::VoiceKind::AttackGrunt);
                if (critical) {
                    s.effects_->speak(index, 16, EffectDirector::VoiceKind::Broadcast);
                }
            }
        }
        s.logEvent(attackerName + " hits " + targetName + " for " + std::to_string(damage)
                   + (sneakDamage > 0 ? " (sneak)" : "") + (critical ? " (critical)" : "")
                   + (coupDeGrace ? " (coup de grace)" : ""));
        applyScriptDamage(a, target, damage, kDamageUniversal, attacker);
        if (coupDeGrace && a.isAlive(target)) {
            kill(target, attacker);
        }
    }

    // retail 005ba8e0 (melee) / 005ba540 (ranged), run for the round's first attack.
    void resolveSpecialAttack(std::uint32_t attacker, std::uint32_t target, std::int32_t feat,
                              bool ranged, bool hit)
    {
        const SpecialAttack special = specialAttack(feat);
        if ((ranged && !special.ranged) || (!ranged && !special.melee) || special.selfPenalty == 0) {
            return;
        }
        // EffectACDecrease(value, AC_DODGE_BONUS) on the attacker for 3.0 s.
        applyEngineEffect(attacker, 47,
                          {script::Value::makeInt(special.selfPenalty), script::Value::makeInt(0)},
                          3.0f, attacker);
        if (!special.stun || !hit || !api().isCreature(target) || !api().isAlive(target)) {
            return;
        }
        if (holdsImmunity(api(), target, 12)) { // IMMUNITY_TYPE_STUN: GetEffectLinkImmunity
            return;
        }
        const std::int32_t dc = levelOf(attacker) + abilityModifierOf(attacker, ranged ? 3 : 0);
        if (savingThrow(target, 1, dc) == 0) {
            applyEngineEffect(target, 29, {}, 6.0f, attacker); // stunned, 6.0 s
            s.logEvent(nameOf(target) + " stunned by " + s.rules_.featName(feat));
        }
    }

    // retail 005b8820: sneak attack needs a sneak-attack feat (0x3c..0x45) and either a
    // debilitated (or downed) target that is not immune to sneak attacks or critical
    // hits, or a target that does not see the attacker (melee always, ranged within
    // 10 m), or an attacker outside the target's front cone (cos >= 0.707 is in front;
    // ranged only within 10 m). The facing and unseen paths skip the immunity check.
    bool sneakAttackApplies(std::uint32_t attacker, std::uint32_t target, const Profile& weapon) const
    {
        ScriptApi a = api();
        if (weapon.sneakDice <= 0 || !a.isCreature(target)) {
            return false;
        }
        const kotor::Vec3 from = a.position(attacker);
        const kotor::Vec3 to = a.position(target);
        const float dx = from.x - to.x;
        const float dy = from.y - to.y;
        const float dz = from.z - to.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const auto immune = [&]() {
            return holdsImmunity(a, target, 30) || holdsImmunity(a, target, 31);
        };
        if (isDebilitated(a, target)) {
            return !immune();
        }
        const bool seen = creatureHasPerceived(a, target, attacker, 1);
        if (!seen) {
            if (!weapon.ranged || distance <= 10.0f) {
                return !immune();
            }
        }
        if (weapon.ranged && distance > 10.0f) {
            return false;
        }
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length <= 0.0f) {
            return false;
        }
        const float radians = a.facing(target) * 3.14159265f / 180.0f;
        const float dot = (dx * std::cos(radians) + dy * std::sin(radians)) / length;
        return dot < 0.707f;
    }

    void startRound(std::uint32_t self, std::uint32_t target, std::int32_t featId)
    {
        ScriptApi a = api();
        ActorCombat& combat = a.actor(self).combat;
        // retail 004d5f70: one on-hand attack, plus effect attacks, plus one for the
        // two-attack specials (004d2a10), plus an off-hand attack (004d2a70).
        std::int32_t attacks = 1 + std::max(0, effectTotal(a, self, effect::kModifyAttacks));
        if (featId >= 0) {
            combat.lastCombatFeat = featId;
        }
        if (specialAttack(featId).extraAttack) {
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
        // Fallback when the body has no attack clip: evenly spaced (this engine's).
        for (std::int32_t i = 0; i < attacks; ++i) {
            combat.attackTimes.push_back(kRoundSeconds * static_cast<float>(i + 1)
                                         / static_cast<float>(attacks + 1));
        }
        // retail 005bb890 resolves the round's melee attack when the action starts and files the hit
        // as a sub-attack at the swing's combatanimations.2da hit1 (GetWeaponImpactTime 005b4f30);
        // 004faf20 signals the damage once that much of the swing has passed (IsSubAttackReady
        // 004d45e0). So the swing starts with the round and the hit lands at its impact frame.
        combat.swingStarted = false;
        // A ranged clip has no hit time (hits 0), so its shot lands as the round starts.
        // Several attacks (005bb890 loop): sub-attack i takes column i+1 of the clip's row (hit1,
        // hit2, hit3, then parry0...). Shipped rows fill only hit1, so the later ones read 0 and,
        // queued behind the first (004d45e0 checks the head record), land as soon as it has.
        // This engine plays one clip for the round where retail picks one per sub-attack (005edce0).
        {
            const std::size_t body = s.bodyOfObject(self);
            std::vector<float> times;
            float last = 0.0f;
            for (std::int32_t i = 0; i < attacks; ++i) {
                const float impact = s.nextAttackImpact(body, i);
                if (impact < 0.0f || impact >= kRoundSeconds) {
                    times.clear();
                    break;
                }
                last = std::max(last, impact);
                times.push_back(last);
            }
            if (!times.empty()) {
                combat.attackTimes = std::move(times);
                s.playAttackClip(body);
                combat.swingStarted = true;
            }
        }
        combat.pendingAction = kActionAttackObject;
        const std::int32_t number = ++a.sim().combatRoundCounter;
        s.traceRound(number, self);
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

    bool rangedAttacker(std::uint32_t object) const
    {
        if (isPc(object)) {
            return profile(object, false, -1).ranged;
        }
        const kotor::Entity* found = entity(object);
        return found != nullptr && found->ranged;
    }

    // retail 004f1310 DesiredAttackRange for a creature target: both creatures' appearance
    // CREPERSPACE (+0x340 +8, loaded by 004ed6e0) + 0.7. creatureApproachRange is that sum
    // + 0.3 (004ee440).
    float meleeDesiredRange(std::uint32_t self, std::uint32_t target) const
    {
        if (target == kInvalid || !api().isCreature(target)) {
            return kMeleeReach - 0.5f;
        }
        return s.creatureApproachRange(self, target, 0.0f) - 0.3f + 0.7f;
    }

    // retail 004fb0f0 MaxAttackRange: melee is DesiredAttackRange + 0.5. Ranged keeps the
    // engine's own reach (retail: base item range, or 30 m; 20 m for the cleave search).
    float reach(std::uint32_t object, std::uint32_t target = kInvalid) const
    {
        const kotor::Entity* found = entity(object);
        if (isPc(object)) {
            return profile(object, false, -1).ranged ? 15.0f : meleeDesiredRange(object, target) + 0.5f;
        }
        return found != nullptr && found->ranged ? found->attackRange : meleeDesiredRange(object, target) + 0.5f;
    }

    // retail 004d4620
    void endRound(std::uint32_t self)
    {
        ScriptApi a = api();
        ActorCombat& combat = a.actor(self).combat;
        s.traceRound(a.sim().combatRoundCounter, self, true); // 004d4620 EndCombatRound
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
        } else if (a.isAlive(self)) {
            continueLeaderAttack(self, combat.roundTarget);
        }
    }

    // retail 004d4620 EndCombatRound -> 005b6980 CreateNewAttackActions, for the creature
    // the player controls only: when the round's target is gone, the nearest hostile the
    // leader perceives within MaxCleaveRange (004ffad0: MaxAttackRange + 2.0; ranged
    // 20 + 2.0, melee desired reach + 0.5 + 2.0) of the fallen target, else of the
    // leader (004f2de0 GetNearestEnemy), becomes the new attack target and the leader
    // fights on with no input. With none in range, the combat mode is cancelled.
    void continueLeaderAttack(std::uint32_t self, std::uint32_t oldTarget)
    {
        ScriptApi a = api();
        ActorState& state = a.actor(self);
        if (oldTarget != kInvalid && a.isAlive(oldTarget) && reputation(self, oldTarget) <= 10) {
            return; // The attack action goes on against the same target.
        }
        const bool queueFree = state.queue.empty()
            || (state.queue.front().kind == ActorActionKind::Attack
                && !a.isAlive(state.queue.front().object));
        if (!queueFree || oldTarget == kInvalid) {
            return;
        }
        const bool ranged = profile(self, false, -1).ranged;
        // 004ffad0 MaxCleaveRange = MaxAttackRange(object, 0, 1) + 2.0. With object 0 as the
        // target, DesiredAttackRange (004f1310) takes its non-creature default of 1.5, so a
        // melee cleave reaches 1.5 + 0.5 + 2.0 = 4.0 m; ranged (param_3 set) is 20 + 2.0.
        const float cleave = (ranged ? 20.0f : 1.5f + 0.5f) + 2.0f;
        const auto nearestFrom = [&](kotor::Vec3 center) {
            std::uint32_t best = kInvalid;
            float bestDistance = cleave * cleave;
            for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
                const kotor::Entity& candidate = s.scene_.entities[i];
                const std::uint32_t other = a.objectFor(i);
                if (candidate.kind != kotor::EntityKind::Creature || candidate.dead
                    || other == self || other == oldTarget || !a.isAlive(other)
                    || reputation(self, other) > 10
                    || !creatureHasPerceived(a, self, other, 1)) {
                    continue;
                }
                const kotor::Vec3 at = a.position(other);
                const float dx = at.x - center.x;
                const float dy = at.y - center.y;
                const float dz = at.z - center.z;
                const float d = dx * dx + dy * dy + dz * dz;
                if (d <= bestDistance) {
                    bestDistance = d;
                    best = other;
                }
            }
            return best;
        };
        std::uint32_t next = kInvalid;
        if (oldTarget != kInvalid && a.world().valid(oldTarget)) {
            next = nearestFrom(a.position(oldTarget));
        }
        if (next == kInvalid) {
            next = nearestFrom(a.position(self));
        }
        if (next == kInvalid) {
            s.combatMode_ = CombatMode::Normal; // SetCombatMode(0) / ClearAllSpecialAttacks
            return;
        }
        std::erase_if(state.queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
        ActorAction attack;
        attack.kind = ActorActionKind::Attack;
        attack.object = next;
        attack.userIssued = true;
        a.enqueue(self, std::move(attack));
        a.actor(self).combat.attackTarget = next;
        const std::size_t index = a.entityFor(next);
        if (index != kNone) {
            s.combatTarget_ = static_cast<std::uint32_t>(index); // SetLastTarget
        }
        s.logEvent("auto target " + nameOf(next));
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
                if (inReach(object, target)) {
                    performAttack(object, target, offhand, feat, index == 0);
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
        return std::hypot(from.x - to.x, from.y - to.y) <= reach(self, target) + 0.5f;
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
                s.effects_->speak(index, 15, EffectDirector::VoiceKind::Broadcast); // 004e0ac0
            }
            s.onCreatureKilled(index, attacker);
            return;
        }
        s.syncEntityToScriptWorld(index);
        if (s.audio_ && dealt > 0) {
            s.effects_->speak(index, 12, EffectDirector::VoiceKind::Pain); // 00617eb0
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
        if (amount > 0) {
            s.floatingText(object, 1, amount); // retail 006027c0 type 1
        }
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

    void tempDeathUpdate(float dt)
    {
        ScriptApi a = api();
        WorldSim& sim = a.sim();
        if (s.gameOverShown_ || sim.ending.pending) {
            return;
        }
        std::vector<std::uint32_t> members;
        members.push_back(s.scriptWorld_.pc);
        for (const CompanionState& companion : s.companions_) {
            const std::size_t index = s.findEntity(companion.tag);
            if (index != kNone) {
                members.push_back(a.objectFor(index));
            }
        }
        // The party table (004b6da0 walks CSWParty): a member whose avatar is in the scene counts
        // even when no companion record names it (a save loaded into a module).
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const std::uint32_t object = a.objectFor(i);
            if (s.scene_.entities[i].partyAvatar && object != kInvalid
                && std::find(members.begin(), members.end(), object) == members.end()) {
                members.push_back(object);
            }
        }
        std::vector<std::uint32_t> down;
        for (const std::uint32_t member : members) {
            if (!a.isAlive(member)) {
                down.push_back(member);
            }
        }
        if (down.empty() || down.size() == members.size()) {
            if (sim.tempDeathScan > 0.0f || sim.tempDeathClock > 0.0f) {
                logInfo(concat("temp death: idle, members ", members.size(), " down ", down.size(), " at ", s.elapsed_));
            }
            sim.tempDeathScan = 0.0f;
            sim.tempDeathClock = 0.0f;
            return;
        }
        sim.tempDeathScan += dt;
        if (sim.tempDeathScan > 1.0f) {
            sim.tempDeathScan = 0.0f;
            for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
                const kotor::Entity& candidate = s.scene_.entities[i];
                const std::uint32_t other = a.objectFor(i);
                if (candidate.kind != kotor::EntityKind::Creature || candidate.dead
                    || inParty(other) || !a.isAlive(other)
                    || reputation(other, members.back()) > 10) {
                    continue;
                }
                const auto& perceived = a.actor(other).perceived;
                const bool seesParty = std::any_of(members.begin(), members.end(), [&](std::uint32_t m) {
                    const auto it = perceived.find(m);
                    // 004f7650 GetHasActivePartyMember920: only an entry with bit 0 (seen) counts.
                    return it != perceived.end() && (it->second & 1) != 0;
                });
                if (seesParty) {
                    logInfo(concat("temp death: clock reset by ", candidate.tag, " at ", s.elapsed_));
                    sim.tempDeathClock = 0.0f;
                    return;
                }
            }
        }
        sim.tempDeathClock += dt;
        if (sim.tempDeathClock <= 5.0f) {
            return;
        }
        sim.tempDeathClock = 0.0f;
        const kotor::Vec3 leader = a.position(s.controlledObject());
        for (const std::uint32_t member : down) {
            resurrect(member);
            a.clearQueue(member);
            if (!isPc(member)) {
                const kotor::Vec3 at = a.position(member);
                const float dx = at.x - leader.x;
                const float dy = at.y - leader.y;
                const float dz = at.z - leader.z;
                if (dx * dx + dy * dy + dz * dz > 1600.0f) {
                    if (kotor::Entity* found = entity(member); found != nullptr) {
                        found->position = leader;
                        s.syncEntityToScriptWorld(a.entityFor(member));
                    }
                }
            }
            s.logEvent(nameOf(member) + " recovers");
            logInfo(concat("temp death: ", nameOf(member), " recovers at ", s.elapsed_));
        }
        ++s.animationRevision_;
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

    // retail 004cdf50 passes the caster's good/evil (stats +0x17e) and creature +0xa88
    // (party member) to 0059b8a0 GetAdjustedForcePoints.
    std::int32_t forceCost(std::uint32_t object, std::int32_t spell) const
    {
        std::int32_t goodEvil = 50;
        if (isPc(object)) {
            goodEvil = s.character_.alignment;
        } else if (const kotor::Entity* found = entity(object); found != nullptr) {
            goodEvil = found->stats.goodEvil;
        }
        return s.rules_.adjustedForceCost(spell, goodEvil, inParty(object));
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
    // retail 005b92b0 SavingThrowRoll: d20 + base + effect bonus (capped at 20) >= DC is 1.
    // A failed save against a save type the creature is immune to returns 2: type 10
    // (mind) -> immunity 1, 12 (poison) -> 2, 5 (disease) -> 3, 8 (fear) -> 4, 14 (trap) -> 5.
    // Type 4 (death) needs the casting spell's +0x140 flag, which is not modelled.
    std::int32_t savingThrow(std::uint32_t object, std::int32_t which, std::int32_t dc,
                             std::int32_t saveType = 0)
    {
        ScriptApi a = api();
        // 004f3fe0 case 3: the increases and the decreases are each capped at 20 before they are subtracted.
        std::int32_t gain = 0;
        std::int32_t loss = 0;
        for (const AppliedEffect& held : a.actor(object).effects) {
            if (held.type != 50 && held.type != 51) {
                continue;
            }
            const EffectData* data = a.sim().engine.get<EffectData>(held.effect);
            if (data != nullptr && data->args.size() > 1
                && (data->args[0].asInt() == which || data->args[0].asInt() == 0)) {
                const std::int32_t value = static_cast<std::int32_t>(data->args[1].asInt());
                (held.type == 50 ? gain : loss) += value;
            }
        }
        const std::int32_t bonus = std::min(gain, 20) - std::min(loss, 20);
        if (roll(20) + saveBase(object, which) + bonus >= dc) {
            return 1;
        }
        const std::int32_t immunity = saveType == 10 ? 1 : saveType == 12 ? 2 : saveType == 5 ? 3
            : saveType == 8 ? 4 : saveType == 14 ? 5 : -1;
        return immunity >= 0 && holdsImmunity(a, object, immunity) ? 2 : 0;
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

    // Skill rank as retail GetSkillRank totals it: ranks + key ability modifier (Stealth DEX,
    // Awareness WIS, skills.2da keyability) + the skill feats. Rules::skillBonus is only the
    // feat part; it used to be the whole PC value here, so the PC's ranks never counted.
    std::int32_t skillOf(std::uint32_t object, int skill) const
    {
        const char* name = skill == 2 ? "stealth" : "awareness";
        if (isPc(object)) {
            std::int32_t rank = 0;
            for (const auto& [label, value] : s.character_.skills) {
                std::string lowered = label;
                for (char& c : lowered) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lowered == name) {
                    rank = value;
                }
            }
            return rank + modifier(skill == 2 ? s.character_.dex : s.character_.wis)
                + s.rules_.skillBonus(s.character_, name);
        }
        const kotor::Entity* found = entity(object);
        if (found == nullptr || static_cast<std::size_t>(skill) >= found->stats.skills.size()) {
            return 0;
        }
        return found->stats.skills[static_cast<std::size_t>(skill)]
            + modifier(found->stats.abilities[skill == 2 ? 1 : 4]);
    }

    // retail 004f1fd0 CSWSCreature::DoSpotDetection, for a target in stealth mode with a
    // Stealth rank: Awareness (half unless the observer is in detect mode, which this engine
    // has no way to enter) + the observer's spot roll - (the target's hide roll + Stealth),
    // -5 when the target is player-controlled and behind the observer (cos < -0.707),
    // -10 when the observer is in combat, -5 when the target stands still, +5 when the
    // observer stands still, minus trunc(distance / 3) once that exceeds 2. Seen at >= 1.
    // Rolls renew every 20 s (00502ac0): hide 11..20; spot d10, d20 for a still observer.
    bool spotsStealth(std::uint32_t observer, std::uint32_t target) const
    {
        ScriptApi a = api();
        const std::int32_t stealth = skillOf(target, 2);
        if (stealth == 0) {
            return true;
        }
        const ActorState& watcher = a.actor(observer);
        const ActorState& sneaker = a.actor(target);
        const std::int32_t awareness = skillOf(observer, 3) / 2;
        const std::int32_t spot = pairRoll(target, observer, 1, watcher.stationary ? 20 : 10);
        const std::int32_t hide = 10 + pairRoll(observer, target, 2, 10);
        std::int32_t total = awareness + spot - hide - stealth;
        const kotor::Vec3 from = a.position(observer);
        const kotor::Vec3 to = a.position(target);
        if (playerControlled(target)) {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::hypot(dx, dy);
            const float radians = a.facing(observer) * 3.14159265f / 180.0f;
            const float dot = length > 0.0f
                ? (dx * std::cos(radians) + dy * std::sin(radians)) / length
                : 1.0f;
            if (dot < -0.707f) {
                total -= 5;
            }
        }
        if (watcher.combat.inCombat) {
            total -= 10;
        }
        if (sneaker.stationary) {
            total -= 5;
        }
        if (watcher.stationary) {
            total += 5;
        }
        const float dx = from.x - to.x;
        const float dy = from.y - to.y;
        const float dz = from.z - to.z;
        const float thirds = std::sqrt(dx * dx + dy * dy + dz * dz) / 3.0f;
        if (thirds > 2.0f) {
            total -= static_cast<std::int32_t>(thirds);
        }
        return total >= 1;
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
    // retail 004eb6e0 SpawnInHeartbeatPerception: each creature runs its own visible-list pass
    // (UpdateVisibleList 0051b050) on its first update, then the player's creature (stats +0x9d4)
    // every 300 + rand() % 400 ms and every other creature every 4000 ms.
    void perceive(float dt)
    {
        ScriptApi a = api();
        WorldSim& sim = a.sim();
        sim.perceptionTimer += dt;
        const bool stationaryPass = sim.perceptionTimer >= kPerceptionInterval;
        if (stationaryPass) {
            sim.perceptionTimer = 0.0f;
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
        for (const std::uint32_t creature : creatures) {
            if (!stationaryPass) {
                break;
            }
            ActorState& state = a.actor(creature);
            const kotor::Vec3 at = a.position(creature);
            state.stationary = !state.perceivePositionKnown
                || std::hypot(at.x - state.perceivePosition.x, at.y - state.perceivePosition.y) < 0.05f;
            state.perceivePosition = at;
            state.perceivePositionKnown = true;
        }
        for (const std::uint32_t observer : creatures) {
            if (a.inConversation(observer)) {
                continue; // 004eb6e0 / 0051b050 skip a creature that is in a conversation
            }
            ActorState& schedule = a.actor(observer);
            schedule.perceptionDue -= dt;
            if (schedule.perceptionStarted && schedule.perceptionDue > 0.0f) {
                continue;
            }
            schedule.perceptionStarted = true;
            schedule.perceptionDue = isPc(observer)
                ? static_cast<float>(300u + s.random() % 400u) / 1000.0f
                : 4.0f;
            const bool partyOnly = std::exchange(schedule.firstPassPartyOnly, false);
            const auto [sight, hearing] = sees(observer);
            const kotor::Vec3 from = a.position(observer);
            std::map<std::uint32_t, std::uint8_t> old = std::move(a.actor(observer).perceived);
            std::map<std::uint32_t, std::uint8_t> now;
            const bool isParty = inParty(observer);
            for (const std::uint32_t target : creatures) {
                if (target == observer || (partyOnly && !inParty(target))) {
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
                        && a.hasLineOfSight(observer, target, true)) {
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
        changeForce(self, -forceCost(self, action.spell));
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
    if (amount > 0) {
        api.session.floatingText(target, 0, amount); // retail 006027c0 type 0
    }
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

void leaveCombatOnDeath(ScriptApi api, std::uint32_t object)
{
    CombatOps{api.session}.leaveCombat(object);
}

void resurrectCreature(ScriptApi api, std::uint32_t object)
{
    CombatOps{api.session}.resurrect(object);
}

// retail 004b6da0 CServerExoAppInternal::TempDeathUpdate (real-time clock, skipped while the
// game is over). While some party member is down: every 1000 ms scan the area for a
// non-party creature hostile to the party (reputation <= 10) that perceives a party
// member; one found resets the clock. Once the clock passes 5000 ms every downed member
// gets EffectResurrection (004e13c0: HP 1 if below, ClearAllActions) at a safe spot within
// 5 m, and a member more than 40 m (1600 sq) from the leader goes to its follow point.
// Everyone down is the death sequence instead (updatePartyWipe).
void tickTempDeathRecovery(ScriptApi api, float dt)
{
    CombatOps{api.session}.tempDeathUpdate(dt);
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
    const float reach = ops.reach(self, target);
    const bool ranged = ops.rangedAttacker(self);
    // 005bbbf0 AIActionPhysicalAttacks casts 0050c330 (the perception sight cast, 00506650 included) from the
    // attacker to a creature target before every attack, melee or ranged: a closed door on the line stops the
    // attack like a wall. The cast runs from attacker z + 1.5 (DAT 0073f414) + 0.01 (0073dfec) to target
    // z + 1.5 - 0.01.
    const bool castBlocked = api.isCreature(target)
        && !api.hasLineOfSight(self, target, true, 1.5f + 0.01f, 1.5f - 0.01f);
    if (distance > reach || castBlocked) {
        // 005bbbf0 AIActionPhysicalAttacks: a party member that has to move more than 0.25 m
        // to its target sets the combat message 42477 (CSWPartyMemberData::SetCombatMessage).
        // The HUD shows the controlled creature's message; started marks it raised.
        if (!action.started && distance - reach > 0.25f && self == api.session.controlledObject()) {
            api.session.raiseCombatMessage(42477);
            action.started = true;
        }
        // A melee attacker closes to DesiredAttackRange (004f1310), half a metre inside reach.
        const ActorStep step = api.step(self, to, api.moveSpeed(self, true), dt,
                                        ranged ? std::max(1.0f, reach * 0.9f)
                                               : std::min(kMeleeStop, ops.meleeDesiredRange(self, target)));
        action.stuck = step == ActorStep::Blocked || step == ActorStep::Aborted ? action.stuck + dt : 0.0f;
        if (action.stuck >= kAttackStuckGiveUp) {
            api.log("attack gave up: no way to target");
            combat.attackTarget = kInvalid;
            state.queue.pop_front();
            return true;
        }
        return false;
    }
    combat.attackTarget = target;
    action.started = false;
    if (!combat.roundActive) {
        ops.startRound(self, target, action.spell);
        // The special attack covers the round it was queued for; the action then goes
        // on with ordinary rounds.
        action.spell = -1;
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
        if (!action.fake) {
            api.session.effects().spellCast(api.entityFor(self), action.spell);
        }
        // retail 00514af0 AIActionCastSpell: the conjure animation runs for spells.2da
        // conjtime, then SpellCastAndImpact (004cdf50) fires and the cast animation runs;
        // the action ends at conjtime + casttime. The round is paused for
        // conjtime + casttime + catchtime (SetPauseTimer / DecrementRoundLength).
        const Rules::SpellTimes times = api.session.rules().spellTimes(action.spell);
        action.remaining = static_cast<float>(times.conjure + times.cast) / 1000.0f;
        action.timeout = static_cast<float>(times.conjure) / 1000.0f; // impact time
        action.impacted = false;
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
    action.timeout -= dt;
    if (!action.impacted && action.timeout <= 0.0f) {
        action.impacted = true;
        const ActorAction copy = action;
        ops.finishCast(self, copy);
        // The impact script may have changed the queue (ClearAllActions on the caster).
        if (api.actor(self).queue.empty()
            || api.actor(self).queue.front().kind != ActorActionKind::CastSpell
            || !api.actor(self).queue.front().impacted) {
            return true;
        }
    }
    ActorAction& current = api.actor(self).queue.front();
    if (current.remaining > 0.0f) {
        return false;
    }
    api.actor(self).queue.pop_front();
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

// The creature's Force resistance: the stat byte 005a5a60 GetForceResistance returns, the
// increase minus the penalty, floored at 0. The increase handler (004db180) does not add: each
// effect raises the stat only when its value (0..128) is above the current one, so the stat is
// the largest increase effect; the penalty is kept the same way (005afae0).
std::int32_t forceResistance(ScriptApi api, std::uint32_t object)
{
    const auto largest = [&](std::int32_t type) {
        std::int32_t best = 0;
        for (const AppliedEffect& held : api.actor(object).effects) {
            if (held.type != type) {
                continue;
            }
            const EffectData* data = api.sim().engine.get<EffectData>(held.effect);
            if (data != nullptr && !data->args.empty()) {
                best = std::max(best, std::min<std::int32_t>(static_cast<std::int32_t>(data->args[0].asInt()), 128));
            }
        }
        return best;
    };
    return std::max(0, largest(52) - largest(53));
}

// ACT_169 ResistForce 00541cc0. Retail rolls a d20 plus the source's level against the
// target's Force resistance (effects 52 and 53, see forceResistance). 0 not resisted, 1 resisted;
// the three-way result the header mentions (global immunity) was not traced.
Cells resistForce(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::int32_t resistance = forceResistance(api, call.objectArg(1));
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

// ACT_362 / ACT_363 (handler 00544d70): the talent struct's type and id fields. A default talent has type -1
// (GetIsTalentValid 00539f90 tests it), so a never-assigned talent is not a Force power and the droid and
// lightsaber checks of k_inc_generic skip their id comparisons.
Cells getTypeFromTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    return ActionCall::retInt(talent != nullptr ? talent->type : -1);
}

Cells getIdFromTalent(ActionCall& call)
{
    const TalentData* talent = call.api().sim().engine.get<TalentData>(call.engineArg(0));
    return ActionCall::retInt(talent != nullptr ? talent->id : -1);
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

// ACT_48 ActionCastSpellAtObject: every caster, the PC included, queues the cast and
// runs the shipped impact script from spells.2da (k_sp1_generic), as retail does.
Cells actionCastSpellAtObject(ActionCall& call)
{
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
    if (call.self() == call.api().pc() && type == 1) {
        call.session.useFeat(id, target);
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

// ACT_111 0053d320 reads the caster's save DC through 004ef6e0
// CSWSCreature::CalculateSpellSaveDC: class levels + WIS mod + CHA mod + 5 + Force
// Focus (feats 0x58/0x59/0x5a: 1/2/4). 14 when the caller is not a creature.
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
    std::int32_t focus = 0;
    const auto hasFeat = [&](std::int32_t feat) {
        return std::find(stats.feats.begin(), stats.feats.end(), feat) != stats.feats.end();
    };
    if (hasFeat(0x5a)) {
        focus = 4;
    } else if (hasFeat(0x59)) {
        focus = 2;
    } else if (hasFeat(0x58)) {
        focus = 1;
    }
    return ActionCall::retInt(levels + modifier(stats.abilities[4]) + modifier(stats.abilities[5]) + 5 + focus);
}

// ---- saves -------------------------------------------------------------------

Cells savingThrow(ActionCall& call)
{
    const std::int32_t which = call.id == 108 ? 1 : call.id == 109 ? 2 : 3;
    return ActionCall::retInt(CombatOps{call.session}.savingThrow(
        call.objectArg(0), which, static_cast<std::int32_t>(call.intArg(1)),
        call.hasArg(2) ? static_cast<std::int32_t>(call.intArg(2)) : 0));
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
        call.session.statusFeedback(3, sim.stealthXpCurrent); // retail status row 3
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
    // Everyone else near the arena goes too: a module Sith Soldier standing there (faction 1,
    // perceiving on its own 4 s schedule, 004eb6e0) otherwise reaches blue before red lands a hit,
    // while red is already in a round against blue.
    for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
        kotor::Entity& e = s.scene_.entities[i];
        if (e.kind != kotor::EntityKind::Creature || e.dead || e.partyAvatar) {
            continue;
        }
        if (i == hostileIndex || std::hypot(e.position.x - arena.x, e.position.y - arena.y) < 40.0f) {
            e.position.x += 200.0f; // Out of the fight.
            s.syncEntityToScriptWorld(i);
        }
    }
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
    {
        const auto dump = [&](const char* tag, std::uint32_t o) {
            const ActorState& st = a.actor(o);
            const auto seen = [&](std::uint32_t t) { auto it = st.perceived.find(t); return it == st.perceived.end() ? -1 : int(it->second); };
            logInfo(concat("combat family: diag ", tag, " ", o, " fac ", s.factionOfObject(o), " inCombat ", st.combat.inCombat,
                           " round ", st.combat.roundActive, " target ", st.combat.attackTarget, " attempted ", st.combat.attemptedTarget,
                           " queue ", st.queue.size(), " seesRed ", seen(red), " seesBlue ", seen(blue),
                           " started ", st.perceptionStarted, " due ", st.perceptionDue));
        };
        dump("red", red);
        dump("blue", blue);
        const std::uint32_t other = a.actor(blue).combat.lastHostileActor;
        if (other != kInvalid && other != red) dump("other", other);
    }
    check(fought, "npc_factions_fought");
    logInfo(concat("combat family: npc fight red ", red, " blue ", blue, " red.lastHostile ",
                   a.actor(red).combat.lastHostileActor, " blue.lastHostile ", a.actor(blue).combat.lastHostileActor,
                   " (", nameOf(a.actor(blue).combat.lastHostileActor), ") hp ", hpOf(red), "/", redBefore, " ", hpOf(blue), "/",
                   blueBefore, " pc ", pc, " hostile ", hostile));
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

    // ---- W038: the dead creature is destroyed after DestroyObjectDelay (004e0e2e / 004ff1a4) -----
    {
        // Old rule: bodies stayed valid objects for ever and dropped nothing.
        const std::size_t corpseIndex = a.entityFor(corpse);
        const float delay = corpseIndex != kNone ? s.destroyObjectDelay(s.scene_.entities[corpseIndex]) : -1.0f;
        const auto valid = [&] {
            const script::GameObject* object = a.world().get(corpse);
            return object != nullptr && !object->attrs.contains("destroyed");
        };
        const auto bags = [&] {
            std::size_t n = 0;
            for (const kotor::Entity& e : s.scene_.entities) {
                n += e.kind == kotor::EntityKind::Placeable && e.tag == "BodyBag" ? 1 : 0;
            }
            return n;
        };
        call(pc, 339, {obj(corpse)}); // GetFirstItemInInventory loads the template items.
        std::size_t droppable = 0;
        if (const script::GameObject* holder = a.world().get(corpse); holder != nullptr) {
            for (const std::uint32_t id : holder->inventory) {
                const script::GameObject* item = a.world().get(id);
                if (item == nullptr || !item->valid) {
                    continue;
                }
                const auto it = item->attrs.find("item_droppable");
                const std::int64_t* flag = it != item->attrs.end() ? std::get_if<std::int64_t>(&it->second) : nullptr;
                droppable += flag != nullptr && *flag != 0 ? 1 : 0;
            }
        }
        const std::size_t bagsBefore = bags();
        const bool validAtDeath = valid();
        const bool destroyed = tickUntil(delay + 1.0f, [&] { return !valid(); });
        const bool bodyShown = corpseIndex != kNone && s.scene_.entities[corpseIndex].corpse();
        logInfo(concat("combat family: corpse delay ", delay, " valid at death ", validAtDeath, " destroyed ",
                       destroyed, " body shown ", bodyShown, " droppable ", droppable, " bags ", bagsBefore,
                       " -> ", bags()));
        check(delay == 3.0f && validAtDeath && destroyed && bodyShown,
              "corpse_destroyed_after_delay_004e0e2e");
        check(bags() == bagsBefore + (droppable > 0 ? 1u : 0u), "body_bag_for_droppable_items_004ce220");
        // The bag lies where the body fell; using it there opens its inventory (useObject -> openLoot),
        // as a click on the remains does.
        if (droppable > 0) {
            std::size_t bagIndex = kNone;
            for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
                if (s.scene_.entities[i].kind == kotor::EntityKind::Placeable && s.scene_.entities[i].tag == "BodyBag") {
                    bagIndex = i;
                }
            }
            const bool opened = bagIndex != kNone && s.useObjectImpl(bagIndex, false) == UseOutcome::Looted;
            const std::size_t shown = s.lootItems_.size();
            s.closeLoot();
            check(opened && shown == droppable, "body_bag_opens_with_the_droppable_items");
        }
    }

    // ---- FadeDelayOnDeath 0 (appearance rows 65, 353, 437): the body leaves with the destroy -------
    {
        // Old rule: every body stayed on screen after its destroy event.
        std::uint32_t spider = script::kObjectInvalid;
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const kotor::Entity& e = s.scene_.entities[i];
            const std::uint32_t object = a.objectFor(i);
            if (e.kind == kotor::EntityKind::Creature && !e.dead && !e.partyAvatar && object != survivor
                && object != s.scriptWorld_.pc && !inParty(object)) {
                spider = object;
                break;
            }
        }
        check(spider != script::kObjectInvalid, "fade_fixture");
        if (kotor::Entity* e = entity(spider); e != nullptr) {
            kotor::Entity blank = *e;
            check(s.fadeDelayOnDeath(blank) < 0.0f || blank.appearance == 65 || blank.appearance == 353
                      || blank.appearance == 437,
                  "fade_blank_keeps_body_004ff263");
            e->appearance = 65; // Droid_Spyder: DestroyObjectDelay 1, FadeDelayOnDeath 0.
            e->plot = false;
            e->min1Hp = false;
            a.actor(spider).destroyable = true;
            s.addCreatureGold(spider, 25); // 004edd60: gold alone makes a bag (00588690 g_i_credits001).
            std::size_t bagsBefore = 0;
            for (const kotor::Entity& other : s.scene_.entities) {
                bagsBefore += other.kind == kotor::EntityKind::Placeable && other.tag == "BodyBag" ? 1 : 0;
            }
            killCreature(a, spider, 0);
            const std::size_t index = a.entityFor(spider);
            const bool shownAtDeath = index != kNone && s.scene_.entities[index].corpse();
            tickUntil(2.0f, [] { return false; });
            const bool shownAfter = index != kNone && s.scene_.entities[index].corpse();
            logInfo(concat("combat family: fade 0 body shown at death ", shownAtDeath, " after ", shownAfter));
            check(shownAtDeath && !shownAfter, "fade_delay_zero_hides_body_004ce8a0");
            std::size_t bagIndex = kNone;
            std::size_t bagsAfter = 0;
            for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
                if (s.scene_.entities[i].kind == kotor::EntityKind::Placeable && s.scene_.entities[i].tag == "BodyBag") {
                    ++bagsAfter;
                    bagIndex = i;
                }
            }
            const std::int32_t goldBefore = s.gold_;
            bool tookCredits = false;
            if (bagsAfter == bagsBefore + 1 && s.useObjectImpl(bagIndex, false) == UseOutcome::Looted) {
                for (std::size_t k = 0; k < s.lootItems_.size(); ++k) {
                    if (s.lootItems_[k].resref == "g_i_credits001") {
                        tookCredits = s.takeLootItem(k);
                        break;
                    }
                }
                s.closeLoot();
            }
            logInfo(concat("combat family: gold bag ", bagsBefore, " -> ", bagsAfter, " gold ", goldBefore, " -> ", s.gold_));
            check(tookCredits && s.gold_ == goldBefore + 25 && s.creatureGold(spider) == 0,
                  "creature_gold_goes_to_the_bag_00588690");
        }
    }


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

    // ---- special attacks, coup de grace (retail tables) ---------------------------
    {
        // 005a4ec0 / 005a4fb0 / 005a5130 / 004d2a10. The old table had Whirlwind at -4
        // hitting every hostile in reach, Critical Strike at -2 with +1 threat per rank.
        check(specialAttack(53).attack == -1 && specialAttack(53).extraAttack
                  && specialAttack(53).selfPenalty == 1,
              "whirlwind_is_master_flurry_005a4ec0");
        check(specialAttack(91).attack == -2 && specialAttack(11).attack == -4, "flurry_ranks_005a4ec0");
        check(specialAttack(8).attack == 0 && specialAttack(8).threatRanks == 1
                  && specialAttack(19).threatRanks == 2 && specialAttack(81).threatRanks == 3,
              "critical_strike_005a5130");
        check(specialAttack(28).attack == -3 && specialAttack(28).damage == 5
                  && specialAttack(17).damage == 8 && specialAttack(83).damage == 10,
              "power_attack_005a4fb0");
        check(specialAttack(30).extraAttack && specialAttack(30).attack == -4
                  && specialAttack(26).attack == -1 && specialAttack(31).attack == 0,
              "ranged_specials_005a4ec0");
        check(!specialAttack(28).extraAttack && specialAttack(28).selfPenalty == 0,
              "power_attack_no_penalty_005ba8e0");
        // 005b4880 / 004e1c20: SetState effects debilitate; Entangle is not one, WhirlWind is.
        check(!isDisablingEffect(11) && isDisablingEffect(effect::kWhirlwind),
              "debilitated_set_states_004e1c20");
        // Flurry puts a 4-point dodge AC decrease on the attacker after the round's
        // first attack (005ba8e0), once per round.
        kotor::Entity& e = s.scene_.entities[hostileIndex];
        e.plot = false;
        e.hitPoints = 100000;
        e.maxHitPoints = 100000;
        const std::int32_t before = effectTotal(a, pc, 47);
        performAttack(pc, hostile, false, 11, true);
        performAttack(pc, hostile, false, 11, false);
        check(effectTotal(a, pc, 47) - before == 4, "flurry_self_penalty_once_005ba8e0");
        // Coup de grace (005bb890 / 005b8b00): a debilitated non-party creature below
        // level 5 dies to any melee attack.
        applyEngineEffect(hostile, 29, {}, 6.0f, pc);
        const bool lowLevel = levelOf(hostile) < 5;
        const bool meleePc = !profile(pc, false, -1).ranged;
        performAttack(pc, hostile, false, -1, true);
        if (lowLevel && meleePc) {
            check(!a.isAlive(hostile) && logHas("coup de grace"), "coup_de_grace_005bb890");
            resurrect(hostile);
        } else {
            check(a.isAlive(hostile), "no_coup_de_grace_level5_or_ranged");
        }
        std::erase_if(a.actor(hostile).effects, [](const AppliedEffect& held) { return held.type == 29; });
        std::erase_if(a.actor(pc).effects, [](const AppliedEffect& held) { return held.type == 47; });
    }

    // ---- stealth detection terms (004f1fd0) ---------------------------------------
    {
        kotor::Entity& e = s.scene_.entities[hostileIndex];
        const std::vector<std::int32_t> savedSkills = e.stats.skills;
        const kotor::Vec3 savedAt = e.position;
        const auto savedPcSkills = s.character_.skills;
        s.character_.skills["Stealth"] = 10;
        if (e.stats.skills.size() < 4) {
            e.stats.skills.resize(4, 0);
        }
        e.stats.skills[3] = 40;
        e.position = s.position_;
        e.position.x += 30.0f;
        s.syncEntityToScriptWorld(hostileIndex);
        const bool wasInCombat = a.actor(hostile).combat.inCombat;
        a.actor(hostile).combat.inCombat = true;
        a.actor(hostile).stationary = false;
        a.actor(pc).stationary = false;
        // Old rule: in combat counted full Awareness and had no combat or distance
        // penalty, so 40 Awareness always saw Stealth 10. Retail: 20 + d10 - (11..20) - Stealth
        // - 10 (combat) - 10 (30 m / 3) never reaches 1.
        {
            const kotor::Vec3 from = a.position(hostile);
            const kotor::Vec3 to = a.position(pc);
            logInfo(concat("combat family: stealth terms stealth ", skillOf(pc, 2), " awareness ",
                           skillOf(hostile, 3), " distance ", std::hypot(from.x - to.x, from.y - to.y)));
        }
        check(skillOf(pc, 2) > 0 && !spotsStealth(hostile, pc), "stealth_combat_distance_terms_004f1fd0");
        a.actor(hostile).combat.inCombat = wasInCombat;
        e.stats.skills = savedSkills;
        e.position = savedAt;
        s.syncEntityToScriptWorld(hostileIndex);
        s.character_.skills = savedPcSkills;
    }

    // ---- faction 5 never enters combat (004f2610) -------------------------------
    {
        const int savedFaction = factionOf(hostile);
        const bool savedCombat = a.actor(hostile).combat.inCombat;
        a.actor(hostile).combat.inCombat = false;
        s.setObjectFaction(hostile, 5);
        enterCombat(hostile);
        // Old rule: any creature entered combat when attacked.
        check(factionOf(hostile) == 5 && !a.actor(hostile).combat.inCombat,
              "neutral_faction_no_combat_004f2610");
        s.setObjectFaction(hostile, savedFaction);
        a.actor(hostile).combat.inCombat = savedCombat;
    }

    // ---- AdjustReputation within one faction changes nothing (0052e1a0 / 00514070) -------
    {
        // Old rule: the target faction's row moved even toward itself.
        const int own = s.factionOfObject(hostile);
        const int before = s.factions_.reputation(own, own);
        call(hostile, 209, {obj(hostile), obj(hostile), num(-50)});
        const int after = s.factions_.reputation(own, own);
        logInfo(concat("combat family: adjust own faction ", own, " ", before, " -> ", after));
        check(after == before, "adjust_reputation_same_faction_noop_00514070");
        s.factions_.setReputation(own, own, before);
    }

    // ---- Defense and the equip screen's hand numbers (004ed1d0, 005a7770, 005a9e10) -----------
    {
        // The walk's recommended Scoundrel: level 1, Dexterity 16, Strength 10, bare hands.
        CharacterSheet scoundrel;
        scoundrel.classId = 2;
        scoundrel.level = 1;
        scoundrel.str = 10;
        scoundrel.dex = 16;
        scoundrel.feats = {104}; // SCOUNDRELS_LUCK: the scd column of acbonus.2da, no extra term
        std::int32_t low = 0;
        std::int32_t high = 0;
        s.rules_.handDamageRange(scoundrel, false, low, high);
        const std::int32_t defense = s.rules_.armorClass(scoundrel);
        const std::int32_t hit = s.rules_.handAttackBonus(scoundrel, false);
        logInfo(concat("combat family: scoundrel defense ", defense, " hit ", hit, " bare ", low, "-", high,
                       " acbonus L6 ", s.rules_.classArmorBonus(2, 6)));
        // Old rules: 17 (Scoundrel's Luck added again), +1 (any Weapon Focus), 1-3.
        check(defense == 15, "defense_scoundrel_15_004ed1d0");
        check(hit == 0 && low == 1 && high == 1, "bare_hands_1_1_to_hit_0_005a9e10");
        // 005be770 reads row 6 at level 6 (4), not row 5 (2).
        check(s.rules_.classArmorBonus(2, 6) == 4, "acbonus_indexed_by_level_005be770");
        // A double-bladed sword (base item 6, weaponwield 3, 2d6) in the right hand: both columns
        // read that weapon and each takes its two-weapon penalty (005a7770 param 5: main -4 for a
        // double weapon, off -10 without Two-Weapon Fighting; 005a9e10). Old rule: the left column
        // stayed blank, then both penalties were skipped (retail reads -2 / 0 with the feat).
        InventoryItem staff;
        staff.baseItem = 6;
        staff.slot = 4;
        staff.equipped = true;
        scoundrel.inventory.push_back(staff);
        std::int32_t leftLow = 0;
        std::int32_t leftHigh = 0;
        s.rules_.handDamageRange(scoundrel, true, leftLow, leftHigh, true);
        s.rules_.handDamageRange(scoundrel, false, low, high, true);
        const std::int32_t leftHit = s.rules_.handAttackBonus(scoundrel, true, true);
        const std::int32_t rightHit = s.rules_.handAttackBonus(scoundrel, false, true);
        logInfo(concat("combat family: double blade ", leftLow, "-", leftHigh, " ", leftHit, " / ", low, "-", high, " ",
                       rightHit));
        check(leftLow == 2 && leftHigh == 12 && low == 2 && high == 12 && leftHit == -10 && rightHit == -4,
              "double_weapon_fills_both_columns_006b9970");

        // A blaster pistol with Enhancement +3 (g_w_blstrpstl004: property 5, iprp_meleecost row 3)
        // against the plain one: 004f3fe0 adds its attack and damage effects to 005a7b60 / 005a9e10.
        // Old rule: item properties were read for creatures only; a party member's weapon showed the
        // base item's numbers.
        CharacterSheet gunner = scoundrel;
        gunner.inventory.clear();
        InventoryItem pistol;
        pistol.resref = "g_w_blstrpstl001";
        pistol.baseItem = 12;
        pistol.slot = 4;
        pistol.equipped = true;
        gunner.inventory.push_back(pistol);
        std::int32_t plainLow = 0;
        std::int32_t plainHigh = 0;
        s.rules_.handDamageRange(gunner, false, plainLow, plainHigh);
        const std::int32_t plainHit = s.rules_.handAttackBonus(gunner, false);
        gunner.inventory.front().resref = "g_w_blstrpstl004";
        std::int32_t enchLow = 0;
        std::int32_t enchHigh = 0;
        s.rules_.handDamageRange(gunner, false, enchLow, enchHigh);
        const std::int32_t enchHit = s.rules_.handAttackBonus(gunner, false);
        logInfo(concat("combat family: pistol plain ", plainLow, "-", plainHigh, " ", plainHit, " enhanced ", enchLow,
                       "-", enchHigh, " ", enchHit));
        check(enchHit == plainHit + 3 && enchLow == plainLow + 3 && enchHigh == plainHigh + 3,
              "weapon_item_properties_add_to_hit_and_damage_004f3fe0");

        // The five weapons RETAIL's equip screen showed (SC\paired\retail\raw\req_eq_a..e) on the
        // Taris save: Strength 14 (+2), base attack +2, Two-Weapon Fighting only. Upgrade-slot
        // properties (Mission's Vibroblade 38 and 11/12) count only once the upgrade is installed.
        {
            CharacterSheet fighter;
            fighter.classId = 0;
            fighter.level = 2;
            fighter.str = 14;
            fighter.dex = 12;
            fighter.feats = {3};
            struct Row {
                const char* resref;
                std::int32_t baseItem;
                bool dbl;
                std::int32_t low;
                std::int32_t high;
                std::int32_t hitRight;
                std::int32_t hitLeft;
            };
            const Row rows[] = {
                {"g_w_vbroswrd01", 3, false, 4, 14, 4, 0},
                {"g_w_dblswrd001", 6, true, 4, 14, 0, -2},
                {"g_w_vbroswrd02", 3, false, 5, 15, 5, 0},
                {"g_w_vbroshort08", 5, false, 4, 13, 4, 0},
                {"g_w_vbroshort03", 5, false, 7, 16, 6, 0},
            };
            bool all = true;
            for (const Row& row : rows) {
                CharacterSheet sheet = fighter;
                InventoryItem item;
                item.resref = row.resref;
                item.baseItem = row.baseItem;
                item.slot = 4;
                item.equipped = true;
                sheet.inventory.push_back(item);
                std::int32_t rLow = 0;
                std::int32_t rHigh = 0;
                s.rules_.handDamageRange(sheet, false, rLow, rHigh, row.dbl);
                const std::int32_t rHitRight = s.rules_.handAttackBonus(sheet, false, row.dbl);
                bool rOk = rLow == row.low && rHigh == row.high && rHitRight == row.hitRight;
                std::int32_t rLeftLow = 0;
                std::int32_t rLeftHigh = 0;
                std::int32_t rHitLeft = 0;
                if (row.dbl) {
                    s.rules_.handDamageRange(sheet, true, rLeftLow, rLeftHigh, true);
                    rHitLeft = s.rules_.handAttackBonus(sheet, true, true);
                    rOk = rOk && rLeftLow == row.low && rLeftHigh == row.high && rHitLeft == row.hitLeft;
                }
                logInfo(concat("combat family: retail weapon ", row.resref, " ", rLow, "-", rHigh, " ", rHitRight,
                               " left ", rLeftLow, "-", rLeftHigh, " ", rHitLeft, rOk ? " ok" : " MISMATCH"));
                all = all && rOk;
            }
            // The save's own loadout (retail eq_a1): Long Sword left, Vibrosword right (4-14 is its range), 3-14 and 4-14,
            // to hit -2 and -2 (main hand -6 since the long sword is no light off hand, off hand -6).
            {
                CharacterSheet sheet = fighter;
                InventoryItem shortSword;
                shortSword.resref = "g_w_vbroswrd01";
                shortSword.baseItem = 3;
                shortSword.slot = 4;
                shortSword.equipped = true;
                InventoryItem longSword;
                longSword.resref = "g_w_lngswrd01";
                longSword.baseItem = 2;
                longSword.slot = 5;
                longSword.equipped = true;
                sheet.inventory.push_back(shortSword);
                sheet.inventory.push_back(longSword);
                std::int32_t rightLow = 0;
                std::int32_t rightHigh = 0;
                std::int32_t leftLowD = 0;
                std::int32_t leftHighD = 0;
                s.rules_.handDamageRange(sheet, false, rightLow, rightHigh);
                s.rules_.handDamageRange(sheet, true, leftLowD, leftHighD);
                const std::int32_t rightHitD = s.rules_.handAttackBonus(sheet, false);
                const std::int32_t leftHitD = s.rules_.handAttackBonus(sheet, true);
                logInfo(concat("combat family: retail loadout left ", leftLowD, "-", leftHighD, " ", leftHitD,
                               " right ", rightLow, "-", rightHigh, " ", rightHitD));
                all = all && leftLowD == 3 && leftHighD == 14 && leftHitD == -2 && rightLow == 4
                    && rightHigh == 14 && rightHitD == -2;
            }
            check(all, "equip_screen_weapon_numbers_match_retail_006b9970");
        }
    }

    // ---- 004f3fe0 stacking: one spell's attack bonus counts once, the largest ---------------------
    {
        // Old rule: every attack-increase effect added up, uncapped.
        const std::uint32_t target = hostile;
        const std::int32_t before = attackEffectNet(target, 1);
        const auto add = [&](std::int32_t amount, std::int32_t spell) {
            EffectData data;
            data.type = 40;
            data.args = {script::Value::makeInt(amount), script::Value::makeInt(0)};
            const std::int64_t id = a.sim().engine.add(std::move(data));
            AppliedEffect held;
            held.effect = id;
            held.type = 40;
            held.durationType = 2;
            held.spellId = spell;
            a.actor(target).effects.push_back(held);
        };
        const std::size_t effectsBefore = a.actor(target).effects.size();
        add(2, 77);
        add(3, 77); // the same spell again: only the larger counts
        add(4, -1); // no spell: adds
        const std::int32_t stacked = attackEffectNet(target, 1);
        add(30, -1); // past the cap of 20
        const std::int32_t capped = attackEffectNet(target, 1);
        a.actor(target).effects.resize(effectsBefore);
        check(stacked == before + 3 + 4 && capped == 20 && attackEffectNet(target, 1) == before,
              "attack_effects_stack_per_spell_and_cap_at_20_004f3fe0");
    }

    // ---- Force resistance: the largest increase minus the largest penalty, floor 0 (004db180) -----
    {
        const std::uint32_t target = hostile;
        const std::size_t effectsBefore = a.actor(target).effects.size();
        const auto add = [&](std::int32_t type, std::int32_t amount) {
            EffectData data;
            data.type = type;
            data.args = {script::Value::makeInt(amount)};
            const std::int64_t id = a.sim().engine.add(std::move(data));
            AppliedEffect held;
            held.effect = id;
            held.type = type;
            held.durationType = 2;
            a.actor(target).effects.push_back(held);
        };
        const std::int32_t before = forceResistance(a, target);
        add(52, 5);
        add(52, 8); // not added: the stat keeps the larger
        const std::int32_t twoIncreases = forceResistance(a, target);
        add(53, 3);
        const std::int32_t withPenalty = forceResistance(a, target);
        add(53, 30);
        const std::int32_t floored = forceResistance(a, target);
        a.actor(target).effects.resize(effectsBefore);
        check(before == 0 && twoIncreases == 8 && withPenalty == 5 && floored == 0,
              "force_resistance_is_largest_increase_minus_penalty_005a5a60");
    }

    // ---- AdjustReputation reads [source][target], writes [target][source] (0052b420/0052b3d0) --
    {
        std::size_t otherIndex = kNone;
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const kotor::Entity& e = s.scene_.entities[i];
            if (i != hostileIndex && e.kind == kotor::EntityKind::Creature && !e.dead) {
                otherIndex = i;
                break;
            }
        }
        if (otherIndex != kNone) {
            const std::uint32_t other = a.objectFor(otherIndex);
            const int hostileFaction = s.factionOfObject(hostile);
            const int otherFaction = s.factionOfObject(other);
            s.setObjectFaction(hostile, 2);
            s.setObjectFaction(other, 4);
            const int saved24 = s.factions_.reputation(2, 4);
            const int saved42 = s.factions_.reputation(4, 2);
            s.setReputation(2, 4, 30);
            s.setReputation(4, 2, 60);
            call(hostile, 209, {obj(hostile), obj(other), num(5)});
            const int moved = s.factions_.reputation(2, 4);
            // A dead source changes nothing (0052e20d).
            s.scene_.entities[otherIndex].dead = true;
            call(hostile, 209, {obj(hostile), obj(other), num(5)});
            const int afterDead = s.factions_.reputation(2, 4);
            s.scene_.entities[otherIndex].dead = false;
            logInfo(concat("combat family: adjust 2<-4 30/60 +5 -> ", moved, " dead source -> ", afterDead));
            // Old rule: 30 + 5 = 35.
            check(moved == 65 && s.factions_.reputation(4, 2) == 60, "adjust_reputation_reads_source_row_0052b420");
            check(afterDead == 65, "adjust_reputation_dead_source_noop_0052e1a0");
            s.setReputation(2, 4, saved24);
            s.setReputation(4, 2, saved42);
            s.setObjectFaction(hostile, hostileFaction);
            s.setObjectFaction(other, otherFaction);
        }
    }

    // ---- perception schedule (004eb6e0) -------------------------------------------
    {
        // Old rule: every creature re-perceived together once a second. Retail: the PC every
        // 300..699 ms, any other creature 4000 ms after its own previous pass.
        kotor::Entity& e = s.scene_.entities[hostileIndex];
        const kotor::Vec3 savedAt = e.position;
        const bool savedCombat = a.actor(hostile).combat.inCombat;
        e.position = s.position_;
        e.position.x += 500.0f;
        s.syncEntityToScriptWorld(hostileIndex);
        a.actor(hostile).perceptionStarted = false;
        a.actor(pc).perceptionStarted = false;
        perceive(0.01f); // both run their first pass now
        const bool farUnseen = !creatureHasPerceived(a, hostile, pc, 3) && !creatureHasPerceived(a, pc, hostile, 3);
        e.position = s.position_;
        e.position.x += 3.0f;
        s.syncEntityToScriptWorld(hostileIndex);
        perceive(0.75f);
        const bool pcFirst = creatureHasPerceived(a, pc, hostile, 2) && !creatureHasPerceived(a, hostile, pc, 3);
        perceive(1.0f);
        const bool npcWaits = !creatureHasPerceived(a, hostile, pc, 3);
        perceive(2.3f);
        const bool npcAt4s = creatureHasPerceived(a, hostile, pc, 2);
        logInfo(concat("combat family: perception schedule far ", farUnseen, " pc ", pcFirst, " npc wait ",
                       npcWaits, " npc 4s ", npcAt4s));
        check(farUnseen && pcFirst && npcWaits && npcAt4s, "perception_schedule_pc_fast_npc_4s_004eb6e0");
        a.clearQueue(hostile);
        a.actor(hostile).combat.inCombat = savedCombat;
        e.position = savedAt;
        s.syncEntityToScriptWorld(hostileIndex);
        s.update(1.0f / 30.0f, InputState{});
    }

    // ---- melee hit at the swing's impact frame (005bb890 / 005b4f30 / 004faf20) ------------
    {
        // Old rule: a one-attack round struck at 1.5 s with the swing starting at the hit.
        // Retail: the swing starts with the round and the hit lands at combatanimations hit1.
        // The hostile fights unarmed here (wield class 8: g8a1/g8a2, rows 247/248, hit1 500 ms).
        kotor::Entity& body = s.scene_.entities[hostileIndex];
        const kotor::Entity savedBody = body;
        body.weaponModel.clear();
        body.offhandModel.clear();
        body.ranged = false;
        const ActorCombat saved = a.actor(hostile).combat;
        const float expected = s.nextAttackImpact(hostileIndex);
        startRound(hostile, pc, -1);
        const ActorCombat& round = a.actor(hostile).combat;
        logInfo(concat("combat family: melee impact expected ", expected, " scheduled ",
                       round.attackTimes.empty() ? -1.0f : round.attackTimes.front(), " g8a1 ",
                       s.rules_.attackImpactMs("g8a1")));
        check(s.rules_.attackImpactMs("g8a1") == 500 && std::abs(expected - 0.5f) < 1.0e-4f
                  && round.attackTimes.size() == 1 && std::abs(round.attackTimes.front() - 0.5f) < 1.0e-4f
                  && round.swingStarted,
              "melee_hit_at_impact_time_005b4f30");
        // Sub-attack i reads column i+1 by index (005bb890 -> 005b4f30 -> 00414260): hit2 is blank in
        // every shipped row, so a second attack lands with the first; the fourth reads parry0.
        // Old rule: several attacks were spaced evenly over the round.
        {
            const bool readsColumns = s.rules_.attackImpactMs("g8a1", 1) == 0
                && s.rules_.attackImpactMs("g8a1", 3) == 278;
            // An off-hand weapon adds the second attack (004d2a70).
            const bool hadOffhand = body.stats.offHand.present;
            body.stats.offHand.present = true;
            startRound(hostile, pc, -1);
            const ActorCombat two = a.actor(hostile).combat;
            body.stats.offHand.present = hadOffhand;
            logInfo(concat("combat family: two attacks at ", two.attackTimes.size() > 0 ? two.attackTimes[0] : -1.0f,
                           " ", two.attackTimes.size() > 1 ? two.attackTimes[1] : -1.0f, " columns ", readsColumns));
            check(readsColumns && two.attackTimes.size() == 2 && std::abs(two.attackTimes[0] - 0.5f) < 1.0e-4f
                      && std::abs(two.attackTimes[1] - 0.5f) < 1.0e-4f,
                  "second_attack_lands_with_the_first_005bb890");
        }
        a.actor(hostile).combat = saved;
        body.weaponModel = savedBody.weaponModel;
        body.offhandModel = savedBody.offhandModel;
        body.ranged = savedBody.ranged;
        // A blaster pistol (weaponwield 4 -> b5a1, hits 0): the shot is signalled as the attack starts.
        {
            body.weaponModel = "w_blstrpstl_001";
            body.offhandModel.clear();
            body.rightWield = 4;
            body.ranged = true;
            const float shot = s.nextAttackImpact(hostileIndex);
            startRound(hostile, pc, -1);
            const ActorCombat& ranged = a.actor(hostile).combat;
            logInfo(concat("combat family: ranged impact ", shot, " scheduled ",
                           ranged.attackTimes.empty() ? -1.0f : ranged.attackTimes.front()));
            // Old rule: 1.5 s into the round.
            check(shot == 0.0f && ranged.attackTimes.size() == 1 && ranged.attackTimes.front() == 0.0f,
                  "ranged_hit_at_attack_start_005b4f30");
            a.actor(hostile).combat = saved;
            body.weaponModel = savedBody.weaponModel;
            body.offhandModel = savedBody.offhandModel;
            body.rightWield = savedBody.rightWield;
            body.ranged = savedBody.ranged;
        }
    }

    // ---- steering in combat shows 42476, letting go puts 48208 back (006227e0) ----------
    {
        // Old rule: steering raised no combat message.
        ActorCombat& combat = a.actor(pc).combat;
        const bool savedCombat = combat.inCombat;
        const float savedTimer = combat.combatTimer;
        const kotor::Vec3 savedAt = s.position_;
        combat.inCombat = true;
        combat.combatTimer = 60.0f;
        (void)s.takeCombatMessage();
        InputState walk{};
        walk.forward = true;
        s.update(1.0f / 30.0f, walk);
        const std::uint32_t moving = s.takeCombatMessage();
        s.update(1.0f / 30.0f, InputState{});
        const std::uint32_t stopped = s.takeCombatMessage();
        logInfo(concat("combat family: steering message ", moving, " then ", stopped));
        check(moving == 42476u && stopped == 48208u, "steering_raises_42476_006227e0");
        a.actor(pc).combat.inCombat = savedCombat;
        a.actor(pc).combat.combatTimer = savedTimer;
        s.position_ = savedAt;
        s.syncPlayerToScriptWorld();
    }

    // ---- melee reach from CREPERSPACE (004f1310 / 004fb0f0) -------------------------
    {
        const float expected = s.creatureApproachRange(pc, hostile, 0.0f) - 0.3f + 0.7f + 0.5f;
        const float got = reach(pc, hostile);
        logInfo(concat("combat family: melee reach ", got, " (creperspace sum + 1.2)"));
        // Old rule: a fixed 2.4 m for every pair.
        check(profile(pc, false, -1).ranged || std::fabs(got - expected) < 1e-4f,
              "melee_reach_creperspace_004fb0f0");
    }

    // ---- an unseen attacker takes the defender's Dex bonus away (005ad0e0) ---------
    {
        kotor::Entity& e = s.scene_.entities[hostileIndex];
        const std::int32_t savedDex = e.stats.abilities[1];
        const auto savedPerceived = a.actor(hostile).perceived;
        e.stats.abilities[1] = 16; // +3
        a.actor(hostile).perceived[pc] = 3;
        const std::int32_t seenAc = armorClass(hostile, pc);
        a.actor(hostile).perceived.erase(pc);
        const std::int32_t unseenAc = armorClass(hostile, pc);
        // Old rule: AC did not depend on whether the defender saw the attacker.
        check(seenAc - unseenAc == 3, "unseen_attacker_drops_dex_005ad0e0");
        e.stats.abilities[1] = savedDex;
        a.actor(hostile).perceived = savedPerceived;
    }

    // ---- a failed save against an immune save type returns 2 (005b92b0) -----------
    {
        applyEngineEffect(hostile, 15, {script::Value::makeInt(4)}, 6.0f, pc); // IMMUNITY_TYPE_FEAR
        // Old rule: 0 or 1 only. DC 500 always fails the roll.
        check(savingThrow(hostile, 3, 500, 8) == 2 && savingThrow(hostile, 3, 500, 0) == 0
                  && savingThrow(hostile, 3, -500, 8) == 1,
              "save_immune_returns_2_005b92b0");
        std::erase_if(a.actor(hostile).effects, [](const AppliedEffect& held) { return held.type == 15; });
    }

    // ---- the leader picks the next enemy after a kill (004d4620 -> 005b6980) -------
    {
        std::size_t secondIndex = kNone;
        for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
            const kotor::Entity& other = s.scene_.entities[i];
            if (i != hostileIndex && other.kind == kotor::EntityKind::Creature && !other.dead
                && !other.partyAvatar && isEnemy(a, pc, a.objectFor(i))) {
                secondIndex = i;
                break;
            }
        }
        if (secondIndex == kNone) {
            logInfo("combat family: no second hostile for the auto-target check (not run)");
        } else if (s.leaderTag_ == "PC" && a.isAlive(hostile)) {
            const std::uint32_t second = a.objectFor(secondIndex);
            kotor::Entity& near = s.scene_.entities[secondIndex];
            near.position = s.position_;
            near.position.x += 1.5f;
            near.plot = false;
            s.syncEntityToScriptWorld(secondIndex);
            a.actor(pc).perceived[second] = 3;
            a.clearQueue(pc);
            kill(hostile, pc);
            continueLeaderAttack(pc, hostile);
            const ActorState& pcState = a.actor(pc);
            check(!pcState.queue.empty() && pcState.queue.front().kind == ActorActionKind::Attack
                      && pcState.queue.front().object == second,
                  "leader_auto_targets_next_enemy_005b6980");
            a.clearQueue(pc);
            resurrect(hostile);
            near.position.x += 500.0f;
            s.syncEntityToScriptWorld(secondIndex);
        }
    }

    // ---- companions fight through the same path -----------------------------------
    // A party member is an ordinary target of the damage path (the party family
    // covers forming a party; a shipped module without one has no avatar to test).
    for (std::size_t i = 0; i < s.scene_.entities.size(); ++i) {
        if (s.scene_.entities[i].partyAvatar && !s.scene_.entities[i].dead) {
            const std::uint32_t member = a.objectFor(i);
            apply(hostile, kInstant, makeEffect(79, {num(1000000), num(8), num(0)}), member, 0.0);
            check(!a.isAlive(member), "companion_can_be_killed");
            // 004b6da0: with no hostile perceiving the party the member stands up after 5 s.
            for (std::size_t k = 0; k < s.scene_.entities.size(); ++k) {
                kotor::Entity& other = s.scene_.entities[k];
                if (other.kind == kotor::EntityKind::Creature && !other.partyAvatar
                    && isEnemy(a, pc, a.objectFor(k))) {
                    other.position.x += 1000.0f;
                    s.syncEntityToScriptWorld(k);
                    a.actor(a.objectFor(k)).perceived.clear();
                }
            }
            tick(3.0f);
            check(!a.isAlive(member), "companion_still_down_before_5s_004b6da0");
            tick(4.0f);
            check(a.isAlive(member), "companion_recovers_after_5s_004b6da0");
            // 004babb0 runs TempDeathUpdate on the real clock while the world is paused, so the
            // 5 s can pass during a pause (retail capture: Carth stood up 1.3 s after an unpause
            // that followed a ~6 s pause). Old rule: the clock only ran with the world.
            apply(hostile, kInstant, makeEffect(79, {num(1000000), num(8), num(0)}), member, 0.0);
            for (int step = 0; step < 12; ++step) {
                s.tickWhilePaused(0.5f);
            }
            check(a.isAlive(member), "companion_recovers_while_paused_004babb0");
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
