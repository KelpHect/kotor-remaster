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
    int width = 1280;
    int height = 720;
    int frames = 0;          // 0 = unlimited
    double seconds = 0.0;    // 0 = unlimited
    std::string title = "KotOR Remaster - Engine Scaffold";
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
