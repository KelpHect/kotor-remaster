#include "kotor/Formats.h"

#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace ke::kotor {

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) {
        throw FormatError("cannot size " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw FormatError("short read " + path.string());
    }
    return bytes;
}

std::vector<std::uint8_t> readRange(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t size)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open " + path.string());
    }
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        throw FormatError("cannot seek in " + path.string());
    }
    std::vector<std::uint8_t> bytes(size);
    if (size != 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw FormatError("short read in " + path.string());
    }
    return bytes;
}

std::string resKey(const std::string& resref, std::uint32_t type)
{
    return resref + '\x1f' + std::to_string(type);
}

// FIPS 180-4 SHA-256. Small enough to keep the engine dependency-free.
std::string sha256(const std::uint8_t* data, std::size_t size)
{
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    std::array<std::uint32_t, 8> h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const std::uint64_t bitSize = static_cast<std::uint64_t>(size) * 8u;
    std::vector<std::uint8_t> padded(data, data + size);
    padded.push_back(0x80u);
    while ((padded.size() % 64u) != 56u) {
        padded.push_back(0u);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bitSize >> shift) & 0xFFu));
    }
    for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const std::size_t o = chunk + static_cast<std::size_t>(i) * 4u;
            w[i] = (static_cast<std::uint32_t>(padded[o]) << 24)
                | (static_cast<std::uint32_t>(padded[o + 1]) << 16)
                | (static_cast<std::uint32_t>(padded[o + 2]) << 8)
                | static_cast<std::uint32_t>(padded[o + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const std::uint32_t word : h) {
        out << std::setw(8) << word;
    }
    return out.str();
}

} // namespace

void requireRange(std::size_t size, std::size_t offset, std::size_t length, const char* what)
{
    if (offset > size || length > size - offset) {
        throw FormatError(std::string(what) + ": range " + std::to_string(offset) + "+"
            + std::to_string(length) + " exceeds " + std::to_string(size));
    }
}

std::uint16_t readU16(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what)
{
    requireRange(size, offset, 2, what);
    return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

std::uint32_t readU32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what)
{
    requireRange(size, offset, 4, what);
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::int32_t readI32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what)
{
    return static_cast<std::int32_t>(readU32(data, size, offset, what));
}

float readF32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what)
{
    const std::uint32_t bits = readU32(data, size, offset, what);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string readCString(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t maxLength, const char* what)
{
    requireRange(size, offset, maxLength, what);
    std::size_t length = 0;
    while (length < maxLength && data[offset + length] != 0) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(data + offset), length);
}

std::string normalizeResref(std::string text)
{
    if (const std::size_t nul = text.find('\0'); nul != std::string::npos) {
        text.resize(nul);
    }
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string normalizeResref(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t maxLength, const char* what)
{
    return normalizeResref(readCString(data, size, offset, maxLength, what));
}

std::vector<std::uint8_t> readBytes(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t length, const char* what)
{
    requireRange(size, offset, length, what);
    return std::vector<std::uint8_t>(data + offset, data + offset + length);
}

std::string sha256Bytes(const std::uint8_t* data, std::size_t size)
{
    return sha256(data, size);
}

std::string sha256File(const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> bytes = readFile(path);
    return sha256(bytes.data(), bytes.size());
}

// ---- Archive -------------------------------------------------------------------------------------------------------------

Archive Archive::open(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open archive " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    file.seekg(0, std::ios::beg);
    if (end < 0) {
        throw FormatError("cannot size archive " + path.string());
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(end);
    std::array<std::uint8_t, 160> header{};
    const std::size_t headerBytes = fileSize < header.size() ? static_cast<std::size_t>(fileSize) : header.size();
    if (headerBytes == 0 || !file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(headerBytes))) {
        throw FormatError("short archive header " + path.string());
    }

    Archive archive;
    archive.path_ = path;
    archive.fileSize_ = fileSize;

    if (std::memcmp(header.data(), "RIM ", 4) == 0) {
        archive.format_ = "RIM";
        const std::uint32_t count = readU32(header.data(), headerBytes, 12, "RIM entry count");
        const std::uint32_t table = readU32(header.data(), headerBytes, 16, "RIM entry table");
        if (count > 1'000'000u) {
            throw FormatError("implausible RIM entry count in " + path.string());
        }
        requireRange(fileSize, table, static_cast<std::size_t>(count) * 32u, "RIM entry table");
        file.seekg(table, std::ios::beg);
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(count) * 32u);
        if (!rows.empty() && !file.read(reinterpret_cast<char*>(rows.data()), static_cast<std::streamsize>(rows.size()))) {
            throw FormatError("short RIM entry table " + path.string());
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t o = static_cast<std::size_t>(i) * 32u;
            ArchiveEntry entry;
            entry.resref = normalizeResref(rows.data(), rows.size(), o, 16, "RIM resref");
            entry.type = readU32(rows.data(), rows.size(), o + 16, "RIM type");
            entry.id = readU32(rows.data(), rows.size(), o + 20, "RIM id");
            entry.offset = readU32(rows.data(), rows.size(), o + 24, "RIM offset");
            entry.size = readU32(rows.data(), rows.size(), o + 28, "RIM size");
            entry.index = i;
            if (entry.offset + entry.size > fileSize) {
                throw FormatError("RIM entry exceeds file: " + entry.resref);
            }
            archive.entries_.push_back(std::move(entry));
        }
        return archive;
    }

    if (std::memcmp(header.data(), "ERF ", 4) != 0 && std::memcmp(header.data(), "MOD ", 4) != 0
        && std::memcmp(header.data(), "SAV ", 4) != 0) {
        throw FormatError("not an ERF/RIM archive: " + path.string());
    }
    archive.format_ = std::string(reinterpret_cast<const char*>(header.data()), 4);
    const std::uint32_t count = readU32(header.data(), headerBytes, 16, "ERF entry count");
    const std::uint32_t keyTable = readU32(header.data(), headerBytes, 24, "ERF key table");
    const std::uint32_t resourceTable = readU32(header.data(), headerBytes, 28, "ERF resource table");
    if (count > 1'000'000u) {
        throw FormatError("implausible ERF entry count in " + path.string());
    }
    requireRange(fileSize, keyTable, static_cast<std::size_t>(count) * 24u, "ERF key table");
    requireRange(fileSize, resourceTable, static_cast<std::size_t>(count) * 8u, "ERF resource table");
    file.seekg(keyTable, std::ios::beg);
    std::vector<std::uint8_t> keys(static_cast<std::size_t>(count) * 24u);
    if (!keys.empty() && !file.read(reinterpret_cast<char*>(keys.data()), static_cast<std::streamsize>(keys.size()))) {
        throw FormatError("short ERF key table " + path.string());
    }
    file.seekg(resourceTable, std::ios::beg);
    std::vector<std::uint8_t> resources(static_cast<std::size_t>(count) * 8u);
    if (!resources.empty() && !file.read(reinterpret_cast<char*>(resources.data()), static_cast<std::streamsize>(resources.size()))) {
        throw FormatError("short ERF resource table " + path.string());
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t ko = static_cast<std::size_t>(i) * 24u;
        const std::size_t ro = static_cast<std::size_t>(i) * 8u;
        ArchiveEntry entry;
        entry.resref = normalizeResref(keys.data(), keys.size(), ko, 16, "ERF resref");
        entry.id = readU32(keys.data(), keys.size(), ko + 16, "ERF id");
        entry.type = readU16(keys.data(), keys.size(), ko + 20, "ERF type");
        entry.offset = readU32(resources.data(), resources.size(), ro, "ERF offset");
        entry.size = readU32(resources.data(), resources.size(), ro + 4, "ERF size");
        entry.index = i;
        if (entry.offset + entry.size > fileSize) {
            throw FormatError("ERF entry exceeds file: " + entry.resref);
        }
        archive.entries_.push_back(std::move(entry));
    }
    return archive;
}

const ArchiveEntry* Archive::find(const std::string& resref, std::uint32_t type) const
{
    const std::string wanted = normalizeResref(resref);
    for (const ArchiveEntry& entry : entries_) {
        if (entry.type == type && entry.resref == wanted) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> Archive::read(const ArchiveEntry& entry) const
{
    return readRange(path_, entry.offset, entry.size);
}

// ---- BIF -----------------------------------------------------------------------------------------------------------------

BifFile BifFile::open(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open BIF " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    file.seekg(0, std::ios::beg);
    if (end < 20) {
        throw FormatError("short BIF " + path.string());
    }
    BifFile bif;
    bif.path_ = path;
    bif.fileSize_ = static_cast<std::uint64_t>(end);
    std::array<std::uint8_t, 20> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), 20)) {
        throw FormatError("short BIF header " + path.string());
    }
    if (std::memcmp(header.data(), "BIFF", 4) != 0 || std::memcmp(header.data() + 4, "V1  ", 4) != 0) {
        throw FormatError("not a BIFF V1 file: " + path.string());
    }
    const std::uint32_t varCount = readU32(header.data(), header.size(), 8, "BIF var count");
    const std::uint32_t fixedCount = readU32(header.data(), header.size(), 12, "BIF fixed count");
    const std::uint32_t varTable = readU32(header.data(), header.size(), 16, "BIF var table");
    const std::uint64_t fixedTable = static_cast<std::uint64_t>(varTable) + static_cast<std::uint64_t>(varCount) * 16u;
    requireRange(bif.fileSize_, varTable, static_cast<std::size_t>(varCount) * 16u, "BIF var table");
    requireRange(bif.fileSize_, fixedTable, static_cast<std::size_t>(fixedCount) * 16u, "BIF fixed table");
    bif.records_.reserve(static_cast<std::size_t>(varCount) + fixedCount);
    std::uint32_t index = 0;
    const auto readTable = [&](std::uint64_t offset, std::uint32_t count) {
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(count) * 16u);
        if (!rows.empty() && !file.read(reinterpret_cast<char*>(rows.data()), static_cast<std::streamsize>(rows.size()))) {
            throw FormatError("short BIF table " + path.string());
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t o = static_cast<std::size_t>(i) * 16u;
            BifRecord record;
            record.id = readU32(rows.data(), rows.size(), o, "BIF record id");
            record.offset = readU32(rows.data(), rows.size(), o + 4, "BIF record offset");
            record.size = readU32(rows.data(), rows.size(), o + 8, "BIF record size");
            record.type = readU32(rows.data(), rows.size(), o + 12, "BIF record type");
            record.index = index++;
            if (record.offset + record.size > bif.fileSize_) {
                throw FormatError("BIF record exceeds file: " + std::to_string(record.id));
            }
            bif.byId_.try_emplace(record.id, bif.records_.size());
            bif.records_.push_back(record);
        }
    };
    readTable(varTable, varCount);
    readTable(fixedTable, fixedCount);
    return bif;
}

const BifRecord* BifFile::find(std::uint32_t id) const
{
    if (const auto it = byId_.find(id); it != byId_.end()) {
        return &records_[it->second];
    }
    return nullptr;
}

std::vector<std::uint8_t> BifFile::read(const BifRecord& record) const
{
    return readRange(path_, record.offset, record.size);
}

// ---- KEY -----------------------------------------------------------------------------------------------------------------

KeyFile KeyFile::open(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open KEY " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    file.seekg(0, std::ios::beg);
    if (end < 64) {
        throw FormatError("short KEY " + path.string());
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(end);
    std::array<std::uint8_t, 64> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), 64)) {
        throw FormatError("short KEY header " + path.string());
    }
    if (std::memcmp(header.data(), "KEY ", 4) != 0 || std::memcmp(header.data() + 4, "V1  ", 4) != 0) {
        throw FormatError("not a KEY V1 file: " + path.string());
    }
    KeyFile key;
    const std::uint32_t bifCount = readU32(header.data(), header.size(), 8, "KEY bif count");
    const std::uint32_t keyCount = readU32(header.data(), header.size(), 12, "KEY key count");
    const std::uint32_t fileTable = readU32(header.data(), header.size(), 16, "KEY file table");
    const std::uint32_t keyTable = readU32(header.data(), header.size(), 20, "KEY key table");
    if (bifCount > 4096u || keyCount > 2'000'000u) {
        throw FormatError("implausible KEY counts " + path.string());
    }
    requireRange(fileSize, fileTable, static_cast<std::size_t>(bifCount) * 12u, "KEY file table");
    requireRange(fileSize, keyTable, static_cast<std::size_t>(keyCount) * 22u, "KEY key table");
    file.seekg(fileTable, std::ios::beg);
    std::vector<std::uint8_t> files(static_cast<std::size_t>(bifCount) * 12u);
    if (!files.empty() && !file.read(reinterpret_cast<char*>(files.data()), static_cast<std::streamsize>(files.size()))) {
        throw FormatError("short KEY file table " + path.string());
    }
    key.bifs_.reserve(bifCount);
    std::vector<std::uint8_t> all(fileSize > 0 ? static_cast<std::size_t>(fileSize) : 0);
    file.seekg(0, std::ios::beg);
    if (!all.empty() && !file.read(reinterpret_cast<char*>(all.data()), static_cast<std::streamsize>(all.size()))) {
        throw FormatError("short KEY file " + path.string());
    }
    for (std::uint32_t i = 0; i < bifCount; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 12u;
        KeyBif bif;
        bif.index = i;
        bif.declaredSize = readU32(files.data(), files.size(), o, "KEY bif size");
        const std::uint32_t nameOffset = readU32(files.data(), files.size(), o + 4, "KEY name offset");
        const std::uint16_t nameLength = readU16(files.data(), files.size(), o + 8, "KEY name length");
        if (nameLength == 0 || nameLength > 260) {
            throw FormatError("invalid KEY BIF name length");
        }
        std::string name;
        for (const std::uint64_t candidate : {static_cast<std::uint64_t>(nameOffset),
                                             static_cast<std::uint64_t>(fileTable) + nameOffset}) {
            if (candidate + nameLength > fileSize) {
                continue;
            }
            std::string text = readCString(all.data(), all.size(), static_cast<std::size_t>(candidate), nameLength, "KEY name");
            for (char& ch : text) {
                if (ch == '\\') {
                    ch = '/';
                }
            }
            std::string lower = text;
            for (char& ch : lower) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".bif") {
                name = std::move(text);
                break;
            }
        }
        if (name.empty()) {
            throw FormatError("cannot resolve KEY BIF name");
        }
        bif.name = std::move(name);
        key.bifs_.push_back(std::move(bif));
    }
    std::size_t stride = 22;
    if (keyCount != 0 && (fileSize - keyTable) % keyCount == 0) {
        if ((fileSize - keyTable) / keyCount == 24) {
            stride = 24;
        }
    }
    requireRange(fileSize, keyTable, static_cast<std::size_t>(keyCount) * stride, "KEY entries");
    key.entries_.reserve(keyCount);
    for (std::uint32_t i = 0; i < keyCount; ++i) {
        const std::size_t o = static_cast<std::size_t>(keyTable) + static_cast<std::size_t>(i) * stride;
        KeyEntry entry;
        entry.resref = normalizeResref(all.data(), all.size(), o, 16, "KEY resref");
        entry.type = readU16(all.data(), all.size(), o + 16, "KEY type");
        entry.resid = readU32(all.data(), all.size(), o + 18, "KEY resid");
        entry.bifIndex = (entry.resid >> 20) & 0xFFFu;
        entry.tableIndex = entry.resid & 0xFFFFFu;
        key.byKey_[resKey(entry.resref, entry.type)].push_back(key.entries_.size());
        key.entries_.push_back(std::move(entry));
    }
    return key;
}

const KeyEntry* KeyFile::find(const std::string& resref, std::uint32_t type) const
{
    const auto it = byKey_.find(resKey(normalizeResref(resref), type));
    if (it == byKey_.end() || it->second.empty()) {
        return nullptr;
    }
    return &entries_[it->second.front()];
}

} // namespace ke::kotor
