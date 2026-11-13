#pragma once

// Launcher state and the options screen's model.
//
// Two files are involved. The engine's own settings file (settings.ini, numeric key=value, read by the
// engine and edited here through ke::UserSettings) holds the in-game options. The launcher file
// (launcher.ini, text key=value) holds what only the launcher needs: which install, the backend, the
// optional native font, the saves folder. Both are written atomically and never while the engine runs
// (the launcher saves, starts the engine and exits).

#include "launcher/InstallDetect.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ke::launcher {

class LauncherConfig {
public:
    // A missing file is an empty config; unknown keys are kept and written back.
    static LauncherConfig load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    void set(const std::string& key, const std::string& value);
    void erase(const std::string& key);
    bool getBool(const std::string& key, bool fallback) const;
    int getInt(const std::string& key, int fallback) const;

    // Folders the player pointed at by hand, remembered across runs ("extra.0", "extra.1", ...).
    std::vector<std::string> extraInstalls() const;
    void addExtraInstall(const std::string& path);

private:
    std::map<std::string, std::string> values_;
};

// What the player chose for one launch.
struct LaunchChoices {
    std::string gameId = "kotor1";
    std::filesystem::path install;
    std::filesystem::path settingsFile; // the engine's settings.ini
    std::filesystem::path savesDir;     // empty: the engine's default
    std::string backend = "auto";       // auto | vulkan | gl
    std::string language;               // empty: the install's own language
    std::string startModule = "end_m01aa";
    std::string startScreen = "mainmenu";
    // Remaster options. Classic (the scaled original font) is the default; no font is bundled.
    bool nativeFont = false;
    std::filesystem::path fontFile;

    static LaunchChoices fromConfig(const LauncherConfig& config);
    void toConfig(LauncherConfig& config) const;
};

struct LaunchPlan {
    std::vector<std::string> args; // args[0] is the engine executable
    std::vector<std::string> problems; // anything here blocks the launch
    bool ok() const { return problems.empty(); }
};

// Checks the choices against the install and builds the engine command line. Reads the install, never
// writes it. `engine` is the engine executable.
LaunchPlan buildLaunchPlan(const LaunchChoices& choices, const std::filesystem::path& engine);

bool isBackendName(const std::string& name);
bool isFontFile(const std::filesystem::path& path);

// Font files in the given folders (non-recursive), sorted by file name. The launcher passes the system
// and user font folders; the tests pass a fixture folder.
std::vector<std::filesystem::path> scanFonts(const std::vector<std::filesystem::path>& folders);
std::vector<std::filesystem::path> systemFontFolders();

// ---- options screen model ----------------------------------------------------------------------------------------

enum class OptionKind { Toggle, Slider, Choice, Integer };

struct OptionDef {
    const char* key;   // a ke::UserSettings key
    const char* group; // section of the options screen
    const char* label;
    OptionKind kind;
    float minimum;
    float maximum;
    std::vector<const char*> choices; // Choice: value i is choices[i]
};

const std::vector<OptionDef>& engineOptions();

} // namespace ke::launcher
