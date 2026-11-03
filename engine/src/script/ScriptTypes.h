#pragma once

// Typed values for the KotOR NCS virtual machine. The wire format stores
// 4-byte cells; a value keeps its NWScript type and its payload separately so
// action calls and diagnostics can report both. Numeric payloads use 64-bit
// storage to reproduce the oracle's integer/float behavior without silent
// narrowing; object ids stay 32-bit.

#include <cstdint>
#include <string>
#include <vector>

namespace ke::script {

constexpr std::uint32_t kObjectInvalid = 0x7F000000u;

enum class ValueType : std::uint8_t { Int, Float, String, Object, Engine, Functor };

struct Value {
    ValueType type = ValueType::Int;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;

    static Value makeInt(std::int64_t value) { Value v; v.type = ValueType::Int; v.integer = value; return v; }
    static Value makeFloat(double value) { Value v; v.type = ValueType::Float; v.real = value; return v; }
    static Value makeString(std::string value) { Value v; v.type = ValueType::String; v.text = std::move(value); return v; }
    static Value makeObject(std::uint32_t value) { Value v; v.type = ValueType::Object; v.integer = value; return v; }
    static Value makeEngine(std::int64_t tag) { Value v; v.type = ValueType::Engine; v.integer = tag; return v; }
    static Value makeFunctor(std::int64_t id) { Value v; v.type = ValueType::Functor; v.integer = id; return v; }

    std::int64_t asInt() const
    {
        if (type == ValueType::Float) {
            return static_cast<std::int64_t>(real);
        }
        if (type == ValueType::String) {
            return 0;
        }
        return integer;
    }

    double asFloat() const
    {
        if (type == ValueType::Float) {
            return real;
        }
        if (type == ValueType::String) {
            return 0.0;
        }
        return static_cast<double>(integer);
    }

    const std::string& asString() const
    {
        static const std::string empty;
        return type == ValueType::String ? text : empty;
    }

    bool truthy() const
    {
        switch (type) {
        case ValueType::String:
            return !text.empty();
        case ValueType::Float:
            return real != 0.0;
        case ValueType::Object:
            return integer != 0 && static_cast<std::uint32_t>(integer) != kObjectInvalid;
        default:
            return integer != 0;
        }
    }

    const char* typeName() const
    {
        switch (type) {
        case ValueType::Int: return "int";
        case ValueType::Float: return "float";
        case ValueType::String: return "string";
        case ValueType::Object: return "object";
        case ValueType::Engine: return "engine";
        case ValueType::Functor: return "functor";
        }
        return "unknown";
    }
};

// Column/return kinds used by the generated action table.
enum class ParamKind : std::uint8_t { None, Int, Float, String, Object, Vector, Engine, Action };
enum class ReturnKind : std::uint8_t { Void, Int, Float, String, Object, Vector, Engine };

constexpr int kMaxActionParams = 12;

struct ActionSpec {
    std::uint16_t id = 0;
    const char* name = "";
    ReturnKind returnKind = ReturnKind::Void;
    std::uint8_t paramCount = 0;
    ParamKind params[kMaxActionParams]{};
};

// Number of stack cells a declared parameter consumes. Vectors are three
// floats; action/functor parameters are not stack cells at all.
int parameterCells(ParamKind kind);

// Typed default cells for a return kind, matching the verified VM convention.
void appendDefault(std::vector<Value>& out, ReturnKind kind);

} // namespace ke::script
