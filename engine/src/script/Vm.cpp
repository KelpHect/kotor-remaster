#include "script/Vm.h"

#include "script/ActionTable.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace ke::script {

namespace {

// The retail cell is 32 bits: integers wrap and floats are single precision (005d2bd0).
std::int64_t wrap32(std::int64_t value)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

double single(double value)
{
    return static_cast<double>(static_cast<float>(value));
}

// `SP - ((-offset) >> 2)` in retail, so an offset rounds as an arithmetic shift does.
std::int64_t cellOffset(std::int32_t offset)
{
    return -((-static_cast<std::int64_t>(offset)) >> 2);
}

// A size operand is read as a signed 16-bit byte count, then shifted (005d2bd0).
std::int64_t cellCount(std::uint16_t size)
{
    return static_cast<std::int16_t>(size) >> 2;
}

// Bits a cell holds, which is what EQUAL compares for int, float and object (005d2bd0).
std::uint32_t cellBits(const Value& value)
{
    if (value.type == ValueType::Float) {
        const float real = static_cast<float>(value.real);
        return std::bit_cast<std::uint32_t>(real);
    }
    return static_cast<std::uint32_t>(value.integer);
}

[[noreturn]] void badForm() { throw VmError("bad operand form"); }           // retail -0x61
[[noreturn]] void badStack() { throw VmError("stack or type check failed"); } // retail -100
[[noreturn]] void divideByZero() { throw VmError("divide by zero"); }        // retail -0x69

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
    : program_(makeProgram(std::move(instructions)))
    , instructions_(&program_->code)
    , sink_(sink)
    , budget_(budget)
    , blockingActions_(std::move(blockingActions))
    , traceEnabled_(trace)
{
}

Vm::Vm(SharedProgram, ProgramPtr program, ActionSink* sink, std::uint64_t budget)
    : program_(program != nullptr ? std::move(program) : makeProgram({}))
    , instructions_(&program_->code)
    , sink_(sink)
    , budget_(budget)
    , blockingActions_({202})
{
}

Vm Vm::shared(ProgramPtr program, ActionSink* sink, std::uint64_t budget)
{
    return Vm(SharedProgram{}, std::move(program), sink, budget);
}

std::vector<Value> Vm::pop(std::size_t count)
{
    if (count == 0) {
        return {};
    }
    if (stack_.size() < count) {
        throw VmError("stack underflow");
    }
    const auto first = stack_.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<Value> out(std::make_move_iterator(first), std::make_move_iterator(stack_.end()));
    stack_.erase(first, stack_.end());
    return out;
}

Value Vm::popValue()
{
    if (stack_.empty()) {
        throw VmError("stack underflow");
    }
    Value value = std::move(stack_.back());
    stack_.pop_back();
    return value;
}

void Vm::drop(std::size_t count)
{
    if (stack_.size() < count) {
        throw VmError("stack underflow");
    }
    stack_.erase(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
}

// Pushes copies of existing cells; the caller has checked the range.
void Vm::pushCopies(std::size_t index, std::size_t count)
{
    const std::size_t needed = stack_.size() + count;
    if (stack_.capacity() < needed) {
        stack_.reserve(std::max(needed, stack_.capacity() * 2));
    }
    for (std::size_t i = 0; i < count; ++i) {
        stack_.push_back(stack_[index + i]);
    }
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
    const auto it = program_->byOffset.find(static_cast<std::uint32_t>(target));
    if (it == program_->byOffset.end()) {
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

// Pops a cell of one type. Retail checks the tag and fails the run on a mismatch
// (005d24c0 int, 005d2510 float, 005d25e0 string, 005d26a0 object).
std::int32_t Vm::popInt()
{
    if (stack_.empty() || stack_.back().type != ValueType::Int) {
        badStack();
    }
    const std::int32_t value = static_cast<std::int32_t>(stack_.back().integer);
    stack_.pop_back();
    return value;
}

double Vm::popFloat()
{
    if (stack_.empty() || stack_.back().type != ValueType::Float) {
        badStack();
    }
    const double value = stack_.back().real;
    stack_.pop_back();
    return value;
}

std::string Vm::popString()
{
    if (stack_.empty() || stack_.back().type != ValueType::String) {
        badStack();
    }
    std::string value = stack_.back().asString();
    stack_.pop_back();
    return value;
}

// INC/DEC only touch an int cell; any other cell is left alone (005d16c0).
void Vm::increment(Value& value, int delta)
{
    if (value.type == ValueType::Int) {
        value.integer = wrap32(value.integer + delta);
    }
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

// The right operand's tag picks the rule: bits for int, float and object, so -0.0 and 0.0
// differ; ASCII case-insensitive for string (005e6450); the type's own compare for engine
// values (0052c510). The left operand's tag is not looked at.
bool Vm::valuesEqual(const Value& a, const Value& b) const
{
    switch (b.type) {
    case ValueType::Int:
    case ValueType::Float:
    case ValueType::Object:
        return cellBits(a) == cellBits(b);
    case ValueType::String:
        return a.type == ValueType::String && lowerAscii(a.text) == lowerAscii(b.text);
    case ValueType::Engine:
        return sink_ != nullptr ? sink_->engineEquals(a, b) : a.integer == b.integer;
    case ValueType::Functor:
        break;
    }
    throw VmError("cell of no comparable type"); // retail -0x6d
}

void Vm::doAction(const Instruction& ins)
{
    const ActionSpec& spec = actionSpec(ins.actionId);
    std::vector<std::vector<Value>> args;
    args.reserve(ins.actionArgc);
    // Handlers pop what they declare, so a cell a stale script pushed beyond that stays on
    // the stack: k_creditsplay's second argument to 518 (005473e0 pops one int).
    const std::uint8_t readable = std::min<std::uint8_t>(ins.actionArgc, static_cast<std::uint8_t>(spec.paramCount));
    for (std::uint8_t index = 0; index < readable; ++index) {
        const ParamKind kind = spec.params[index];
        if (kind == ParamKind::Action) {
            // Not a stack cell: the state STORESTATE noted is saved when the handler
            // asks for it, after the arguments above it are off the stack (005d4380).
            if (savedState_.has_value()) {
                const std::int64_t id = sink_ != nullptr ? sink_->registerFunctor(takeSavedState()) : -1;
                args.push_back({Value::makeFunctor(id)});
                savedState_.reset();
            } else {
                args.emplace_back();
            }
            continue;
        }
        const int width = parameterCells(kind);
        args.push_back(pop(static_cast<std::size_t>(std::max(0, width))));
        // Handlers read arguments through the tag-checking pops; a mismatch fails the run.
        const auto wrong = [&](ValueType wanted) {
            return std::any_of(args.back().begin(), args.back().end(),
                               [&](const Value& cell) { return cell.type != wanted; });
        };
        if ((kind == ParamKind::Int && wrong(ValueType::Int))
            || ((kind == ParamKind::Float || kind == ParamKind::Vector) && wrong(ValueType::Float))
            || (kind == ParamKind::String && wrong(ValueType::String))
            || (kind == ParamKind::Object && wrong(ValueType::Object))) {
            ++notes_["action_arg_type"];
            badStack();
        }
    }
    if (ins.actionArgc < spec.paramCount) {
        ++notes_["action_missing_args"];
    } else if (ins.actionArgc > spec.paramCount) {
        ++notes_["action_extra_args"];
    }
    ++actionCounts_[ins.actionId];
    if (sink_ != nullptr) {
        std::vector<Value> cells = sink_->call(ins.actionId, args, *this);
        for (Value& cell : cells) {
            if (cell.type == ValueType::Float) {
                cell.real = single(cell.real);
            } else if (cell.type == ValueType::Int) {
                cell.integer = wrap32(cell.integer);
            }
        }
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
        throw VmError("unknown opcode"); // Past the dispatch bound at 005d2c45: retail -0x65.
    case Op::STORESTATEALL:
    case Op::STORESTATE:
        // Only notes where to resume and how much to keep (005d2bd0). The saving happens
        // in the action that consumes it, so a new note replaces an unused one. Sizes of
        // zero, and STORESTATEALL, keep the whole stack (005d28f0).
        savedState_ = SavedState{ins.offset + ins.qualifier,
                                 ins.op == Op::STORESTATE ? ins.operand1 : 0,
                                 ins.op == Op::STORESTATE ? ins.operand0 : 0};
        return next;
    case Op::CPTOPSP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = stackSize + cellOffset(ins.operand0);
        const std::int64_t count = cellCount(ins.u0);
        if (count <= 0) {
            return next;
        }
        if (index < 0 || index + count > stackSize) {
            throw VmError("CPTOPSP outside frame");
        }
        pushCopies(static_cast<std::size_t>(index), static_cast<std::size_t>(count));
        return next;
    }
    case Op::CPDOWNSP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = stackSize + cellOffset(ins.operand0);
        const std::int64_t count = cellCount(ins.u0);
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
        const std::int64_t index = bp_ + cellOffset(ins.operand0);
        const std::int64_t count = cellCount(ins.u0);
        if (count <= 0) {
            return next;
        }
        if (index < 0 || index + count > stackSize) {
            throw VmError("CPTOPBP outside globals");
        }
        pushCopies(static_cast<std::size_t>(index), static_cast<std::size_t>(count));
        return next;
    }
    case Op::CPDOWNBP: {
        const std::int64_t stackSize = static_cast<std::int64_t>(stack_.size());
        const std::int64_t index = bp_ + cellOffset(ins.operand0);
        const std::int64_t count = cellCount(ins.u0);
        if (count <= 0) {
            return next;
        }
        if (stackSize < count || index < 0 || index + count > stackSize) {
            throw VmError("CPDOWNBP outside globals");
        }
        std::copy(stack_.end() - count, stack_.end(), stack_.begin() + index);
        return next;
    }
    case Op::RSADD:
        switch (ins.qualifier) {
        case 0x03: stack_.push_back(Value::makeInt(0)); break;
        case 0x04: stack_.push_back(Value::makeFloat(0.0)); break;
        case 0x05: stack_.push_back(Value::makeString("")); break;
        case 0x06: stack_.push_back(Value::makeObject(kObjectInvalid)); break;
        default:
            if (ins.qualifier < 0x10 || ins.qualifier > 0x19) {
                badForm();
            }
            stack_.push_back(Value::makeEngine(0));
            break;
        }
        return next;
    case Op::CONST:
        if (ins.qualifier == 0x03) {
            stack_.push_back(Value::makeInt(ins.operand0));
        } else if (ins.qualifier == 0x04) {
            stack_.push_back(Value::makeFloat(ins.f0));
        } else if (ins.qualifier == 0x05) {
            // Built as a C string, so a NUL ends it (005e5680).
            const auto end = std::find(ins.bytes.begin(), ins.bytes.end(), std::uint8_t{0});
            stack_.push_back(Value::makeString(std::string(ins.bytes.begin(), end)));
        } else if (ins.qualifier == 0x06) {
            std::uint32_t object = static_cast<std::uint32_t>(ins.operand0);
            if (liveObjectConstants_) {
                // Only 0 means anything: OBJECT_SELF, when the run has one (005d2bd0).
                if (object == 0) {
                    object = selfValid_ ? selfObject_ : kObjectInvalid;
                } else if (object == 1 || !keepObjectIds_) {
                    object = kObjectInvalid;
                }
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
        bp_ = popValue().asInt();
        return next;
    }
    case Op::MOVSP: {
        // Only ever shrinks: 005d21f0 ignores a target at or above SP.
        if (ins.operand0 < 0) {
            drop(static_cast<std::size_t>(-cellOffset(ins.operand0)));
        }
        return next;
    }
    case Op::INCSP:
    case Op::DECSP: {
        if (ins.qualifier != 0x03) {
            return next;
        }
        const std::int64_t index = static_cast<std::int64_t>(stack_.size()) + cellOffset(ins.operand0);
        if (index < 0 || index >= static_cast<std::int64_t>(stack_.size())) {
            throw VmError("INCSP/DECSP outside stack");
        }
        increment(stack_[static_cast<std::size_t>(index)], ins.op == Op::INCSP ? 1 : -1);
        return next;
    }
    case Op::INCBP:
    case Op::DECBP: {
        if (ins.qualifier != 0x03) {
            return next;
        }
        const std::int64_t index = bp_ + cellOffset(ins.operand0);
        if (index < 0 || index >= static_cast<std::int64_t>(stack_.size())) {
            throw VmError("INCBP/DECBP outside globals");
        }
        increment(stack_[static_cast<std::size_t>(index)], ins.op == Op::INCBP ? 1 : -1);
        return next;
    }
    case Op::DESTRUCT: {
        const std::int64_t size = cellCount(ins.u0);
        const std::int64_t offset = cellCount(ins.u1);
        const std::int64_t keep = cellCount(ins.u2);
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
        if (offset > 0) {
            std::move(stack_.begin() + begin + offset, stack_.begin() + begin + offset + keep,
                      stack_.begin() + begin);
        }
        stack_.erase(stack_.begin() + begin + keep, stack_.end());
        return next;
    }
    case Op::JMP:
        return targetIndex(ins);
    case Op::JSR: {
        // retail 005d17f0: a 128-entry return stack that fails once it holds 128. The run's
        // own -1 sentinel (005d4270) and any enclosing ExecuteScript run share it. The limit
        // of 8 at 005d2260 is nested script runs, not JSR.
        int& depth = counters().returnDepth;
        if (depth + 1 >= 128) {
            throw VmError("recursion limit 128 exceeded"); // retail -99
        }
        ++depth;
        callStack_.push_back(next);
        return targetIndex(ins);
    }
    case Op::RETN:
        if (!callStack_.empty()) {
            const std::size_t target = callStack_.back();
            callStack_.pop_back();
            --counters().returnDepth;
            return target;
        }
        status_ = VmStatus::Returned;
        return std::nullopt;
    case Op::JZ:
    case Op::JNZ: {
        const bool condition = popInt() != 0;
        const bool take = ins.op == Op::JNZ ? condition : !condition;
        return take ? std::optional<std::size_t>(targetIndex(ins)) : std::optional<std::size_t>(next);
    }
    case Op::LOGAND:
    case Op::LOGOR: {
        if (ins.qualifier != 0x20) {
            badForm();
        }
        const bool b = popInt() != 0;
        const bool a = popInt() != 0;
        const bool value = ins.op == Op::LOGAND ? (a && b) : (a || b);
        stack_.push_back(Value::makeInt(value ? 1 : 0));
        return next;
    }
    case Op::BOOLAND:
    case Op::INCOR:
    case Op::EXCOR: {
        if (ins.qualifier != 0x20) {
            badForm();
        }
        const std::int32_t b = popInt();
        const std::int32_t a = popInt();
        const std::int32_t value = ins.op == Op::BOOLAND ? (a & b) : ins.op == Op::INCOR ? (a | b) : (a ^ b);
        stack_.push_back(Value::makeInt(value));
        return next;
    }
    case Op::SHLEFT:
    case Op::SHRIGHT:
    case Op::USHRIGHT: {
        if (ins.qualifier != 0x20) {
            badForm();
        }
        const int shift = popInt() & 31;
        const std::int32_t a = popInt();
        std::int32_t value = 0;
        if (ins.op == Op::SHLEFT) {
            value = static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << shift);
        } else if (ins.op == Op::USHRIGHT) {
            // `sar`, not `shr` (005d36da): despite the name it keeps the sign.
            value = a >> shift;
        } else if (a < 0) {
            // SHRIGHT shifts the magnitude and puts the sign back, so it rounds toward
            // zero (005d2bd0). -INT_MIN wraps to itself and then shifts arithmetically.
            const std::int32_t magnitude = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a));
            value = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(magnitude >> shift));
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
    case Op::MOD:
        return arithmetic(ins, next);
    case Op::EQUAL:
    case Op::NEQUAL: {
        const std::uint8_t q = ins.qualifier;
        if (!(q >= 0x20 && q <= 0x24) && !(q >= 0x30 && q <= 0x39)) {
            badForm();
        }
        const std::int64_t count = q == 0x24 ? cellCount(ins.u0) : 1;
        const std::int64_t size = static_cast<std::int64_t>(stack_.size());
        if (size < count * 2) {
            badStack();
        }
        bool equal = true;
        for (std::int64_t i = 1; i <= count; ++i) {
            const Value& right = stack_[static_cast<std::size_t>(size - i)];
            const Value& left = stack_[static_cast<std::size_t>(size - i - count)];
            equal = valuesEqual(left, right) && equal;
        }
        stack_.erase(stack_.end() - static_cast<std::ptrdiff_t>(std::max<std::int64_t>(0, count) * 2), stack_.end());
        stack_.push_back(Value::makeInt((ins.op == Op::EQUAL) == equal ? 1 : 0));
        return next;
    }
    case Op::GEQ:
    case Op::GT:
    case Op::LT:
    case Op::LEQ: {
        bool value = false;
        if (ins.qualifier == 0x20) {
            const std::int32_t y = popInt();
            const std::int32_t x = popInt();
            value = ins.op == Op::GEQ ? x >= y : ins.op == Op::GT ? x > y : ins.op == Op::LT ? x < y : x <= y;
        } else if (ins.qualifier == 0x21) {
            const double y = popFloat();
            const double x = popFloat();
            value = ins.op == Op::GEQ ? x >= y : ins.op == Op::GT ? x > y : ins.op == Op::LT ? x < y : x <= y;
        } else {
            badForm(); // Strings do not order in retail.
        }
        stack_.push_back(Value::makeInt(value ? 1 : 0));
        return next;
    }
    case Op::NEG:
        if (ins.qualifier == 0x03) {
            stack_.push_back(Value::makeInt(wrap32(-static_cast<std::int64_t>(popInt()))));
        } else if (ins.qualifier == 0x04) {
            stack_.push_back(Value::makeFloat(-popFloat()));
        } else {
            badForm();
        }
        return next;
    case Op::COMP:
        if (ins.qualifier != 0x03) {
            badForm();
        }
        stack_.push_back(Value::makeInt(~popInt()));
        return next;
    case Op::NOT:
        if (ins.qualifier != 0x03) {
            badForm();
        }
        stack_.push_back(Value::makeInt(popInt() == 0 ? 1 : 0));
        return next;
    case Op::SCRIPTSIZE:
        throw VmError("unknown opcode");
    }
    throw VmError("unsupported opcode");
}

// The type forms of 005d2bd0 cases 0x14 to 0x18. A form that names no result for this
// operator pops its operands and pushes nothing, as retail does.
std::optional<std::size_t> Vm::arithmetic(const Instruction& ins, std::size_t next)
{
    const Op op = ins.op;
    const auto pushVector = [this](double x, double y, double z) {
        stack_.push_back(Value::makeFloat(single(x)));
        stack_.push_back(Value::makeFloat(single(y)));
        stack_.push_back(Value::makeFloat(single(z)));
    };
    struct Vec {
        double x, y, z;
    };
    const auto popVector = [this]() {
        Vec v{};
        v.z = popFloat();
        v.y = popFloat();
        v.x = popFloat();
        return v;
    };
    if (op == Op::MOD && ins.qualifier != 0x20) {
        badForm();
    }
    switch (ins.qualifier) {
    case 0x20: {
        const std::int32_t y = popInt();
        const std::int32_t x = popInt();
        std::int64_t value = 0;
        switch (op) {
        case Op::ADD: value = static_cast<std::int64_t>(x) + y; break;
        case Op::SUB: value = static_cast<std::int64_t>(x) - y; break;
        case Op::MUL: value = static_cast<std::int64_t>(x) * y; break;
        default:
            if (y == 0) {
                divideByZero();
            }
            // INT_MIN / -1 traps in the retail `idiv`; no script reaches it and no handler
            // was traced. Failing the run is the nearest defined outcome.
            if (x == std::numeric_limits<std::int32_t>::min() && y == -1) {
                throw VmError("integer divide overflow");
            }
            value = op == Op::DIV ? x / y : x % y;
            break;
        }
        stack_.push_back(Value::makeInt(wrap32(value)));
        return next;
    }
    case 0x21:
    case 0x25:
    case 0x26: {
        // 0x25 is int then float, 0x26 float then int: the second operand is on top.
        const double y = ins.qualifier == 0x26 ? static_cast<double>(static_cast<float>(popInt())) : popFloat();
        const double x = ins.qualifier == 0x25 ? static_cast<double>(static_cast<float>(popInt())) : popFloat();
        double value = 0.0;
        switch (op) {
        case Op::ADD: value = x + y; break;
        case Op::SUB: value = x - y; break;
        case Op::MUL: value = x * y; break;
        default:
            if (y == 0.0) {
                divideByZero();
            }
            value = x / y;
            break;
        }
        stack_.push_back(Value::makeFloat(single(value)));
        return next;
    }
    case 0x23: {
        std::string right = popString();
        std::string left = popString();
        if (op == Op::ADD) {
            stack_.push_back(Value::makeString(left + right));
        }
        return next;
    }
    case 0x3A: {
        const Vec b = popVector();
        const Vec a = popVector();
        if (op == Op::ADD) {
            pushVector(a.x + b.x, a.y + b.y, a.z + b.z);
        } else if (op == Op::SUB) {
            pushVector(a.x - b.x, a.y - b.y, a.z - b.z);
        }
        return next;
    }
    case 0x3B:
    case 0x3C: {
        Vec v{};
        double scalar = 0.0;
        if (ins.qualifier == 0x3B) {
            scalar = popFloat();
            v = popVector();
        } else {
            v = popVector();
            scalar = popFloat();
        }
        if (op == Op::MUL) {
            pushVector(scalar * v.x, scalar * v.y, scalar * v.z);
        } else if (op == Op::DIV) {
            if (scalar == 0.0) {
                divideByZero();
            }
            // 00437a90 multiplies by the single-precision inverse.
            const double inverse = single(1.0 / scalar);
            pushVector(inverse * v.x, inverse * v.y, inverse * v.z);
        }
        return next;
    }
    default:
        badForm();
    }
}

Functor Vm::takeSavedState() const
{
    // 005d4380 and 005d28f0: the top `stackBytes` of the stack over the `baseBytes` under
    // BP, with BP moved to sit between them. Script offsets keep their meaning.
    const SavedState& state = *savedState_;
    const std::int64_t size = static_cast<std::int64_t>(stack_.size());
    std::int64_t top = static_cast<std::int64_t>(state.stackBytes) >> 2;
    std::int64_t base = static_cast<std::int64_t>(state.baseBytes) >> 2;
    if (top == 0 && base == 0) {
        base = bp_;
        top = size - base;
    }
    base = std::clamp<std::int64_t>(base, 0, std::min<std::int64_t>(0x10000, bp_));
    top = std::clamp<std::int64_t>(top, 0, std::min<std::int64_t>(0x10000, size));
    Functor functor;
    functor.program = program_;
    const auto entry = program_->byOffset.find(state.resumeOffset);
    functor.ip = entry != program_->byOffset.end() ? entry->second : instructions_->size();
    functor.stack.reserve(static_cast<std::size_t>(base + top));
    functor.stack.insert(functor.stack.end(), stack_.begin() + (bp_ - base), stack_.begin() + bp_);
    functor.stack.insert(functor.stack.end(), stack_.end() - top, stack_.end());
    functor.bp = base;
    functor.script = scriptName_;
    return functor;
}

void Vm::setEntry(std::size_t ip, const std::vector<Value>& stack, std::int64_t bp)
{
    ip_ = std::min(ip, instructions_->size());
    // Room to run in, so the first pushes do not move the whole snapshot again.
    stack_.reserve(stack.size() + 64);
    stack_.assign(stack.begin(), stack.end());
    bp_ = bp;
    maxStack_ = stack_.size();
    status_ = VmStatus::Running;
}

Functor Vm::captureContinuation(const std::string& source) const
{
    Functor functor;
    functor.program = program_;
    functor.ip = std::min(ip_ + 1u, instructions_->size());
    functor.stack = stack_;
    functor.bp = bp_;
    functor.source = source;
    functor.script = scriptName_;
    return functor;
}

std::uint64_t Vm::coverageKey(const std::string& script, std::uint32_t offset)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char ch : script) {
        hash = (hash ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)))) * 1099511628211ULL;
    }
    return hash ^ (static_cast<std::uint64_t>(offset) * 0x9E3779B97F4A7C15ULL);
}

VmResult Vm::run()
{
    // 005d4270: the run holds a return-stack entry of its own, and gives it all back at the end.
    VmCounters& shared = counters();
    const int returnDepth = shared.returnDepth;
    ++shared.returnDepth;
    try {
        const std::vector<Instruction>& code = *instructions_;
        while (true) {
            if (ip_ >= code.size()) {
                throw VmError("ran past the end of the code"); // retail -0x6b
            }
            // 005d2bd0: past the limit the run fails; the counter spans enclosing runs.
            if (shared.steps >= budget_) {
                status_ = VmStatus::Budget;
                break;
            }
            const Instruction& ins = code[ip_];
            errorIp_ = static_cast<std::int32_t>(ins.offset);
            ++steps_;
            ++totalSteps;
            ++shared.steps;
            if (traceEnabled_) {
                trace_.push_back(ins.offset);
            }
            if (coverage != nullptr) {
                coverage->insert(coverageKey(scriptName_, ins.offset));
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
    shared.returnDepth = returnDepth;
    if (status_ == VmStatus::Running) {
        status_ = VmStatus::Returned;
    }
    // A failed run takes nothing back but its stack (005d4270).
    if (status_ == VmStatus::Error || status_ == VmStatus::Budget) {
        stack_.clear();
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
    // 005d45d0: a value comes back only from a clean run ending with exactly one int cell.
    if (status_ == VmStatus::Returned && stack_.size() == 1 && stack_.back().type == ValueType::Int) {
        result.returned = static_cast<std::int32_t>(stack_.back().integer);
    }
    return result;
}

} // namespace ke::script
