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
    bool allowFallback = true;
    bool selfTest = false;
    bool sceneStats = false;
    bool contentStats = false;
    bool walkTest = false;
    bool doorTest = false;
    bool dialogueTest = false;
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
    std::string vfsFixtures;
    std::string factionScript;
    std::string guiScreen;
    std::string guiAction;
    std::string minigameScript;
    std::string moviePlay;
    std::string movieShot;
    float movieTime = -1.0f;
    int language = -1;
    int facialDemo = -1;
    std::string audioScript;
    std::string animationScript;
    std::string combatForceScript;
    std::string delayedTest;
    std::string panel;
    std::string addParty;
    bool demoDialogue = false;
    bool demoCombat = false;
    int width = 1280;
    int height = 720;
    int frames = 0;          // 0 = unlimited
    double seconds = 0.0;    // 0 = unlimited
    std::string title = "KotOR Remaster - Engine Scaffold";
    std::string install;
    std::string module;
    std::string screenshot;
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
    bool vmJsonSummary = false;
    int vmLimit = 0;
    int vmBudget = 200000;
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
