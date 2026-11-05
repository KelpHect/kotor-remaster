// Effect*, ApplyEffect*, removal and iteration. An effect is an EffectData record;
// instant kinds act once on apply, the rest are held on the target's ActorState
// (AppliedEffect) and read by the calculators in ScriptCombat.cpp.
#include "game/ScriptHost.h"

#include "game/GameSession.h"

#include <algorithm>
#include <map>

namespace ke {

namespace {

using Cells = std::optional<std::vector<script::Value>>;

// nwscript EFFECT_TYPE_* values where one exists. Retail's GetEffectType (170)
// maps its internal kinds to these; constructors without a public constant use
// internal kinds above kLastPublicEffect and read back as EFFECT_TYPE_INVALIDEFFECT.
constexpr std::int32_t kLastPublicEffect = 78;

const std::map<std::uint16_t, std::int32_t>& constructorTypes()
{
    static const std::map<std::uint16_t, std::int32_t> types = {
        {51, 74},   // EffectAssuredHit
        {78, effect::kHeal},
        {79, effect::kDamage},
        {80, 38},   // EffectAbilityIncrease
        {81, 1},    // EffectDamageResistance
        {82, 14},   // EffectResurrection
        {115, 46},  // EffectACIncrease
        {117, 50},  // EffectSavingThrowIncrease
        {118, 40},  // EffectAttackIncrease
        {119, 7},   // EffectDamageReduction
        {120, 42},  // EffectDamageIncrease
        {130, 11},  // EffectEntangle
        {133, effect::kDeath},
        {134, effect::kKnockdown},
        {148, 27},  // EffectParalyze
        {149, 73},  // EffectSpellImmunity
        {153, 77},  // EffectForceJump
        {154, 30},  // EffectSleep
        {156, effect::kTemporaryForce},
        {157, 24},  // EffectConfused
        {158, 25},  // EffectFrightened
        {159, effect::kChoke},
        {161, 29},  // EffectStunned
        {164, 3},   // EffectRegenerate
        {165, 48},  // EffectMovementSpeedIncrease
        {171, 20},  // EffectAreaOfEffect
        {180, 75},  // EffectVisualEffect
        {207, 21},  // EffectBeam
        {212, 52},  // EffectForceResistanceIncrease
        {224, effect::kBodyFuel},
        {250, 31},  // EffectPoison
        {252, 78},  // EffectAssuredDeflection
        {269, effect::kForcePushTargeted},
        {270, 36},  // EffectHaste
        {273, 15},  // EffectImmunity
        {275, 44},  // EffectDamageImmunityIncrease
        {314, 9},   // EffectTemporaryHitpoints
        {351, 54},  // EffectSkillIncrease
        {372, effect::kDamageForce},
        {373, effect::kHealForce},
        {387, effect::kDyingHitPoints},
        {391, effect::kDroidStun},
        {392, effect::kForcePushed},
        {402, effect::kForceResisted},
        {420, effect::kForceFizzle},
        {446, 39},  // EffectAbilityDecrease
        {447, 41},  // EffectAttackDecrease
        {448, 43},  // EffectDamageDecrease
        {449, 45},  // EffectDamageImmunityDecrease
        {450, 47},  // EffectACDecrease
        {451, 49},  // EffectMovementSpeedDecrease
        {452, 51},  // EffectSavingThrowDecrease
        {453, 55},  // EffectSkillDecrease
        {454, 53},  // EffectForceResistanceDecrease
        {457, 56},  // EffectInvisibility
        {458, 72},  // EffectConcealment
        {459, effect::kForceShield},
        {460, 59},  // EffectDispelMagicAll
        {463, 62},  // EffectDisguise
        {465, 64},  // EffectTrueSeeing
        {466, 65},  // EffectSeeInvisible
        {467, 66},  // EffectTimeStop
        {469, effect::kDeflectionIncrease},
        {470, effect::kDeflectionDecrease},
        {471, effect::kHorrified},
        {472, 68},  // EffectSpellLevelAbsorption
        {473, 69},  // EffectDispelMagicBest
        {477, 71},  // EffectMissChance
        {485, effect::kModifyAttacks},
        {487, effect::kDamageShield},
        {675, effect::kForceDrain},
        {676, effect::kPsychicStatic},
        {702, 76},  // EffectLightsaberThrow
        {703, effect::kWhirlwind},
        {754, effect::kCutsceneHorrified},
        {755, effect::kCutsceneParalyze},
        {756, effect::kCutsceneStunned},
    };
    return types;
}

Cells makeEffect(ActionCall& call)
{
    EffectData data;
    const auto& types = constructorTypes();
    const auto it = types.find(call.id);
    data.type = it != types.end() ? it->second : 0;
    data.args.reserve(call.args.size());
    for (const std::vector<script::Value>& arg : call.args) {
        if (!arg.empty()) {
            data.args.push_back(arg.front());
        }
    }
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(data)));
}

std::int32_t argInt(const EffectData& data, std::size_t index, std::int32_t fallback = 0)
{
    return index < data.args.size() ? static_cast<std::int32_t>(data.args[index].asInt())
                                    : fallback;
}

// Held effects whose record matches, plus what it links to.
void removeEffectRecord(ScriptApi api, std::uint32_t target, std::int64_t id)
{
    std::erase_if(api.actor(target).effects,
                  [&](const AppliedEffect& held) { return held.effect == id; });
    if (const EffectData* data = api.sim().engine.get<EffectData>(id); data != nullptr) {
        const std::vector<std::int64_t> links = data->links;
        for (const std::int64_t link : links) {
            removeEffectRecord(api, target, link);
        }
    }
}

// Dispel: spell-made effects are the magical ones scripts can strip.
void dispelEffects(ScriptApi api, std::uint32_t target, bool best)
{
    std::vector<AppliedEffect>& held = api.actor(target).effects;
    for (auto it = held.begin(); it != held.end();) {
        if (it->spellId >= 0) {
            it = held.erase(it);
            if (best) {
                return;
            }
        } else {
            ++it;
        }
    }
}

void applyEffect(ScriptApi api, std::int32_t durationType, std::int64_t id, std::uint32_t target,
                 float duration, std::uint32_t creator, std::int32_t spellId)
{
    const EffectData* found = api.sim().engine.get<EffectData>(id);
    if (found == nullptr || target == script::kObjectInvalid) {
        return;
    }
    // Copy: an effect script can add store records, and a damage path runs scripts.
    const EffectData data = *found;
    for (const std::int64_t link : data.links) {
        applyEffect(api, durationType, link, target, duration, creator, spellId);
    }
    const bool creature = api.isCreature(target);
    // Retail effect handlers return early on a dead target (004dfa40, 004e0ac0);
    // resurrection is the one that needs it.
    if (!api.isAlive(target) && data.type != 14) {
        return;
    }
    switch (data.type) {
    case effect::kHeal:
        if (creature) {
            healCreature(api, target, argInt(data, 0));
        }
        return;
    case effect::kDamage: // retail 004dfa40
        applyScriptDamage(api, target, argInt(data, 0), argInt(data, 1, 8), creator);
        return;
    case effect::kDeath: // retail 004e0ac0
        if (creature) {
            killCreature(api, target, creator);
        }
        return;
    case 14:
        if (creature) {
            resurrectCreature(api, target);
        }
        return;
    case effect::kHealForce:
        if (creature) {
            changeForcePoints(api, target, argInt(data, 0));
        }
        return;
    case effect::kDamageForce:
    case effect::kForceDrain:
        if (creature) {
            changeForcePoints(api, target, -argInt(data, 0));
        }
        return;
    case 59: // EffectDispelMagicAll
        dispelEffects(api, target, false);
        return;
    case 69: // EffectDispelMagicBest
        dispelEffects(api, target, true);
        return;
    default:
        break;
    }
    if (durationType != 1 && durationType != 2) {
        return; // An instant application of a held kind leaves nothing behind.
    }
    AppliedEffect held;
    held.effect = id;
    held.type = data.type;
    held.durationType = durationType;
    held.remaining = duration;
    held.creator = creator;
    held.spellId = spellId;
    if (data.type == 3 && data.args.size() > 1) { // EffectRegenerate(nAmount, fInterval)
        held.interval = static_cast<float>(data.args[1].asFloat());
    }
    if (data.type == 9) { // Temporary hit points absorb until spent. retail 004f3830
        held.pool = argInt(data, 0);
    }
    if (data.type == 1) { // EffectDamageResistance(nType, nAmount, nLimit)
        held.pool = argInt(data, 2);
    }
    if (data.type == 7) { // EffectDamageReduction(nAmount, nPower, nLimit)
        held.pool = argInt(data, 2);
    }
    if (creature && isDisablingEffect(data.type)) {
        // A disabled creature stops what it was doing. retail 004ccd80 from 00503e40.
        ActorCombat& combat = api.actor(target).combat;
        combat.roundActive = false;
    }
    api.actor(target).effects.push_back(held);
}

std::int32_t currentSpellId(ScriptApi api)
{
    const ScriptContext& context = api.context();
    const auto it = context.find("spellId");
    return it != context.end() ? static_cast<std::int32_t>(it->second) : -1;
}

// ACT_220 ApplyEffectToObject(nDurationType, eEffect, oTarget, fDuration)
Cells applyEffectToObject(ActionCall& call)
{
    const std::int64_t id = call.engineArg(1);
    if (call.api().sim().engine.get<EffectData>(id) == nullptr) {
        return std::nullopt; // A legacy runtime tag, not a store record.
    }
    applyEffect(call.api(), static_cast<std::int32_t>(call.intArg(0)), id, call.objectArg(2),
                static_cast<float>(call.floatArg(3)), call.self(), currentSpellId(call.api()));
    return ActionCall::none();
}

// ACT_216 ApplyEffectAtLocation. VFX are data only here; an area of effect would
// need a persistent object, which is a stated gap.
Cells applyEffectAtLocation(ActionCall& call)
{
    const EffectData* data = call.api().sim().engine.get<EffectData>(call.engineArg(1));
    if (data == nullptr) {
        return std::nullopt;
    }
    if (data->type == 20) {
        call.api().log("ApplyEffectAtLocation: area of effect not simulated");
    }
    return ActionCall::none();
}

Cells getFirstEffect(ActionCall& call)
{
    ActorState& actor = call.api().actor(call.objectArg(0));
    actor.effectCursor = 0;
    return ActionCall::retEngine(actor.effects.empty() ? 0 : actor.effects.front().effect);
}

Cells getNextEffect(ActionCall& call)
{
    ActorState& actor = call.api().actor(call.objectArg(0));
    ++actor.effectCursor;
    return ActionCall::retEngine(actor.effectCursor < actor.effects.size()
                                     ? actor.effects[actor.effectCursor].effect
                                     : 0);
}

Cells removeEffect(ActionCall& call)
{
    const std::uint32_t target = call.objectArg(0);
    const std::int64_t id = call.engineArg(1);
    if (call.api().sim().engine.get<EffectData>(id) == nullptr) {
        return ActionCall::none();
    }
    ActorState& actor = call.api().actor(target);
    const auto first = std::find_if(actor.effects.begin(), actor.effects.end(),
                                    [&](const AppliedEffect& held) { return held.effect == id; });
    const std::size_t removedAt = static_cast<std::size_t>(first - actor.effects.begin());
    removeEffectRecord(call.api(), target, id);
    // The usual walk removes and carries on with GetNextEffect: step back so the
    // element that slid into this slot is not skipped.
    if (removedAt <= actor.effectCursor && actor.effectCursor > 0) {
        --actor.effectCursor;
    }
    return ActionCall::none();
}

Cells getIsEffectValid(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().engine.get<EffectData>(call.engineArg(0)) != nullptr
                                  ? 1
                                  : 0);
}

Cells getEffectType(ActionCall& call)
{
    const EffectData* data = call.api().sim().engine.get<EffectData>(call.engineArg(0));
    return ActionCall::retInt(data != nullptr && data->type <= kLastPublicEffect ? data->type : 0);
}

const AppliedEffect* findHeld(ScriptApi api, std::int64_t id)
{
    for (const auto& [object, actor] : api.sim().actors) {
        for (const AppliedEffect& held : actor.effects) {
            if (held.effect == id) {
                return &held;
            }
        }
    }
    return nullptr;
}

Cells getEffectDurationType(ActionCall& call)
{
    const AppliedEffect* held = findHeld(call.api(), call.engineArg(0));
    return ActionCall::retInt(held != nullptr ? held->durationType : 0);
}

Cells getEffectCreator(ActionCall& call)
{
    const AppliedEffect* held = findHeld(call.api(), call.engineArg(0));
    return ActionCall::retObject(held != nullptr ? held->creator : script::kObjectInvalid);
}

Cells getEffectSpellId(ActionCall& call)
{
    const AppliedEffect* held = findHeld(call.api(), call.engineArg(0));
    return ActionCall::retInt(held != nullptr ? held->spellId : -1);
}

Cells getEffectSubType(ActionCall& call)
{
    const EffectData* data = call.api().sim().engine.get<EffectData>(call.engineArg(0));
    return ActionCall::retInt(data != nullptr ? data->subType : 0);
}

// MagicalEffect, SupernaturalEffect, ExtraordinaryEffect: nss SUBTYPE_* values.
template <std::int32_t SubType>
Cells setSubType(ActionCall& call)
{
    const EffectData* data = call.api().sim().engine.get<EffectData>(call.engineArg(0));
    if (data == nullptr) {
        return ActionCall::retEngine(call.engineArg(0));
    }
    EffectData copy = *data;
    copy.subType = SubType;
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(copy)));
}

// Versus*: the condition is kept out of the resolver (stated gap); the effect stands.
Cells passEffect(ActionCall& call)
{
    return ActionCall::retEngine(call.engineArg(0));
}

// ACT_199 EffectLinkEffects(eChild, eParent)
Cells linkEffects(ActionCall& call)
{
    const EffectData* parent = call.api().sim().engine.get<EffectData>(call.engineArg(1));
    if (parent == nullptr) {
        return ActionCall::retEngine(0);
    }
    EffectData copy = *parent;
    copy.links.push_back(call.engineArg(0));
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(copy)));
}

// ACT_552 SetEffectIcon: a UI hint, kept as data.
Cells setEffectIcon(ActionCall& call)
{
    const EffectData* data = call.api().sim().engine.get<EffectData>(call.engineArg(0));
    if (data == nullptr) {
        return ActionCall::retEngine(0);
    }
    EffectData copy = *data;
    copy.icon = static_cast<std::int32_t>(call.intArg(1));
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(copy)));
}

Cells clearAllEffects(ActionCall& call)
{
    ActorState& actor = call.api().actor(call.self());
    actor.effects.clear();
    actor.effectCursor = 0;
    return ActionCall::none();
}

// ACT_304 GetHasSpellEffect. A held effect, or the PC/entity Force stack.
Cells getHasSpellEffect(ActionCall& call)
{
    const std::int32_t spell = static_cast<std::int32_t>(call.intArg(0));
    const std::uint32_t object = call.objectArg(1);
    ScriptApi api = call.api();
    for (const AppliedEffect& held : api.actor(object).effects) {
        if (held.spellId == spell) {
            return ActionCall::retInt(1);
        }
    }
    const std::size_t index = api.entityFor(object);
    if (index != static_cast<std::size_t>(-1)) {
        for (const kotor::ActiveEffect& active : api.scene().entities[index].effects) {
            if (active.spellId == spell && active.remaining > 0.0f) {
                return ActionCall::retInt(1);
            }
        }
        return ActionCall::retInt(0);
    }
    return object == api.pc() ? Cells{std::nullopt} : ActionCall::retInt(0);
}

} // namespace

bool isDisablingEffect(std::int32_t type)
{
    switch (type) {
    case 11: // Entangle
    case 24: // Confused
    case 25: // Frightened
    case 27: // Paralyze
    case 29: // Stunned
    case 30: // Sleep
    case effect::kKnockdown:
    case effect::kChoke:
    case effect::kDroidStun:
    case effect::kForcePushed:
    case effect::kForcePushTargeted:
    case effect::kHorrified:
    case effect::kCutsceneHorrified:
    case effect::kCutsceneParalyze:
    case effect::kCutsceneStunned:
        return true;
    default:
        return false;
    }
}

std::int32_t effectTotal(ScriptApi api, std::uint32_t object, std::int32_t type, std::size_t arg)
{
    std::int32_t total = 0;
    for (const AppliedEffect& held : api.actor(object).effects) {
        if (held.type != type) {
            continue;
        }
        if (const EffectData* data = api.sim().engine.get<EffectData>(held.effect);
            data != nullptr && arg < data->args.size()) {
            total += static_cast<std::int32_t>(data->args[arg].asInt());
        }
    }
    return total;
}

bool hasEffectOfType(ScriptApi api, std::uint32_t object, std::int32_t type)
{
    for (const AppliedEffect& held : api.actor(object).effects) {
        if (held.type == type) {
            return true;
        }
    }
    return false;
}

void tickScriptEffects(ScriptApi api, float dt)
{
    std::vector<std::uint32_t> objects;
    for (const auto& [object, actor] : api.sim().actors) {
        if (!actor.effects.empty()) {
            objects.push_back(object);
        }
    }
    for (const std::uint32_t object : objects) {
        std::vector<AppliedEffect>& held = api.actor(object).effects;
        std::int32_t heal = 0;
        for (AppliedEffect& one : held) {
            if (one.type == 3 && one.interval > 0.0f) {
                const EffectData* data = api.sim().engine.get<EffectData>(one.effect);
                if (data != nullptr && data->args.size() > 1) {
                    one.interval -= dt;
                    if (one.interval <= 0.0f) {
                        one.interval += static_cast<float>(data->args[1].asFloat());
                        heal += static_cast<std::int32_t>(data->args[0].asInt());
                    }
                }
            }
            if (one.durationType == 1) {
                one.remaining -= dt;
            }
        }
        std::erase_if(held, [](const AppliedEffect& one) {
            return one.durationType == 1 && one.remaining <= 0.0f;
        });
        if (heal > 0 && api.isAlive(object)) {
            healCreature(api, object, heal);
        }
    }
}

void registerEffectActions(HandlerTable& table)
{
    for (const auto& [id, type] : constructorTypes()) {
        table.add(id, makeEffect);
    }
    table.add(85, getFirstEffect);
    table.add(86, getNextEffect);
    table.add(87, removeEffect);
    table.add(88, getIsEffectValid);
    table.add(89, getEffectDurationType);
    table.add(90, getEffectSubType);
    table.add(91, getEffectCreator);
    table.add(112, setSubType<8>);
    table.add(113, setSubType<16>);
    table.add(114, setSubType<24>);
    table.add(170, getEffectType);
    table.add(199, linkEffects);
    table.add(216, applyEffectAtLocation);
    table.add(220, applyEffectToObject);
    table.add(304, getHasSpellEffect);
    table.add(305, getEffectSpellId);
    table.add(355, passEffect);
    table.add(356, passEffect);
    table.add(357, passEffect);
    table.add(552, setEffectIcon);
    table.add(710, clearAllEffects);
}

} // namespace ke
