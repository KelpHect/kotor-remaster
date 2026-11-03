#include "kotor/GffWrite.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace ke::kotor {

namespace {

void requireLabel(const std::string& label)
{
    if (label.empty() || label.size() > 16) {
        throw FormatError("GFF label must be 1..16 bytes: " + label);
    }
}

std::uint32_t floatBits(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t doubleLow(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);
}

std::uint32_t doubleHigh(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<std::uint32_t>(bits >> 32);
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

std::uint32_t readU32At(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset + 4 > data.size()) {
        throw FormatError("GFF source field data truncated");
    }
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

void appendRaw(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& raw)
{
    out.insert(out.end(), raw.begin(), raw.end());
}

void collectNodes(const GffNodePtr& node, std::vector<GffNodePtr>& order,
                  std::unordered_map<const GffNode*, std::size_t>& index)
{
    if (!node) {
        throw FormatError("GFF tree contains a null struct");
    }
    const auto inserted = index.emplace(node.get(), order.size());
    if (!inserted.second) {
        throw FormatError("GFF tree shares a struct node");
    }
    order.push_back(node);
    for (const GffNodeField& field : node->fields()) {
        if (field.type == GffType::Struct) {
            collectNodes(field.child, order, index);
        } else if (field.type == GffType::List) {
            for (const GffNodePtr& child : field.list) {
                collectNodes(child, order, index);
            }
        }
    }
}

} // namespace

GffNode& GffNode::add(std::string label, GffNodeField value)
{
    requireLabel(label);
    value.label = std::move(label);
    fields_.push_back(std::move(value));
    return *this;
}

GffNode& GffNode::addInt8(std::string label, std::int32_t value)
{
    GffNodeField field;
    field.type = GffType::Int8;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addUInt8(std::string label, std::uint32_t value)
{
    GffNodeField field;
    field.type = GffType::UInt8;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addInt16(std::string label, std::int32_t value)
{
    GffNodeField field;
    field.type = GffType::Int16;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addUInt16(std::string label, std::uint32_t value)
{
    GffNodeField field;
    field.type = GffType::UInt16;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addInt32(std::string label, std::int32_t value)
{
    GffNodeField field;
    field.type = GffType::Int32;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addUInt32(std::string label, std::uint32_t value)
{
    GffNodeField field;
    field.type = GffType::UInt32;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addInt64(std::string label, std::int64_t value)
{
    GffNodeField field;
    field.type = GffType::Int64;
    field.raw.resize(8);
    for (int i = 0; i < 8; ++i) {
        field.raw[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> (i * 8)) & 0xFFu);
    }
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addUInt64(std::string label, std::uint64_t value)
{
    GffNodeField field;
    field.type = GffType::UInt64;
    field.raw.resize(8);
    for (int i = 0; i < 8; ++i) {
        field.raw[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addFloat(std::string label, float value)
{
    GffNodeField field;
    field.type = GffType::Float;
    field.integer = floatBits(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addDouble(std::string label, double value)
{
    GffNodeField field;
    field.type = GffType::Double;
    field.raw.resize(4);
    appendU32(field.raw, doubleLow(value));
    appendU32(field.raw, doubleHigh(value));
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addString(std::string label, std::string value)
{
    GffNodeField field;
    field.type = GffType::String;
    field.text = std::move(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addResRef(std::string label, std::string value)
{
    if (value.size() > 16) {
        throw FormatError("GFF resref longer than 16 bytes: " + value);
    }
    GffNodeField field;
    field.type = GffType::ResRef;
    field.text = std::move(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addLocString(std::string label, std::uint32_t strref,
                               std::vector<std::pair<std::uint32_t, std::string>> substrings)
{
    GffNodeField field;
    field.type = GffType::LocString;
    field.strref = strref;
    field.substrings = std::move(substrings);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addBinary(std::string label, std::vector<std::uint8_t> value)
{
    GffNodeField field;
    field.type = GffType::Binary;
    field.raw = std::move(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addStruct(std::string label, GffNodePtr value)
{
    GffNodeField field;
    field.type = GffType::Struct;
    field.child = std::move(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addList(std::string label, std::vector<GffNodePtr> value)
{
    GffNodeField field;
    field.type = GffType::List;
    field.list = std::move(value);
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addVector3(std::string label, Vec3 value)
{
    GffNodeField field;
    field.type = GffType::Vector3;
    field.vector = value;
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addVector4(std::string label, float x, float y, float z, float w)
{
    GffNodeField field;
    field.type = GffType::Vector4;
    field.vector = {x, y, z};
    field.raw.resize(4);
    appendU32(field.raw, floatBits(w));
    return add(std::move(label), std::move(field));
}

GffNode& GffNode::addStrRef(std::string label, std::uint32_t value)
{
    GffNodeField field;
    field.type = GffType::StrRef;
    field.integer = value;
    return add(std::move(label), std::move(field));
}

GffNodeField* GffNode::find(const std::string& label)
{
    for (GffNodeField& field : fields_) {
        if (field.label == label) {
            return &field;
        }
    }
    return nullptr;
}

const GffNodeField* GffNode::find(const std::string& label) const
{
    for (const GffNodeField& field : fields_) {
        if (field.label == label) {
            return &field;
        }
    }
    return nullptr;
}

bool GffNode::remove(const std::string& label)
{
    const auto it = std::find_if(fields_.begin(), fields_.end(),
                                 [&](const GffNodeField& field) { return field.label == label; });
    if (it == fields_.end()) {
        return false;
    }
    fields_.erase(it);
    return true;
}

GffNodePtr GffNode::fromGff(const Gff& gff, std::uint32_t structIndex)
{
    const GffStruct& structure = gff.structure(structIndex);
    auto node = std::make_shared<GffNode>(structure.typeId);
    for (const std::uint32_t row : structure.fieldRows) {
        const GffField& source = gff.fieldAt(row);
        GffNodeField field;
        field.label = source.label;
        field.type = source.type;
        switch (source.type) {
        case GffType::UInt8:
            field.integer = source.data & 0xFFu;
            break;
        case GffType::Int8:
            field.integer = static_cast<std::int8_t>(source.data & 0xFFu);
            break;
        case GffType::UInt16:
            field.integer = source.data & 0xFFFFu;
            break;
        case GffType::Int16:
            field.integer = static_cast<std::int16_t>(source.data & 0xFFFFu);
            break;
        case GffType::UInt32:
        case GffType::StrRef:
            field.integer = source.data;
            break;
        case GffType::Int32:
            field.integer = static_cast<std::int32_t>(source.data);
            break;
        case GffType::Float:
            field.integer = source.data;
            break;
        case GffType::UInt64:
        case GffType::Int64:
        case GffType::Double: {
            if (source.data + 8u > gff.fieldData().size()) {
                throw FormatError("GFF 64-bit field data truncated");
            }
            field.raw.assign(gff.fieldData().begin() + source.data,
                             gff.fieldData().begin() + source.data + 8u);
            break;
        }
        case GffType::String:
        case GffType::ResRef:
            field.text = gff.text(source);
            break;
        case GffType::LocString:
            field.strref = gff.locStringStrref(source);
            field.substrings = gff.locStringParts(source);
            break;
        case GffType::Binary: {
            const std::uint32_t length = readU32At(gff.fieldData(), source.data);
            if (source.data + 4u + length > gff.fieldData().size()) {
                throw FormatError("GFF binary field data truncated");
            }
            field.raw.assign(gff.fieldData().begin() + source.data + 4u,
                             gff.fieldData().begin() + source.data + 4u + length);
            break;
        }
        case GffType::Vector4:
        case GffType::Vector3: {
            const std::size_t size = source.type == GffType::Vector4 ? 16u : 12u;
            if (source.data + size > gff.fieldData().size()) {
                throw FormatError("GFF vector field data truncated");
            }
            field.raw.assign(gff.fieldData().begin() + source.data,
                             gff.fieldData().begin() + source.data + size);
            if (size >= 12u) {
                std::memcpy(&field.vector.x, gff.fieldData().data() + source.data, 4);
                std::memcpy(&field.vector.y, gff.fieldData().data() + source.data + 4u, 4);
                std::memcpy(&field.vector.z, gff.fieldData().data() + source.data + 8u, 4);
            }
            break;
        }
        case GffType::Struct:
            field.child = fromGff(gff, source.data);
            break;
        case GffType::List: {
            for (const std::uint32_t child : gff.list(source)) {
                field.list.push_back(fromGff(gff, child));
            }
            break;
        }
        default:
            throw FormatError("GFF writer: unsupported source type");
        }
        node->fields().push_back(std::move(field));
    }
    return node;
}

std::vector<std::uint8_t> writeGff(const std::string& magic4, const GffNode& root,
                                   const std::string& version)
{
    if (magic4.size() != 4 || version.size() != 4) {
        throw FormatError("GFF magic/version must be exactly 4 bytes");
    }
    auto rootPtr = std::make_shared<GffNode>();
    std::vector<GffNodePtr> order;
    std::unordered_map<const GffNode*, std::size_t> index;
    // The caller owns the tree; this shallow alias is only for traversal.
    rootPtr.reset(const_cast<GffNode*>(&root), [](GffNode*) {});
    collectNodes(rootPtr, order, index);

    std::vector<std::string> labels;
    std::unordered_map<std::string, std::uint32_t> labelIndex;
    const auto labelId = [&](const std::string& label) -> std::uint32_t {
        const auto found = labelIndex.find(label);
        if (found != labelIndex.end()) {
            return found->second;
        }
        requireLabel(label);
        const auto id = static_cast<std::uint32_t>(labels.size());
        labels.push_back(label);
        labelIndex.emplace(label, id);
        return id;
    };

    std::vector<std::uint8_t> fdata;
    std::vector<std::uint8_t> fidx;
    std::vector<std::uint8_t> lidx;
    struct FieldRow {
        std::uint32_t type = 0;
        std::uint32_t label = 0;
        std::uint32_t data = 0;
    };
    struct StructRow {
        std::uint32_t type = 0;
        std::uint32_t data = 0;
        std::uint32_t count = 0;
    };
    std::vector<FieldRow> fieldRows;
    std::vector<StructRow> structRows;

    const auto addFdata = [&](const std::vector<std::uint8_t>& payload) -> std::uint32_t {
        while ((fdata.size() % 4u) != 0u) {
            fdata.push_back(0);
        }
        const auto offset = static_cast<std::uint32_t>(fdata.size());
        appendRaw(fdata, payload);
        return offset;
    };

    for (const GffNodePtr& node : order) {
        StructRow structRow;
        structRow.type = node->typeId();
        for (const GffNodeField& field : node->fields()) {
            FieldRow row;
            row.type = static_cast<std::uint32_t>(field.type);
            row.label = labelId(field.label);
            switch (field.type) {
            case GffType::UInt8:
            case GffType::Int8:
            case GffType::UInt16:
            case GffType::Int16:
            case GffType::UInt32:
            case GffType::Int32:
            case GffType::Float:
            case GffType::StrRef:
                row.data = static_cast<std::uint32_t>(field.integer);
                break;
            case GffType::UInt64:
            case GffType::Int64:
            case GffType::Double:
                row.data = addFdata(field.raw);
                break;
            case GffType::String: {
                std::vector<std::uint8_t> payload;
                appendU32(payload, static_cast<std::uint32_t>(field.text.size()));
                payload.insert(payload.end(), field.text.begin(), field.text.end());
                row.data = addFdata(payload);
                break;
            }
            case GffType::ResRef: {
                if (field.text.size() > 16) {
                    throw FormatError("GFF resref longer than 16 bytes: " + field.text);
                }
                std::vector<std::uint8_t> payload;
                payload.push_back(static_cast<std::uint8_t>(field.text.size()));
                payload.insert(payload.end(), field.text.begin(), field.text.end());
                row.data = addFdata(payload);
                break;
            }
            case GffType::LocString: {
                std::size_t substringBytes = 0;
                for (const auto& [language, text] : field.substrings) {
                    (void)language;
                    substringBytes += 8u + text.size();
                }
                std::vector<std::uint8_t> payload;
                appendU32(payload, static_cast<std::uint32_t>(8u + substringBytes));
                appendU32(payload, field.strref);
                appendU32(payload, static_cast<std::uint32_t>(field.substrings.size()));
                for (const auto& [language, text] : field.substrings) {
                    appendU32(payload, language);
                    appendU32(payload, static_cast<std::uint32_t>(text.size()));
                    payload.insert(payload.end(), text.begin(), text.end());
                }
                row.data = addFdata(payload);
                break;
            }
            case GffType::Binary: {
                std::vector<std::uint8_t> payload;
                appendU32(payload, static_cast<std::uint32_t>(field.raw.size()));
                appendRaw(payload, field.raw);
                row.data = addFdata(payload);
                break;
            }
            case GffType::Vector3: {
                std::vector<std::uint8_t> payload;
                appendU32(payload, floatBits(field.vector.x));
                appendU32(payload, floatBits(field.vector.y));
                appendU32(payload, floatBits(field.vector.z));
                row.data = addFdata(payload);
                break;
            }
            case GffType::Vector4: {
                std::vector<std::uint8_t> payload;
                appendU32(payload, floatBits(field.vector.x));
                appendU32(payload, floatBits(field.vector.y));
                appendU32(payload, floatBits(field.vector.z));
                // fromGff keeps the original 16-byte vector; addVector4 keeps
                // only the fourth component. Support both representations.
                const std::uint32_t w = field.raw.size() >= 16u
                    ? readU32At(field.raw, 12u)
                    : (field.raw.size() >= 4u ? readU32At(field.raw, 0u) : 0u);
                appendU32(payload, w);
                row.data = addFdata(payload);
                break;
            }
            case GffType::Struct: {
                const auto found = index.find(field.child.get());
                if (found == index.end()) {
                    throw FormatError("GFF struct field points outside the tree");
                }
                row.data = static_cast<std::uint32_t>(found->second);
                break;
            }
            case GffType::List: {
                row.data = static_cast<std::uint32_t>(lidx.size());
                appendU32(lidx, static_cast<std::uint32_t>(field.list.size()));
                for (const GffNodePtr& child : field.list) {
                    const auto found = index.find(child.get());
                    if (found == index.end()) {
                        throw FormatError("GFF list field points outside the tree");
                    }
                    appendU32(lidx, static_cast<std::uint32_t>(found->second));
                }
                break;
            }
            }
            structRow.count += 1;
            const auto rowIndex = static_cast<std::uint32_t>(fieldRows.size());
            fieldRows.push_back(row);
            if (structRow.count == 1) {
                structRow.data = rowIndex;
            } else if (structRow.count == 2) {
                // The first row was stored directly; move both into fidx.
                const std::uint32_t first = structRow.data;
                structRow.data = static_cast<std::uint32_t>(fidx.size());
                appendU32(fidx, first);
                appendU32(fidx, rowIndex);
            } else {
                appendU32(fidx, rowIndex);
            }
        }
        structRows.push_back(structRow);
    }

    std::vector<std::uint8_t> structBytes;
    for (const StructRow& row : structRows) {
        appendU32(structBytes, row.type);
        appendU32(structBytes, row.data);
        appendU32(structBytes, row.count);
    }
    std::vector<std::uint8_t> fieldBytes;
    for (const FieldRow& row : fieldRows) {
        appendU32(fieldBytes, row.type);
        appendU32(fieldBytes, row.label);
        appendU32(fieldBytes, row.data);
    }
    std::vector<std::uint8_t> labelBytes;
    for (const std::string& label : labels) {
        labelBytes.insert(labelBytes.end(), label.begin(), label.end());
        labelBytes.resize(labelBytes.size() + (16u - label.size()), 0);
    }

    const std::uint32_t structOff = 56;
    const std::uint32_t fieldOff = structOff + static_cast<std::uint32_t>(structBytes.size());
    const std::uint32_t labelOff = fieldOff + static_cast<std::uint32_t>(fieldBytes.size());
    const std::uint32_t fdataOff = labelOff + static_cast<std::uint32_t>(labelBytes.size());
    const std::uint32_t fidxOff = fdataOff + static_cast<std::uint32_t>(fdata.size());
    const std::uint32_t lidxOff = fidxOff + static_cast<std::uint32_t>(fidx.size());

    std::vector<std::uint8_t> out;
    out.reserve(lidxOff + lidx.size());
    out.insert(out.end(), magic4.begin(), magic4.end());
    out.insert(out.end(), version.begin(), version.end());
    appendU32(out, structOff);
    appendU32(out, static_cast<std::uint32_t>(structRows.size()));
    appendU32(out, fieldOff);
    appendU32(out, static_cast<std::uint32_t>(fieldRows.size()));
    appendU32(out, labelOff);
    appendU32(out, static_cast<std::uint32_t>(labels.size()));
    appendU32(out, fdataOff);
    appendU32(out, static_cast<std::uint32_t>(fdata.size()));
    appendU32(out, fidxOff);
    appendU32(out, static_cast<std::uint32_t>(fidx.size()));
    appendU32(out, lidxOff);
    appendU32(out, static_cast<std::uint32_t>(lidx.size()));
    appendRaw(out, structBytes);
    appendRaw(out, fieldBytes);
    appendRaw(out, labelBytes);
    appendRaw(out, fdata);
    appendRaw(out, fidx);
    appendRaw(out, lidx);
    return out;
}

std::vector<std::uint8_t> writeErf(const std::vector<ErfWriteEntry>& entries,
                                   const std::string& magic, const std::string& version)
{
    if (magic.size() != 4 || version.size() != 4) {
        throw FormatError("ERF magic/version must be exactly 4 bytes");
    }
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    const std::uint32_t keyOffset = 160;
    const std::uint32_t resourceOffset = keyOffset + count * 24u;
    std::uint32_t dataStart = resourceOffset + count * 8u;
    while ((dataStart % 4u) != 0u) {
        ++dataStart;
    }

    std::vector<std::uint8_t> keys;
    std::vector<std::uint8_t> resources;
    std::vector<std::uint8_t> payload;
    for (std::uint32_t i = 0; i < count; ++i) {
        const ErfWriteEntry& entry = entries[i];
        if (entry.resref.size() > 16) {
            throw FormatError("ERF resref longer than 16 bytes: " + entry.resref);
        }
        keys.insert(keys.end(), entry.resref.begin(), entry.resref.end());
        keys.resize(keys.size() + (16u - entry.resref.size()), 0);
        appendU32(keys, i);
        appendU16(keys, static_cast<std::uint16_t>(entry.type));
        appendU16(keys, 0);
        while ((payload.size() % 4u) != 0u) {
            payload.push_back(0);
        }
        appendU32(resources, dataStart + static_cast<std::uint32_t>(payload.size()));
        appendU32(resources, static_cast<std::uint32_t>(entry.data.size()));
        appendRaw(payload, entry.data);
    }

    std::vector<std::uint8_t> out;
    out.insert(out.end(), magic.begin(), magic.end());
    out.insert(out.end(), version.begin(), version.end());
    appendU32(out, 0); // localized string count
    appendU32(out, 0); // localized string size
    appendU32(out, count);
    appendU32(out, 160); // header size
    appendU32(out, keyOffset);
    appendU32(out, resourceOffset);
    appendU32(out, 0); // build year
    appendU32(out, 0); // build day
    appendU32(out, 0); // description strref
    out.resize(160, 0);
    appendRaw(out, keys);
    appendRaw(out, resources);
    out.resize(dataStart, 0);
    appendRaw(out, payload);
    return out;
}

} // namespace ke::kotor
