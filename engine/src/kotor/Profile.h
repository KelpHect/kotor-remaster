#pragma once

// Game-profile identity for the data layer. A profile names the shipped
// executable, the supported build hash and the capabilities the engine is
// allowed to claim for that build. KotOR 1 and KotOR 2 both load worlds; K2
// uses profile-gated geometry/resource layout differences. Original save
// round-tripping remains KotOR 1 only.

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

    bool supportsWorldLoad() const {
        return recognized && (kind == GameKind::KotOR1 || kind == GameKind::KotOR2);
    }
    bool supportsOriginalSaves() const { return kind == GameKind::KotOR1; }

    static GameProfile detect(const std::filesystem::path& gameDir);
};

const char* gameKindName(GameKind kind);

} // namespace ke::kotor
