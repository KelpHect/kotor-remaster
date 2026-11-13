#include "launcher/InstallDetect.h"

#include "kotor/Formats.h"
#include "kotor/Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ke::launcher {

namespace fs = std::filesystem;

namespace {

std::string lowerText(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// The entry of `dir` named `name`, ignoring case (the install may sit on a case-sensitive disk).
fs::path findEntry(const fs::path& dir, const std::string& name)
{
    std::error_code error;
    const fs::path exact = dir / name;
    if (fs::exists(exact, error)) {
        return exact;
    }
    const std::string wanted = lowerText(name);
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, error)) {
        if (lowerText(entry.path().filename().string()) == wanted) {
            return entry.path();
        }
    }
    return {};
}

// Resolves "data/foo.bif" or "data\\foo.bif" under `root`, one segment at a time.
fs::path resolveRelative(const fs::path& root, std::string relative)
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    fs::path current = root;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t slash = relative.find('/', start);
        const std::string part = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            current = findEntry(current, part);
            if (current.empty()) {
                return {};
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return current;
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

std::vector<std::uint8_t> readFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void add(ValidationReport& report, Severity severity, std::string code, std::string message, std::string path = {})
{
    report.issues.push_back({severity, std::move(code), std::move(message), std::move(path)});
}

constexpr std::size_t kKeyHeader = 64;
constexpr std::size_t kKeyBifRecord = 12;
constexpr std::size_t kKeyEntryRecord = 22;
constexpr std::size_t kTlkHeader = 20;
constexpr std::size_t kTlkEntryRecord = 40;

void checkKey(ValidationReport& report, const fs::path& keyPath)
{
    const std::vector<std::uint8_t> blob = readFile(keyPath);
    if (blob.size() < kKeyHeader || std::string(blob.begin(), blob.begin() + 8) != "KEY V1  ") {
        add(report, Severity::Error, "key_format", "chitin.key is not a KEY V1 file", "chitin.key");
        return;
    }
    const std::uint32_t bifCount = u32(blob, 8);
    const std::uint32_t keyCount = u32(blob, 12);
    const std::uint32_t offFiles = u32(blob, 16);
    const std::uint32_t offKeys = u32(blob, 20);
    const std::uint64_t tableEnd = static_cast<std::uint64_t>(offFiles) + static_cast<std::uint64_t>(bifCount) * kKeyBifRecord;
    if (offFiles < kKeyHeader || tableEnd > blob.size()) {
        add(report, Severity::Error, "key_format", "chitin.key archive table lies outside the file", "chitin.key");
        return;
    }
    if (static_cast<std::uint64_t>(offKeys) + static_cast<std::uint64_t>(keyCount) * kKeyEntryRecord > blob.size()) {
        add(report, Severity::Error, "key_format", "chitin.key resource table is cut off", "chitin.key");
        return;
    }
    report.bifCount = bifCount;
    report.keyEntries = keyCount;

    const fs::path root = report.root;
    std::size_t missing = 0;
    std::size_t resized = 0;
    std::vector<std::string> firstMissing;
    for (std::uint32_t i = 0; i < bifCount; ++i) {
        const std::size_t record = offFiles + static_cast<std::size_t>(i) * kKeyBifRecord;
        const std::uint32_t declared = u32(blob, record);
        const std::uint32_t nameOffset = u32(blob, record + 4);
        const std::uint16_t nameLength = u16(blob, record + 8);
        if (static_cast<std::uint64_t>(nameOffset) + nameLength > blob.size()) {
            add(report, Severity::Error, "key_format", "chitin.key names an archive outside the file", "chitin.key");
            return;
        }
        std::string name(blob.begin() + nameOffset, blob.begin() + nameOffset + nameLength);
        name = name.substr(0, name.find(static_cast<char>(0)));
        const fs::path found = resolveRelative(root, name);
        std::error_code error;
        if (found.empty() || !fs::is_regular_file(found, error)) {
            ++missing;
            if (firstMissing.size() < 5) {
                std::replace(name.begin(), name.end(), '\\', '/');
                firstMissing.push_back(name);
            }
        } else if (fs::file_size(found, error) != declared) {
            ++resized;
        }
    }
    if (missing > 0) {
        std::string list;
        for (const std::string& name : firstMissing) {
            list += (list.empty() ? "" : ", ") + name;
        }
        add(report, Severity::Error, "missing_bif",
            std::to_string(missing) + " archive(s) named by chitin.key are missing (" + list
                + (missing > firstMissing.size() ? ", ..." : "") + ")",
            "data");
    }
    if (resized > 0) {
        add(report, Severity::Warning, "bif_size",
            std::to_string(resized) + " archive(s) differ in size from what chitin.key declares; the install is patched or modified",
            "data");
    }
}

void checkTlk(ValidationReport& report, const fs::path& tlkPath)
{
    std::ifstream file(tlkPath, std::ios::binary | std::ios::ate);
    const std::uint64_t size = file ? static_cast<std::uint64_t>(file.tellg()) : 0;
    file.seekg(0);
    std::vector<std::uint8_t> head(kTlkHeader);
    if (!file || size < kTlkHeader || !file.read(reinterpret_cast<char*>(head.data()), kTlkHeader)
        || std::string(head.begin(), head.begin() + 8) != "TLK V3.0") {
        add(report, Severity::Error, "tlk_format", "dialog.tlk is not a TLK V3.0 file", "dialog.tlk");
        return;
    }
    const std::uint32_t language = u32(head, 8);
    const std::uint32_t count = u32(head, 12);
    const std::uint32_t stringsOffset = u32(head, 16);
    if (kTlkHeader + static_cast<std::uint64_t>(count) * kTlkEntryRecord > size || stringsOffset > size) {
        add(report, Severity::Error, "tlk_format", "dialog.tlk string table is cut off", "dialog.tlk");
        return;
    }
    report.languageId = static_cast<int>(language);
    report.languageName = kotor::languageName(language);
    report.tlkStrings = count;
}

std::size_t countOverride(const fs::path& dir)
{
    std::error_code error;
    std::size_t files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, error)) {
        if (entry.is_regular_file(error)) {
            ++files;
        }
    }
    return files;
}

std::string readText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string pathKey(const fs::path& path)
{
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    return lowerText((error ? path : canonical).generic_string());
}

} // namespace

const std::vector<GameDef>& gameCatalog()
{
    static const std::vector<GameDef> catalog = [] {
        std::vector<GameDef> games;

        GameDef one;
        one.id = "kotor1";
        one.name = "Knights of the Old Republic";
        one.kind = kotor::GameKind::KotOR1;
        one.launchable = true;
        one.steamAppId = 32370;
        one.folderNames = {"swkotor", "Star Wars - KotOR", "Knights of the Old Republic"};
        one.registry = {
            {true, "SOFTWARE\\GOG.com\\Games\\1207666283", "path"},
            {true, "SOFTWARE\\BioWare\\SW\\KOTOR", "Path"},
            {true, "SOFTWARE\\LucasArts\\KotOR", "InstallPath"},
        };
        one.requiredFiles = {"chitin.key", "dialog.tlk", "patch.erf"};
        one.requiredDirs = {"data", "modules", "rims"};
        one.expectedDirs = {"lips", "TexturePacks", "streammusic", "streamwaves"};
        one.overrideDir = "override";
        one.known = {{"2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98", "kotor1-pc-1.0.3.0-english", "1.0.3.0", 0}};
        games.push_back(std::move(one));

        // Listed so it is detected and named; the engine does not play it yet (gameplay parity is Sprint 18).
        GameDef two;
        two.id = "kotor2";
        two.name = "Knights of the Old Republic II";
        two.kind = kotor::GameKind::KotOR2;
        two.launchable = false;
        two.steamAppId = 208580;
        two.folderNames = {"Knights of the Old Republic II", "swkotor2"};
        two.registry = {
            {true, "SOFTWARE\\GOG.com\\Games\\1421404581", "path"},
            {true, "SOFTWARE\\LucasArts\\KotOR2", "Path"},
        };
        two.requiredFiles = {"chitin.key", "dialog.tlk"};
        two.requiredDirs = {"data", "modules"};
        two.expectedDirs = {"lips", "TexturePacks", "streammusic", "streamvoice"};
        two.overrideDir = "override";
        two.known = {{"47bea08efb5c582918fe2ff32fec845ce1cf604ce8bdd62f5b437004fb257412", "kotor2-pc-steam-english", "", 0}};
        games.push_back(std::move(two));
        return games;
    }();
    return catalog;
}

const GameDef* findGame(const std::string& id)
{
    for (const GameDef& game : gameCatalog()) {
        if (game.id == id) {
            return &game;
        }
    }
    return nullptr;
}

std::size_t ValidationReport::count(Severity severity) const
{
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(),
        [severity](const Issue& issue) { return issue.severity == severity; }));
}

ValidationReport validateInstall(const fs::path& root, const GameDef& game, bool hashKey)
{
    ValidationReport report;
    report.gameId = game.id;
    report.root = root;
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        add(report, Severity::Error, "no_folder", "The folder does not exist");
        return report;
    }

    fs::path keyPath;
    fs::path tlkPath;
    for (const std::string& name : game.requiredFiles) {
        const fs::path found = findEntry(root, name);
        if (found.empty() || !fs::is_regular_file(found, error)) {
            add(report, Severity::Error, "missing_file", "Required file is missing: " + name, name);
        } else if (lowerText(name) == "chitin.key") {
            keyPath = found;
        } else if (lowerText(name) == "dialog.tlk") {
            tlkPath = found;
        }
    }
    for (const std::string& name : game.requiredDirs) {
        const fs::path found = findEntry(root, name);
        if (found.empty() || !fs::is_directory(found, error)) {
            add(report, Severity::Error, "missing_dir", "Required folder is missing: " + name, name);
        } else if (lowerText(name) == "data") {
            bool anyBif = false;
            for (const fs::directory_entry& entry : fs::directory_iterator(found, error)) {
                if (lowerText(entry.path().extension().string()) == ".bif") {
                    anyBif = true;
                    break;
                }
            }
            if (!anyBif) {
                add(report, Severity::Error, "no_bif", "The data folder holds no .bif archives", name);
            }
        }
    }
    for (const std::string& name : game.expectedDirs) {
        if (findEntry(root, name).empty()) {
            add(report, Severity::Warning, "missing_expected_dir",
                "Folder not found: " + name + " (some content may be unavailable)", name);
        }
    }

    if (!keyPath.empty()) {
        checkKey(report, keyPath);
        if (hashKey) {
            report.keySha256 = kotor::sha256File(keyPath);
            for (const KnownDataSet& known : game.known) {
                if (known.keySha256 == report.keySha256) {
                    report.recognized = true;
                    report.datasetId = known.id;
                    report.version = known.version;
                }
            }
            if (!report.recognized) {
                add(report, Severity::Warning, "unknown_dataset",
                    "chitin.key is not a data set this project has seen (another version, language or a modified install); "
                    "it may still work",
                    "chitin.key");
            }
        }
    }
    if (!tlkPath.empty()) {
        checkTlk(report, tlkPath);
        if (report.recognized && report.languageId >= 0) {
            for (const KnownDataSet& known : game.known) {
                if (known.keySha256 == report.keySha256 && !known.version.empty() && known.languageId != report.languageId) {
                    add(report, Severity::Warning, "language_mismatch",
                        "dialog.tlk language (" + report.languageName + ") differs from the reference data set", "dialog.tlk");
                }
            }
        }
    }

    if (!game.overrideDir.empty()) {
        const fs::path overrideFolder = findEntry(root, game.overrideDir);
        if (!overrideFolder.empty() && fs::is_directory(overrideFolder, error)) {
            report.overrideFiles = countOverride(overrideFolder);
            if (report.overrideFiles > 0) {
                add(report, Severity::Warning, "modded",
                    std::to_string(report.overrideFiles) + " file(s) in " + game.overrideDir
                        + ": this install carries modifications. They are read in place and never changed or removed",
                    game.overrideDir);
            }
        }
    }

    report.valid = report.count(Severity::Error) == 0;
    return report;
}

const GameDef* identifyInstall(const fs::path& root)
{
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        return nullptr;
    }
    const fs::path key = findEntry(root, "chitin.key");
    if (key.empty()) {
        return nullptr;
    }
    const std::string sha = kotor::sha256File(key);
    const GameDef* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();
    for (const GameDef& game : gameCatalog()) {
        int score = 0;
        for (const std::string& name : game.requiredFiles) {
            score += findEntry(root, name).empty() ? -1 : 1; // a missing piece counts against the game
        }
        for (const std::string& name : game.requiredDirs) {
            score += findEntry(root, name).empty() ? -1 : 1; // a missing piece counts against the game
        }
        for (const KnownDataSet& known : game.known) {
            if (known.keySha256 == sha) {
                score += 1000;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            best = &game;
        }
    }
    return best;
}

std::string formatReport(const ValidationReport& report)
{
    const GameDef* game = findGame(report.gameId);
    std::ostringstream out;
    out << (game ? game->name : report.gameId) << ": " << (report.valid ? "usable" : "not usable") << "\n";
    out << "  folder: " << report.root.generic_string() << "\n";
    if (report.recognized) {
        out << "  data set: " << report.datasetId << "\n";
    } else if (!report.keySha256.empty()) {
        out << "  data set: not recognised (key " << report.keySha256.substr(0, 12) << ")\n";
    }
    if (report.languageId >= 0) {
        out << "  language: " << report.languageName << " (" << report.tlkStrings << " strings)\n";
    }
    if (report.bifCount > 0) {
        out << "  archives: " << report.bifCount << ", resources: " << report.keyEntries << "\n";
    }
    for (const Issue& issue : report.issues) {
        out << "  "
            << (issue.severity == Severity::Error ? "error: " : issue.severity == Severity::Warning ? "warning: " : "note: ")
            << issue.message << "\n";
    }
    return out.str();
}

// ---- detection ---------------------------------------------------------------------------------------------------

std::vector<fs::path> parseLibraryFolders(const std::string& text)
{
    // Valve's text format: "path"  "D:\\Library". Only "path" keys matter; escapes are backslash pairs.
    std::vector<fs::path> libraries;
    std::size_t at = 0;
    while ((at = text.find("\"path\"", at)) != std::string::npos) {
        at += 6;
        const std::size_t open = text.find('"', at);
        if (open == std::string::npos) {
            break;
        }
        std::string value;
        std::size_t i = open + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
            }
            value += text[i];
        }
        if (!value.empty()) {
            libraries.emplace_back(value);
        }
        at = i;
    }
    return libraries;
}

std::string parseInstallDir(const std::string& text)
{
    const std::size_t key = text.find("\"installdir\"");
    if (key == std::string::npos) {
        return {};
    }
    const std::size_t open = text.find('"', key + 12);
    if (open == std::string::npos) {
        return {};
    }
    const std::size_t close = text.find('"', open + 1);
    return close == std::string::npos ? std::string{} : text.substr(open + 1, close - open - 1);
}

std::vector<DetectedInstall> detectInstalls(const DetectionSources& sources)
{
    std::vector<PathSource> candidates;
    std::error_code error;

    std::vector<fs::path> libraries = sources.libraryRoots;
    for (const fs::path& steam : sources.steamRoots) {
        libraries.push_back(steam);
        for (const fs::path& library : parseLibraryFolders(readText(steam / "steamapps" / "libraryfolders.vdf"))) {
            libraries.push_back(library);
        }
    }
    for (const fs::path& library : libraries) {
        const fs::path apps = library / "steamapps";
        for (const GameDef& game : gameCatalog()) {
            std::vector<std::string> names;
            if (game.steamAppId != 0) {
                const std::string installDir =
                    parseInstallDir(readText(apps / ("appmanifest_" + std::to_string(game.steamAppId) + ".acf")));
                if (!installDir.empty()) {
                    names.push_back(installDir);
                }
            }
            names.insert(names.end(), game.folderNames.begin(), game.folderNames.end());
            for (const std::string& name : names) {
                candidates.push_back({apps / "common" / name, "steam"});
            }
        }
    }
    for (const fs::path& common : sources.commonRoots) {
        for (const GameDef& game : gameCatalog()) {
            for (const std::string& name : game.folderNames) {
                candidates.push_back({common / name, "common"});
            }
        }
    }
    candidates.insert(candidates.end(), sources.installs.begin(), sources.installs.end());

    std::vector<DetectedInstall> found;
    std::set<std::string> seen;
    for (const PathSource& candidate : candidates) {
        if (candidate.path.empty() || !fs::is_directory(candidate.path, error)) {
            continue;
        }
        if (!seen.insert(pathKey(candidate.path)).second) {
            continue;
        }
        const GameDef* game = identifyInstall(candidate.path);
        if (game == nullptr) {
            continue;
        }
        DetectedInstall install;
        install.gameId = game->id;
        install.origin = candidate.origin;
        install.report = validateInstall(candidate.path, *game);
        found.push_back(std::move(install));
    }
    return found;
}

#if defined(_WIN32)
namespace {

fs::path readRegistryPath(const RegistryLocation& location)
{
    HKEY key = nullptr;
    const HKEY hive = location.localMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring subkey(location.key.begin(), location.key.end());
    const std::wstring value(location.value.begin(), location.value.end());
    // The 32-bit view: these installers are 32-bit.
    if (RegOpenKeyExW(hive, subkey.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    fs::path result;
    if (RegQueryValueExW(key, value.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        buffer[size / sizeof(wchar_t)] = L'\0';
        result = fs::path(std::wstring(buffer));
    }
    RegCloseKey(key);
    return result;
}

} // namespace
#endif

std::string environmentValue(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    std::string result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        result = value;
        std::free(value);
    }
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
#endif
}

DetectionSources systemSources()
{
    DetectionSources sources;
#if defined(_WIN32)
    std::error_code error;
    const fs::path steam = readRegistryPath({false, "Software\\Valve\\Steam", "SteamPath"});
    if (!steam.empty()) {
        sources.steamRoots.push_back(steam);
    }
    for (const GameDef& game : gameCatalog()) {
        for (const RegistryLocation& location : game.registry) {
            const fs::path path = readRegistryPath(location);
            if (!path.empty()) {
                sources.installs.push_back({path, location.key.find("GOG") != std::string::npos ? "gog" : "registry"});
            }
        }
    }
    const DWORD drives = GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if ((drives & (1u << letter)) == 0) {
            continue;
        }
        const std::string root = std::string(1, static_cast<char>('A' + letter)) + ":/";
        if (GetDriveTypeA(root.c_str()) != DRIVE_FIXED) {
            continue;
        }
        for (const char* name : {"SteamLibrary", "Steam", "Program Files (x86)/Steam", "Program Files/Steam"}) {
            if (fs::is_directory(fs::path(root) / name / "steamapps", error)) {
                sources.libraryRoots.push_back(fs::path(root) / name);
            }
        }
        for (const char* name : {"GOG Games", "Games", "Program Files (x86)/GOG Galaxy/Games", "Program Files (x86)/LucasArts"}) {
            sources.commonRoots.push_back(fs::path(root) / name);
        }
    }
#endif
    if (const std::string value = environmentValue("KOTOR_INSTALL"); !value.empty()) {
        sources.installs.push_back({fs::path(value), "environment"});
    }
    return sources;
}

} // namespace ke::launcher
