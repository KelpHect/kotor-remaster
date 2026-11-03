#include "script/ActionTable.h"

#include <array>

namespace ke::script {

namespace {

constexpr ActionSpec makeSpec(std::uint16_t id, const char* name, ReturnKind ret,
                              std::uint8_t count, ParamKind p0, ParamKind p1, ParamKind p2,
                              ParamKind p3, ParamKind p4, ParamKind p5, ParamKind p6,
                              ParamKind p7, ParamKind p8, ParamKind p9, ParamKind p10,
                              ParamKind p11)
{
    return ActionSpec{id, name, ret, count, {p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11}};
}

#define ACTION(id, name, ret, count, p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11) \
    makeSpec(id, name, ReturnKind::ret, count, ParamKind::p0, ParamKind::p1, ParamKind::p2, \
             ParamKind::p3, ParamKind::p4, ParamKind::p5, ParamKind::p6, ParamKind::p7, \
             ParamKind::p8, ParamKind::p9, ParamKind::p10, ParamKind::p11),

constexpr std::array<ActionSpec, 772> kActions{{
#include "script/ActionTable.inc"
}};

#undef ACTION

} // namespace

const ActionSpec& actionSpec(std::uint16_t id)
{
    return kActions[id < kActions.size() ? id : 0];
}

std::span<const ActionSpec> actionTable()
{
    return kActions;
}

const ActionSpec* findAction(std::uint16_t id)
{
    return id < kActions.size() ? &kActions[id] : nullptr;
}

int parameterCells(ParamKind kind)
{
    switch (kind) {
    case ParamKind::Vector: return 3;
    case ParamKind::Action: return 0;
    default: return 1;
    }
}

void appendDefault(std::vector<Value>& out, ReturnKind kind)
{
    switch (kind) {
    case ReturnKind::Void:
        break;
    case ReturnKind::Int:
        out.push_back(Value::makeInt(0));
        break;
    case ReturnKind::Float:
        out.push_back(Value::makeFloat(0.0));
        break;
    case ReturnKind::String:
        out.push_back(Value::makeString(""));
        break;
    case ReturnKind::Object:
        out.push_back(Value::makeObject(kObjectInvalid));
        break;
    case ReturnKind::Vector:
        out.push_back(Value::makeFloat(0.0));
        out.push_back(Value::makeFloat(0.0));
        out.push_back(Value::makeFloat(0.0));
        break;
    case ReturnKind::Engine:
        out.push_back(Value::makeEngine(0));
        break;
    }
}

} // namespace ke::script
