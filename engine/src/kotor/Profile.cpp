#include "kotor/Profile.h"

#include "kotor/Formats.h"

namespace ke::kotor {

namespace {

constexpr const char* kKotor1Sha = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6";
constexpr const char* kKotor2Sha = "6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef";
// Data-set identity for installs without an executable (ADR 0003: the executable is never required):
// the SHA-256 of chitin.key, as the install validator tool and the launcher catalog use it.
constexpr const char* kKotor1KeySha = "2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98";
constexpr const char* kKotor2KeySha = "47bea08efb5c582918fe2ff32fec845ce1cf604ce8bdd62f5b437004fb257412";

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
    // No executable: identify the data set by its key file. buildSha256 then holds that hash.
    const std::filesystem::path key = gameDir / "chitin.key";
    if (isFile(key)) {
        const std::string keySha = sha256File(key);
        if (keySha == kKotor1KeySha) {
            profile.kind = GameKind::KotOR1;
            profile.name = "Knights of the Old Republic";
        } else if (keySha == kKotor2KeySha) {
            profile.kind = GameKind::KotOR2;
            profile.name = "Knights of the Old Republic II";
        }
        if (profile.kind != GameKind::Unknown) {
            profile.buildSha256 = keySha;
            profile.recognized = true;
            return profile;
        }
    }
    profile.name = "not a KotOR install";
    return profile;
}

} // namespace ke::kotor
