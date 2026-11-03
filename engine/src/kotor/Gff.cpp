#include "kotor/Formats.h"

#include <cstring>

namespace ke::kotor {

namespace {

std::uint32_t needU32(const std::vector<std::uint8_t>& data, std::size_t offset, const char* what)
{
    return readU32(data.data(), data.size(), offset, what);
}

} // namespace

Gff Gff::parse(std::vector<std::uint8_t> bytes, std::string name)
{
    if (bytes.size() < 56) {
        throw FormatError(name + ": shorter than 56-byte GFF header");
    }
    if (std::memcmp(bytes.data() + 4, "V3.2", 4) != 0) {
        throw FormatError(name + ": not a GFF V3.2 file");
    }
    Gff gff;
    gff.name_ = std::move(name);
    gff.bytes_ = std::move(bytes);
    const auto& b = gff.bytes_;
    const std::uint32_t structOff = needU32(b, 8, "GFF struct offset");
    const std::uint32_t structCount = needU32(b, 12, "GFF struct count");
    const std::uint32_t fieldOff = needU32(b, 16, "GFF field offset");
    const std::uint32_t fieldCount = needU32(b, 20, "GFF field count");
    const std::uint32_t labelOff = needU32(b, 24, "GFF label offset");
    const std::uint32_t labelCount = needU32(b, 28, "GFF label count");
    const std::uint32_t dataOff = needU32(b, 32, "GFF field data offset");
    const std::uint32_t dataLen = needU32(b, 36, "GFF field data length");
    const std::uint32_t fidxOff = needU32(b, 40, "GFF field index offset");
    const std::uint32_t fidxLen = needU32(b, 44, "GFF field index length");
    const std::uint32_t lidxOff = needU32(b, 48, "GFF list index offset");
    const std::uint32_t lidxLen = needU32(b, 52, "GFF list index length");
    requireRange(b.size(), structOff, static_cast<std::size_t>(structCount) * 12u, "GFF struct table");
    requireRange(b.size(), fieldOff, static_cast<std::size_t>(fieldCount) * 12u, "GFF field table");
    requireRange(b.size(), labelOff, static_cast<std::size_t>(labelCount) * 16u, "GFF label table");
    requireRange(b.size(), dataOff, dataLen, "GFF field data");
    requireRange(b.size(), fidxOff, fidxLen, "GFF field index");
    requireRange(b.size(), lidxOff, lidxLen, "GFF list index");

    gff.fieldData_.assign(b.begin() + dataOff, b.begin() + dataOff + dataLen);
    gff.fieldIndices_.resize(fidxLen / 4u);
    for (std::size_t i = 0; i < gff.fieldIndices_.size(); ++i) {
        gff.fieldIndices_[i] = needU32(b, fidxOff + i * 4u, "GFF field index row");
    }
    gff.listIndices_.resize(lidxLen / 4u);
    for (std::size_t i = 0; i < gff.listIndices_.size(); ++i) {
        gff.listIndices_[i] = needU32(b, lidxOff + i * 4u, "GFF list index row");
    }
    gff.labels_.reserve(labelCount);
    for (std::uint32_t i = 0; i < labelCount; ++i) {
        gff.labels_.push_back(readCString(b.data(), b.size(), labelOff + static_cast<std::size_t>(i) * 16u, 16, "GFF label"));
    }
    gff.fields_.reserve(fieldCount);
    for (std::uint32_t i = 0; i < fieldCount; ++i) {
        const std::size_t o = fieldOff + static_cast<std::size_t>(i) * 12u;
        const std::uint32_t type = needU32(b, o, "GFF field type");
        const std::uint32_t label = needU32(b, o + 4u, "GFF field label");
        const std::uint32_t data = needU32(b, o + 8u, "GFF field data");
        if (type > 18u) {
            throw FormatError(gff.name_ + ": GFF field type " + std::to_string(type));
        }
        if (label >= gff.labels_.size()) {
            throw FormatError(gff.name_ + ": GFF field label out of range");
        }
        GffField field;
        field.type = static_cast<GffType>(type);
        field.labelIndex = label;
        field.data = data;
        field.label = gff.labels_[label];
        gff.fields_.push_back(std::move(field));
    }
    gff.structures_.reserve(structCount);
    for (std::uint32_t i = 0; i < structCount; ++i) {
        const std::size_t o = structOff + static_cast<std::size_t>(i) * 12u;
        GffStruct structure;
        structure.typeId = needU32(b, o, "GFF struct type");
        const std::uint32_t data = needU32(b, o + 4u, "GFF struct data");
        const std::uint32_t count = needU32(b, o + 8u, "GFF struct field count");
        if (count == 1) {
            if (data >= gff.fields_.size()) {
                throw FormatError(gff.name_ + ": GFF direct field row out of range");
            }
            structure.fieldRows.push_back(data);
        } else if (count > 1) {
            if ((data % 4u) != 0) {
                throw FormatError(gff.name_ + ": GFF field index offset not aligned");
            }
            const std::size_t slot = data / 4u;
            if (slot + count > gff.fieldIndices_.size()) {
                throw FormatError(gff.name_ + ": GFF field index overflow");
            }
            for (std::uint32_t j = 0; j < count; ++j) {
                const std::uint32_t row = gff.fieldIndices_[slot + j];
                if (row >= gff.fields_.size()) {
                    throw FormatError(gff.name_ + ": GFF field row out of range");
                }
                structure.fieldRows.push_back(row);
            }
        }
        gff.structures_.push_back(std::move(structure));
    }
    gff.rootIndex_ = 0;
    return gff;
}

const GffStruct& Gff::structure(std::uint32_t index) const
{
    if (index >= structures_.size()) {
        throw FormatError(name_ + ": GFF struct index out of range");
    }
    return structures_[index];
}

const GffField* Gff::field(const GffStruct& structure, const std::string& label) const
{
    for (const std::uint32_t row : structure.fieldRows) {
        if (row < fields_.size() && fields_[row].label == label) {
            return &fields_[row];
        }
    }
    return nullptr;
}

const GffField* Gff::field(std::uint32_t structureIndex, const std::string& label) const
{
    return field(structure(structureIndex), label);
}

std::string Gff::text(const GffField& field) const
{
    switch (field.type) {
    case GffType::String: {
        requireRange(fieldData_.size(), field.data, 4, "GFF string length");
        const std::uint32_t length = readU32(fieldData_.data(), fieldData_.size(), field.data, "GFF string length");
        requireRange(fieldData_.size(), field.data + 4u, length, "GFF string bytes");
        return readCString(fieldData_.data(), fieldData_.size(), field.data + 4u, length, "GFF string");
    }
    case GffType::ResRef: {
        requireRange(fieldData_.size(), field.data, 1, "GFF resref length");
        const std::uint8_t length = fieldData_[field.data];
        requireRange(fieldData_.size(), field.data + 1u, length, "GFF resref bytes");
        return readCString(fieldData_.data(), fieldData_.size(), field.data + 1u, length, "GFF resref");
    }
    case GffType::LocString:
        return locString(field);
    default:
        return {};
    }
}

float Gff::number(const GffField& field) const
{
    switch (field.type) {
    case GffType::UInt8: return static_cast<float>(field.data & 0xFFu);
    case GffType::Int8: return static_cast<float>(static_cast<std::int8_t>(field.data & 0xFFu));
    case GffType::UInt16: return static_cast<float>(field.data & 0xFFFFu);
    case GffType::Int16: return static_cast<float>(static_cast<std::int16_t>(field.data & 0xFFFFu));
    case GffType::UInt32: return static_cast<float>(field.data);
    case GffType::Int32: return static_cast<float>(static_cast<std::int32_t>(field.data));
    case GffType::Float: {
        const std::uint32_t bits = field.data;
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case GffType::UInt64:
    case GffType::Int64:
    case GffType::Double:
        return 0.0f;
    case GffType::StrRef:
        return static_cast<float>(field.data);
    default:
        return 0.0f;
    }
}

std::int32_t Gff::integer(const GffField& field) const
{
    return static_cast<std::int32_t>(number(field));
}

Vec3 Gff::vector3(const GffField& field) const
{
    if (field.type != GffType::Vector3) {
        return {};
    }
    requireRange(fieldData_.size(), field.data, 12, "GFF vector3");
    return {readF32(fieldData_.data(), fieldData_.size(), field.data, "GFF vector3 x"),
            readF32(fieldData_.data(), fieldData_.size(), field.data + 4u, "GFF vector3 y"),
            readF32(fieldData_.data(), fieldData_.size(), field.data + 8u, "GFF vector3 z")};
}

std::array<float, 4> Gff::vector4(const GffField& field) const
{
    if (field.type != GffType::Vector4) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    requireRange(fieldData_.size(), field.data, 16, "GFF vector4");
    return {readF32(fieldData_.data(), fieldData_.size(), field.data, "GFF vector4 x"),
            readF32(fieldData_.data(), fieldData_.size(), field.data + 4u, "GFF vector4 y"),
            readF32(fieldData_.data(), fieldData_.size(), field.data + 8u, "GFF vector4 z"),
            readF32(fieldData_.data(), fieldData_.size(), field.data + 12u, "GFF vector4 w")};
}

std::vector<std::uint32_t> Gff::list(const GffField& field) const
{
    if (field.type != GffType::List) {
        return {};
    }
    if ((field.data % 4u) != 0) {
        throw FormatError(name_ + ": GFF list offset not u32 aligned");
    }
    const std::size_t slot = field.data / 4u;
    if (slot >= listIndices_.size()) {
        throw FormatError(name_ + ": GFF list offset outside list index");
    }
    const std::uint32_t count = listIndices_[slot];
    if (slot + 1u + count > listIndices_.size()) {
        throw FormatError(name_ + ": GFF list entries overflow");
    }
    std::vector<std::uint32_t> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(listIndices_[slot + 1u + i]);
    }
    return out;
}

std::uint32_t Gff::structIndex(const GffField& field) const
{
    if (field.type != GffType::Struct) {
        return UINT32_MAX;
    }
    return field.data;
}

std::string Gff::locString(const GffField& field) const
{
    if (field.type != GffType::LocString) {
        return {};
    }
    const auto parts = locStringParts(field);
    for (const auto& [language, text] : parts) {
        if (language == 0) {
            return text;
        }
    }
    return parts.empty() ? std::string{} : parts.front().second;
}

std::vector<std::pair<std::uint32_t, std::string>> Gff::locStringParts(const GffField& field) const
{
    std::vector<std::pair<std::uint32_t, std::string>> out;
    if (field.type != GffType::LocString) {
        return out;
    }
    requireRange(fieldData_.size(), field.data, 12, "GFF locstring header");
    const std::uint32_t count = readU32(fieldData_.data(), fieldData_.size(), field.data + 8u, "GFF locstring count");
    std::size_t cursor = field.data + 12u;
    for (std::uint32_t i = 0; i < count; ++i) {
        requireRange(fieldData_.size(), cursor, 8, "GFF locstring substring header");
        const std::uint32_t language = readU32(fieldData_.data(), fieldData_.size(), cursor, "GFF locstring language");
        const std::uint32_t length = readU32(fieldData_.data(), fieldData_.size(), cursor + 4u, "GFF locstring length");
        cursor += 8u;
        requireRange(fieldData_.size(), cursor, length, "GFF locstring bytes");
        out.emplace_back(language,
                         readCString(fieldData_.data(), fieldData_.size(), cursor, length, "GFF locstring text"));
        cursor += length;
    }
    return out;
}

std::uint32_t Gff::locStringStrref(const GffField& field) const
{
    if (field.type != GffType::LocString) {
        return UINT32_MAX;
    }
    requireRange(fieldData_.size(), field.data, 8, "GFF locstring strref");
    return readU32(fieldData_.data(), fieldData_.size(), field.data + 4u, "GFF locstring strref");
}

} // namespace ke::kotor
