#include "app/Args.h"

#include "kotor/Localization.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
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
    // A flag missing its value is always the last argument. Handlers get an empty string,
    // never a null, so atoi or a string assignment can't crash on it; the error follows below.
    std::string_view flagMissingValue;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto requireValue = [&]() -> const char* {
            if (i + 1 >= argc) {
                flagMissingValue = arg;
                return "";
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
        if (arg == "--no-activate") {
            result.options.noActivate = true;
            continue;
        }
        if (arg == "--fullscreen") {
            result.options.fullscreen = true;
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
        if (arg == "--max-fps") {
            result.options.maxFps = std::atoi(requireValue());
            continue;
        }
        if (arg == "--background-fps") {
            result.options.backgroundFps = std::atoi(requireValue());
            continue;
        }
        if (arg == "--start-minimized") {
            result.options.startMinimized = true;
            continue;
        }
        if (arg == "--soak") {
            result.options.soak = requireValue();
            continue;
        }
        if (arg == "--soak-modules") {
            result.options.soakModules = requireValue();
            continue;
        }
        if (arg == "--soak-out") {
            result.options.soakOut = requireValue();
            continue;
        }
        if (arg == "--soak-cycles") {
            result.options.soakCycles = std::atoi(requireValue());
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
        if (arg == "--world-sim-test") {
            result.options.worldSimTest = true;
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
        if (arg == "--chargen-portrait-seed") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--chargen-portrait-seed requires a number");
            }
            result.options.chargenPortraitSeed = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            continue;
        }
        if (arg == "--chargen-portrait-rows") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--chargen-portrait-rows requires six comma separated rows");
            }
            result.options.chargenPortraitRows.clear();
            std::istringstream rows{std::string(value)};
            for (std::string item; std::getline(rows, item, ',');) {
                result.options.chargenPortraitRows.push_back(static_cast<std::int32_t>(std::strtol(item.c_str(), nullptr, 10)));
            }
            if (result.options.chargenPortraitRows.size() != 6) {
                return fail(std::move(result), "--chargen-portrait-rows requires six comma separated rows");
            }
            continue;
        }
        if (arg == "--chargen-gui") {
            result.options.chargenGui = true;
            continue;
        }
        if (arg == "--front-end-test") {
            result.options.frontEndTest = true;
            result.options.guiScreen = "mainmenu";
            continue;
        }
        if (arg == "--front-end-scenario") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--front-end-scenario requires a name");
            }
            result.options.frontEndScenario = value;
            continue;
        }
        if (arg == "--front-end-out") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--front-end-out requires a directory");
            }
            result.options.frontEndOut = value;
            continue;
        }
        if (arg == "--chargen-gui-test") {
            result.options.chargenGuiTest = true;
            continue;
        }
        if (arg == "--chargen-preview-test") {
            result.options.chargenPreviewTest = true;
            result.options.chargenGui = true;
            continue;
        }
        if (arg == "--new-game-flow-test") {
            result.options.newGameFlowTest = true;
            result.options.guiScreen = "mainmenu";
            continue;
        }
        if (arg == "--chargen-preview-out") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--chargen-preview-out requires a directory");
            }
            result.options.chargenPreviewOut = value;
            continue;
        }
        if (arg == "--chargen-test") {
            result.options.chargenTest = true;
            continue;
        }
        if (arg == "--campaign-test") {
            result.options.campaignTest = true;
            continue;
        }
        if (arg == "--avatar-test") {
            result.options.avatarTest = true;
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
        if (arg == "--startup-movies") {
            result.options.startupMovies = true;
            continue;
        }
        if (arg == "--play-dialogue") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--play-dialogue requires a DLG resref");
            }
            result.options.playDialogue = value;
            continue;
        }
        if (arg == "--no-auto-advance") {
            result.options.noAutoAdvance = true;
            continue;
        }
        if (arg == "--no-audio") {
            result.options.noAudio = true;
            continue;
        }
        if (arg == "--audio-capture") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--audio-capture requires a file prefix");
            }
            result.options.audioCapture = value;
            continue;
        }
        if (arg == "--fixed-dt") {
            const char* value = requireValue();
            if (!parseDouble(value, 0.0001, result.options.fixedDt)) {
                return fail(std::move(result), "--fixed-dt requires seconds per frame");
            }
            continue;
        }
        if (arg == "--camera-noclip") {
            result.options.cameraNoClip = true;
            continue;
        }
        if (arg == "--start-minigame") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--start-minigame requires pazaak, swoop or turret");
            }
            result.options.startMinigame = value;
            continue;
        }
        if (arg == "--family-test") {
            result.options.familyTest = requireValue();
            continue;
        }
        if (arg == "--action-coverage") {
            result.options.actionCoverage = requireValue();
            continue;
        }
        if (arg == "--quest-trace") {
            result.options.questTrace = requireValue();
            continue;
        }
        if (arg == "--conversation-flow") {
            result.options.conversationFlow = requireValue();
            continue;
        }
        if (arg == "--plot-replay") {
            result.options.plotReplay = requireValue();
            continue;
        }
        if (arg == "--quest-graph") {
            result.options.questGraph = requireValue();
            continue;
        }
        if (arg == "--quest-trace-baseline") {
            result.options.questBaseline = requireValue();
            continue;
        }
        if (arg == "--quest-trace-modules") {
            result.options.questModules = requireValue();
            continue;
        }
        if (arg == "--quest-trace-break") {
            result.options.questBreak = requireValue();
            continue;
        }
        if (arg == "--action-corpus") {
            result.options.actionCorpus = requireValue();
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
        if (arg == "--debug-ui") {
            result.options.debugUi = true;
            continue;
        }
        if (arg == "--saves-dir" || arg == "--settings-file") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), std::string(arg) + " requires a path");
            }
            (arg == "--saves-dir" ? result.options.savesDir : result.options.settingsFile) = value;
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
        if (arg == "--video-effect-fixtures") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--video-effect-fixtures requires a directory");
            }
            result.options.videoEffectFixtures = value;
            continue;
        }
        if (arg == "--video-effect-sequence") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--video-effect-sequence requires a comma list");
            }
            result.options.videoEffectSequence = value;
            continue;
        }
        if (arg == "--video-effect-warmup") {
            const char* value = requireValue();
            if (value == nullptr) {
                return fail(std::move(result), "--video-effect-warmup requires a frame count");
            }
            result.options.videoEffectWarmup = std::max(0, std::atoi(value));
            continue;
        }
        if (arg == "--video-effect-shots") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--video-effect-shots requires a path prefix");
            }
            result.options.videoEffectShots = value;
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
        if (arg == "--minigame-render-check") {
            result.options.minigameRenderCheck = true;
            continue;
        }
        if (arg == "--texture-wrap-check") {
            result.options.textureWrapCheck = true;
            continue;
        }
        if (arg == "--capture-camera") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--capture-camera requires a view");
            }
            result.options.captureCamera = value;
            continue;
        }
        if (arg == "--input-script" || arg == "--input-script-out") {
            const bool script = arg == "--input-script";
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), script ? "--input-script requires a file" : "--input-script-out requires a folder");
            }
            (script ? result.options.inputScript : result.options.inputScriptOut) = value;
            continue;
        }
        if (arg == "--new-game") {
            result.options.newGame = true;
            continue;
        }
        if (arg == "--new-game-slot") {
            const char* value = requireValue();
            int slot = 0;
            if (value == nullptr || !parseInteger(value, 0, slot) || slot > 5) {
                return fail(std::move(result), "--new-game-slot requires 0..5");
            }
            result.options.newGameSlot = slot;
            continue;
        }
        if (arg == "--walk-godmode") {
            result.options.walkGodmode = true;
            continue;
        }
        if (arg == "--walk-continue") {
            result.options.walkContinue = true;
            continue;
        }
        if (arg == "--start-wp" || arg == "--seed-file") {
            const bool waypoint = arg == "--start-wp";
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), waypoint ? "--start-wp requires a waypoint tag" : "--seed-file requires a file");
            }
            (waypoint ? result.options.startWp : result.options.seedFile) = value;
            continue;
        }
        if (arg == "--state-trace" || arg == "--state-trace-samp") {
            const bool file = arg == "--state-trace";
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), file ? "--state-trace requires a file" : "--state-trace-samp requires a count");
            }
            if (file) {
                result.options.stateTrace = value;
                // The retail sound system re-tries a waiting looped sample on every rendered frame and the trace
                // records each try, so a trace run needs a retail-like frame rate even without input focus.
                if (result.options.backgroundFps == 15) {
                    result.options.backgroundFps = 60;
                }
            } else {
                const int samp = std::atoi(value);
                result.options.stateTraceSamp = samp > 1 ? samp : 1;
            }
            continue;
        }
        if (arg == "--state-trace-load") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--state-trace-load requires a save folder");
            }
            result.options.stateTraceLoad = value;
            continue;
        }
        if (arg == "--capture-action") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--capture-action requires door:n@frame or equip:n:model@frame");
            }
            result.options.captureAction = value;
            continue;
        }
        if (arg == "--spawn-vfx") {
            const char* value = requireValue();
            if (value == nullptr || *value == '\0') {
                return fail(std::move(result), "--spawn-vfx requires id[:creature]");
            }
            result.options.spawnVfx = value;
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
        if (arg == "--shadow-probe") {
            const char* value = requireValue();
            if (!parseInteger(value, 1, result.options.shadowProbeFrame)) {
                return fail(std::move(result), "--shadow-probe requires a positive frame number");
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
            result.options.sizeGiven = true;
            continue;
        }
        if (arg == "--height") {
            const char* value = requireValue();
            if (!parseInteger(value, 64, result.options.height)) {
                return fail(std::move(result), "--height requires an integer >= 64");
            }
            result.options.sizeGiven = true;
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

    if (!flagMissingValue.empty()) {
        return fail(std::move(result), std::string(flagMissingValue) + " requires a value");
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
        << "  --fullscreen          Start borderless at the desktop size (F11 toggles)\n"
        << "  --hidden              Create the window hidden\n"
        << "  --no-activate         Show the window without ever giving it focus\n"
        << "  --input-script <f>    Play a timed walk of mouse, key and shot steps (app/InputScript.h)\n"
        << "  --input-script-out <d>  Folder for the walk's frames\n"
        << "  --new-game            Start a new game (default character, end_m01aa) instead of a bare module\n"
        << "  --new-game-slot <n>   With --new-game: the class screen's button 0..5 (default 2, male Soldier; 0 male Scoundrel)\n"
        << "  --walk-godmode        Walk steps only: keep the party from dying (route discovery)\n"
        << "  --walk-continue       Walk steps only: keep going after a failed step\n"
        << "  --start-wp <tag>      Per-module probing: stand the leader on this waypoint after the load\n"
        << "  --seed-file <f>       Per-module probing: globals, journal, party, gold, xp from a file\n"
        << "  --state-trace-load <d> Load this save folder at start, as Load Game does (with --state-trace)\n"
        << "  --state-trace <f>     Write the per-frame state trace (game/StateTrace.h); --state-trace-samp <n> sets the cadence\n"
        << "  --headless            Force the SDL dummy video driver\n"
        << "  --frames <n>          Exit after n frames\n"
        << "  --seconds <s>         Exit after s seconds\n"
        << "  --shadow-probe <n>    Log the pixels frame n's shadow volumes cover\n"
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
        << "  --world-sim-test      Assert action queues, events, dialogue rules and module state\n"
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
        << "  --minigame-test       Assert the pazaak state machine\n"
        << "  --minigame-render-check  Compare a minigame frame with and without its meshes\n"
        << "  --texture-wrap-check  Render a tiled room triangle under each wrap mode and measure it\n"
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
        << "  --chargen-gui         Use the shipped classsel/abilities/skills/name screens\n"
        << "  --chargen-gui-test    Assert the shipped chargen screen flow headlessly\n"
        << "  --chargen-preview-test  Pixel-assert the 3D chargen model controls (GL/Vulkan)\n"
        << "  --chargen-preview-out <dir>  Save the preview test's captured frames as BMP\n"
        << "  --chargen-test        Assert the headless character-generation gate\n"
        << "  --campaign-test       Run the multi-act campaign continuity gate\n"
        << "  --avatar-test         Assert player/companion meshes and locomotion\n"
        << "  --third-person        Render the player avatar with an orbit camera\n"
        << "  --no-movies           Suppress module/movie overlays for inspection\n"
        << "  --play-dialogue <dlg> Start this conversation with the controlled creature as owner\n"
        << "  --no-auto-advance     Dialogue entries wait for a click instead of following their voice-over\n"
        << "  --startup-movies      Play the retail intro logos (leclogo, biologo, legal); Esc skips each\n"
        << "  --no-audio            Open no audio device (the mixer still runs on the frame clock)\n"
        << "  --audio-capture <p>   Write each bus and the final mix to <p>_music|ambient|effects|voice|movie|reverb|final.wav\n"
        << "  --fixed-dt <s>       Advance the session by s seconds every frame (reproducible captures)\n"
        << "  --camera-noclip       Disable third-person camera collision (debug)\n"
        << "  --start-minigame <pazaak|swoop|turret>  Open pazaak, or load a shipped race or turret module\n"
        << "  --family-test <name>  Run one Wave 2 family gate (e.g. worldstate)\n"
        << "  --action-coverage <out.tsv>  Dump each action's handler status\n"
        << "  --action-corpus <dir>  NCS scripts; fail when one uses an unhandled action\n"
        << "  --quest-graph <dir>   Write the static quest graph (no simulation)\n"
        << "  --quest-trace <dir>   Quest tracer: graph, per-edge execution, navigation, gate\n"
        << "  --quest-trace-baseline <file>  Allowed exceptions and the pass floor\n"
        << "  --quest-trace-modules <a,b>    Trace only these modules\n"
        << "  --quest-trace-break <name>     Negative check: inject a test-only fault\n"
        << "  --player-appearance <row>  appearance.2da row for the player (default 92)\n"
        << "  --player-name <text>  Player display name (default Player)\n"
        << "  --minigame-script <ncs> PlayPazaak script bridge assertions\n"
        << "  --gui-screen <resref> Render one .gui screen in the debug UI\n"
        << "  --gui-action <tag>    Activate one control before rendering\n"
        << "  --front-end-test      Drive the shipped menu, options, load and movies screens by real events\n"
        << "  --front-end-scenario <n> Run another front-end gate sequence (store)\n"
        << "  --front-end-out <d>   Folder for the front-end gate frames\n"
        << "  --debug-ui            Also draw the developer panels (scaffold, playable slice) over the HUD\n"
        << "  --saves-dir <dir>     Folder listed by the load/save screens\n"
        << "  --settings-file <f>   Options file written by the options screens\n"
        << "  --video-effect-fixtures <dir>  Folder of video_effect_*.ncs fixtures\n"
        << "  --video-effect-sequence <a,b,..>  Run one fixture (row0,row1,row2,off) per frame\n"
        << "  --video-effect-warmup <n>  Frames to render before the sequence starts\n"
        << "  --video-effect-shots <prefix>  Write <prefix>-<i>.bmp after every sequence frame\n"
        << "  --audio-script <ncs>  Sound/music/ambient script bridge assertions\n"
        << "  --animation-script <ncs>  PlayAnimation script bridge assertions\n"
        << "  --combat-force-script <ncs>  Script bridge assertions for the combat-force gate\n"
        << "  --delayed-test <ncs>  Assert DelayCommand/AssignCommand closure execution\n"
        << "  --panel <name>        Open character|inventory|party|journal|merchant at startup\n"
        << "  --add-party <tag>     Add a scene creature to the party at startup\n"
        << "  --demo-dialogue       Position at Trask and open the real dialogue UI\n"
        << "  --demo-combat         Position at a hostile creature and start combat\n"
        << "  --screenshot <path>   Write a BMP after the requested frames\n"
        << "  --capture-camera <v>  entry, creature[:n], placeable[:n], door[:n] or x,y,z,lx,ly,lz[,fov]; hides the UI\n"
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
