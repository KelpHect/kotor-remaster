#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace ke::kotor {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

} // namespace

Install::Install(std::filesystem::path gameDir, bool verifyBuild)
    : root_(std::move(gameDir))
{
    if (!std::filesystem::is_directory(root_)) {
        throw FormatError("install directory not found: " + root_.string());
    }
    profile_ = GameProfile::detect(root_);
    if (profile_.kind == GameKind::Unknown) {
        throw FormatError("no supported KotOR executable in " + root_.string());
    }
    buildSha256_ = profile_.buildSha256;
    if (verifyBuild && !profile_.recognized) {
        throw FormatError(profile_.executable + " SHA-256 " + buildSha256_
                          + " is not a supported " + profile_.name + " build");
    }
    key_ = KeyFile::open(root_ / "chitin.key");
}

std::filesystem::path Install::bifPath(const std::string& keyName) const
{
    std::string name = keyName;
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::string lowerName = lower(name);
    if (lowerName.rfind("data/", 0) == 0) {
        return root_ / name;
    }
    return root_ / "data" / std::filesystem::path(name).filename();
}

const BifFile& Install::bif(std::uint32_t index)
{
    if (const auto it = bifs_.find(index); it != bifs_.end()) {
        return it->second;
    }
    if (index >= key_.bifs().size()) {
        throw FormatError("KEY BIF index out of range: " + std::to_string(index));
    }
    const std::filesystem::path path = bifPath(key_.bifs()[index].name);
    BifFile file = BifFile::open(path);
    return bifs_.emplace(index, std::move(file)).first->second;
}

void Install::bindModule(const std::string& stem)
{
    archives_.clear();
    archiveNames_.clear();
    for (const std::string suffix : {std::string(), std::string("_s")}) {
        const std::filesystem::path path = root_ / "modules" / (stem + suffix + ".rim");
        if (std::filesystem::is_regular_file(path)) {
            archives_.push_back(Archive::open(path));
            archiveNames_.push_back("modules/" + stem + suffix + ".rim");
        }
    }
    if (archives_.empty()) {
        throw FormatError("no module RIM for " + stem);
    }
}

void Install::addPacks()
{
    for (const char* rel : {"patch.erf", "TexturePacks/swpc_tex_gui.erf", "TexturePacks/swpc_tex_tpa.erf"}) {
        const std::filesystem::path path = root_ / rel;
        if (std::filesystem::is_regular_file(path)) {
            archives_.push_back(Archive::open(path));
            archiveNames_.push_back(rel);
        }
    }
}

void Install::addLips()
{
    std::error_code error;
    const std::filesystem::path directory = root_ / "lips";
    if (!std::filesystem::is_directory(directory, error)) {
        return;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != ".mod") {
            continue;
        }
        const std::string name = "lips/" + entry.path().filename().string();
        if (std::find(archiveNames_.begin(), archiveNames_.end(), name)
            != archiveNames_.end()) {
            continue;
        }
        try {
            archives_.push_back(Archive::open(entry.path()));
            archiveNames_.push_back(name);
        } catch (const FormatError&) {
        }
    }
}

std::vector<std::string> Install::resourceNames(std::uint32_t type) const
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const KeyEntry& entry : key_.entries()) {
        if (entry.type == type && seen.insert(entry.resref).second) {
            names.push_back(entry.resref);
        }
    }
    for (const Archive& archive : archives_) {
        for (const ArchiveEntry& entry : archive.entries()) {
            if (entry.type == type && seen.insert(entry.resref).second) {
                names.push_back(entry.resref);
            }
        }
    }
    return names;
}

std::optional<std::vector<std::uint8_t>> Install::find(const std::string& resref, std::uint32_t type,
                                                       std::string* source)
{
    const std::string wanted = normalizeResref(resref);
    for (std::size_t i = 0; i < archives_.size(); ++i) {
        if (const ArchiveEntry* entry = archives_[i].find(wanted, type); entry != nullptr) {
            if (source != nullptr) {
                *source = archiveNames_[i];
            }
            return archives_[i].read(*entry);
        }
    }
    if (const KeyEntry* entry = key_.find(wanted, type); entry != nullptr) {
        if (entry->bifIndex >= key_.bifs().size()) {
            return std::nullopt;
        }
        const BifFile& file = bif(entry->bifIndex);
        const BifRecord* record = file.find(entry->resid);
        if (record == nullptr) {
            record = file.find(entry->tableIndex);
        }
        if (record == nullptr) {
            return std::nullopt;
        }
        if (source != nullptr) {
            *source = key_.bifs()[entry->bifIndex].name + " (KEY)";
        }
        return file.read(*record);
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> Install::findTexture(const std::string& resref, std::uint32_t* type,
                                                              std::string* source)
{
    if (auto data = find(resref, resType(ResType::Tpc), source); data.has_value()) {
        if (type != nullptr) {
            *type = resType(ResType::Tpc);
        }
        return data;
    }
    if (auto data = find(resref, resType(ResType::Tga), source); data.has_value()) {
        if (type != nullptr) {
            *type = resType(ResType::Tga);
        }
        return data;
    }
    return std::nullopt;
}

} // namespace ke::kotor
