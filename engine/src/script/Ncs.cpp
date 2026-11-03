#include "script/Ncs.h"

#include <algorithm>
#include <cstring>

namespace ke::script {

namespace {

constexpr std::size_t kHeaderSize = 13;
constexpr std::uint8_t kScriptSizeOp = 0x42;

std::uint16_t be16(const std::uint8_t* data, std::size_t size, std::size_t at, const char* what)
{
    if (at + 2 > size) {
        throw NcsError(std::string("truncated ") + what);
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[at]) << 8) | data[at + 1]);
}

std::uint32_t be32(const std::uint8_t* data, std::size_t size, std::size_t at, const char* what)
{
    if (at + 4 > size) {
        throw NcsError(std::string("truncated ") + what);
    }
    return (static_cast<std::uint32_t>(data[at]) << 24)
        | (static_cast<std::uint32_t>(data[at + 1]) << 16)
        | (static_cast<std::uint32_t>(data[at + 2]) << 8)
        | static_cast<std::uint32_t>(data[at + 3]);
}

float beFloat(const std::uint8_t* data, std::size_t size, std::size_t at, const char* what)
{
    const std::uint32_t bits = be32(data, size, at, what);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

const char* opcodeName(Op op)
{
    switch (op) {
    case Op::CPDOWNSP: return "CPDOWNSP";
    case Op::RSADD: return "RSADD";
    case Op::CPTOPSP: return "CPTOPSP";
    case Op::CONST: return "CONST";
    case Op::ACTION: return "ACTION";
    case Op::LOGAND: return "LOGAND";
    case Op::LOGOR: return "LOGOR";
    case Op::INCOR: return "INCOR";
    case Op::EXCOR: return "EXCOR";
    case Op::BOOLAND: return "BOOLAND";
    case Op::EQUAL: return "EQUAL";
    case Op::NEQUAL: return "NEQUAL";
    case Op::GEQ: return "GEQ";
    case Op::GT: return "GT";
    case Op::LT: return "LT";
    case Op::LEQ: return "LEQ";
    case Op::SHLEFT: return "SHLEFT";
    case Op::SHRIGHT: return "SHRIGHT";
    case Op::USHRIGHT: return "USHRIGHT";
    case Op::ADD: return "ADD";
    case Op::SUB: return "SUB";
    case Op::MUL: return "MUL";
    case Op::DIV: return "DIV";
    case Op::MOD: return "MOD";
    case Op::NEG: return "NEG";
    case Op::COMP: return "COMP";
    case Op::MOVSP: return "MOVSP";
    case Op::STORESTATEALL: return "STORESTATEALL";
    case Op::JMP: return "JMP";
    case Op::JSR: return "JSR";
    case Op::JZ: return "JZ";
    case Op::RETN: return "RETN";
    case Op::DESTRUCT: return "DESTRUCT";
    case Op::NOT: return "NOT";
    case Op::DECSP: return "DECSP";
    case Op::INCSP: return "INCSP";
    case Op::JNZ: return "JNZ";
    case Op::CPDOWNBP: return "CPDOWNBP";
    case Op::CPTOPBP: return "CPTOPBP";
    case Op::DECBP: return "DECBP";
    case Op::INCBP: return "INCBP";
    case Op::SAVEBP: return "SAVEBP";
    case Op::RESTOREBP: return "RESTOREBP";
    case Op::STORESTATE: return "STORESTATE";
    case Op::NOP: return "NOP";
    case Op::SCRIPTSIZE: return "SCRIPTSIZE";
    }
    return "UNKNOWN";
}

std::int32_t Instruction::branchTarget() const
{
    switch (op) {
    case Op::JMP:
    case Op::JSR:
    case Op::JZ:
    case Op::JNZ:
    case Op::MOVSP:
    case Op::DECSP:
    case Op::INCSP:
    case Op::DECBP:
    case Op::INCBP:
        return static_cast<std::int32_t>(offset) + operand0;
    case Op::STORESTATE:
        return qualifier == 0 ? -1 : static_cast<std::int32_t>(offset) + qualifier;
    default:
        return -1;
    }
}

std::vector<Instruction> decodeNcs(const std::vector<std::uint8_t>& data)
{
    return decodeNcs(data.data(), data.size());
}

std::vector<Instruction> decodeNcs(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size < kHeaderSize) {
        throw NcsError("bad NCS header: shorter than 13 bytes");
    }
    static constexpr std::uint8_t magic[8] = {'N', 'C', 'S', ' ', 'V', '1', '.', '0'};
    if (std::memcmp(data, magic, sizeof(magic)) != 0) {
        throw NcsError("bad NCS magic");
    }
    if (data[8] != kScriptSizeOp) {
        throw NcsError("missing 0x42 script-size op");
    }
    if (be32(data, size, 9, "declared size") != size) {
        throw NcsError("declared size mismatch");
    }

    std::vector<Instruction> out;
    out.reserve(size / 4);
    std::size_t p = kHeaderSize;
    while (p < size) {
        const std::size_t start = p;
        const std::uint8_t raw = data[p];
        if (raw == 0 || raw > 0x2D) {
            throw NcsError("unknown opcode 0x" + std::to_string(raw) + " at 0x"
                           + std::to_string(start));
        }
        Instruction ins;
        ins.offset = static_cast<std::uint32_t>(start);
        ins.opcode = raw;
        ins.op = static_cast<Op>(raw);
        ++p;

        const bool qualifierShape = true;
        if (qualifierShape) {
            if (p >= size) {
                throw NcsError("truncated qualifier at 0x" + std::to_string(start));
            }
            ins.hasQualifier = true;
            ins.qualifier = data[p];
            ++p;
        }

        switch (ins.op) {
        case Op::CPDOWNSP:
        case Op::CPTOPSP:
        case Op::CPDOWNBP:
        case Op::CPTOPBP:
            ins.operand0 = static_cast<std::int32_t>(be32(data, size, p, "copy offset"));
            ins.u0 = be16(data, size, p + 4, "copy size");
            p += 6;
            break;
        case Op::CONST:
            switch (ins.qualifier) {
            case 0x03:
            case 0x06:
                ins.operand0 = static_cast<std::int32_t>(be32(data, size, p, "const int"));
                p += 4;
                break;
            case 0x04:
                ins.f0 = beFloat(data, size, p, "const float");
                p += 4;
                break;
            case 0x05: {
                const std::uint16_t length = be16(data, size, p, "const string length");
                if (p + 2 + length > size) {
                    throw NcsError("string overrun at 0x" + std::to_string(start));
                }
                ins.bytes.assign(data + p + 2, data + p + 2 + length);
                p += 2 + length;
                break;
            }
            default:
                throw NcsError("bad CONST type 0x" + std::to_string(ins.qualifier)
                               + " at 0x" + std::to_string(start));
            }
            break;
        case Op::ACTION:
            if (p + 3 > size) {
                throw NcsError("truncated ACTION operand");
            }
            ins.actionId = be16(data, size, p, "action id");
            ins.actionArgc = data[p + 2];
            p += 3;
            break;
        case Op::EQUAL:
        case Op::NEQUAL:
            if (ins.qualifier == 0x24) {
                ins.u0 = be16(data, size, p, "equality size");
                p += 2;
            }
            break;
        case Op::JMP:
        case Op::JSR:
        case Op::JZ:
        case Op::JNZ:
        case Op::MOVSP:
        case Op::DECSP:
        case Op::INCSP:
        case Op::DECBP:
        case Op::INCBP:
            ins.operand0 = static_cast<std::int32_t>(be32(data, size, p, "jump offset"));
            p += 4;
            break;
        case Op::DESTRUCT:
            ins.u0 = be16(data, size, p, "destruct size");
            ins.u1 = be16(data, size, p + 2, "destruct offset");
            ins.u2 = be16(data, size, p + 4, "destruct keep");
            p += 6;
            break;
        case Op::STORESTATE:
            ins.operand0 = static_cast<std::int32_t>(be32(data, size, p, "storestate var"));
            ins.operand1 = static_cast<std::int32_t>(be32(data, size, p + 4, "storestate size"));
            p += 8;
            break;
        default:
            break;
        }
        if (p > size) {
            throw NcsError("truncated operand at 0x" + std::to_string(start));
        }
        out.push_back(std::move(ins));
    }
    return out;
}

std::vector<InvalidJump> validateControlFlow(const std::vector<Instruction>& instructions)
{
    std::vector<std::uint32_t> offsets;
    offsets.reserve(instructions.size());
    for (const Instruction& ins : instructions) {
        offsets.push_back(ins.offset);
    }
    auto present = [&offsets](std::int32_t target) {
        return target >= 0
            && std::binary_search(offsets.begin(), offsets.end(), static_cast<std::uint32_t>(target));
    };

    std::vector<InvalidJump> bad;
    for (const Instruction& ins : instructions) {
        switch (ins.op) {
        case Op::JMP:
        case Op::JSR:
        case Op::JZ:
        case Op::JNZ: {
            const std::int32_t target = static_cast<std::int32_t>(ins.offset) + ins.operand0;
            if (!present(target)) {
                bad.push_back({"jump", ins.offset, target});
            }
            break;
        }
        case Op::STORESTATE:
            if (ins.qualifier != 0) {
                const std::int32_t target = static_cast<std::int32_t>(ins.offset) + ins.qualifier;
                if (!present(target)) {
                    bad.push_back({"storestate", ins.offset, target});
                }
            }
            break;
        default:
            break;
        }
    }
    return bad;
}

} // namespace ke::script
