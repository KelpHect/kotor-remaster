#pragma once

#include "script/Functor.h"
#include "script/Ncs.h"
#include "script/ScriptTypes.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ke::script {

struct VmError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The instruction limit: retail fails a run once its counter would reach 0x20000 (005d2bd0).
constexpr std::uint64_t kRetailInstructionBudget = 0x1FFFF;
// Nested script runs: retail keeps one frame per run and refuses a ninth (005d2260, 005d23e0).
constexpr int kRetailNestedRuns = 8;

// What an ExecuteScript run shares with the run that started it: retail has one instruction
// counter and one return stack, reset only by an outermost run (005d45d0, 005d4ad0).
struct VmCounters {
    std::uint64_t steps = 0;
    int returnDepth = 0;
};

enum class VmStatus { Running, Returned, Blocked, Budget, Error };

const char* statusName(VmStatus status);

class Vm;

// Action mediation boundary. The typed-default mode leaves this null; the
// functional runtime implements it over a World. Handlers may set the VM
// status to Blocked (ActionWait) and return no cells.
class ActionSink {
public:
    virtual ~ActionSink() = default;
    virtual std::vector<Value> call(std::uint16_t actionId,
                                    const std::vector<std::vector<Value>>& args, Vm& vm) = 0;
    // Register a captured action closure. The default sink (typed-default VM)
    // does not execute closures and returns an invalid id.
    virtual std::int64_t registerFunctor(Functor /*functor*/) { return -1; }
    // Engine values compare by content in retail (0052c510). Without a store, by id.
    virtual bool engineEquals(const Value& a, const Value& b) { return a.integer == b.integer; }
};

struct VmResult {
    VmStatus status = VmStatus::Returned;
    std::uint64_t steps = 0;
    std::size_t maxStack = 0;
    std::map<std::uint16_t, std::uint32_t> actions;
    std::map<std::string, std::uint32_t> notes;
    std::string error;
    std::int32_t errorIp = -1;
    std::uint16_t blockedAction = 0;
    bool hasBlockedAction = false;
    std::optional<Value> top;
    // The script's return value as a caller sees it (005d45d0, 005d1820).
    std::optional<std::int32_t> returned;
};

class Vm {
public:
    Vm(std::vector<Instruction> instructions, ActionSink* sink = nullptr,
       std::uint64_t budget = kRetailInstructionBudget, std::set<std::uint16_t> blockingActions = {202},
       bool trace = false);
    // Runs an already decoded script without copying it.
    static Vm shared(ProgramPtr program, ActionSink* sink, std::uint64_t budget);

    VmResult run();
    void setEntry(std::size_t ip, const std::vector<Value>& stack, std::int64_t bp);
    Functor captureContinuation(const std::string& source) const;

    std::vector<Value>& stack() { return stack_; }
    const std::vector<Value>& stack() const { return stack_; }
    std::int64_t& basePointer() { return bp_; }
    VmStatus status() const { return status_; }
    void setStatus(VmStatus status) { status_ = status; }
    void block(std::uint16_t actionId);
    std::uint64_t steps() const { return steps_; }
    const std::vector<std::uint32_t>& traceOffsets() const { return trace_; }
    // The instruction an action was called from, for a harness that looks at what follows it.
    std::uint32_t currentOffset() const { return static_cast<std::uint32_t>(errorIp_); }
    const std::vector<Instruction>& code() const { return *instructions_; }
    // When set, every instruction run by any Vm lands here (the quest search). Off in play.
    static inline std::uint64_t totalSteps = 0; // Every instruction of every run: a measure of work that repeats.
    static inline std::unordered_set<std::uint64_t>* coverage = nullptr;
    static std::uint64_t coverageKey(const std::string& script, std::uint32_t offset);
    // Names the running script so captured closures can be persisted.
    void setScriptName(std::string name) { scriptName_ = std::move(name); }
    // Live sessions resolve the two reserved object constants when CONST runs:
    // 0 is OBJECT_SELF and 1 is OBJECT_INVALID (shipped scripts encode them this
    // way). The pinned corpus oracle leaves this off and keeps the raw cells.
    // `self` is the id OBJECT_SELF resolves to.
    void setLiveObjectConstants(std::uint32_t self)
    {
        liveObjectConstants_ = true;
        selfObject_ = self;
    }
    // Gate fixtures name objects by id in a CONST, which no shipped script does and retail
    // turns into OBJECT_INVALID. Only the fixture entry point (runScriptNcsForTest) asks for this.
    void keepObjectIdsForTest() { keepObjectIds_ = true; }
    // Retail runs a script with or without an OBJECT_SELF (005d45d0's last argument).
    void setSelfValid(bool valid) { selfValid_ = valid; }
    // Joins an enclosing run's instruction counter and return stack.
    void shareCounters(VmCounters* counters) { shared_ = counters; }

    friend struct VmTestAccess;

private:
    struct SharedProgram {};
    Vm(SharedProgram, ProgramPtr program, ActionSink* sink, std::uint64_t budget);

    void pushDefault(ReturnKind kind);
    std::vector<Value> pop(std::size_t count);
    Value popValue();
    void drop(std::size_t count);
    void pushCopies(std::size_t index, std::size_t count);
    std::vector<Value> peek(std::size_t count) const;
    std::size_t targetIndex(const Instruction& ins) const;
    std::optional<std::size_t> step(const Instruction& ins);
    void doAction(const Instruction& ins);
    std::optional<std::size_t> arithmetic(const Instruction& ins, std::size_t next);
    std::int32_t popInt();
    double popFloat();
    std::string popString();
    Functor takeSavedState() const;
    VmCounters& counters() { return shared_ != nullptr ? *shared_ : own_; }
    static void increment(Value& value, int delta);
    bool valuesEqual(const Value& a, const Value& b) const;
    static std::string lowerAscii(std::string text);

    ProgramPtr program_;
    const std::vector<Instruction>* instructions_ = nullptr;
    ActionSink* sink_ = nullptr;
    std::uint64_t budget_ = kRetailInstructionBudget;
    std::set<std::uint16_t> blockingActions_;

    std::vector<Value> stack_;
    std::int64_t bp_ = 0;
    std::vector<std::size_t> callStack_;
    std::size_t ip_ = 0;
    std::uint64_t steps_ = 0;
    std::size_t maxStack_ = 0;
    struct SavedState {
        std::uint32_t resumeOffset = 0;
        std::int32_t stackBytes = 0;
        std::int32_t baseBytes = 0;
    };
    std::optional<SavedState> savedState_;
    VmCounters own_;
    VmCounters* shared_ = nullptr;
    VmStatus status_ = VmStatus::Running;
    std::string error_;
    std::int32_t errorIp_ = -1;
    std::uint16_t blockedAction_ = 0;
    bool hasBlockedAction_ = false;
    std::map<std::uint16_t, std::uint32_t> actionCounts_;
    std::map<std::string, std::uint32_t> notes_;
    bool traceEnabled_ = false;
    std::vector<std::uint32_t> trace_;
    std::string scriptName_;
    bool liveObjectConstants_ = false;
    bool selfValid_ = true;
    bool keepObjectIds_ = false;
    std::uint32_t selfObject_ = 0;
};

} // namespace ke::script
