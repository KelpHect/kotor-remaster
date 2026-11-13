// Launcher: finds user-owned game installs, validates their data set (never the executable), holds the
// engine and remaster options, and starts the engine with the choice. It reads the install and never
// writes to it (ADR 0003). Neutral branding: no game artwork, text or fonts are shipped.
//
//   kotor_launcher                         the window
//   kotor_launcher --list                  print detected installs and their validation, then exit
//   kotor_launcher --check <folder>        validate one folder (exit 0 usable, 2 not usable)
//   kotor_launcher --print-command         print the engine command line the window would start
//
// Paths: --config-file, --settings-file, --saves-dir, --engine, --install. Test runs pass the first
// three so nothing is written to the player's profile.

#include "game/UserSettings.h"
#include "kotor/Localization.h"
#include "launcher/InstallDetect.h"
#include "launcher/LauncherConfig.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ke;
using namespace ke::launcher;

namespace {

constexpr int kExitSkip = 77; // no usable OpenGL context here (the smoke test treats it as a skip)

struct CommandLine {
    bool list = false;
    bool printCommand = false;
    bool hidden = false;
    int frames = 0; // exit after this many frames (smoke test)
    std::string check;
    std::string screenshot;
    std::string tab;
    fs::path configFile;
    fs::path settingsFile;
    fs::path savesDir;
    fs::path engine;
    fs::path install;
    std::string error;
};

CommandLine parseCommandLine(int argc, char** argv)
{
    CommandLine cl;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                cl.error = std::string(name) + " needs a value";
                return {};
            }
            return argv[++i];
        };
        if (arg == "--list") {
            cl.list = true;
        } else if (arg == "--print-command") {
            cl.printCommand = true;
        } else if (arg == "--hidden") {
            cl.hidden = true;
        } else if (arg == "--frames") {
            cl.frames = std::atoi(value("--frames").c_str());
        } else if (arg == "--check") {
            cl.check = value("--check");
        } else if (arg == "--screenshot") {
            cl.screenshot = value("--screenshot");
        } else if (arg == "--tab") {
            cl.tab = value("--tab");
        } else if (arg == "--config-file") {
            cl.configFile = fs::path(value("--config-file"));
        } else if (arg == "--settings-file") {
            cl.settingsFile = fs::path(value("--settings-file"));
        } else if (arg == "--saves-dir") {
            cl.savesDir = fs::path(value("--saves-dir"));
        } else if (arg == "--engine") {
            cl.engine = fs::path(value("--engine"));
        } else if (arg == "--install") {
            cl.install = fs::path(value("--install"));
        } else {
            cl.error = "unknown argument: " + arg;
        }
        if (!cl.error.empty()) {
            break;
        }
    }
    return cl;
}

fs::path exeFolder()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fs::path(base) : fs::path(".");
}

fs::path prefFolder(const char* app)
{
    fs::path result;
    if (char* path = SDL_GetPrefPath("kotor-remaster", app)) {
        result = path;
        SDL_free(path);
    }
    return result;
}

std::string utf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

// ---- screenshot (stored-deflate PNG, no extra dependency) ---------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0)
{
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

void be32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data)
{
    be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(body.data(), body.size()));
}

bool writePng(const fs::path& path, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 3 + 1));
    for (int y = height - 1; y >= 0; --y) { // GL rows run bottom to top
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            raw.push_back(rgba[at]);
            raw.push_back(rgba[at + 1]);
            raw.push_back(rgba[at + 2]);
        }
    }
    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
        zlib.push_back(at + length >= raw.size() ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(length & 0xff));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~length & 0xff));
        zlib.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xff));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + length));
    }
    be32(zlib, (b << 16) | a);
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<std::uint8_t> header;
    be32(header, static_cast<std::uint32_t>(width));
    be32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    std::error_code ignored;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ignored);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(file);
}

// ---- launcher state ----------------------------------------------------------------------------------------------

struct Dialog {
    std::mutex lock;
    std::optional<std::string> folder; // set by the file dialog callbacks
    std::optional<std::string> font;
    std::optional<std::string> saves;
};

void SDLCALL folderPicked(void* user, const char* const* list, int /*filter*/)
{
    auto* slot = static_cast<std::pair<Dialog*, std::optional<std::string> Dialog::*>*>(user);
    if (list != nullptr && list[0] != nullptr) {
        std::lock_guard<std::mutex> guard(slot->first->lock);
        slot->first->*(slot->second) = std::string(list[0]);
    }
}

struct Launcher {
    CommandLine cl;
    fs::path configPath;
    fs::path settingsPath;
    fs::path enginePath;
    LauncherConfig config;
    UserSettings settings;
    LaunchChoices choices;
    std::vector<DetectedInstall> installs;
    int selected = -1;
    std::string status;
    bool settingsDirty = false;
    bool configDirty = false;
    bool launched = false;

    // fonts
    std::vector<fs::path> fonts;
    ImFont* previewFont = nullptr;
    fs::path previewLoaded;
    char savesBuffer[512] = {};
    char fontBuffer[512] = {};

    Dialog dialog;
    std::pair<Dialog*, std::optional<std::string> Dialog::*> folderSlot{&dialog, &Dialog::folder};
    std::pair<Dialog*, std::optional<std::string> Dialog::*> fontSlot{&dialog, &Dialog::font};
    std::pair<Dialog*, std::optional<std::string> Dialog::*> savesSlot{&dialog, &Dialog::saves};

    void init()
    {
        configPath = cl.configFile;
        if (configPath.empty()) {
            configPath = prefFolder("launcher") / "launcher.ini";
        }
        config = LauncherConfig::load(configPath);
        choices = LaunchChoices::fromConfig(config);
        settingsPath = !cl.settingsFile.empty() ? cl.settingsFile : choices.settingsFile;
        if (settingsPath.empty()) {
            // Share the engine's own file unless this run keeps its files elsewhere.
            settingsPath = cl.configFile.empty() ? prefFolder("engine") / "settings.ini" : cl.configFile.parent_path() / "settings.ini";
        }
        choices.settingsFile = settingsPath;
        if (!cl.savesDir.empty()) {
            choices.savesDir = cl.savesDir;
        }
        enginePath = !cl.engine.empty() ? cl.engine : exeFolder() / "kotor_engine.exe";
        settings.load(settingsPath);
        settings.dirty = false;
        fonts = scanFonts(systemFontFolders());
        std::snprintf(savesBuffer, sizeof(savesBuffer), "%s", utf8(choices.savesDir).c_str());
        std::snprintf(fontBuffer, sizeof(fontBuffer), "%s", utf8(choices.fontFile).c_str());
        rescan();
    }

    void rescan()
    {
        DetectionSources sources = systemSources();
        for (const std::string& extra : config.extraInstalls()) {
            sources.installs.push_back({fromUtf8(extra), "browsed"});
        }
        if (!choices.install.empty()) {
            sources.installs.insert(sources.installs.begin(), {choices.install, "saved"});
        }
        if (!cl.install.empty()) {
            sources.installs.insert(sources.installs.begin(), {cl.install, "command line"});
        }
        installs = detectInstalls(sources);
        selected = -1;
        for (std::size_t i = 0; i < installs.size() && selected < 0; ++i) {
            if (!choices.install.empty() && installs[i].report.root == choices.install) {
                selected = static_cast<int>(i);
            }
        }
        if (!cl.install.empty() && !installs.empty()) {
            selected = 0;
        }
        for (std::size_t i = 0; i < installs.size() && selected < 0; ++i) {
            const GameDef* game = findGame(installs[i].gameId);
            if (installs[i].report.valid && game != nullptr && game->launchable) {
                selected = static_cast<int>(i);
            }
        }
        if (selected < 0 && !installs.empty()) {
            selected = 0;
        }
        select(selected);
        status = installs.empty() ? "No install was found. Use Browse to point at your game folder." : "";
    }

    void select(int index)
    {
        selected = index;
        if (index >= 0 && index < static_cast<int>(installs.size())) {
            choices.gameId = installs[static_cast<std::size_t>(index)].gameId;
            choices.install = installs[static_cast<std::size_t>(index)].report.root;
        }
    }

    bool saveAll()
    {
        bool ok = true;
        choices.toConfig(config);
        if (!config.save(configPath)) {
            status = "Could not write " + utf8(configPath);
            ok = false;
        }
        if (settings.dirty) {
            if (settings.save(settingsPath)) {
                settings.dirty = false;
            } else {
                status = "Could not write " + utf8(settingsPath);
                ok = false;
            }
        }
        configDirty = false;
        settingsDirty = false;
        return ok;
    }

    void addFolder(const std::string& folder)
    {
        const fs::path path = fromUtf8(folder);
        const GameDef* game = identifyInstall(path);
        if (game == nullptr) {
            status = "That folder does not look like a supported game data set (no chitin.key).";
            return;
        }
        config.addExtraInstall(folder);
        choices.install = path;
        choices.gameId = game->id;
        rescan();
        configDirty = true;
    }

    LaunchPlan plan() const { return buildLaunchPlan(choices, enginePath); }

    void play()
    {
        const LaunchPlan current = plan();
        if (!current.ok()) {
            status = current.problems.front();
            return;
        }
        if (!saveAll()) {
            return; // never start the engine on settings that did not save
        }
        std::vector<const char*> argv;
        for (const std::string& arg : current.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);
        SDL_Process* process = SDL_CreateProcess(argv.data(), false);
        if (process == nullptr) {
            status = std::string("Could not start the engine: ") + SDL_GetError();
            return;
        }
        SDL_DestroyProcess(process); // release our handle; the engine keeps running on its own
        launched = true;
    }

    void loadPreviewFont()
    {
        if (!choices.nativeFont || choices.fontFile.empty() || !isFontFile(choices.fontFile)) {
            return;
        }
        if (choices.fontFile == previewLoaded) {
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        std::error_code error;
        if (!fs::is_regular_file(choices.fontFile, error)) {
            return;
        }
        if (previewFont != nullptr) {
            io.Fonts->RemoveFont(previewFont);
            previewFont = nullptr;
        }
        const std::string file = utf8(choices.fontFile);
        previewFont = io.Fonts->AddFontFromFileTTF(file.c_str(), 18.0f);
        previewLoaded = choices.fontFile;
    }
};

const char* badge(const ValidationReport& report)
{
    if (!report.valid) {
        return "not usable";
    }
    return report.count(Severity::Warning) > 0 ? "usable, with notes" : "usable";
}

ImVec4 badgeColor(const ValidationReport& report)
{
    if (!report.valid) {
        return ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
    }
    return report.count(Severity::Warning) > 0 ? ImVec4(0.95f, 0.78f, 0.35f, 1.0f) : ImVec4(0.45f, 0.85f, 0.50f, 1.0f);
}

void drawPlayTab(Launcher& app)
{
    ImGui::TextWrapped("Pick the game install to run. The launcher reads it and never changes it; the engine builds nothing inside it.");
    ImGui::Spacing();
    if (ImGui::Button("Rescan")) {
        app.rescan();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse for a folder...")) {
        SDL_ShowOpenFolderDialog(folderPicked, &app.folderSlot, nullptr, nullptr, false);
    }
    ImGui::Spacing();

    ImGui::BeginChild("installs", ImVec2(0, 150), ImGuiChildFlags_Borders);
    if (app.installs.empty()) {
        ImGui::TextDisabled("Nothing found yet.");
    }
    for (std::size_t i = 0; i < app.installs.size(); ++i) {
        const DetectedInstall& install = app.installs[i];
        const GameDef* game = findGame(install.gameId);
        ImGui::PushID(static_cast<int>(i));
        const std::string line = (game ? game->name : install.gameId) + "   " + utf8(install.report.root);
        if (ImGui::Selectable(line.c_str(), app.selected == static_cast<int>(i))) {
            app.select(static_cast<int>(i));
            app.configDirty = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(badgeColor(install.report), "[%s]", badge(install.report));
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", install.origin.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextUnformatted("Validation report");
    ImGui::BeginChild("report", ImVec2(0, 190), ImGuiChildFlags_Borders);
    if (app.selected >= 0 && app.selected < static_cast<int>(app.installs.size())) {
        const ValidationReport& report = app.installs[static_cast<std::size_t>(app.selected)].report;
        ImGui::TextColored(badgeColor(report), "%s", badge(report));
        if (report.recognized) {
            ImGui::Text("Data set: %s", report.datasetId.c_str());
        } else if (!report.keySha256.empty()) {
            ImGui::TextDisabled("Data set not recognised (it may still work).");
        }
        if (report.languageId >= 0) {
            ImGui::Text("Language: %s   Strings: %zu   Archives: %zu", report.languageName.c_str(), report.tlkStrings, report.bifCount);
        }
        for (const Issue& issue : report.issues) {
            const ImVec4 color = issue.severity == Severity::Error ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                : issue.severity == Severity::Warning ? ImVec4(0.95f, 0.78f, 0.35f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", issue.message.c_str());
            ImGui::PopStyleColor();
        }
        const GameDef* game = findGame(report.gameId);
        if (game != nullptr && !game->launchable) {
            ImGui::TextWrapped("This game is recognised, but the engine cannot play it yet.");
        }
    } else {
        ImGui::TextDisabled("Select an install to see its report.");
    }
    ImGui::EndChild();

    ImGui::Spacing();
    const LaunchPlan plan = app.plan();
    ImGui::BeginDisabled(!plan.ok());
    if (ImGui::Button("Play", ImVec2(160, 40))) {
        app.play();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!plan.ok()) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "%s", plan.problems.front().c_str());
    } else {
        ImGui::TextDisabled("Saves options, starts the engine and closes the launcher.");
    }
}

void drawOptionsTab(Launcher& app)
{
    ImGui::BeginChild("options", ImVec2(0, -70));

    if (ImGui::CollapsingHeader("Remaster options", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Classic keeps the original interface font, scaled. Native uses a font from your own system for interface text. "
                           "No font is shipped with the engine.");
        int mode = app.choices.nativeFont ? 1 : 0;
        if (ImGui::RadioButton("Classic (default)", mode == 0)) {
            app.choices.nativeFont = false;
            app.configDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Native font", mode == 1)) {
            app.choices.nativeFont = true;
            app.configDirty = true;
        }
        if (app.choices.nativeFont) {
            const std::string current = app.choices.fontFile.empty() ? std::string("(choose a font)") : utf8(app.choices.fontFile.filename());
            if (ImGui::BeginCombo("Installed fonts", current.c_str())) {
                for (const fs::path& font : app.fonts) {
                    const bool picked = font == app.choices.fontFile;
                    if (ImGui::Selectable(utf8(font.filename()).c_str(), picked)) {
                        app.choices.fontFile = font;
                        std::snprintf(app.fontBuffer, sizeof(app.fontBuffer), "%s", utf8(font).c_str());
                        app.configDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputText("Font file", app.fontBuffer, sizeof(app.fontBuffer))) {
                app.choices.fontFile = fromUtf8(app.fontBuffer);
                app.configDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##font")) {
                SDL_ShowOpenFileDialog(folderPicked, &app.fontSlot, nullptr, nullptr, 0, nullptr, false);
            }
            std::error_code error;
            if (app.choices.fontFile.empty()) {
                ImGui::TextDisabled("No font chosen yet.");
            } else if (!isFontFile(app.choices.fontFile) || !fs::is_regular_file(app.choices.fontFile, error)) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "That is not a .ttf, .otf or .ttc font file.");
            } else {
                app.loadPreviewFont();
                if (app.previewFont != nullptr) {
                    ImGui::PushFont(app.previewFont, 24.0f);
                    ImGui::TextUnformatted("Preview: The quick brown fox jumps over the lazy dog 0123456789");
                    ImGui::PopFont();
                }
            }
        }
    }

    if (ImGui::CollapsingHeader("Launch", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* backends[] = {"auto", "vulkan", "gl"};
        int backend = 0;
        for (int i = 0; i < 3; ++i) {
            if (app.choices.backend == backends[i]) {
                backend = i;
            }
        }
        if (ImGui::Combo("Renderer", &backend, "Automatic\0Vulkan\0OpenGL\0")) {
            app.choices.backend = backends[backend];
            app.configDirty = true;
        }
        const std::string language = app.choices.language.empty() ? std::string("Install default") : app.choices.language;
        if (ImGui::BeginCombo("Language", language.c_str())) {
            if (ImGui::Selectable("Install default", app.choices.language.empty())) {
                app.choices.language.clear();
                app.configDirty = true;
            }
            for (std::uint32_t id = 0; id <= 12; ++id) {
                const std::string name = kotor::languageName(id);
                if (ImGui::Selectable(name.c_str(), app.choices.language == name)) {
                    app.choices.language = name;
                    app.configDirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::InputText("Saves folder (empty: default)", app.savesBuffer, sizeof(app.savesBuffer))) {
            app.choices.savesDir = fromUtf8(app.savesBuffer);
            app.configDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##saves")) {
            SDL_ShowOpenFolderDialog(folderPicked, &app.savesSlot, nullptr, nullptr, false);
        }
        ImGui::TextDisabled("Settings file: %s", utf8(app.settingsPath).c_str());
    }

    std::string group;
    bool open = false;
    for (const OptionDef& option : engineOptions()) {
        if (group != option.group) {
            group = option.group;
            open = ImGui::CollapsingHeader(option.group, group == "Display" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }
        if (!open) {
            continue;
        }
        float value = app.settings.get(option.key);
        bool changed = false;
        ImGui::PushID(option.key);
        switch (option.kind) {
        case OptionKind::Toggle: {
            bool on = value != 0.0f;
            changed = ImGui::Checkbox(option.label, &on);
            value = on ? 1.0f : 0.0f;
            break;
        }
        case OptionKind::Slider: {
            int v = static_cast<int>(value);
            changed = ImGui::SliderInt(option.label, &v, static_cast<int>(option.minimum), static_cast<int>(option.maximum));
            value = static_cast<float>(v);
            break;
        }
        case OptionKind::Integer: {
            int v = static_cast<int>(value);
            changed = ImGui::InputInt(option.label, &v, 10, 100);
            v = std::clamp(v, static_cast<int>(option.minimum), static_cast<int>(option.maximum));
            value = static_cast<float>(v);
            break;
        }
        case OptionKind::Choice: {
            int v = std::clamp(static_cast<int>(value), static_cast<int>(option.minimum), static_cast<int>(option.maximum));
            const char* label = option.choices[static_cast<std::size_t>(v)];
            if (ImGui::BeginCombo(option.label, label)) {
                for (std::size_t i = 0; i < option.choices.size(); ++i) {
                    if (ImGui::Selectable(option.choices[i], static_cast<int>(i) == v)) {
                        value = static_cast<float>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        }
        ImGui::PopID();
        if (changed) {
            app.settings.set(option.key, value);
            app.settingsDirty = true;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Save options")) {
        if (app.saveAll()) {
            app.status = "Saved.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore engine defaults")) {
        for (const OptionDef& option : engineOptions()) {
            const auto found = UserSettings::defaults().find(option.key);
            app.settings.set(option.key, found != UserSettings::defaults().end() ? found->second : 0.0f);
        }
        app.settingsDirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", (app.settings.dirty || app.configDirty) ? "unsaved changes" : "");
}

void drawFrame(Launcher& app, const std::string& tab)
{
    {
        std::lock_guard<std::mutex> guard(app.dialog.lock);
        if (app.dialog.folder) {
            app.addFolder(*app.dialog.folder);
            app.dialog.folder.reset();
        }
        if (app.dialog.font) {
            app.choices.fontFile = fromUtf8(*app.dialog.font);
            std::snprintf(app.fontBuffer, sizeof(app.fontBuffer), "%s", app.dialog.font->c_str());
            app.configDirty = true;
            app.dialog.font.reset();
        }
        if (app.dialog.saves) {
            app.choices.savesDir = fromUtf8(*app.dialog.saves);
            std::snprintf(app.savesBuffer, sizeof(app.savesBuffer), "%s", app.dialog.saves->c_str());
            app.configDirty = true;
            app.dialog.saves.reset();
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Engine launcher");
    ImGui::Separator();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Play", nullptr, tab == "play" ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawPlayTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Options", nullptr, tab == "options" ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawOptionsTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!app.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app.status.c_str());
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() * 2.0f);
    ImGui::TextDisabled("Independent open-source engine. Not affiliated with or endorsed by the makers or publishers of any game it can run.");
    ImGui::TextDisabled("It needs your own copy of the game and ships none of its files.");
    ImGui::End();
}

int runList(Launcher& app)
{
    for (const DetectedInstall& install : app.installs) {
        std::cout << formatReport(install.report) << "  found via: " << install.origin << "\n";
    }
    if (app.installs.empty()) {
        std::cout << "no installs found\n";
    }
    return 0;
}

int runWindow(Launcher& app)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return kExitSkip;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (app.cl.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    SDL_Window* window = SDL_CreateWindow("Engine launcher", 900, 720, flags);
    if (window == nullptr) {
        std::cerr << "window failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return kExitSkip;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) {
        std::cerr << "OpenGL context failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return kExitSkip;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(app.cl.hidden ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // never write layout files beside the player's profile
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const float scale = SDL_GetWindowDisplayScale(window);
    ImGui::GetStyle().ScaleAllSizes(scale > 0.0f ? scale : 1.0f);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 330");

    int frame = 0;
    bool running = true;
    while (running && !app.launched) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawFrame(app, app.cl.tab);
        ImGui::Render();
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.09f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frame;
        if (!app.cl.screenshot.empty() && app.cl.frames > 0 && frame == app.cl.frames) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (!writePng(fromUtf8(app.cl.screenshot), width, height, pixels)) {
                std::cerr << "could not write " << app.cl.screenshot << "\n";
            }
        }
        SDL_GL_SwapWindow(window);
        if (app.cl.frames > 0 && frame >= app.cl.frames) {
            running = false;
        }
    }

    if (app.configDirty || app.settings.dirty) {
        app.saveAll();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "launcher frames: " << frame << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Launcher app;
    app.cl = parseCommandLine(argc, argv);
    if (!app.cl.error.empty()) {
        std::cerr << app.cl.error << "\n";
        return 2;
    }
    if (!app.cl.check.empty()) {
        const fs::path folder = fromUtf8(app.cl.check);
        const GameDef* game = identifyInstall(folder);
        if (game == nullptr) {
            std::cout << "not a recognised game data set: " << app.cl.check << "\n";
            return 2;
        }
        const ValidationReport report = validateInstall(folder, *game);
        std::cout << formatReport(report);
        return report.valid ? 0 : 2;
    }
    app.init();
    if (app.cl.list) {
        return runList(app);
    }
    if (app.cl.printCommand) {
        const LaunchPlan plan = app.plan();
        for (const std::string& problem : plan.problems) {
            std::cout << "problem: " << problem << "\n";
        }
        for (const std::string& arg : plan.args) {
            std::cout << arg << "\n";
        }
        return plan.ok() ? 0 : 2;
    }
    return runWindow(app);
}
