#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace ke::kotor {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::vector<std::string> splitTabs(const std::uint8_t* data, std::size_t size, std::size_t offset,
                                   std::size_t end, const char* what)
{
    requireRange(size, offset, end > offset ? end - offset : 0, what);
    std::vector<std::string> out;
    std::size_t start = offset;
    for (std::size_t i = offset; i < end; ++i) {
        if (data[i] == '\t') {
            out.emplace_back(reinterpret_cast<const char*>(data + start), i - start);
            start = i + 1;
        }
    }
    if (start < end) {
        out.emplace_back(reinterpret_cast<const char*>(data + start), end - start);
    }
    return out;
}

std::vector<std::string> splitWhitespace(const std::string& line)
{
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (const char ch : line) {
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

} // namespace

int TwoDa::columnIndex(const std::string& name) const
{
    const std::string wanted = lower(name);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (lower(columns_[i]) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TwoDa::rowIndex(const std::string& label) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (lower(rows_[i].label) == lower(label)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string TwoDa::cell(std::size_t row, std::size_t column) const
{
    if (row >= rows_.size() || column >= rows_[row].cells.size()) {
        return {};
    }
    return rows_[row].cells[column];
}

std::string TwoDa::cell(const std::string& row, const std::string& column) const
{
    const int r = rowIndex(row);
    const int c = columnIndex(column);
    if (r < 0 || c < 0) {
        return {};
    }
    return cell(static_cast<std::size_t>(r), static_cast<std::size_t>(c));
}

TwoDa TwoDa::merge(const TwoDa& base, const TwoDa& patch, std::string name)
{
    TwoDa merged = base;
    merged.name_ = std::move(name);
    std::vector<std::size_t> patchColumnToMerged(patch.columns_.size());
    for (std::size_t column = 0; column < patch.columns_.size(); ++column) {
        const int existing = merged.columnIndex(patch.columns_[column]);
        if (existing >= 0) {
            patchColumnToMerged[column] = static_cast<std::size_t>(existing);
            continue;
        }
        patchColumnToMerged[column] = merged.columns_.size();
        merged.columns_.push_back(patch.columns_[column]);
        for (Row& row : merged.rows_) {
            row.cells.push_back({});
        }
    }
    for (const Row& patchRow : patch.rows_) {
        int existing = merged.rowIndex(patchRow.label);
        if (existing < 0) {
            Row row;
            row.label = patchRow.label;
            row.cells.assign(merged.columns_.size(), {});
            merged.rows_.push_back(std::move(row));
            existing = static_cast<int>(merged.rows_.size()) - 1;
        }
        Row& target = merged.rows_[static_cast<std::size_t>(existing)];
        for (std::size_t column = 0; column < patchRow.cells.size()
             && column < patchColumnToMerged.size(); ++column) {
            const std::string& value = patchRow.cells[column];
            if (value.empty() || value == "****") {
                continue;
            }
            target.cells[patchColumnToMerged[column]] = value;
        }
    }
    return merged;
}

TwoDa TwoDa::parse(std::vector<std::uint8_t> bytes, std::string name)
{
    TwoDa table;
    table.name_ = std::move(name);
    if (bytes.size() >= 10 && std::memcmp(bytes.data(), "2DA V2.0", 8) == 0) {
        std::string text(bytes.begin(), bytes.end());
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos
                                                ? std::string::npos : end - start);
            while (!line.empty()
                   && (line.back() == '\r' || std::isspace(
                           static_cast<unsigned char>(line.back())) != 0)) {
                line.pop_back();
            }
            lines.push_back(std::move(line));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        std::size_t index = 0;
        while (index < lines.size() && lines[index].empty()) {
            ++index;
        }
        if (index >= lines.size()
            || lines[index].rfind("2DA V2.0", 0) != 0) {
            throw FormatError(table.name_ + ": ASCII 2DA has no V2.0 header");
        }
        ++index;
        while (index < lines.size() && lines[index].empty()) {
            ++index;
        }
        if (index >= lines.size()) {
            throw FormatError(table.name_ + ": ASCII 2DA has no columns");
        }
        table.columns_ = splitWhitespace(lines[index++]);
        if (table.columns_.empty()) {
            throw FormatError(table.name_ + ": ASCII 2DA has no columns");
        }
        while (index < lines.size()) {
            if (lines[index].empty()
                || lines[index].rfind("DEFAULT:", 0) == 0) {
                ++index;
                continue;
            }
            std::vector<std::string> tokens = splitWhitespace(lines[index++]);
            if (tokens.empty()) {
                continue;
            }
            Row row;
            row.label = std::move(tokens.front());
            row.cells.assign(table.columns_.size(), {});
            for (std::size_t column = 0;
                 column + 1 < tokens.size() && column < row.cells.size(); ++column) {
                row.cells[column] = std::move(tokens[column + 1]);
            }
            table.rows_.push_back(std::move(row));
        }
        return table;
    }
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "2DA V2.b", 8) != 0) {
        throw FormatError(table.name_ + ": not a 2DA table");
    }
    std::size_t pos = 8;
    if (pos < bytes.size() && bytes[pos] == '\r') {
        ++pos;
    }
    if (pos >= bytes.size() || bytes[pos] != '\n') {
        throw FormatError(table.name_ + ": 2DA header has no newline");
    }
    ++pos;
    const std::size_t columnsEnd = [&]() {
        for (std::size_t i = pos; i < bytes.size(); ++i) {
            if (bytes[i] == 0) {
                return i;
            }
        }
        throw FormatError(table.name_ + ": unterminated 2DA column table");
    }();
    if (columnsEnd > pos) {
        std::vector<std::string> parts = splitTabs(bytes.data(), bytes.size(), pos, columnsEnd,
                                                  "2DA columns");
        if (parts.empty() || parts.back().empty()) {
            if (!parts.empty()) {
                parts.pop_back();
            }
        }
        for (const std::string& part : parts) {
            if (part.empty()) {
                throw FormatError(table.name_ + ": empty 2DA column name");
            }
            table.columns_.push_back(part);
        }
    }
    pos = columnsEnd + 1;
    const std::uint32_t rowCount = readU32(bytes.data(), bytes.size(), pos, "2DA row count");
    pos += 4;
    if (rowCount > 1'000'000u) {
        throw FormatError(table.name_ + ": implausible 2DA row count");
    }
    if (rowCount != 0 && table.columns_.empty()) {
        throw FormatError(table.name_ + ": 2DA rows without columns");
    }
    table.rows_.resize(rowCount);
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        const std::size_t end = [&]() {
            for (std::size_t i = pos; i < bytes.size(); ++i) {
                if (bytes[i] == '\t') {
                    return i;
                }
            }
            throw FormatError(table.name_ + ": unterminated 2DA row label");
        }();
        table.rows_[row].label.assign(reinterpret_cast<const char*>(bytes.data() + pos), end - pos);
        pos = end + 1;
    }
    const std::size_t cellCount = static_cast<std::size_t>(rowCount) * table.columns_.size();
    requireRange(bytes.size(), pos, cellCount * 2u + 2u, "2DA cell offsets");
    std::vector<std::uint16_t> offsets(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        offsets[i] = readU16(bytes.data(), bytes.size(), pos + i * 2u, "2DA cell offset");
    }
    pos += cellCount * 2u;
    const std::uint16_t textSize = readU16(bytes.data(), bytes.size(), pos, "2DA string size");
    pos += 2;
    requireRange(bytes.size(), pos, textSize, "2DA string table");
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        table.rows_[row].cells.resize(table.columns_.size());
        for (std::size_t column = 0; column < table.columns_.size(); ++column) {
            const std::uint16_t offset = offsets[static_cast<std::size_t>(row) * table.columns_.size() + column];
            if (offset >= textSize) {
                throw FormatError(table.name_ + ": 2DA cell offset outside string table");
            }
            std::size_t end = pos + offset;
            while (end < pos + textSize && bytes[end] != 0) {
                ++end;
            }
            if (end >= pos + textSize) {
                throw FormatError(table.name_ + ": unterminated 2DA cell string");
            }
            table.rows_[row].cells[column].assign(
                reinterpret_cast<const char*>(bytes.data() + pos + offset), end - (pos + offset));
        }
    }
    return table;
}

} // namespace ke::kotor
