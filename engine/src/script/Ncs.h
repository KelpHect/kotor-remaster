#pragma once

// Clean-room NCS V1.0 decoder for the shipped KotOR script corpus. The wire
// layout is the documented big-endian form (13-byte header, opcode stream);
// the opcode table is bytecode-format knowledge, not decompiled program text.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ke::script {

struct NcsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Op : std::uint8_t {
    CPDOWNSP = 0x01, RSADD = 0x02, CPTOPSP = 0x03, CONST = 0x04, ACTION = 0x05,
    LOGAND = 0x06, LOGOR = 0x07, INCOR = 0x08, EXCOR = 0x09, BOOLAND = 0x0A,
    EQUAL = 0x0B, NEQUAL = 0x0C, GEQ = 0x0D, GT = 0x0E, LT = 0x0F, LEQ = 0x10,
    SHLEFT = 0x11, SHRIGHT = 0x12, USHRIGHT = 0x13, ADD = 0x14, SUB = 0x15,
    MUL = 0x16, DIV = 0x17, MOD = 0x18, NEG = 0x19, COMP = 0x1A, MOVSP = 0x1B,
    STORESTATEALL = 0x1C, JMP = 0x1D, JSR = 0x1E, JZ = 0x1F, RETN = 0x20,
    DESTRUCT = 0x21, NOT = 0x22, DECSP = 0x23, INCSP = 0x24, JNZ = 0x25,
    CPDOWNBP = 0x26, CPTOPBP = 0x27, DECBP = 0x28, INCBP = 0x29, SAVEBP = 0x2A,
    RESTOREBP = 0x2B, STORESTATE = 0x2C, NOP = 0x2D, SCRIPTSIZE = 0x42,
};

const char* opcodeName(Op op);

struct Instruction {
    std::uint32_t offset = 0;
    std::uint8_t opcode = 0;
    Op op = Op::NOP;
    bool hasQualifier = false;
    std::uint8_t qualifier = 0;
    std::int32_t operand0 = 0;
    std::int32_t operand1 = 0;
    std::uint16_t u0 = 0;
    std::uint16_t u1 = 0;
    std::uint16_t u2 = 0;
    float f0 = 0.0f;
    std::vector<std::uint8_t> bytes;
    std::uint16_t actionId = 0;
    std::uint8_t actionArgc = 0;

    // Jump/STORESTATE target in bytes, or -1 when the instruction has none.
    std::int32_t branchTarget() const;
};

std::vector<Instruction> decodeNcs(const std::vector<std::uint8_t>& data);
std::vector<Instruction> decodeNcs(const std::uint8_t* data, std::size_t size);

struct InvalidJump {
    std::string kind;
    std::uint32_t at = 0;
    std::int32_t target = 0;
};

std::vector<InvalidJump> validateControlFlow(const std::vector<Instruction>& instructions);

} // namespace ke::script
