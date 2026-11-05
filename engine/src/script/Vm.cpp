#include "script/Vm.h"

#include "script/ActionTable.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace ke::script {

namespace {

ParamKind kindForQualifier(std::uint8_t qualifier)
{
    switch (qualifier) {
    case 0x03: return ParamKind::Int;
    case 0x04: return ParamKind::Float;
    case 0x05: return ParamKind::String;
    case 0x06: return ParamKind::Object;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: return ParamKind::Engine;
    default: return ParamKind::Int;
    }
}

} // namespace

const char* statusName(VmStatus status)
{
    switch (status) {
    case VmStatus::Running: return "running";
    case VmStatus::Returned: return "returned";
    case VmStatus::Blocked: return "blocked";
    case VmStatus::Budget: return "budget";
    case VmStatus::Error: return "error";
    }
    return "unknown";
}

Vm::Vm(std::vector<Instruction> instructions, ActionSink* sink, std::uint64_t budget,
       std::set<std::uint16_t> blockingActions, bool trace)
    : instructions_(std::move(instructions))
    , sink_(sink)
    , budget_(budget)
    , blockingActions_(std::move(blockingActions))
    , traceEnabled_(trace)
{
    byOffset_.reserve(instructions_.size() * 2);
    for (std::size_t i = 0; i < instructions_.size(); ++i) {
        byOffset_.emplace(instructions_[i].offset, i);
    }
}

std::vector<Value> Vm::pop(std::size_t count)
{
    if (count == 0) {
        return {};
    }
    if (stack_.size() < count) {
        throw VmError("stack underflow");
    }
    std::vector<Value> out(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
    stack_.erase(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
    return out;
}

std::vector<Value> Vm::peek(std::size_t count) const
{
    if (stack_.size() < count) {
        throw VmError("stack underflow");
    }
    return std::vector<Value>(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
}

void Vm::pushDefault(ReturnKind kind)
{
    appendDefault(stack_, kind);
}

std::size_t Vm::targetIndex(const Instruction& ins) const
{
    const std::int32_t target = static_cast<std::int32_t>(ins.offset) + ins.operand0;
    const auto it = byOffset_.find(static_cast<std::uint32_t>(target));
    if (it == byOffset_.end()) {
        throw VmError("jump target is not an instruction");
    }
    return it->second;
}

void Vm::block(std::uint16_t actionId)
{
    status_ = VmStatus::Blocked;
    blockedAction_ = actionId;
    hasBlockedAction_ = true;
}

void Vm::increment(Value& value, int delta)
{
    if (value.type == ValueType::Float) {
        value.real += static_cast<double>(delta);
    } else {
        value.integer += delta;
    }
}

std::int64_t Vm::truncatedDivide(std::int64_t a, std::int64_t b)
{
    if (b == 0) {
        return 0;
    }
    const std::int64_t q = std::llabs(a) / std::llabs(b);
    return (a < 0) != (b < 0) ? -q : q;
}

std::string Vm::lowerAscii(std::string text)
{
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

bool Vm::valuesEqual(const Value& a, const Value& b)
{
    if (a.type == ValueType::String || b.type == ValueType::String) {
        return a.type == ValueType::String && b.type == ValueType::String
            && lowerAscii(a.text) == lowerAscii(b.text);
    }
    if (a.type == ValueType::Float || b.type == ValueType::Float) {
        return a.asFloat() == b.asFloat();
    }
    return a.integer == b.integer;
}

void Vm::doAction(const Instruction& ins)
{
    const ActionSpec& spec = actionSpec(ins.actionId);
    std::vector<std::vector<Value>> args;
    args.reserve(ins.actionArgc);
    for (std::uint8_t index = 0; index < ins.actionArgc; ++index) {
        const ParamKind kind = index < spec.paramCount ? spec.params[index] : ParamKind::Int;
        if (kind == ParamKind::Action) {
            // A functor is created by STORESTATE and is not a stack cell.
            if (pendingFunctor_ && pendingFunctorData_.has_value()) {
                Functor functor = std::move(*pendingFunctorData_);
                pendingFunctorData_.reset();
                const std::int64_t id = sink_ != nullptr
                    ? sink_->registerFunctor(std::move(functor))
                    : -1;
                args.push_back({Value::makeFunctor(id)});
            } else {
                args.emplace_back();
            }
            pendingFunctor_ = false;
            continue;
        }
        const int width = parameterCells(kind);
        args.push_back(pop(static_cast<std::size_t>(std::max(0, width))));
    }
    if (ins.actionArgc < spec.paramCount) {
        ++notes_["action_missing_args"];
    } else if (ins.actionArgc > spec.paramCount) {
        ++notes_["action_extra_args"];
    }
    ++actionCounts_[ins.actionId];
    if (sink_ != nullptr) {
        std::vector<Value> cells = sink_->call(ins.actionId, args, *this);
        if (!cells.empty()) {
            stack_.insert(stack_.end(), cells.begin(), cells.end());
        }
        return;
    }
    if (blockingActions_.contains(ins.actionId)) {
        block(ins.actionId);
        return;
    }
    pushDefault(spec.returnKind);
}

std::optional<std::size_t> Vm::step(const Instruction& ins)
{
    const std::size_t next = ip_ + 1;
    switch (ins.op) {
    case Op::NOP:
    case Op::STORESTATEALL:
        return next;
    case Op::STORESTATE: {
        // The closure body follows STORESTATE and a JMP that skips it on the
        // normal path. Resume at the body, with a snapshot of the stack/BP.
        std::size_t entry = ip_ + 1u;
        if (entry < instructions_.size() && instructions_[entry].op == Op::JMP) {
            ++entry;
        }
        Functor functor;
        functor.instructions = instructions_;
        functor.ip = entry;
        functor.stack = stack_;
        functor.bp = bp_;
        functor.script = scriptName_;
        pendingFunctorData_ = std::move(functor);
        pendingFunctor_ = true;
        return next;
    }
    case Op::CPTOPSP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = stackSize + ins.operand0 / 4;
        const std::int64_t count = ins.u0 / 4;
        if (count <= 0) {
            return next;
        }
        if (index < 0 || index + count > stackSize) {
            throw VmError("CPTOPSP outside frame");
        }
        const std::vector<Value> copy(stack_.begin() + index, stack_.begin() + index + count);
        stack_.insert(stack_.end(), copy.begin(), copy.end());
        return next;
    }
    case Op::CPDOWNSP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = stackSize + ins.operand0 / 4;
        const std::int64_t count = ins.u0 / 4;
        if (count <= 0) {
            return next;
        }
        if (stackSize < count || index < 0 || index + count > stackSize) {
            throw VmError("CPDOWNSP outside frame");
        }
        std::copy(stack_.end() - count, stack_.end(), stack_.begin() + index);
        return next;
    }
    case Op::CPTOPBP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = bp_ + ins.operand0 / 4;
        const std::int64_t count = ins.u0 / 4;
        if (count <= 0) {
            return next;
        }
        if (index < 0 || index + count > stackSize) {
            throw VmError("CPTOPBP outside globals");
        }
        const std::vector<Value> copy(stack_.begin() + index, stack_.begin() + index + count);
        stack_.insert(stack_.end(), copy.begin(), copy.end());
        return next;
    }
    case Op::CPDOWNBP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = bp_ + ins.operand0 / 4;
        const std::int64_t count = ins.u0 / 4;
        if (count <= 0) {
            return next;
        }
        if (stackSize < count || index < 0 || index + count > stackSize) {
            throw VmError("CPDOWNBP outside globals");
        }
        std::copy(stack_.end() - count, stack_.end(), stack_.begin() + index);
        return next;
    }
    case Op::RSADD: {
        const ParamKind kind = kindForQualifier(ins.qualifier);
        switch (kind) {
        case ParamKind::Float: stack_.push_back(Value::makeFloat(0.0)); break;
        case ParamKind::String: stack_.push_back(Value::makeString("")); break;
        case ParamKind::Object: stack_.push_back(Value::makeObject(kObjectInvalid)); break;
        case ParamKind::Engine: stack_.push_back(Value::makeEngine(0)); break;
        default: stack_.push_back(Value::makeInt(0)); break;
        }
        return next;
    }
    case Op::CONST:
        if (ins.qualifier == 0x03) {
            stack_.push_back(Value::makeInt(ins.operand0));
        } else if (ins.qualifier == 0x04) {
            stack_.push_back(Value::makeFloat(ins.f0));
        } else if (ins.qualifier == 0x05) {
            stack_.push_back(Value::makeString(std::string(ins.bytes.begin(), ins.bytes.end())));
        } else if (ins.qualifier == 0x06) {
            std::uint32_t object = static_cast<std::uint32_t>(ins.operand0);
            if (liveObjectConstants_) {
                object = object == 0 ? selfObject_ : object == 1 ? kObjectInvalid : object;
            }
            stack_.push_back(Value::makeObject(object));
        } else {
            throw VmError("bad CONST type");
        }
        return next;
    case Op::ACTION:
        doAction(ins);
        return status_ == VmStatus::Blocked ? std::nullopt : std::optional<std::size_t>(next);
    case Op::SAVEBP:
        stack_.push_back(Value::makeInt(bp_));
        bp_ = static_cast<std::int64_t>(stack_.size()) - 1;
        return next;
    case Op::RESTOREBP: {
        const std::vector<Value> saved = pop(1);
        bp_ = saved.front().asInt();
        return next;
    }
    case Op::MOVSP: {
        if (ins.operand0 < 0) {
            const std::size_t count = static_cast<std::size_t>(-ins.operand0) / 4;
            if (count != 0) {
                pop(count);
            }
        } else if (ins.operand0 > 0) {
            const std::size_t count = static_cast<std::size_t>(ins.operand0) / 4;
            for (std::size_t i = 0; i < count; ++i) {
                stack_.push_back(Value::makeInt(0));
            }
        }
        return next;
    }
    case Op::INCSP:
    case Op::DECSP: {
        const std::int64_t index = static_cast<std::int64_t>(stack_.size()) + ins.operand0 / 4;
        if (index < 0 || index >= static_cast<std::int64_t>(stack_.size())) {
            throw VmError("INCSP/DECSP outside stack");
        }
        increment(stack_[static_cast<std::size_t>(index)], ins.op == Op::INCSP ? 1 : -1);
        return next;
    }
    case Op::INCBP:
    case Op::DECBP: {
        const std::int64_t index = bp_ + ins.operand0 / 4;
        if (index < 0 || index >= static_cast<std::int64_t>(stack_.size())) {
            throw VmError("INCBP/DECBP outside globals");
        }
        increment(stack_[static_cast<std::size_t>(index)], ins.op == Op::INCBP ? 1 : -1);
        return next;
    }
    case Op::DESTRUCT: {
        const std::int64_t size = ins.u0 / 4;
        const std::int64_t offset = ins.u1 / 4;
        const std::int64_t keep = ins.u2 / 4;
        if (size < 0 || offset < 0 || keep < 0 || offset + keep > size) {
            throw VmError("DESTRUCT invalid");
        }
        if (size == 0) {
            return next;
        }
        const std::int64_t begin = static_cast<std::int64_t>(stack_.size()) - size;
        if (begin < 0) {
            throw VmError("DESTRUCT underflow");
        }
        std::vector<Value> retained(stack_.begin() + begin + offset,
                                    stack_.begin() + begin + offset + keep);
        stack_.erase(stack_.begin() + begin, stack_.end());
        stack_.insert(stack_.end(), retained.begin(), retained.end());
        return next;
    }
    case Op::JMP:
        return targetIndex(ins);
    case Op::JSR:
        if (callStack_.size() >= 8) {
            throw VmError("recursion limit 8 exceeded");
        }
        callStack_.push_back(next);
        return targetIndex(ins);
    case Op::RETN:
        if (!callStack_.empty()) {
            const std::size_t target = callStack_.back();
            callStack_.pop_back();
            return target;
        }
        status_ = VmStatus::Returned;
        return std::nullopt;
    case Op::JZ:
    case Op::JNZ: {
        const bool condition = pop(1).front().truthy();
        const bool take = ins.op == Op::JNZ ? condition : !condition;
        return take ? std::optional<std::size_t>(targetIndex(ins)) : std::optional<std::size_t>(next);
    }
    case Op::LOGAND:
    case Op::LOGOR: {
        const bool b = pop(1).front().truthy();
        const bool a = pop(1).front().truthy();
        const bool value = ins.op == Op::LOGAND ? (a && b) : (a || b);
        stack_.push_back(Value::makeInt(value ? 1 : 0));
        return next;
    }
    case Op::BOOLAND:
    case Op::INCOR:
    case Op::EXCOR: {
        const std::int64_t b = pop(1).front().asInt();
        const std::int64_t a = pop(1).front().asInt();
        std::int64_t value = 0;
        if (ins.op == Op::BOOLAND) {
            value = a & b;
        } else if (ins.op == Op::INCOR) {
            value = a | b;
        } else {
            value = a ^ b;
        }
        stack_.push_back(Value::makeInt(value));
        return next;
    }
    case Op::SHLEFT:
    case Op::SHRIGHT:
    case Op::USHRIGHT: {
        const std::int64_t shift = pop(1).front().asInt() & 31;
        const std::int64_t a = pop(1).front().asInt();
        std::int64_t value = 0;
        if (ins.op == Op::SHLEFT) {
            value = static_cast<std::int64_t>((static_cast<std::uint64_t>(a) << shift) & 0xFFFFFFFFu);
        } else if (ins.op == Op::USHRIGHT) {
            value = static_cast<std::int64_t>((static_cast<std::uint64_t>(a) & 0xFFFFFFFFu) >> shift);
        } else {
            value = a >> shift;
        }
        stack_.push_back(Value::makeInt(value));
        return next;
    }
    case Op::ADD:
    case Op::SUB:
    case Op::MUL:
    case Op::DIV:
    case Op::MOD: {
        const Value b = pop(1).front();
        const Value a = pop(1).front();
        const ParamKind qualifier = kindForQualifier(ins.qualifier);
        if (ins.op == Op::ADD
            && (qualifier == ParamKind::String || a.type == ValueType::String
                || b.type == ValueType::String)) {
            stack_.push_back(Value::makeString(a.asString() + b.asString()));
            return next;
        }
        if (qualifier == ParamKind::Float || a.type == ValueType::Float
            || b.type == ValueType::Float) {
            const double x = a.asFloat();
            const double y = b.asFloat();
            double value = 0.0;
            switch (ins.op) {
            case Op::ADD: value = x + y; break;
            case Op::SUB: value = x - y; break;
            case Op::MUL: value = x * y; break;
            case Op::DIV: value = y == 0.0 ? 0.0 : x / y; break;
            default:
                value = y == 0.0 ? 0.0
                                 : x - static_cast<double>(truncatedDivide(
                                             static_cast<std::int64_t>(x),
                                             static_cast<std::int64_t>(y)))
                    * y;
                break;
            }
            stack_.push_back(Value::makeFloat(value));
            return next;
        }
        const std::int64_t x = a.asInt();
        const std::int64_t y = b.asInt();
        std::int64_t value = 0;
        switch (ins.op) {
        case Op::ADD: value = x + y; break;
        case Op::SUB: value = x - y; break;
        case Op::MUL: value = x * y; break;
        case Op::DIV: value = truncatedDivide(x, y); break;
        default: value = x - truncatedDivide(x, y) * y; break;
        }
        stack_.push_back(Value::makeInt(value));
        return next;
    }
    case Op::EQUAL:
    case Op::NEQUAL: {
        bool equal = false;
        if (ins.qualifier == 0x24) {
            std::size_t count = ins.u0 / 4;
            if (count == 0) {
                count = 1;
            }
            const std::vector<Value> b = pop(count);
            const std::vector<Value> a = pop(count);
            equal = true;
            for (std::size_t i = 0; i < count; ++i) {
                if (!valuesEqual(a[i], b[i])) {
                    equal = false;
                    break;
                }
            }
        } else {
            const Value b = pop(1).front();
            const Value a = pop(1).front();
            equal = valuesEqual(a, b);
        }
        const bool value = ins.op == Op::EQUAL ? equal : !equal;
        stack_.push_back(Value::makeInt(value ? 1 : 0));
        return next;
    }
    case Op::GEQ:
    case Op::GT:
    case Op::LT:
    case Op::LEQ: {
        const Value b = pop(1).front();
        const Value a = pop(1).front();
        bool value = false;
        if (a.type == ValueType::String || b.type == ValueType::String) {
            const std::string x = lowerAscii(a.asString());
            const std::string y = lowerAscii(b.asString());
            switch (ins.op) {
            case Op::GEQ: value = x >= y; break;
            case Op::GT: value = x > y; break;
            case Op::LT: value = x < y; break;
            default: value = x <= y; break;
            }
        } else if (a.type == ValueType::Float || b.type == ValueType::Float
                   || kindForQualifier(ins.qualifier) == ParamKind::Float) {
            const double x = a.asFloat();
            const double y = b.asFloat();
            switch (ins.op) {
            case Op::GEQ: value = x >= y; break;
            case Op::GT: value = x > y; break;
            case Op::LT: value = x < y; break;
            default: value = x <= y; break;
            }
        } else {
            const std::int64_t x = a.asInt();
            const std::int64_t y = b.asInt();
            switch (ins.op) {
            case Op::GEQ: value = x >= y; break;
            case Op::GT: value = x > y; break;
            case Op::LT: value = x < y; break;
            default: value = x <= y; break;
            }
        }
        stack_.push_back(Value::makeInt(value ? 1 : 0));
        return next;
    }
    case Op::NEG: {
        const Value value = pop(1).front();
        if (value.type == ValueType::Float) {
            stack_.push_back(Value::makeFloat(-value.asFloat()));
        } else {
            stack_.push_back(Value::makeInt(-value.asInt()));
        }
        return next;
    }
    case Op::COMP: {
        const std::int64_t value = pop(1).front().asInt();
        stack_.push_back(Value::makeInt((~value) & 0xFFFFFFFF));
        return next;
    }
    case Op::NOT: {
        const bool value = pop(1).front().truthy();
        stack_.push_back(Value::makeInt(value ? 0 : 1));
        return next;
    }
    case Op::SCRIPTSIZE:
        return next;
    }
    throw VmError("unsupported opcode");
}

void Vm::setEntry(std::size_t ip, std::vector<Value> stack, std::int64_t bp)
{
    ip_ = std::min(ip, instructions_.size());
    stack_ = std::move(stack);
    bp_ = bp;
    maxStack_ = stack_.size();
    status_ = VmStatus::Running;
}

Functor Vm::captureContinuation(const std::string& source) const
{
    Functor functor;
    functor.instructions = instructions_;
    functor.ip = std::min(ip_ + 1u, instructions_.size());
    functor.stack = stack_;
    functor.bp = bp_;
    functor.source = source;
    functor.script = scriptName_;
    return functor;
}

VmResult Vm::run()
{
    try {
        while (ip_ < instructions_.size()) {
            if (steps_ >= budget_) {
                status_ = VmStatus::Budget;
                break;
            }
            const Instruction& ins = instructions_[ip_];
            errorIp_ = static_cast<std::int32_t>(ins.offset);
            ++steps_;
            if (traceEnabled_) {
                trace_.push_back(ins.offset);
            }
            const std::optional<std::size_t> next = step(ins);
            maxStack_ = std::max(maxStack_, stack_.size());
            if (!next.has_value()) {
                break;
            }
            ip_ = *next;
        }
    } catch (const VmError& error) {
        status_ = VmStatus::Error;
        error_ = error.what();
    } catch (const std::exception& error) {
        status_ = VmStatus::Error;
        error_ = std::string("exception: ") + error.what();
    }
    if (status_ == VmStatus::Running) {
        status_ = VmStatus::Returned;
    }
    VmResult result;
    result.status = status_;
    result.steps = steps_;
    result.maxStack = maxStack_;
    result.actions = actionCounts_;
    result.notes = notes_;
    result.error = error_;
    result.errorIp = status_ == VmStatus::Error ? errorIp_ : -1;
    result.blockedAction = blockedAction_;
    result.hasBlockedAction = hasBlockedAction_;
    if (!stack_.empty()) {
        result.top = stack_.back();
    }
    return result;
}

} // namespace ke::script
