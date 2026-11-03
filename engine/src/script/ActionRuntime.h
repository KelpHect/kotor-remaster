#pragma once

#include "script/ActionTable.h"
#include "script/Vm.h"
#include "script/World.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke::script {

// Optional bridge from script actions to live engine state. Returning a value
// means the host handled the action (including its return cells); returning
// nullopt lets the deterministic runtime handle or mediate it.
class ActionHost {
public:
    virtual ~ActionHost() = default;
    virtual std::optional<std::vector<Value>> action(
        std::uint16_t actionId, const std::vector<std::vector<Value>>& args, World& world, Vm& vm)
    {
        (void)actionId;
        (void)args;
        (void)world;
        (void)vm;
        return std::nullopt;
    }
    virtual std::string moduleName() const { return "end_m01aa"; }
};

struct RuntimeSummary {
    std::uint64_t calls = 0;
    std::size_t uniqueActions = 0;
    std::vector<std::string> specific;
    std::vector<std::string> generic;
    std::vector<std::string> defaulted;
    std::size_t timers = 0;
    std::size_t queuedActions = 0;
    std::map<std::string, double> world;
};

// Stateful action runtime for the 772 NWScript actions. The 344 ids used by
// the shipped corpus have functional handlers; every other id resolves to a
// typed default or a mediated effect. No original executable is involved.
class ActionRuntime : public ActionSink {
public:
    using Args = std::vector<std::vector<Value>>;

    explicit ActionRuntime(World* world = nullptr, ActionHost* host = nullptr);
    ~ActionRuntime() override;
    ActionRuntime(const ActionRuntime&) = delete;
    ActionRuntime& operator=(const ActionRuntime&) = delete;
    ActionRuntime(ActionRuntime&&) = delete;
    ActionRuntime& operator=(ActionRuntime&&) = delete;

    std::vector<Value> call(std::uint16_t actionId, const Args& args, Vm& vm) override;

    World& world() { return *world_; }
    const World& world() const { return *world_; }
    void setHost(ActionHost* host) { host_ = host; }
    ActionHost* host() const { return host_; }
    void setTraceEvents(bool enabled) { traceEvents_ = enabled; }

    RuntimeSummary summary() const;
    std::uint64_t timerCount() const { return timersScheduled_; }
    std::uint64_t actionQueueCount() const { return actionsQueued_; }

    // Used by the engine's script bridge for event arguments.
    void setContextObject(const std::string& name, std::uint32_t id) { context_[name] = id; }
    std::uint32_t contextObject(const std::string& name) const;

private:
    Value cell(const Args& args, std::size_t index, ParamKind kind = ParamKind::Int) const;
    std::int64_t argInt(const Args& args, std::size_t index) const;
    double argFloat(const Args& args, std::size_t index) const;
    std::string argString(const Args& args, std::size_t index) const;
    std::uint32_t argObject(const Args& args, std::size_t index) const;
    std::vector<Value> argVector(const Args& args, std::size_t index) const;
    std::vector<Value> ret(std::uint16_t actionId) const;
    std::string localKey(const Args& args, std::size_t index) const;

    std::optional<std::vector<Value>> specific(const std::string& name, const Args& args, Vm& vm,
                                               std::uint16_t actionId);
    std::vector<Value> generic(const std::string& name, const Args& args, Vm& vm,
                               std::uint16_t actionId);
    std::vector<Value> swmg(const std::string& name, const Args& args, std::uint16_t actionId);
    std::optional<std::vector<Value>> behaviors(const std::string& name, const Args& args,
                                                std::uint16_t actionId);
    std::vector<Value> behaviorField(const std::string& name, const Args& args,
                                     std::uint16_t actionId);
    std::optional<std::vector<Value>> behaviorFaction(const std::string& name, const Args& args);
    std::optional<std::vector<Value>> behaviorSpecial(const std::string& name, const Args& args,
                                                      std::uint16_t actionId);
    std::uint32_t firstObjectArg(const Args& args, std::uint16_t actionId) const;
    std::vector<Value> fieldValue(ValueType type, std::int64_t integer, double real,
                                  const std::string& text) const;

    // Specific handlers (ported one-for-one from the verified Python model).
    std::vector<Value> hGetTag(const Args&);
    std::vector<Value> hGetObjectByTag(const Args&);
    std::vector<Value> hGetWaypointByTag(const Args&);
    std::vector<Value> hGetArea(const Args&);
    std::vector<Value> hGetFirstPC(const Args&);
    std::vector<Value> hGetPartyMemberByIndex(const Args&);
    std::vector<Value> hGetIsObjectValid(const Args&);
    std::vector<Value> hGetNextObjectInArea(const Args&);
    std::vector<Value> hGetNextItemInInventory(const Args&);
    std::vector<Value> hGetItemInSlot(const Args&);
    std::vector<Value> hGetBaseItemType(const Args&);
    std::vector<Value> hGetGold(const Args&);
    std::vector<Value> hGetGlobalNumber(const Args&);
    std::vector<Value> hSetGlobalNumber(const Args&);
    std::vector<Value> hGetGlobalBoolean(const Args&);
    std::vector<Value> hSetGlobalBoolean(const Args&);
    std::vector<Value> hGetGlobalString(const Args&);
    std::vector<Value> hSetGlobalString(const Args&);
    std::vector<Value> hGetGlobalLocation(const Args&);
    std::vector<Value> hSetGlobalLocation(const Args&);
    std::vector<Value> hGetLocalNumber(const Args&);
    std::vector<Value> hSetLocalNumber(const Args&);
    std::vector<Value> hGetLocalBoolean(const Args&);
    std::vector<Value> hSetLocalBoolean(const Args&);
    std::vector<Value> hGetLocalString(const Args&);
    std::vector<Value> hSetLocalString(const Args&);
    std::vector<Value> hRandom(const Args&);
    std::vector<Value> hAbs(const Args&);
    std::vector<Value> hVectorNormalize(const Args&);
    std::vector<Value> hFabs(const Args&);
    std::vector<Value> hCos(const Args&);
    std::vector<Value> hSin(const Args&);
    std::vector<Value> hTan(const Args&);
    std::vector<Value> hAcos(const Args&);
    std::vector<Value> hAsin(const Args&);
    std::vector<Value> hAtan(const Args&);
    std::vector<Value> hLog(const Args&);
    std::vector<Value> hPow(const Args&);
    std::vector<Value> hSqrt(const Args&);
    std::vector<Value> hVectorMagnitude(const Args&);
    std::vector<Value> hTurnsToSeconds(const Args&);
    std::vector<Value> hAngleToVector(const Args&);
    std::vector<Value> hVectorToAngle(const Args&);
    std::vector<Value> hFeetToMeters(const Args&);
    std::vector<Value> hYardsToMeters(const Args&);
    std::vector<Value> hInsertString(const Args&);
    std::vector<Value> hStringToFloat(const Args&);
    std::vector<Value> hIntToHexString(const Args&);
    std::vector<Value> hTestStringAgainstPattern(const Args&);
    std::vector<Value> hRandomName(const Args&);
    std::vector<Value> hWriteTimestampedLogEntry(const Args&);
    std::vector<Value> hEffect(std::int64_t tag);
    std::vector<Value> hDeleteJournalWorldAllEntries();
    std::vector<Value> hDeleteJournalWorldEntry(const Args&);
    std::vector<Value> hQueue(std::int64_t actionId, const Args& args);
    std::vector<Value> hCutsceneMove(const Args&);
    std::vector<Value> hTouchAttack(const Args&);
    std::vector<Value> hGetStringLength(const Args&);
    std::vector<Value> hGetStringLeft(const Args&);
    std::vector<Value> hGetStringRight(const Args&);
    std::vector<Value> hGetSubString(const Args&);
    std::vector<Value> hFindSubString(const Args&);
    std::vector<Value> hStringToInt(const Args&);
    std::vector<Value> hFloatToInt(const Args&);
    std::vector<Value> hIntToFloat(const Args&);
    std::vector<Value> hIntToString(const Args&);
    std::vector<Value> hFloatToString(const Args&);
    std::vector<Value> hVector(const Args&);
    std::vector<Value> hGetPosition(const Args&);
    std::vector<Value> hGetDistanceToObject(const Args&);
    std::vector<Value> hGetTime();
    std::vector<Value> hActionWait(Vm&);
    std::vector<Value> hDelayCommand(const Args&);
    std::vector<Value> hAssignCommand(const Args&);
    std::vector<Value> hSetListenPattern(const Args&);

    World* ownedWorld_ = nullptr;
    World* world_ = nullptr;
    ActionHost* host_ = nullptr;
    bool traceEvents_ = false;
    std::map<std::uint16_t, std::uint32_t> calls_;
    std::set<std::string> specific_;
    std::set<std::string> generic_;
    std::set<std::string> default_;
    std::uint64_t timersScheduled_ = 0;
    std::uint64_t actionsQueued_ = 0;
    std::map<std::string, std::uint32_t> context_;
};

} // namespace ke::script
