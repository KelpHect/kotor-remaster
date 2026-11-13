#pragma once

// Install detection and data-set validation for the launcher (ADR 0003).
//
// The launcher points at a user-owned game install and reads it without ever writing to it. An install is
// judged by its data set (chitin.key, dialog.tlk, data/*.bif, the module folders), never by the original
// executable. Detection is pure: every place it looks (Steam roots, GOG folders, extra paths) is passed in,
// so the tests run on fixtures. Only systemSources() touches the registry and the environment, read-only.
// A new game is one more row in gameCatalog().

#include "kotor/Profile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ke::launcher {

enum class Severity { Info, Warning, Error };

struct Issue {
    Severity severity = Severity::Info;
    std::string code;    // stable machine name, e.g. "missing_file"
    std::string message; // what to tell the player
    std::string path;    // install-relative, forward slashes (may be empty)
};

// A data set the project has seen and can name. The chitin.key hash identifies it; no path is embedded.
struct KnownDataSet {
    std::string keySha256;
    std::string id;
    std::string version;
    int languageId = 0;
};

struct RegistryLocation {
    bool localMachine = true; // HKLM or HKCU
    std::string key;          // path under the hive (32-bit view)
    std::string value;
};

struct GameDef {
    std::string id; // stable name used in the launcher config
    std::string name;
    kotor::GameKind kind = kotor::GameKind::Unknown;
    bool launchable = false; // the engine can play it today
    std::uint32_t steamAppId = 0;
    std::vector<std::string> folderNames;       // folder names under steamapps/common and generic roots
    std::vector<RegistryLocation> registry;     // GOG and original-installer locations
    std::vector<std::string> requiredFiles;     // missing: the install is rejected
    std::vector<std::string> requiredDirs;      // missing: the install is rejected
    std::vector<std::string> expectedDirs;      // missing: a warning
    std::string overrideDir;                    // loose mod files; content here is a warning
    std::vector<KnownDataSet> known;
};

const std::vector<GameDef>& gameCatalog();
const GameDef* findGame(const std::string& id);

struct ValidationReport {
    std::string gameId;
    std::filesystem::path root;
    bool valid = false;       // no error issue
    bool recognized = false;  // chitin.key matches a known data set
    std::string datasetId;
    std::string version;
    int languageId = -1;
    std::string languageName;
    std::string keySha256;
    std::size_t bifCount = 0;
    std::size_t keyEntries = 0;
    std::size_t tlkStrings = 0;
    std::size_t overrideFiles = 0;
    std::vector<Issue> issues;

    std::size_t count(Severity severity) const;
    bool modded() const { return overrideFiles > 0; }
};

// Reads the install and checks it against `game`. Never writes. `hashKey` false skips the SHA-256 of
// chitin.key (the structure checks still run).
ValidationReport validateInstall(const std::filesystem::path& root, const GameDef& game, bool hashKey = true);

// The game whose data set best matches the folder, or nullptr when it is none of the catalog.
const GameDef* identifyInstall(const std::filesystem::path& root);

// A readable multi-line report, for the launcher and for logs.
std::string formatReport(const ValidationReport& report);

struct PathSource {
    std::filesystem::path path;
    std::string origin; // "steam", "gog", "registry", "environment", "saved", "browsed", "common"
};

struct DetectionSources {
    std::vector<std::filesystem::path> steamRoots;    // each holds steamapps/libraryfolders.vdf
    std::vector<std::filesystem::path> libraryRoots;  // Steam libraries known without the vdf
    std::vector<std::filesystem::path> commonRoots;   // folders that may hold a game folder directly
    std::vector<PathSource> installs;                 // folders that are an install root themselves
};

struct DetectedInstall {
    std::string gameId;
    std::string origin;
    ValidationReport report;
};

// Every distinct install found, in the order the sources list them. Unreadable places are skipped.
std::vector<DetectedInstall> detectInstalls(const DetectionSources& sources);

// The library folders a Steam libraryfolders.vdf lists, and the installdir of an appmanifest.
std::vector<std::filesystem::path> parseLibraryFolders(const std::string& vdfText);
std::string parseInstallDir(const std::string& manifestText);

// An environment variable's value, empty when unset.
std::string environmentValue(const char* name);

// The real machine: Steam and GOG registry keys (read-only), KOTOR_INSTALL, fixed-drive library folders.
DetectionSources systemSources();

} // namespace ke::launcher
