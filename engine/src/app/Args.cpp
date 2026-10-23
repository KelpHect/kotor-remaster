#include "app/Args.h"

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
