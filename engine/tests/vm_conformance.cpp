// NCS VM conformance gate. Each case is a small program assembled here, run on the engine's VM,
// and compared with what the retail interpreter does. The address on each case is the retail code
// that decides the expected result; 005d2bd0 is the interpreter itself. No install is needed.
// `--negative` perturbs every expected result and passes only if every case then fails, which
// proves that the comparison can fail.

#include "script/ActionRuntime.h"
#include "script/Ncs.h"
#include "script/Vm.h"
#include "script/World.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace ke::script;

namespace {

class Code {
public:
    Code& raw(std::uint8_t op, std::uint8_t q = 0)
    {
        bytes_.push_back(op);
        bytes_.push_back(q);
        return *this;
    }
    Code& ci(std::int32_t v) { raw(0x04, 0x03); return be32(static_cast<std::uint32_t>(v)); }
    Code& cf(float v)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        raw(0x04, 0x04);
        return be32(bits);
    }
    Code& cs(const std::string& text)
    {
        raw(0x04, 0x05);
        be16(static_cast<std::uint16_t>(text.size()));
        bytes_.insert(bytes_.end(), text.begin(), text.end());
        return *this;
    }
    Code& co(std::uint32_t v) { raw(0x04, 0x06); return be32(v); }
    Code& withOffset(std::uint8_t op, std::int32_t offset, std::uint8_t q = 0)
    {
        raw(op, q);
        return be32(static_cast<std::uint32_t>(offset));
    }
    Code& copy(std::uint8_t op, std::int32_t offset, std::uint16_t size)
    {
        raw(op, 0x01);
        be32(static_cast<std::uint32_t>(offset));
        return be16(size);
    }
    Code& movsp(std::int32_t bytes) { return withOffset(0x1B, bytes); }
    Code& jmp(std::int32_t delta) { return withOffset(0x1D, delta); }
    Code& jsr(std::int32_t delta) { return withOffset(0x1E, delta); }
    Code& jz(std::int32_t delta) { return withOffset(0x1F, delta); }
    Code& jnz(std::int32_t delta) { return withOffset(0x25, delta); }
    Code& retn() { return raw(0x20); }
    Code& equalStruct(std::uint8_t op, std::uint16_t size)
    {
        raw(op, 0x24);
        return be16(size);
    }
    Code& destruct(std::uint16_t size, std::uint16_t offset, std::uint16_t keep)
    {
        raw(0x21, 0x01);
        be16(size);
        be16(offset);
        return be16(keep);
    }
    Code& action(std::uint16_t id, std::uint8_t argc)
    {
        raw(0x05);
        be16(id);
        bytes_.push_back(argc);
        return *this;
    }
    Code& storeState(std::uint8_t resume, std::int32_t baseBytes, std::int32_t topBytes)
    {
        raw(0x2C, resume);
        be32(static_cast<std::uint32_t>(baseBytes));
        return be32(static_cast<std::uint32_t>(topBytes));
    }
    std::vector<std::uint8_t> file() const
    {
        std::vector<std::uint8_t> out{'N', 'C', 'S', ' ', 'V', '1', '.', '0', 0x42};
        const std::uint32_t size = static_cast<std::uint32_t>(13 + bytes_.size());
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), bytes_.begin(), bytes_.end());
        return out;
    }

private:
    Code& be16(std::uint16_t v)
    {
        bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
        bytes_.push_back(static_cast<std::uint8_t>(v));
        return *this;
    }
    Code& be32(std::uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(v >> shift));
        }
        return *this;
    }
    std::vector<std::uint8_t> bytes_;
};

// Opcodes.
constexpr std::uint8_t CPDOWNSP = 0x01, RSADD = 0x02, CPTOPSP = 0x03, LOGAND = 0x06, LOGOR = 0x07, INCOR = 0x08,
                       EXCOR = 0x09, BOOLAND = 0x0A, EQUAL = 0x0B, NEQUAL = 0x0C, GEQ = 0x0D, GT = 0x0E, LT = 0x0F,
                       LEQ = 0x10, SHLEFT = 0x11, SHRIGHT = 0x12, USHRIGHT = 0x13, ADD = 0x14, SUB = 0x15, MUL = 0x16,
                       DIV = 0x17, MOD = 0x18, NEG = 0x19, COMP = 0x1A, STORESTATEALL = 0x1C, NOT = 0x22, DECSP = 0x23,
                       INCSP = 0x24, CPDOWNBP = 0x26, CPTOPBP = 0x27, DECBP = 0x28, INCBP = 0x29, SAVEBP = 0x2A,
                       RESTOREBP = 0x2B, NOP = 0x2D;
// Type forms.
constexpr std::uint8_t II = 0x20, FF = 0x21, SS = 0x23, IF = 0x25, FI = 0x26, VV = 0x3A, VF = 0x3B, FV = 0x3C;

std::string cellText(const Value& cell)
{
    char buffer[64];
    switch (cell.type) {
    case ValueType::Int:
        std::snprintf(buffer, sizeof(buffer), "i:%lld", static_cast<long long>(cell.integer));
        return buffer;
    case ValueType::Float:
        std::snprintf(buffer, sizeof(buffer), "f:%.9g", cell.real);
        return buffer;
    case ValueType::String: return "s:" + cell.text.str();
    case ValueType::Object:
        std::snprintf(buffer, sizeof(buffer), "o:%llx", static_cast<unsigned long long>(cell.integer));
        return buffer;
    case ValueType::Engine: return "e";
    case ValueType::Functor: return "fn";
    }
    return "?";
}

std::string stackText(const std::vector<Value>& stack)
{
    std::string out;
    for (const Value& cell : stack) {
        if (!out.empty()) {
            out += ' ';
        }
        out += cellText(cell);
    }
    return out;
}

// What the run produced, in one comparable line.
std::string outcomeText(const VmResult& result, const std::vector<Value>& stack)
{
    std::string out = statusName(result.status);
    if (result.status == VmStatus::Error) {
        out += "(" + result.error + ")";
    }
    out += " [" + stackText(stack) + "]";
    return out;
}

std::string ok(const std::string& stack) { return "returned [" + stack + "]"; }
std::string fails(const std::string& why) { return "error(" + why + ") []"; }

const std::string kBadForm = "bad operand form";
const std::string kBadStack = "stack or type check failed";
const std::string kDivide = "divide by zero";
const std::string kNoOpcode = "unknown opcode";
const std::string kRecursion = "recursion limit 128 exceeded";

struct Case {
    const char* name;
    const char* retail;
    Code code;
    std::string expected;
};

// Binary operator on two constants.
Code ii(std::uint8_t op, std::int32_t a, std::int32_t b, std::uint8_t form = II)
{
    Code c;
    c.ci(a).ci(b).raw(op, form).retn();
    return c;
}
Code ff(std::uint8_t op, float a, float b)
{
    Code c;
    c.cf(a).cf(b).raw(op, FF).retn();
    return c;
}
Code ss(std::uint8_t op, const std::string& a, const std::string& b, std::uint8_t form = SS)
{
    Code c;
    c.cs(a).cs(b).raw(op, form).retn();
    return c;
}
Code vec(Code c, float x, float y, float z)
{
    c.cf(x).cf(y).cf(z);
    return c;
}

constexpr std::int32_t kIntMax = std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t kIntMin = std::numeric_limits<std::int32_t>::min();

std::vector<Case> buildCases()
{
    std::vector<Case> cases;
    const auto add = [&](const char* name, const char* retail, Code code, std::string expected) {
        cases.push_back(Case{name, retail, std::move(code), std::move(expected)});
    };

    // Integer arithmetic: 32-bit cells that wrap.
    add("int add", "005d2bd0", ii(ADD, 2, 3), ok("i:5"));
    add("int add wraps", "005d2bd0", ii(ADD, kIntMax, 1), ok("i:-2147483648"));
    add("int sub wraps", "005d2bd0", ii(SUB, kIntMin, 1), ok("i:2147483647"));
    add("int mul wraps", "005d2bd0", ii(MUL, 65536, 65536), ok("i:0"));
    add("int div truncates", "005d2bd0", ii(DIV, -7, 2), ok("i:-3"));
    add("int mod takes the dividend's sign", "005d2bd0", ii(MOD, -7, 2), ok("i:-1"));
    add("int div by zero fails", "005d2bd0 -0x69", ii(DIV, 1, 0), fails(kDivide));
    add("int mod by zero fails", "005d2bd0 -0x69", ii(MOD, 1, 0), fails(kDivide));
    add("INT_MIN / -1 ends the run", "005d2bd0 idiv", ii(DIV, kIntMin, -1), fails("integer divide overflow"));
    add("INT_MIN % -1 ends the run", "005d2bd0 idiv", ii(MOD, kIntMin, -1), fails("integer divide overflow"));
    add("mod has no float form", "005d2bd0", ff(MOD, 5.0f, 2.0f), fails(kBadForm));

    // Float arithmetic: single precision.
    add("float add is single precision", "005d2bd0", ff(ADD, 0.1f, 0.2f), ok("f:0.300000012"));
    add("float sub", "005d2bd0", ff(SUB, 1.5f, 0.25f), ok("f:1.25"));
    add("float mul", "005d2bd0", ff(MUL, 1.5f, 2.0f), ok("f:3"));
    add("float div", "005d2bd0", ff(DIV, 1.0f, 3.0f), ok("f:0.333333343"));
    add("float div by zero fails", "005d2bd0 -0x69", ff(DIV, 1.0f, 0.0f), fails(kDivide));
    {
        Code c;
        c.ci(3).cf(0.5f).raw(ADD, IF).retn();
        add("int + float", "005d2bd0 form 0x25", c, ok("f:3.5"));
    }
    {
        Code c;
        c.cf(0.5f).ci(3).raw(SUB, FI).retn();
        add("float - int", "005d2bd0 form 0x26", c, ok("f:-2.5"));
    }
    {
        Code c;
        c.ci(1).cf(0.0f).raw(DIV, IF).retn();
        add("int / float zero fails", "005d2bd0 -0x69", c, fails(kDivide));
    }

    // Typed pops: the wrong tag fails the run.
    {
        Code c;
        c.cf(1.0f).ci(1).raw(ADD, II).retn();
        add("int add over a float cell", "005d24c0", c, fails(kBadStack));
    }
    {
        Code c;
        c.ci(1).ci(1).raw(ADD, FF).retn();
        add("float add over int cells", "005d2510", c, fails(kBadStack));
    }
    {
        Code c;
        c.ci(1).cs("a").raw(ADD, SS).retn();
        add("string add over an int cell", "005d25e0", c, fails(kBadStack));
    }
    {
        Code c;
        c.ci(1).raw(ADD, II).retn();
        add("add on one cell", "005d24c0", c, fails(kBadStack));
    }
    add("arithmetic on no known form", "005d2bd0 -0x61", ii(ADD, 1, 1, 0x30), fails(kBadForm));

    // Strings.
    add("string add concatenates left to right", "005d3820", ss(ADD, "ab", "cd"), ok("s:abcd"));
    add("string sub pops and pushes nothing", "005d2bd0", ss(SUB, "a", "b"), ok(""));
    add("string mul pops and pushes nothing", "005d2bd0", ss(MUL, "a", "b"), ok(""));
    add("string div pops and pushes nothing", "005d2bd0", ss(DIV, "a", "b"), ok(""));
    {
        Code c;
        c.cs(std::string("ab\0cd", 5)).retn();
        add("string constant ends at NUL", "005e5680", c, ok("s:ab"));
    }

    // Vectors.
    add("vector + vector", "005d2bd0 form 0x3a",
        [] { Code c = vec(vec(Code{}, 1, 2, 3), 10, 20, 30); c.raw(ADD, VV).retn(); return c; }(),
        ok("f:11 f:22 f:33"));
    add("vector - vector", "005d2bd0 form 0x3a",
        [] { Code c = vec(vec(Code{}, 1, 2, 3), 10, 20, 30); c.raw(SUB, VV).retn(); return c; }(),
        ok("f:-9 f:-18 f:-27"));
    add("vector * vector pops and pushes nothing", "005d2bd0 form 0x3a",
        [] { Code c = vec(vec(Code{}, 1, 2, 3), 1, 2, 3); c.raw(MUL, VV).retn(); return c; }(), ok(""));
    add("vector / vector pops and pushes nothing", "005d2bd0 form 0x3a",
        [] { Code c = vec(vec(Code{}, 1, 2, 3), 1, 2, 3); c.raw(DIV, VV).retn(); return c; }(), ok(""));
    add("vector * float", "00423bf0",
        [] { Code c = vec(Code{}, 1, 2, 3); c.cf(2.0f).raw(MUL, VF).retn(); return c; }(), ok("f:2 f:4 f:6"));
    add("float * vector", "00423bf0",
        [] { Code c; c.cf(2.0f); c = vec(c, 1, 2, 3); c.raw(MUL, FV).retn(); return c; }(), ok("f:2 f:4 f:6"));
    add("vector / float multiplies by the single inverse", "00437a90",
        [] { Code c = vec(Code{}, 1, 3, 7); c.cf(3.0f).raw(DIV, VF).retn(); return c; }(),
        ok("f:0.333333343 f:1 f:2.33333349"));
    add("vector / zero fails", "005d2bd0 -0x69",
        [] { Code c = vec(Code{}, 1, 2, 3); c.cf(0.0f).raw(DIV, VF).retn(); return c; }(), fails(kDivide));
    add("vector + float pops and pushes nothing", "005d2bd0 form 0x3b",
        [] { Code c = vec(Code{}, 1, 2, 3); c.cf(2.0f).raw(ADD, VF).retn(); return c; }(), ok(""));

    // Equality: the right cell's tag picks the rule.
    add("int equal", "005d2bd0", ii(EQUAL, 5, 5), ok("i:1"));
    add("int not equal", "005d2bd0", ii(NEQUAL, 5, 5), ok("i:0"));
    add("float equal by bits", "005d2bd0", ff(EQUAL, 1.5f, 1.5f), ok("i:1"));
    add("negative zero differs by bits", "005d2bd0", ff(EQUAL, -0.0f, 0.0f), ok("i:0"));
    add("string equal ignores ASCII case", "005e6450", ss(EQUAL, "AbC", "aBc", 0x23), ok("i:1"));
    add("string length counts", "005e6450", ss(EQUAL, "abc", "abcd", 0x23), ok("i:0"));
    add("empty string equals empty", "005e5a90", ss(EQUAL, "", "", 0x23), ok("i:1"));
    add("string not equal", "005e6450", ss(NEQUAL, "a", "b", 0x23), ok("i:1"));
    {
        Code c;
        c.co(7).co(7).raw(EQUAL, 0x22).retn();
        add("object equal", "005d2bd0", c, ok("i:1"));
    }
    {
        Code c;
        c.raw(RSADD, 0x10).raw(RSADD, 0x10).raw(EQUAL, 0x30).retn();
        add("engine values compare through the implementer", "0052c510", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(1).cf(2.0f).ci(1).cf(2.0f).equalStruct(EQUAL, 8).retn();
        add("struct equal over its cells", "005d2bd0 form 0x24", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(1).cf(2.0f).ci(1).cf(3.0f).equalStruct(EQUAL, 8).retn();
        add("struct equal fails on any cell", "005d2bd0 form 0x24", c, ok("i:0"));
    }
    {
        Code c;
        c.ci(1).cf(2.0f).ci(1).cf(3.0f).equalStruct(NEQUAL, 8).retn();
        add("struct not equal", "005d2bd0 form 0x24", c, ok("i:1"));
    }
    add("equal has no 0x25 form", "005d2bd0 -0x61", ii(EQUAL, 1, 1, 0x25), fails(kBadForm));

    // Ordering: int and float only.
    add("geq int", "005d2bd0", ii(GEQ, 3, 3), ok("i:1"));
    add("gt int", "005d2bd0", ii(GT, 3, 3), ok("i:0"));
    add("lt int", "005d2bd0", ii(LT, 2, 3), ok("i:1"));
    add("leq int", "005d2bd0", ii(LEQ, 4, 3), ok("i:0"));
    add("geq float", "005d2bd0", ff(GEQ, 3.0f, 2.5f), ok("i:1"));
    add("gt float", "005d2bd0", ff(GT, 3.0f, 2.5f), ok("i:1"));
    add("lt float", "005d2bd0", ff(LT, 3.0f, 2.5f), ok("i:0"));
    add("leq float", "005d2bd0", ff(LEQ, 2.5f, 2.5f), ok("i:1"));
    add("strings do not order", "005d2bd0 -0x61", ss(GEQ, "a", "b", 0x23), fails(kBadForm));

    // Logic and bits: form 0x20 only.
    add("logical and", "005d2bd0", ii(LOGAND, 2, 4), ok("i:1"));
    add("logical and with zero", "005d2bd0", ii(LOGAND, 2, 0), ok("i:0"));
    add("logical or", "005d2bd0", ii(LOGOR, 0, 4), ok("i:1"));
    add("bit or", "005d2bd0", ii(INCOR, 5, 2), ok("i:7"));
    add("bit xor", "005d2bd0", ii(EXCOR, 5, 1), ok("i:4"));
    add("bit and", "005d2bd0", ii(BOOLAND, 6, 3), ok("i:2"));
    add("logical and has no float form", "005d2bd0 -0x61", ff(LOGAND, 1.0f, 1.0f), fails(kBadForm));
    add("shift left masks its count", "005d2bd0", ii(SHLEFT, 3, 33), ok("i:6"));
    add("shift left wraps", "005d2bd0", ii(SHLEFT, 1, 31), ok("i:-2147483648"));
    add("shift right of a negative rounds toward zero", "005d2bd0", ii(SHRIGHT, -5, 1), ok("i:-2"));
    add("shift right of a positive", "005d2bd0", ii(SHRIGHT, 5, 1), ok("i:2"));
    add("unsigned shift is arithmetic", "005d36da", ii(USHRIGHT, -5, 1), ok("i:-3"));
    {
        Code c;
        c.ci(5).raw(COMP, 0x03).retn();
        add("complement", "005d2bd0", c, ok("i:-6"));
    }
    {
        Code c;
        c.ci(kIntMin).raw(NEG, 0x03).retn();
        add("negate wraps", "005d2bd0", c, ok("i:-2147483648"));
    }
    {
        Code c;
        c.cf(1.5f).raw(NEG, 0x04).retn();
        add("negate float", "005d2bd0", c, ok("f:-1.5"));
    }
    {
        Code c;
        c.cs("a").raw(NEG, 0x05).retn();
        add("negate has no string form", "005d2bd0 -0x61", c, fails(kBadForm));
    }
    {
        Code c;
        c.ci(0).raw(NOT, 0x03).ci(7).raw(NOT, 0x03).retn();
        add("not", "005d2bd0", c, ok("i:1 i:0"));
    }
    {
        Code c;
        c.cf(0.0f).raw(NOT, 0x04).retn();
        add("not has no float form", "005d2bd0 -0x61", c, fails(kBadForm));
    }

    // Reserving and constants.
    {
        Code c;
        c.raw(RSADD, 0x03).raw(RSADD, 0x04).raw(RSADD, 0x05).raw(RSADD, 0x06).raw(RSADD, 0x10).raw(RSADD, 0x19).retn();
        add("reserve every type", "005d2bd0", c, ok("i:0 f:0 s: o:7f000000 e e"));
    }
    {
        Code c;
        c.raw(RSADD, 0x07).retn();
        add("reserve an unknown type", "005d2bd0 -0x61", c, fails(kBadForm));
    }

    // Stack moves.
    {
        Code c;
        c.ci(1).ci(2).ci(3).movsp(-8).retn();
        add("movsp drops cells", "005d21f0", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(1).movsp(8).retn();
        add("movsp ignores a positive move", "005d21f0", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(1).ci(2).copy(CPTOPSP, -8, 8).retn();
        add("cptopsp copies a block", "005d2bd0", c, ok("i:1 i:2 i:1 i:2"));
    }
    {
        Code c;
        c.ci(1).ci(2).copy(CPTOPSP, -12, 4).retn();
        add("cptopsp below the stack", "005d2bd0", c, fails("CPTOPSP outside frame"));
    }
    {
        Code c;
        c.ci(1).ci(2).ci(9).copy(CPDOWNSP, -12, 4).retn();
        add("cpdownsp writes below and keeps the top", "005d2bd0", c, ok("i:9 i:2 i:9"));
    }
    {
        Code c;
        c.ci(1).raw(SAVEBP).ci(5).copy(CPTOPBP, -4, 4).retn();
        add("cptopbp reads under BP", "005d2bd0", c, ok("i:1 i:0 i:5 i:1"));
    }
    {
        Code c;
        c.ci(1).raw(SAVEBP).ci(5).copy(CPDOWNBP, -4, 4).retn();
        add("cpdownbp writes under BP", "005d2bd0", c, ok("i:5 i:0 i:5"));
    }
    {
        Code c;
        c.ci(1).raw(SAVEBP).raw(RESTOREBP).retn();
        add("savebp pushes BP, restorebp pops it", "005d2bd0", c, ok("i:1"));
    }
    {
        Code c;
        c.raw(RESTOREBP).retn();
        add("restorebp on an empty stack", "005d24c0", c, fails("stack underflow"));
    }
    {
        Code c;
        c.ci(1).ci(2).ci(3).destruct(12, 4, 4).retn();
        add("destruct keeps the named field", "005d2bd0", c, ok("i:2"));
    }
    {
        Code c;
        c.ci(1).destruct(8, 0, 4).retn();
        add("destruct under the stack", "005d2bd0", c, fails("DESTRUCT underflow"));
    }
    {
        Code c;
        c.ci(5).withOffset(INCSP, -4, 0x03).retn();
        add("incsp", "005d16c0", c, ok("i:6"));
    }
    {
        Code c;
        c.ci(5).withOffset(DECSP, -4, 0x03).retn();
        add("decsp", "005d16c0", c, ok("i:4"));
    }
    {
        Code c;
        c.ci(kIntMax).withOffset(INCSP, -4, 0x03).retn();
        add("incsp wraps", "005d16c0", c, ok("i:-2147483648"));
    }
    {
        Code c;
        c.cf(5.0f).withOffset(INCSP, -4, 0x03).retn();
        add("incsp leaves a non-int cell alone", "005d16c0", c, ok("f:5"));
    }
    {
        Code c;
        c.ci(5).withOffset(INCSP, -4, 0x04).retn();
        add("incsp only with the int form", "005d2bd0", c, ok("i:5"));
    }
    {
        Code c;
        c.ci(5).raw(SAVEBP).withOffset(INCBP, -4, 0x03).retn();
        add("incbp", "005d16c0", c, ok("i:6 i:0"));
    }
    {
        Code c;
        c.ci(5).raw(SAVEBP).withOffset(DECBP, -4, 0x03).retn();
        add("decbp", "005d16c0", c, ok("i:4 i:0"));
    }

    // Control flow.
    {
        Code c;
        c.jmp(12).ci(9).ci(1).retn();
        add("jmp skips", "005d2bd0", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(0).jz(14).ci(9).retn().ci(1).retn();
        add("jz takes zero", "005d2bd0", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(3).jz(14).ci(9).retn().ci(1).retn();
        add("jz passes non-zero", "005d2bd0", c, ok("i:9"));
    }
    {
        Code c;
        c.ci(3).jnz(14).ci(9).retn().ci(1).retn();
        add("jnz takes non-zero", "005d2bd0", c, ok("i:1"));
    }
    {
        Code c;
        c.cf(1.0f).jz(6).retn();
        add("jz on a float cell", "005d24c0", c, fails(kBadStack));
    }
    {
        Code c;
        c.jsr(8).retn().ci(4).retn();
        add("jsr and retn", "005d17f0", c, ok("i:4"));
    }
    {
        Code c;
        c.jsr(0);
        add("return stack holds 128 with the run's own entry", "005d17f0, 005d4270", c, fails(kRecursion));
    }
    {
        Code c;
        c.ci(1).retn().ci(2);
        add("retn at the top ends the run", "005d17b0, 005d4270", c, ok("i:1"));
    }
    {
        Code c;
        c.ci(1);
        add("running off the end fails", "005d2bd0 -0x6b", c, fails("ran past the end of the code"));
    }
    {
        Code c;
        c.jmp(7);
        add("jump into no instruction", "005d2bd0", c, fails("jump target is not an instruction"));
    }
    {
        Code c;
        c.raw(NOP).retn();
        add("nop is past the dispatch bound", "005d2c45 -0x65", c, fails(kNoOpcode));
    }
    {
        Code c;
        c.ci(1).raw(STORESTATEALL, 0x10).retn();
        add("storestateall alone changes nothing", "005d2bd0", c, ok("i:1"));
    }

    // Actions: handlers pop their declared cells through the typed pops.
    {
        Code c;
        c.cs("x").action(0, 1).retn();
        add("action argument of the wrong tag", "005d24c0, 0052c0d0", c, fails(kBadStack));
    }
    {
        Code c;
        c.co(1).action(574, 1).retn();
        add("574 with only an object fails its int pop", "0052de70", c, fails(kBadStack));
    }
    {
        Code c;
        c.cs("left").ci(0).action(518, 2).retn();
        add("518 leaves the cell it does not declare", "005473e0", c, ok("s:left"));
    }
    {
        Code c;
        c.ci(7).action(0, 1).retn();
        add("action result lands on the stack", "0052c0d0", c, ok("i:0"));
    }
    return cases;
}

// A sink that keeps the closures it is given and answers actions with nothing.
class ClosureSink : public ActionSink {
public:
    std::vector<Value> call(std::uint16_t, const std::vector<std::vector<Value>>&, Vm&) override { return {}; }
    std::int64_t registerFunctor(Functor functor) override
    {
        functors.push_back(std::move(functor));
        return static_cast<std::int64_t>(functors.size()) - 1;
    }
    std::vector<Functor> functors;
};

struct Report {
    int passed = 0;
    int failed = 0;
    bool negative = false;
    void check(const char* name, const char* retail, const std::string& got, std::string want)
    {
        if (negative) {
            want += " (perturbed)";
        }
        if (got == want) {
            ++passed;
            return;
        }
        ++failed;
        if (!negative) {
            std::printf("FAIL %s (retail %s)\n  want %s\n  got  %s\n", name, retail, want.c_str(), got.c_str());
        }
    }
};

std::string runPlain(const Code& code)
{
    Vm vm(decodeNcs(code.file()));
    const VmResult result = vm.run();
    return outcomeText(result, vm.stack());
}

void runSpecial(Report& report)
{
    // The instruction limit: 0x1FFFF instructions run, then the run fails with its stack gone.
    {
        Code c;
        c.ci(1).jmp(0);
        Vm vm(decodeNcs(c.file()));
        const VmResult result = vm.run();
        report.check("instruction limit", "005d2bd0 -0x5d",
                     outcomeText(result, vm.stack()) + " steps " + std::to_string(result.steps),
                     "budget [] steps 131071");
    }
    // One counter across nested runs: the outer run's spending counts (005d45d0 resets it only
    // at depth -1).
    {
        Code c;
        c.ci(1).ci(2).retn();
        VmCounters shared;
        shared.steps = kRetailInstructionBudget - 1;
        Vm vm(decodeNcs(c.file()));
        vm.shareCounters(&shared);
        const VmResult result = vm.run();
        report.check("nested run shares the instruction counter", "005d45d0, 005d4ad0",
                     outcomeText(result, vm.stack()), "budget []");
    }
    // One return stack across nested runs.
    {
        Code c;
        c.jsr(8).retn().retn();
        VmCounters shared;
        shared.returnDepth = 126;
        Vm vm(decodeNcs(c.file()));
        vm.shareCounters(&shared);
        const VmResult result = vm.run();
        report.check("nested run shares the return stack", "005d17f0, 005d4270", outcomeText(result, vm.stack()),
                     fails(kRecursion));
        report.check("the return stack comes back after a run", "005d4270", std::to_string(shared.returnDepth), "126");
    }
    // What a caller sees as the script's return value.
    {
        const auto returned = [](Code code) {
            Vm vm(decodeNcs(code.file()));
            const std::optional<std::int32_t> value = vm.run().returned;
            return value.has_value() ? std::to_string(*value) : std::string("none");
        };
        report.check("one int cell is a return value", "005d45d0", returned(Code{}.ci(4).retn()), "4");
        report.check("no cell is no value", "005d45d0", returned(Code{}.retn()), "none");
        report.check("two cells are no value", "005d45d0", returned(Code{}.ci(1).ci(2).retn()), "none");
        report.check("a float cell is no value", "005d45d0", returned(Code{}.cf(1.0f).retn()), "none");
        report.check("a failed run has no value", "005d4270", returned(Code{}.ci(4).ci(0).ci(0).raw(DIV, II).retn()),
                     "none");
    }
    // OBJECT_SELF: object constant 0 is self when the run has one, anything else is invalid.
    {
        Code c;
        c.co(0).co(1).retn();
        Vm plain(decodeNcs(c.file()));
        plain.run();
        report.check("object constants in the corpus mode stay raw", "tools/vm oracle", stackText(plain.stack()),
                     "o:0 o:1");
        Vm live(decodeNcs(c.file()));
        live.setLiveObjectConstants(0x1234);
        live.run();
        report.check("object constant 0 is self, others invalid", "005d2bd0", stackText(live.stack()),
                     "o:1234 o:7f000000");
        Vm noSelf(decodeNcs(c.file()));
        noSelf.setLiveObjectConstants(0x1234);
        noSelf.setSelfValid(false);
        noSelf.run();
        report.check("a run without self has none", "005d2bd0, 005d45d0", stackText(noSelf.stack()),
                     "o:7f000000 o:7f000000");
    }
    // STORESTATE: the operands are only noted, and the action that takes the closure saves
    // the base cells under BP and the top cells, with BP between them.
    const auto closureOf = [](std::int32_t base, std::int32_t top) {
        Code c;
        c.ci(7).raw(SAVEBP).ci(8).storeState(16, base, top); // Offset 27; the closure at 43.
        c.jmp(16);                                           // Offset 37, past the closure.
        c.copy(CPTOPBP, -4, 4);                              // Offset 43.
        c.retn();                                            // Offset 51.
        c.cf(1.0f).action(7, 2).retn();                      // Offset 53.
        return c;
    };
    {
        ClosureSink sink;
        Vm vm(decodeNcs(closureOf(4, 4).file()), &sink);
        const VmResult result = vm.run();
        report.check("the run goes on past a closure", "005d2bd0", outcomeText(result, vm.stack()),
                     ok("i:7 i:0 i:8"));
        const Functor* functor = sink.functors.empty() ? nullptr : &sink.functors.front();
        report.check("closure keeps base cells and top cells", "005d4380, 005d28f0",
                     functor == nullptr ? "none" : stackText(functor->stack) + " bp " + std::to_string(functor->bp),
                     "i:7 i:8 bp 1");
        if (functor != nullptr) {
            Vm resumed = Vm::shared(functor->program, &sink, kRetailInstructionBudget);
            resumed.setEntry(functor->ip, functor->stack, functor->bp);
            const VmResult run = resumed.run();
            report.check("a closure resumes at STORESTATE + qualifier with its globals", "005d4ad0, 005d2bd0",
                         outcomeText(run, resumed.stack()), ok("i:7 i:8 i:7"));
        }
    }
    {
        ClosureSink sink;
        Vm vm(decodeNcs(closureOf(0, 0).file()), &sink);
        vm.run();
        report.check("closure sizes of zero keep the whole stack", "005d28f0",
                     sink.functors.empty() ? "none"
                                           : stackText(sink.functors.front().stack) + " bp "
                                                 + std::to_string(sink.functors.front().bp),
                     "i:7 i:0 i:8 bp 1");
    }
    // The header.
    {
        const auto decodes = [](std::vector<std::uint8_t> file) {
            try {
                decodeNcs(file);
                return std::string("decoded");
            } catch (const NcsError&) {
                return std::string("refused");
            }
        };
        Code c;
        c.retn();
        std::vector<std::uint8_t> file = c.file();
        file[12] = 0x7F; // Retail reads the file's own length.
        report.check("declared size is not read", "005d2260", decodes(file), "decoded");
        file = c.file();
        file[8] = 0x41;
        report.check("the byte after the version is checked", "005d2260", decodes(file), "refused");
        file = c.file();
        file[5] = '2';
        // Retail's check is version 10 from the two digits; a different one is refused.
        report.check("the version is checked", "005d2260", decodes(file), "refused");
    }
    report.check("nested run limit", "005d2260, 005d23e0", std::to_string(kRetailNestedRuns), "8");

    // The event queue behind DelayCommand and AssignCommand (World timers).
    const auto order = [](World& world, double seconds) {
        std::string out;
        for (const Timer& timer : world.advance(seconds)) {
            out += (out.empty() ? "" : " ") + std::to_string(timer.functor);
        }
        return out;
    };
    {
        World world;
        world.scheduleFunctor(1.8, 1);
        world.scheduleFunctor(1.2, 2);
        report.check("due events run in time order, not posting order", "004afdb0, 004b0b70", order(world, 2.0), "2 1");
    }
    {
        World world;
        world.scheduleFunctor(1.0009, 1);
        world.scheduleFunctor(1.0, 2);
        world.scheduleFunctor(1.0, 3);
        report.check("delays are whole milliseconds and equal times keep posting order", "0052fe30 _ftol, 004afdb0",
                     order(world, 1.5), "1 2 3");
    }
    {
        World world;
        world.scheduleFunctor(-1.0, 4);
        world.scheduleFunctor(0.0, 5);
        const std::string ran = order(world, 0.1);
        report.check("a negative delay is dropped", "004adf50, 004b08d0",
                     ran + " left " + std::to_string(world.timers.size()), "5 left 0");
    }
    {
        World world;
        world.scheduleFunctor(0.5, 6);
        world.advanceClock(1.0);
        std::string out;
        while (std::optional<Timer> timer = world.popDue()) {
            out += (out.empty() ? "" : " ") + std::to_string(timer->functor);
            if (timer->functor == 6) {
                world.scheduleFunctor(0.0, 7); // An AssignCommand inside the delayed command.
                world.scheduleFunctor(0.001, 8);
            }
        }
        report.check("a no-delay event posted while the queue runs runs in the same pass", "004b0b70",
                     out + " left " + std::to_string(world.timers.size()), "6 7 left 1");
    }

    // Conversions read as the shipped CRT does.
    const auto actionOn = [](Code code) {
        World world;
        ActionRuntime runtime(&world);
        Vm vm(decodeNcs(code.retn().file()), &runtime);
        const VmResult result = vm.run();
        return outcomeText(result, vm.stack());
    };
    report.check("StringToInt reads the leading number", "00544710 atol",
                 actionOn(Code{}.cs("12abc").action(232, 1).cs(" -7x").action(232, 1).cs("abc").action(232, 1)),
                 ok("i:12 i:-7 i:0"));
    report.check("StringToFloat reads the leading decimal number", "00544710 atof",
                 actionOn(Code{}.cs("1.5e1x").action(233, 1).cs("  .5").action(233, 1).cs("inf").action(233, 1)),
                 ok("f:15 f:0.5 f:0"));
    report.check("FloatToInt truncates and keeps the low 32 bits", "00535fb0 _ftol",
                 actionOn(Code{}.cf(-2.7f).action(231, 1).cf(3.0e9f).action(231, 1).cf(1.0e30f).action(231, 1)),
                 ok("i:-2 i:-1294967296 i:0"));
}

} // namespace

int main(int argc, char** argv)
{
    Report report;
    report.negative = argc > 1 && std::strcmp(argv[1], "--negative") == 0;
    const std::vector<Case> cases = buildCases();
    for (const Case& item : cases) {
        std::string got;
        try {
            got = runPlain(item.code);
        } catch (const std::exception& error) {
            got = std::string("threw ") + error.what();
        }
        report.check(item.name, item.retail, got, item.expected);
    }
    runSpecial(report);
    if (report.negative) {
        const bool allFailed = report.passed == 0 && report.failed > 0;
        std::printf("negative: %d of %d perturbed cases were caught\n", report.failed, report.failed + report.passed);
        if (allFailed) {
            std::printf("VM_CONFORMANCE_NEGATIVE_PASS\n");
        }
        return allFailed ? 0 : 1;
    }
    std::printf("vm conformance: %d passed, %d failed\n", report.passed, report.failed);
    return report.failed == 0 ? 0 : 1;
}
