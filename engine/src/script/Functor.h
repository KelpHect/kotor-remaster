#pragma once

// A captured NWScript action closure. KotOR compiles an action argument
// (DelayCommand/AssignCommand) as STORESTATE followed by a JMP over the
// closure body. The captured state is the instruction stream, the entry
// instruction and the stack/BP snapshot the closure must resume with.

#include "script/Ncs.h"
#include "script/ScriptTypes.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ke::script {

struct Functor {
    std::int64_t id = -1;
    std::vector<Instruction> instructions;
    std::size_t ip = 0;
    std::vector<Value> stack;
    std::int64_t bp = 0;
    std::map<std::string, std::uint32_t> context;
    std::string source;
};

} // namespace ke::script
