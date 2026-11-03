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
#include <vector>

namespace ke::script {

struct VmError : std::runtime_error {
    using std::runtime_error::runtime_error;
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
};

class Vm {
public:
    Vm(std::vector<Instruction> instructions, ActionSink* sink = nullptr,
       std::uint64_t budget = 200000, std::set<std::uint16_t> blockingActions = {202},
       bool trace = false);

    VmResult run();
    void setEntry(std::size_t ip, std::vector<Value> stack, std::int64_t bp);
    Functor captureContinuation(const std::string& source) const;

    std::vector<Value>& stack() { return stack_; }
    const std::vector<Value>& stack() const { return stack_; }
    std::int64_t& basePointer() { return bp_; }
    VmStatus status() const { return status_; }
    void setStatus(VmStatus status) { status_ = status; }
    void block(std::uint16_t actionId);
    std::uint64_t steps() const { return steps_; }
    const std::vector<std::uint32_t>& traceOffsets() const { return trace_; }

    friend struct VmTestAccess;

private:
    void pushDefault(ReturnKind kind);
    std::vector<Value> pop(std::size_t count);
    std::vector<Value> peek(std::size_t count) const;
    std::size_t targetIndex(const Instruction& ins) const;
    std::optional<std::size_t> step(const Instruction& ins);
    void doAction(const Instruction& ins);
    static void increment(Value& value, int delta);
    static std::int64_t truncatedDivide(std::int64_t a, std::int64_t b);
    static bool valuesEqual(const Value& a, const Value& b);
    static std::string lowerAscii(std::string text);

    std::vector<Instruction> instructions_;
    std::unordered_map<std::uint32_t, std::size_t> byOffset_;
    ActionSink* sink_ = nullptr;
    std::uint64_t budget_ = 200000;
    std::set<std::uint16_t> blockingActions_;

    std::vector<Value> stack_;
    std::int64_t bp_ = 0;
    std::vector<std::size_t> callStack_;
    std::size_t ip_ = 0;
    std::uint64_t steps_ = 0;
    std::size_t maxStack_ = 0;
    bool pendingFunctor_ = false;
    std::optional<Functor> pendingFunctorData_;
    VmStatus status_ = VmStatus::Running;
    std::string error_;
    std::int32_t errorIp_ = -1;
    std::uint16_t blockedAction_ = 0;
    bool hasBlockedAction_ = false;
    std::map<std::uint16_t, std::uint32_t> actionCounts_;
    std::map<std::string, std::uint32_t> notes_;
    bool traceEnabled_ = false;
    std::vector<std::uint32_t> trace_;
};

} // namespace ke::script
