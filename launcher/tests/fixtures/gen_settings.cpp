// Writes reference settings files with the engine's own ke::UserSettings, for the Rust launcher tests.
#include "game/UserSettings.h"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: gen <defaults.ini> <custom.ini>\n";
        return 2;
    }
    ke::UserSettings defaults;
    if (!defaults.save(argv[1])) {
        return 1;
    }
    ke::UserSettings custom;
    custom.set("sound.music", 35.0f);
    custom.set("graphics.width", 1920.0f);
    custom.set("graphics.height", 1080.0f);
    custom.set("graphics.fullscreen", 1.0f);
    custom.set("autopause.endround", 1.0f);
    custom.set("graphics.gamma", 33.3333f);
    custom.set("mouse.sensitivity", 0.1f);
    custom.set("test.tiny", 0.00001234f);
    custom.set("test.big", 1234567.0f);
    custom.set("test.negative", -2.5f);
    custom.set("test.round", 999999.5f);
    custom.set("test.third", 1.0f / 3.0f);
    custom.set("Test.Upper", 7.0f);
    custom.seenMovies.insert("leclogo");
    custom.seenMovies.insert("biologo");
    custom.seenMovies.insert("01a");
    if (!custom.save(argv[2])) {
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
