#include "app/Args.h"

#include "kotor/Localization.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace ke {

namespace {

bool parseInteger(const char* text, int minimum, int& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum
        || value > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool parseDouble(const char* text, double minimum, double& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || value < minimum) {
        return false;
    }
    out = value;
    return true;
}

ParseResult fail(ParseResult result, std::string message)
{
    result.action = ParseAction::Error;
    result.error = std::move(message);
    return result;
}

} // namespace

ParseResult parseArguments(int argc, char** argv)
{
    ParseResult result;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto requireValue = [&]() -> const char* {
            if (i + 1 >= argc) {
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (arg == "--help" || arg == "-h") {
            result.action = ParseAction::Help;
            return result;
        }
        if (arg == "--version") {
            result.action = ParseAction::Version;
            return result;
        }
        if (arg == "--vulkan") {
            result.options.backend = BackendKind::Vulkan;
            continue;
        }
        if (arg == "--gl" || arg == "--opengl") {
            result.options.backend = BackendKind::OpenGL;
            continue;
        }
        if (arg == "--null") {
            result.options.backend = BackendKind::Null;
            continue;
        }
        if (arg == "--headless") {
            result.options.headless = true;
            continue;
        }
        if (arg == "--hidden") {
            result.options.hidden = true;
            continue;
        }
        if (arg == "--validate") {
            result.options.validate = true;
            continue;
        }
        if (arg == "--vsync") {
            result.options.vsync = true;
            continue;
        }
        if (arg == "--no-vsync") {
            result.options.vsync = false;
            continue;
        }
        if (arg == "--no-fallback") {
            result.options.allowFallback = false;
            continue;
        }
        if (arg == "--self-test") {
            result.options.selfTest = true;
            continue;
        }
        if (arg == "--scene-stats") {
            result.options.sceneStats = true;
            continue;
        }
        if (arg == "--content-stats") {
            result.options.contentStats = true;
            continue;
        }
        if (arg == "--walk-test") {
            result.options.walkTest = true;
            continue;
        }
        if (arg == "--door-test") {
            result.options.doorTest = true;
            continue;
        }
        if (arg == "--dialogue-test") {
            result.options.dialogueTest = true;
            continue;
        }
        if (arg == "--combat-test") {
            result.options.combatTest = true;
            continue;
        }
        if (arg == "--playthrough") {
            result.options.playthrough = true;
            continue;
        }
        if (arg == "--landmark-test") {
            result.options.landmarkTest = true;
            continue;
        }
        if (arg == "--scene-assert") {
            result.options.sceneAssert = true;
            continue;
        }
        if (arg == "--script-test") {
            result.options.scriptTest = true;
            continue;
        }
        if (arg == "--rules-test") {
            result.options.rulesTest = true;
            continue;
        }
        if (arg == "--systems-test") {
            result.options.systemsTest = true;
            continue;
        }
        if (arg == "--merchant-test") {
            result.options.merchantTest = true;
            continue;
        }
        if (arg == "--encounter-test") {
            result.options.encounterTest = true;
            continue;
        }
        if (arg == "--encounter-reset-test") {
            result.options.encounterResetTest = true;
            continue;
        }
        if (arg == "--party-test") {
            result.options.partyTest = true;
            continue;
        }
        if (arg == "--ui-test") {
            result.options.uiTest = true;
            continue;
        }
        if (arg == "--loot-test") {
            result.options.lootTest = true;
            continue;
        }
        if (arg == "--combat-force-test") {
            result.options.combatForceTest = true;
            continue;
        }
        if (arg == "--animation-test") {
            result.options.animationTest = true;
            continue;
        }
        if (arg == "--audio-test") {
            result.options.audioTest = true;
            continue;
        }
        if (arg == "--media-test") {
            result.options.mediaTest = true;
            continue;
        }
        if (arg == "--movie-play") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--movie-play requires a movie resref");
            }
            result.options.moviePlay = value;
            continue;
        }
        if (arg == "--movie-shot") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--movie-shot requires a BMP path");
            }
            result.options.movieShot = value;
            continue;
        }
        if (arg == "--movie-time") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--movie-time requires seconds");
            }
            result.options.movieTime = std::strtof(value, nullptr);
            continue;
        }
        if (arg == "--facial-demo") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--facial-demo requires a viseme 0..15");
            }
            const long viseme = std::strtol(value, nullptr, 10);
            if (viseme < 0 || viseme > 15) {
                return fail(std::move(result), "--facial-demo viseme must be 0..15");
            }
            result.options.facialDemo = static_cast<int>(viseme);
            continue;
        }
        if (arg == "--language") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--language requires an id or name");
            }
            const int id = kotor::languageId(value);
            if (id < 0) {
                return fail(std::move(result),
                            std::string("unknown language '") + value + "'");
            }
            result.options.language = id;
            continue;
        }
        if (arg == "--gui-test") {
            result.options.guiTest = true;
            continue;
        }
        if (arg == "--minigame-test") {
            result.options.minigameTest = true;
            continue;
        }
        if (arg == "--localization-test") {
            result.options.localizationTest = true;
            continue;
        }
        if (arg == "--vfs-test") {
            result.options.vfsTest = true;
            continue;
        }
        if (arg == "--k2-test") {
            result.options.k2Test = true;
            continue;
        }
        if (arg == "--faction-test") {
            result.options.factionTest = true;
            continue;
        }
        if (arg == "--save-matrix-test") {
            result.options.saveMatrixTest = true;
            continue;
        }
        if (arg == "--dialogue-corpus-test") {
            result.options.dialogueCorpusTest = true;
            continue;
        }
        if (arg == "--differential-test") {
            result.options.differentialTest = true;
            continue;
        }
        if (arg == "--retail-differential-test") {
            result.options.retailDifferentialTest = true;
            continue;
        }
        if (arg == "--module-trace") {
            result.options.moduleTrace = true;
            continue;
        }
        if (arg == "--engine-trace") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--engine-trace requires a path");
            }
            result.options.engineTrace = value;
            continue;
        }
        if (arg == "--save-fixture") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--save-fixture requires a save folder");
            }
            result.options.saveFixture = value;
            continue;
        }
        if (arg == "--module-playthrough-test") {
            result.options.modulePlaythroughTest = true;
            continue;
        }
        if (arg == "--bug-parity-test") {
            result.options.bugParityTest = true;
            continue;
        }
        if (arg == "--chargen") {
            result.options.chargen = true;
            continue;
        }
        if (arg == "--chargen-test") {
            result.options.chargenTest = true;
            continue;
        }
        if (arg == "--third-person") {
            result.options.thirdPerson = true;
            continue;
        }
        if (arg == "--no-movies") {
            result.options.noMovies = true;
            continue;
        }
        if (arg == "--camera-noclip") {
            result.options.cameraNoClip = true;
            continue;
        }
        if (arg == "--player-appearance") {
            const char* value = requireValue();
            int appearance = 0;
            if (!parseInteger(value, 0, appearance) || appearance > 508) {
                return fail(std::move(result),
                            "--player-appearance requires a row 0..508");
            }
            result.options.playerAppearance = appearance;
            continue;
        }
        if (arg == "--player-name") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--player-name requires text");
            }
            result.options.playerName = value;
            continue;
        }
        if (arg == "--save-fixtures") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--save-fixtures requires a directory");
            }
            result.options.saveFixtures = value;
            continue;
        }
        if (arg == "--faction-script") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--faction-script requires an .ncs path");
            }
            result.options.factionScript = value;
            continue;
        }
        if (arg == "--vfs-fixtures") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--vfs-fixtures requires a directory");
            }
            result.options.vfsFixtures = value;
            continue;
        }
        if (arg == "--minigame-script") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--minigame-script requires an .ncs path");
            }
            result.options.minigameScript = value;
            continue;
        }
        if (arg == "--gui-screen") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--gui-screen requires a resref");
            }
            result.options.guiScreen = value;
            continue;
        }
        if (arg == "--gui-action") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--gui-action requires a control tag");
            }
            result.options.guiAction = value;
            continue;
        }
        if (arg == "--audio-script") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--audio-script requires an .ncs path");
            }
            result.options.audioScript = value;
            continue;
        }
        if (arg == "--animation-script") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--animation-script requires an .ncs path");
            }
            result.options.animationScript = value;
            continue;
        }
        if (arg == "--combat-force-script") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--combat-force-script requires an .ncs path");
            }
            result.options.combatForceScript = value;
            continue;
        }
        if (arg == "--delayed-test") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--delayed-test requires an .ncs path");
            }
            result.options.delayedTest = value;
            continue;
        }
        if (arg == "--panel") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--panel requires a panel name");
            }
            result.options.panel = value;
            continue;
        }
        if (arg == "--add-party") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--add-party requires a tag");
            }
            result.options.addParty = value;
            continue;
        }
        if (arg == "--demo-dialogue") {
            result.options.demoDialogue = true;
            continue;
        }
        if (arg == "--demo-combat") {
            result.options.demoCombat = true;
            continue;
        }
        if (arg == "--load") {
            result.options.loadSave = true;
            continue;
        }
        if (arg == "--vm-corpus") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--vm-corpus requires a directory of .ncs files");
            }
            result.options.vmCorpus = value;
            continue;
        }
        if (arg == "--vm-out") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--vm-out requires a JSONL path");
            }
            result.options.vmOut = value;
            continue;
        }
        if (arg == "--vm-functional") {
            result.options.vmFunctional = true;
            continue;
        }
        if (arg == "--vm-self-aware") {
            result.options.vmSelfAware = true;
            continue;
        }
        if (arg == "--vm-drain") {
            result.options.vmDrain = true;
            continue;
        }
        if (arg == "--vm-summary") {
            result.options.vmJsonSummary = true;
            continue;
        }
        if (arg == "--vm-limit") {
            const char* value = requireValue();
            if (!parseInteger(value, 1, result.options.vmLimit)) {
                return fail(std::move(result), "--vm-limit requires a positive integer");
            }
            continue;
        }
        if (arg == "--vm-budget") {
            const char* value = requireValue();
            if (!parseInteger(value, 1, result.options.vmBudget)) {
                return fail(std::move(result), "--vm-budget requires a positive integer");
            }
            continue;
        }
        if (arg == "--module") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--module requires a module stem, such as end_m01aa");
            }
            result.options.module = value;
            continue;
        }
        if (arg == "--install") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--install requires the KotOR install directory");
            }
            result.options.install = value;
            continue;
        }
        if (arg == "--screenshot") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--screenshot requires a path");
            }
            result.options.screenshot = value;
            continue;
        }
        if (arg == "--save") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--save requires a path");
            }
            result.options.savePath = value;
            continue;
        }
        if (arg == "--original-save-load") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--original-save-load requires a save folder");
            }
            result.options.originalSaveLoad = value;
            continue;
        }
        if (arg == "--original-save-roundtrip") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--original-save-roundtrip requires a save folder");
            }
            result.options.originalSaveRoundtrip = value;
            continue;
        }
        if (arg == "--profile-info") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--profile-info requires a game directory");
            }
            result.options.profileInfo = value;
            continue;
        }
        if (arg == "--original-save-create") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--original-save-create requires a folder");
            }
            result.options.originalSaveCreate = value;
            continue;
        }
        if (arg == "--original-save-out") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--original-save-out requires a folder");
            }
            result.options.originalSaveOut = value;
            continue;
        }
        if (arg.starts_with("--backend=")) {
            const std::string_view value = arg.substr(std::string_view("--backend=").size());
            if (value == "auto") {
                result.options.backend = BackendKind::Auto;
            } else if (value == "vulkan") {
                result.options.backend = BackendKind::Vulkan;
            } else if (value == "gl" || value == "opengl") {
                result.options.backend = BackendKind::OpenGL;
            } else if (value == "null") {
                result.options.backend = BackendKind::Null;
            } else {
                return fail(std::move(result), "Unknown backend: " + std::string(value));
            }
            continue;
        }
        if (arg == "--frames") {
            const char* value = requireValue();
            if (!parseInteger(value, 1, result.options.frames)) {
                return fail(std::move(result), "--frames requires a positive integer");
            }
            continue;
        }
        if (arg == "--seconds") {
            const char* value = requireValue();
            if (!parseDouble(value, 0.01, result.options.seconds)) {
                return fail(std::move(result), "--seconds requires a positive number");
            }
            continue;
        }
        if (arg == "--width") {
            const char* value = requireValue();
            if (!parseInteger(value, 64, result.options.width)) {
                return fail(std::move(result), "--width requires an integer >= 64");
            }
            continue;
        }
        if (arg == "--height") {
            const char* value = requireValue();
            if (!parseInteger(value, 64, result.options.height)) {
                return fail(std::move(result), "--height requires an integer >= 64");
            }
            continue;
        }
        if (arg == "--title") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--title requires a non-empty string");
            }
            result.options.title = value;
            continue;
        }

        return fail(std::move(result), "Unknown argument: " + std::string(arg));
    }

    return result;
}

void printUsage()
{
    std::cout
        << "kotor_engine - KotOR Remaster engine scaffold (Lane 08)\n"
        << "\n"
        << "Usage: kotor_engine [options]\n"
        << "\n"
        << "Backend selection:\n"
        << "  --vulkan              Use the Vulkan backend (primary)\n"
        << "  --gl, --opengl        Use the OpenGL 4.5 core backend (fallback)\n"
        << "  --null                Headless logical loop, no GPU work\n"
        << "  --backend=<name>      auto|vulkan|gl|null (default: auto)\n"
        << "  --no-fallback         Do not fall back to OpenGL when Vulkan fails\n"
        << "\n"
        << "Window / loop:\n"
        << "  --width <px>          Window width (default 1280)\n"
        << "  --height <px>         Window height (default 720)\n"
        << "  --hidden              Create the window hidden\n"
        << "  --headless            Force the SDL dummy video driver\n"
        << "  --frames <n>          Exit after n frames\n"
        << "  --seconds <s>         Exit after s seconds\n"
        << "  --title <text>        Window title\n"
        << "  --validate            Enable Vulkan validation layers if present\n"
        << "  --vsync / --no-vsync  Swap interval preference\n"
        << "\n"
        << "Diagnostics:\n"
        << "  --self-test           Run the ImGui headless self-test and exit\n"
        << "  --version             Print version and exit\n"
        << "  --help, -h            Print this help and exit\n"
        << "\n"
        << "KotOR module (C++ data layer):\n"
        << "  --install <dir>       KotOR install (default: KOTOR_INSTALL or the Steam path)\n"
        << "  --module <stem>       Load one module, e.g. end_m01aa or tar_m02af\n"
        << "  --scene-stats         Parse the module, print counts, then exit\n"
        << "  --content-stats       Print one JSON content line, then exit\n"
        << "  --walk-test           Run the headless player/collision smoke test\n"
        << "  --door-test           Load a linked door transition and report the target module\n"
        << "  --dialogue-test       Load a real DLG, start it and follow one branch\n"
        << "  --combat-test         Resolve a real hostile creature to death\n"
        << "  --playthrough         Run the deterministic 30-minute scripted gate\n"
        << "  --landmark-test       Check GIT landmarks/headings against the verified oracle\n"
        << "  --scene-assert        Assert the end_m01aa loader counts and exit\n"
        << "  --script-test         Run the shipped script hooks and assert their effects\n"
        << "  --rules-test          Assert the 2DA-driven character and d20 rules\n"
        << "  --systems-test        Assert party, inventory, journal and save/load\n"
        << "  --merchant-test       Assert store stock, markup and buy/sell\n"
        << "  --encounter-test      Assert encounter volumes and deterministic spawns\n"
        << "  --encounter-reset-test  Assert reset/respawn timer behavior\n"
        << "  --party-test          Assert companion sheets and 80% XP sharing\n"
        << "  --ui-test             Assert the systems UI panel state\n"
        << "  --loot-test           Assert container loot transfer\n"
        << "  --combat-force-test   Assert feats, Force powers, saves and alignment\n"
        << "  --animation-test      Assert MDL clips, skinning and playback\n"
        << "  --audio-test          Assert WAV/MP3 decode, buses and area audio\n"
        << "  --media-test          Assert Bink containers and LIP viseme timelines\n"
        << "  --movie-play <resref> Play a Bink movie (native pixel decode)\n"
        << "  --movie-time <sec>    Seek the movie before presenting\n"
        << "  --movie-shot <bmp>    Save the first presented movie frame\n"
        << "  --facial-demo <0-15>  Drive the lip-sync rig with a demo viseme\n"
        << "  --language <id|name>  Select the TLK language (default: auto)\n"
        << "  --gui-test            Assert all shipped GUI screens and control trees\n"
        << "  --minigame-test       Assert pazaak/swoop/turret state machines\n"
        << "  --localization-test   Assert TLK language and localized paths\n"
        << "  --vfs-test            Assert five-pass VFS and 2DA merges\n"
        << "  --k2-test             Assert KotOR 2 profile, geometry and 001EBO\n"
        << "  --faction-test        Assert repute.2da, actions and FAC round-trip\n"
        << "  --save-matrix-test    Load/round-trip every retail save fixture\n"
        << "  --save-fixtures <dir> Directory of SAVEGAME.sav fixture slots\n"
        << "  --dialogue-corpus-test Assert every shipped DLG parses and CamVidEffect\n"
        << "  --differential-test   Save-matrix + dialogue corpus + playthrough gate\n"
        << "  --retail-differential-test  Playthrough + engine semantic trace\n"
        << "  --module-trace        Trace the already-loaded module without playthrough\n"
        << "  --engine-trace <path> Engine trace JSONL for the retail differential\n"
        << "  --save-fixture <dir> Load this save for the engine trace\n"
        << "  --module-playthrough-test  All-module conversation matrix only\n"
        << "  --bug-parity-test     Classic bug-parity vs fixed-mode assertions\n"
        << "  --chargen             Start in interactive character generation\n"
        << "  --chargen-test        Assert the headless character-generation gate\n"
        << "  --third-person        Render the player avatar with an orbit camera\n"
        << "  --no-movies           Suppress module/movie overlays for inspection\n"
        << "  --camera-noclip       Disable third-person camera collision (debug)\n"
        << "  --player-appearance <row>  appearance.2da row for the player (default 92)\n"
        << "  --player-name <text>  Player display name (default Player)\n"
        << "  --minigame-script <ncs> PlayPazaak script bridge assertions\n"
        << "  --gui-screen <resref> Render one .gui screen in the debug UI\n"
        << "  --gui-action <tag>    Activate one control before rendering\n"
        << "  --audio-script <ncs>  Sound/music/ambient script bridge assertions\n"
        << "  --animation-script <ncs>  PlayAnimation script bridge assertions\n"
        << "  --combat-force-script <ncs>  Script bridge assertions for the combat-force gate\n"
        << "  --delayed-test <ncs>  Assert DelayCommand/AssignCommand closure execution\n"
        << "  --panel <name>        Open character|inventory|party|journal|merchant at startup\n"
        << "  --add-party <tag>     Add a scene creature to the party at startup\n"
        << "  --demo-dialogue       Position at Trask and open the real dialogue UI\n"
        << "  --demo-combat         Position at a hostile creature and start combat\n"
        << "  --screenshot <path>   Write a BMP after the requested frames\n"
        << "  --save <path>         Save slot for the player state (F5 writes, --load reads)\n"
        << "  --load                Load the save slot at startup\n"
        << "  --original-save-load <folder>       Load an original SAVEGAME.sav folder\n"
        << "  --original-save-roundtrip <folder>  Load, write and reload an original slot\n"
        << "  --profile-info <dir>                Detect KotOR 1 / KotOR 2 and exit\n"
        << "  --original-save-create <folder>     Write a fresh original slot from --module\n"
        << "  --original-save-out <folder>        Output folder for the round-trip slot\n"
        << "\n"
        << "Script verification (no module):\n"
        << "  --vm-corpus <dir>     Execute every .ncs in a directory and report\n"
        << "  --vm-out <path>       Write per-file JSONL results\n"
        << "  --vm-functional       Use the stateful action runtime\n"
        << "  --vm-self-aware       Resolve OBJECT_SELF in functional corpus runs\n"
        << "  --vm-drain            Execute delayed/queued closures for coverage\n"
        << "  --vm-limit <n>        Stop after n files\n"
        << "  --vm-budget <n>       Instruction budget per script (default 200000)\n"
        << "\n"
        << "Examples:\n"
        << "  kotor_engine --vulkan --frames 120 --seconds 5\n"
        << "  kotor_engine --headless --frames 5\n"
        << "  kotor_engine --self-test\n";
}

const char* backendName(BackendKind kind)
{
    switch (kind) {
    case BackendKind::Auto:
        return "auto";
    case BackendKind::Vulkan:
        return "vulkan";
    case BackendKind::OpenGL:
        return "opengl";
    case BackendKind::Null:
        return "null";
    }
    return "unknown";
}

} // namespace ke
