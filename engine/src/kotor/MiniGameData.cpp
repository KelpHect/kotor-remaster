#include "kotor/MiniGameData.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ke::kotor {

namespace {

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string textOf(const Gff& gff, std::uint32_t index, const char* label)
{
    const GffField* field = gff.field(index, label);
    return field != nullptr ? gff.text(*field) : std::string{};
}

float numberOf(const Gff& gff, std::uint32_t index, const char* label, float fallback)
{
    const GffField* field = gff.field(index, label);
    return field != nullptr ? gff.number(*field) : fallback;
}

std::int32_t integerOf(const Gff& gff, std::uint32_t index, const char* label, std::int32_t fallback)
{
    const GffField* field = gff.field(index, label);
    return field != nullptr ? gff.integer(*field) : fallback;
}

std::optional<std::uint32_t> structOf(const Gff& gff, std::uint32_t index, const char* label)
{
    const GffField* field = gff.field(index, label);
    if (field == nullptr || field->type != GffType::Struct) {
        return std::nullopt;
    }
    return gff.structIndex(*field);
}

std::vector<std::uint32_t> listOf(const Gff& gff, std::uint32_t index, const char* label)
{
    const GffField* field = gff.field(index, label);
    if (field == nullptr || field->type != GffType::List) {
        return {};
    }
    return gff.list(*field);
}

MgScripts parseScripts(const Gff& gff, std::uint32_t parent)
{
    MgScripts scripts;
    const std::optional<std::uint32_t> index = structOf(gff, parent, "Scripts");
    if (!index.has_value()) {
        return scripts;
    }
    scripts[kMgOnCreate] = lowered(textOf(gff, *index, "OnCreate"));
    scripts[kMgOnHitBullet] = lowered(textOf(gff, *index, "OnHitBullet"));
    scripts[kMgOnHitFollower] = lowered(textOf(gff, *index, "OnHitFollower"));
    scripts[kMgOnAnimEvent] = lowered(textOf(gff, *index, "OnAnimEvent"));
    scripts[kMgOnHeartbeat] = lowered(textOf(gff, *index, "OnHeartbeat"));
    scripts[kMgOnDamage] = lowered(textOf(gff, *index, "OnDamage"));
    scripts[kMgOnDeath] = lowered(textOf(gff, *index, "OnDeath"));
    scripts[kMgOnFire] = lowered(textOf(gff, *index, "OnFire"));
    scripts[kMgOnHitObstacle] = lowered(textOf(gff, *index, "OnHitObstacle"));
    scripts[kMgOnTrackLoop] = lowered(textOf(gff, *index, "OnTrackLoop"));
    return scripts;
}

MgGunBankData parseGunBank(const Gff& gff, std::uint32_t index)
{
    MgGunBankData bank;
    bank.bankId = integerOf(gff, index, "BankID", 0);
    bank.gunModel = lowered(textOf(gff, index, "Gun_Model"));
    bank.fireSound = lowered(textOf(gff, index, "Fire_Sound"));
    bank.inaccuracy = numberOf(gff, index, "Inaccuracy", 0.0f);
    bank.sensingRadius = numberOf(gff, index, "Sensing_Radius", 0.0f);
    bank.horizontalSpread = numberOf(gff, index, "Horiz_Spread", 0.0f);
    bank.verticalSpread = numberOf(gff, index, "Vert_Spread", 0.0f);
    if (const std::optional<std::uint32_t> bullet = structOf(gff, index, "Bullet")) {
        bank.damage = integerOf(gff, *bullet, "Damage", 0);
        bank.lifespan = numberOf(gff, *bullet, "Lifespan", 0.0f);
        bank.bulletModel = lowered(textOf(gff, *bullet, "Bullet_Model"));
        bank.rateOfFire = numberOf(gff, *bullet, "Rate_Of_Fire", 0.0f);
        bank.collisionSound = lowered(textOf(gff, *bullet, "Collision_Sound"));
        bank.speed = numberOf(gff, *bullet, "Speed", 0.0f);
        bank.targetType = integerOf(gff, *bullet, "Target_Type", 0);
    }
    return bank;
}

void parseFollower(const Gff& gff, std::uint32_t index, MgFollowerData& follower)
{
    follower.track = lowered(textOf(gff, index, "Track"));
    follower.numLoops = integerOf(gff, index, "Num_Loops", follower.numLoops);
    follower.sphereRadius = numberOf(gff, index, "Sphere_Radius", follower.sphereRadius);
    follower.invincePeriod = numberOf(gff, index, "Invince_Period", follower.invincePeriod);
    follower.hitPoints = integerOf(gff, index, "Hit_Points", follower.hitPoints);
    follower.maxHitPoints = integerOf(gff, index, "Max_HPs", follower.maxHitPoints);
    follower.bumpDamage = integerOf(gff, index, "Bump_Damage", follower.bumpDamage);
    if (const std::optional<std::uint32_t> sounds = structOf(gff, index, "Sounds")) {
        follower.deathSound = lowered(textOf(gff, *sounds, "Death"));
        follower.engineSound = lowered(textOf(gff, *sounds, "Engine"));
    }
    for (const std::uint32_t row : listOf(gff, index, "Models")) {
        MgModelRef model;
        model.model = lowered(textOf(gff, row, "Model"));
        model.rotating = integerOf(gff, row, "RotatingModel", 1) != 0;
        follower.models.push_back(std::move(model));
    }
    follower.scripts = parseScripts(gff, index);
    for (const std::uint32_t row : listOf(gff, index, "Gun_Banks")) {
        follower.gunBanks.push_back(parseGunBank(gff, row));
    }
}

Vec3 parseVector(const Gff& gff, std::uint32_t index, const char* x, const char* y, const char* z)
{
    return {numberOf(gff, index, x, 0.0f), numberOf(gff, index, y, 0.0f), numberOf(gff, index, z, 0.0f)};
}

Vec3 originOf(const std::vector<MgLayoutEntry>& entries, const std::string& name)
{
    const std::string wanted = lowered(name);
    for (const MgLayoutEntry& entry : entries) {
        if (entry.name == wanted) {
            return entry.origin;
        }
    }
    return {};
}

} // namespace

Vec3 MgLayout::trackOrigin(const std::string& name) const
{
    return originOf(tracks, name);
}

Vec3 MgLayout::obstacleOrigin(const std::string& name) const
{
    return originOf(obstacles, name);
}

std::optional<MiniGameData> parseMiniGame(const Gff& are)
{
    const std::optional<std::uint32_t> root = structOf(are, are.rootIndex(), "MiniGame");
    if (!root.has_value()) {
        return std::nullopt;
    }
    MiniGameData game;
    game.type = integerOf(are, *root, "Type", 0);
    if (game.type != 1 && game.type != 2) {
        return std::nullopt; // retail 006723d0 refuses anything else.
    }
    game.nearClip = numberOf(are, *root, "Near_Clip", game.nearClip);
    game.farClip = numberOf(are, *root, "Far_Clip", game.farClip);
    game.cameraViewAngle = numberOf(are, *root, "CameraViewAngle", game.cameraViewAngle);
    game.bumpPlane = integerOf(are, *root, "Bump_Plane", 0);
    game.useInertia = integerOf(are, *root, "UseInertia", 0) != 0;
    game.doBumping = integerOf(are, *root, "DoBumping", 0) != 0;
    game.dof = integerOf(are, *root, "DOF", 0);
    // retail 006723d0: 6 for a swoop, 90 for a turret when the field is absent.
    game.movementPerSec = numberOf(are, *root, "MovementPerSec", game.type == 1 ? 6.0f : 90.0f);
    game.lateralAccel = numberOf(are, *root, "LateralAccel", game.lateralAccel);
    game.music = lowered(textOf(are, *root, "Music"));
    if (const std::optional<std::uint32_t> mouse = structOf(are, *root, "Mouse")) {
        // A flip negates the axis; retail keeps the sign on the axis index (00671bc0).
        game.mouseAxisX = integerOf(are, *mouse, "AxisX", 0);
        game.mouseAxisY = integerOf(are, *mouse, "AxisY", 0);
        if (integerOf(are, *mouse, "FlipAxisX", 0) != 0) {
            game.mouseAxisX = -game.mouseAxisX;
        }
        if (integerOf(are, *mouse, "FlipAxisY", 0) != 0) {
            game.mouseAxisY = -game.mouseAxisY;
        }
    }
    if (const std::optional<std::uint32_t> player = structOf(are, *root, "Player")) {
        parseFollower(are, *player, game.player);
        MgPlayerData& p = game.player;
        p.camera = lowered(textOf(are, *player, "Camera"));
        p.cameraRotate = integerOf(are, *player, "CameraRotate", 1) != 0;
        p.minimumSpeed = numberOf(are, *player, "Minimum_Speed", p.minimumSpeed);
        p.maximumSpeed = numberOf(are, *player, "Maximum_Speed", p.maximumSpeed);
        p.accelSeconds = numberOf(are, *player, "Accel_Secs", p.accelSeconds);
        p.startOffset = parseVector(are, *player, "Start_Offset_X", "Start_Offset_Y", "Start_Offset_Z");
        p.tunnelPositive = parseVector(are, *player, "TunnelXPos", "TunnelYPos", "TunnelZPos");
        p.tunnelNegative = parseVector(are, *player, "TunnelXNeg", "TunnelYNeg", "TunnelZNeg");
        p.targetOffset = parseVector(are, *player, "Target_Offset_X", "Target_Offset_Y", "Target_Offset_Z");
        if (const GffField* infinite = are.field(*player, "TunnelInfinite")) {
            p.tunnelInfinite = are.vector3(*infinite);
        }
    }
    for (const std::uint32_t row : listOf(are, *root, "Enemies")) {
        MgEnemyData enemy;
        parseFollower(are, row, enemy);
        enemy.trigger = integerOf(are, row, "Trigger", 0) != 0;
        game.enemies.push_back(std::move(enemy));
    }
    for (const std::uint32_t row : listOf(are, *root, "Obstacles")) {
        MgObstacleData obstacle;
        obstacle.name = lowered(textOf(are, row, "Name"));
        obstacle.scripts = parseScripts(are, row);
        game.obstacles.push_back(std::move(obstacle));
    }
    return game;
}

MgLayout parseMiniGameLayout(const std::vector<std::uint8_t>& lyt)
{
    MgLayout layout;
    std::istringstream stream(std::string(lyt.begin(), lyt.end()));
    std::string line;
    std::vector<MgLayoutEntry>* section = nullptr;
    int remaining = 0;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string word;
        if (!(row >> word)) {
            continue;
        }
        const std::string key = lowered(word);
        if (remaining > 0 && section != nullptr) {
            MgLayoutEntry entry;
            entry.name = key;
            row >> entry.origin.x >> entry.origin.y >> entry.origin.z;
            section->push_back(std::move(entry));
            --remaining;
            continue;
        }
        section = nullptr;
        if (key == "trackcount") {
            section = &layout.tracks;
            row >> remaining;
        } else if (key == "obstaclecount") {
            section = &layout.obstacles;
            row >> remaining;
        } else if (key == "roomcount" || key == "doorhookcount") {
            // Skip these rows: the scene loader owns them.
            int rows = 0;
            row >> rows;
            for (int i = 0; i < rows && std::getline(stream, line); ++i) {
            }
        }
    }
    return layout;
}

} // namespace ke::kotor
