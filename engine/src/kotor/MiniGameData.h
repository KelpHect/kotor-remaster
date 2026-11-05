// The ARE MiniGame struct and the LYT track and obstacle lists that place it.
// Field names and defaults follow retail's loader (006723d0 and the follower
// loaders 0066fff0, 006702f0). docs/recon/53_MINIGAMES.md is the contract.
#pragma once

#include "kotor/Formats.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ke::kotor {

// Slot order is retail's: the follower's script array at +0xa4 (0066c970).
enum MgScriptSlot : std::size_t {
    kMgOnCreate = 0,
    kMgOnHitBullet = 1,
    kMgOnHitFollower = 2,
    kMgOnAnimEvent = 3,
    kMgOnHeartbeat = 4,
    kMgOnDamage = 5,
    kMgOnDeath = 6,
    kMgOnFire = 7,
    kMgOnHitObstacle = 8,
    kMgOnTrackLoop = 9,
    kMgScriptSlots = 10,
};

using MgScripts = std::array<std::string, kMgScriptSlots>;

struct MgGunBankData {
    std::int32_t bankId = 0;
    std::string gunModel;
    std::string fireSound;
    std::int32_t damage = 0;
    float lifespan = 0.0f;
    std::string bulletModel;
    float rateOfFire = 0.0f;
    std::string collisionSound;
    float speed = 0.0f;
    std::int32_t targetType = 0;
    // Enemy banks only.
    float inaccuracy = 0.0f;
    float sensingRadius = 0.0f;
    float horizontalSpread = 0.0f;
    float verticalSpread = 0.0f;
};

struct MgModelRef {
    std::string model;
    bool rotating = true;
};

struct MgFollowerData {
    std::string track;
    std::int32_t numLoops = -10; // Retail's default when the field is absent (0066fff0).
    float sphereRadius = 0.0f;
    float invincePeriod = 0.0f;
    std::int32_t hitPoints = 100;
    std::int32_t maxHitPoints = 100;
    std::int32_t bumpDamage = 0;
    std::string deathSound;
    std::string engineSound;
    std::vector<MgModelRef> models;
    MgScripts scripts;
    std::vector<MgGunBankData> gunBanks;
};

struct MgPlayerData : MgFollowerData {
    std::string camera;
    bool cameraRotate = true;
    float minimumSpeed = -1.0f;
    float maximumSpeed = 100.0f;
    float accelSeconds = -1.0f;
    Vec3 startOffset;
    Vec3 tunnelInfinite;
    Vec3 tunnelPositive;
    Vec3 tunnelNegative;
    Vec3 targetOffset;
};

struct MgEnemyData : MgFollowerData {
    bool trigger = false;
};

struct MgObstacleData {
    std::string name;
    MgScripts scripts;
};

struct MiniGameData {
    std::int32_t type = 0; // 1 swoop, 2 turret.
    float nearClip = 0.1f;
    float farClip = 100.0f;
    float cameraViewAngle = 65.0f;
    std::int32_t bumpPlane = 0;
    bool useInertia = false;
    bool doBumping = false;
    std::int32_t dof = 0;
    float movementPerSec = 0.0f;
    float lateralAccel = 60.0f;
    std::string music;
    std::int32_t mouseAxisX = 0;
    std::int32_t mouseAxisY = 0;
    MgPlayerData player;
    std::vector<MgEnemyData> enemies;
    std::vector<MgObstacleData> obstacles;
};

struct MgLayoutEntry {
    std::string name; // Lower case.
    Vec3 origin;
};

struct MgLayout {
    std::vector<MgLayoutEntry> tracks;
    std::vector<MgLayoutEntry> obstacles;

    Vec3 trackOrigin(const std::string& name) const;
    Vec3 obstacleOrigin(const std::string& name) const;
};

// Empty when the ARE has no MiniGame struct.
std::optional<MiniGameData> parseMiniGame(const Gff& are);
// The trackcount and obstaclecount sections of a module layout.
MgLayout parseMiniGameLayout(const std::vector<std::uint8_t>& lyt);

} // namespace ke::kotor
