#include "launcher/LauncherConfig.h"

#include "kotor/Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

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

std::string toUtf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string trimmed(const std::string& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

// ---- LauncherConfig ----------------------------------------------------------------------------------------------

LauncherConfig LauncherConfig::load(const fs::path& path)
{
    LauncherConfig config;
    std::ifstream file(path);
    std::string line;
    while (file && std::getline(file, line)) {
        const std::string text = trimmed(line);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos || equals == 0) {
            continue;
        }
        config.values_[trimmed(text.substr(0, equals))] = trimmed(text.substr(equals + 1));
    }
    return config;
}

bool LauncherConfig::save(const fs::path& path) const
{
    std::error_code ignored;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ignored);
    }
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            return false;
        }
        file << "# Launcher settings. Edited by the launcher; unknown keys are kept.\n";
        for (const auto& [key, value] : values_) {
            file << key << '=' << value << '\n';
        }
        file.flush();
        if (!file) {
            file.close();
            fs::remove(temporary, ignored);
            return false;
        }
    }
    fs::rename(temporary, path, ignored);
    if (ignored) {
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string LauncherConfig::get(const std::string& key, const std::string& fallback) const
{
    const auto found = values_.find(key);
    return found != values_.end() ? found->second : fallback;
}

void LauncherConfig::set(const std::string& key, const std::string& value)
{
    std::string clean = value;
    clean.erase(std::remove_if(clean.begin(), clean.end(), [](char ch) { return ch == '\n' || ch == '\r'; }), clean.end());
    values_[key] = clean;
}

void LauncherConfig::erase(const std::string& key)
{
    values_.erase(key);
}

bool LauncherConfig::getBool(const std::string& key, bool fallback) const
{
    const std::string value = lowerText(get(key));
    if (value == "1" || value == "true" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no") {
        return false;
    }
    return fallback;
}

int LauncherConfig::getInt(const std::string& key, int fallback) const
{
    const std::string value = get(key);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

std::vector<std::string> LauncherConfig::extraInstalls() const
{
    std::vector<std::string> result;
    for (int i = 0;; ++i) {
        const std::string value = get("extra." + std::to_string(i));
        if (value.empty()) {
            break;
        }
        result.push_back(value);
    }
    return result;
}

void LauncherConfig::addExtraInstall(const std::string& path)
{
    const std::vector<std::string> existing = extraInstalls();
    if (std::find(existing.begin(), existing.end(), path) != existing.end()) {
        return;
    }
    set("extra." + std::to_string(existing.size()), path);
}

// ---- LaunchChoices -----------------------------------------------------------------------------------------------

LaunchChoices LaunchChoices::fromConfig(const LauncherConfig& config)
{
    LaunchChoices choices;
    choices.gameId = config.get("game", choices.gameId);
    choices.install = fromUtf8(config.get("install." + choices.gameId));
    choices.settingsFile = fromUtf8(config.get("settings.file"));
    choices.savesDir = fromUtf8(config.get("saves.dir"));
    choices.backend = config.get("backend", choices.backend);
    choices.language = config.get("language");
    choices.startModule = config.get("start.module", choices.startModule);
    choices.startScreen = config.get("start.screen", choices.startScreen);
    choices.nativeFont = config.getBool("remaster.native_font", false);
    choices.fontFile = fromUtf8(config.get("remaster.font_file"));
    return choices;
}

void LaunchChoices::toConfig(LauncherConfig& config) const
{
    config.set("game", gameId);
    config.set("install." + gameId, toUtf8(install));
    config.set("backend", backend);
    config.set("language", language);
    config.set("saves.dir", toUtf8(savesDir));
    config.set("settings.file", toUtf8(settingsFile));
    config.set("start.module", startModule);
    config.set("start.screen", startScreen);
    config.set("remaster.native_font", nativeFont ? "1" : "0");
    config.set("remaster.font_file", toUtf8(fontFile));
}

// ---- launch plan -------------------------------------------------------------------------------------------------

bool isBackendName(const std::string& name)
{
    return name == "auto" || name == "vulkan" || name == "gl";
}

bool isFontFile(const fs::path& path)
{
    const std::string extension = lowerText(toUtf8(path.extension()));
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}

LaunchPlan buildLaunchPlan(const LaunchChoices& choices, const fs::path& engine)
{
    LaunchPlan plan;
    std::error_code error;
    const GameDef* game = findGame(choices.gameId);
    if (game == nullptr) {
        plan.problems.push_back("Unknown game: " + choices.gameId);
        return plan;
    }
    if (!game->launchable) {
        plan.problems.push_back(game->name + " is detected but the engine cannot play it yet");
    }
    if (choices.install.empty()) {
        plan.problems.push_back("No install folder is chosen");
    } else {
        const ValidationReport report = validateInstall(choices.install, *game, false);
        if (!report.valid) {
            for (const Issue& issue : report.issues) {
                if (issue.severity == Severity::Error) {
                    plan.problems.push_back(issue.message);
                }
            }
        }
    }
    if (!isBackendName(choices.backend)) {
        plan.problems.push_back("Unknown backend: " + choices.backend);
    }
    if (!choices.language.empty() && kotor::languageId(choices.language) < 0) {
        plan.problems.push_back("Unknown language: " + choices.language);
    }
    if (choices.settingsFile.empty()) {
        plan.problems.push_back("No settings file is chosen");
    }
    if (choices.nativeFont) {
        if (choices.fontFile.empty() || !fs::is_regular_file(choices.fontFile, error)) {
            plan.problems.push_back("The native font file does not exist");
        } else if (!isFontFile(choices.fontFile)) {
            plan.problems.push_back("The native font must be a .ttf, .otf or .ttc file");
        }
    }
    if (!fs::is_regular_file(engine, error)) {
        plan.problems.push_back("The engine executable was not found: " + toUtf8(engine));
    }
    if (!plan.ok()) {
        return plan;
    }

    plan.args = {toUtf8(engine), "--install", toUtf8(choices.install), "--settings-file", toUtf8(choices.settingsFile)};
    if (!choices.savesDir.empty()) {
        plan.args.push_back("--saves-dir");
        plan.args.push_back(toUtf8(choices.savesDir));
    }
    plan.args.push_back("--backend=" + choices.backend);
    if (!choices.language.empty()) {
        plan.args.push_back("--language");
        plan.args.push_back(choices.language);
    }
    if (!choices.startModule.empty()) {
        plan.args.push_back("--module");
        plan.args.push_back(choices.startModule);
    }
    if (!choices.startScreen.empty()) {
        plan.args.push_back("--gui-screen");
        plan.args.push_back(choices.startScreen);
    }
    if (choices.nativeFont) {
        plan.args.push_back("--ui-font");
        plan.args.push_back(toUtf8(choices.fontFile));
    }
    return plan;
}

// ---- fonts -------------------------------------------------------------------------------------------------------

std::vector<fs::path> scanFonts(const std::vector<fs::path>& folders)
{
    std::vector<fs::path> fonts;
    std::error_code error;
    for (const fs::path& folder : folders) {
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error) && isFontFile(entry.path())) {
                fonts.push_back(entry.path());
            }
        }
    }
    std::sort(fonts.begin(), fonts.end(), [](const fs::path& a, const fs::path& b) {
        return lowerText(toUtf8(a.filename())) < lowerText(toUtf8(b.filename()));
    });
    return fonts;
}

std::vector<fs::path> systemFontFolders()
{
    std::vector<fs::path> folders;
#if defined(_WIN32)
    if (const std::string windows = environmentValue("WINDIR"); !windows.empty()) {
        folders.push_back(fs::path(windows) / "Fonts");
    }
    if (const std::string local = environmentValue("LOCALAPPDATA"); !local.empty()) {
        folders.push_back(fs::path(local) / "Microsoft" / "Windows" / "Fonts");
    }
#else
    folders.push_back("/usr/share/fonts");
    folders.push_back("/usr/local/share/fonts");
    if (const std::string home = environmentValue("HOME"); !home.empty()) {
        folders.push_back(fs::path(home) / ".fonts");
        folders.push_back(fs::path(home) / ".local" / "share" / "fonts");
    }
#endif
    return folders;
}

// ---- options -----------------------------------------------------------------------------------------------------

const std::vector<OptionDef>& engineOptions()
{
    static const std::vector<OptionDef> options = {
        {"graphics.fullscreen", "Display", "Fullscreen (borderless)", OptionKind::Toggle, 0, 1, {}},
        {"graphics.width", "Display", "Window width (0: default)", OptionKind::Integer, 0, 7680, {}},
        {"graphics.height", "Display", "Window height (0: default)", OptionKind::Integer, 0, 4320, {}},
        {"graphics.vsync", "Display", "Vertical sync", OptionKind::Toggle, 0, 1, {}},
        {"graphics.gamma", "Display", "Gamma", OptionKind::Slider, 0, 100, {}},
        {"graphics.shadows", "Display", "Shadows", OptionKind::Toggle, 0, 1, {}},
        {"graphics.softshadows", "Display", "Soft shadows", OptionKind::Toggle, 0, 1, {}},
        {"graphics.grass", "Display", "Grass", OptionKind::Toggle, 0, 1, {}},
        {"graphics.framebuffer", "Display", "Framebuffer effects", OptionKind::Toggle, 0, 1, {}},

        {"sound.music", "Sound", "Music volume", OptionKind::Slider, 0, 100, {}},
        {"sound.voice", "Sound", "Voice volume", OptionKind::Slider, 0, 100, {}},
        {"sound.effects", "Sound", "Effects volume", OptionKind::Slider, 0, 100, {}},
        {"sound.movie", "Sound", "Movie volume", OptionKind::Slider, 0, 100, {}},
        {"sound.forcesoftware", "Sound", "Force software mixing", OptionKind::Toggle, 0, 1, {}},

        {"gameplay.difficulty", "Gameplay", "Difficulty", OptionKind::Choice, 0, 2, {"Easy", "Normal", "Hard"}},
        {"gameplay.autosave", "Gameplay", "Autosave", OptionKind::Toggle, 0, 1, {}},
        {"gameplay.autolevelup", "Gameplay", "Automatic level-up", OptionKind::Toggle, 0, 1, {}},
        {"gameplay.invertcamera", "Gameplay", "Invert camera", OptionKind::Toggle, 0, 1, {}},
        {"gameplay.reversecamera", "Gameplay", "Reverse camera controls", OptionKind::Toggle, 0, 1, {}},
        {"gameplay.disablemouse", "Gameplay", "Disable mouse movement", OptionKind::Toggle, 0, 1, {}},

        {"mouse.sensitivity", "Mouse", "Mouse sensitivity", OptionKind::Slider, 0, 100, {}},
        {"mouse.reversebuttons", "Mouse", "Swap mouse buttons", OptionKind::Toggle, 0, 1, {}},

        {"feedback.subtitles", "Feedback", "Subtitles", OptionKind::Toggle, 0, 1, {}},
        {"feedback.tooltips", "Feedback", "Tooltips", OptionKind::Toggle, 0, 1, {}},
        {"feedback.minimap", "Feedback", "Minimap", OptionKind::Toggle, 0, 1, {}},
        {"feedback.floatingnumbers", "Feedback", "Floating numbers", OptionKind::Toggle, 0, 1, {}},
        {"feedback.tutorialpopups", "Feedback", "Tutorial pop-ups", OptionKind::Toggle, 0, 1, {}},
        {"feedback.statussummary", "Feedback", "Status summary", OptionKind::Toggle, 0, 1, {}},
        {"feedback.hidequickmenu", "Feedback", "Hide quick menu", OptionKind::Toggle, 0, 1, {}},
        {"feedback.smallfont", "Feedback", "Small interface font", OptionKind::Toggle, 0, 1, {}},
        {"feedback.hideunequippable", "Feedback", "Hide unequippable items", OptionKind::Toggle, 0, 1, {}},

        {"autopause.enemysighted", "Auto-pause", "Enemy sighted", OptionKind::Toggle, 0, 1, {}},
        {"autopause.partykilled", "Auto-pause", "Party member down", OptionKind::Toggle, 0, 1, {}},
        {"autopause.actionmenu", "Auto-pause", "Action menu opened", OptionKind::Toggle, 0, 1, {}},
        {"autopause.endround", "Auto-pause", "End of combat round", OptionKind::Toggle, 0, 1, {}},
        {"autopause.triggers", "Auto-pause", "Triggers", OptionKind::Toggle, 0, 1, {}},
        {"autopause.minesighted", "Auto-pause", "Mine sighted", OptionKind::Toggle, 0, 1, {}},
    };
    return options;
}

} // namespace ke::launcher
