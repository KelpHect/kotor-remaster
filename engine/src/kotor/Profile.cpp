#include "kotor/Profile.h"

#include "kotor/Formats.h"

namespace ke::kotor {

namespace {

constexpr const char* kKotor1Sha = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6";
constexpr const char* kKotor2Sha = "6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef";

bool isFile(const std::filesystem::path& path)
{
    return std::filesystem::is_regular_file(path);
}

} // namespace

const char* gameKindName(GameKind kind)
{
    switch (kind) {
    case GameKind::KotOR1: return "kotor1";
    case GameKind::KotOR2: return "kotor2";
    case GameKind::Unknown: return "unknown";
    }
    return "unknown";
}

GameProfile GameProfile::detect(const std::filesystem::path& gameDir)
{
    GameProfile profile;
    const std::filesystem::path kotor2 = gameDir / "swkotor2.exe";
    const std::filesystem::path kotor1 = gameDir / "swkotor.exe";
    if (isFile(kotor2)) {
        profile.kind = GameKind::KotOR2;
        profile.name = "Knights of the Old Republic II";
        profile.executable = "swkotor2.exe";
        profile.buildSha256 = sha256File(kotor2);
        profile.recognized = profile.buildSha256 == kKotor2Sha;
        return profile;
    }
    if (isFile(kotor1)) {
        profile.kind = GameKind::KotOR1;
        profile.name = "Knights of the Old Republic";
        profile.executable = "swkotor.exe";
        profile.buildSha256 = sha256File(kotor1);
        profile.recognized = profile.buildSha256 == kKotor1Sha;
        return profile;
    }
    profile.name = "not a KotOR install";
    return profile;
}

} // namespace ke::kotor
