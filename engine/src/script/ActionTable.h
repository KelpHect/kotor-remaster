#pragma once

#include "script/ScriptTypes.h"

#include <cstdint>
#include <span>

namespace ke::script {

// Generated from the verified 772-action spec. Ids are contiguous 0..771.
const ActionSpec& actionSpec(std::uint16_t id);
std::span<const ActionSpec> actionTable();
const ActionSpec* findAction(std::uint16_t id);

} // namespace ke::script
