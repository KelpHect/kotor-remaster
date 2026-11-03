#pragma once

// Game-profile identity for the data layer. A profile names the shipped
// executable, the supported build hash and the capabilities the engine is
// allowed to claim for that build. KotOR 1 is the supported world target for
// Sprint 9; KotOR 2 is recognized so its data can be identified and gated
// explicitly instead of being mistaken for a corrupted KotOR 1 install.

#include <filesystem>
#include <string>

namespace ke::kotor {

enum class GameKind { KotOR1, KotOR2, Unknown };

struct GameProfile {
    GameKind kind = GameKind::Unknown;
    std::string name = "unknown";
    std::string executable;
    std::string buildSha256;
    bool recognized = false;

    bool supportsWorldLoad() const { return kind == GameKind::KotOR1; }
    bool supportsOriginalSaves() const { return kind == GameKind::KotOR1; }

    static GameProfile detect(const std::filesystem::path& gameDir);
};

const char* gameKindName(GameKind kind);

} // namespace ke::kotor
