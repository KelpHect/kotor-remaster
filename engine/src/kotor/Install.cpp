#include "kotor/Formats.h"
#include "kotor/Mods.h"
#include "kotor/ResourceMisses.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
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

std::uint32_t typeForExtension(const std::string& extension)
{
    static const std::map<std::string, std::uint32_t> types = {
        {"2da", 2017}, {"tpc", 3007}, {"tga", 3}, {"mdl", 2002}, {"mdx", 3008},
        {"wok", 2016}, {"are", 2012}, {"ifo", 2014}, {"git", 2023}, {"dlg", 2029},
        {"ncs", 2010}, {"nss", 2011}, {"uti", 2025}, {"utc", 2027}, {"utd", 2042},
        {"utt", 2032}, {"uts", 2035}, {"ute", 2040}, {"utw", 2058}, {"ssf", 2060},
        {"gui", 2047}, {"bik", 2063}, {"lip", 3004}, {"wav", 4}, {"mp3", 8},
        {"txi", 2022}, {"jrl", 2056}, {"fac", 2038}, {"pth", 3003}, {"lyt", 3000},
        {"vis", 3001}, {"tlk", 2018},
    };
    const auto it = types.find(lower(extension));
    return it == types.end() ? 0u : it->second;
}

std::vector<std::uint8_t> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

} // namespace

Install::Install(std::filesystem::path gameDir, bool verifyBuild)
    : root_(std::move(gameDir))
    , overlayRoot_(root_)
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
    rebuildLooseIndex();
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

std::filesystem::path Install::overlayPath(const std::string& relative) const
{
    if (overlayRoot_ != root_) {
        std::error_code error;
        const std::filesystem::path candidate = overlayRoot_ / relative;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return root_ / relative;
}

void Install::dropResolvedCaches()
{
    tables_.clear();
    models_.clear();
    supermodels_.clear();
}

std::shared_ptr<const Tlk> Install::dialogTlk()
{
    if (dialogTlkRead_) {
        return dialogTlk_;
    }
    dialogTlkRead_ = true;
    std::ifstream file(root_ / "dialog.tlk", std::ios::binary | std::ios::ate);
    if (!file) {
        return nullptr;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return nullptr;
    }
    try {
        dialogTlk_ = std::make_shared<const Tlk>(Tlk::parse(std::move(bytes), "dialog.tlk"));
    } catch (const FormatError&) {
    }
    return dialogTlk_;
}

std::shared_ptr<const TwoDa> Install::table(const std::string& name)
{
    if (const auto it = tables_.find(name); it != tables_.end()) {
        return it->second;
    }
    std::shared_ptr<const TwoDa> table;
    if (auto data = findUnpatched(name, resType(ResType::TwoDa), nullptr); data.has_value()) {
        try {
            table = std::make_shared<const TwoDa>(TwoDa::parse(std::move(*data), name));
        } catch (const FormatError&) {
        }
    }
    // Mod table patches, in load order. A patch for a table nothing else provides is the table.
    if (const auto patches = modTablePatches_.find(lower(name)); patches != modTablePatches_.end()) {
        for (const std::filesystem::path& path : patches->second) {
            try {
                TwoDa patch = TwoDa::parse(readWholeFile(path), name);
                table = std::make_shared<const TwoDa>(
                    table ? TwoDa::merge(*table, patch, name) : std::move(patch));
            } catch (const FormatError&) {
            }
        }
    }
    tables_.emplace(name, table);
    return table;
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

void Install::addLayer(int pass, int order, const std::filesystem::path& path,
                       const std::string& name)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return;
    }
    for (const Layer& layer : layers_) {
        if (layer.name == name) {
            return;
        }
    }
    try {
        Layer layer;
        layer.pass = pass;
        layer.order = order;
        layer.sequence = nextSequence_++;
        layer.name = name;
        layer.archive = Archive::open(path);
        layers_.push_back(std::move(layer));
        sortLayers();
        dropResolvedCaches();
    } catch (const FormatError&) {
    }
}

void Install::sortLayers()
{
    std::sort(layers_.begin(), layers_.end(), [](const Layer& a, const Layer& b) {
        if (a.pass != b.pass) {
            return a.pass < b.pass;
        }
        if (a.order != b.order) {
            return a.order < b.order;
        }
        return a.sequence > b.sequence;
    });
}

void Install::indexLooseDirectory(const std::filesystem::path& directory, const std::string& origin)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(extension.begin());
        }
        const std::uint32_t type = typeForExtension(extension);
        if (type == 0) {
            continue;
        }
        const std::string resref = lower(entry.path().stem().string());
        loose_[resref].push_back({type, entry.path(), origin});
    }
}

void Install::rebuildLooseIndex()
{
    loose_.clear();
    // Loose directories are flat, one level; the overlay directory wins when
    // the same resref exists in both the overlay and the install.
    indexLooseDirectory(root_ / "Override");
    indexLooseDirectory(overlayRoot_ / "Override");
    indexLooseDirectory(root_ / "movies");
    indexLooseDirectory(overlayRoot_ / "movies");
    indexLooseDirectory(root_ / "streamwaves");
    indexLooseDirectory(overlayRoot_ / "streamwaves");
    indexLooseDirectory(root_ / "streammusic");
    indexLooseDirectory(overlayRoot_ / "streammusic");
    // Mods are indexed last, so they are searched first; a later mod before an earlier one.
    for (const ModMount& mount : modMounts_) {
        indexLooseDirectory(mount.overrideDirectory, "mod:" + mount.id);
    }
}

void Install::mountMods(const ModSet& mods)
{
    modMounts_.clear();
    modTablePatches_.clear();
    for (const Mod& mod : mods.mods()) {
        if (!mod.overrideDirectory.empty()) {
            modMounts_.push_back({mod.manifest.id, mod.overrideDirectory});
        }
        for (const auto& [table, path] : mod.tablePatches) {
            modTablePatches_[table].push_back(path);
        }
    }
    rebuildLooseIndex();
    dropResolvedCaches();
}

void Install::setOverlayRoot(std::filesystem::path root)
{
    overlayRoot_ = std::move(root);
    rebuildLooseIndex();
    dropResolvedCaches();
}

void Install::bindModule(const std::string& stem)
{
    layers_.clear();
    nextSequence_ = 0;
    dropResolvedCaches();

    // Pass 3: active module RIMs. `rims/global.rim` is not mounted for area
    // loads: the retail global copy contains 2DA revisions that reference
    // models absent from the shipped BIF set (verified by the module sweep).
    addLayer(3, 2, root_ / "rims" / (stem + "_a.rim"), "rims/" + stem + "_a.rim");
    addLayer(3, 1, root_ / "rims" / (stem + "_adx.rim"), "rims/" + stem + "_adx.rim");

    const std::filesystem::path mod = overlayPath("modules/" + stem + ".mod");
    std::error_code error;
    if (std::filesystem::is_regular_file(mod, error)) {
        // Pass 4: the .mod replaces _s.rim.
        addLayer(4, 0, mod, "modules/" + stem + ".mod");
    } else {
        addLayer(3, 0, root_ / "modules" / (stem + "_s.rim"),
                 "modules/" + stem + "_s.rim");
    }
    addLayer(4, 0, root_ / "modules" / (stem + ".rim"),
             "modules/" + stem + ".rim");
    // K2 ships dialog resources separately from the area RIM.
    addLayer(4, 2, overlayPath("modules/" + stem + "_dlg.erf"),
             "modules/" + stem + "_dlg.erf");
    if (layers_.empty()) {
        throw FormatError("no module archive for " + stem);
    }
}

void Install::addRim(const std::string& stem)
{
    addLayer(3, 8, root_ / "rims" / (stem + "dx.rim"), "rims/" + stem + "dx.rim");
    addLayer(3, 9, root_ / "rims" / (stem + ".rim"), "rims/" + stem + ".rim");
}

void Install::addPacks()
{
    // Pass 2: patch.erf is searched before Override/textures.erf.
    addLayer(2, 0, overlayPath("patch.erf"), "patch.erf");
    addLayer(2, 1, overlayPath("Override/textures.erf"), "Override/textures.erf");
    // Pass 4: texture packs below module/lips/players.
    addLayer(4, 3, overlayPath("TexturePacks/swpc_tex_gui.erf"),
             "TexturePacks/swpc_tex_gui.erf");
    addLayer(4, 3, overlayPath("TexturePacks/swpc_tex_tpa.erf"),
             "TexturePacks/swpc_tex_tpa.erf");
}

void Install::addLips()
{
    const auto index = [&](const std::filesystem::path& directory) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            return;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory, error)) {
            if (error || !entry.is_regular_file()
                || entry.path().extension() != ".mod") {
                continue;
            }
            addLayer(4, 1, entry.path(),
                     "lips/" + entry.path().filename().string());
        }
    };
    index(overlayRoot_ / "lips");
    if (overlayRoot_ != root_) {
        index(root_ / "lips");
    }
}

std::vector<std::string> Install::resourceNames(std::uint32_t type) const
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    std::vector<std::string> looseNames;
    for (const auto& [resref, files] : loose_) {
        for (const LooseFile& file : files) {
            if (file.type == type) {
                looseNames.push_back(resref);
                break;
            }
        }
    }
    std::sort(looseNames.begin(), looseNames.end());
    for (const std::string& resref : looseNames) {
        if (seen.insert(resref).second) {
            names.push_back(resref);
        }
    }
    for (const Layer& layer : layers_) {
        for (const ArchiveEntry& entry : layer.archive.entries()) {
            if (entry.type == type && seen.insert(lower(entry.resref)).second) {
                names.push_back(entry.resref);
            }
        }
    }
    for (const KeyEntry& entry : key_.entries()) {
        if (entry.type == type && seen.insert(lower(entry.resref)).second) {
            names.push_back(entry.resref);
        }
    }
    return names;
}

std::optional<std::vector<std::uint8_t>> Install::find(const std::string& resref, std::uint32_t type,
                                                       std::string* source)
{
    // A table that mods patch is returned patched, so callers that parse the bytes
    // themselves see the same table as Install::table.
    if (type == resType(ResType::TwoDa) && !modTablePatches_.empty()) {
        const std::string wanted = normalizeResref(resref);
        if (modTablePatches_.count(wanted) != 0) {
            if (const auto patched = table(wanted)) {
                if (source != nullptr) {
                    *source = "mod:2da/" + wanted + ".2da (patched)";
                }
                return patched->serialize();
            }
        }
    }
    return findUnpatched(resref, type, source);
}

std::optional<std::vector<std::uint8_t>> Install::findUnpatched(const std::string& resref, std::uint32_t type,
                                                                std::string* source)
{
    const std::string wanted = normalizeResref(resref);
    // Walk tooling: a lookup that finds nothing is counted (only while recording is on).
    struct MissGuard {
        std::uint32_t type;
        const std::string& resref;
        bool hit = false;
        ~MissGuard()
        {
            if (!hit) {
                recordResourceMiss(type, resref);
            }
        }
    } missGuard{type, resref};
    // Pass 1: flat loose directories (overlay entries are indexed last and
    // searched first).
    if (const auto it = loose_.find(lower(wanted)); it != loose_.end()) {
        for (auto file = it->second.rbegin(); file != it->second.rend(); ++file) {
            if (file->type != type) {
                continue;
            }
            std::ifstream stream(file->path, std::ios::binary | std::ios::ate);
            if (!stream) {
                continue;
            }
            const std::streamoff size = stream.tellg();
            if (size < 0) {
                continue;
            }
            stream.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (size != 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
                continue;
            }
            if (source != nullptr) {
                *source = file->origin.empty()
                    ? file->path.string() + " (loose)"
                    : file->origin + "/override/" + file->path.filename().string();
            }
            missGuard.hit = true;
            return bytes;
        }
    }
    // Passes 2-4: ERF/RIM tables in precedence order.
    for (const Layer& layer : layers_) {
        if (const ArchiveEntry* entry = layer.archive.find(wanted, type);
            entry != nullptr) {
            if (source != nullptr) {
                *source = layer.name;
            }
            missGuard.hit = true;
            return layer.archive.read(*entry);
        }
    }
    // Pass 5: chitin.key and its BIFs.
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
        missGuard.hit = true;
        return file.read(*record);
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> Install::findTexture(const std::string& resref, std::uint32_t* type,
                                                              std::string* source)
{
    // The two probes are one question: a texture is missing only when neither form exists.
    struct Probe {
        Probe() { setTextureProbe(true); }
        ~Probe() { setTextureProbe(false); }
    };
    {
        Probe probe;
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
    }
    recordTextureMiss(resref);
    return std::nullopt;
}

} // namespace ke::kotor
