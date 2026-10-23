#include "app/Application.h"
#include "app/Args.h"
#include "core/Log.h"

#include "imgui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <iostream>

int main(int argc, char** argv)
{
    const ke::ParseResult parsed = ke::parseArguments(argc, argv);

    switch (parsed.action) {
    case ke::ParseAction::Help:
        ke::printUsage();
        return 0;
    case ke::ParseAction::Version:
        std::cout << "kotor_engine 0.1.0 (scaffold)\n"
                  << "SDL " << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION << "."
                  << SDL_MICRO_VERSION << "\n"
                  << "Dear ImGui " << IMGUI_VERSION << "\n";
        return 0;
    case ke::ParseAction::Error:
        ke::logError(parsed.error);
        ke::printUsage();
        return 2;
    case ke::ParseAction::Run:
        break;
    }

    return ke::runApplication(parsed.options);
}
