#pragma once

#include <string>

namespace ke {

enum class BackendKind { Auto, Vulkan, OpenGL, Null };

struct Options {
    BackendKind backend = BackendKind::Auto;
    bool headless = false;
    bool hidden = false;
    bool validate = false;
    bool vsync = true;
    int maxFps = -1;          // --max-fps N: -1 takes the engine's default, 0 is no cap
    int backgroundFps = 15;   // --background-fps N: minimized or unfocused; 0 keeps the foreground rate
    bool startMinimized = false; // --start-minimized
    std::string soak;         // --soak idle|modules|ui: leak gate inside the main loop
    std::string soakModules;  // --soak-modules a,b,c: the varied set of the modules scenario
    std::string soakOut = "."; // --soak-out dir: samples and a scratch save
    int soakCycles = 0;       // --soak-cycles N: 0 takes the scenario's default
    bool allowFallback = true;
    bool selfTest = false;
    bool sceneStats = false;
    bool contentStats = false;
    bool walkTest = false;
    bool doorTest = false;
    bool dialogueTest = false;
    bool worldSimTest = false;
    bool combatTest = false;
    bool playthrough = false;
    bool landmarkTest = false;
    bool sceneAssert = false;
    bool scriptTest = false;
    bool rulesTest = false;
    bool systemsTest = false;
    bool merchantTest = false;
    bool encounterTest = false;
    bool encounterResetTest = false;
    bool partyTest = false;
    bool uiTest = false;
    bool lootTest = false;
    bool combatForceTest = false;
    bool animationTest = false;
    bool audioTest = false;
    bool mediaTest = false;
    bool guiTest = false;
    bool minigameTest = false;
    bool localizationTest = false;
    bool vfsTest = false;
    bool k2Test = false;
    bool factionTest = false;
    bool saveMatrixTest = false;
    bool dialogueCorpusTest = false;
    bool differentialTest = false;
    bool retailDifferentialTest = false;
    bool moduleTrace = false;
    bool modulePlaythroughTest = false;
    bool bugParityTest = false;
    bool chargen = false;
    bool chargenGui = false;
    bool debugUi = false;        // developer panels over the shipped HUD
    bool frontEndTest = false;   // --front-end-test: menu/options/load/movies by real events
    std::string frontEndOut;     // directory for the gate's frames
    std::string frontEndScenario; // --front-end-scenario: another gate sequence ("store": shipped store + loading screen)
    bool chargenGuiTest = false;
    bool chargenPreviewTest = false; // pixel gate for the 3D chargen controls (needs a GPU backend)
    std::string chargenPreviewOut;   // optional directory for the captured frames (BMP)
    bool newGameFlowTest = false;    // main menu, New Game, chargen, first module (pointer events)
    bool chargenTest = false;
    bool campaignTest = false;
    bool avatarTest = false;
    bool thirdPerson = false;
    bool noMovies = false;
    bool startupMovies = false; // retail BeginIntro: leclogo, biologo, legal before the first screen
    std::string playDialogue;   // start this DLG with the controlled creature as owner (captures)
    bool noAutoAdvance = false; // dialogue entries wait for a click instead of following their voice
    bool noAudio = false;     // no device: the mixer is pumped by the frame clock instead
    std::string audioCapture; // write every bus and the final mix to <prefix>_<bus>.wav (diagnostic)
    double fixedDt = 0.0;     // > 0: every frame advances the session by this many seconds
    bool cameraNoClip = false;
    std::string startMinigame;
    int playerAppearance = 92;
    std::string familyTest; // --family-test <name>: a Wave 2 family gate
    std::string actionCoverage; // --action-coverage <out.tsv>: handler status per action
    std::string actionCorpus;   // --action-corpus <dir>: scripts whose use is checked
    std::string questTrace;     // --quest-trace <outdir>: the quest tracer and its gate
    std::string questGraph;     // --quest-graph <outdir>: stage 1 only
    std::string questBaseline;  // --quest-trace-baseline <file>
    std::string questModules;   // --quest-trace-modules a,b
    std::string questBreak;     // --quest-trace-break <name>: the negative check
    std::string conversationFlow; // --conversation-flow <outdir>: the conversation walk and its gate
    std::string plotReplay;       // --plot-replay <outdir>: the main plot replay and its gate

    std::string playerName = "Player";
    std::string engineTrace;
    std::string saveFixture;
    std::string vfsFixtures;
    std::string factionScript;
    std::string saveFixtures;
    std::string guiScreen;
    std::string guiAction;
    std::string savesDir;     // folder the load/save screens list (default: user data)
    std::string settingsFile; // options file (default: user data)
    std::string minigameScript;
    std::string moviePlay;
    std::string movieShot;
    float movieTime = -1.0f;
    int language = -1;
    int facialDemo = -1;
    std::string audioScript;
    std::string videoEffectFixtures; // directory with video_effect_*.ncs
    std::string videoEffectSequence; // comma list of fixture stems, one per frame ("keep" = none)
    int videoEffectWarmup = 0;       // frames to render before the sequence starts
    std::string videoEffectShots;    // path prefix: BMP after every sequence frame
    std::string animationScript;
    std::string combatForceScript;
    std::string delayedTest;
    std::string panel;
    std::string addParty;
    bool demoDialogue = false;
    bool demoCombat = false;
    int width = 1280;
    int height = 720;
    bool sizeGiven = false;   // --width or --height was passed: a saved size does not override it
    bool fullscreen = false;  // --fullscreen: borderless at the desktop size
    int frames = 0;          // 0 = unlimited
    double seconds = 0.0;    // 0 = unlimited
    std::string title = "KotOR Remaster - Engine Scaffold";
    std::string install;
    std::string module;
    std::string screenshot;
    // Gallery shots: entry | creature[:n] | placeable[:n] | door[:n] | x,y,z,lx,ly,lz[,fov].
    // Overrides the game camera and hides the UI and the module fade.
    std::string captureCamera;
    // Gallery shots: comma-separated door:n@frame (opens the nth door from the entry),
    // equip:n:model@frame (puts a weapon model in the nth creature's hand),
    // fullupload@frame (times a whole scene upload, for comparison with the sync).
    std::string captureAction;
    // --input-script: a timed walk (app/InputScript.h) and the folder its frames go to.
    std::string inputScript;
    std::string inputScriptOut;
    // Gallery shots: "id[:n]" holds visualeffects.2da row id on the nth creature (default 0).
    std::string spawnVfx;
    bool minigameRenderCheck = false;
    bool textureWrapCheck = false;
    std::string savePath;
    bool loadSave = false;
    std::string originalSaveLoad;
    std::string originalSaveRoundtrip;
    std::string originalSaveCreate;
    std::string originalSaveOut;
    std::string profileInfo;
    // Script runtime verification (no game module needed).
    std::string vmCorpus;
    std::string vmOut;
    bool vmFunctional = false;
    bool vmSelfAware = false;
    bool vmDrain = false;
    bool vmJsonSummary = false;
    int vmLimit = 0;
    int vmBudget = 0x1FFFF; // Retail's instruction limit (005d2bd0).
};

enum class ParseAction { Run, Help, Version, Error };

struct ParseResult {
    ParseAction action = ParseAction::Run;
    Options options;
    std::string error;
};

ParseResult parseArguments(int argc, char** argv);
void printUsage();

const char* backendName(BackendKind kind);

} // namespace ke
