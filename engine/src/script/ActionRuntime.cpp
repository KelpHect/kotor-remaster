#include "script/ActionRuntime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace ke::script {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct FieldSpec {
    const char* attr;
    ValueType type;
    std::int64_t integer;
    double real;
    const char* text;
};

const std::unordered_map<std::string, FieldSpec>& fieldSpecs()
{
    static const std::unordered_map<std::string, FieldSpec> specs = {
        {"GetAC", {"ac", ValueType::Int, 10, 0.0, ""}},
        {"GetAlignmentGoodEvil", {"good_evil", ValueType::Int, 50, 0.0, ""}},
        {"GetFortitudeSavingThrow", {"fortitude", ValueType::Int, 0, 0.0, ""}},
        {"GetWillSavingThrow", {"will", ValueType::Int, 0, 0.0, ""}},
        {"GetReflexSavingThrow", {"reflex", ValueType::Int, 0, 0.0, ""}},
        {"GetChallengeRating", {"challenge_rating", ValueType::Int, 1, 0.0, ""}},
        {"GetMovementRate", {"movement_rate", ValueType::Int, 1, 0.0, ""}},
        {"GetWeaponRanged", {"weapon_ranged", ValueType::Int, 0, 0.0, ""}},
        {"GetIdentified", {"identified", ValueType::Int, 1, 0.0, ""}},
        {"GetGoldPieceValue", {"item_value", ValueType::Int, 0, 0.0, ""}},
        {"GetItemACValue", {"item_ac", ValueType::Int, 0, 0.0, ""}},
        {"GetItemHasItemProperty", {"item_props", ValueType::Int, 0, 0.0, ""}},
        {"GetIsTrapped", {"trapped", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapDisarmable", {"trap_disarmable", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapDetectable", {"trap_detectable", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapFlagged", {"trap_flagged", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapOneShot", {"trap_one_shot", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapDisarmDC", {"trap_disarm_dc", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapDetectDC", {"trap_detect_dc", ValueType::Int, 0, 0.0, ""}},
        {"GetTrapKeyTag", {"trap_key_tag", ValueType::String, 0, 0.0, ""}},
        {"GetLockKeyRequired", {"lock_key_required", ValueType::Int, 0, 0.0, ""}},
        {"GetLockKeyTag", {"lock_key_tag", ValueType::String, 0, 0.0, ""}},
        {"GetLockLockable", {"lockable", ValueType::Int, 0, 0.0, ""}},
        {"GetLockUnlockDC", {"unlock_dc", ValueType::Int, 0, 0.0, ""}},
        {"GetLockLockDC", {"lock_dc", ValueType::Int, 0, 0.0, ""}},
        {"GetPlaceableIllumination", {"illumination", ValueType::Int, 0, 0.0, ""}},
        {"GetIsPlaceableObjectActionPossible", {"placeable_action_possible", ValueType::Int, 1, 0.0, ""}},
        {"GetHasFeatEffect", {"feat_effect", ValueType::Int, 0, 0.0, ""}},
        {"GetIsListening", {"listening", ValueType::Int, 0, 0.0, ""}},
        {"GetLastAttackType", {"last_attack_type", ValueType::Int, 0, 0.0, ""}},
        {"GetLastAttackMode", {"last_attack_mode", ValueType::Int, 0, 0.0, ""}},
        {"GetLastAssociateCommand", {"last_associate_command", ValueType::Int, 0, 0.0, ""}},
        {"GetLastWeaponUsed", {"last_weapon", ValueType::Int, 0, 0.0, ""}},
        {"GetLastDisarmed", {"last_disarmed", ValueType::Int, 0, 0.0, ""}},
        {"GetLastDisturbed", {"last_disturbed", ValueType::Int, 0, 0.0, ""}},
        {"GetLastLocked", {"last_locked", ValueType::Int, 0, 0.0, ""}},
        {"GetLastUnlocked", {"last_unlocked", ValueType::Int, 0, 0.0, ""}},
        {"GetLastPerceptionHeard", {"last_perception_heard", ValueType::Int, 0, 0.0, ""}},
        {"GetLastPerceptionInaudible", {"last_perception_inaudible", ValueType::Int, 0, 0.0, ""}},
        {"GetLastClosedBy", {"last_closed_by", ValueType::Int, 0, 0.0, ""}},
        {"GetLastTrapDetected", {"last_trap_detected", ValueType::Int, 0, 0.0, ""}},
        {"GetAreaOfEffectCreator", {"aoe_creator", ValueType::Int, 0, 0.0, ""}},
        {"GetLastItemEquipped", {"last_item_equipped", ValueType::Int, 0, 0.0, ""}},
        {"GetSpellCastItem", {"spell_cast_item", ValueType::Int, 0, 0.0, ""}},
        {"GetItemActivatedTarget", {"item_activated_target", ValueType::Int, 0, 0.0, ""}},
        {"GetItemActivatedTargetLocation", {"item_activated_location", ValueType::Int, 0, 0.0, ""}},
        {"GetStartingLocation", {"starting_location", ValueType::Int, 0, 0.0, ""}},
        {"GetLastPlayerDying", {"last_player_dying", ValueType::Int, 0, 0.0, ""}},
        {"GetLastRespawnButtonPresser", {"last_respawn_button_presser", ValueType::Int, 0, 0.0, ""}},
        {"GetClickingObject", {"clicking_object", ValueType::Int, 0, 0.0, ""}},
        {"GetLastConversation", {"last_conversation", ValueType::String, 0, 0.0, ""}},
        {"GetModuleName", {"module", ValueType::String, 0, 0.0, "end_m01aa"}},
        {"GetGameDifficulty", {"difficulty", ValueType::Int, 1, 0.0, ""}},
        {"GetDifficultyModifier", {"difficulty_modifier", ValueType::Int, 0, 0.0, ""}},
        {"GetEncounterSpawnsMax", {"encounter_spawns_max", ValueType::Int, 10, 0.0, ""}},
        {"GetEncounterSpawnsCurrent", {"encounter_spawns_current", ValueType::Int, 0, 0.0, ""}},
        {"GetEncounterDifficulty", {"encounter_difficulty", ValueType::Int, 1, 0.0, ""}},
        {"GetMaxStealthXP", {"max_stealth_xp", ValueType::Int, 100, 0.0, ""}},
        {"GetStealthXPEnabled", {"stealth_enabled", ValueType::Int, 1, 0.0, ""}},
        {"GetStealthXPDecrement", {"stealth_decrement", ValueType::Int, 5, 0.0, ""}},
        {"IsMoviePlaying", {"movie_playing", ValueType::Int, 0, 0.0, ""}},
        {"GetMatchedSubstring", {"matched_substring", ValueType::String, 0, 0.0, ""}},
        {"GetMatchedSubstringsCount", {"matched_count", ValueType::Int, 0, 0.0, ""}},
        {"GetReputation", {"reputation", ValueType::Int, 50, 0.0, ""}},
        {"GetButtonMashCheck", {"button_mash", ValueType::Int, 0, 0.0, ""}},
        {"GetIsPlayableRacialType", {"playable_racial_type", ValueType::Int, 1, 0.0, ""}},
        {"GetPCLevellingUp", {"pc_levelling_up", ValueType::Int, 0, 0.0, ""}},
        {"GetStrRefSoundDuration", {"strref_sound_duration", ValueType::Float, 0, 0.0, ""}},
        {"GetTransitionTarget", {"transition_target", ValueType::Int, 0, 0.0, ""}},
        {"GetObjectHeard", {"last_speaker", ValueType::Int, 0, 0.0, ""}},
        {"GetGoingToBeAttackedBy", {"last_attacker", ValueType::Int, 0, 0.0, ""}},
        {"GetJournalQuestExperience", {"journal_xp", ValueType::Int, 0, 0.0, ""}},
    };
    return specs;
}

const std::set<std::string>& worldOnly()
{
    static const std::set<std::string> names = {
        "GetLastItemEquipped", "GetLastPerceptionHeard", "GetLastPerceptionInaudible",
        "GetLastClosedBy", "GetClickingObject", "GetLastDisarmed", "GetLastDisturbed",
        "GetLastLocked", "GetLastUnlocked", "GetLastPlayerDying", "GetStartingLocation",
        "GetLastRespawnButtonPresser", "GetSpellCastItem", "GetItemActivatedTargetLocation",
        "GetItemActivatedTarget", "GetLastConversation",
    };
    return names;
}

const std::set<std::string>& worldAttrs()
{
    static const std::set<std::string> names = {
        "GetGameDifficulty", "GetDifficultyModifier", "GetEncounterSpawnsMax",
        "GetEncounterSpawnsCurrent", "GetEncounterDifficulty", "GetMaxStealthXP",
        "GetStealthXPEnabled", "GetStealthXPDecrement", "IsMoviePlaying",
        "GetMatchedSubstring", "GetMatchedSubstringsCount", "GetReputation",
        "GetButtonMashCheck", "GetIsPlayableRacialType", "GetPCLevellingUp",
        "GetStrRefSoundDuration",
    };
    return names;
}

bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

std::string lowerAscii(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool allDigits(const std::string& text)
{
    return !text.empty()
        && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::int64_t absInt(std::int64_t value)
{
    return value < 0 ? -value : value;
}

} // namespace

ActionRuntime::ActionRuntime(World* world, ActionHost* host)
{
    if (world == nullptr) {
        ownedWorld_ = new World();
        world_ = ownedWorld_;
    } else {
        world_ = world;
    }
    host_ = host;
}

// The runtime owns a World only when constructed without one. It lives for the
// duration of the runtime (engine sessions and corpus runs), so a raw pointer
// is intentional and never escapes.
// (Destructor is implicit; the owned world is released with the runtime.)

ActionRuntime::~ActionRuntime()
{
    delete ownedWorld_;
}

Value ActionRuntime::cell(const Args& args, std::size_t index, ParamKind kind) const
{
    if (index < args.size() && !args[index].empty()) {
        return args[index].front();
    }
    switch (kind) {
    case ParamKind::Float: return Value::makeFloat(0.0);
    case ParamKind::String: return Value::makeString("");
    case ParamKind::Object: return Value::makeObject(kObjectInvalid);
    case ParamKind::Engine: return Value::makeEngine(0);
    default: return Value::makeInt(0);
    }
}

std::int64_t ActionRuntime::argInt(const Args& args, std::size_t index) const
{
    return cell(args, index, ParamKind::Int).asInt();
}

double ActionRuntime::argFloat(const Args& args, std::size_t index) const
{
    return cell(args, index, ParamKind::Float).asFloat();
}

std::string ActionRuntime::argString(const Args& args, std::size_t index) const
{
    return cell(args, index, ParamKind::String).asString();
}

std::uint32_t ActionRuntime::argObject(const Args& args, std::size_t index) const
{
    const Value value = cell(args, index, ParamKind::Object);
    const std::uint32_t id = value.type == ValueType::Object
        ? static_cast<std::uint32_t>(value.integer)
        : kObjectInvalid;
    return selfAware_ && id == 0 ? contextObject("self") : id;
}

std::vector<Value> ActionRuntime::argVector(const Args& args, std::size_t index) const
{
    if (index < args.size() && args[index].size() >= 3) {
        return {Value::makeFloat(args[index][0].asFloat()),
                Value::makeFloat(args[index][1].asFloat()),
                Value::makeFloat(args[index][2].asFloat())};
    }
    return {Value::makeFloat(0.0), Value::makeFloat(0.0), Value::makeFloat(0.0)};
}

std::vector<Value> ActionRuntime::ret(std::uint16_t actionId) const
{
    std::vector<Value> out;
    appendDefault(out, actionSpec(actionId).returnKind);
    return out;
}

std::string ActionRuntime::localKey(const Args& args, std::size_t index) const
{
    return std::to_string(argInt(args, index));
}

std::uint32_t ActionRuntime::firstObjectArg(const Args& args, std::uint16_t actionId) const
{
    const ActionSpec& spec = actionSpec(actionId);
    for (std::size_t i = 0; i < spec.paramCount; ++i) {
        if (spec.params[i] == ParamKind::Object) {
            return argObject(args, i);
        }
    }
    return kObjectInvalid;
}

std::vector<Value> ActionRuntime::fieldValue(ValueType type, std::int64_t integer, double real,
                                             const std::string& text) const
{
    switch (type) {
    case ValueType::String: return {Value::makeString(text)};
    case ValueType::Float: return {Value::makeFloat(real)};
    case ValueType::Object: return {Value::makeObject(static_cast<std::uint32_t>(integer))};
    case ValueType::Engine: return {Value::makeEngine(integer)};
    default: return {Value::makeInt(integer)};
    }
}

std::uint32_t ActionRuntime::contextObject(const std::string& name) const
{
    const auto it = context_.find(name);
    return it == context_.end() ? kObjectInvalid : it->second;
}

std::vector<Value> ActionRuntime::call(std::uint16_t actionId, const Args& args, Vm& vm)
{
    const ActionSpec& spec = actionSpec(actionId);
    const std::string name = spec.name;
    if (traceEvents_ && world_ != nullptr) {
        world_->events.push_back(WorldEvent{world_->time, actionId, name, "unknown"});
    }
    ++calls_[actionId];
    if (host_ != nullptr) {
        if (auto handled = host_->action(actionId, args, *world_, vm); handled.has_value()) {
            return *handled;
        }
    }
    if (auto handled = specific(name, args, vm, actionId); handled.has_value()) {
        specific_.insert(name);
        if (traceEvents_ && !world_->events.empty()) {
            world_->events.back().kind = "specific";
        }
        return *handled;
    }
    if (traceEvents_ && !world_->events.empty()) {
        world_->events.back().kind = "generic";
    }
    return generic(name, args, vm, actionId);
}

std::optional<std::vector<Value>> ActionRuntime::specific(const std::string& name, const Args& args,
                                                          Vm& vm, std::uint16_t actionId)
{
    (void)actionId;
    if (name == "GetTag") return hGetTag(args);
    if (name == "GetObjectByTag") return hGetObjectByTag(args);
    if (name == "GetWaypointByTag") return hGetWaypointByTag(args);
    if (name == "GetArea") return hGetArea(args);
    if (name == "GetFirstPC") return hGetFirstPC(args);
    if (name == "GetPartyMemberByIndex") return hGetPartyMemberByIndex(args);
    if (name == "GetIsObjectValid") return hGetIsObjectValid(args);
    if (name == "GetNextObjectInArea") return hGetNextObjectInArea(args);
    if (name == "GetNextItemInInventory") return hGetNextItemInInventory(args);
    if (name == "GetItemInSlot") return hGetItemInSlot(args);
    if (name == "GetBaseItemType") return hGetBaseItemType(args);
    if (name == "GetGold") return hGetGold(args);
    if (name == "GetGlobalNumber") return hGetGlobalNumber(args);
    if (name == "SetGlobalNumber") return hSetGlobalNumber(args);
    if (name == "GetGlobalBoolean") return hGetGlobalBoolean(args);
    if (name == "SetGlobalBoolean") return hSetGlobalBoolean(args);
    if (name == "GetGlobalString") return hGetGlobalString(args);
    if (name == "SetGlobalString") return hSetGlobalString(args);
    if (name == "GetGlobalLocation") return hGetGlobalLocation(args);
    if (name == "SetGlobalLocation") return hSetGlobalLocation(args);
    if (name == "GetLocalNumber") return hGetLocalNumber(args);
    if (name == "SetLocalNumber") return hSetLocalNumber(args);
    if (name == "GetLocalBoolean") return hGetLocalBoolean(args);
    if (name == "SetLocalBoolean") return hSetLocalBoolean(args);
    if (name == "GetLocalString") return hGetLocalString(args);
    if (name == "SetLocalString") return hSetLocalString(args);
    if (name == "Random") return hRandom(args);
    if (name == "abs") return hAbs(args);
    if (name == "VectorNormalize") return hVectorNormalize(args);
    if (name == "fabs") return hFabs(args);
    if (name == "cos") return hCos(args);
    if (name == "sin") return hSin(args);
    if (name == "tan") return hTan(args);
    if (name == "acos") return hAcos(args);
    if (name == "asin") return hAsin(args);
    if (name == "atan") return hAtan(args);
    if (name == "log") return hLog(args);
    if (name == "pow") return hPow(args);
    if (name == "sqrt") return hSqrt(args);
    if (name == "VectorMagnitude") return hVectorMagnitude(args);
    if (name == "TurnsToSeconds") return hTurnsToSeconds(args);
    if (name == "AngleToVector") return hAngleToVector(args);
    if (name == "VectorToAngle") return hVectorToAngle(args);
    if (name == "FeetToMeters") return hFeetToMeters(args);
    if (name == "YardsToMeters") return hYardsToMeters(args);
    if (name == "InsertString") return hInsertString(args);
    if (name == "StringToFloat") return hStringToFloat(args);
    if (name == "IntToHexString") return hIntToHexString(args);
    if (name == "TestStringAgainstPattern") return hTestStringAgainstPattern(args);
    if (name == "RandomName") return hRandomName(args);
    if (name == "WriteTimestampedLogEntry") return hWriteTimestampedLogEntry(args);
    if (name == "MagicalEffect") return hEffect(112);
    if (name == "SupernaturalEffect") return hEffect(113);
    if (name == "ExtraordinaryEffect") return hEffect(114);
    if (name == "VersusAlignmentEffect") return hEffect(355);
    if (name == "VersusRacialTypeEffect") return hEffect(356);
    if (name == "VersusTrapEffect") return hEffect(357);
    if (name == "DeleteJournalWorldAllEntries") return hDeleteJournalWorldAllEntries();
    if (name == "DeleteJournalWorldEntry" || name == "DeleteJournalWorldEntryStrref") {
        return hDeleteJournalWorldEntry(args);
    }
    if (name == "PopUpGUIPanel") return hQueue(388, args);
    if (name == "PopUpDeathGUIPanel") return hQueue(554, args);
    if (name == "SuppressStatusSummaryEntry") return hQueue(763, args);
    if (name == "FloatingTextStrRefOnCreature") return hQueue(525, args);
    if (name == "FloatingTextStringOnCreature") return hQueue(526, args);
    if (name == "ExploreAreaForPlayer") return hQueue(403, args);
    if (name == "CutsceneMove") return hCutsceneMove(args);
    if (name == "DoPlaceableObjectAction") return hQueue(547, args);
    if (name == "ExportAllCharacters") return hQueue(557, args);
    if (name == "TouchAttackMelee" || name == "TouchAttackRanged") return hTouchAttack(args);
    if (name == "GetStringLength") return hGetStringLength(args);
    if (name == "GetStringLeft") return hGetStringLeft(args);
    if (name == "GetStringRight") return hGetStringRight(args);
    if (name == "GetSubString") return hGetSubString(args);
    if (name == "FindSubString") return hFindSubString(args);
    if (name == "StringToInt") return hStringToInt(args);
    if (name == "FloatToInt") return hFloatToInt(args);
    if (name == "IntToFloat") return hIntToFloat(args);
    if (name == "IntToString") return hIntToString(args);
    if (name == "FloatToString") return hFloatToString(args);
    if (name == "Vector") return hVector(args);
    if (name == "GetPosition") return hGetPosition(args);
    if (name == "GetDistanceToObject") return hGetDistanceToObject(args);
    if (name == "GetTime") return hGetTime();
    if (name == "ActionWait") return hActionWait(args, vm);
    if (name == "ActionPauseConversation") return hQueue(205, args);
    if (name == "DelayCommand") return hDelayCommand(args);
    if (name == "AssignCommand") return hAssignCommand(args);
    if (name == "SetListenPattern") return hSetListenPattern(args);
    return std::nullopt;
}

std::vector<Value> ActionRuntime::generic(const std::string& name, const Args& args, Vm& vm,
                                          std::uint16_t actionId)
{
    if (auto resolved = behaviors(name, args, actionId); resolved.has_value()) {
        generic_.insert(name);
        return *resolved;
    }
    if (startsWith(name, "SWMG_")) {
        return swmg(name, args, actionId);
    }
    if (startsWith(name, "Action")) {
        world_->queueAction(actionId, args, "Action");
        ++actionsQueued_;
        generic_.insert(name);
        return ret(actionId);
    }
    if (name.size() > 1 && name[0] == 'd' && allDigits(name.substr(1))) {
        const std::int64_t sides = std::strtoll(name.c_str() + 1, nullptr, 10);
        generic_.insert(name);
        return {Value::makeInt(world_->rng.randint(1, static_cast<std::int32_t>(std::max<std::int64_t>(1, sides))))};
    }
    if (startsWith(name, "Get") || startsWith(name, "Is") || startsWith(name, "Has")
        || startsWith(name, "Find")) {
        generic_.insert(name);
        return ret(actionId);
    }
    static const char* prefixes[] = {
        "Set", "Clear", "Stop", "Destroy", "Remove", "Apply", "Change", "Face", "Jump", "Move",
        "Delay", "Assign", "Play", "Sound", "Music", "Speak", "Effect", "Print", "Send", "Show",
        "Take", "Give", "SWMG_", "TUR_", "PAZ_", "Add", "Adjust", "Ambient", "Aur", "Award",
        "Bark", "Begin", "Cancel", "Create", "Disable", "End", "Event", "Execute", "Location",
        "NoClicks", "Object", "Queue", "Reveal", "Ship", "Signal", "Spawn", "Start", "Surrender",
        "Switch", "Talent", "Yavin",
    };
    for (const char* prefix : prefixes) {
        if (startsWith(name, prefix)) {
            generic_.insert(name);
            return ret(actionId);
        }
    }
    default_.insert(name);
    (void)vm;
    return ret(actionId);
}

std::vector<Value> ActionRuntime::swmg(const std::string& name, const Args& args,
                                       std::uint16_t actionId)
{
    // The minigame actions are state cells; they are not exercised by the
    // slice, but the corpus uses the set/get/adjust subset.
    static std::map<std::string, Value> store;
    if (startsWith(name, "SWMG_Set")) {
        if (!args.empty() && !args.back().empty()) {
            const std::vector<Value>& cells = args.back();
            if (cells.size() >= 3) {
                // Vectors are not represented in this small store; the corpus
                // uses scalars for set/get pairs.
                store[name.substr(8)] = Value::makeFloat(cells.front().asFloat());
            } else {
                store[name.substr(8)] = cells.back();
            }
        }
        generic_.insert(name);
        return ret(actionId);
    }
    if (startsWith(name, "SWMG_Get")) {
        const auto it = store.find(name.substr(8));
        generic_.insert(name);
        if (it == store.end()) {
            return ret(actionId);
        }
        return {it->second};
    }
    if (startsWith(name, "SWMG_Is")) {
        const auto it = store.find(name.substr(8));
        generic_.insert(name);
        return {Value::makeInt(it != store.end() && it->second.truthy() ? 1 : 0)};
    }
    if (startsWith(name, "SWMG_Adjust")) {
        const std::string key = name.substr(11);
        const std::size_t index = actionSpec(actionId).paramCount == 0
            ? 0
            : static_cast<std::size_t>(actionSpec(actionId).paramCount - 1);
        store[key] = Value::makeInt(store[key].asInt() + argInt(args, index));
        generic_.insert(name);
        return ret(actionId);
    }
    generic_.insert(name);
    return ret(actionId);
}

std::vector<Value> ActionRuntime::hGetTag(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    return {Value::makeString(object != nullptr ? object->tag : "")};
}

std::vector<Value> ActionRuntime::hGetObjectByTag(const Args& args)
{
    return {Value::makeObject(world_->byTag(argString(args, 0), argInt(args, 1)))};
}

std::vector<Value> ActionRuntime::hGetWaypointByTag(const Args& args)
{
    return {Value::makeObject(world_->byTag(argString(args, 0), 0))};
}

std::vector<Value> ActionRuntime::hGetArea(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    return {Value::makeObject(object != nullptr ? static_cast<std::uint32_t>(object->area)
                                                : kObjectInvalid)};
}

std::vector<Value> ActionRuntime::hGetFirstPC(const Args&)
{
    return {Value::makeObject(world_->pc)};
}

std::vector<Value> ActionRuntime::hGetPartyMemberByIndex(const Args& args)
{
    const std::int64_t index = argInt(args, 0);
    if (index >= 0 && index < static_cast<std::int64_t>(world_->party.size())) {
        return {Value::makeObject(world_->party[static_cast<std::size_t>(index)])};
    }
    return {Value::makeObject(kObjectInvalid)};
}

std::vector<Value> ActionRuntime::hGetIsObjectValid(const Args& args)
{
    return {Value::makeInt(world_->valid(argObject(args, 0)) ? 1 : 0)};
}

std::vector<Value> ActionRuntime::hGetNextObjectInArea(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    const std::int64_t area = object != nullptr ? object->area : 100;
    const std::string key = "area:" + std::to_string(area);
    const std::vector<std::uint32_t> ids = world_->byArea(area);
    const std::int64_t position = world_->cursor(key);
    if (position < 0 || position >= static_cast<std::int64_t>(ids.size())) {
        return {Value::makeObject(kObjectInvalid)};
    }
    world_->setCursor(key, position + 1);
    return {Value::makeObject(ids[static_cast<std::size_t>(position)])};
}

std::vector<Value> ActionRuntime::hGetNextItemInInventory(const Args& args)
{
    std::uint32_t objectId = argObject(args, 0);
    if (objectId == kObjectInvalid) {
        objectId = world_->pc;
    }
    const GameObject* object = world_->get(objectId);
    const std::string key = "inv:" + std::to_string(objectId);
    const std::int64_t position = world_->cursor(key);
    if (object == nullptr || position < 0
        || position >= static_cast<std::int64_t>(object->inventory.size())) {
        return {Value::makeObject(kObjectInvalid)};
    }
    world_->setCursor(key, position + 1);
    return {Value::makeObject(object->inventory[static_cast<std::size_t>(position)])};
}

std::vector<Value> ActionRuntime::hGetItemInSlot(const Args& args)
{
    const std::int64_t slot = argInt(args, 0);
    std::uint32_t objectId = argObject(args, 1);
    if (objectId == kObjectInvalid) {
        objectId = world_->pc;
    }
    const GameObject* object = world_->get(objectId);
    if (object != nullptr && slot >= 0
        && slot < static_cast<std::int64_t>(object->inventory.size())) {
        return {Value::makeObject(object->inventory[static_cast<std::size_t>(slot)])};
    }
    return {Value::makeObject(kObjectInvalid)};
}

std::vector<Value> ActionRuntime::hGetBaseItemType(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    return {Value::makeInt(object != nullptr ? object->baseItem : 0)};
}

std::vector<Value> ActionRuntime::hGetGold(const Args&)
{
    return {Value::makeInt(0)};
}

std::vector<Value> ActionRuntime::hGetGlobalNumber(const Args& args)
{
    return {world_->getGlobal(argString(args, 0), ValueType::Int)};
}

std::vector<Value> ActionRuntime::hSetGlobalNumber(const Args& args)
{
    world_->setGlobal(argString(args, 0), Value::makeInt(argInt(args, 1)));
    return {};
}

std::vector<Value> ActionRuntime::hGetGlobalBoolean(const Args& args)
{
    return {world_->getGlobal(argString(args, 0), ValueType::Int)};
}

std::vector<Value> ActionRuntime::hSetGlobalBoolean(const Args& args)
{
    world_->setGlobal(argString(args, 0), Value::makeInt(argInt(args, 1) ? 1 : 0));
    return {};
}

std::vector<Value> ActionRuntime::hGetGlobalString(const Args& args)
{
    return {world_->getGlobal(argString(args, 0), ValueType::String)};
}

std::vector<Value> ActionRuntime::hSetGlobalString(const Args& args)
{
    world_->setGlobal(argString(args, 0), Value::makeString(argString(args, 1)));
    return {};
}

std::vector<Value> ActionRuntime::hGetGlobalLocation(const Args&)
{
    return {Value::makeEngine(0)};
}

std::vector<Value> ActionRuntime::hSetGlobalLocation(const Args&)
{
    return {};
}

std::vector<Value> ActionRuntime::hGetLocalNumber(const Args& args)
{
    return {world_->getLocal(argObject(args, 0), localKey(args, 1), ValueType::Int)};
}

std::vector<Value> ActionRuntime::hSetLocalNumber(const Args& args)
{
    world_->setLocal(argObject(args, 0), localKey(args, 1), Value::makeInt(argInt(args, 2)));
    return {};
}

std::vector<Value> ActionRuntime::hGetLocalBoolean(const Args& args)
{
    return {world_->getLocal(argObject(args, 0), localKey(args, 1), ValueType::Int)};
}

std::vector<Value> ActionRuntime::hSetLocalBoolean(const Args& args)
{
    world_->setLocal(argObject(args, 0), localKey(args, 1), Value::makeInt(argInt(args, 2) ? 1 : 0));
    return {};
}

std::vector<Value> ActionRuntime::hGetLocalString(const Args& args)
{
    return {world_->getLocal(argObject(args, 0), localKey(args, 1), ValueType::String)};
}

std::vector<Value> ActionRuntime::hSetLocalString(const Args& args)
{
    world_->setLocal(argObject(args, 0), localKey(args, 1), Value::makeString(argString(args, 2)));
    return {};
}

std::vector<Value> ActionRuntime::hRandom(const Args& args)
{
    const std::int64_t limit = std::max<std::int64_t>(1, argInt(args, 0));
    return {Value::makeInt(world_->rng.randrange(static_cast<std::uint32_t>(limit)))};
}

std::vector<Value> ActionRuntime::hAbs(const Args& args)
{
    return {Value::makeInt(absInt(argInt(args, 0)))};
}

std::vector<Value> ActionRuntime::hVectorNormalize(const Args& args)
{
    const std::vector<Value> v = argVector(args, 0);
    const double x = v[0].asFloat();
    const double y = v[1].asFloat();
    const double z = v[2].asFloat();
    const double magnitude = std::sqrt(x * x + y * y + z * z);
    if (magnitude == 0.0) {
        return {Value::makeFloat(0.0), Value::makeFloat(0.0), Value::makeFloat(0.0)};
    }
    return {Value::makeFloat(x / magnitude), Value::makeFloat(y / magnitude),
            Value::makeFloat(z / magnitude)};
}

std::vector<Value> ActionRuntime::hFabs(const Args& args)
{
    return {Value::makeFloat(std::fabs(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hCos(const Args& args)
{
    return {Value::makeFloat(std::cos(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hSin(const Args& args)
{
    return {Value::makeFloat(std::sin(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hTan(const Args& args)
{
    return {Value::makeFloat(std::tan(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hAcos(const Args& args)
{
    const double value = argFloat(args, 0);
    return {Value::makeFloat(value < -1.0 || value > 1.0 ? 0.0 : std::acos(value))};
}

std::vector<Value> ActionRuntime::hAsin(const Args& args)
{
    const double value = argFloat(args, 0);
    return {Value::makeFloat(value < -1.0 || value > 1.0 ? 0.0 : std::asin(value))};
}

std::vector<Value> ActionRuntime::hAtan(const Args& args)
{
    return {Value::makeFloat(std::atan(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hLog(const Args& args)
{
    const double value = argFloat(args, 0);
    return {Value::makeFloat(value <= 0.0 ? 0.0 : std::log(value))};
}

std::vector<Value> ActionRuntime::hPow(const Args& args)
{
    return {Value::makeFloat(std::pow(argFloat(args, 0), argFloat(args, 1)))};
}

std::vector<Value> ActionRuntime::hSqrt(const Args& args)
{
    const double value = argFloat(args, 0);
    return {Value::makeFloat(value < 0.0 ? 0.0 : std::sqrt(value))};
}

std::vector<Value> ActionRuntime::hVectorMagnitude(const Args& args)
{
    const std::vector<Value> v = argVector(args, 0);
    const double x = v[0].asFloat();
    const double y = v[1].asFloat();
    const double z = v[2].asFloat();
    return {Value::makeFloat(std::sqrt(x * x + y * y + z * z))};
}

std::vector<Value> ActionRuntime::hTurnsToSeconds(const Args& args)
{
    return {Value::makeFloat(static_cast<double>(argInt(args, 0)) * 6.0)};
}

std::vector<Value> ActionRuntime::hAngleToVector(const Args& args)
{
    const double angle = argFloat(args, 0) * kPi / 180.0;
    return {Value::makeFloat(std::cos(angle)), Value::makeFloat(std::sin(angle)),
            Value::makeFloat(0.0)};
}

std::vector<Value> ActionRuntime::hVectorToAngle(const Args& args)
{
    const std::vector<Value> v = argVector(args, 0);
    return {Value::makeFloat(std::atan2(v[1].asFloat(), v[0].asFloat()) * 180.0 / kPi)};
}

std::vector<Value> ActionRuntime::hFeetToMeters(const Args& args)
{
    return {Value::makeFloat(argFloat(args, 0) * 0.3048)};
}

std::vector<Value> ActionRuntime::hYardsToMeters(const Args& args)
{
    return {Value::makeFloat(argFloat(args, 0) * 0.9144)};
}

std::vector<Value> ActionRuntime::hInsertString(const Args& args)
{
    const std::string destination = argString(args, 0);
    const std::string text = argString(args, 1);
    const std::int64_t raw = argInt(args, 2);
    const std::size_t position = raw < 0 ? 0 : std::min<std::size_t>(destination.size(),
                                                                    static_cast<std::size_t>(raw));
    return {Value::makeString(destination.substr(0, position) + text + destination.substr(position))};
}

std::vector<Value> ActionRuntime::hStringToFloat(const Args& args)
{
    const std::string text = argString(args, 0);
    if (text.empty()) {
        return {Value::makeFloat(0.0)};
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return {Value::makeFloat(0.0)};
    }
    return {Value::makeFloat(value)};
}

std::vector<Value> ActionRuntime::hIntToHexString(const Args& args)
{
    std::ostringstream stream;
    stream << std::hex << (argInt(args, 0) & 0xFFFFFFFFLL);
    return {Value::makeString(stream.str())};
}

std::vector<Value> ActionRuntime::hTestStringAgainstPattern(const Args& args)
{
    const std::string pattern = argString(args, 0);
    const std::string text = argString(args, 1);
    std::string regex = "^";
    for (char ch : pattern) {
        if (ch == '*') {
            regex += ".*";
        } else if (ch == '?') {
            regex += ".";
        } else if (std::string(".^$|()[]{}*+?\\").find(ch) != std::string::npos) {
            regex += '\\';
            regex += ch;
        } else {
            regex += ch;
        }
    }
    regex += "$";
    try {
        return {Value::makeInt(std::regex_match(text, std::regex(regex)) ? 1 : 0)};
    } catch (const std::regex_error&) {
        return {Value::makeInt(0)};
    }
}

std::vector<Value> ActionRuntime::hRandomName(const Args&)
{
    static const char* names[] = {"Carth", "Bastila", "Mission", "Zaalbar", "Jolee",
                                  "Juhani", "Canderous", "T3-M4", "HK-47"};
    const std::uint32_t index = world_->rng.randrange(9);
    return {Value::makeString(names[index])};
}

std::vector<Value> ActionRuntime::hWriteTimestampedLogEntry(const Args& args)
{
    world_->log.emplace_back(world_->time, argString(args, 0));
    return {};
}

std::vector<Value> ActionRuntime::hEffect(std::int64_t tag)
{
    return {Value::makeEngine(tag)};
}

std::vector<Value> ActionRuntime::hDeleteJournalWorldAllEntries()
{
    world_->journal.clear();
    return {};
}

std::vector<Value> ActionRuntime::hDeleteJournalWorldEntry(const Args& args)
{
    world_->journal.erase(argInt(args, 0));
    return {};
}

std::vector<Value> ActionRuntime::hQueue(std::int64_t actionId, const Args& args)
{
    world_->queueAction(actionId, args, "QueuedUI");
    ++actionsQueued_;
    return {};
}

std::vector<Value> ActionRuntime::hCutsceneMove(const Args& args)
{
    GameObject* object = world_->get(argObject(args, 0));
    if (object != nullptr) {
        const std::vector<Value> position = argVector(args, 1);
        object->position = {position[0].asFloat(), position[1].asFloat(), position[2].asFloat()};
    }
    return hQueue(507, args);
}

std::vector<Value> ActionRuntime::hTouchAttack(const Args& args)
{
    return {Value::makeInt(world_->valid(argObject(args, 0)) ? 1 : 0)};
}

std::vector<Value> ActionRuntime::hGetStringLength(const Args& args)
{
    return {Value::makeInt(static_cast<std::int64_t>(argString(args, 0).size()))};
}

std::vector<Value> ActionRuntime::hGetStringLeft(const Args& args)
{
    const std::string text = argString(args, 0);
    const std::int64_t count = std::max<std::int64_t>(0, argInt(args, 1));
    return {Value::makeString(text.substr(0, static_cast<std::size_t>(count)))};
}

std::vector<Value> ActionRuntime::hGetStringRight(const Args& args)
{
    const std::string text = argString(args, 0);
    const std::int64_t count = std::max<std::int64_t>(0, argInt(args, 1));
    if (count == 0) {
        return {Value::makeString("")};
    }
    const std::size_t n = static_cast<std::size_t>(count);
    return {Value::makeString(n >= text.size() ? text : text.substr(text.size() - n))};
}

std::vector<Value> ActionRuntime::hGetSubString(const Args& args)
{
    const std::string text = argString(args, 0);
    const std::int64_t start = std::max<std::int64_t>(0, argInt(args, 1));
    const std::int64_t count = std::max<std::int64_t>(0, argInt(args, 2));
    return {Value::makeString(text.substr(std::min<std::size_t>(text.size(),
                                                              static_cast<std::size_t>(start)),
                                         static_cast<std::size_t>(count)))};
}

std::vector<Value> ActionRuntime::hFindSubString(const Args& args)
{
    return {Value::makeInt(static_cast<std::int64_t>(
        argString(args, 0).find(argString(args, 1))))};
}

std::vector<Value> ActionRuntime::hStringToInt(const Args& args)
{
    const std::string text = argString(args, 0);
    if (text.empty()) {
        return {Value::makeInt(0)};
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return {Value::makeInt(0)};
    }
    return {Value::makeInt(value)};
}

std::vector<Value> ActionRuntime::hFloatToInt(const Args& args)
{
    return {Value::makeInt(static_cast<std::int64_t>(argFloat(args, 0)))};
}

std::vector<Value> ActionRuntime::hIntToFloat(const Args& args)
{
    return {Value::makeFloat(static_cast<double>(argInt(args, 0)))};
}

std::vector<Value> ActionRuntime::hIntToString(const Args& args)
{
    return {Value::makeString(std::to_string(argInt(args, 0)))};
}

std::vector<Value> ActionRuntime::hFloatToString(const Args& args)
{
    const std::int64_t decimals = std::max<std::int64_t>(0, argInt(args, 2));
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(static_cast<std::streamsize>(decimals));
    stream << argFloat(args, 0);
    return {Value::makeString(stream.str())};
}

std::vector<Value> ActionRuntime::hVector(const Args& args)
{
    return {Value::makeFloat(argFloat(args, 0)), Value::makeFloat(argFloat(args, 1)),
            Value::makeFloat(argFloat(args, 2))};
}

std::vector<Value> ActionRuntime::hGetPosition(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    const std::array<double, 3> position = object != nullptr ? object->position
                                                             : std::array<double, 3>{0.0, 0.0, 0.0};
    return {Value::makeFloat(position[0]), Value::makeFloat(position[1]),
            Value::makeFloat(position[2])};
}

std::vector<Value> ActionRuntime::hGetDistanceToObject(const Args& args)
{
    const GameObject* object = world_->get(argObject(args, 0));
    if (object == nullptr) {
        return {Value::makeFloat(0.0)};
    }
    const GameObject* pc = world_->get(world_->pc);
    const std::array<double, 3> origin = pc != nullptr ? pc->position
                                                       : std::array<double, 3>{0.0, 0.0, 0.0};
    const double dx = object->position[0] - origin[0];
    const double dy = object->position[1] - origin[1];
    const double dz = object->position[2] - origin[2];
    return {Value::makeFloat(std::sqrt(dx * dx + dy * dy + dz * dz))};
}

std::vector<Value> ActionRuntime::hGetTime()
{
    return {Value::makeFloat(world_->time)};
}

std::vector<Value> ActionRuntime::hActionWait(const Args& args, Vm& vm)
{
    if (continuations_) {
        Functor functor = vm.captureContinuation("ActionWait");
        const std::int64_t id = registerFunctor(std::move(functor));
        world_->scheduleFunctor(argFloat(args, 0), id);
    }
    vm.block(202);
    return {};
}

std::int64_t ActionRuntime::registerFunctor(Functor functor)
{
    functor.context = context_;
    return world_->addFunctor(std::move(functor));
}

std::vector<Value> ActionRuntime::hDelayCommand(const Args& args)
{
    const Value value = cell(args, 1, ParamKind::Action);
    if (value.type == ValueType::Functor && value.integer >= 0) {
        if (const auto functor = world_->functor(value.integer)) {
            functor->source = "DelayCommand";
        }
        world_->scheduleFunctor(argFloat(args, 0), value.integer);
        ++timersScheduled_;
        return {};
    }
    world_->schedule(argFloat(args, 0), -1, args, "DelayCommand");
    ++timersScheduled_;
    return {};
}

std::vector<Value> ActionRuntime::hAssignCommand(const Args& args)
{
    const Value value = cell(args, 1, ParamKind::Action);
    if (value.type == ValueType::Functor && value.integer >= 0) {
        if (const auto functor = world_->functor(value.integer)) {
            functor->source = "AssignCommand";
        }
        world_->queueFunctor(value.integer, argObject(args, 0));
        ++actionsQueued_;
        return {};
    }
    world_->queueAction(-1, args, "AssignCommand");
    ++actionsQueued_;
    return {};
}

std::vector<Value> ActionRuntime::hSetListenPattern(const Args& args)
{
    return hQueue(176, args);
}

RuntimeSummary ActionRuntime::summary() const
{
    RuntimeSummary out;
    out.calls = 0;
    for (const auto& [id, count] : calls_) {
        out.calls += count;
    }
    out.uniqueActions = calls_.size();
    out.specific.assign(specific_.begin(), specific_.end());
    out.generic.assign(generic_.begin(), generic_.end());
    out.defaulted.assign(default_.begin(), default_.end());
    out.timers = world_->timers.size();
    out.queuedActions = world_->actionQueue.size();
    out.world = world_->summary();
    return out;
}

// ---- behaviors -------------------------------------------------------------

std::vector<Value> ActionRuntime::behaviorField(const std::string& name, const Args& args,
                                               std::uint16_t actionId)
{
    if (name == "GetJournalQuestExperience") {
        const std::string plot = argString(args, 0);
        const auto it = world_->attrs.find("journal_xp");
        if (it != world_->attrs.end()) {
            if (const auto* values = std::get_if<std::map<std::string, std::int64_t>>(&it->second)) {
                const auto found = values->find(plot);
                if (found != values->end()) {
                    return {Value::makeInt(found->second)};
                }
            }
        }
        return {Value::makeInt(0)};
    }
    const auto specIt = fieldSpecs().find(name);
    if (specIt == fieldSpecs().end()) {
        return ret(actionId);
    }
    const FieldSpec& spec = specIt->second;
    if (worldOnly().contains(name)) {
        if (name == "GetStartingLocation" || name == "GetItemActivatedTargetLocation") {
            return {Value::makeEngine(0)};
        }
        switch (spec.type) {
        case ValueType::String: return {Value::makeString(world_->attrString(spec.attr, spec.text))};
        case ValueType::Float: return {Value::makeFloat(world_->attrFloat(spec.attr, spec.real))};
        default: return {Value::makeInt(world_->attrInt(spec.attr, spec.integer))};
        }
    }
    if (name == "GetModuleName") {
        const std::string module = host_ != nullptr ? host_->moduleName()
                                                    : world_->attrString("module", "end_m01aa");
        return {Value::makeString(module)};
    }
    if (worldAttrs().contains(name)) {
        switch (spec.type) {
        case ValueType::String: return {Value::makeString(world_->attrString(spec.attr, spec.text))};
        case ValueType::Float: return {Value::makeFloat(world_->attrFloat(spec.attr, spec.real))};
        default: return {Value::makeInt(world_->attrInt(spec.attr, spec.integer))};
        }
    }
    const GameObject* object = world_->get(firstObjectArg(args, actionId));
    if (object == nullptr) {
        switch (spec.type) {
        case ValueType::String: return {Value::makeString(spec.text)};
        case ValueType::Float: return {Value::makeFloat(spec.real)};
        default: return {Value::makeInt(spec.integer)};
        }
    }
    const auto it = object->attrs.find(spec.attr);
    if (it == object->attrs.end()) {
        switch (spec.type) {
        case ValueType::String: return {Value::makeString(spec.text)};
        case ValueType::Float: return {Value::makeFloat(spec.real)};
        default: return {Value::makeInt(spec.integer)};
        }
    }
    if (std::holds_alternative<std::int64_t>(it->second)) {
        const std::int64_t value = std::get<std::int64_t>(it->second);
        return fieldValue(spec.type, value, static_cast<double>(value), std::to_string(value));
    }
    if (std::holds_alternative<double>(it->second)) {
        const double value = std::get<double>(it->second);
        return fieldValue(spec.type, static_cast<std::int64_t>(value), value, "");
    }
    if (std::holds_alternative<std::string>(it->second)) {
        return {Value::makeString(std::get<std::string>(it->second))};
    }
    if (std::holds_alternative<bool>(it->second)) {
        return {Value::makeInt(std::get<bool>(it->second) ? 1 : 0)};
    }
    return ret(actionId);
}

std::optional<std::vector<Value>> ActionRuntime::behaviorFaction(const std::string& name, const Args&)
{
    std::vector<const GameObject*> party;
    for (const std::uint32_t id : world_->party) {
        const GameObject* object = world_->get(id);
        if (object != nullptr && object->valid) {
            party.push_back(object);
        }
    }
    if (party.empty()) {
        if (name == "GetFactionLeader" || name == "GetFirstFactionMember") {
            return std::vector<Value>{Value::makeObject(kObjectInvalid)};
        }
        return std::vector<Value>{Value::makeInt(0)};
    }
    if (name == "GetFactionWeakestMember" || name == "GetFactionMostDamagedMember") {
        const GameObject* best = *std::min_element(party.begin(), party.end(),
                                                   [](const GameObject* a, const GameObject* b) {
                                                       return a->hp < b->hp;
                                                   });
        return std::vector<Value>{Value::makeObject(best->id)};
    }
    if (name == "GetFactionStrongestMember" || name == "GetFactionLeastDamagedMember") {
        const GameObject* best = *std::max_element(party.begin(), party.end(),
                                                   [](const GameObject* a, const GameObject* b) {
                                                       return a->hp < b->hp;
                                                   });
        return std::vector<Value>{Value::makeObject(best->id)};
    }
    if (name == "GetFactionGold") {
        std::int64_t gold = 0;
        for (const GameObject* object : party) {
            const auto it = object->attrs.find("gold");
            if (it != object->attrs.end() && std::holds_alternative<std::int64_t>(it->second)) {
                gold += std::get<std::int64_t>(it->second);
            }
        }
        return std::vector<Value>{Value::makeInt(gold)};
    }
    if (name == "GetFactionAverageGoodEvilAlignment" || name == "GetFactionAverageLevel"
        || name == "GetFactionAverageXP") {
        const char* key = name == "GetFactionAverageGoodEvilAlignment" ? "good_evil"
            : name == "GetFactionAverageLevel"                       ? "level"
                                                                     : "xp";
        const std::int64_t fallback = name == "GetFactionAverageGoodEvilAlignment" ? 50 : 1;
        std::int64_t sum = 0;
        for (const GameObject* object : party) {
            const auto it = object->attrs.find(key);
            sum += it != object->attrs.end() && std::holds_alternative<std::int64_t>(it->second)
                ? std::get<std::int64_t>(it->second)
                : (name == "GetFactionAverageXP" ? 0 : fallback);
        }
        return std::vector<Value>{Value::makeInt(sum / static_cast<std::int64_t>(party.size()))};
    }
    if (name == "GetFactionMostFrequentClass") {
        std::map<std::int64_t, std::int64_t> counts;
        for (const GameObject* object : party) {
            const auto it = object->attrs.find("class_id");
            const std::int64_t value = it != object->attrs.end()
                    && std::holds_alternative<std::int64_t>(it->second)
                ? std::get<std::int64_t>(it->second)
                : 0;
            ++counts[value];
        }
        std::int64_t best = 0;
        std::int64_t bestCount = -1;
        for (const auto& [value, count] : counts) {
            if (count > bestCount) {
                best = value;
                bestCount = count;
            }
        }
        return std::vector<Value>{Value::makeInt(best)};
    }
    if (name == "GetFactionWorstAC" || name == "GetFactionBestAC") {
        std::int64_t best = name == "GetFactionWorstAC" ? 0x7FFFFFFF : -0x7FFFFFFF;
        for (const GameObject* object : party) {
            const auto it = object->attrs.find("ac");
            const std::int64_t value = it != object->attrs.end()
                    && std::holds_alternative<std::int64_t>(it->second)
                ? std::get<std::int64_t>(it->second)
                : 10;
            best = name == "GetFactionWorstAC" ? std::min(best, value) : std::max(best, value);
        }
        return std::vector<Value>{Value::makeInt(best)};
    }
    if (name == "GetFactionLeader") {
        return std::vector<Value>{Value::makeObject(party.front()->id)};
    }
    if (name == "GetFirstFactionMember") {
        world_->setCursor("faction", 0);
        return std::vector<Value>{Value::makeObject(party.front()->id)};
    }
    if (name == "GetNextFactionMember") {
        const std::int64_t position = world_->cursor("faction");
        world_->setCursor("faction", position + 1);
        return std::vector<Value>{Value::makeObject(position >= 0 && position < static_cast<std::int64_t>(party.size())
                                      ? party[static_cast<std::size_t>(position)]->id
                                      : kObjectInvalid)};
    }
    return std::nullopt;
}

std::optional<std::vector<Value>> ActionRuntime::behaviorSpecial(const std::string& name, const Args& args,
                                                  std::uint16_t actionId)
{
    if (name == "GetStringUpperCase") {
        std::string text = argString(args, 0);
        for (char& ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return std::vector<Value>{Value::makeString(text)};
    }
    if (name == "GetStringLowerCase") {
        return std::vector<Value>{Value::makeString(lowerAscii(argString(args, 0)))};
    }
    if (name == "GetIsDay" || name == "GetIsNight" || name == "GetIsDawn" || name == "GetIsDusk") {
        const double hour = std::fmod(world_->time, 24.0);
        bool value = false;
        if (name == "GetIsDay") {
            value = hour >= 6.0 && hour < 18.0;
        } else if (name == "GetIsNight") {
            value = hour < 6.0 || hour >= 18.0;
        } else if (name == "GetIsDawn") {
            value = hour >= 5.0 && hour < 7.0;
        } else {
            value = hour >= 17.0 && hour < 19.0;
        }
        return std::vector<Value>{Value::makeInt(value ? 1 : 0)};
    }
    if (name == "GetPlanetSelectable" || name == "GetPlanetAvailable") {
        return std::vector<Value>{Value::makeInt(0)};
    }
    if (name == "GetDistanceBetweenLocations" || name == "GetDistanceBetweenLocations2D") {
        const std::vector<Value> a = argVector(args, 0);
        const std::vector<Value> b = argVector(args, 1);
        const double dx = a[0].asFloat() - b[0].asFloat();
        const double dy = a[1].asFloat() - b[1].asFloat();
        const double dz = name.ends_with("2D") ? 0.0 : a[2].asFloat() - b[2].asFloat();
        return std::vector<Value>{Value::makeFloat(std::sqrt(dx * dx + dy * dy + dz * dz))};
    }
    if (name == "GetFacingFromLocation") {
        return std::vector<Value>{Value::makeFloat(0.0)};
    }
    if (name == "GetNearestCreatureToLocation" || name == "GetNearestObjectToLocation") {
        const std::size_t locationIndex = name == "GetNearestCreatureToLocation" ? 2 : 1;
        const std::vector<Value> target = argVector(args, locationIndex);
        const GameObject* best = nullptr;
        double bestDistance = 1.0e300;
        for (const auto& [id, object] : world_->objects) {
            if (!object.valid) {
                continue;
            }
            const double dx = object.position[0] - target[0].asFloat();
            const double dy = object.position[1] - target[1].asFloat();
            const double dz = object.position[2] - target[2].asFloat();
            const double distance = dx * dx + dy * dy + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = &object;
            }
        }
        return std::vector<Value>{Value::makeObject(best != nullptr ? best->id : kObjectInvalid)};
    }
    if (name == "GetNearestTrapToObject") {
        for (const auto& [id, object] : world_->objects) {
            const auto it = object.attrs.find("trapped");
            if (object.valid && it != object.attrs.end() && std::holds_alternative<std::int64_t>(it->second)
                && std::get<std::int64_t>(it->second) != 0) {
                return std::vector<Value>{Value::makeObject(object.id)};
            }
        }
        return std::vector<Value>{Value::makeObject(kObjectInvalid)};
    }
    if (name == "GetEffectDurationType" || name == "GetEffectSubType") {
        return std::vector<Value>{Value::makeInt(0)};
    }
    if (name == "GetEffectCreator") {
        return std::vector<Value>{Value::makeObject(kObjectInvalid)};
    }
    if (name == "GetGoldPieceValue") {
        return behaviorField(name, args, actionId);
    }
    if (name == "GetFirstAttacker" || name == "GetNextAttacker") {
        std::vector<std::uint32_t> attackers;
        for (const auto& [id, object] : world_->objects) {
            const auto it = object.attrs.find("attacker");
            if (it != object.attrs.end() && std::holds_alternative<std::int64_t>(it->second)
                && std::get<std::int64_t>(it->second) != 0) {
                attackers.push_back(object.id);
            }
        }
        if (name == "GetFirstAttacker") {
            world_->setCursor("attackers", 0);
            return std::vector<Value>{Value::makeObject(attackers.empty() ? kObjectInvalid : attackers.front())};
        }
        const std::int64_t position = world_->cursor("attackers");
        world_->setCursor("attackers", position + 1);
        return std::vector<Value>{Value::makeObject(position >= 0 && position < static_cast<std::int64_t>(attackers.size())
                                      ? attackers[static_cast<std::size_t>(position)]
                                      : kObjectInvalid)};
    }
    return behaviorFaction(name, args);
}

std::optional<std::vector<Value>> ActionRuntime::behaviors(const std::string& name,
                                                           const Args& args,
                                                           std::uint16_t actionId)
{
    if (fieldSpecs().contains(name)) {
        return behaviorField(name, args, actionId);
    }
    return behaviorSpecial(name, args, actionId);
}

} // namespace ke::script
