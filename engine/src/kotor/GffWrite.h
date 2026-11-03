#pragma once

// Mutable GFF V3.2 tree and canonical writer. The reader in Gff.cpp stays
// read-only; SaveGame.cpp uses this tree to patch install templates (copy the
// module IFO/GIT/ARE, replace the player list, add the save entry point) and
// to build the PARTYTABLE/GLOBALVARS/savenfo/INVENTORY sidecars.

#include "kotor/Formats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ke::kotor {

class GffNode;

using GffNodePtr = std::shared_ptr<GffNode>;

struct GffNodeField {
    std::string label;
    GffType type = GffType::Int32;
    std::int64_t integer = 0;
    std::string text;
    std::uint32_t strref = UINT32_MAX;
    std::vector<std::pair<std::uint32_t, std::string>> substrings;
    std::vector<std::uint8_t> raw;
    Vec3 vector{};
    GffNodePtr child;
    std::vector<GffNodePtr> list;
};

class GffNode {
public:
    explicit GffNode(std::uint32_t typeId = 0) : typeId_(typeId) {}

    std::uint32_t typeId() const { return typeId_; }
    const std::vector<GffNodeField>& fields() const { return fields_; }
    std::vector<GffNodeField>& fields() { return fields_; }

    GffNode& add(std::string label, GffNodeField value);
    GffNode& addInt8(std::string label, std::int32_t value);
    GffNode& addUInt8(std::string label, std::uint32_t value);
    GffNode& addInt16(std::string label, std::int32_t value);
    GffNode& addUInt16(std::string label, std::uint32_t value);
    GffNode& addInt32(std::string label, std::int32_t value);
    GffNode& addUInt32(std::string label, std::uint32_t value);
    GffNode& addInt64(std::string label, std::int64_t value);
    GffNode& addUInt64(std::string label, std::uint64_t value);
    GffNode& addFloat(std::string label, float value);
    GffNode& addDouble(std::string label, double value);
    GffNode& addString(std::string label, std::string value);
    GffNode& addResRef(std::string label, std::string value);
    GffNode& addLocString(std::string label, std::uint32_t strref,
                          std::vector<std::pair<std::uint32_t, std::string>> substrings);
    GffNode& addBinary(std::string label, std::vector<std::uint8_t> value);
    GffNode& addStruct(std::string label, GffNodePtr value);
    GffNode& addList(std::string label, std::vector<GffNodePtr> value);
    GffNode& addVector3(std::string label, Vec3 value);
    GffNode& addVector4(std::string label, float x, float y, float z, float w);
    GffNode& addStrRef(std::string label, std::uint32_t value);

    GffNodeField* find(const std::string& label);
    const GffNodeField* find(const std::string& label) const;
    bool remove(const std::string& label);

    // Deep-copy one parsed GFF struct into a mutable tree.
    static GffNodePtr fromGff(const Gff& gff, std::uint32_t structIndex = 0);

private:
    std::uint32_t typeId_ = 0;
    std::vector<GffNodeField> fields_;
};

// Canonical GFF V3.2 bytes: struct/field/label/fdata/fidx/lidx, 4-byte aligned
// field data. The Python writer and reader are the independent oracle.
std::vector<std::uint8_t> writeGff(const std::string& magic4, const GffNode& root,
                                   const std::string& version = "V3.2");

// --- KotOR ERF/MOD writer -------------------------------------------------

struct ErfWriteEntry {
    std::string resref;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> writeErf(const std::vector<ErfWriteEntry>& entries,
                                   const std::string& magic = "MOD ",
                                   const std::string& version = "V1.0");

} // namespace ke::kotor
