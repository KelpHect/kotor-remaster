#pragma once

// Modding API v1 (docs/MODDING_API.md): data-only mod packages that live outside the game install.
// A mod is a directory with a `mod.ini` manifest, an `override/` directory of whole-resource
// replacements and a `2da/` directory of table patches. No native code is loaded.

#include "kotor/Profile.h"

#include <compare>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ke::kotor {

// The manifest `api` value this engine loads.
inline constexpr int kModApiVersion = 1;

struct ModVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    // "1", "1.2" or "1.2.3"; nullopt for anything else.
    static std::optional<ModVersion> parse(std::string_view text);
    std::string str() const;
    auto operator<=>(const ModVersion&) const = default;
};

struct ModDependency {
    std::string id;
    std::optional<ModVersion> minimum;
};

struct ModManifest {
    std::string id;
    std::string name;
    ModVersion version;
    int api = 0;
    std::string game = "any"; // any | kotor1 | kotor2
    std::string author;
    std::string description;
    std::vector<ModDependency> requires_; // must be enabled, and loads first
    std::vector<std::string> after;       // loads first when enabled
    std::vector<std::string> conflicts;   // cannot be enabled together

    // Throws FormatError naming `origin` and the line.
    static ModManifest parse(std::string_view text, const std::string& origin);
};

struct Mod {
    ModManifest manifest;
    std::filesystem::path root;
    std::filesystem::path overrideDirectory;                  // empty when the mod has none
    std::vector<std::string> overrideFiles;                   // lowercase file names, sorted
    std::map<std::string, std::filesystem::path> tablePatches; // lowercase table name -> patch file
};

struct ModProblem {
    std::string mod; // mod id or directory name; empty for the mods directory itself
    std::string message;
};

// A resource two enabled mods both provide. `winner` loads later.
struct ModOverlap {
    std::string resource; // "appearance.2da", or "2da:appearance" for a table patch
    std::string winner;
    std::string loser;
};

class ModSet {
public:
    // Reads `<modsDirectory>/<id>/mod.ini` for every sub-directory and resolves the load order.
    // `load_order.txt` in the mods directory, one id per line, enables and orders mods; without it
    // every mod is enabled in id order. A mod with an error is left out, along with every mod that
    // requires it. A missing mods directory is an empty set with no errors.
    static ModSet discover(const std::filesystem::path& modsDirectory, GameKind game);

    // Enabled mods in load order: a later mod wins over an earlier one.
    const std::vector<Mod>& mods() const { return mods_; }
    const std::vector<ModProblem>& errors() const { return errors_; }
    const std::vector<ModProblem>& warnings() const { return warnings_; }
    const std::vector<ModOverlap>& overlaps() const { return overlaps_; }
    bool ok() const { return errors_.empty(); }
    const Mod* find(std::string_view id) const;

    // "id@version" of every enabled mod in load order, joined with ';'. Saves record it.
    std::string fingerprint() const;

private:
    std::vector<Mod> mods_;
    std::vector<ModProblem> errors_;
    std::vector<ModProblem> warnings_;
    std::vector<ModOverlap> overlaps_;
};

} // namespace ke::kotor
