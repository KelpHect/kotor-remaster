#include "kotor/Mods.h"

#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
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

std::string trim(std::string_view text)
{
    const auto space = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

bool validId(std::string_view id)
{
    if (id.empty() || id.size() > 32) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

std::vector<std::string> splitList(std::string_view text)
{
    std::vector<std::string> items;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string item = trim(text.substr(0, comma));
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return items;
}

std::optional<std::string> readText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// The child directory called `name` in any letter case; empty when there is none.
std::filesystem::path childDirectory(const std::filesystem::path& parent, std::string_view name)
{
    std::error_code error;
    std::vector<std::filesystem::path> matches;
    for (const auto& entry : std::filesystem::directory_iterator(parent, error)) {
        if (entry.is_directory() && lower(entry.path().filename().string()) == name) {
            matches.push_back(entry.path());
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches.empty() ? std::filesystem::path{} : matches.front();
}

std::vector<std::filesystem::path> sortedEntries(const std::filesystem::path& directory)
{
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

bool gameMatches(const std::string& wanted, GameKind game)
{
    return wanted == "any" || (wanted == "kotor1" && game == GameKind::KotOR1)
        || (wanted == "kotor2" && game == GameKind::KotOR2);
}

} // namespace

std::optional<ModVersion> ModVersion::parse(std::string_view text)
{
    ModVersion version;
    int* parts[] = {&version.major, &version.minor, &version.patch};
    std::size_t part = 0;
    bool digits = false;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (*parts[part] > 99999) {
                return std::nullopt;
            }
            *parts[part] = *parts[part] * 10 + (ch - '0');
            digits = true;
        } else if (ch == '.' && digits && part < 2) {
            ++part;
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    return version;
}

std::string ModVersion::str() const
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

ModManifest ModManifest::parse(std::string_view text, const std::string& origin)
{
    ModManifest manifest;
    std::set<std::string> seen;
    bool haveVersion = false;
    int lineNumber = 0;
    const auto fail = [&](const std::string& message) -> void {
        throw FormatError(origin + ":" + std::to_string(lineNumber) + ": " + message);
    };
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string line = trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        ++lineNumber;
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            fail("expected `key = value`");
        }
        const std::string key = lower(trim(std::string_view(line).substr(0, equals)));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (!seen.insert(key).second) {
            fail("`" + key + "` is set twice");
        }
        if (key == "id") {
            if (!validId(value)) {
                fail("`id` must be 1-32 characters of a-z, 0-9, `_` or `-`");
            }
            manifest.id = value;
        } else if (key == "name") {
            manifest.name = value;
        } else if (key == "version") {
            const auto version = ModVersion::parse(value);
            if (!version) {
                fail("`version` must look like 1, 1.2 or 1.2.3");
            }
            manifest.version = *version;
            haveVersion = true;
        } else if (key == "api") {
            const auto api = ModVersion::parse(value);
            if (!api || value.find('.') != std::string::npos) {
                fail("`api` must be a whole number");
            }
            manifest.api = api->major;
        } else if (key == "game") {
            manifest.game = lower(value);
            if (manifest.game != "any" && manifest.game != "kotor1" && manifest.game != "kotor2") {
                fail("`game` must be any, kotor1 or kotor2");
            }
        } else if (key == "author") {
            manifest.author = value;
        } else if (key == "description") {
            manifest.description = value;
        } else if (key == "requires") {
            for (const std::string& item : splitList(value)) {
                ModDependency dependency;
                const std::size_t op = item.find(">=");
                dependency.id = trim(std::string_view(item).substr(0, op));
                if (op != std::string::npos) {
                    dependency.minimum = ModVersion::parse(trim(std::string_view(item).substr(op + 2)));
                    if (!dependency.minimum) {
                        fail("bad version in `requires`: " + item);
                    }
                }
                if (!validId(dependency.id)) {
                    fail("bad mod id in `requires`: " + item);
                }
                manifest.requires_.push_back(std::move(dependency));
            }
        } else if (key == "after" || key == "conflicts") {
            for (const std::string& item : splitList(value)) {
                if (!validId(item)) {
                    fail("bad mod id in `" + key + "`: " + item);
                }
                (key == "after" ? manifest.after : manifest.conflicts).push_back(item);
            }
        }
        // Other keys are ignored so a later API version can add them.
    }
    lineNumber = 0;
    if (manifest.id.empty()) {
        throw FormatError(origin + ": `id` is missing");
    }
    if (!haveVersion) {
        throw FormatError(origin + ": `version` is missing");
    }
    if (seen.count("api") == 0) {
        throw FormatError(origin + ": `api` is missing");
    }
    if (manifest.name.empty()) {
        manifest.name = manifest.id;
    }
    return manifest;
}

ModSet ModSet::discover(const std::filesystem::path& modsDirectory, GameKind game)
{
    ModSet set;
    std::error_code error;
    if (!std::filesystem::is_directory(modsDirectory, error)) {
        return set;
    }

    // 1. Read every mod on its own.
    std::map<std::string, Mod> found;
    for (const std::filesystem::path
& root : sortedEntries(modsDirectory)) {
        const std::string directoryName = root.filename().string();
        if (!std::filesystem::is_directory(root, error) || directoryName.empty()
            || directoryName.front() == '.' || directoryName.front() == '_') {
            continue;
        }
        const auto text = readText(root / "mod.ini");
        if (!text) {
            set.warnings_.push_back({directoryName, "no mod.ini, so the directory is not a mod"});
            continue;
        }
        Mod mod;
        mod.root = root;
        try {
            mod.manifest = ModManifest::parse(*text, directoryName + "/mod.ini");
        } catch (const FormatError& failure) {
            set.errors_.push_back({directoryName, failure.what()});
            continue;
        }
        const std::string& id = mod.manifest.id;
        if (id != directoryName) {
            set.errors_.push_back({directoryName, "the directory must be named after the mod id `" + id + "`"});
            continue;
        }
        if (mod.manifest.api != kModApiVersion) {
            set.errors_.push_back({id, "needs modding API " + std::to_string(mod.manifest.api)
                                           + "; this engine has API " + std::to_string(kModApiVersion)});
            continue;
        }
        if (!gameMatches(mod.manifest.game, game)) {
            set.errors_.push_back({id, std::string("is for ") + mod.manifest.game + ", not "
                                           + gameKindName(game)});
            continue;
        }
        bool usable = true;
        mod.overrideDirectory = childDirectory(root, "override");
        if (!mod.overrideDirectory.empty()) {
            for (const std::filesystem::path& path : sortedEntries(mod.overrideDirectory)) {
                if (std::filesystem::is_directory(path, error)) {
                    set.warnings_.push_back({id, "override/" + path.filename().string()
                                                     + " is a directory; override is flat, so it is ignored"});
                    continue;
                }
                const std::string name = lower(path.filename().string());
                if (std::find(mod.overrideFiles.begin(), mod.overrideFiles.end(), name)
                    != mod.overrideFiles.end()) {
                    set.warnings_.push_back({id, "override/" + name + " exists twice in different letter case"});
                    continue;
                }
                mod.overrideFiles.push_back(name);
            }
            std::sort(mod.overrideFiles.begin(), mod.overrideFiles.end());
        }
        if (const std::filesystem::path tables = childDirectory(root, "2da"); !tables.empty()) {
            for (const std::filesystem::path& path : sortedEntries(tables)) {
                if (lower(path.extension().string()) != ".2da") {
                    set.warnings_.push_back({id, "2da/" + path.filename().string() + " is not a .2da file; ignored"});
                    continue;
                }
                const std::string table = lower(path.stem().string());
                try {
                    const auto bytes = readText(path).value_or(std::string{});
                    (void)TwoDa::parse(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), table);
                } catch (const FormatError& failure) {
                    set.errors_.push_back({id, "2da/" + path.filename().string() + ": " + failure.what()});
                    usable = false;
                    continue;
                }
                mod.tablePatches[table] = path;
            }
        }
        if (usable) {
            found.emplace(id, std::move(mod));
        }
    }

    // 2. Which mods are enabled, and their preferred order.
    std::vector<std::string> order;
    if (const auto text = readText(modsDirectory / "load_order.txt")) {
        std::istringstream lines(*text);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            if (std::find(order.begin(), order.end(), line) != order.end()) {
                set.warnings_.push_back({line, "is listed twice in load_order.txt"});
            } else if (found.count(line) == 0) {
                set.errors_.push_back({line, "is in load_order.txt but is not a usable mod"});
            } else {
                order.push_back(line);
            }
        }
    } else {
        for (const auto& [id, mod] : found) {
            order.push_back(id);
        }
    }

    // 3. Drop mods whose requirements or conflicts cannot be met, until nothing changes.
    const auto enabled = [&](const std::string& id) {
        return std::find(order.begin(), order.end(), id) != order.end();
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const std::string& id : order) {
            const ModManifest& manifest = found.at(id).manifest;
            std::string problem;
            for (const ModDependency& dependency : manifest.requires_) {
                if (!enabled(dependency.id)) {
                    problem = "requires `" + dependency.id + "`, which is not enabled";
                } else if (dependency.minimum && found.at(dependency.id).manifest.version < *dependency.minimum) {
                    problem = "requires `" + dependency.id + "` " + dependency.minimum->str() + " or later; found "
                        + found.at(dependency.id).manifest.version.str();
                }
            }
            for (const std::string& other : manifest.conflicts) {
                if (other != id && enabled(other)) {
                    problem = "conflicts with `" + other + "`, which is enabled";
                }
            }
            if (!problem.empty()) {
                set.errors_.push_back({id, problem});
                order.erase(std::find(order.begin(), order.end(), id));
                changed = true;
                break;
            }
        }
    }

    // 4. Load order: the preferred order, moved only as far as `requires` and `after` demand.
    std::vector<std::string> sorted;
    std::vector<std::string> waiting = order;
    while (!waiting.empty()) {
        const auto ready = std::find_if(waiting.begin(), waiting.end(), [&](const std::string& id) {
            const ModManifest& manifest = found.at(id).manifest;
            const auto pending = [&](const std::string& other) {
                return std::find(waiting.begin(), waiting.end(), other) != waiting.end();
            };
            return std::none_of(manifest.requires_.begin(), manifest.requires_.end(),
                                [&](const ModDependency& dependency) { return pending(dependency.id); })
                && std::none_of(manifest.after.begin(), manifest.after.end(), pending);
        });
        if (ready == waiting.end()) {
            // Reject the mods that are on a cycle, then the mods that require a rejected mod.
            // A mod that only lists a rejected mod in `after` stays.
            const auto isWaiting = [&](const std::string& id) {
                return std::find(waiting.begin(), waiting.end(), id) != waiting.end();
            };
            const auto reachesItself = [&](const std::string& start) {
                std::set<std::string> seen;
                std::vector<std::string> stack{start};
                while (!stack.empty()) {
                    const ModManifest& manifest = found.at(stack.back()).manifest;
                    stack.pop_back();
                    std::vector<std::string> next = manifest.after;
                    for (const ModDependency& dependency : manifest.requires_) {
                        next.push_back(dependency.id);
                    }
                    for (const std::string& id : next) {
                        if (id == start) {
                            return true;
                        }
                        if (isWaiting(id) && seen.insert(id).second) {
                            stack.push_back(id);
                        }
                    }
                }
                return false;
            };
            std::set<std::string> rejected;
            for (const std::string& id : waiting) {
                if (reachesItself(id)) {
                    rejected.insert(id);
                    set.errors_.push_back({id, "is in a `requires`/`after` cycle"});
                }
            }
            for (bool changed = true; changed;) {
                changed = false;
                for (const std::string& id : waiting) {
                    if (rejected.count(id) != 0) {
                        continue;
                    }
                    for (const ModDependency& dependency : found.at(id).manifest.requires_) {
                        if (rejected.count(dependency.id) != 0) {
                            set.errors_.push_back({id, "requires `" + dependency.id + "`, which was rejected"});
                            rejected.insert(id);
                            changed = true;
                            break;
                        }
                    }
                }
            }
            if (rejected.empty()) {
                break;
            }
            std::erase_if(waiting, [&](const std::string& id) { return rejected.count(id) != 0; });
            continue;
        }
        sorted.push_back(*ready);
        waiting.erase(ready);
    }
    for (const std::string& id : sorted) {
        set.mods_.push_back(std::move(found.at(id)));
    }

    // 5. Resources more than one mod provides.
    std::map<std::string, std::string> owner;
    const auto claim = [&](const std::string& resource, const std::string& id) {
        const auto [it, added] = owner.emplace(resource, id);
        if (!added) {
            set.overlaps_.push_back({resource, id, it->second});
            it->second = id;
        }
    };
    for (const Mod& mod : set.mods_) {
        for (const std::string& file : mod.overrideFiles) {
            claim(file, mod.manifest.id);
        }
        for (const auto& [table, path] : mod.tablePatches) {
            claim("2da:" + table, mod.manifest.id);
        }
    }
    return set;
}

const Mod* ModSet::find(std::string_view id) const
{
    for (const Mod& mod : mods_) {
        if (mod.manifest.id == id) {
            return &mod;
        }
    }
    return nullptr;
}

std::string ModSet::fingerprint() const
{
    std::string text;
    for (const Mod& mod : mods_) {
        if (!text.empty()) {
            text += ';';
        }
        text += mod.manifest.id + "@" + mod.manifest.version.str();
    }
    return text;
}

} // namespace ke::kotor
