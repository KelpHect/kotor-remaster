// The runtime of a shipped minigame module: the player follower on its track, the
// enemy followers, obstacles, gun banks and bullets, all driven by the module's own
// data and scripts. Retail evidence is cited per routine in MiniGame3D.cpp and the
// contract is docs/recon/53_MINIGAMES.md. Win and loss are never decided here: the
// shipped scripts decide them, and this file only runs what they ask for.
#pragma once

#include "kotor/Formats.h"
#include "kotor/MiniGameData.h"
#include "render/SceneView.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ke {

struct ScriptApi;
class GameSession;

// Player-level input for one tick. Bots and the window use this same path.
struct MiniGameInput {
    float axisX = 0.0f; // Swoop: lateral, + is right. Turret: keyboard yaw.
    float axisY = 0.0f; // Turret: keyboard pitch. Unused on a swoop.
    float mouseX = 0.0f; // Turret: 1 turns at MovementPerSec degrees per second.
    float mouseY = 0.0f;
    bool fire = false;   // Swoop: gear shift through OnFire. Turret: pull the trigger.
};

struct MiniGameFollowerView {
    std::uint32_t object = 0;
    std::string name;
    kotor::Vec3 position;
    kotor::Vec3 velocity; // World units per second over the last tick.
    std::int32_t hitPoints = 0;
    std::int32_t maxHitPoints = 0;
    float sphereRadius = 0.0f;
    bool trigger = false;
};

struct MiniGameObstacleView {
    std::uint32_t object = 0;
    std::string name;
    kotor::Vec3 position;
};

// Everything a bot may read. No field grants a win.
struct MiniGameView {
    bool active = false;
    std::int32_t type = 0;
    std::string module;
    bool started = false;
    kotor::Vec3 playerPosition;
    kotor::Vec3 playerOffset;
    kotor::Vec3 tunnelPositive;
    kotor::Vec3 tunnelNegative;
    float speed = 0.0f;
    float minimumSpeed = 0.0f;
    float maximumSpeed = 0.0f;
    float acceleration = 0.0f;
    std::int32_t hitPoints = 0;
    std::int32_t maxHitPoints = 0;
    float invulnerable = 0.0f;
    kotor::Vec3 aimOrigin;    // Where the first player gun's bullets leave from.
    kotor::Vec3 aimDirection;
    float bulletSpeed = 0.0f;
    float bulletDamage = 0.0f;
    std::size_t liveBullets = 0;
    std::size_t bulletsFired = 0;
    std::size_t bulletHits = 0;     // Bullet to hit-mesh collisions so far.
    std::size_t playerBulletsFired = 0;
    std::size_t playerBulletHits = 0; // The player's bullets on an enemy's hit mesh.
    std::size_t triggerHits = 0;    // Player spheres meeting trigger enemies.
    std::size_t obstacleHits = 0;   // Player paths crossing an obstacle's hit mesh.
    bool endGamePending = false;    // A shipped script called EndGame.
    bool endGameGui = false;
    std::vector<MiniGameFollowerView> enemies;
    std::vector<MiniGameObstacleView> obstacles;
};

struct MgObject;
struct MgFollower;
struct MgGun;
struct MgBullet;

struct MgAnim {
    std::string name;
    float time = 0.0f;
    float rate = 1.0f;
    float duration = 0.0f;
    bool loop = true;
};

// One model in the world.
struct MgInstance {
    std::string name;
    std::shared_ptr<kotor::Mdl> model;
    MgAnim base;
    bool holding = false; // A one-shot base clip ended and holds its last frame.
    std::vector<MgAnim> queue;
    std::vector<MgAnim> overlays;
    kotor::Mat3 rotation;
    kotor::Vec3 position;
    kotor::Vec3 previousPosition;
    bool rotating = true;
    bool alive = true;
    std::size_t mesh = static_cast<std::size_t>(-1);
    // Who gets this model's animation events.
    MgObject* owner = nullptr;
    MgGun* gun = nullptr;
    MgBullet* bullet = nullptr;
    bool isTrack = false;
    // Last baked state, so unchanged models are not rebuilt.
    std::vector<kotor::MdlAnimLayer> bakedLayers;
    kotor::Vec3 bakedPosition;
    kotor::Mat3 bakedRotation;
    bool baked = false;

    std::vector<kotor::MdlAnimLayer> layers() const;
};

// The fields retail keeps on its minigame object base (0066c130, 0066c160).
struct MgObject {
    std::uint32_t object = 0;
    std::string name;
    kotor::MgScripts scripts;
    bool isFollower = false;
    std::string lastEvent;
    std::string lastEventModel;
    std::int32_t lastBulletDamage = 0;
    std::int32_t lastBulletTarget = 0;
    std::uint32_t lastBulletShooter = 0x7F000000u;
    std::string lastBulletPart;
    kotor::Vec3 lastBulletPoint;
    std::string lastBulletSound;
    std::uint32_t lastFollowerHit = 0x7F000000u;
    std::vector<std::unique_ptr<MgInstance>> models;
};

struct MgGun {
    MgFollower* follower = nullptr;
    std::size_t bank = 0;
    std::unique_ptr<MgInstance> instance;
    MgInstance* parent = nullptr;
    std::string hook;
    float cooldown = 0.0f;
    bool inRange = false;
    std::size_t bulletMeshPool = 0;
};

struct MgBullet {
    std::unique_ptr<MgInstance> instance;
    kotor::Vec3 forward;
    float speed = 0.0f;
    float life = 0.0f;
    std::int32_t damage = 0;
    std::int32_t targetType = 0;
    std::string collisionSound;
    MgFollower* shooter = nullptr;
    bool exploding = false;
    bool dead = false;
};

struct MgFollower : MgObject {
    bool isPlayer = false;
    bool trigger = false;
    std::unique_ptr<MgInstance> track;
    kotor::Vec3 layoutOrigin;
    std::vector<kotor::MgGunBankData> banks;
    std::vector<std::unique_ptr<MgGun>> guns;
    float sphereRadius = 0.0f;
    float invulnerable = 0.0f;
    float invincePeriod = 0.0f;
    std::int32_t hitPoints = 100;
    std::int32_t maxHitPoints = 100;
    std::int32_t bumpDamage = 0;
    std::int32_t numLoops = -1;
    std::int32_t lastHpChange = 0;
    std::uint32_t lastObstacleHit = 0x7F000000u;
    std::int32_t lastFiredDamage = 0;
    std::int32_t lastFiredTarget = 0;
    float speed = 100.0f; // retail default at +0x98
    bool started = false;
    bool dead = false;
    bool removed = false;
    std::string deathSound;
    std::string engineSound;
    // SWMG_*Sound*: index 0 engine, 1 death (nwscript names the slots in the same order).
    std::array<std::int32_t, 2> soundFrequency{0, 0};
    std::array<std::int32_t, 2> soundFrequencyRandom{0, 0};
    std::array<std::int32_t, 2> soundVolume{127, 127};
    kotor::Vec3 previousPosition;
    kotor::Vec3 velocity;
};

struct MgObstacle : MgObject {
    std::vector<std::array<kotor::Vec3, 3>> hitBump;
    std::vector<std::array<kotor::Vec3, 3>> hitBullet;
};

class MiniGameWorld {
public:
    // Builds the world from the module's ARE and LYT. Null when the area has no MiniGame.
    static std::unique_ptr<MiniGameWorld> create(ScriptApi api);

    // OnCreate scripts and track start, retail's order inside its area load (00605e50).
    void start(ScriptApi api);
    void tick(ScriptApi api, float dt);

    // Writes model poses into the scene's mesh pool. Returns how many meshes changed.
    std::size_t bake(kotor::Scene& scene);
    SceneCamera camera() const;
    MiniGameView view() const;
    std::uint64_t traceHash() const { return trace_; }
    const std::vector<std::string>& scriptLog() const { return scriptLog_; }

    // ---- script surface (ScriptMiniGame.cpp) ----
    MgObject* object(std::uint32_t id) const;
    MgFollower* follower(std::uint32_t id) const;
    MgObject* byName(const std::string& name) const;
    MgFollower* player() const { return player_.get(); }
    const std::vector<MgFollower*>& enemies() const { return enemies_; }
    const std::vector<std::unique_ptr<MgObstacle>>& obstacles() const { return obstacles_; }

    bool play(MgObject& target, const std::string& animation, float rate, unsigned flags);
    void removeAnimation(MgObject& target, const std::string& animation);
    void adjustHitPoints(ScriptApi api, MgFollower& follower, std::int32_t delta);
    void onBulletHit(ScriptApi api, MgObject& target, bool fromDefault);
    void onObstacleHit(ScriptApi api, MgFollower& follower, bool fromDefault);
    void onDamage(ScriptApi api, MgFollower& follower, bool fromDefault);
    void onDeath(ScriptApi api, MgFollower& follower, bool fromDefault);
    void setTrackSpeed(MgFollower& follower, float speed);
    void setBulletModel(MgFollower& follower, std::size_t bank, const std::string& model);
    void reseed(std::uint32_t value) { seed = value; rng_ = value; }
    void setMeshesHidden(bool hidden) { meshesHidden_ = hidden; }

    // ---- public tuning state, read and written by the SWMG actions ----
    kotor::MiniGameData data;
    float nearClip = 0.1f;
    float farClip = 100.0f;
    float lateralAccel = 60.0f;
    float movementPerSec = 6.0f;
    float minimumSpeed = -1.0f;
    float maximumSpeed = 100.0f;
    float acceleration = 0.0f;
    kotor::Vec3 offset;
    kotor::Vec3 tunnelPositive;
    kotor::Vec3 tunnelNegative;
    kotor::Vec3 tunnelInfinite;
    kotor::Vec3 origin;
    kotor::Vec3 targetOffset;
    bool speedBlur = false;
    float speedBlurRatio = 0.75f;
    MiniGameInput input;
    std::uint32_t seed = 1;

    // Test hooks into pure math, so assertions read the same code the game runs.
    void stepLateral(kotor::Vec3 keys, float dt);
    void clampOffset();
    kotor::Vec3 lateralVelocity() const { return lateralVelocity_; }
    kotor::Vec3 lateralPosition() const { return lateralPosition_; }
    // Slides the player sideways so its path meets `world` on this coming tick.
    void setPlayerOffsetForTest(kotor::Vec3 world);

private:
    MiniGameWorld() = default;

    struct ModelEntry {
        std::shared_ptr<kotor::Mdl> model;
        std::vector<std::array<kotor::Vec3, 3>> hitBullet;
        std::vector<std::array<kotor::Vec3, 3>> hitBump;
    };
    struct PendingEvent {
        MgInstance* instance = nullptr;
        std::string name;
    };

    const ModelEntry* model(ScriptApi api, const std::string& name);
    std::unique_ptr<MgInstance> instance(ScriptApi api, const std::string& name, bool mesh);
    void loadFollower(ScriptApi api, MgFollower& follower, const kotor::MgFollowerData& source);
    void loadGuns(ScriptApi api, MgFollower& follower);
    bool playOn(MgInstance& instance, const std::string& animation, float rate, unsigned flags);
    void advance(MgInstance& instance, float dt);
    void dispatchEvents(ScriptApi api);
    void handleEvent(ScriptApi api, MgInstance& instance, const std::string& name);
    void gunEvent(ScriptApi api, MgGun& gun, const std::string& name);
    void spawnBullet(ScriptApi api, MgGun& gun, int hookIndex);
    void fireGun(ScriptApi api, MgGun& gun);
    void aimEnemyGun(ScriptApi api, MgGun& gun);
    void placeFollower(MgFollower& follower);
    void placeGuns(MgFollower& follower);
    void updateFollower(ScriptApi api, MgFollower& follower, float dt);
    void updateBullets(ScriptApi api, float dt);
    bool bulletHitsObject(ScriptApi api, MgBullet& bullet, MgObject& target,
                          const std::vector<std::array<kotor::Vec3, 3>>* obstacleMesh);
    void collideBullets(ScriptApi api);
    void collidePlayer(ScriptApi api);
    void updateSpeedAndOffset(float dt);
    void applyBank(float lateral, float dt);
    bool runSlot(ScriptApi api, MgObject& target, std::size_t slot);
    void playSound(ScriptApi api, const std::string& resref, const kotor::Vec3* where);
    void destroyInstance(MgInstance& instance);
    void releaseBullet(MgBullet& bullet);
    std::int32_t nextRandom();
    void mix(std::uint64_t value);
    void mix(const std::string& text);
    void mix(kotor::Vec3 value);

    std::string module_;
    kotor::MgLayout layout_;
    std::map<std::string, ModelEntry> models_;
    std::unique_ptr<MgFollower> player_;
    std::vector<std::unique_ptr<MgFollower>> enemyStore_;
    std::vector<MgFollower*> enemies_;
    std::vector<std::unique_ptr<MgObstacle>> obstacles_;
    std::vector<std::unique_ptr<MgBullet>> bullets_;
    std::unique_ptr<MgInstance> cameraModel_;
    std::map<std::uint32_t, MgObject*> objects_;
    std::vector<PendingEvent> pending_;
    std::map<std::string, std::vector<std::size_t>> freeBulletMeshes_;
    std::vector<std::unique_ptr<MgInstance>> retiredInstances_;
    kotor::Mat3 cameraRotation_;
    // retail state at +0xc0: velocity and position of the lateral integrator.
    kotor::Vec3 lateralVelocity_;
    kotor::Vec3 lateralPosition_;
    float bank_ = 0.0f;
    int bankLevel_ = 0;
    bool started_ = false;
    bool fireHeld_ = false;
    bool meshesHidden_ = false;
    std::size_t bulletsFired_ = 0;
    std::size_t bulletHits_ = 0;
    std::size_t playerBulletsFired_ = 0;
    std::size_t playerBulletHits_ = 0;
    std::size_t triggerHits_ = 0;
    std::size_t obstacleHits_ = 0;
    std::uint32_t rng_ = 1;
    std::uint64_t trace_ = 0xcbf29ce484222325ull;
    std::vector<std::string> scriptLog_;
};

} // namespace ke
