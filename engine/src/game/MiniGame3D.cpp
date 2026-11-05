// Minigame simulation. Evidence is the retail routine named beside each rule; rules
// the exports left ambiguous are called out as such. Nothing here decides who wins.
#include "game/MiniGame3D.h"

#include "game/AudioSystem.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ke {

namespace {

using kotor::Mat3;
using kotor::Vec3;

constexpr float kDegrees = 180.0f / 3.14159265358979f;
constexpr float kRadians = 3.14159265358979f / 180.0f;
constexpr std::size_t kNoMesh = static_cast<std::size_t>(-1);
constexpr std::uint32_t kInvalid = 0x7F000000u;
// How many bullets of one model can be drawn at once. A renderer limit, not a retail
// rule: the simulation keeps every bullet, a bullet past the pool just is not drawn.
constexpr std::size_t kBulletMeshPool = 48;

// Animation flags, as retail passes them to the model's play call (005cb730, 0066d550).
constexpr unsigned kOneShot = 0x01;
constexpr unsigned kKeepTime = 0x10;
constexpr unsigned kOverlay = 0x08;
constexpr unsigned kQueue = 0x20;

Mat3 identityMat()
{
    Mat3 m;
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    return m;
}

Mat3 mul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

Vec3 apply(const Mat3& m, Vec3 v)
{
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

Vec3 applyInverse(const Mat3& m, Vec3 v)
{
    return {m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
            m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
            m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}

Vec3 column(const Mat3& m, int c)
{
    return {m.m[0][c], m.m[1][c], m.m[2][c]};
}

Mat3 rotationX(float degrees)
{
    const float c = std::cos(degrees * kRadians);
    const float s = std::sin(degrees * kRadians);
    Mat3 m = identityMat();
    m.m[1][1] = c;
    m.m[1][2] = -s;
    m.m[2][1] = s;
    m.m[2][2] = c;
    return m;
}

Mat3 rotationZ(float degrees)
{
    const float c = std::cos(degrees * kRadians);
    const float s = std::sin(degrees * kRadians);
    Mat3 m = identityMat();
    m.m[0][0] = c;
    m.m[0][1] = -s;
    m.m[1][0] = s;
    m.m[1][1] = c;
    return m;
}

// Basis whose local Y is `forward`. Retail orients an aimed bullet the same way: a
// look-at followed by a quarter turn about X (00673d7d), and flies it along local Y
// (006db090 reads the quaternion's Y axis through 0044f420).
Mat3 basisAlongY(Vec3 forward)
{
    const Vec3 y = kotor::normalize(forward);
    Vec3 x = kotor::cross(y, Vec3{0.0f, 0.0f, 1.0f});
    if (kotor::length(x) < 1e-5f) {
        x = {1.0f, 0.0f, 0.0f};
    }
    x = kotor::normalize(x);
    const Vec3 z = kotor::cross(x, y);
    Mat3 m;
    m.m[0][0] = x.x; m.m[1][0] = x.y; m.m[2][0] = x.z;
    m.m[0][1] = y.x; m.m[1][1] = y.y; m.m[2][1] = y.z;
    m.m[0][2] = z.x; m.m[1][2] = z.y; m.m[2][2] = z.z;
    return m;
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool sameMat(const Mat3& a, const Mat3& b)
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (a.m[i][j] != b.m[i][j]) {
                return false;
            }
        }
    }
    return true;
}

bool sameLayers(const std::vector<kotor::MdlAnimLayer>& a, const std::vector<kotor::MdlAnimLayer>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].time != b[i].time || a[i].animation != b[i].animation) {
            return false;
        }
    }
    return true;
}

// Moller-Trumbore. Returns the segment parameter of the first hit, or a negative value.
float segmentTriangle(Vec3 from, Vec3 to, const std::array<Vec3, 3>& face)
{
    const Vec3 direction = to - from;
    const Vec3 edge1 = face[1] - face[0];
    const Vec3 edge2 = face[2] - face[0];
    const Vec3 p = kotor::cross(direction, edge2);
    const float det = kotor::dot(edge1, p);
    if (std::fabs(det) < 1e-9f) {
        return -1.0f;
    }
    const float inverse = 1.0f / det;
    const Vec3 s = from - face[0];
    const float u = kotor::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const Vec3 q = kotor::cross(s, edge1);
    const float v = kotor::dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    const float t = kotor::dot(edge2, q) * inverse;
    return t >= 0.0f && t <= 1.0f ? t : -1.0f;
}

// First crossing of a world-space segment with `faces`, placed by `instance`.
float segmentMesh(Vec3 from, Vec3 to, const MgInstance& instance,
                  const std::vector<std::array<Vec3, 3>>& faces)
{
    const Vec3 a = applyInverse(instance.rotation, from - instance.position);
    const Vec3 b = applyInverse(instance.rotation, to - instance.position);
    float best = -1.0f;
    for (const std::array<Vec3, 3>& face : faces) {
        const float t = segmentTriangle(a, b, face);
        if (t >= 0.0f && (best < 0.0f || t < best)) {
            best = t;
        }
    }
    return best;
}

// A sphere at the origin against the segment from `from` to `to`. retail 004abdd0.
bool segmentSphere(Vec3 from, Vec3 to, float radius)
{
    const Vec3 d = to - from;
    const float a = kotor::dot(d, d);
    const float c = kotor::dot(from, from) - radius * radius;
    if (c <= 0.0f) {
        return true;
    }
    if (a <= 0.0f) {
        return false;
    }
    const float b = kotor::dot(from, d);
    if (b >= 0.0f) {
        return false;
    }
    const float t = std::min(-b / a, 1.0f);
    const Vec3 nearest = from + d * t;
    return kotor::dot(nearest, nearest) <= radius * radius;
}

std::string gunHookName(std::int32_t bankId)
{
    return "gunbank" + std::to_string(bankId);
}

} // namespace

std::vector<kotor::MdlAnimLayer> MgInstance::layers() const
{
    std::vector<kotor::MdlAnimLayer> out;
    out.reserve(overlays.size() + 1);
    if (!base.name.empty()) {
        out.push_back({base.name, base.time});
    }
    for (const MgAnim& overlay : overlays) {
        out.push_back({overlay.name, overlay.time});
    }
    return out;
}

// ---- construction -----------------------------------------------------------

const MiniGameWorld::ModelEntry* MiniGameWorld::model(ScriptApi api, const std::string& name)
{
    const std::string key = lowered(name);
    if (key.empty()) {
        return nullptr;
    }
    const auto found = models_.find(key);
    if (found != models_.end()) {
        return found->second.model ? &found->second : nullptr;
    }
    ModelEntry entry;
    entry.model = kotor::loadWorldModel(api.install(), key);
    if (entry.model) {
        entry.hitBullet = entry.model->nodeFaces("hitbullet");
        entry.hitBump = entry.model->nodeFaces("hitbump");
    }
    const auto inserted = models_.emplace(key, std::move(entry));
    return inserted.first->second.model ? &inserted.first->second : nullptr;
}

std::unique_ptr<MgInstance> MiniGameWorld::instance(ScriptApi api, const std::string& name, bool mesh)
{
    const ModelEntry* entry = model(api, name);
    if (entry == nullptr) {
        api.log("minigame: missing model " + name);
        return nullptr;
    }
    auto created = std::make_unique<MgInstance>();
    created->name = lowered(name);
    created->model = entry->model;
    created->rotation = identityMat();
    created->bakedRotation = identityMat();
    if (mesh) {
        kotor::EntityMesh built = kotor::buildWorldEntityMesh(api.scene(), api.install(), entry->model);
        if (!built.vertices.empty()) {
            api.scene().entityMeshes.push_back(std::move(built));
            created->mesh = api.scene().entityMeshes.size() - 1;
        }
    }
    return created;
}

void MiniGameWorld::loadGuns(ScriptApi api, MgFollower& follower)
{
    // retail 0066e490 builds one gun per model carrying the bank's gunbank node.
    for (std::size_t bank = 0; bank < follower.banks.size(); ++bank) {
        const kotor::MgGunBankData& source = follower.banks[bank];
        const std::string hook = gunHookName(source.bankId);
        for (const std::unique_ptr<MgInstance>& parent : follower.models) {
            if (!parent->model->hasNode(hook)) {
                continue;
            }
            auto gun = std::make_unique<MgGun>();
            gun->follower = &follower;
            gun->bank = bank;
            gun->parent = parent.get();
            gun->hook = hook;
            gun->instance = instance(api, source.gunModel, true);
            if (!gun->instance) {
                continue;
            }
            gun->instance->gun = gun.get();
            playOn(*gun->instance, "ready", 1.0f, 0);
            follower.guns.push_back(std::move(gun));
        }
        // The pool is sized once per bullet model, before the renderer sees the scene.
        const ModelEntry* bullet = model(api, source.bulletModel);
        if (bullet != nullptr && !freeBulletMeshes_.contains(source.bulletModel)) {
            std::vector<std::size_t>& pool = freeBulletMeshes_[source.bulletModel];
            for (std::size_t i = 0; i < kBulletMeshPool; ++i) {
                kotor::EntityMesh built =
                    kotor::buildWorldEntityMesh(api.scene(), api.install(), bullet->model);
                if (built.vertices.empty()) {
                    break;
                }
                built.visible = false;
                api.scene().entityMeshes.push_back(std::move(built));
                pool.push_back(api.scene().entityMeshes.size() - 1);
            }
        }
    }
}

void MiniGameWorld::loadFollower(ScriptApi api, MgFollower& follower, const kotor::MgFollowerData& source)
{
    follower.isFollower = true;
    follower.name = source.track;
    follower.scripts = source.scripts;
    follower.sphereRadius = source.sphereRadius;
    follower.invincePeriod = source.invincePeriod;
    follower.hitPoints = source.hitPoints;
    follower.maxHitPoints = source.maxHitPoints;
    follower.bumpDamage = source.bumpDamage;
    // retail's setters: a player takes -1 or a positive count (0066cf20), an enemy only
    // -1 or 1 (0066d090). Anything else, such as the absent-field default, is ignored.
    if (follower.isPlayer ? source.numLoops >= -1 && source.numLoops != 0 : std::abs(source.numLoops) == 1) {
        follower.numLoops = source.numLoops;
    }
    follower.deathSound = source.deathSound;
    follower.engineSound = source.engineSound;
    follower.banks = source.gunBanks;
    follower.layoutOrigin = layout_.trackOrigin(source.track);
    follower.object = api.world().addObject(follower.name).id;
    follower.track = instance(api, source.track, true);
    if (follower.track) {
        follower.track->isTrack = true;
        follower.track->owner = &follower;
    }
    for (const kotor::MgModelRef& ref : source.models) {
        std::unique_ptr<MgInstance> created = instance(api, ref.model, true);
        if (!created) {
            continue;
        }
        created->rotating = ref.rotating;
        created->owner = &follower;
        follower.models.push_back(std::move(created));
    }
    objects_[follower.object] = &follower;
    loadGuns(api, follower);
}

std::unique_ptr<MiniGameWorld> MiniGameWorld::create(ScriptApi api)
{
    kotor::Scene& scene = api.scene();
    std::optional<std::vector<std::uint8_t>> areBytes = api.install().find(scene.area, 2012);
    if (!areBytes.has_value()) {
        return nullptr;
    }
    std::optional<kotor::MiniGameData> parsed;
    try {
        parsed = kotor::parseMiniGame(kotor::Gff::parse(std::move(*areBytes), scene.area + ".are"));
    } catch (const kotor::FormatError& error) {
        api.log(std::string("minigame: bad ARE: ") + error.what());
        return nullptr;
    }
    if (!parsed.has_value()) {
        return nullptr;
    }
    std::unique_ptr<MiniGameWorld> world(new MiniGameWorld());
    world->data = std::move(*parsed);
    world->module_ = scene.module;
    if (std::optional<std::vector<std::uint8_t>> lyt = api.install().find(scene.area, 3000)) {
        world->layout_ = kotor::parseMiniGameLayout(*lyt);
    }
    world->nearClip = world->data.nearClip;
    world->farClip = world->data.farClip;
    world->lateralAccel = world->data.lateralAccel;
    world->movementPerSec = world->data.movementPerSec;
    const kotor::MgPlayerData& source = world->data.player;
    world->minimumSpeed = source.minimumSpeed;
    world->maximumSpeed = source.maximumSpeed;
    // retail 006702f0: the ramp rate comes from the time to cross the speed range.
    world->acceleration = source.accelSeconds > 0.0f
        ? (source.maximumSpeed - std::max(source.minimumSpeed, 0.0f)) / source.accelSeconds
        : 0.0f;
    world->offset = source.startOffset;
    world->tunnelPositive = source.tunnelPositive;
    world->tunnelNegative = source.tunnelNegative;
    world->tunnelInfinite = source.tunnelInfinite;
    world->targetOffset = source.targetOffset;
    world->cameraRotation_ = identityMat();

    world->player_ = std::make_unique<MgFollower>();
    world->player_->isPlayer = true;
    world->loadFollower(api, *world->player_, source);
    if (!source.camera.empty()) {
        world->cameraModel_ = world->instance(api, source.camera, false);
    }
    for (const kotor::MgEnemyData& enemy : world->data.enemies) {
        auto follower = std::make_unique<MgFollower>();
        follower->trigger = enemy.trigger;
        world->loadFollower(api, *follower, enemy);
        world->enemies_.push_back(follower.get());
        world->enemyStore_.push_back(std::move(follower));
    }
    for (const kotor::MgObstacleData& source_ : world->data.obstacles) {
        const ModelEntry* entry = world->model(api, source_.name);
        if (entry == nullptr) {
            continue; // retail 006723d0 drops an obstacle whose model will not load.
        }
        auto obstacle = std::make_unique<MgObstacle>();
        obstacle->name = source_.name;
        obstacle->scripts = source_.scripts;
        obstacle->hitBump = entry->hitBump;
        obstacle->hitBullet = entry->hitBullet;
        obstacle->object = api.world().addObject(obstacle->name).id;
        std::unique_ptr<MgInstance> created = world->instance(api, source_.name, true);
        created->owner = obstacle.get();
        created->position = world->layout_.obstacleOrigin(source_.name);
        created->previousPosition = created->position;
        world->playOn(*created, "ready", 1.0f, 0);
        obstacle->models.push_back(std::move(created));
        world->objects_[obstacle->object] = obstacle.get();
        world->obstacles_.push_back(std::move(obstacle));
    }
    return world;
}

void MiniGameWorld::start(ScriptApi api)
{
    rng_ = seed;
    // retail 0066c370: follower construction runs OnCreate. Obstacles have no such call.
    runSlot(api, *player_, kotor::kMgOnCreate);
    for (MgFollower* enemy : enemies_) {
        runSlot(api, *enemy, kotor::kMgOnCreate);
    }
    // retail 006714e0 / 006701c0: the idle clip, then the track at speed * 0.01.
    const auto startFollower = [this](MgFollower& follower) {
        for (const std::unique_ptr<MgInstance>& created : follower.models) {
            if (created->base.name.empty()) {
                playOn(*created, "Ready_01", 1.0f, 0);
            }
        }
        follower.started = true;
        setTrackSpeed(follower, follower.speed);
        placeFollower(follower);
        follower.previousPosition = follower.models.empty() ? Vec3{} : follower.models.front()->position;
        for (const std::unique_ptr<MgInstance>& created : follower.models) {
            created->previousPosition = created->position;
        }
        placeGuns(follower);
    };
    startFollower(*player_);
    for (MgFollower* enemy : enemies_) {
        startFollower(*enemy);
    }
    started_ = true;
    if (!data.music.empty()) {
        // Retail starts this as the area's music (005d5b10). The mixer path here plays
        // it once; the looped music channel is keyed by ambientmusic.2da row only.
        playSound(api, data.music, nullptr);
    }
    bake(api.scene());
    dispatchEvents(api);
}

// ---- lookups ----------------------------------------------------------------

MgObject* MiniGameWorld::object(std::uint32_t id) const
{
    const auto found = objects_.find(id);
    return found != objects_.end() ? found->second : nullptr;
}

MgFollower* MiniGameWorld::follower(std::uint32_t id) const
{
    MgObject* found = object(id);
    return found != nullptr && found->isFollower ? static_cast<MgFollower*>(found) : nullptr;
}

MgObject* MiniGameWorld::byName(const std::string& name) const
{
    const std::string wanted = lowered(name);
    for (const auto& [id, candidate] : objects_) {
        (void)id;
        if (candidate->name == wanted) {
            return candidate;
        }
    }
    return nullptr;
}

// ---- animation --------------------------------------------------------------

bool MiniGameWorld::playOn(MgInstance& target, const std::string& animation, float rate, unsigned flags)
{
    if (animation.empty()) {
        // retail 0066e2a0 passes a null name to stop everything before `die`.
        target.base = {};
        target.queue.clear();
        target.overlays.clear();
        target.holding = false;
        return true;
    }
    if (!target.model->hasAnimationAnyCase(animation)) {
        return false;
    }
    MgAnim clip;
    clip.name = animation;
    clip.rate = rate;
    clip.loop = (flags & kOneShot) == 0;
    clip.duration = target.model->animationDurationAnyCase(animation);
    if ((flags & kOverlay) != 0) {
        for (MgAnim& overlay : target.overlays) {
            if (lowered(overlay.name) == lowered(animation)) {
                overlay = clip;
                pending_.push_back({&target, "start" + lowered(animation)});
                return true;
            }
        }
        target.overlays.push_back(clip);
        pending_.push_back({&target, "start" + lowered(animation)});
        return true;
    }
    if ((flags & kQueue) != 0 && !target.base.name.empty() && !target.holding) {
        target.queue.push_back(clip);
        return true;
    }
    if ((flags & kKeepTime) != 0 && lowered(target.base.name) == lowered(animation)) {
        // retail re-issues `track` every speed change with this flag; the clip keeps its place.
        target.base.rate = rate;
        target.base.loop = clip.loop;
        return true;
    }
    target.base = clip;
    target.queue.clear();
    target.holding = false;
    pending_.push_back({&target, "start" + lowered(animation)});
    return true;
}

bool MiniGameWorld::play(MgObject& target, const std::string& animation, float rate, unsigned flags)
{
    // retail 0066d550: the call goes to every model the object has.
    bool any = false;
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        if (created->alive) {
            any = playOn(*created, animation, rate, flags) || any;
        }
    }
    return any;
}

void MiniGameWorld::removeAnimation(MgObject& target, const std::string& animation)
{
    const std::string wanted = lowered(animation);
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        std::erase_if(created->overlays,
                      [&wanted](const MgAnim& overlay) { return lowered(overlay.name) == wanted; });
        std::erase_if(created->queue,
                      [&wanted](const MgAnim& queued) { return lowered(queued.name) == wanted; });
        if (lowered(created->base.name) == wanted) {
            created->base = {};
            created->holding = false;
        }
    }
}

void MiniGameWorld::advance(MgInstance& target, float dt)
{
    if (!target.alive) {
        return;
    }
    const auto advanceClip = [&](MgAnim& clip) -> bool {
        const float previous = clip.time;
        clip.time += dt * clip.rate;
        const float duration = clip.duration;
        const std::vector<kotor::MdlAnimEvent> events = target.model->animationEvents(clip.name);
        const float limit = std::min(clip.time, std::max(duration, 0.0f));
        for (const kotor::MdlAnimEvent& event : events) {
            if (event.time >= previous && event.time < limit) {
                pending_.push_back({&target, lowered(event.name)});
            } else if (duration <= 0.0f && previous == 0.0f && event.time == 0.0f) {
                pending_.push_back({&target, lowered(event.name)});
            }
        }
        if (clip.time < duration) {
            return false;
        }
        if (clip.loop) {
            if (duration > 0.0f) {
                clip.time = std::fmod(clip.time, duration);
                for (const kotor::MdlAnimEvent& event : events) {
                    if (event.time < clip.time) {
                        pending_.push_back({&target, lowered(event.name)});
                    }
                }
                // retail: a loop notification; 0066fe50 acts on it for the track only.
                pending_.push_back({&target, "loop"});
            }
            return true;
        }
        clip.time = duration;
        pending_.push_back({&target, "done" + lowered(clip.name)});
        return true;
    };

    if (!target.base.name.empty() && !target.holding) {
        const bool finished = advanceClip(target.base);
        const bool oneShot = !target.base.loop;
        if (finished && !target.queue.empty()) {
            const MgAnim next = target.queue.front();
            target.queue.erase(target.queue.begin());
            target.base = next;
            pending_.push_back({&target, "start" + lowered(next.name)});
        } else if (finished && oneShot) {
            target.holding = true;
        }
    }
    for (std::size_t i = 0; i < target.overlays.size();) {
        const bool finished = advanceClip(target.overlays[i]);
        if (finished && !target.overlays[i].loop) {
            target.overlays.erase(target.overlays.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void MiniGameWorld::dispatchEvents(ScriptApi api)
{
    // Handlers can raise more events (an explosion's start, a queued clip's start).
    for (int guard = 0; guard < 16 && !pending_.empty(); ++guard) {
        std::vector<PendingEvent> batch;
        batch.swap(pending_);
        for (const PendingEvent& event : batch) {
            handleEvent(api, *event.instance, event.name);
        }
    }
}

void MiniGameWorld::handleEvent(ScriptApi api, MgInstance& target, const std::string& name)
{
    if (target.gun != nullptr) {
        gunEvent(api, *target.gun, name);
        return;
    }
    if (target.bullet != nullptr) {
        if (name == "doneexplode") {
            target.bullet->dead = true; // retail 00670b20 deletes the model.
        }
        return;
    }
    if (target.owner == nullptr) {
        return;
    }
    MgObject& owner = *target.owner;
    // retail 0066fe50: anything else (a plain `detonate`, say) has no minigame meaning.
    if (name.rfind("custom_", 0) == 0) {
        owner.lastEvent = name.substr(7);
        owner.lastEventModel = target.name;
        runSlot(api, owner, kotor::kMgOnAnimEvent);
        return;
    }
    if (name == "donedie") {
        // retail 0066de50 removes the model once `die` has played.
        destroyInstance(target);
        return;
    }
    if (name == "loop" && target.isTrack && owner.isFollower) {
        MgFollower& follower = static_cast<MgFollower&>(owner);
        if (follower.numLoops != -1) {
            --follower.numLoops;
            setTrackSpeed(follower, follower.speed);
        }
        runSlot(api, follower, kotor::kMgOnTrackLoop);
    }
}

void MiniGameWorld::destroyInstance(MgInstance& target)
{
    target.alive = false;
}

// ---- scripts ----------------------------------------------------------------

bool MiniGameWorld::runSlot(ScriptApi api, MgObject& target, std::size_t slot)
{
    const std::string& resref = target.scripts[slot];
    if (resref.empty()) {
        return false;
    }
    if (slot != kotor::kMgOnHeartbeat) {
        if (scriptLog_.size() < 20000) {
            scriptLog_.push_back(resref + ":" + target.name);
        }
        mix(resref);
    }
    // retail FUN_005d0fc0(script, follower id, 1): OBJECT_SELF is the object.
    api.runScript(resref, target.object, ScriptContext{});
    return true;
}

void MiniGameWorld::playSound(ScriptApi api, const std::string& resref, const kotor::Vec3* where)
{
    if (resref.empty()) {
        return;
    }
    if (AudioSystem* audio = api.session.audioOrNull()) {
        audio->playEffect(resref, where);
    }
}

// retail 0066c190. fromDefault is true when a script calls SWMG_OnBulletHit.
void MiniGameWorld::onBulletHit(ScriptApi api, MgObject& target, bool fromDefault)
{
    if (!fromDefault && runSlot(api, target, kotor::kMgOnHitBullet)) {
        return;
    }
    if (target.isFollower) {
        MgFollower& hit = static_cast<MgFollower&>(target);
        // Follower type mask: the player is 1, an enemy 2 (0063e7a0, 00670b10).
        const std::int32_t type = hit.isPlayer ? 1 : 2;
        if ((target.lastBulletTarget & type) != 0) {
            adjustHitPoints(api, hit, -target.lastBulletDamage);
        }
    }
    playSound(api, target.lastBulletSound, &target.lastBulletPoint);
}

// retail 0066e8e0
void MiniGameWorld::adjustHitPoints(ScriptApi api, MgFollower& target, std::int32_t delta)
{
    target.lastHpChange = delta;
    onDamage(api, target, false);
}

// retail 0066e7a0
void MiniGameWorld::onDamage(ScriptApi api, MgFollower& target, bool fromDefault)
{
    if (!fromDefault && runSlot(api, target, kotor::kMgOnDamage)) {
        return;
    }
    target.hitPoints += target.lastHpChange;
    if (target.lastHpChange < 0) {
        if (target.hitPoints > 0) {
            play(target, "damage", 1.0f, kOneShot);
            play(target, "Ready_01", 1.0f, kQueue);
        } else if (!target.dead) {
            target.dead = true;
            onDeath(api, target, false);
        }
    } else {
        target.hitPoints = std::min(target.hitPoints, target.maxHitPoints);
    }
}

// retail 0066e2a0
void MiniGameWorld::onDeath(ScriptApi api, MgFollower& target, bool fromDefault)
{
    if (!fromDefault && runSlot(api, target, kotor::kMgOnDeath)) {
        return;
    }
    Vec3 where = target.models.empty() ? Vec3{} : target.models.front()->position;
    playSound(api, target.deathSound, &where);
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        if (!created->alive) {
            continue;
        }
        playOn(*created, {}, 1.0f, 0);
        if (!playOn(*created, "die", 1.0f, kOneShot)) {
            destroyInstance(*created);
        }
    }
    for (const std::unique_ptr<MgGun>& gun : target.guns) {
        destroyInstance(*gun->instance); // The bank's vcall +4 retires the gun models.
    }
    target.guns.clear();
    target.dead = true;
    target.removed = true;
    std::erase(enemies_, &target); // retail 006715b0
    mix(target.name);
}

// retail 0066e5c0. The default stops the models' motion and marks the follower dead;
// every shipped swoop replaces it with an OnHitObstacle script.
void MiniGameWorld::onObstacleHit(ScriptApi api, MgFollower& target, bool fromDefault)
{
    if (!fromDefault && runSlot(api, target, kotor::kMgOnHitObstacle)) {
        return;
    }
    target.dead = true;
}

void MiniGameWorld::setTrackSpeed(MgFollower& target, float speed)
{
    // retail 0066c870: the clip's rate is speed * 0.01, one-shot only when Num_Loops is 1.
    target.speed = speed;
    if (target.track && target.started) {
        const unsigned flags = target.numLoops != 1 ? 0x14u : 0x11u;
        playOn(*target.track, "track", speed * 0.01f, flags | kKeepTime);
    }
}

void MiniGameWorld::setBulletModel(MgFollower& target, std::size_t bank, const std::string& name)
{
    // A model the pool was not built for still flies and collides; it just is not drawn,
    // because mesh topology is fixed once the renderer has the scene.
    if (bank < target.banks.size()) {
        target.banks[bank].bulletModel = lowered(name);
    }
}

// ---- movement ---------------------------------------------------------------

// retail 00670fb0: axis conventions, then RK4 on (velocity, position) with
// dv = accel * input - (accel / movementPerSec) * v, dp = v (00670a90). The state
// flag at +0xac selects this path; its constructor default is the only value seen.
void MiniGameWorld::stepLateral(Vec3 keys, float dt)
{
    const Vec3 in = data.type == 1 ? Vec3{keys.x, -keys.y, -keys.z}
                                   : Vec3{-keys.z, -keys.y, -keys.x};
    const float a = lateralAccel;
    const float drag = movementPerSec != 0.0f ? a / movementPerSec : 0.0f;
    const auto derivative = [&](Vec3 v) { return in * a - v * drag; };
    const Vec3 v0 = lateralVelocity_;
    const Vec3 k1v = derivative(v0);
    const Vec3 k1p = v0;
    const Vec3 v1 = v0 + k1v * (dt * 0.5f);
    const Vec3 k2v = derivative(v1);
    const Vec3 k2p = v1;
    const Vec3 v2 = v0 + k2v * (dt * 0.5f);
    const Vec3 k3v = derivative(v2);
    const Vec3 k3p = v2;
    const Vec3 v3 = v0 + k3v * dt;
    const Vec3 k4v = derivative(v3);
    const Vec3 k4p = v3;
    lateralVelocity_ = v0 + (k1v + k2v * 2.0f + k3v * 2.0f + k4v) * (dt / 6.0f);
    lateralPosition_ = lateralPosition_ + (k1p + k2p * 2.0f + k3p * 2.0f + k4p) * (dt / 6.0f);
}

void MiniGameWorld::setPlayerOffsetForTest(Vec3 world)
{
    if (player_ && !player_->models.empty()) {
        const MgInstance& body = *player_->models.front();
        offset.x += world.x - body.position.x;
        clampOffset();
    }
}

// retail 0066cb40. A finite axis clamps to the tunnel around the origin; an infinite
// axis on a turret is a heading, wrapped below 0 and above 359 by a full turn.
void MiniGameWorld::clampOffset()
{
    const auto one = [this](float value, float infinite, float negative, float positive, float base) {
        if (infinite == 0.0f) {
            return std::clamp(value, std::min(negative + base, positive + base),
                              std::max(negative + base, positive + base));
        }
        if (data.type == 2) {
            if (value < 0.0f) {
                return value + 360.0f;
            }
            if (value > 359.0f) {
                return value - 360.0f;
            }
        }
        return value;
    };
    offset.x = one(offset.x, tunnelInfinite.x, tunnelNegative.x, tunnelPositive.x, origin.x);
    offset.y = one(offset.y, tunnelInfinite.y, tunnelNegative.y, tunnelPositive.y, origin.y);
    offset.z = one(offset.z, tunnelInfinite.z, tunnelNegative.z, tunnelPositive.z, origin.z);
}

// retail 0066d640. The UseInertia branch also smooths the model toward the track's
// queued rotation keys (ReportKey lists at +0x1a4). That smoothing is not reproduced:
// models follow the modelhook orientation directly.
void MiniGameWorld::updateSpeedAndOffset(float dt)
{
    MgFollower& p = *player_;
    if (p.speed != maximumSpeed) {
        const float step = dt * acceleration;
        const float next = p.speed < maximumSpeed ? std::min(p.speed + step, maximumSpeed)
                                                  : std::max(p.speed - step, maximumSpeed);
        setTrackSpeed(p, next);
    }
    offset = offset + lateralPosition_;
    lateralPosition_ = {};
    clampOffset();
}

// retail 006710e0: a swoop's banking value and the Bank clip it selects (0066ce60).
void MiniGameWorld::applyBank(float lateral, float dt)
{
    if (lateral != 0.0f) {
        const bool sameSign = (lateral > 0.0f) == (bank_ > 0.0f) && bank_ != 0.0f;
        bank_ = sameSign ? std::clamp(bank_ + lateral * dt, -0.255f, 0.255f) : lateral * dt;
    } else if (bank_ != 0.0f) {
        const float size = std::fabs(bank_);
        const float sign = bank_ > 0.0f ? 1.0f : -1.0f;
        float snapped = size;
        if (size >= 0.23f && size < 0.235f) {
            snapped = 0.235f;
        } else if (size >= 0.17f && size < 0.175f) {
            snapped = 0.175f;
        } else if (size >= 0.1f && size < 0.105f) {
            snapped = 0.105f;
        } else if (size >= 0.042f && size < 0.047f) {
            snapped = 0.042f;
        }
        const float decayed = snapped - dt;
        bank_ = decayed > 0.0f ? decayed * sign : 0.0f;
    }
    const float size = std::fabs(bank_);
    static constexpr std::array<float, 10> kSteps{0.021f, 0.042f, 0.071f, 0.1f, 0.135f,
                                                  0.17f,  0.2f,   0.23f,  0.24f, 0.25f};
    int level = 0;
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        if (size >= kSteps[i]) {
            level = static_cast<int>(i) + 1;
        }
    }
    level = bank_ < 0.0f ? -level : level;
    if (level == bankLevel_) {
        return;
    }
    bankLevel_ = level;
    char clip[16];
    if (level == 0) {
        std::snprintf(clip, sizeof(clip), "Ready_01");
    } else {
        std::snprintf(clip, sizeof(clip), "Bank%c_%02d", level < 0 ? 'L' : 'R', std::abs(level));
    }
    play(*player_, clip, 1.0f, kKeepTime);
}

// Where a follower's models sit. retail 0066d2f0 attaches them to the track's modelhook;
// RotatingModel 0 attaches by position only (ebon hawk hull under the rotating turret).
// The turret offset as Euler degrees (pitch about X, then heading about Z) is an
// interpretation of the attachment fields set in 0066d640: the tunnel limits and the
// shipped k_heartbeat fit it, but the composing routine was not recovered.
void MiniGameWorld::placeFollower(MgFollower& target)
{
    Vec3 base = target.layoutOrigin;
    Mat3 hook = identityMat();
    if (target.track) {
        if (const std::optional<kotor::Mdl::Pose> pose =
                target.track->model->nodePoseLayered("modelhook", target.track->layers())) {
            base = base + pose->translation;
            hook = pose->rotation;
        }
    }
    const bool player = target.isPlayer;
    const Mat3 turret = player && data.type == 2 ? mul(rotationZ(offset.z), rotationX(offset.x)) : identityMat();
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        if (!created->alive) {
            continue;
        }
        created->previousPosition = created->position;
        if (created->rotating) {
            created->rotation = mul(hook, turret);
            created->position = player && data.type == 1 ? base + apply(hook, offset) : base;
        } else {
            created->rotation = identityMat();
            created->position = base;
        }
    }
    if (player && cameraModel_) {
        cameraModel_->rotation = data.player.cameraRotate ? mul(hook, turret) : hook;
        cameraModel_->position = base;
    }
}

void MiniGameWorld::placeGuns(MgFollower& target)
{
    for (const std::unique_ptr<MgGun>& gun : target.guns) {
        const std::optional<kotor::Mdl::Pose> pose =
            gun->parent->model->nodePoseLayered(gun->hook, gun->parent->layers());
        if (!pose.has_value()) {
            continue;
        }
        gun->instance->previousPosition = gun->instance->position;
        gun->instance->position = gun->parent->position + apply(gun->parent->rotation, pose->translation);
        gun->instance->rotation = mul(gun->parent->rotation, pose->rotation);
    }
}

// ---- guns and bullets ---------------------------------------------------------

std::int32_t MiniGameWorld::nextRandom()
{
    rng_ = rng_ * 214013u + 2531011u; // The C library generator retail calls at 006fc447.
    return static_cast<std::int32_t>((rng_ >> 16) & 0x7fffu);
}

// retail 006daec0
void MiniGameWorld::fireGun(ScriptApi api, MgGun& gun)
{
    if (gun.cooldown > 0.0f) {
        return;
    }
    const kotor::MgGunBankData& bank = gun.follower->banks[gun.bank];
    gun.cooldown = bank.rateOfFire;
    playOn(*gun.instance, "fire", 1.0f, kOneShot);
    playOn(*gun.instance, "ready", 1.0f, kQueue);
    playSound(api, bank.fireSound, &gun.instance->position);
}

// retail 006db180: within Sensing_Radius, aim within the spreads, clamped to them; the
// gun may fire only while the target is inside both. The horizontal angle's axis pair
// comes from 004aa0f0, whose tail was not read to the end: assumed atan2(x, y).
void MiniGameWorld::aimEnemyGun(ScriptApi api, MgGun& gun)
{
    gun.inRange = false;
    if (!player_ || player_->models.empty() || player_->removed || !player_->models.front()->alive) {
        return;
    }
    const kotor::MgGunBankData& bank = gun.follower->banks[gun.bank];
    const Vec3 toTarget = player_->models.front()->position - gun.instance->position;
    if (kotor::dot(toTarget, toTarget) > bank.sensingRadius * bank.sensingRadius) {
        return;
    }
    const Vec3 local = applyInverse(gun.instance->rotation, toTarget);
    float horizontal = std::atan2(local.x, local.y) * kDegrees;
    float vertical = std::atan2(local.z, std::sqrt(local.x * local.x + local.y * local.y)) * kDegrees;
    gun.inRange = true;
    if (std::fabs(horizontal) > bank.horizontalSpread) {
        gun.inRange = false;
        horizontal = horizontal >= 0.0f ? bank.horizontalSpread : -bank.horizontalSpread;
    }
    if (std::fabs(vertical) > bank.verticalSpread) {
        gun.inRange = false;
        vertical = vertical >= 0.0f ? bank.verticalSpread : -bank.verticalSpread;
    }
    gun.instance->rotation = mul(gun.instance->rotation, mul(rotationZ(-horizontal), rotationX(vertical)));
    if (gun.inRange && gun.cooldown <= 0.0f) {
        fireGun(api, gun);
    }
}

// retail 00673a40, 006738d0
void MiniGameWorld::gunEvent(ScriptApi api, MgGun& gun, const std::string& name)
{
    if (name == "startfire") {
        // The notification retail delivers as the clip begins: records, then OnFire.
        MgFollower& owner = *gun.follower;
        const kotor::MgGunBankData& bank = owner.banks[gun.bank];
        owner.lastFiredDamage = bank.damage;
        owner.lastFiredTarget = bank.targetType;
        runSlot(api, owner, kotor::kMgOnFire);
        return;
    }
    if (name.rfind("fire", 0) == 0 && name.size() > 4) {
        spawnBullet(api, gun, std::atoi(name.c_str() + 4));
    }
}

void MiniGameWorld::spawnBullet(ScriptApi, MgGun& gun, int hookIndex)
{
    MgFollower& owner = *gun.follower;
    const kotor::MgGunBankData& bank = owner.banks[gun.bank];
    const std::optional<kotor::Mdl::Pose> hook = gun.instance->model->nodePoseLayered(
        "bullethook" + std::to_string(hookIndex), gun.instance->layers());
    const auto entry = models_.find(bank.bulletModel);
    if (!hook.has_value() || entry == models_.end() || !entry->second.model) {
        return; // retail logs and fires nothing when the hook or model is missing.
    }
    auto bullet = std::make_unique<MgBullet>();
    bullet->instance = std::make_unique<MgInstance>();
    MgInstance& shot = *bullet->instance;
    shot.name = bank.bulletModel;
    shot.model = entry->second.model;
    shot.bullet = bullet.get();
    shot.bakedRotation = identityMat();
    shot.position = gun.instance->position + apply(gun.instance->rotation, hook->translation);
    shot.previousPosition = shot.position;
    shot.rotation = mul(gun.instance->rotation, hook->rotation);
    if (!owner.isPlayer && player_ && !player_->models.empty()) {
        // Enemy shots aim at the target's offset point, scattered off the line of
        // fire by Inaccuracy * distance (retail 00673bab..00673d7d).
        const MgInstance& targetModel = *player_->models.front();
        Vec3 aim = targetModel.position + apply(targetModel.rotation, targetOffset);
        if (bank.inaccuracy > 0.0f) {
            const Vec3 line = aim - shot.position;
            const float distance = kotor::length(line);
            const float rx = static_cast<float>(nextRandom() - 0x3fff);
            const float ry = static_cast<float>(nextRandom() - 0x3fff);
            const float rz = static_cast<float>(nextRandom() - 0x3fff);
            const Vec3 scatter = kotor::normalize(Vec3{rx, ry, rz}) * (bank.inaccuracy * distance);
            const Vec3 axis = kotor::normalize(line);
            aim = aim + scatter - axis * kotor::dot(scatter, axis);
        }
        shot.rotation = basisAlongY(aim - shot.position);
    }
    bullet->forward = column(shot.rotation, 1);
    bullet->speed = bank.speed;
    bullet->life = bank.lifespan;
    bullet->damage = bank.damage;
    bullet->targetType = bank.targetType;
    bullet->collisionSound = bank.collisionSound;
    bullet->shooter = &owner;
    std::vector<std::size_t>& pool = freeBulletMeshes_[bank.bulletModel];
    if (!pool.empty()) {
        shot.mesh = pool.back();
        pool.pop_back();
    }
    playOn(shot, "ready", 1.0f, 0);
    ++bulletsFired_;
    playerBulletsFired_ += owner.isPlayer ? 1u : 0u;
    mix(shot.position);
    bullets_.push_back(std::move(bullet));
}

void MiniGameWorld::releaseBullet(MgBullet& bullet)
{
    if (bullet.instance->mesh != kNoMesh) {
        freeBulletMeshes_[bullet.instance->name].push_back(bullet.instance->mesh);
    }
}

// retail 006db090
void MiniGameWorld::updateBullets(ScriptApi, float dt)
{
    for (const std::unique_ptr<MgBullet>& bullet : bullets_) {
        MgInstance& shot = *bullet->instance;
        advance(shot, dt);
        shot.previousPosition = shot.position;
        if (bullet->exploding) {
            continue;
        }
        bullet->life -= dt;
        if (bullet->life <= 0.0f) {
            bullet->dead = true;
            continue;
        }
        shot.position = shot.position + bullet->forward * (bullet->speed * dt);
    }
}

// retail 006730a0: the segment moved this frame against the target model's `hitbullet`.
bool MiniGameWorld::bulletHitsObject(ScriptApi api, MgBullet& bullet, MgObject& target,
                                     const std::vector<std::array<Vec3, 3>>* obstacleMesh)
{
    MgInstance& shot = *bullet.instance;
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        if (!created->alive) {
            continue;
        }
        const std::vector<std::array<Vec3, 3>>* faces = obstacleMesh;
        if (faces == nullptr) {
            const auto entry = models_.find(created->name);
            faces = entry != models_.end() ? &entry->second.hitBullet : nullptr;
        }
        if (faces == nullptr || faces->empty()) {
            continue;
        }
        const float t = segmentMesh(shot.previousPosition, shot.position, *created, *faces);
        if (t < 0.0f) {
            continue;
        }
        const Vec3 where = shot.previousPosition + (shot.position - shot.previousPosition) * t;
        target.lastBulletDamage = bullet.damage;
        target.lastBulletTarget = bullet.targetType;
        target.lastBulletShooter = bullet.shooter != nullptr ? bullet.shooter->object : kInvalid;
        target.lastBulletPart = "hitbullet";
        target.lastBulletPoint = where;
        target.lastBulletSound = bullet.collisionSound;
        ++bulletHits_;
        if (bullet.shooter == player_.get() && target.isFollower) {
            ++playerBulletHits_;
        }
        mix(target.name);
        onBulletHit(api, target, false);
        shot.position = where;
        if (playOn(shot, "explode", 1.0f, 3u)) {
            bullet.exploding = true;
        } else {
            bullet.dead = true;
        }
        return true;
    }
    return false;
}

// retail 006732f0: each live bullet against the player, the enemies, then the obstacles.
// 0066d4e0 skips the shooter's own models.
void MiniGameWorld::collideBullets(ScriptApi api)
{
    for (std::size_t i = 0; i < bullets_.size(); ++i) {
        MgBullet& bullet = *bullets_[i];
        if (bullet.dead || bullet.exploding) {
            continue;
        }
        if (bullet.shooter != player_.get() && !player_->removed
            && bulletHitsObject(api, bullet, *player_, nullptr)) {
            continue;
        }
        bool hit = false;
        const std::vector<MgFollower*> targets = enemies_;
        for (MgFollower* enemy : targets) {
            if (enemy != bullet.shooter && bulletHitsObject(api, bullet, *enemy, nullptr)) {
                hit = true;
                break;
            }
        }
        if (hit) {
            continue;
        }
        for (const std::unique_ptr<MgObstacle>& obstacle : obstacles_) {
            if (bulletHitsObject(api, bullet, *obstacle, &obstacle->hitBullet)) {
                break;
            }
        }
    }
}

// retail 0066eda0 and 0066e6b0. All four shipped ARE files have DoBumping 0 and no
// Bump_Damage, so the push-out branch of 0066eda0 cannot run and is not implemented.
void MiniGameWorld::collidePlayer(ScriptApi api)
{
    if (player_->models.empty() || !player_->models.front()->alive) {
        return;
    }
    const MgInstance& body = *player_->models.front();
    const std::vector<MgFollower*> targets = enemies_;
    for (MgFollower* enemy : targets) {
        if (!enemy->trigger || enemy->models.empty() || !enemy->models.front()->alive) {
            continue;
        }
        const MgInstance& other = *enemy->models.front();
        if (!segmentSphere(body.previousPosition - other.previousPosition, body.position - other.position,
                           player_->sphereRadius + enemy->sphereRadius)) {
            continue;
        }
        ++triggerHits_;
        mix(enemy->name);
        player_->lastFollowerHit = enemy->object;
        runSlot(api, *player_, kotor::kMgOnHitFollower);
        enemy->lastFollowerHit = player_->object;
        runSlot(api, *enemy, kotor::kMgOnHitFollower);
    }
    for (const std::unique_ptr<MgObstacle>& obstacle : obstacles_) {
        if (obstacle->hitBump.empty() || obstacle->models.empty()) {
            continue;
        }
        if (segmentMesh(body.previousPosition, body.position, *obstacle->models.front(), obstacle->hitBump) < 0.0f) {
            continue;
        }
        ++obstacleHits_;
        mix(obstacle->name);
        player_->lastObstacleHit = obstacle->object;
        onObstacleHit(api, *player_, false);
        obstacle->lastFollowerHit = player_->object;
        runSlot(api, *obstacle, kotor::kMgOnHitFollower);
    }
}

// retail 0066e130: heartbeat first, then the invulnerability timer, then the models.
void MiniGameWorld::updateFollower(ScriptApi api, MgFollower& target, float dt)
{
    if (!target.removed) {
        runSlot(api, target, kotor::kMgOnHeartbeat);
        target.invulnerable = std::max(0.0f, target.invulnerable - dt);
        for (const std::unique_ptr<MgGun>& gun : target.guns) {
            gun->cooldown = std::max(0.0f, gun->cooldown - dt);
        }
    }
    if (target.track && target.started && !target.removed) {
        advance(*target.track, dt);
    }
    const Vec3 before = target.models.empty() ? Vec3{} : target.models.front()->position;
    placeFollower(target);
    if (!target.models.empty()) {
        target.velocity = dt > 0.0f ? (target.models.front()->position - before) * (1.0f / dt) : Vec3{};
    }
    for (const std::unique_ptr<MgInstance>& created : target.models) {
        advance(*created, dt);
    }
    placeGuns(target);
    for (const std::unique_ptr<MgGun>& gun : target.guns) {
        advance(*gun->instance, dt);
        if (!target.isPlayer && !target.removed) {
            aimEnemyGun(api, *gun); // retail 006db3d0, each frame.
        }
    }
}

// ---- the frame ----------------------------------------------------------------

// Order follows retail: input at 006227e0, then the manager at 006735d0.
void MiniGameWorld::tick(ScriptApi api, float dt)
{
    if (!started_ || dt <= 0.0f) {
        return;
    }
    const MiniGameInput now = input;
    input = {};
    const float axisX = std::clamp(now.axisX, -1.0f, 1.0f);
    const float axisY = std::clamp(now.axisY, -1.0f, 1.0f);
    if (data.type == 1) {
        stepLateral({axisX, 0.0f, 0.0f}, dt);
    } else {
        stepLateral({axisX, 0.0f, axisY}, dt);
        // retail 00671020: the axis the ARE names moves directly, a flip reverses it.
        const auto moveAxis = [this, dt](std::int32_t axis, float amount) {
            const float delta = amount * dt * movementPerSec * (axis < 0 ? -1.0f : 1.0f);
            switch (std::abs(axis)) {
            case 1: lateralPosition_.x -= delta; break;
            case 2: lateralPosition_.y -= delta; break;
            case 3: lateralPosition_.z -= delta; break;
            default: break;
            }
        };
        moveAxis(data.mouseAxisX, now.mouseX);
        moveAxis(data.mouseAxisY, now.mouseY);
    }
    if (now.fire) {
        // retail key 0xd9 goes to 0066dc50: every bank on the player.
        for (const std::unique_ptr<MgGun>& gun : player_->guns) {
            fireGun(api, *gun);
        }
    }
    dispatchEvents(api);

    updateSpeedAndOffset(dt);
    if (data.type == 1) {
        applyBank(axisX, dt);
    }

    updateFollower(api, *player_, dt);
    for (std::size_t i = 0; i < enemyStore_.size(); ++i) {
        updateFollower(api, *enemyStore_[i], dt);
    }
    for (const std::unique_ptr<MgObstacle>& obstacle : obstacles_) {
        runSlot(api, *obstacle, kotor::kMgOnHeartbeat);
        for (const std::unique_ptr<MgInstance>& created : obstacle->models) {
            advance(*created, dt);
        }
    }
    updateBullets(api, dt);
    dispatchEvents(api);
    if (player_->started) {
        collidePlayer(api);
        collideBullets(api);
    }
    dispatchEvents(api);

    for (std::size_t i = 0; i < bullets_.size();) {
        if (bullets_[i]->dead) {
            releaseBullet(*bullets_[i]);
            // Retired, not freed: a script's later event can no longer reference it, but
            // the mesh is hidden on the next bake through the free list.
            bullets_.erase(bullets_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (!player_->models.empty()) {
        mix(player_->models.front()->position);
    }
    mix(offset);
    mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(player_->hitPoints)));
    mix(static_cast<std::uint64_t>(enemies_.size()));
}

// ---- output -------------------------------------------------------------------

std::size_t MiniGameWorld::bake(kotor::Scene& scene)
{
    std::size_t changed = 0;
    const auto bakeOne = [&](MgInstance& target) {
        if (target.mesh == kNoMesh || target.mesh >= scene.entityMeshes.size()) {
            return;
        }
        kotor::EntityMesh& mesh = scene.entityMeshes[target.mesh];
        const bool shown = target.alive && !meshesHidden_;
        if (mesh.visible != shown) {
            mesh.visible = shown;
            ++changed;
        }
        if (!target.alive) {
            return;
        }
        std::vector<kotor::MdlAnimLayer> layers = target.layers();
        if (target.baked && target.bakedPosition.x == target.position.x
            && target.bakedPosition.y == target.position.y && target.bakedPosition.z == target.position.z
            && sameMat(target.bakedRotation, target.rotation) && sameLayers(target.bakedLayers, layers)) {
            return;
        }
        kotor::bakeWorldEntityMesh(mesh, target.model, layers, target.rotation, target.position);
        target.bakedLayers = std::move(layers);
        target.bakedPosition = target.position;
        target.bakedRotation = target.rotation;
        target.baked = true;
        ++changed;
    };
    // The minigame has no creatures of its own: the party meshes stay out of it.
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity != UINT32_MAX && mesh.visible) {
            mesh.visible = false;
            ++changed;
        }
    }
    const auto followerMeshes = [&](MgFollower& target) {
        if (target.track) {
            bakeOne(*target.track);
        }
        for (const std::unique_ptr<MgInstance>& created : target.models) {
            bakeOne(*created);
        }
        for (const std::unique_ptr<MgGun>& gun : target.guns) {
            bakeOne(*gun->instance);
        }
    };
    followerMeshes(*player_);
    for (const std::unique_ptr<MgFollower>& enemy : enemyStore_) {
        followerMeshes(*enemy);
    }
    for (const std::unique_ptr<MgObstacle>& obstacle : obstacles_) {
        for (const std::unique_ptr<MgInstance>& created : obstacle->models) {
            bakeOne(*created);
        }
    }
    // Pool meshes not on a live bullet this frame are hidden, then live ones are shown.
    for (const auto& [name, pool] : freeBulletMeshes_) {
        (void)name;
        for (const std::size_t index : pool) {
            if (index < scene.entityMeshes.size() && scene.entityMeshes[index].visible) {
                scene.entityMeshes[index].visible = false;
                ++changed;
            }
        }
    }
    for (const std::unique_ptr<MgBullet>& bullet : bullets_) {
        bakeOne(*bullet->instance);
    }
    return changed;
}

SceneCamera MiniGameWorld::camera() const
{
    SceneCamera camera;
    camera.nearClip = nearClip;
    camera.farClip = farClip;
    camera.fovYDegrees = data.cameraViewAngle;
    const MgInstance* holder = nullptr;
    if (cameraModel_ && cameraModel_->model->hasNode("camerahook")) {
        holder = cameraModel_.get();
    } else if (player_) {
        for (const std::unique_ptr<MgInstance>& created : player_->models) {
            if (created->model->hasNode("camerahook")) {
                holder = created.get();
                break;
            }
        }
    }
    if (holder == nullptr) {
        // No shipped module takes this path; it keeps the view defined if one did.
        const Vec3 at = player_ && !player_->models.empty() ? player_->models.front()->position : Vec3{};
        camera.eye = at + Vec3{0.0f, -6.0f, 2.5f};
        camera.forward = {0.0f, 1.0f, 0.0f};
        return camera;
    }
    const std::optional<kotor::Mdl::Pose> pose = holder->model->nodePoseLayered("camerahook", holder->layers());
    const Mat3 rotation = pose.has_value() ? mul(holder->rotation, pose->rotation) : holder->rotation;
    camera.eye = pose.has_value() ? holder->position + apply(holder->rotation, pose->translation)
                                  : holder->position;
    // The shipped m12ab_camera and m17mg_camera hooks have local Y up and look along
    // local -Z, level with the track's +Y at rest.
    camera.forward = kotor::normalize(column(rotation, 2)) * -1.0f;
    camera.up = kotor::normalize(column(rotation, 1));
    return camera;
}

MiniGameView MiniGameWorld::view() const
{
    MiniGameView out;
    out.active = true;
    out.type = data.type;
    out.module = module_;
    out.started = started_;
    if (player_) {
        out.playerPosition = player_->models.empty() ? Vec3{} : player_->models.front()->position;
        out.speed = player_->speed;
        out.hitPoints = player_->hitPoints;
        out.maxHitPoints = player_->maxHitPoints;
        out.invulnerable = player_->invulnerable;
        if (!player_->guns.empty()) {
            const MgGun& gun = *player_->guns.front();
            const kotor::MgGunBankData& bank = player_->banks[gun.bank];
            out.bulletSpeed = bank.speed;
            out.bulletDamage = static_cast<float>(bank.damage);
            const std::optional<kotor::Mdl::Pose> hook =
                gun.instance->model->nodePoseLayered("bullethook0", gun.instance->layers());
            if (hook.has_value()) {
                out.aimOrigin = gun.instance->position + apply(gun.instance->rotation, hook->translation);
                out.aimDirection = column(mul(gun.instance->rotation, hook->rotation), 1);
            }
        }
    }
    out.playerOffset = offset;
    out.tunnelPositive = tunnelPositive;
    out.tunnelNegative = tunnelNegative;
    out.minimumSpeed = minimumSpeed;
    out.maximumSpeed = maximumSpeed;
    out.acceleration = acceleration;
    out.liveBullets = bullets_.size();
    out.bulletsFired = bulletsFired_;
    out.bulletHits = bulletHits_;
    out.playerBulletsFired = playerBulletsFired_;
    out.playerBulletHits = playerBulletHits_;
    out.triggerHits = triggerHits_;
    out.obstacleHits = obstacleHits_;
    for (const MgFollower* enemy : enemies_) {
        MiniGameFollowerView row;
        row.object = enemy->object;
        row.name = enemy->name;
        row.position = enemy->models.empty() ? Vec3{} : enemy->models.front()->position;
        row.velocity = enemy->velocity;
        row.hitPoints = enemy->hitPoints;
        row.maxHitPoints = enemy->maxHitPoints;
        row.sphereRadius = enemy->sphereRadius;
        row.trigger = enemy->trigger;
        out.enemies.push_back(std::move(row));
    }
    for (const std::unique_ptr<MgObstacle>& obstacle : obstacles_) {
        MiniGameObstacleView row;
        row.object = obstacle->object;
        row.name = obstacle->name;
        row.position = obstacle->models.empty() ? Vec3{} : obstacle->models.front()->position;
        out.obstacles.push_back(std::move(row));
    }
    return out;
}

void MiniGameWorld::mix(std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        trace_ ^= (value >> (i * 8)) & 0xffu;
        trace_ *= 0x100000001b3ull;
    }
}

void MiniGameWorld::mix(const std::string& text)
{
    for (const char c : text) {
        trace_ ^= static_cast<unsigned char>(c);
        trace_ *= 0x100000001b3ull;
    }
}

void MiniGameWorld::mix(Vec3 value)
{
    const auto bits = [](float f) {
        std::uint32_t out = 0;
        std::memcpy(&out, &f, sizeof(out));
        return static_cast<std::uint64_t>(out);
    };
    mix(bits(value.x));
    mix(bits(value.y));
    mix(bits(value.z));
}

} // namespace ke
