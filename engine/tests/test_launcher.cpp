// Launcher tests: install detection and data-set validation, launcher config, launch plan, settings
// round trip. Fixtures are tiny synthetic data sets built under the work folder (argv[1]); no game
// file is copied or written. When a real install exists (KOTOR_INSTALL or the Steam path) it is read
// only, to prove the validator accepts a real data set without the executable.

#include "game/UserSettings.h"
#include "kotor/Formats.h"
#include "kotor/Profile.h"
#include "launcher/InstallDetect.h"
#include "launcher/LauncherConfig.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ke;
using namespace ke::launcher;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
}

void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value)
{
    out[at] = static_cast<std::uint8_t>(value & 0xff);
    out[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeText(const fs::path& path, const std::string& text)
{
    writeBytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

// A KEY V1 file naming the given archives, each declared as `size` bytes.
std::vector<std::uint8_t> makeKey(const std::vector<std::string>& bifNames, std::uint32_t size)
{
    std::size_t namesAt = 64 + bifNames.size() * 12;
    std::size_t namesLength = 0;
    for (const std::string& name : bifNames) {
        namesLength += name.size() + 1;
    }
    const std::size_t keysAt = namesAt + namesLength;
    std::vector<std::uint8_t> key(keysAt + 22, 0);
    const std::string magic = "KEY V1  ";
    std::copy(magic.begin(), magic.end(), key.begin());
    put32(key, 8, static_cast<std::uint32_t>(bifNames.size()));
    put32(key, 12, 1);
    put32(key, 16, 64);
    put32(key, 20, static_cast<std::uint32_t>(keysAt));
    for (std::size_t i = 0; i < bifNames.size(); ++i) {
        put32(key, 64 + i * 12, size);
        put32(key, 64 + i * 12 + 4, static_cast<std::uint32_t>(namesAt));
        put16(key, 64 + i * 12 + 8, static_cast<std::uint16_t>(bifNames[i].size() + 1));
        std::copy(bifNames[i].begin(), bifNames[i].end(), key.begin() + static_cast<std::ptrdiff_t>(namesAt));
        namesAt += bifNames[i].size() + 1;
    }
    return key;
}

std::vector<std::uint8_t> makeTlk(std::uint32_t language, std::uint32_t count)
{
    std::vector<std::uint8_t> tlk(20 + static_cast<std::size_t>(count) * 40 + 8, 0);
    const std::string magic = "TLK V3.0";
    std::copy(magic.begin(), magic.end(), tlk.begin());
    put32(tlk, 8, language);
    put32(tlk, 12, count);
    put32(tlk, 16, static_cast<std::uint32_t>(20 + static_cast<std::size_t>(count) * 40));
    return tlk;
}

struct Fixture {
    fs::path root;
};

// A synthetic KotOR 1 shaped data set. There is no executable in it.
Fixture makeK1(const fs::path& root, bool patch = true, bool bifs = true)
{
    fs::remove_all(root);
    fs::create_directories(root / "modules");
    fs::create_directories(root / "rims");
    fs::create_directories(root / "lips");
    fs::create_directories(root / "TexturePacks");
    fs::create_directories(root / "streammusic");
    fs::create_directories(root / "streamwaves");
    writeBytes(root / "chitin.key", makeKey({"data\\one.bif", "data\\two.bif"}, 16));
    writeBytes(root / "dialog.tlk", makeTlk(0, 3));
    if (patch) {
        writeText(root / "patch.erf", "ERF V1.0");
    }
    fs::create_directories(root / "data");
    if (bifs) {
        writeBytes(root / "data" / "one.bif", std::vector<std::uint8_t>(16, 1));
        writeBytes(root / "data" / "two.bif", std::vector<std::uint8_t>(16, 2));
    }
    return {root};
}

bool hasIssue(const ValidationReport& report, const std::string& code)
{
    return std::any_of(report.issues.begin(), report.issues.end(), [&](const Issue& issue) { return issue.code == code; });
}

fs::path realInstall()
{
    std::vector<fs::path> options;
    if (const std::string value = environmentValue("KOTOR_INSTALL"); !value.empty()) {
        options.emplace_back(value);
    }
    options.emplace_back("E:/SteamLibrary/steamapps/common/swkotor");
    for (const fs::path& path : options) {
        std::error_code error;
        if (fs::is_regular_file(path / "chitin.key", error)) {
            return path;
        }
    }
    return {};
}

void testValidation(const fs::path& work)
{
    const GameDef& k1 = *findGame("kotor1");
    const Fixture good = makeK1(work / "k1_good");
    ValidationReport report = validateInstall(good.root, k1);
    check(report.valid, "fixture without an executable validates");
    check(!fs::exists(good.root / "swkotor.exe"), "fixture has no executable");
    check(report.bifCount == 2 && report.keyEntries == 1, "key counts read");
    check(report.languageId == 0 && report.languageName == "English" && report.tlkStrings == 3, "tlk header read");
    check(!report.recognized && hasIssue(report, "unknown_dataset"), "unknown key hash is a warning, not an error");
    check(report.count(Severity::Error) == 0 && !report.modded(), "no error and not modded");
    check(formatReport(report).find("usable") != std::string::npos, "report text says usable");

    // The same fixture is recognised when its key hash is a known data set.
    GameDef custom = k1;
    custom.known = {{kotor::sha256File(good.root / "chitin.key"), "fixture-set", "9.9", 0}};
    report = validateInstall(good.root, custom);
    check(report.recognized && report.datasetId == "fixture-set" && report.version == "9.9", "known hash is recognised");
    check(!hasIssue(report, "unknown_dataset"), "recognised set raises no unknown warning");
    custom.known[0].languageId = 1;
    check(hasIssue(validateInstall(good.root, custom), "language_mismatch"), "language differing from the reference warns");

    // Missing pieces are errors that name the piece.
    const Fixture noPatch = makeK1(work / "k1_nopatch", false);
    report = validateInstall(noPatch.root, k1);
    check(!report.valid && hasIssue(report, "missing_file"), "missing patch.erf rejects");
    const Fixture noBif = makeK1(work / "k1_nobif", true, false);
    report = validateInstall(noBif.root, k1);
    check(!report.valid && hasIssue(report, "no_bif") && hasIssue(report, "missing_bif"), "missing archives reject");
    fs::remove_all(work / "k1_nodirs");
    fs::create_directories(work / "k1_nodirs");
    check(!validateInstall(work / "k1_nodirs", k1).valid, "empty folder rejects");
    check(!validateInstall(work / "does_not_exist", k1).valid, "missing folder rejects");

    // A truncated key file is an error, not a crash.
    {
        const Fixture cut = makeK1(work / "k1_cutkey");
        std::vector<std::uint8_t> key = makeKey({"data\\one.bif"}, 16);
        key.resize(40);
        writeBytes(cut.root / "chitin.key", key);
        check(hasIssue(validateInstall(cut.root, k1), "key_format"), "truncated key reports key_format");
        writeText(cut.root / "chitin.key", "not a key file, not even close to one, padded to be long enough to pass 64 bytes.....");
        check(hasIssue(validateInstall(cut.root, k1), "key_format"), "wrong key magic reports key_format");
        writeText(cut.root / "dialog.tlk", "short");
        check(hasIssue(validateInstall(cut.root, k1), "tlk_format"), "short tlk reports tlk_format");
    }

    // A size that differs from the declared one is a warning (patched or modified), not a rejection.
    {
        const Fixture patched = makeK1(work / "k1_resized");
        writeBytes(patched.root / "data" / "one.bif", std::vector<std::uint8_t>(40, 1));
        report = validateInstall(patched.root, k1);
        check(report.valid && hasIssue(report, "bif_size"), "resized archive warns");
    }

    // Override content is a warning and is counted; nothing is removed.
    {
        const Fixture modded = makeK1(work / "k1_modded");
        writeText(modded.root / "Override" / "x.tga", "x");
        writeText(modded.root / "Override" / "y.2da", "y");
        report = validateInstall(modded.root, k1);
        check(report.valid && report.modded() && report.overrideFiles == 2 && hasIssue(report, "modded"), "override files warn");
        check(fs::exists(modded.root / "Override" / "x.tga"), "validation leaves override files alone");
    }

    // Folder and file case does not matter (case-sensitive disks).
    {
        const Fixture mixed = makeK1(work / "k1_case");
        fs::rename(mixed.root / "modules", mixed.root / "Modules_tmp");
        fs::rename(mixed.root / "Modules_tmp", mixed.root / "MODULES");
        fs::rename(mixed.root / "data", mixed.root / "Data_tmp");
        fs::rename(mixed.root / "Data_tmp", mixed.root / "DATA");
        check(validateInstall(mixed.root, k1).valid, "mixed-case folders validate");
    }
}

void testIdentifyAndDetect(const fs::path& work)
{
    const Fixture k1 = makeK1(work / "id_k1");
    const GameDef* found = identifyInstall(k1.root);
    check(found != nullptr && found->id == "kotor1", "K1 shaped folder is identified as kotor1");

    // A KotOR 2 shaped folder (no patch.erf, no rims) is identified as kotor2 but cannot be launched.
    const fs::path k2 = work / "id_k2";
    fs::remove_all(k2);
    fs::create_directories(k2 / "modules");
    fs::create_directories(k2 / "data");
    writeBytes(k2 / "chitin.key", makeKey({"data\\a.bif"}, 8));
    writeBytes(k2 / "dialog.tlk", makeTlk(0, 1));
    writeBytes(k2 / "data" / "a.bif", std::vector<std::uint8_t>(8, 0));
    found = identifyInstall(k2);
    check(found != nullptr && found->id == "kotor2" && !found->launchable, "K2 shaped folder is kotor2, not launchable");
    check(identifyInstall(work / "id_none") == nullptr, "missing folder is no game");

    // Steam: libraryfolders.vdf in the main root lists a second library; the appmanifest names the folder.
    const fs::path steam = work / "steam_main";
    const fs::path lib2 = work / "steam_lib2";
    fs::remove_all(steam);
    fs::remove_all(lib2);
    fs::create_directories(steam / "steamapps");
    std::string lib2Text = lib2.generic_string();
    writeText(steam / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam.generic_string() + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\""
                  + lib2Text + "\"\n\t}\n}\n");
    writeText(lib2 / "steamapps" / "appmanifest_32370.acf", "\"AppState\"\n{\n\t\"appid\"\t\t\"32370\"\n\t\"installdir\"\t\t\"custom folder\"\n}\n");
    makeK1(lib2 / "steamapps" / "common" / "custom folder");

    DetectionSources sources;
    sources.steamRoots = {steam};
    std::vector<DetectedInstall> detected = detectInstalls(sources);
    check(detected.size() == 1 && detected[0].gameId == "kotor1" && detected[0].origin == "steam" && detected[0].report.valid,
          "steam library from libraryfolders.vdf and appmanifest installdir is found");

    // The same folder given again (browse, saved path, registry) is not listed twice; a second one is.
    const Fixture extra = makeK1(work / "extra_install");
    sources.installs = {{lib2 / "steamapps" / "common" / "custom folder", "saved"}, {extra.root, "browsed"},
                        {work / "nowhere", "registry"}, {work / "id_none", "gog"}};
    detected = detectInstalls(sources);
    check(detected.size() == 2, "duplicates collapse, missing folders are skipped");
    check(detected.size() == 2 && detected[1].origin == "browsed" && detected[1].report.valid, "browsed install is listed with its origin");

    // A generic root with a default folder name.
    DetectionSources common;
    common.commonRoots = {work / "common_root"};
    makeK1(work / "common_root" / "swkotor");
    detected = detectInstalls(common);
    check(detected.size() == 1 && detected[0].origin == "common", "default folder name under a common root is found");

    // Text parsers.
    const std::vector<fs::path> libs = parseLibraryFolders("\"path\"\t\t\"D:\\\\Games\\\\Lib\"\n\"path\" \"E:/Other\"\n\"label\" \"x\"");
    check(libs.size() == 2 && libs[0].generic_string() == "D:/Games/Lib" && libs[1].generic_string() == "E:/Other",
          "vdf paths are unescaped");
    check(parseInstallDir("\"installdir\" \"abc def\"") == "abc def" && parseInstallDir("nothing").empty(), "installdir parsed");

    // The real machine sources must be readable without throwing.
    const DetectionSources machine = systemSources();
    check(machine.steamRoots.size() < 100, "systemSources returns");
}

void testRealInstall(const fs::path& work)
{
    const fs::path real = realInstall();
    if (real.empty()) {
        std::cout << "SKIP real install checks: no install found\n";
        return;
    }
    const GameDef& k1 = *findGame("kotor1");
    const ValidationReport report = validateInstall(real, k1);
    std::cout << formatReport(report);
    check(report.valid, "real install validates");
    check(report.recognized && report.datasetId == "kotor1-pc-1.0.3.0-english", "real install matches the reference data set");
    check(report.languageId == 0 && report.tlkStrings > 1000, "real install language read");
    check(identifyInstall(real) != nullptr && identifyInstall(real)->id == "kotor1", "real install identified");

    DetectionSources sources;
    sources.installs = {{real, "environment"}};
    check(detectInstalls(sources).size() == 1, "real install is detected from a direct path");

    // ADR 0003: the engine's own profile detection must accept the data set without the executable.
    const fs::path keyOnly = work / "real_key_only";
    fs::remove_all(keyOnly);
    fs::create_directories(keyOnly);
    fs::copy_file(real / "chitin.key", keyOnly / "chitin.key");
    const kotor::GameProfile profile = kotor::GameProfile::detect(keyOnly);
    check(profile.kind == kotor::GameKind::KotOR1 && profile.recognized && profile.executable.empty(),
          "profile detection identifies the data set without swkotor.exe");
    check(profile.supportsWorldLoad(), "data-set profile supports world load");
    const kotor::GameProfile none = kotor::GameProfile::detect(work / "k1_good");
    check(none.kind == kotor::GameKind::Unknown && !none.recognized, "unknown key hash stays unknown");
    fs::remove_all(keyOnly);
}

void testConfig(const fs::path& work)
{
    fs::create_directories(work);
    const fs::path file = work / "launcher.ini";
    fs::remove(file);
    check(LauncherConfig::load(file).get("game").empty(), "missing config is empty");

    LauncherConfig config;
    config.set("unknown.key", "kept");
    config.set("note", "line\nbreak");
    LaunchChoices choices;
    choices.gameId = "kotor1";
    choices.install = work / "k1_good";
    choices.settingsFile = work / "settings.ini";
    choices.savesDir = work / "saves";
    choices.backend = "gl";
    choices.language = "english";
    choices.nativeFont = true;
    choices.fontFile = work / "fonts" / "some font.ttf";
    choices.toConfig(config);
    config.addExtraInstall("D:/a");
    config.addExtraInstall("D:/b");
    config.addExtraInstall("D:/a");
    check(config.save(file), "config saves");
    check(!fs::exists(file.string() + ".tmp"), "no temporary file is left behind");

    const LauncherConfig back = LauncherConfig::load(file);
    check(back.get("unknown.key") == "kept", "unknown keys survive");
    check(back.get("note") == "linebreak", "values cannot hold line breaks");
    const LaunchChoices read = LaunchChoices::fromConfig(back);
    check(read.gameId == "kotor1" && read.install == choices.install && read.settingsFile == choices.settingsFile
              && read.savesDir == choices.savesDir && read.backend == "gl" && read.language == "english"
              && read.nativeFont && read.fontFile == choices.fontFile,
          "launch choices round trip, including a path with a space");
    check(back.extraInstalls().size() == 2 && back.extraInstalls()[1] == "D:/b", "extra installs are listed once");
    check(back.getBool("remaster.native_font", false) && back.getInt("missing", 7) == 7, "typed getters");

    // Classic is the default: a fresh config means no native font.
    const LaunchChoices fresh = LaunchChoices::fromConfig(LauncherConfig{});
    check(!fresh.nativeFont && fresh.fontFile.empty() && fresh.backend == "auto", "classic is the default");
}

void testPlan(const fs::path& work)
{
    const Fixture good = makeK1(work / "plan_k1");
    const fs::path engine = work / "plan_engine.exe";
    writeText(engine, "stub");
    writeText(work / "plan_font.ttf", "stub");
    writeText(work / "plan_font.txt", "stub");

    LaunchChoices choices;
    choices.install = good.root;
    choices.settingsFile = work / "plan_settings.ini";
    choices.savesDir = work / "plan_saves";
    LaunchPlan plan = buildLaunchPlan(choices, engine);
    check(plan.ok(), "plan for a valid install is ok");
    auto has = [&](const std::string& arg) { return std::find(plan.args.begin(), plan.args.end(), arg) != plan.args.end(); };
    check(plan.args.size() > 4 && plan.args[0] == engine.generic_string(), "engine is the first argument");
    check(has("--install") && has(good.root.generic_string()) && has("--settings-file") && has("--saves-dir") && has("--backend=auto"),
          "install, settings, saves and backend are passed");
    check(!has("--ui-font") && !has("--language"), "classic font and default language pass nothing extra");

    choices.nativeFont = true;
    choices.fontFile = work / "plan_font.ttf";
    choices.backend = "vulkan";
    choices.language = "french";
    plan = buildLaunchPlan(choices, engine);
    check(plan.ok() && has("--ui-font") && has(choices.fontFile.generic_string()) && has("--backend=vulkan") && has("french"),
          "native font, backend and language are passed");

    choices.fontFile = work / "plan_font.txt";
    check(!buildLaunchPlan(choices, engine).ok(), "a non-font file is refused");
    choices.fontFile = work / "missing.ttf";
    check(!buildLaunchPlan(choices, engine).ok(), "a missing font file is refused");
    choices.nativeFont = false;
    choices.backend = "directx";
    check(!buildLaunchPlan(choices, engine).ok(), "unknown backend is refused");
    choices.backend = "auto";
    choices.language = "klingon";
    check(!buildLaunchPlan(choices, engine).ok(), "unknown language is refused");
    choices.language.clear();
    check(!buildLaunchPlan(choices, work / "no_engine.exe").ok(), "missing engine is refused");
    choices.install = work / "k1_nopatch";
    check(!buildLaunchPlan(choices, engine).ok(), "invalid install is refused");
    choices.install.clear();
    check(!buildLaunchPlan(choices, engine).ok(), "no install is refused");
    choices.install = good.root;
    choices.gameId = "kotor2";
    check(!buildLaunchPlan(choices, engine).ok(), "kotor2 is detected but not launchable");
    choices.gameId = "nothing";
    check(!buildLaunchPlan(choices, engine).ok(), "unknown game is refused");
}

void testSettings(const fs::path& work)
{
    fs::create_directories(work);
    const fs::path file = work / "settings.ini";
    fs::remove(file);

    UserSettings settings;
    check(!settings.load(file), "missing settings file reports false");
    check(settings.get("sound.music") == 80.0f && settings.get("gameplay.difficulty") == 1.0f, "defaults apply");
    settings.set("sound.music", 35.0f);
    settings.set("graphics.width", 1920.0f);
    settings.set("graphics.height", 1080.0f);
    settings.set("graphics.fullscreen", 1.0f);
    settings.set("autopause.endround", 1.0f);
    settings.seenMovies.insert("movie_a");
    check(settings.dirty && settings.save(file), "settings save");
    check(!fs::exists(file.string() + ".tmp"), "settings save leaves no temporary file");

    UserSettings back;
    check(back.load(file), "settings load");
    check(back.get("sound.music") == 35.0f && back.get("graphics.width") == 1920.0f && back.get("graphics.height") == 1080.0f
              && back.get("graphics.fullscreen") == 1.0f && back.get("autopause.endround") == 1.0f,
          "values round trip");
    check(back.get("sound.voice") == 100.0f && back.seenMovies.count("movie_a") == 1, "untouched keys and seen movies survive");
    check(!back.dirty, "a loaded file is not dirty");

    // A second save replaces the first.
    back.set("sound.music", 10.0f);
    check(back.save(file), "second save replaces the file");
    UserSettings again;
    again.load(file);
    check(again.get("sound.music") == 10.0f, "second save is what loads");

    // The options screen only offers keys the engine knows.
    const std::set<std::string> displayOnly = {"graphics.fullscreen", "graphics.width", "graphics.height"};
    std::set<std::string> seen;
    bool known = true;
    bool unique = true;
    for (const OptionDef& option : engineOptions()) {
        const bool inDefaults = UserSettings::defaults().count(option.key) == 1;
        known = known && (inDefaults || displayOnly.count(option.key) == 1);
        unique = unique && seen.insert(option.key).second;
        known = known && option.maximum > option.minimum;
    }
    check(known, "every option key exists in the engine settings (or is a display key)");
    check(unique, "no option appears twice");
    bool covered = true;
    for (const auto& [key, value] : UserSettings::defaults()) {
        (void)value;
        covered = covered && seen.count(key) == 1;
    }
    check(covered, "every engine setting has an option on the screen");
}

void testFonts(const fs::path& work)
{
    const fs::path folder = work / "fonts";
    fs::remove_all(folder);
    writeText(folder / "b.ttf", "x");
    writeText(folder / "A.OTF", "x");
    writeText(folder / "c.txt", "x");
    writeText(folder / "d.ttc", "x");
    const std::vector<fs::path> fonts = scanFonts({folder, work / "no_such_folder"});
    check(fonts.size() == 3 && fonts[0].filename() == "A.OTF" && fonts[1].filename() == "b.ttf" && fonts[2].filename() == "d.ttc",
          "font scan keeps font files, sorted, ignoring missing folders");
    check(isFontFile("x.TTF") && !isFontFile("x.woff") && !isFontFile("x"), "font extensions");
}

} // namespace

int main(int argc, char** argv)
{
    const fs::path work = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("launcher_test_work"));
    fs::create_directories(work);
    testValidation(work);
    testIdentifyAndDetect(work);
    testRealInstall(work);
    testConfig(work);
    testPlan(work);
    testSettings(work);
    testFonts(work);
    std::cout << "launcher tests: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
