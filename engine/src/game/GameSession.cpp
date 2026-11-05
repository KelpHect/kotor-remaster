#include "game/GameSession.h"
#include "game/Pathfinding.h"

#include "core/Log.h"
#include "kotor/SaveGame.h"
#include "script/Ncs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ke {

const char* combatModeName(CombatMode mode)
{
    switch (mode) {
    case CombatMode::PowerAttack: return "power_attack";
    case CombatMode::Flurry: return "flurry";
    case CombatMode::CriticalStrike: return "critical_strike";
    case CombatMode::Whirlwind: return "whirlwind";
    case CombatMode::Normal: return "normal";
    }
    return "normal";
}

const char* uiPanelName(UiPanel panel)
{
    switch (panel) {
    case UiPanel::Character: return "character";
    case UiPanel::Inventory: return "inventory";
    case UiPanel::Party: return "party";
    case UiPanel::Journal: return "journal";
    case UiPanel::Merchant: return "merchant";
    case UiPanel::Loot: return "loot";
    case UiPanel::None: break;
    }
    return "none";
}

bool GameSession::applyGuiAction(const std::string& tag)
{
    if (tag == "LBL_ICON1") {
        toggleUiPanel(UiPanel::Character);
        return true;
    }
    if (tag == "LBL_ICON2") {
        toggleUiPanel(UiPanel::Inventory);
        return true;
    }
    if (tag == "LBL_ICON3") {
        toggleUiPanel(UiPanel::Party);
        return true;
    }
    if (tag == "LBL_ICON4") {
        toggleUiPanel(UiPanel::Journal);
        return true;
    }
    if (tag == "BTN_EXIT") {
        return true;
    }
    return false;
}

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEyeHeight = 1.55f;
constexpr float kPlayerRadius = 0.28f;
constexpr float kStepHeight = 0.55f;
constexpr float kGravity = 18.0f;

kotor::EntityKind entityKindForSaved(kotor::SavedObjectState::Kind kind)
{
    switch (kind) {
    case kotor::SavedObjectState::Kind::Creature: return kotor::EntityKind::Creature;
    case kotor::SavedObjectState::Kind::Door: return kotor::EntityKind::Door;
    case kotor::SavedObjectState::Kind::Placeable: return kotor::EntityKind::Placeable;
    case kotor::SavedObjectState::Kind::Trigger: return kotor::EntityKind::Trigger;
    case kotor::SavedObjectState::Kind::Store: return kotor::EntityKind::Store;
    case kotor::SavedObjectState::Kind::Encounter: return kotor::EntityKind::Encounter;
    case kotor::SavedObjectState::Kind::Waypoint: return kotor::EntityKind::Waypoint;
    case kotor::SavedObjectState::Kind::Sound: return kotor::EntityKind::Sound;
    case kotor::SavedObjectState::Kind::Other: break;
    }
    return kotor::EntityKind::Unknown;
}

float clamp(float value, float low, float high)
{
    return std::max(low, std::min(high, value));
}

float distance2D(kotor::Vec3 a, kotor::Vec3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(delimiter, start);
        parts.push_back(text.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

bool hasEffectLabel(const std::vector<kotor::ActiveEffect>& effects, const std::string& needle)
{
    for (const kotor::ActiveEffect& effect : effects) {
        if (effect.remaining > 0.0f
            && effect.label.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::int32_t effectBonus(const std::vector<kotor::ActiveEffect>& effects,
                         std::int32_t kotor::ActiveEffect::*member)
{
    std::int32_t total = 0;
    for (const kotor::ActiveEffect& effect : effects) {
        if (effect.remaining > 0.0f) {
            total += effect.*member;
        }
    }
    return total;
}

void tickEffects(std::vector<kotor::ActiveEffect>& effects, float dt)
{
    for (kotor::ActiveEffect& effect : effects) {
        effect.remaining = std::max(0.0f, effect.remaining - dt);
    }
    std::erase_if(effects, [](const kotor::ActiveEffect& effect) {
        return effect.remaining <= 0.0f;
    });
}

void addOrRefreshEffect(std::vector<kotor::ActiveEffect>& effects,
                        kotor::ActiveEffect effect)
{
    for (kotor::ActiveEffect& existing : effects) {
        if (existing.label == effect.label && existing.spellId == effect.spellId) {
            existing = std::move(effect);
            return;
        }
    }
    effects.push_back(std::move(effect));
}

kotor::Vec3 rotateQuaternion(float w, float x, float y, float z, kotor::Vec3 value)
{
    const float n = w * w + x * x + y * y + z * z;
    if (n <= 0.0f) {
        return value;
    }
    const float s = 2.0f / n;
    const float xx = x * x * s;
    const float yy = y * y * s;
    const float zz = z * z * s;
    const float xy = x * y * s;
    const float xz = x * z * s;
    const float yz = y * z * s;
    const float wx = w * x * s;
    const float wy = w * y * s;
    const float wz = w * z * s;
    return {(1.0f - yy - zz) * value.x + (xy - wz) * value.y + (xz + wy) * value.z,
            (xy + wz) * value.x + (1.0f - xx - zz) * value.y + (yz - wx) * value.z,
            (xz - wy) * value.x + (yz + wx) * value.y + (1.0f - xx - yy) * value.z};
}

kotor::Vec3 rotateZ(kotor::Vec3 value, float radians)
{
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {value.x * c - value.y * s, value.x * s + value.y * c, value.z};
}

// Barycentric floor height at (x, y); the highest triangle below maxZ wins.
bool floorHeight(const kotor::Scene& scene, float x, float y, float maxZ, float& out)
{
    bool found = false;
    float best = -1.0e30f;
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (!facet.walkable) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1.0e-8f) {
            continue;
        }
        const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / den;
        const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / den;
        const float w = 1.0f - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) {
            continue;
        }
        const float z = u * a.z + v * b.z + w * c.z;
        if (z <= maxZ + kStepHeight && z > best) {
            best = z;
            found = true;
        }
    }
    if (found) {
        out = best;
    }
    return found;
}

int floorSurface(const kotor::Scene& scene, float x, float y, float maxZ)
{
    int bestSurface = -1;
    float best = -1.0e30f;
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (!facet.floor) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1.0e-8f) {
            continue;
        }
        const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / den;
        const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / den;
        const float w = 1.0f - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) {
            continue;
        }
        const float z = u * a.z + v * b.z + w * c.z;
        if (z <= maxZ + kStepHeight && z > best) {
            best = z;
            bestSurface = static_cast<int>(facet.surface);
        }
    }
    return bestSurface;
}

bool segmentIntersects(const kotor::Vec3& a, const kotor::Vec3& b,
                       const kotor::Vec3& c, const kotor::Vec3& d)
{
    const float rX = b.x - a.x;
    const float rY = b.y - a.y;
    const float sX = d.x - c.x;
    const float sY = d.y - c.y;
    const float den = rX * sY - rY * sX;
    if (std::fabs(den) < 1.0e-8f) {
        return false;
    }
    const float qpx = c.x - a.x;
    const float qpy = c.y - a.y;
    const float t = (qpx * sY - qpy * sX) / den;
    const float u = (qpx * rY - qpy * rX) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

bool wallBlocked(const kotor::Scene& scene, kotor::Vec3 from, kotor::Vec3 to, float radius)
{
    // A small square around the path approximates the player's radius. It is
    // deliberately conservative: the first slice must not tunnel through a
    // wall, and sliding is handled by trying the two axes separately.
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float nx = len > 1.0e-6f ? -dy / len : 0.0f;
    const float ny = len > 1.0e-6f ? dx / len : 0.0f;
    const kotor::Vec3 offsets[2] = {{nx * radius, ny * radius, 0.0f}, {-nx * radius, -ny * radius, 0.0f}};
    const float zLow = std::min(from.z, to.z);
    const float zHigh = std::max(from.z, to.z);
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (facet.floor || !facet.blocksSight) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        // A wall on another storey is no wall here.
        if (std::max({a.z, b.z, c.z}) < zLow || std::min({a.z, b.z, c.z}) > zHigh) {
            continue;
        }
        for (const kotor::Vec3& offset : offsets) {
            const kotor::Vec3 p0{from.x + offset.x, from.y + offset.y, from.z};
            const kotor::Vec3 p1{to.x + offset.x, to.y + offset.y, to.z};
            if (segmentIntersects(p0, p1, a, b) || segmentIntersects(p0, p1, b, c)
                || segmentIntersects(p0, p1, c, a)) {
                return true;
            }
        }
    }
    return false;
}

bool pointInTriangle2D(float x, float y, const kotor::Vec3& a, const kotor::Vec3& b, const kotor::Vec3& c)
{
    const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
    const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
    const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
    const bool hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    const bool hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(hasNegative && hasPositive);
}

// nwscript OBJECT_TYPE_* for an entity kind.
std::int64_t objectTypeForKind(kotor::EntityKind kind)
{
    switch (kind) {
    case kotor::EntityKind::Creature: return 1;
    case kotor::EntityKind::Trigger: return 4;
    case kotor::EntityKind::Door: return 8;
    case kotor::EntityKind::Waypoint: return 32;
    case kotor::EntityKind::Placeable: return 64;
    case kotor::EntityKind::Store: return 128;
    case kotor::EntityKind::Encounter: return 256;
    case kotor::EntityKind::Sound: return 512;
    case kotor::EntityKind::Unknown: break;
    }
    return 0;
}

std::vector<script::Value> intCells(std::int64_t value)
{
    return {script::Value::makeInt(value)};
}

std::vector<script::Value> objectCells(std::uint32_t value)
{
    return {script::Value::makeObject(value)};
}

std::vector<script::Value> floatCells(double value)
{
    return {script::Value::makeFloat(value)};
}

} // namespace

std::int32_t GameSession::forceArmorBonus() const
{
    return effectBonus(character_.effects, &kotor::ActiveEffect::armorBonus);
}

std::int32_t GameSession::forceAttackBonus() const
{
    return effectBonus(character_.effects, &kotor::ActiveEffect::attackBonus);
}

std::int32_t GameSession::forceDamageBonus() const
{
    return effectBonus(character_.effects, &kotor::ActiveEffect::damageBonus);
}

std::int32_t GameSession::forceShield() const
{
    return effectBonus(character_.effects, &kotor::ActiveEffect::shield);
}

bool GameSession::hasEffect(const std::string& label) const
{
    return hasEffectLabel(character_.effects, label);
}

bool GameSession::enterStealth()
{
    if (!canStealth() || playerDead_ || dialogueView_.active) {
        return false;
    }
    if (stealthActive_) {
        return true;
    }
    stealthActive_ = true;
    combatTarget_ = UINT32_MAX;
    logEvent("stealth enter");
    return true;
}

void GameSession::leaveStealth()
{
    if (!stealthActive_) {
        return;
    }
    stealthActive_ = false;
    logEvent("stealth exit");
}

namespace {

kotor::EntityMesh* findEntityMesh(kotor::Scene& scene, std::size_t entityIndex)
{
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entityIndex) {
            return &mesh;
        }
    }
    return nullptr;
}

const kotor::EntityMesh* findEntityMesh(const kotor::Scene& scene, std::size_t entityIndex)
{
    for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entityIndex) {
            return &mesh;
        }
    }
    return nullptr;
}

} // namespace

bool GameSession::playAnimation(std::size_t entityIndex, const std::string& name, bool loop)
{
    kotor::EntityMesh* mesh = findEntityMesh(scene_, entityIndex);
    if (mesh == nullptr || !mesh->model || mesh->model->animationIndex(name) < 0) {
        return false;
    }
    const float transition = mesh->model->animationTransition(name);
    if (!mesh->animation.empty() && mesh->animation != name && transition > 0.0f) {
        mesh->previousAnimation = mesh->animation;
        mesh->previousAnimationTime = mesh->animationTime;
        mesh->blendDuration = transition;
        mesh->blendRemaining = transition;
    } else {
        mesh->previousAnimation.clear();
        mesh->blendDuration = 0.0f;
        mesh->blendRemaining = 0.0f;
    }
    mesh->animation = name;
    mesh->animationTime = 0.0f;
    mesh->animationLoop = loop;
    kotor::refreshEntityMesh(*mesh);
    ++animationRevision_;
    return true;
}

void GameSession::stopAnimation(std::size_t entityIndex)
{
    if (kotor::EntityMesh* mesh = findEntityMesh(scene_, entityIndex)) {
        mesh->animation.clear();
        mesh->animationTime = 0.0f;
        kotor::refreshEntityMesh(*mesh);
        ++animationRevision_;
    }
}

std::string GameSession::animationName(std::size_t entityIndex) const
{
    const kotor::EntityMesh* mesh = findEntityMesh(scene_, entityIndex);
    return mesh == nullptr ? std::string{} : mesh->animation;
}

float GameSession::animationTime(std::size_t entityIndex) const
{
    const kotor::EntityMesh* mesh = findEntityMesh(scene_, entityIndex);
    return mesh == nullptr ? 0.0f : mesh->animationTime;
}

void GameSession::updateAnimations(float dt)
{
    if (!animationEnabled_) {
        return;
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model) {
            continue;
        }
        if (mesh.fx) {
            mesh.animationTime = std::fmod(mesh.animationTime + dt, 10.0f);
            kotor::refreshEntityMesh(mesh);
            ++animationRevision_;
            continue;
        }
        const bool dangly = mesh.model->danglyNodeCount() > 0;
        if (mesh.animation.empty() && !dangly && mesh.extraRotations.empty()) {
            continue;
        }
        const float duration = mesh.model->animationDuration(mesh.animation);
        if (!mesh.animation.empty() && duration > 0.0f) {
            mesh.animationTime += dt;
            if (mesh.animationLoop) {
                mesh.animationTime = std::fmod(mesh.animationTime, duration);
            } else {
                mesh.animationTime = std::min(mesh.animationTime, duration);
            }
        }
        if (dangly) {
            mesh.danglyTime += dt;
        }
        if (mesh.blendRemaining > 0.0f && !mesh.previousAnimation.empty()) {
            const float previousDuration = mesh.model->animationDuration(mesh.previousAnimation);
            if (previousDuration > 0.0f) {
                mesh.previousAnimationTime += dt;
                if (mesh.animationLoop) {
                    mesh.previousAnimationTime = std::fmod(mesh.previousAnimationTime,
                                                           previousDuration);
                } else {
                    mesh.previousAnimationTime = std::min(mesh.previousAnimationTime,
                                                          previousDuration);
                }
            }
        }
        kotor::refreshEntityMesh(mesh);
        if (mesh.blendRemaining > 0.0f) {
            mesh.blendRemaining = std::max(0.0f, mesh.blendRemaining - dt);
            if (mesh.blendRemaining <= 0.0f) {
                mesh.previousAnimation.clear();
            }
        }
        ++animationRevision_;
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.attachModel) {
            continue;
        }
        const kotor::EntityMesh* parent = nullptr;
        for (const kotor::EntityMesh& candidate : scene_.entityMeshes) {
            if (candidate.entity == mesh.entity && !candidate.attachModel) {
                parent = &candidate;
                break;
            }
        }
        if (parent != nullptr) {
            mesh.attachAnimation = parent->animation;
            mesh.attachTime = parent->animationTime;
        }
        kotor::refreshEntityMesh(mesh);
        ++animationRevision_;
    }
}

void GameSession::initAnimations()
{
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model) {
            continue;
        }
        for (const char* name : {"pause1", "pause", "cpause1", "default"}) {
            if (mesh.model->animationDuration(name) > 0.0f) {
                mesh.animation = name;
                mesh.animationTime = 0.0f;
                mesh.animationLoop = true;
                break;
            }
        }
    }
    ++animationRevision_;
}

GameSession::GameSession(std::string installPath, std::string module, int language,
                        bool traceActions)
    : installPath_(std::move(installPath))
    , rsvRoot_(std::filesystem::path(installPath_) / "modules")
    , install_(installPath_)
    , rules_(install_)
{
    traceActions_ = traceActions;
    sim_ = std::make_unique<WorldSim>();
    localization_ = std::make_unique<kotor::Localization>(
        std::filesystem::path(installPath_));
    std::string localizationError;
    if (!localization_->load(install_, language, localizationError)) {
        if (language >= 0) {
            throw kotor::FormatError(localizationError);
        }
        logEvent("localization: " + localizationError);
    } else {
        tlk_ = localization_->tlk();
        languageId_ = localization_->languageId();
    }
    if (rules_.loaded()) {
        character_ = rules_.createCharacter(0, 1);
    } else {
        character_.str = 14;
        character_.dex = 12;
        character_.con = 12;
        character_.intel = 10;
        character_.wis = 10;
        character_.cha = 10;
        character_.maxHp = 30;
        character_.hp = 30;
    }
    playerMaxHp_ = character_.maxHp;
    playerHp_ = character_.hp;
    gold_ = character_.gold;
    audio_ = std::make_unique<AudioSystem>(install_);
    audio_->setTlk(tlk_ ? &*tlk_ : nullptr);
    std::string facialError;
    if (!facialRig_.load(install_, facialError)) {
        logEvent("facial rig: " + facialError);
    }
    std::string factionError;
    if (!factions_.load(install_, factionError)) {
        logEvent("factions: " + factionError);
    }
    if (!loadModule(module, nullptr, {})) {
        throw kotor::FormatError("cannot load module " + module);
    }
    runtime_ = std::make_unique<script::ActionRuntime>(&scriptWorld_, this);
    runtime_->setSelfAware(true);
    runtime_->setContinuations(true);
    runtime_->setTraceEvents(traceActions_);
    initScriptWorld();
    syncPlayerToScriptWorld();
}

GameSession::~GameSession() = default;

const std::vector<script::WorldEvent>& GameSession::actionTrace() const
{
    static const std::vector<script::WorldEvent> empty;
    return runtime_ ? runtime_->traceHistory() : empty;
}

bool GameSession::probeAction(std::uint16_t actionId)
{
    if (runtime_ == nullptr) {
        return false;
    }
    script::Vm vm({}, runtime_.get(), 1000);
    runtime_->call(actionId, {}, vm);
    runtime_->markLastTracePhase("probe");
    return true;
}

void GameSession::clearTraces()
{
    if (runtime_) {
        runtime_->clearTrace();
    }
    dialogueChoices_.clear();
    transitions_.clear();
}

void GameSession::placeAtLink(const std::string& linkedTo)
{
    const std::string wanted = lower(linkedTo);
    // A door is entered from its front; anything else (LinkedToFlags 2, or a
    // StartNewModule waypoint) places the player exactly on the object.
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind == kotor::EntityKind::Door && lower(entity.tag) == wanted) {
            position_ = entity.position;
            position_.x += std::cos(entity.bearing) * 0.9f;
            position_.y += std::sin(entity.bearing) * 0.9f;
            yaw_ = entity.bearing + kPi;
            return;
        }
    }
    for (const kotor::Entity& entity : scene_.entities) {
        if (lower(entity.tag) == wanted) {
            position_ = entity.position;
            yaw_ = entity.bearing;
            return;
        }
    }
    logWarn(concat("module link target ", linkedTo, " not found in ", module_));
}

bool GameSession::loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor)
{
    const std::string previousModule = module_;
    // Leave hooks and the snapshot run first: loading the next module rebinds
    // the install, after which the old module's scripts can no longer be found.
    if (runtime_ != nullptr && !sim_->restoringSave && !previousModule.empty()) {
        leaveModule();
    }
    kotor::Scene loaded = kotor::loadModule(install_, module);
    module_ = module;
    scene_ = std::move(loaded);
    if (nav_) {
        nav_->mesh.reset();
        nav_->actors.clear();
    }
    moveOutcome_ = MoveOutcome::None;
    moveBlockingDoor_ = static_cast<std::size_t>(-1);
    // Script resrefs resolve per module, so nothing cached survives the change.
    ncsCache_.clear();
    missingScripts_.clear();
    sim_->decoded.clear();
    playerEntity_ = static_cast<std::size_t>(-1);
    if (traceActions_ && !previousModule.empty() && previousModule != module_) {
        transitions_.push_back({previousModule, module_});
    }
    refreshHostility();
    if (spawn != nullptr) {
        position_ = *spawn;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    } else {
        position_ = scene_.entry;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    }
    if (!linkedDoor.empty()) {
        placeAtLink(linkedDoor);
    }
    float ground = 0.0f;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    activeDialogue_.reset();
    dialogueView_ = {};
    sim_->dialoguePaused = false;
    ++dialogueGeneration_;
    combatTarget_ = UINT32_MAX;
    triggerInside_.assign(scene_.triggers.size(), false);
    if (playerHp_ <= 0) {
        playerHp_ = playerMaxHp_;
        playerDead_ = false;
    }
    install_.addLips();
    if (audio_) {
        std::string audioError;
        audio_->loadScene(scene_, audioError);
        if (!audioError.empty()) {
            scene_.warnings.push_back(audioError);
        }
        audio_->setListener(position_, yaw_);
    }
    if (runtime_) {
        initScriptWorld();
        syncPlayerToScriptWorld();
    }
    if (thirdPerson_) {
        spawnPlayerAvatar();
    }
    for (const CompanionState& companion : companions_) {
        ensureCompanionEntity(companion);
    }
    placePartyAtEntry();
    // Arriving on top of a transition trigger must not bounce straight back: it
    // fires again only after the player has walked out of it.
    for (std::size_t i = 0; i < scene_.triggers.size() && i < triggerInside_.size(); ++i) {
        const std::size_t entity = scene_.triggers[i].entity;
        if (entity < scene_.entities.size() && !scene_.entities[entity].linkedModule.empty()
            && pointInTrigger(scene_.triggers[i], position_.x, position_.y)) {
            triggerInside_[i] = true;
        }
    }
    initAnimations();
    return true;
}

float GameSession::floorHeightAt(float x, float y) const
{
    float ground = 0.0f;
    if (floorHeight(scene_, x, y, position_.z + 2.0f, ground)) {
        return ground;
    }
    return position_.z;
}

std::size_t GameSession::findEntity(const std::string& tag) const
{
    const std::string wanted = lower(tag);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (lower(scene_.entities[i].tag) == wanted) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

std::uint32_t GameSession::random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

std::uint32_t GameSession::objectForEntity(std::size_t index) const
{
    if (index >= entityObjects_.size()) {
        return script::kObjectInvalid;
    }
    return entityObjects_[index];
}

std::uint32_t GameSession::objectForTag(const std::string& tag) const
{
    return scriptWorld_.byTag(tag, 0);
}

bool GameSession::runHookForTest(const std::string& hook, const std::string& tag,
                                 std::uint32_t entering)
{
    const std::size_t index = findEntity(tag);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    const auto it = scene_.entities[index].scripts.find(hook);
    if (it == scene_.entities[index].scripts.end() || it->second.empty()) {
        return false;
    }
    runHook(hook, index, entering == 0 ? scriptWorld_.pc : entering);
    return true;
}

bool GameSession::scriptValueTruthy(const std::optional<script::Value>& value) const
{
    return value.has_value() && value->truthy();
}

std::string GameSession::scriptSource(const std::string& resref)
{
    if (resref.empty()) {
        return {};
    }
    if (const auto injected = sim_->injectedScripts.find(resref);
        injected != sim_->injectedScripts.end()) {
        return std::string(injected->second.begin(), injected->second.end());
    }
    const auto cached = ncsCache_.find(resref);
    if (cached != ncsCache_.end()) {
        return std::string(cached->second.begin(), cached->second.end());
    }
    auto data = install_.find(resref, 2010);
    if (!data.has_value()) {
        if (missingScripts_.insert(resref).second) {
            scriptLog_.push_back("missing script " + resref);
        }
        return {};
    }
    ncsCache_.emplace(resref, *data);
    return std::string(data->begin(), data->end());
}

void GameSession::addScriptForTest(const std::string& resref, std::vector<std::uint8_t> bytes)
{
    sim_->injectedScripts[resref] = std::move(bytes);
    sim_->decoded.erase(resref);
}

std::shared_ptr<const std::vector<script::Instruction>> GameSession::decodedScript(
    const std::string& resref)
{
    if (const auto found = sim_->decoded.find(resref); found != sim_->decoded.end()) {
        return found->second;
    }
    const std::string bytes = scriptSource(resref);
    if (bytes.empty()) {
        return nullptr;
    }
    try {
        auto decoded = std::make_shared<const std::vector<script::Instruction>>(script::decodeNcs(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        sim_->decoded[resref] = decoded;
        return decoded;
    } catch (const script::NcsError& error) {
        scriptLog_.push_back("decode " + resref + ": " + error.what());
        return nullptr;
    }
}

bool GameSession::runScriptNcsForTest(const std::string& label,
                                      const std::vector<std::uint8_t>& bytes,
                                      std::uint32_t self)
{
    if (runtime_ == nullptr || bytes.empty()) {
        return false;
    }
    std::vector<script::Instruction> instructions;
    try {
        instructions = script::decodeNcs(bytes.data(), bytes.size());
    } catch (const script::NcsError& error) {
        scriptLog_.push_back("decode " + label + ": " + error.what());
        return false;
    }
    const std::uint32_t actualSelf = self == 0 ? scriptWorld_.pc : self;
    const ScriptContext saved = runtime_->contextSnapshot();
    runtime_->setContextObject("self", actualSelf);
    runtime_->setContextObject("entering", scriptWorld_.pc);
    runtime_->setContextObject("exiting", scriptWorld_.pc);
    runtime_->setContextObject("caller", scriptWorld_.pc);
    runtime_->setContextObject("trigger", scriptWorld_.pc);
    runtime_->setContextObject("speaker", actualSelf);
    script::Vm vm(std::move(instructions), runtime_.get(), 200000);
    vm.setScriptName(label);
    vm.setLiveObjectConstants(actualSelf);
    const script::VmResult result = vm.run();
    runtime_->setContext(saved);
    scriptLog_.push_back("script " + label + " status=" + script::statusName(result.status)
                         + " steps=" + std::to_string(result.steps));
    return result.status == script::VmStatus::Returned
        || result.status == script::VmStatus::Blocked;
}

bool GameSession::runFunctor(const script::Functor& functor, std::uint32_t target)
{
    if (runtime_ == nullptr) {
        return false;
    }
    const ScriptContext saved = runtime_->contextSnapshot();
    ScriptContext context = functor.context;
    if (target != 0 && target != script::kObjectInvalid) {
        context["self"] = target;
        context["speaker"] = target;
    }
    const auto selfEntry = context.find("self");
    const std::uint32_t self = selfEntry != context.end() ? selfEntry->second : scriptWorld_.pc;
    runtime_->setContext(context);
    ++sim_->scriptDepth;
    script::Vm vm(functor.instructions, runtime_.get(), 200000);
    vm.setEntry(functor.ip, functor.stack, functor.bp);
    vm.setScriptName(functor.script);
    vm.setLiveObjectConstants(self);
    const script::VmResult result = vm.run();
    --sim_->scriptDepth;
    runtime_->setContext(saved);
    scriptLog_.push_back("functor " + functor.source + " status="
                         + script::statusName(result.status)
                         + " steps=" + std::to_string(result.steps));
    syncScriptWorld();
    return result.status == script::VmStatus::Returned;
}

void GameSession::updateTimers(float dt)
{
    tickEffects(character_.effects, dt);
    character_.forceSuppressedTimer = std::max(0.0f, character_.forceSuppressedTimer - dt);
    for (kotor::Entity& entity : scene_.entities) {
        entity.stunTimer = std::max(0.0f, entity.stunTimer - dt);
        entity.fearTimer = std::max(0.0f, entity.fearTimer - dt);
        entity.forceSuppressedTimer = std::max(0.0f, entity.forceSuppressedTimer - dt);
        tickEffects(entity.effects, dt);
    }
    const std::vector<script::Timer> due = scriptWorld_.advance(dt);
    for (const script::Timer& timer : due) {
        if (timer.functor < 0) {
            continue;
        }
        const std::shared_ptr<script::Functor> functor = scriptWorld_.functor(timer.functor);
        if (functor != nullptr) {
            // A closure whose owner has been destroyed never runs.
            std::uint32_t owner = timer.target;
            if (owner == 0 || owner == script::kObjectInvalid) {
                const auto self = functor->context.find("self");
                owner = self != functor->context.end() ? self->second : scriptWorld_.pc;
            }
            const script::GameObject* object = scriptWorld_.get(owner);
            if (object != nullptr && !object->attrs.contains("destroyed")) {
                runFunctor(*functor, timer.target);
            }
        }
        scriptWorld_.functors.erase(timer.functor);
    }
    // The world-level queue only ever held closures this tick can run. Action*
    // now queue per object (ScriptActions.cpp); an Action* no family implements
    // yet still lands here through ActionRuntime::generic and is discarded, where
    // it used to accumulate for the life of the module.
    std::size_t executed = 0;
    while (!scriptWorld_.actionQueue.empty() && executed < 32) {
        script::QueuedAction action = std::move(scriptWorld_.actionQueue.front());
        scriptWorld_.actionQueue.pop_front();
        if (action.functor < 0) {
            continue;
        }
        const std::shared_ptr<script::Functor> functor = scriptWorld_.functor(action.functor);
        if (functor != nullptr) {
            runFunctor(*functor, action.target);
            ++executed;
        }
        scriptWorld_.functors.erase(action.functor);
    }
}

std::optional<script::Value> GameSession::runScript(const std::string& resref,
                                                    std::uint32_t self,
                                                    std::uint32_t entering,
                                                    std::uint32_t caller,
                                                    std::uint32_t trigger)
{
    ScriptContext context;
    context["entering"] = entering;
    context["exiting"] = entering;
    context["caller"] = caller;
    context["trigger"] = trigger;
    return runScriptWith(resref, self, context);
}

std::optional<script::Value> GameSession::runScriptWith(const std::string& resref,
                                                        std::uint32_t self,
                                                        const ScriptContext& context)
{
    constexpr int kMaxScriptDepth = 32;
    if (runtime_ == nullptr || resref.empty()) {
        return std::nullopt;
    }
    if (sim_->scriptDepth >= kMaxScriptDepth) {
        scriptLog_.push_back("script " + resref + " refused: nested too deep");
        return std::nullopt;
    }
    const auto instructions = decodedScript(resref);
    if (instructions == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t actualSelf = self == 0 ? scriptWorld_.pc : self;
    const ScriptContext saved = runtime_->contextSnapshot();
    // GetLast* are fields of the object in retail, so what an earlier event left
    // on this object stays readable; this event's own values go on top.
    ActorState& actor = sim_->actors[actualSelf];
    ScriptContext merged = actor.last;
    for (const auto& [name, value] : context) {
        merged[name] = value;
        if (name.rfind("last", 0) == 0) {
            actor.last[name] = value;
        }
    }
    merged["self"] = actualSelf;
    merged["speaker"] = actualSelf;
    runtime_->setContext(merged);
    ++sim_->scriptDepth;
    script::Vm vm(*instructions, runtime_.get(), 200000);
    vm.setScriptName(resref);
    vm.setLiveObjectConstants(actualSelf);
    const script::VmResult result = vm.run();
    --sim_->scriptDepth;
    runtime_->setContext(saved);
    std::ostringstream line;
    line << "script " << resref << " status=" << script::statusName(result.status)
         << " steps=" << result.steps << " actions=";
    std::size_t count = 0;
    for (const auto& [id, hits] : result.actions) {
        if (count++ != 0) {
            line << ",";
        }
        line << id << ":" << hits;
    }
    if (result.top.has_value()) {
        line << " top=" << result.top->asInt();
    }
    if (!result.error.empty()) {
        line << " error=" << result.error;
    }
    scriptLog_.push_back(line.str());
    if (scriptLog_.size() > 8192) {
        scriptLog_.erase(scriptLog_.begin(), scriptLog_.begin() + 4096);
    }
    syncScriptWorld();
    if (result.top.has_value()) {
        return result.top;
    }
    return std::nullopt;
}

void GameSession::syncPlayerToScriptWorld()
{
    script::GameObject* pc = scriptWorld_.get(scriptWorld_.pc);
    if (pc == nullptr) {
        return;
    }
    pc->position = {position_.x, position_.y, position_.z};
    pc->hp = playerHp_;
    pc->attrs["level"] = static_cast<std::int64_t>(character_.level);
    pc->attrs["xp"] = static_cast<std::int64_t>(character_.xp);
    pc->attrs["gold"] = static_cast<std::int64_t>(gold_);
    pc->attrs["class_id"] = static_cast<std::int64_t>(character_.classId);
    pc->attrs["ac"] = static_cast<std::int64_t>(rules_.armorClass(character_));
    pc->attrs["good_evil"] = static_cast<std::int64_t>(character_.alignment);
    pc->attrs["force_points"] = static_cast<std::int64_t>(character_.fp);
    pc->attrs["max_force_points"] = static_cast<std::int64_t>(character_.maxFp);
    pc->attrs["faction"] = static_cast<std::int64_t>(0);
}

void GameSession::applyCharacter(const CharacterSheet& sheet)
{
    character_ = sheet;
    playerMaxHp_ = std::max(1, sheet.maxHp);
    playerHp_ = std::clamp(sheet.hp, 0, playerMaxHp_);
    playerDead_ = false;
    gold_ = sheet.gold;
    if (!sheet.name.empty()) {
        playerName_ = sheet.name;
    }
    syncPlayerToScriptWorld();
    if (thirdPerson_) {
        spawnPlayerAvatar();
    }
}

void GameSession::setPlayerAppearance(std::uint32_t appearance, const std::string& name)
{
    playerAppearance_ = appearance;
    if (!name.empty()) {
        playerName_ = name;
    }
    if (!thirdPerson_) {
        return;
    }
    for (auto it = scene_.entityMeshes.begin(); it != scene_.entityMeshes.end();) {
        if (it->entity == playerEntity_ && playerEntity_ != static_cast<std::size_t>(-1)) {
            it = scene_.entityMeshes.erase(it);
        } else {
            ++it;
        }
    }
    playerEntity_ = static_cast<std::size_t>(-1);
    spawnPlayerAvatar();
}

void GameSession::spawnPlayerAvatar()
{
    if (!thirdPerson_) {
        return;
    }
    if (playerEntity_ != static_cast<std::size_t>(-1)
        && playerEntity_ < scene_.entities.size()
        && scene_.entities[playerEntity_].tag == "PC") {
        kotor::Entity& entity = scene_.entities[playerEntity_];
        entity.position = position_;
        entity.bearing = playerBearing_;
        entity.dead = false;
        return;
    }
    playerEntity_ = kotor::spawnPlayerAvatar(scene_, install_, playerAppearance_,
                                             playerName_, position_, playerBearing_);
    if (playerEntity_ != static_cast<std::size_t>(-1)) {
        syncScriptWorld();
        initAnimations();
    }
}

void GameSession::updatePlayerAvatar(float dt, bool moving)
{
    (void)dt;
    if (!thirdPerson_ || playerEntity_ == static_cast<std::size_t>(-1)
        || playerEntity_ >= scene_.entities.size()) {
        return;
    }
    kotor::Entity& entity = scene_.entities[playerEntity_];
    entity.position = position_;
    entity.bearing = playerBearing_;
    const kotor::Vec3 target{position_.x, position_.y, position_.z + 1.3f};
    const kotor::Vec3 back{std::cos(yaw_), std::sin(yaw_), 0.0f};
    float distance = 3.2f;
    kotor::Vec3 eye{target.x - back.x * distance, target.y - back.y * distance,
                    position_.z + 1.55f};
    const bool noClip = cameraNoClip_;
    while (!noClip && distance > 0.65f
           && wallBlocked(scene_, {target.x, target.y, eye.z}, eye, 0.22f)) {
        distance -= 0.2f;
        eye.x = target.x - back.x * distance;
        eye.y = target.y - back.y * distance;
    }
    const bool visible = noClip || distance >= 1.6f;
    if (visible != playerVisible_) {
        playerVisible_ = visible;
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == playerEntity_) {
                mesh.visible = visible;
            }
        }
        ++animationRevision_;
    }
    const char* wanted = moving ? "run" : "pause1";
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity == playerEntity_ && mesh.model
            && mesh.model->animationDuration(wanted) > 0.0f
            && mesh.animation != wanted) {
            playAnimation(playerEntity_, wanted, true);
            break;
        }
    }
}

bool GameSession::entityHasFacialPose(std::size_t entityIndex) const
{
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity == entityIndex && !mesh.extraRotations.empty()) {
            return true;
        }
    }
    return false;
}

void GameSession::updateFacialMeshes()
{    std::size_t speaker = static_cast<std::size_t>(-1);
    if (dialogueView_.active && activeDialogue_.has_value()) {
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            if (objectForEntity(i) == activeDialogue_->npcObject) {
                speaker = i;
                break;
            }
        }
    }
    const auto rotations = speaker != static_cast<std::size_t>(-1)
        ? facialRig_.rotations(facialRig_.viseme())
        : std::map<std::string, kotor::Mat3>{};
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity == speaker) {
            mesh.extraRotations = rotations;
            ++animationRevision_;
        } else if (!mesh.extraRotations.empty()) {
            mesh.extraRotations.clear();
            ++animationRevision_;
        }
    }
}

void GameSession::refreshHostility()
{
    if (!factions_.loaded()) {
        return;
    }
    for (kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Creature) {
            continue;
        }
        entity.hostile = entity.hostileOverride
            || factions_.hostile(0, static_cast<int>(entity.faction));
    }
}

int GameSession::factionOfObject(std::uint32_t object) const
{
    const script::GameObject* entry = scriptWorld_.get(object);
    if (entry == nullptr) {
        return 0;
    }
    const auto it = entry->attrs.find("faction");
    if (it != entry->attrs.end() && std::holds_alternative<std::int64_t>(it->second)) {
        return static_cast<int>(std::get<std::int64_t>(it->second));
    }
    return 0;
}

void GameSession::syncEntityToScriptWorld(std::size_t index)
{
    if (index >= scene_.entities.size() || index >= entityObjects_.size()) {
        return;
    }
    script::GameObject* object = scriptWorld_.get(entityObjects_[index]);
    if (object == nullptr) {
        return;
    }
    const kotor::Entity& entity = scene_.entities[index];
    object->position = {entity.position.x, entity.position.y, entity.position.z};
    object->hp = entity.hitPoints;
    object->valid = !entity.dead;
    object->tag = entity.tag.empty() ? object->tag : entity.tag;
    if (!object->attrs.contains("object_type")) {
        object->attrs["object_type"] = objectTypeForKind(entity.kind);
    }
}

void GameSession::syncScriptWorld()
{
    syncPlayerToScriptWorld();
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        syncEntityToScriptWorld(i);
    }
}

void GameSession::initMerchants()
{
    merchants_.clear();
    activeMerchant_ = -1;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Store) {
            continue;
        }
        MerchantState merchant;
        merchant.entity = i;
        merchant.resref = entity.templateResref;
        merchant.name = entity.name.empty() ? entity.tag : entity.name;
        merchant.markUp = entity.storeMarkUp;
        merchant.markDown = entity.storeMarkDown;
        merchant.buySellFlag = entity.storeBuySell;
        for (const auto& [resref, infinite] : entity.storeItems) {
            MerchantStockItem stock;
            stock.item.resref = resref;
            stock.infinite = infinite;
            stock.remaining = infinite ? -1 : 1;
            auto cached = itemTemplates_.find(resref);
            if (cached == itemTemplates_.end()) {
                if (auto item = kotor::loadItemTemplate(install_, resref); item.has_value()) {
                    cached = itemTemplates_.emplace(resref, *item).first;
                }
            }
            if (cached != itemTemplates_.end()) {
                stock.item.baseItem = cached->second.baseItem;
                stock.item.cost = cached->second.cost;
                stock.item.stack = cached->second.stackSize;
                stock.item.identified = cached->second.identified;
            }
            if (stock.item.cost <= 0 && rules_.loaded()) {
                stock.item.cost = rules_.itemCost(stock.item.baseItem);
            }
            merchant.stock.push_back(std::move(stock));
        }
        // UTM carries no gold and the K1 action table has no store-gold call, so a
        // store has no purse: the old 'max(1000, stock/2)' pool was invented.
        merchants_.push_back(std::move(merchant));
    }
}

bool GameSession::openStore(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()
        || scene_.entities[entityIndex].kind != kotor::EntityKind::Store) {
        return false;
    }
    for (std::size_t i = 0; i < merchants_.size(); ++i) {
        if (merchants_[i].entity == entityIndex) {
            activeMerchant_ = static_cast<int>(i);
            storeBonusMarkUp_ = 0;
            storeBonusMarkDown_ = 0;
            runHook("OnOpenStore", entityIndex, scriptWorld_.pc);
            logEvent("store " + merchants_[i].resref + " opened");
            return true;
        }
    }
    return false;
}

bool GameSession::openStoreByTag(const std::string& tag)
{
    const std::size_t index = findEntity(tag);
    return index != static_cast<std::size_t>(-1) && openStore(index);
}

// Store arithmetic: the item's cost (scaled by ChangeItemCost for its base type)
// times the mark percentage, at least 1.
std::int32_t GameSession::scaledPrice(std::int32_t baseItem, std::int32_t cost,
                                      std::int32_t percent) const
{
    std::int64_t scaled = cost;
    if (const auto it = itemCostScale_.find(baseItem); it != itemCostScale_.end()) {
        scaled = static_cast<std::int64_t>(static_cast<float>(cost) * it->second);
    }
    return static_cast<std::int32_t>(
        std::max<std::int64_t>(1, scaled * std::max<std::int32_t>(0, percent) / 100));
}

std::int32_t GameSession::buyPrice(std::size_t stockIndex) const
{
    if (activeMerchant_ < 0 || static_cast<std::size_t>(activeMerchant_) >= merchants_.size()) {
        return 0;
    }
    const MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if (stockIndex >= merchant.stock.size()) {
        return 0;
    }
    return scaledPrice(merchant.stock[stockIndex].item.baseItem, merchant.stock[stockIndex].item.cost,
                       merchant.markUp + storeBonusMarkUp_);
}

std::int32_t GameSession::sellPrice(std::size_t inventoryIndex) const
{
    if (activeMerchant_ < 0 || static_cast<std::size_t>(activeMerchant_) >= merchants_.size()
        || inventoryIndex >= character_.inventory.size()) {
        return 0;
    }
    const MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    return scaledPrice(character_.inventory[inventoryIndex].baseItem,
                       character_.inventory[inventoryIndex].cost,
                       merchant.markDown + storeBonusMarkDown_);
}

bool GameSession::buyMerchantItem(std::size_t stockIndex)
{
    if (activeMerchant_ < 0) {
        return false;
    }
    MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if (stockIndex >= merchant.stock.size() || (merchant.buySellFlag & 1) == 0) {
        return false;
    }
    MerchantStockItem& stock = merchant.stock[stockIndex];
    if (!stock.infinite && stock.remaining <= 0) {
        return false;
    }
    const std::int32_t price = buyPrice(stockIndex);
    if (price <= 0 || gold_ < price) {
        return false;
    }
    if (!addInventoryItem(stock.item.resref, stock.item.baseItem, stock.item.stack,
                          stock.item.cost)) {
        return false;
    }
    gold_ -= price;
    if (!stock.infinite) {
        --stock.remaining;
    }
    logEvent("buy " + stock.item.resref + " for " + std::to_string(price));
    return true;
}

bool GameSession::sellInventoryItem(std::size_t inventoryIndex)
{
    if (activeMerchant_ < 0 || inventoryIndex >= character_.inventory.size()) {
        return false;
    }
    MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if ((merchant.buySellFlag & 2) == 0) {
        return false;
    }
    InventoryItem item = character_.inventory[inventoryIndex];
    // A plot item is never sold (Plot flag semantics; not traced in the store panel).
    if (const script::GameObject* held = scriptWorld_.get(item.object);
        held != nullptr && held->valid && held->attrs.contains("item_plot")
        && std::holds_alternative<std::int64_t>(held->attrs.at("item_plot"))
        && std::get<std::int64_t>(held->attrs.at("item_plot")) != 0) {
        return false;
    }
    const std::int32_t price = sellPrice(inventoryIndex);
    if (!removeInventoryItem(item.resref, item.stack)) {
        return false;
    }
    gold_ += price;
    logEvent("sell " + item.resref + " for " + std::to_string(price));
    return true;
}

bool GameSession::identifyInventoryItem(std::size_t inventoryIndex)
{
    if (inventoryIndex >= character_.inventory.size()) {
        return false;
    }
    InventoryItem& item = character_.inventory[inventoryIndex];
    if (item.identified) {
        return false;
    }
    // Nothing in the K1 action table or the store data prices identification: it
    // is free here (the old half-cost charge was invented).
    item.identified = true;
    logEvent("identify " + item.resref);
    return true;
}

void GameSession::takeAllLoot()
{
    while (!lootItems_.empty()) {
        if (!takeLootItem(0)) {
            break;
        }
    }
}

// Encounter state is set up at module load; creatures appear when the player
// enters a volume (updateEncounterVolumes in ScriptWorldState.cpp).
void GameSession::spawnEncounters()
{
    encounterStates_.assign(scene_.encounters.size(), EncounterState{});
    for (std::size_t encounterIndex = 0; encounterIndex < scene_.encounters.size();
         ++encounterIndex) {
        const kotor::EncounterVolume& volume = scene_.encounters[encounterIndex];
        EncounterState& state = encounterStates_[encounterIndex];
        state.respawnsRemaining = volume.templateData.respawns < 0
            ? -1
            : volume.templateData.respawns;
        state.resetTime = static_cast<float>(std::max(0, volume.templateData.resetTime));
    }
}

std::int32_t GameSession::spawnEncounterCreatures(std::size_t encounterIndex)
{
    return spawnEncounterWave(encounterIndex);
}

void GameSession::updateEncounters(float dt)
{
    updateEncounterVolumes(dt);
}

void GameSession::updateParty(float dt)
{
    if (companions_.empty() || playerDead_) {
        return;
    }
    kotor::Vec3 anchor = position_;
    float anchorYaw = yaw_;
    if (leaderTag_ != "PC") {
        const std::size_t leader = findEntity(leaderTag_);
        if (leader != static_cast<std::size_t>(-1)) {
            anchor = scene_.entities[leader].position;
            anchorYaw = scene_.entities[leader].bearing;
        }
    }
    int slot = 0;
    for (CompanionState& companion : companions_) {
        const std::size_t index = findEntity(companion.tag);
        if (index == static_cast<std::size_t>(-1) || scene_.entities[index].dead) {
            continue;
        }
        if (companion.tag == leaderTag_) {
            continue;
        }
        // A companion fights through its queue and the shipped AI; following is for
        // when it has nothing to do.
        {
            const ActorState& actor = ScriptApi{*this}.actor(objectForEntity(index));
            if (!actor.queue.empty() || actor.combat.inCombat) {
                ++slot;
                continue;
            }
        }
        followLeader(index, anchor, formationSlot(anchor, anchorYaw, slot, 1.6f), dt);
        ++slot;
    }
}

void GameSession::initScriptWorld()
{
    for (kotor::Entity& entity : scene_.entities) {
        rules_.deriveCreatureCombat(entity);
    }
    scriptWorld_.clearObjects();
    scriptWorld_.attrs["module"] = module_;
    scriptWorld_.attrs["difficulty"] = static_cast<std::int64_t>(1);
    spawnRan_.clear();
    sim_->heartbeatDue.clear();
    sim_->events.clear();
    sim_->baseEntityCount = scene_.entities.size();
    restoreCarried();
    const auto found = sim_->snapshots.find(module_);
    const bool returning = found != sim_->snapshots.end();
    if (returning) {
        applyModuleSnapshot(found->second);
    } else {
        entityObjects_.assign(scene_.entities.size(), 0);
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& entity = scene_.entities[i];
            std::string tag = entity.tag;
            if (tag.empty()) {
                tag = "entity_" + std::to_string(i);
            }
            script::GameObject& object = scriptWorld_.addObject(tag, 100, 0, !entity.dead);
            object.position = {entity.position.x, entity.position.y, entity.position.z};
            object.hp = entity.hitPoints;
            object.attrs["ac"] = static_cast<std::int64_t>(
                entity.defense > 0 ? entity.defense : 10);
            object.attrs["fortitude"] = static_cast<std::int64_t>(0);
            object.attrs["will"] = static_cast<std::int64_t>(0);
            object.attrs["reflex"] = static_cast<std::int64_t>(0);
            object.attrs["challenge_rating"] = static_cast<std::int64_t>(
                std::max(1, entity.maxHitPoints / 4));
            object.attrs["movement_rate"] = static_cast<std::int64_t>(1);
            object.attrs["good_evil"] = static_cast<std::int64_t>(50);
            object.attrs["level"] = static_cast<std::int64_t>(1);
            object.attrs["class_id"] = static_cast<std::int64_t>(0);
            object.attrs["xp"] = static_cast<std::int64_t>(0);
            object.attrs["faction"] = static_cast<std::int64_t>(entity.faction);
            entityObjects_[i] = object.id;
        }
        // The area and module are objects scripts can hold and signal. They sit
        // outside the iterated area (area -1) and get ids like anything else.
        script::GameObject& area = scriptWorld_.addObject(module_, -1, 0, true);
        sim_->areaObject = area.id;
        script::GameObject& moduleObject = scriptWorld_.addObject("MODULE", -1, 0, true);
        sim_->moduleObject = moduleObject.id;
    }
    // A party member native to this module uses the object it already carries.
    for (std::size_t i = 0; i < entityObjects_.size() && i < scene_.entities.size(); ++i) {
        const std::uint32_t carried = scriptWorld_.byTag(scene_.entities[i].tag, 0);
        if (carried != entityObjects_[i] && sim_->carried.contains(carried)
            && !scene_.entities[i].tag.empty()) {
            std::erase(scriptWorld_.tags[scene_.entities[i].tag], entityObjects_[i]);
            scriptWorld_.objects.erase(entityObjects_[i]);
            entityObjects_[i] = carried;
        }
    }
    sim_->carried.clear();
    syncPartyViews();
    syncScriptWorld();
    if (returning) {
        refreshHostility();
        if (!sim_->restoringSave) {
            runReturnScripts();
        }
        return;
    }
    initMerchants();
    runSpawnAndEnterScripts();
    spawnEncounters();
}

ScriptContext GameSession::hookContext(const std::string& hook, std::uint32_t entering) const
{
    const std::uint32_t pc = scriptWorld_.pc;
    const std::uint32_t source = entering == 0 ? pc : entering;
    ScriptContext context;
    context["entering"] = source;
    context["exiting"] = source;
    context["caller"] = pc;
    context["trigger"] = pc;
    if (hook == "OnUsed") {
        context["lastUsedBy"] = source;
    } else if (hook == "OnOpen") {
        context["lastOpenedBy"] = source;
    } else if (hook == "OnClosed") {
        context["lastClosedBy"] = source;
    } else if (hook == "OnClick") {
        context["clicking"] = source;
    } else if (hook == "ScriptDialogue") {
        context["lastSpeaker"] = source;
    } else if (hook == "ScriptDeath" || hook == "OnDeath") {
        context["lastKiller"] = source;
    }
    return context;
}

void GameSession::runHook(const std::string& hook, std::size_t entityIndex,
                          std::uint32_t entering, const ScriptContext* extra)
{
    if (entityIndex >= scene_.entities.size()) {
        return;
    }
    const kotor::Entity& entity = scene_.entities[entityIndex];
    const auto it = entity.scripts.find(hook);
    if (it == entity.scripts.end() || it->second.empty()) {
        return;
    }
    scriptLog_.push_back("hook " + hook + " " + entity.tag + " -> " + it->second);
    ScriptContext context = hookContext(hook, entering);
    if (extra != nullptr) {
        for (const auto& [name, value] : *extra) {
            context[name] = value;
        }
    }
    const std::string script = it->second;
    runScriptWith(script, objectForEntity(entityIndex), context);
}

bool GameSession::runSceneHook(const std::string& hook, std::uint32_t entering,
                               const ScriptContext* extra)
{
    const auto it = scene_.scripts.find(hook);
    if (it == scene_.scripts.end() || it->second.empty()) {
        return false;
    }
    // Module hooks run on the module object and area hooks on the area object.
    const std::uint32_t self = hook.rfind("Mod_", 0) == 0 ? sim_->moduleObject : sim_->areaObject;
    ScriptContext context = hookContext(hook, entering);
    if (extra != nullptr) {
        for (const auto& [name, value] : *extra) {
            context[name] = value;
        }
    }
    const std::string script = it->second;
    scriptLog_.push_back("hook " + hook + " -> " + script);
    runScriptWith(script, self, context);
    return true;
}

void GameSession::runSpawnAndEnterScripts()
{
    runSceneHook("Mod_OnModLoad", scriptWorld_.pc);
    runSceneHook("Mod_OnModStart", scriptWorld_.pc);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        const auto spawn = entity.scripts.find("ScriptSpawn");
        if (spawn != entity.scripts.end() && !spawn->second.empty()) {
            if (spawnRan_.insert(objectForEntity(i)).second) {
                runScript(spawn->second, objectForEntity(i), scriptWorld_.pc,
                          scriptWorld_.pc, scriptWorld_.pc);
            }
        }
    }
    runSceneHook("Mod_OnClientEntr", scriptWorld_.pc);
    runSceneHook("OnEnter", scriptWorld_.pc);
}

// Coming back to a module: objects are already spawned and started, so only the
// load and entry notifications fire.
void GameSession::runReturnScripts()
{
    runSceneHook("Mod_OnModLoad", scriptWorld_.pc);
    runSceneHook("Mod_OnClientEntr", scriptWorld_.pc);
    runSceneHook("OnEnter", scriptWorld_.pc);
}

void GameSession::flushScriptEvents()
{
    tickEvents(ScriptApi{*this});
}

void GameSession::updateScripts(float dt)
{
    heartbeatTimer_ += dt;
    if (heartbeatTimer_ >= 6.0) {
        heartbeatTimer_ = 0.0;
        runSceneHook("Mod_OnHeartbeat", scriptWorld_.pc);
        runSceneHook("OnHeartbeat", scriptWorld_.pc);
    }
    ScriptApi api{*this};
    tickHeartbeats(api, dt);
    tickPerception(api, dt);
}

void GameSession::logEvent(std::string line)
{
    combatLog_.push_back(concat("[t=", elapsed_, "] ", line));
    if (combatLog_.size() > 32) {
        combatLog_.erase(combatLog_.begin());
    }
}

std::uint8_t GameSession::lipViseme() const
{
    return activeLip_.has_value() ? activeLip_->visemeAt(lipTime_) : 0;
}

bool GameSession::playMovie(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    if (movieAudioVoice_ != 0 && audio_) {
        audio_->mixer().stopVoice(movieAudioVoice_);
    }
    movieAudioVoice_ = 0;
    movieAudioData_.reset();
    const std::filesystem::path path = std::filesystem::path(installPath_)
        / "movies" / (name + ".bik");
    auto player = std::make_unique<kotor::BinkPlayer>();
    std::string decodeError;
    if (!player->open(path.string(), decodeError)) {
        logEvent("movie " + name + ": " + decodeError);
        return false;
    }
    moviePlayer_ = std::move(player);
    movieName_ = name;
    movieTime_ = 0.0f;
    movieDuration_ = static_cast<float>(
        static_cast<double>(moviePlayer_->frameCount()) / moviePlayer_->framesPerSecond());
    moviePlaying_ = true;
    movieFrameIndex_ = -1;
    movieFrame_ = {};
    if (!moviePlayer_->decodeFrame(0, movieFrame_, decodeError)) {
        logEvent("movie " + name + ": frame 0: " + decodeError);
    } else {
        movieFrameIndex_ = 0;
    }
    if (audio_ && moviePlayer_->hasAudio()) {
        auto track = std::make_shared<audio::SoundData>();
        track->name = name + ".bikaudio";
        track->sampleRate = moviePlayer_->audio().sampleRate;
        track->channels = moviePlayer_->audio().channels;
        track->samples = moviePlayer_->audio().samples;
        if (audio::resample(*track, 48000, 2)) {
            movieAudioData_ = track;
            movieAudioVoice_ = audio_->mixer().play(
                track, audio::Bus::Movie, 1.0f, false);
            audio_->mixer().setVoiceCursor(movieAudioVoice_, 0.0);
        }
    }
    return true;
}

bool GameSession::seekMovie(float seconds)
{
    if (!moviePlayer_ || !moviePlaying_) {
        return false;
    }
    const double fps = moviePlayer_->framesPerSecond();
    const int lastFrame = std::max(0, moviePlayer_->frameCount() - 1);
    movieTime_ = std::clamp(seconds, 0.0f, movieDuration_);
    const int index = std::clamp(
        static_cast<int>(movieTime_ * static_cast<float>(fps)), 0, lastFrame);
    std::string decodeError;
    if (!moviePlayer_->decodeFrame(index, movieFrame_, decodeError)) {
        logEvent("movie seek frame " + std::to_string(index) + ": " + decodeError);
        return false;
    }
    movieFrameIndex_ = index;
    if (audio_ && movieAudioVoice_ != 0) {
        audio_->mixer().setVoiceCursor(movieAudioVoice_, movieTime_);
    }
    return true;
}

void GameSession::stopMovie()
{
    if (audio_ && movieAudioVoice_ != 0) {
        audio_->mixer().stopVoice(movieAudioVoice_);
    }
    movieAudioVoice_ = 0;
    movieAudioData_.reset();
    moviePlaying_ = false;
    moviePlayer_.reset();
    movieFrame_ = {};
    movieFrameIndex_ = -1;
    movieTime_ = 0.0f;
    movieDuration_ = 0.0f;
    movieName_.clear();
}

bool GameSession::queueMovie(const std::string& name, bool playNow)
{
    if (name.empty()) {
        return false;
    }
    movieQueue_.push_back(name);
    return playNow ? playMovieQueue() : true;
}

bool GameSession::playMovieQueue()
{
    if (movieQueue_.empty()) {
        return false;
    }
    const std::string name = movieQueue_.front();
    movieQueue_.erase(movieQueue_.begin());
    return playMovie(name);
}

bool GameSession::startPazaak(std::int32_t opponentDeck, std::string endScript,
                              std::int32_t maxWager, bool tutorial,
                              std::uint32_t opponent)
{
    std::vector<int> playerSide{1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    std::vector<int> opponentSide{1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    if (auto data = install_.find("pazaakdecks", kotor::resType(kotor::ResType::TwoDa));
        data.has_value()) {
        try {
            const kotor::TwoDa decks = kotor::TwoDa::parse(std::move(*data), "pazaakdecks");
            const auto rowCards = [&](const std::string& row) {
                std::vector<int> cards;
                const int rowIndex = decks.rowIndex(row);
                if (rowIndex < 0) {
                    return cards;
                }
                for (std::size_t column = 0; column < decks.columnCount(); ++column) {
                    const std::string& name = decks.column(column);
                    if (name.rfind("card", 0) != 0) {
                        continue;
                    }
                    const std::string value = decks.cell(
                        static_cast<std::size_t>(rowIndex), column);
                    // '*' double cards are excluded until their exact
                    // last-card semantics are verified; the shipped rows that
                    // carry them are marked ERASEME.
                    if (value.empty() || value.front() == '*') {
                        continue;
                    }
                    const int card = std::atoi(value.c_str());
                    if (card >= -6 && card <= 6 && card != 0) {
                        cards.push_back(card);
                    }
                }
                return cards;
            };
            if (std::vector<int> cards = rowCards("0"); !cards.empty()) {
                playerSide = std::move(cards);
            }
            if (std::vector<int> cards = rowCards(std::to_string(std::max(0, opponentDeck)));
                !cards.empty()) {
                opponentSide = std::move(cards);
            }
        } catch (const kotor::FormatError&) {
        }
    }
    const std::uint32_t seed = 0x9E3779B9u
        ^ (static_cast<std::uint32_t>(opponentDeck) * 2654435761u) ^ opponent;
    pazaak_ = std::make_unique<PazaakGame>();
    pazaak_->reset(seed, playerSide, opponentSide);
    pazaakEndScript_ = std::move(endScript);
    pazaakOpponent_ = opponent;
    pazaakWager_ = tutorial ? 0 : std::max(0, maxWager);
    lastPazaakResult_ = -1;
    return true;
}

void GameSession::resolvePazaakIfOver()
{
    if (!pazaak_ || pazaak_->phase() != PazaakGame::Phase::MatchOver) {
        return;
    }
    const bool won = pazaak_->playerSets() > pazaak_->opponentSets();
    lastPazaakResult_ = won ? 1 : 0;
    if (pazaakWager_ > 0) {
        gold_ = std::max(0, gold_ + (won ? pazaakWager_ : -pazaakWager_));
        character_.gold = gold_;
    }
    const std::string endScript = pazaakEndScript_;
    const std::uint32_t opponent = pazaakOpponent_;
    pazaak_.reset();
    pazaakEndScript_.clear();
    pazaakWager_ = 0;
    if (!endScript.empty()) {
        runScript(endScript, opponent, scriptWorld_.pc, scriptWorld_.pc, scriptWorld_.pc);
    }
}

void GameSession::startSwoop()
{
    swoop_.reset();
    swoopActive_ = true;
    turretActive_ = false;
    logEvent("minigame swoop start");
}

void GameSession::startTurret()
{
    turret_.reset();
    turretActive_ = true;
    swoopActive_ = false;
    logEvent("minigame turret start");
}

void GameSession::updateSwoop(float dt, float throttle, bool boost, bool brake)
{
    if (!swoopActive_) {
        return;
    }
    swoop_.update(dt, throttle, boost, brake);
    if (swoop_.state().finished) {
        spaceLastEvent_ = "finish";
    }
}

void GameSession::updateTurret(float dt)
{
    if (!turretActive_) {
        return;
    }
    turret_.update(dt);
    if (turret_.state().hitPoints <= 0) {
        spaceLastEvent_ = "death";
    }
}

void GameSession::update(float dt, const InputState& input)
{
    if (dt <= 0.0f) {
        return;
    }
    dt = std::min(dt, 0.08f);
    elapsed_ += dt;
    if (activeLip_.has_value() && dialogueView_.active) {
        lipTime_ = std::min(lipTime_ + dt, activeLip_->duration);
        facialRig_.setViseme(lipViseme());
    } else {
        facialRig_.setViseme(0);
    }
    updateFacialMeshes();
    if (moviePlaying_ && moviePlayer_) {
        movieTime_ += dt;
        const double fps = moviePlayer_->framesPerSecond();
        const int lastFrame = std::max(0, moviePlayer_->frameCount() - 1);
        const int index = std::clamp(
            static_cast<int>(movieTime_ * static_cast<float>(fps)), 0, lastFrame);
        if (index != movieFrameIndex_) {
            kotor::BinkVideoFrame frame;
            std::string decodeError;
            if (moviePlayer_->decodeFrame(index, frame, decodeError)) {
                movieFrame_ = std::move(frame);
                movieFrameIndex_ = index;
            } else {
                movieFrameIndex_ = index;
                logEvent("movie frame " + std::to_string(index) + ": " + decodeError);
            }
        }
        if (audio_ && movieAudioVoice_ != 0) {
            audio_->mixer().setVoiceCursor(movieAudioVoice_, movieTime_);
        }
        if (movieTime_ >= movieDuration_) {
            moviePlaying_ = false;
            movieTime_ = movieDuration_;
            if (audio_ && movieAudioVoice_ != 0) {
                audio_->mixer().stopVoice(movieAudioVoice_);
            }
            movieAudioVoice_ = 0;
            movieAudioData_.reset();
            if (!movieQueue_.empty()) {
                playMovieQueue();
            }
        }
    }
    updateAnimations(dt);
    if (!dialogueCameraClip_.empty()) {
        dialogueCameraTime_ = std::min(dialogueCameraTime_ + dt, dialogueCameraDuration_);
    }
    yaw_ += input.mouseDx * 0.0026f;
    pitch_ = clamp(pitch_ - input.mouseDy * 0.0022f, -1.35f, 1.35f);

    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    // A non-commandable player does not respond to input; a commandable one
    // drops whatever was queued as soon as they steer.
    ActorState& pcActor = sim_->actors[scriptWorld_.pc];
    if (!playerDead_ && !dialogueView_.active && pcActor.commandable && !controlLocked()) {
        if (input.forward) forwardAxis += 1.0f;
        if (input.back) forwardAxis -= 1.0f;
        if (input.right) rightAxis += 1.0f;
        if (input.left) rightAxis -= 1.0f;
        if ((forwardAxis != 0.0f || rightAxis != 0.0f) && !pcActor.queue.empty()) {
            ScriptApi{*this}.clearQueue(scriptWorld_.pc);
        }
    }
    const float speed = creatureMoveRate(scriptWorld_.pc, input.run);
    const float cosYaw = std::cos(yaw_);
    const float sinYaw = std::sin(yaw_);
    kotor::Vec3 desired{cosYaw * forwardAxis + sinYaw * rightAxis,
                        sinYaw * forwardAxis - cosYaw * rightAxis, 0.0f};
    const float magnitude = std::sqrt(desired.x * desired.x + desired.y * desired.y);
    if (magnitude > 1.0e-5f) {
        desired = desired * (1.0f / magnitude);
    }
    const kotor::Vec3 delta = desired * (speed * dt);

    const kotor::Vec3 start = position_;
    bool steered = false;
    const kotor::Vec3 candidate = magnitude > 1.0e-5f
        ? slidePlayer(start, {position_.x + delta.x, position_.y + delta.y, position_.z}, steered)
        : start;
    position_.x = candidate.x;
    position_.y = candidate.y;

    float ground = position_.z;
    const bool hasFloor = floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground);
    if (input.jump && grounded_) {
        verticalVelocity_ = 5.2f;
        grounded_ = false;
    }
    verticalVelocity_ -= kGravity * dt;
    position_.z += verticalVelocity_ * dt;
    if (hasFloor && position_.z <= ground) {
        position_.z = ground;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    } else if (hasFloor && position_.z - ground < kStepHeight && verticalVelocity_ <= 0.0f) {
        position_.z = ground;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    } else {
        grounded_ = false;
    }

    if (magnitude > 1.0e-5f) {
        playerBearing_ = std::atan2(desired.y, desired.x);
    }
    const bool queueWalked = nav_ && nav_->playerStepped;
    if (nav_) {
        nav_->playerStepped = false;
    }
    updatePlayerAvatar(dt, magnitude > 1.0e-5f || queueWalked);

    if (audio_) {
        if (magnitude > 1.0e-5f && grounded_) {
            footstepTimer_ += dt;
            if (footstepTimer_ >= 0.42f) {
                footstepTimer_ = 0.0f;
                const int surface = floorSurface(scene_, position_.x, position_.y,
                                                 position_.z + 2.0f);
                if (surface >= 0) {
                    audio_->playFootstep(0, surface, position_, 0.45f);
                }
            }
        } else {
            footstepTimer_ = 0.0f;
        }
    }
    syncPlayerToScriptWorld();
    if (quietWorldForTest_) {
        updateParty(dt);
        tickParty(dt);
        tickActors(ScriptApi{*this}, dt);
        syncPlayerToScriptWorld();
        return;
    }
    updateTriggers();
    updateCombat(dt);
    updateParty(dt);
    tickParty(dt);
    updateTimers(dt);
    tickEvents(ScriptApi{*this});
    tickActors(ScriptApi{*this}, dt);
    tickWorldState(ScriptApi{*this}, dt);
    updateScripts(dt);
    updateEncounters(dt);
    syncPlayerToScriptWorld();
    if (audio_) {
        audio_->setListener(position_, yaw_);
    }
    if (!pendingModule_.empty()) {
        const std::string next = pendingModule_;
        const std::string linkedTo = sim_->pendingLinkedTo;
        pendingModule_.clear();
        sim_->pendingLinkedTo.clear();
        try {
            loadModule(next, nullptr, linkedTo);
        } catch (const std::exception& exception) {
            logEvent("module transition failed: " + next + ": " + exception.what());
        }
        const std::vector<std::string> movies = std::move(sim_->pendingMovies);
        sim_->pendingMovies.clear();
        for (const std::string& movie : movies) {
            queueMovie(movie, false);
        }
        if (!movies.empty()) {
            playMovieQueue();
        }
    }
}

bool GameSession::lineOfSight(kotor::Vec3 from, kotor::Vec3 to) const
{
    from.z += 1.2f;
    to.z += 1.2f;
    return !wallBlocked(scene_, from, to, 0.15f);
}

kotor::Vec3 GameSession::actorPosition(std::uint32_t object)
{
    return ScriptApi{*this}.position(object);
}

std::size_t GameSession::actorQueueSize(std::uint32_t object) const
{
    const auto it = sim_->actors.find(object);
    return it != sim_->actors.end() ? it->second.queue.size() : 0;
}

const ModuleSnapshot* GameSession::moduleSnapshot(const std::string& module) const
{
    const auto it = sim_->snapshots.find(module);
    return it != sim_->snapshots.end() ? &it->second : nullptr;
}

SceneCamera GameSession::camera() const
{
    if (dialogueView_.active && dialogueCameraModel_ && !dialogueCameraClip_.empty()) {
        const float time = std::min(dialogueCameraTime_, dialogueCameraDuration_);
        const std::optional<kotor::Mdl::Pose> pose = dialogueCameraModel_->nodePose(
            "camerahook", dialogueCameraClip_, time);
        if (pose.has_value()) {
            SceneCamera camera;
            camera.eye = pose->translation;
            camera.forward = kotor::normalize(
                kotor::Vec3{pose->rotation.m[0][1], pose->rotation.m[1][1],
                            pose->rotation.m[2][1]});
            camera.up = kotor::normalize(
                kotor::Vec3{pose->rotation.m[0][2], pose->rotation.m[1][2],
                            pose->rotation.m[2][2]});
            camera.fovYDegrees = 60.0f;
            return camera;
        }
    }
    if (dialogueView_.active && dialogueView_.cameraAngle >= 1
        && dialogueView_.cameraAngle <= 3) {
        kotor::Vec3 speaker = position_;
        if (activeDialogue_.has_value()) {
            for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
                if (objectForEntity(i) == activeDialogue_->npcObject) {
                    speaker = scene_.entities[i].position;
                    break;
                }
            }
        }
        const SceneCamera preset = presetCamera(dialogueView_.cameraAngle,
                                                dialogueView_.camHeightOffset,
                                                dialogueView_.tarHeightOffset,
                                                speaker, position_);
        if (preset.fovYDegrees > 0.0f) {
            return preset;
        }
    }
    if (dialogueView_.active && dialogueView_.cameraId >= 0) {
        const SceneCamera camera = cinematicCamera(dialogueView_.cameraId);
        if (camera.fovYDegrees > 0.0f) {
            return camera;
        }
    }
    if (thirdPerson_ && playerVisible_) {
        const kotor::Vec3 forward{std::cos(pitch_) * std::cos(yaw_),
                                  std::cos(pitch_) * std::sin(yaw_),
                                  std::sin(pitch_)};
        const kotor::Vec3 target{position_.x, position_.y, position_.z + 1.3f};
        const kotor::Vec3 back{std::cos(yaw_), std::sin(yaw_), 0.0f};
        float distance = 3.2f;
        kotor::Vec3 eye{target.x - back.x * distance, target.y - back.y * distance,
                        std::clamp(position_.z + 1.55f - std::sin(pitch_) * 1.4f,
                                   position_.z + 0.55f, position_.z + 2.3f)};
        const bool noClip = cameraNoClip_;
        while (!noClip && distance > 0.65f
               && wallBlocked(scene_, {target.x, target.y, eye.z}, eye, 0.22f)) {
            distance -= 0.2f;
            eye.x = target.x - back.x * distance;
            eye.y = target.y - back.y * distance;
        }
        if (distance < 1.5f) {
            eye.x = target.x - back.x * 0.8f;
            eye.y = target.y - back.y * 0.8f;
            eye.z = position_.z + 2.25f;
        }
        float floor = eye.z;
        if (floorHeight(scene_, eye.x, eye.y, eye.z + 1.0f, floor)
            && eye.z < floor + 0.25f) {
            eye.z = floor + 0.25f;
        }
        SceneCamera camera;
        camera.eye = eye;
        camera.forward = kotor::normalize(kotor::Vec3{target.x - eye.x,
                                                      target.y - eye.y,
                                                      target.z - eye.z});
        camera.up = {0.0f, 0.0f, 1.0f};
        camera.fovYDegrees = 62.0f;
        return camera;
    }
    SceneCamera camera;
    camera.eye = {position_.x, position_.y, position_.z + kEyeHeight};
    camera.forward = {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_),
                      std::sin(pitch_)};
    camera.up = {0.0f, 0.0f, 1.0f};
    return camera;
}

bool GameSession::loadDialogueCameraModel(const std::string& resref)
{
    dialogueCameraModel_.reset();
    if (resref.empty()) {
        return false;
    }
    auto mdl = install_.find(resref, 2002);
    auto mdx = install_.find(resref, 3008);
    if (!mdl.has_value() || !mdx.has_value()) {
        return false;
    }
    try {
        dialogueCameraModel_ = std::make_shared<kotor::Mdl>(
            kotor::Mdl::parse(std::move(*mdl), std::move(*mdx), resref + ".mdl"));
        return true;
    } catch (const kotor::FormatError&) {
        dialogueCameraModel_.reset();
        return false;
    }
}

void GameSession::setDialogueCameraClip(std::int32_t animation)
{
    dialogueCameraClip_.clear();
    dialogueCameraTime_ = 0.0f;
    dialogueCameraDuration_ = 0.0f;
    if (animation < 1200 || !dialogueCameraModel_) {
        return;
    }
    char name[16]{};
    std::snprintf(name, sizeof(name), "CUT%03dW", animation - 1199);
    const float duration = dialogueCameraModel_->animationDuration(name);
    if (duration > 0.0f) {
        dialogueCameraClip_ = name;
        dialogueCameraDuration_ = duration;
    }
}

bool GameSession::setDialogueCameraAnimationForTest(const std::string& model,
                                                    const std::string& clip)
{
    if (!loadDialogueCameraModel(model)) {
        return false;
    }
    const float duration = dialogueCameraModel_->animationDuration(clip);
    if (duration <= 0.0f) {
        return false;
    }
    dialogueView_.active = true;
    dialogueCameraClip_ = clip;
    dialogueCameraTime_ = 0.0f;
    dialogueCameraDuration_ = duration;
    return true;
}

SceneCamera GameSession::presetCamera(std::int32_t angle, float camHeight, float targetHeight,
                                      kotor::Vec3 speaker, kotor::Vec3 target) const
{
    if (angle < 1 || angle > 3) {
        return {};
    }
    const kotor::Vec3 up{0.0f, 0.0f, 1.0f};
    const kotor::Vec3 delta = target - speaker;
    const float range = kotor::length(delta);
    const kotor::Vec3 direction = range > 0.0f ? delta * (1.0f / range) : kotor::Vec3{0.0f, 1.0f, 0.0f};
    const float yaw = (angle == 2 ? -30.0f : angle == 3 ? 90.0f : 30.0f) * kPi / 180.0f;
    const kotor::Vec3 rotated = rotateZ(direction, yaw);
    kotor::Vec3 targetPoint{};
    kotor::Vec3 eye{};
    if (angle == 1) {
        targetPoint = speaker - direction * 0.2f
            + up * (targetHeight - 0.04f - 0.2f * range);
        eye = targetPoint - rotated * (0.5f + range)
            + up * (camHeight + 0.2f * range);
    } else if (angle == 2) {
        const kotor::Vec3 a = speaker - up * (0.1f * range);
        const kotor::Vec3 b = target - up * (0.05f * range);
        const kotor::Vec3 d = a - b;
        targetPoint = b + d * 0.3f + up * targetHeight;
        eye = targetPoint - rotated * 0.8f - kotor::normalize(rotated) * 0.15f
            + up * camHeight;
    } else {
        const kotor::Vec3 d = speaker - target;
        targetPoint = target + d * 0.5f + up * (targetHeight - 0.2f * range);
        eye = targetPoint - rotated * 1.5f + up * (camHeight + 0.3f * range);
    }
    SceneCamera camera;
    camera.eye = eye;
    camera.forward = kotor::normalize(targetPoint - eye);
    camera.up = up;
    camera.fovYDegrees = 60.0f;
    return camera;
}

SceneCamera GameSession::cinematicCamera(std::int32_t cameraId) const
{
    for (const kotor::CameraDef& definition : scene_.cameras) {
        if (static_cast<std::int32_t>(definition.id) != cameraId) {
            continue;
        }
        SceneCamera camera;
        camera.eye = {definition.position.x, definition.position.y,
                      definition.position.z + definition.height};
        const float pitch = definition.pitch * kPi / 180.0f;
        camera.forward = rotateQuaternion(definition.qw, definition.qx, definition.qy,
                                          definition.qz,
                                          {0.0f, std::sin(pitch), -std::cos(pitch)});
        const kotor::Vec3 right = rotateQuaternion(definition.qw, definition.qx, definition.qy,
                                                   definition.qz, {1.0f, 0.0f, 0.0f});
        camera.up = kotor::normalize(kotor::cross(right, camera.forward));
        camera.fovYDegrees = definition.fov > 0.0f ? definition.fov : 60.0f;
        return camera;
    }
    SceneCamera missing;
    missing.fovYDegrees = 0.0f;
    return missing;
}

const kotor::Entity* GameSession::nearestDoor(float maxDistance) const
{
    const kotor::Entity* best = nullptr;
    float bestDistance = maxDistance;
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Door) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &entity;
        }
    }
    return best;
}

const kotor::Entity* GameSession::nearestConversation(float maxDistance) const
{
    const kotor::Entity* best = nullptr;
    float bestDistance = maxDistance;
    for (const kotor::Entity& entity : scene_.entities) {
        if ((entity.kind != kotor::EntityKind::Creature && entity.kind != kotor::EntityKind::Placeable)
            || entity.conversation.empty() || entity.dead) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &entity;
        }
    }
    return best;
}

std::string GameSession::interactionPrompt() const
{
    if (dialogueView_.active) {
        return "Dialogue: 1-9 choose, Esc end";
    }
    if (const kotor::Entity* talk = nearestConversation(3.0f); talk != nullptr) {
        return "E: talk to " + (talk->name.empty() ? talk->tag : talk->name);
    }
    const kotor::Entity* door = nearestDoor(2.8f);
    if (door != nullptr) {
        if (door->linkedModule.empty()) {
            return "E: " + door->tag + " (no transition)";
        }
        return "E: " + door->tag + " -> " + door->linkedModule;
    }
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
            continue;
        }
        if (distance2D(position_, entity.position) < 2.5f) {
            return "E: use " + (entity.name.empty() ? entity.tag : entity.name);
        }
    }
    return {};
}

bool GameSession::interact()
{
    if (dialogueView_.active) {
        chooseDialogueOption(0);
        return true;
    }
    if (controlLocked()) {
        return false;
    }
    if (const kotor::Entity* talk = nearestConversation(3.0f); talk != nullptr) {
        const std::size_t index = static_cast<std::size_t>(talk - scene_.entities.data());
        // Retail routes a click through the creature's OnDialogue handler, which
        // decides whether to BeginConversation. Only a creature with no handler
        // goes straight to its conversation.
        const auto hook = talk->scripts.find("ScriptDialogue");
        if (hook != talk->scripts.end() && !hook->second.empty()) {
            const ScriptContext context{{"lastSpeaker", scriptWorld_.pc},
                                        {"listenPattern", static_cast<std::uint32_t>(-1)}};
            runHook("ScriptDialogue", index, scriptWorld_.pc, &context);
            return true;
        }
        return startDialogue(index);
    }
    if (controlLocked()) {
        return false;
    }
    const kotor::Entity* door = nearestDoor(2.8f);
    if (door != nullptr) {
        // Retail rules (ScriptWorldState.cpp): locked doors refuse, transitions
        // only cross an unlocked door.
        const std::size_t index = static_cast<std::size_t>(door - scene_.entities.data());
        const UseOutcome outcome = useObject(index);
        if (outcome != UseOutcome::OutOfRange && outcome != UseOutcome::Invalid
            && outcome != UseOutcome::Refused) {
            return true;
        }
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Store) {
            continue;
        }
        if (distance2D(position_, entity.position) < 3.0f && openStore(i)) {
            setUiPanel(UiPanel::Merchant);
            return true;
        }
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
            continue;
        }
        if (distance2D(position_, entity.position) < 2.5f) {
            return usePlaceable(i);
        }
    }
    return false;
}

bool GameSession::usePlaceable(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()
        || scene_.entities[entityIndex].kind != kotor::EntityKind::Placeable) {
        return false;
    }
    const UseOutcome outcome = useObject(entityIndex);
    return outcome == UseOutcome::Used || outcome == UseOutcome::Looted;
}

// Length of the straight walkmesh walk from the player to an entity, stopping
// within `stop` of it. Each step is the same wall/floor test the player and
// scripted actors move with, so a target behind a wall or off the walkmesh is
// unreachable (negative), unlike a bare distance check.
float GameSession::pathDistanceTo(std::size_t entityIndex) const
{
    if (entityIndex >= scene_.entities.size()) {
        return -1.0f;
    }
    const kotor::Vec3 target = scene_.entities[entityIndex].position;
    kotor::Vec3 at = position_;
    float travelled = 0.0f;
    for (int step = 0; step < 600; ++step) {
        const float dx = target.x - at.x;
        const float dy = target.y - at.y;
        const float remaining = std::sqrt(dx * dx + dy * dy);
        if (remaining <= 0.5f) {
            return travelled;
        }
        const float stride = std::min(0.4f, remaining - 0.5f);
        const float ux = dx / remaining;
        const float uy = dy / remaining;
        const kotor::Vec3 candidates[3] = {
            {at.x + ux * stride, at.y + uy * stride, at.z},
            {at.x + ux * stride, at.y, at.z},
            {at.x, at.y + uy * stride, at.z},
        };
        bool moved = false;
        for (kotor::Vec3 candidate : candidates) {
            float ground = candidate.z;
            if (wallBlocked(scene_, at, candidate, kPlayerRadius)
                || !floorHeight(scene_, candidate.x, candidate.y, candidate.z + 2.0f, ground)) {
                continue;
            }
            candidate.z = ground;
            const float length = std::hypot(candidate.x - at.x, candidate.y - at.y);
            if (length > 1.0e-4f) {
                travelled += length;
                at = candidate;
                moved = true;
                break;
            }
        }
        if (!moved) {
            return -1.0f;
        }
    }
    return -1.0f;
}

bool GameSession::inReach(std::size_t entityIndex, float range) const
{
    if (entityIndex >= scene_.entities.size()) {
        return false;
    }
    if (distance2D(position_, scene_.entities[entityIndex].position) > range) {
        return false;
    }
    const float path = pathDistanceTo(entityIndex);
    return path >= 0.0f && path <= range + 0.5f;
}

bool GameSession::testTransition(std::string& from, std::string& to)
{
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Door || entity.linkedModule.empty()) {
            continue;
        }
        if (!classicBugs_) {
            const float dx = entity.position.x - position_.x;
            const float dy = entity.position.y - position_.y;
            const float dz = entity.position.z - position_.z;
            if (dx * dx + dy * dy + dz * dz > 4.0f) {
                continue;
            }
        }
        const std::string targetModule = entity.linkedModule;
        const std::string targetDoor = entity.linkedTo;
        from = module_;
        position_ = entity.position;
        yaw_ = entity.bearing;
        const bool ok = loadModule(targetModule, nullptr, targetDoor);
        to = module_;
        return ok;
    }
    return false;
}

void GameSession::faceTowards(kotor::Vec3 target)
{
    yaw_ = std::atan2(target.y - position_.y, target.x - position_.x);
    pitch_ = 0.0f;
}

bool GameSession::findClearView(kotor::Vec3 target, float radius, kotor::Vec3& out) const
{
    constexpr int kAngles = 16;
    const float radii[3] = {radius, radius + 1.5f, radius + 3.0f};
    for (const float r : radii) {
        for (int i = 0; i < kAngles; ++i) {
            const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kAngles);
            kotor::Vec3 candidate{target.x + std::cos(angle) * r,
                                  target.y + std::sin(angle) * r, target.z};
            float ground = candidate.z;
            if (!floorHeight(scene_, candidate.x, candidate.y, candidate.z + 2.0f, ground)) {
                continue;
            }
            candidate.z = ground;
            const kotor::Vec3 from{candidate.x, candidate.y, ground + 1.2f};
            const kotor::Vec3 to{target.x, target.y, target.z + 1.0f};
            if (!wallBlocked(scene_, from, to, 0.15f)) {
                out = candidate;
                return true;
            }
        }
    }
    return false;
}

bool GameSession::walkTo(kotor::Vec3 target, float maxSeconds)
{
    int steps = std::max(1, static_cast<int>(maxSeconds * 60.0f));
    // Each closed door on the way costs one stop, as it does a player.
    for (int leg = 0; leg < 8; ++leg) {
        if (distance2D(position_, target) < 0.6f) {
            return true;
        }
        if (!moveTo(target, 0.6f, distance2D(position_, target) > 4.0f)) {
            return false;
        }
        while (steps-- > 0 && movePending()) {
            if (dialogueView_.active) {
                return false;
            }
            update(1.0f / 60.0f, InputState{});
        }
        if (moveOutcome_ != MoveOutcome::DoorBlocked
            || !openDoorAsPlayer(moveBlockingDoor_)) {
            break;
        }
    }
    return distance2D(position_, target) < 1.2f;
}

bool GameSession::loadDialogue(const std::string& resref, kotor::Dlg& dlg)
{
    if (resref.empty()) {
        return false;
    }
    auto data = install_.find(resref, 2029);
    if (!data.has_value()) {
        return false;
    }
    try {
        kotor::Gff gff = kotor::Gff::parse(std::move(*data), resref + ".dlg");
        dlg = kotor::Dlg::parse(gff, tlk_ ? &*tlk_ : nullptr, resref);
        return true;
    } catch (const kotor::FormatError&) {
        return false;
    }
}

bool GameSession::startDialogue(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()) {
        return false;
    }
    return startConversation(objectForEntity(entityIndex), {});
}

bool GameSession::startConversation(std::uint32_t owner, const std::string& resrefOverride)
{
    sim_->noStartLink = false;
    ScriptApi api{*this};
    const std::size_t entityIndex = api.entityFor(owner);
    std::string resref = resrefOverride;
    std::string ownerTag;
    std::string ownerName;
    if (entityIndex != static_cast<std::size_t>(-1)) {
        const kotor::Entity& entity = scene_.entities[entityIndex];
        ownerTag = entity.tag;
        ownerName = entity.name.empty() ? entity.tag : entity.name;
        if (resref.empty()) {
            resref = entity.conversation;
        }
    } else if (const script::GameObject* object = scriptWorld_.get(owner); object != nullptr) {
        ownerTag = object->tag;
        ownerName = object->tag;
    }
    if (resref.empty()) {
        logEvent((ownerName.empty() ? std::string("object") : ownerName) + " has nothing to say");
        return false;
    }
    kotor::Dlg dlg;
    if (!loadDialogue(resref, dlg)) {
        logEvent("missing dialogue " + resref);
        return false;
    }
    if (activeDialogue_.has_value()) {
        finishDialogue(true);
    }
    ++dialogueGeneration_;
    ActiveDialogue active;
    active.npcTag = ownerTag;
    active.speaker = ownerName;
    active.resref = resref;
    active.npcObject = owner;
    active.dlg = std::move(dlg);
    activeDialogue_ = std::move(active);
    sim_->dialoguePaused = false;
    if (audio_ && entityIndex != static_cast<std::size_t>(-1)) {
        audio_->playCreatureSound(static_cast<std::uint32_t>(entityIndex), 6); // select
    }
    loadDialogueCameraModel(activeDialogue_->dlg.cameraModel);
    dialogueCameraClip_.clear();
    dialogueCameraTime_ = 0.0f;
    dialogueCameraDuration_ = 0.0f;
    const std::uint32_t generation = dialogueGeneration_;
    const std::vector<std::uint32_t> starting = activeDialogue_->dlg.starting;
    const std::vector<std::string> startingActive = activeDialogue_->dlg.startingActive;
    const std::size_t entryCount = activeDialogue_->dlg.entries.size();
    std::uint32_t entry = UINT32_MAX;
    // StartingList.Active: first truthy link wins, an empty Active is
    // unconditional, and no passing link means no conversation at all.
    for (std::size_t i = 0; i < starting.size(); ++i) {
        const std::uint32_t candidate = starting[i];
        if (candidate >= entryCount) {
            continue;
        }
        const std::string activeScript =
            i < startingActive.size() ? startingActive[i] : std::string{};
        bool condition = true;
        if (!activeScript.empty()) {
            condition = scriptValueTruthy(runScriptWith(activeScript, owner, dialogueContext()));
            scriptLog_.push_back("dialogue condition " + activeScript + " entry="
                                 + std::to_string(candidate) + " = "
                                 + (condition ? "1" : "0"));
            if (!activeDialogue_.has_value() || dialogueGeneration_ != generation) {
                return false;
            }
        }
        if (!condition) {
            continue;
        }
        entry = candidate;
        break;
    }
    if (entry == UINT32_MAX) {
        sim_->noStartLink = true;
        clearDialogueState();
        return false;
    }
    scriptLog_.push_back("dialogue start " + activeDialogue_->resref + " entry="
                         + std::to_string(entry));
    logEvent("dialogue started with " + activeDialogue_->speaker);
    showDialogueEntry(entry);
    return true;
}

bool GameSession::startDialogue(const std::string& tag)
{
    const std::size_t index = findEntity(tag);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    return startDialogue(index);
}

void GameSession::setDialogueForTest(kotor::Dlg dlg, std::string resref,
                                     std::string speaker)
{
    ActiveDialogue active;
    active.npcTag = "test";
    active.speaker = std::move(speaker);
    active.resref = std::move(resref);
    active.npcObject = 0;
    active.dlg = std::move(dlg);
    activeDialogue_ = std::move(active);
    dialogueView_ = {};
    sim_->dialoguePaused = false;
    ++dialogueGeneration_;
    showDialogueEntry(0);
}

ScriptContext GameSession::dialogueContext() const
{
    const std::uint32_t pc = scriptWorld_.pc;
    return {{"entering", pc}, {"exiting", pc}, {"caller", pc}, {"trigger", pc},
            {"lastSpeaker", pc}};
}

// A link condition lives on the link (RepliesList/EntriesList .Active). Shipped
// nodes carry no node-level Active; a hand-built one has to pass as well.
bool GameSession::dialogueLinkPasses(const std::string& linkActive, const std::string& nodeActive)
{
    const std::uint32_t owner = activeDialogue_->npcObject;
    if (!linkActive.empty()
        && !scriptValueTruthy(runScriptWith(linkActive, owner, dialogueContext()))) {
        return false;
    }
    return nodeActive.empty()
        || scriptValueTruthy(runScriptWith(nodeActive, owner, dialogueContext()));
}

std::vector<std::uint32_t> GameSession::passingReplies(const kotor::DlgEntry& entry)
{
    std::vector<std::uint32_t> passing;
    const std::uint32_t generation = dialogueGeneration_;
    for (std::size_t i = 0; i < entry.replies.size(); ++i) {
        const std::uint32_t reply = entry.replies[i];
        if (!activeDialogue_.has_value() || dialogueGeneration_ != generation
            || reply >= activeDialogue_->dlg.replies.size()) {
            continue;
        }
        const std::string link = i < entry.repliesActive.size() ? entry.repliesActive[i]
                                                                : std::string{};
        const std::string node = activeDialogue_->dlg.replies[reply].active;
        if (dialogueLinkPasses(link, node)) {
            passing.push_back(reply);
        }
    }
    return passing;
}

std::uint32_t GameSession::firstPassingEntry(const kotor::DlgReply& reply)
{
    const std::uint32_t generation = dialogueGeneration_;
    for (std::size_t i = 0; i < reply.entries.size(); ++i) {
        const std::uint32_t entry = reply.entries[i];
        if (!activeDialogue_.has_value() || dialogueGeneration_ != generation) {
            return UINT32_MAX;
        }
        if (entry >= activeDialogue_->dlg.entries.size()) {
            continue;
        }
        const std::string link = i < reply.entriesActive.size() ? reply.entriesActive[i]
                                                                : std::string{};
        const std::string node = activeDialogue_->dlg.entries[entry].active;
        if (dialogueLinkPasses(link, node)) {
            return entry;
        }
    }
    return UINT32_MAX;
}

// One node reached: its script every time (retail keeps no "already fired" set),
// then the journal, then plot XP. Node scripts run on the conversation owner.
// Whether retail switches OBJECT_SELF to the Speaker object for a node script
// was not established; the owner matches what shipped k_* node scripts expect.
// Returns false when the script ended or replaced the conversation.
template <typename Node>
bool GameSession::applyDialogueNode(const Node& node, std::uint32_t generation)
{
    if (!node.script.empty()) {
        runScriptWith(node.script, activeDialogue_->npcObject, dialogueContext());
        logEvent("dialog script " + node.script);
        if (!activeDialogue_.has_value() || dialogueGeneration_ != generation) {
            return false;
        }
    }
    if (!node.quest.empty()) {
        logEvent("dialog quest " + node.quest);
        // A node advances the journal under AddJournalQuestEntry's rule (005483f0).
        if (node.hasQuestEntry) {
            addJournalQuest(node.quest, node.questEntry, false);
        }
    }
    if (node.plotIndex >= 0) {
        giveXp(plotXpForIndex(node.plotIndex, node.plotXpPercentage));
    }
    return true;
}

void GameSession::showDialogueEntry(std::uint32_t entryIndex)
{
    if (!activeDialogue_.has_value() || entryIndex >= activeDialogue_->dlg.entries.size()) {
        finishDialogue(false);
        return;
    }
    const std::uint32_t generation = dialogueGeneration_;
    const auto alive = [&]() {
        return activeDialogue_.has_value() && dialogueGeneration_ == generation;
    };
    dialogueView_.active = true;
    dialogueView_.npcTag = activeDialogue_->npcTag;
    dialogueView_.speaker = activeDialogue_->speaker;
    std::vector<std::uint32_t> replies;
    // Control entries carry scripts but no spoken line, and a single textless
    // link. They run and pass straight through (the shipped conversation starts
    // on such an entry).
    for (int guard = 0; guard < 32; ++guard) {
        // Copies: a node script can replace the conversation these live in.
        const kotor::DlgEntry entry = activeDialogue_->dlg.entries[entryIndex];
        if (!applyDialogueNode(entry, generation)) {
            return;
        }
        replies = passingReplies(entry);
        if (!alive()) {
            return;
        }
        if (!entry.text.empty()) {
            break;
        }
        if (replies.empty()) {
            finishDialogue(false);
            return;
        }
        if (replies.size() != 1) {
            break;
        }
        const kotor::DlgReply reply = activeDialogue_->dlg.replies[replies.front()];
        if (!reply.text.empty()) {
            break;
        }
        if (!applyDialogueNode(reply, generation)) {
            return;
        }
        const std::uint32_t next = firstPassingEntry(reply);
        if (!alive()) {
            return;
        }
        if (next == UINT32_MAX) {
            finishDialogue(false);
            return;
        }
        entryIndex = next;
    }
    const kotor::DlgEntry& entry = activeDialogue_->dlg.entries[entryIndex];
    dialogueView_.active = true;
    dialogueView_.npcTag = activeDialogue_->npcTag;
    dialogueView_.speaker = activeDialogue_->speaker;
    dialogueView_.speakerObject = activeDialogue_->npcObject;
    dialogueView_.listenerObject = scriptWorld_.pc;
    dialogueView_.delay = entry.delay;
    dialogueView_.waitFlags = entry.waitFlags;
    // Speaker and Listener name objects by tag; empty means owner and PC.
    if (!entry.speaker.empty()) {
        const std::size_t speaker = findEntity(entry.speaker);
        if (speaker != static_cast<std::size_t>(-1)) {
            dialogueView_.speakerObject = objectForEntity(speaker);
            const kotor::Entity& entity = scene_.entities[speaker];
            dialogueView_.speaker = entity.name.empty() ? entity.tag : entity.name;
        }
    }
    if (!entry.listener.empty()) {
        const std::size_t listener = findEntity(entry.listener);
        if (listener != static_cast<std::size_t>(-1)) {
            dialogueView_.listenerObject = objectForEntity(listener);
        }
    }
    dialogueView_.text = entry.text.empty() ? "(No text)" : substituteTokens(entry.text);
    dialogueView_.entry = entryIndex;
    dialogueView_.cameraId = entry.cameraId;
    dialogueView_.cameraAngle = entry.cameraAngle;
    dialogueView_.camVidEffect = entry.camVidEffect;
    if (entry.camVidEffect > 0) {
        ++unsupportedCamVidEffects_;
        logWarn(concat("CamVidEffect ", entry.camVidEffect, " on ",
                       activeDialogue_->resref, " entry ", entryIndex,
                       " is not supported; effect ignored"));
    }
    dialogueView_.camHeightOffset = entry.camHeightOffset;
    dialogueView_.tarHeightOffset = entry.tarHeightOffset;
    setDialogueCameraClip(entry.cameraAnimation);
    if (audio_ && !entry.sound.empty()) {
        audio_->playVoice(entry.sound);
    }
    activeLip_.reset();
    lipTime_ = 0.0f;
    if (!entry.sound.empty()) {
        if (auto lipData = install_.find(entry.sound, kotor::resType(kotor::ResType::Lip));
            lipData.has_value()) {
            try {
                activeLip_ = kotor::parseLip(*lipData, entry.sound + ".lip");
            } catch (const kotor::FormatError&) {
                activeLip_.reset();
            }
        }
    }
    dialogueView_.options.clear();
    for (const std::uint32_t replyIndex : replies) {
        const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
        DialogueOption option;
        option.text = reply.text.empty() ? "(Continue)" : substituteTokens(reply.text);
        option.reply = replyIndex;
        dialogueView_.options.push_back(std::move(option));
    }
    if (dialogueView_.options.empty()) {
        dialogueView_.options.push_back({"(End conversation)", UINT32_MAX});
    }
    logEvent("dialog " + dialogueView_.speaker + ": " + dialogueView_.text);
}

void GameSession::chooseDialogueOption(std::size_t optionIndex)
{
    if (!activeDialogue_.has_value() || optionIndex >= dialogueView_.options.size()) {
        return;
    }
    if (sim_->dialoguePaused) {
        return;
    }
    if (traceActions_) {
        dialogueChoices_.push_back({module_, activeDialogue_->resref,
                                    dialogueView_.entry, optionIndex,
                                    dialogueView_.options[optionIndex].text});
    }
    const std::uint32_t replyIndex = dialogueView_.options[optionIndex].reply;
    if (replyIndex == UINT32_MAX || replyIndex >= activeDialogue_->dlg.replies.size()) {
        finishDialogue(false);
        return;
    }
    const kotor::DlgReply reply = activeDialogue_->dlg.replies[replyIndex];
    if (reply.cameraId >= 0) {
        dialogueView_.cameraId = reply.cameraId;
    }
    if (reply.cameraAngle >= 0) {
        dialogueView_.cameraAngle = reply.cameraAngle;
    }
    dialogueView_.camVidEffect = reply.camVidEffect;
    if (reply.camVidEffect > 0) {
        ++unsupportedCamVidEffects_;
        logWarn(concat("CamVidEffect ", reply.camVidEffect, " on ",
                       activeDialogue_->resref, " reply ", replyIndex,
                       " is not supported; effect ignored"));
    }
    dialogueView_.camHeightOffset = reply.camHeightOffset;
    dialogueView_.tarHeightOffset = reply.tarHeightOffset;
    if (reply.cameraAnimation >= 0) {
        setDialogueCameraClip(reply.cameraAnimation);
    }
    if (audio_ && !reply.sound.empty()) {
        audio_->playVoice(reply.sound);
    }
    const std::uint32_t generation = dialogueGeneration_;
    if (!applyDialogueNode(reply, generation)) {
        return;
    }
    // After a reply the first linked entry whose Active passes wins; none ends it.
    const std::uint32_t next = firstPassingEntry(reply);
    if (!activeDialogue_.has_value() || dialogueGeneration_ != generation) {
        return;
    }
    if (next == UINT32_MAX) {
        finishDialogue(false);
    } else {
        showDialogueEntry(next);
    }
}

void GameSession::clearDialogueState()
{
    activeDialogue_.reset();
    dialogueView_ = {};
    sim_->dialoguePaused = false;
    ++dialogueGeneration_;
    dialogueCameraModel_.reset();
    dialogueCameraClip_.clear();
    dialogueCameraTime_ = 0.0f;
    dialogueCameraDuration_ = 0.0f;
    activeLip_.reset();
    lipTime_ = 0.0f;
}

// State is cleared before the end scripts run, since they commonly start the
// next conversation or a module transition.
void GameSession::finishDialogue(bool aborted)
{
    if (!activeDialogue_.has_value()) {
        clearDialogueState();
        return;
    }
    const std::uint32_t owner = activeDialogue_->npcObject;
    const std::string script = aborted ? activeDialogue_->dlg.endConversationAbort
                                       : activeDialogue_->dlg.endConversation;
    const ScriptContext context = dialogueContext();
    clearDialogueState();
    if (!script.empty()) {
        runScriptWith(script, owner, context);
    }
    ScriptApi{*this}.dispatchHook(owner, "ScriptEndDialogu", context);
}

void GameSession::endDialogue()
{
    finishDialogue(true);
}

void GameSession::attackNearestHostile()
{
    leaveStealth();
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 20.0f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Creature || entity.dead
            || !isEnemy(ScriptApi{*this}, scriptWorld_.pc, objectForEntity(i))) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    if (best == static_cast<std::size_t>(-1)) {
        logEvent("no hostile target");
        return;
    }
    combatTarget_ = static_cast<std::uint32_t>(best);
    // The attack command: an Attack action the player can cancel by steering. It
    // walks to range and runs rounds until the target falls (ScriptCombat.cpp).
    {
        ScriptApi api{*this};
        std::erase_if(api.actor(scriptWorld_.pc).queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
        ActorAction attack;
        attack.kind = ActorActionKind::Attack;
        attack.object = objectForEntity(best);
        attack.userIssued = true;
        api.enqueue(scriptWorld_.pc, std::move(attack));
    }
    logEvent("combat target " + combatTargetName());
    if (audio_) {
        audio_->playCreatureSound(static_cast<std::uint32_t>(best), 0); // battle cry
    }
    const kotor::Entity& target = scene_.entities[best];
    const float distance = distance2D(position_, target.position);
    const std::int32_t jump = rules_.forceJumpBonus(character_);
    if (distance > 10.0f && jump > 0 && rules_.hasLightsaber(character_)) {
        const kotor::Vec3 direction = (position_ - target.position) * (1.0f / distance);
        position_ = target.position + direction * 2.0f;
        float ground = position_.z;
        if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
            position_.z = ground;
        }
        forceJumpBonus_ = jump;
        logEvent("force jump +" + std::to_string(jump));
        syncPlayerToScriptWorld();
    }
}

std::string GameSession::combatTargetName() const
{
    if (combatTarget_ >= scene_.entities.size()) {
        return {};
    }
    const kotor::Entity& entity = scene_.entities[combatTarget_];
    return entity.name.empty() ? entity.tag : entity.name;
}

void GameSession::damageEntity(std::size_t index, std::int32_t amount, std::uint32_t attacker)
{
    if (index < scene_.entities.size() && !scene_.entities[index].dead) {
        applyScriptDamage(ScriptApi{*this}, objectForEntity(index), amount, 8, attacker);
    }
}

// The one way a creature dies. retail 004fb1e0 awards XP from the death itself,
// whoever struck the blow: never for the PC or a party member, and only when the
// dead creature's faction stands at enemy (<= 10) against the player's. The value
// is xptable by leader level and challenge rating (004f19e0), and giveXp splits it
// across the party with each member's npc.2da share (005653a0, 004ef930).
void GameSession::onCreatureKilled(std::size_t index, std::uint32_t killer)
{
    if (index >= scene_.entities.size() || scene_.entities[index].dead) {
        return;
    }
    kotor::Entity& entity = scene_.entities[index];
    entity.dead = true;
    if (combatTarget_ == index) {
        combatTarget_ = UINT32_MAX;
    }
    const std::uint32_t object = objectForEntity(index);
    ScriptApi api{*this};
    api.clearQueue(object);
    syncEntityToScriptWorld(index);
    if (!entity.partyAvatar && entity.kind == kotor::EntityKind::Creature
        && reputationBetween(api, scriptWorld_.pc, object) <= 10) {
        const std::int32_t award = rules_.killXp(
            rules_.levelForXp(character_.xp), entity.stats.challengeRating,
            static_cast<std::int32_t>(scriptWorld_.party.size()) + 1);
        if (award > 0) {
            giveXp(award);
        }
    }
    api.raiseHook(object, "Death", {{"lastKiller", killer}});
}

void GameSession::updateCombat(float dt)
{
    // New creatures, whichever path spawned them, get their sheet derived once.
    for (kotor::Entity& entity : scene_.entities) {
        if (entity.stats.parsed && !entity.stats.combatDerived) {
            rules_.deriveCreatureCombat(entity);
        }
    }
    tickScriptEffects(ScriptApi{*this}, dt);
    if (combatModeTimer_ > 0.0f) {
        combatModeTimer_ -= dt;
        if (combatModeTimer_ <= 0.0f) {
            combatMode_ = CombatMode::Normal;
        }
    }
    ScriptApi api{*this};
    ActorState& pc = api.actor(scriptWorld_.pc);
    if (stealthActive_) {
        std::erase_if(pc.queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
    }
    // combatTarget_ is the UI's view of the player's attack command.
    const auto attacking = std::find_if(pc.queue.begin(), pc.queue.end(), [](const ActorAction& action) {
        return action.kind == ActorActionKind::Attack;
    });
    if (attacking == pc.queue.end()) {
        combatTarget_ = UINT32_MAX;
    } else if (const std::size_t index = api.entityFor(attacking->object);
               index != static_cast<std::size_t>(-1)) {
        combatTarget_ = static_cast<std::uint32_t>(index);
    }
    if (!dialogueView_.active) {
        tickCombat(api, dt);
    }
}

bool GameSession::pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const
{
    for (std::size_t i = 0; i + 2 < trigger.indices.size(); i += 3) {
        if (trigger.indices[i] >= trigger.vertices.size()
            || trigger.indices[i + 1] >= trigger.vertices.size()
            || trigger.indices[i + 2] >= trigger.vertices.size()) {
            continue;
        }
        if (pointInTriangle2D(x, y, trigger.vertices[trigger.indices[i]],
                              trigger.vertices[trigger.indices[i + 1]],
                              trigger.vertices[trigger.indices[i + 2]])) {
            return true;
        }
    }
    return false;
}

void GameSession::updateTriggers()
{
    if (triggerInside_.size() != scene_.triggers.size()) {
        triggerInside_.assign(scene_.triggers.size(), false);
    }
    for (std::size_t i = 0; i < scene_.triggers.size(); ++i) {
        const kotor::TriggerVolume& trigger = scene_.triggers[i];
        if (trigger.entity >= scene_.entities.size()) {
            continue;
        }
        if (scene_.entities[trigger.entity].dead) {
            continue;
        }
        const bool inside = pointInTrigger(trigger, position_.x, position_.y);
        if (inside == triggerInside_[i]) {
            continue;
        }
        triggerInside_[i] = inside;
        // Hooks can spawn entities, so nothing keeps a reference across them.
        const std::size_t index = trigger.entity;
        if (inside) {
            runHook("ScriptOnEnter", index, scriptWorld_.pc);
            const kotor::Entity& entity = scene_.entities[index];
            logEvent("trigger " + entity.tag + ": enter " + entity.scriptOnEnter);
            // A trap trigger springs on the walk-in edge unless disarmed.
            if (entity.trapFlag) {
                springTrap(index, scriptWorld_.pc);
            }
            // Honour LinkedToModule on the walk-in edge; the swap happens at the
            // end of the tick, never with this loop mid-flight. A KeyName that is
            // required must be held (and is consumed with AutoRemoveKey), and an
            // unescapable area cannot be left this way.
            if (index < scene_.entities.size()) {
                const kotor::Entity& transition = scene_.entities[index];
                const bool wantsLeave = !transition.linkedModule.empty()
                    && transition.linkedFlags != 0 && pendingModule_.empty();
                if (wantsLeave && !sim_->areaUnescapable
                    && (!transition.keyRequired || transition.keyName.empty()
                        || keyUnlock(index) || playerHasKey(transition.keyName))) {
                    pendingModule_ = transition.linkedModule;
                    sim_->pendingLinkedTo = transition.linkedTo;
                } else if (wantsLeave) {
                    logEvent("trigger " + transition.tag + ": transition refused");
                }
            }
        } else {
            runHook("ScriptOnExit", index, scriptWorld_.pc);
            const kotor::Entity& entity = scene_.entities[index];
            logEvent("trigger " + entity.tag + ": exit " + entity.scriptOnExit);
        }
    }
}

std::int32_t GameSession::journalState(const std::string& plotId) const
{
    const auto it = journal_.find(plotId);
    if (it != journal_.end()) {
        return it->second;
    }
    const std::string wanted = lower(plotId);
    for (const auto& [plot, state] : journal_) {
        if (lower(plot) == wanted) {
            return state;
        }
    }
    return 0;
}

// global.jrl: Categories[]{Tag, PlotIndex, XP, EntryList[]{ID, End, Text,
// XP_Percentage}}, read the way 005c5a40 reads it.
void GameSession::loadJournalData()
{
    if (sim_->journalLoaded) {
        return;
    }
    sim_->journalLoaded = true;
    auto data = install_.find("global", 2056);
    if (!data.has_value()) {
        logEvent("journal: global.jrl not found");
        return;
    }
    try {
        kotor::Gff gff = kotor::Gff::parse(std::move(*data), "global.jrl");
        const kotor::GffField* categories = gff.field(gff.rootIndex(), "Categories");
        if (categories == nullptr) {
            return;
        }
        for (const std::uint32_t structure : gff.list(*categories)) {
            JournalCategory category;
            if (const kotor::GffField* f = gff.field(structure, "Tag"); f != nullptr) {
                category.tag = gff.text(*f);
            }
            if (const kotor::GffField* f = gff.field(structure, "PlotIndex"); f != nullptr) {
                category.plotIndex = gff.integer(*f);
            }
            if (const kotor::GffField* f = gff.field(structure, "XP"); f != nullptr) {
                category.xp = gff.integer(*f);
            }
            if (const kotor::GffField* f = gff.field(structure, "EntryList"); f != nullptr) {
                for (const std::uint32_t item : gff.list(*f)) {
                    JournalEntryRecord entry;
                    std::int32_t id = 0;
                    if (const kotor::GffField* v = gff.field(item, "ID"); v != nullptr) {
                        id = gff.integer(*v);
                    }
                    if (const kotor::GffField* v = gff.field(item, "End"); v != nullptr) {
                        entry.end = gff.integer(*v) != 0;
                    }
                    if (const kotor::GffField* v = gff.field(item, "XP_Percentage"); v != nullptr) {
                        entry.xpPercentage = gff.number(*v);
                    }
                    if (const kotor::GffField* v = gff.field(item, "Text");
                        v != nullptr && v->type == kotor::GffType::LocString) {
                        entry.textStrref = gff.locStringStrref(*v);
                    }
                    category.entries[id] = entry;
                }
            }
            sim_->journal[lower(category.tag)] = std::move(category);
        }
    } catch (const kotor::FormatError& error) {
        logEvent(std::string("journal: ") + error.what());
    }
}

void GameSession::loadPlotTable()
{
    if (sim_->plotLoaded) {
        return;
    }
    sim_->plotLoaded = true;
    auto data = install_.find("plot", 2017);
    if (!data.has_value()) {
        logEvent("plot.2da not found");
        return;
    }
    try {
        sim_->plot = kotor::TwoDa::parse(std::move(*data), "plot.2da");
    } catch (const kotor::FormatError& error) {
        logEvent(std::string("plot.2da: ") + error.what());
    }
}

const JournalCategory* GameSession::journalCategory(const std::string& plotId)
{
    loadJournalData();
    const auto it = sim_->journal.find(lower(plotId));
    return it != sim_->journal.end() ? &it->second : nullptr;
}

std::int32_t GameSession::plotXp(std::int32_t row, double fraction)
{
    loadPlotTable();
    if (!sim_->plot.has_value() || row < 0
        || static_cast<std::size_t>(row) >= sim_->plot->rowCount()) {
        return 0;
    }
    const int column = sim_->plot->columnIndex("xp");
    if (column < 0) {
        return 0;
    }
    const std::string cell =
        sim_->plot->cell(static_cast<std::size_t>(row), static_cast<std::size_t>(column));
    if (cell.empty() || cell == "****") {
        return 0;
    }
    return static_cast<std::int32_t>(std::atof(cell.c_str()) * fraction);
}

// 00566600 looks the row up by its label. plot.2da names its rows; the `label`
// column holds the same names, so accept either.
std::int32_t GameSession::plotXpByLabel(const std::string& label, double fraction)
{
    loadPlotTable();
    if (!sim_->plot.has_value()) {
        return 0;
    }
    const std::string wanted = lower(label);
    const int column = sim_->plot->columnIndex("label");
    for (std::size_t row = 0; row < sim_->plot->rowCount(); ++row) {
        if (lower(sim_->plot->rowLabel(row)) == wanted
            || (column >= 0
                && lower(sim_->plot->cell(row, static_cast<std::size_t>(column))) == wanted)) {
            return plotXp(static_cast<std::int32_t>(row), fraction);
        }
    }
    return 0;
}

std::int32_t GameSession::plotXpForIndex(std::int32_t plotIndex, float fraction)
{
    return plotXp(plotIndex, static_cast<double>(fraction));
}

void GameSession::givePlotXp(const std::string& plotLabel, std::int32_t percent)
{
    giveXp(plotXpByLabel(plotLabel, static_cast<double>(percent) * 0.01));
}

std::int32_t GameSession::journalQuestExperience(const std::string& plotId)
{
    const JournalCategory* category = journalCategory(plotId);
    return category != nullptr ? category->xp : 0;
}

std::string GameSession::journalEntryText(const std::string& plotId, std::int32_t state)
{
    const JournalCategory* category = journalCategory(plotId);
    if (category == nullptr || !tlk_.has_value()) {
        return {};
    }
    const auto entry = category->entries.find(state);
    if (entry == category->entries.end() || entry->second.textStrref == UINT32_MAX) {
        return {};
    }
    return substituteTokens(tlk_->text(entry->second.textStrref));
}

// ACT_367 005483f0: the state is taken absolute; unless override is set a state
// that does not advance is ignored; a state that does awards the entry's plot XP
// (trunc(plot.2da[Category.PlotIndex].xp * Entry.XP_Percentage), 005c5a40).
bool GameSession::addJournalQuest(const std::string& plotId, std::int32_t state,
                                  bool allowOverrideHigher)
{
    if (plotId.empty()) {
        return false;
    }
    state = std::abs(state);
    if (!allowOverrideHigher && state <= journalState(plotId)) {
        return false;
    }
    std::string key = plotId;
    const std::string wanted = lower(plotId);
    for (const auto& [plot, current] : journal_) {
        if (lower(plot) == wanted) {
            key = plot;
            break;
        }
    }
    addJournalEntry(key, state);
    if (const JournalCategory* category = journalCategory(plotId); category != nullptr) {
        const auto entry = category->entries.find(state);
        if (entry != category->entries.end()) {
            giveXp(plotXp(category->plotIndex, entry->second.xpPercentage));
        }
    }
    return true;
}

bool GameSession::addJournalEntry(const std::string& plotId, std::int32_t state)
{
    if (plotId.empty()) {
        return false;
    }
    journal_[plotId] = state;
    logEvent("journal " + plotId + " = " + std::to_string(state));
    return true;
}

bool GameSession::removeJournalEntry(const std::string& plotId)
{
    return journal_.erase(plotId) != 0;
}

bool GameSession::learnFeat(std::int32_t featId)
{
    if (character_.featPoints <= 0 || character_.feats.contains(featId)
        || !rules_.canTakeFeat(character_, featId)) {
        return false;
    }
    character_.feats.insert(featId);
    --character_.featPoints;
    if (rules_.loaded()) {
        const std::int32_t oldMax = character_.maxHp;
        character_.maxHp = rules_.maxHitPoints(character_.classId, character_.level,
                                               character_.con)
            + rules_.toughnessBonus(character_);
        character_.hp += std::max<std::int32_t>(0, character_.maxHp - oldMax);
        playerMaxHp_ = character_.maxHp;
        playerHp_ = std::min(playerHp_ + std::max<std::int32_t>(0, character_.maxHp - oldMax),
                            playerMaxHp_);
    }
    logEvent("learn feat " + rules_.featName(featId));
    return true;
}

bool GameSession::learnPower(std::int32_t powerId)
{
    if (character_.powers.contains(powerId) || !rules_.canTakePower(character_, powerId)) {
        return false;
    }
    character_.powers.insert(powerId);
    logEvent("learn power " + rules_.powerName(powerId));
    return true;
}

bool GameSession::spendSkillPoint(const std::string& skill)
{
    if (character_.skillPoints <= 0 || skill.empty()) {
        return false;
    }
    const auto it = character_.skills.find(skill);
    if (it == character_.skills.end()) {
        return false;
    }
    ++it->second;
    --character_.skillPoints;
    syncPlayerToScriptWorld();
    return true;
}

bool GameSession::spendAttributePoint(std::int32_t index)
{
    if (character_.attributePoints <= 0 || index < 0 || index > 5) {
        return false;
    }
    std::int32_t* value = nullptr;
    switch (index) {
    case 0: value = &character_.str; break;
    case 1: value = &character_.dex; break;
    case 2: value = &character_.con; break;
    case 3: value = &character_.intel; break;
    case 4: value = &character_.wis; break;
    case 5: value = &character_.cha; break;
    default: return false;
    }
    ++*value;
    --character_.attributePoints;
    if (index == 2 && rules_.loaded()) {
        character_.maxHp = rules_.maxHitPoints(character_.classId, character_.level,
                                               character_.con);
        playerMaxHp_ = character_.maxHp;
        playerHp_ = std::min(playerMaxHp_, playerHp_ + 1);
    }
    syncPlayerToScriptWorld();
    return true;
}

void GameSession::giveXp(std::int32_t amount){
    if (amount <= 0) {
        return;
    }
    character_.xp += amount;
    const std::int32_t level = rules_.loaded() ? rules_.levelForXp(character_.xp)
                                               : character_.level;
    if (level > character_.level) {
        for (std::int32_t next = character_.level + 1; next <= level; ++next) {
            if (rules_.loaded()) {
                character_.skillPoints += rules_.skillPointsForLevel(character_.classId,
                                                                     character_.intel);
                character_.featPoints += rules_.featsForLevel(character_.classId, next);
            }
            if (next % 4 == 0) {
                ++character_.attributePoints;
            }
        }
        character_.level = level;
        if (rules_.loaded()) {
            character_.maxHp = rules_.maxHitPoints(character_.classId, level,
                                                   character_.con);
            character_.hp = character_.maxHp;
        }
        playerMaxHp_ = character_.maxHp;
        playerHp_ = playerMaxHp_;
        logEvent("level up " + std::to_string(level));
        ScriptApi{*this}.raiseHook(sim_->moduleObject, "Mod_OnPlrLvlUp",
                                   {{"pcLevellingUp", scriptWorld_.pc}});
    }
    awardPartyXp(amount);
    logEvent("xp +" + std::to_string(amount) + " = " + std::to_string(character_.xp));
}

bool GameSession::useFeat(std::int32_t featId, std::uint32_t target)
{
    (void)target;
    if (!character_.feats.contains(featId) || !rules_.loaded()) {
        return false;
    }
    const std::string label = rules_.featName(featId);
    if (label.find("POWER_ATTACK") != std::string::npos
        || label.find("POWER_BLAST") != std::string::npos) {
        combatMode_ = CombatMode::PowerAttack;
    } else if (label.find("FLURRY") != std::string::npos) {
        combatMode_ = CombatMode::Flurry;
    } else if (label.find("CRITICAL_STRIKE") != std::string::npos) {
        combatMode_ = CombatMode::CriticalStrike;
    } else if (label == "WHIRLWIND_ATTACK") {
        combatMode_ = CombatMode::Whirlwind;
    } else {
        return false;
    }
    combatModeRank_ = label.find("MASTER") != std::string::npos ? 3
        : label.find("IMPROVED") != std::string::npos ? 2 : 1;
    leaveStealth();
    combatModeTimer_ = 30.0f;
    logEvent("feat " + label);
    return true;
}

bool GameSession::useForcePower(std::int32_t powerId, std::uint32_t target)
{
    if (!rules_.loaded() || !character_.powers.contains(powerId)) {
        return false;
    }
    const std::optional<ForcePowerInfo> info = rules_.forcePower(powerId);
    if (!info.has_value() || character_.fp < info->cost) {
        lastForcePower_ = powerId;
        lastForcePowerSuccess_ = false;
        return false;
    }
    const auto contains = [&](const char* needle) {
        return info->label.find(needle) != std::string::npos;
    };
    const std::uint32_t self = scriptWorld_.pc;
    const std::uint32_t targetObject = target == 0 ? self : target;
    const auto entityIndex = [&](std::uint32_t object) -> std::size_t {
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            if (objectForEntity(i) == object) {
                return i;
            }
        }
        return static_cast<std::size_t>(-1);
    };
    const bool selfTarget = targetObject == self;
    const bool immuneFear = selfTarget
        && rules_.hasFeatLabel(character_, "FORCE_IMMUNITY_FEAR");
    const bool immuneStun = selfTarget
        && rules_.hasFeatLabel(character_, "FORCE_IMMUNITY_STUN");
    const bool immuneParalysis = selfTarget
        && rules_.hasFeatLabel(character_, "FORCE_IMMUNITY_PARALYSIS");
    const std::size_t targetIndex = selfTarget
        ? static_cast<std::size_t>(-1)
        : entityIndex(targetObject);
    if (!selfTarget && targetIndex == static_cast<std::size_t>(-1)) {
        lastForcePower_ = powerId;
        lastForcePowerSuccess_ = false;
        return false;
    }
    if (!selfTarget && info->range > 0) {
        const kotor::Vec3 delta = scene_.entities[targetIndex].position - position_;
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        if (distance > static_cast<float>(info->range) + 0.5f) {
            lastForcePower_ = powerId;
            lastForcePowerSuccess_ = false;
            return false;
        }
    }
    std::vector<kotor::ActiveEffect>* targetEffects = selfTarget
        ? &character_.effects
        : &scene_.entities[targetIndex].effects;
    const bool bypassImmunity = contains("BREACH") || contains("SUPRESS")
        || contains("SUPPRESS");
    const bool casterSuppressed = character_.forceSuppressedTimer > 0.0f;
    const bool targetSuppressed = !selfTarget
        && scene_.entities[targetIndex].forceSuppressedTimer > 0.0f;
    if (!bypassImmunity
        && (hasEffectLabel(*targetEffects, "FORCE_IMMUNITY") || casterSuppressed
            || targetSuppressed)) {
        lastForcePower_ = powerId;
        lastForcePowerSuccess_ = false;
        logEvent("force " + info->label + " resisted");
        return false;
    }
    character_.fp -= info->cost;
    leaveStealth();
    lastForcePower_ = powerId;
    lastForcePowerSuccess_ = true;
    const auto roll = [&]() { return 1 + static_cast<std::int32_t>(random() % 20u); };
    const auto damageRoll = [&](int dice, int sides) {
        int value = 0;
        for (int i = 0; i < dice; ++i) {
            value += 1 + static_cast<int>(random() % static_cast<std::uint32_t>(sides));
        }
        return value;
    };
    const std::int32_t dc = rules_.forcePowerSaveDC(character_);
    const int saveType = (contains("LIGHTNING") || contains("SHOCK") || contains("STORM")
                          || contains("PUSH") || contains("WAVE") || contains("WHIRLWIND"))
        ? 2
        : (contains("FEAR") || contains("HORROR") || contains("INSANITY")
           || contains("STUN") || contains("STASIS") || contains("SLOW"))
        ? 1
        : 0;
    const int saveBonus = selfTarget ? 0
        : saveType == 0 ? scene_.entities[targetIndex].fortSave
        : saveType == 1 ? scene_.entities[targetIndex].willSave
                        : scene_.entities[targetIndex].refSave;
    const bool saved = !selfTarget && roll() + saveBonus >= dc;
    if (!selfTarget) {
        ScriptApi{*this}.raiseHook(objectForEntity(targetIndex), "SpellAt",
                                   {{"lastSpellCaster", scriptWorld_.pc},
                                    {"lastSpell", static_cast<std::uint32_t>(powerId)},
                                    {"lastSpellHarmful",
                                     contains("HEAL") || contains("CURE") ? 0u : 1u}});
    }

    if (contains("HEAL") || contains("CURE")) {
        const std::int32_t heal = 8 + character_.level * 2
            + std::max(0, (character_.wis - 10) / 2);
        if (selfTarget) {
            playerHp_ = std::min(playerMaxHp_, playerHp_ + heal);
            character_.hp = playerHp_;
        } else {
            kotor::Entity& entity = scene_.entities[targetIndex];
            entity.hitPoints = std::min(entity.maxHitPoints, entity.hitPoints + heal);
        }
        logEvent("force heal " + std::to_string(heal));
    } else if (contains("LIGHTNING") || contains("SHOCK") || contains("STORM")
               || contains("DEATH_FIELD") || contains("DRAIN_LIFE") || contains("WOUND")
               || contains("KILL") || contains("CHOKE") || contains("PLAGUE")
               || contains("AFFLICTION") || contains("SABER_THROW") || contains("PUSH")
               || contains("WAVE") || contains("WHIRLWIND") || contains("DROID_DISABLE")
               || contains("DROID_DESTROY")) {
        if (selfTarget) {
            logEvent("force power has no hostile target");
            lastForcePowerSuccess_ = false;
        } else {
            kotor::Entity& entity = scene_.entities[targetIndex];
            const int dice = std::max(1, character_.level / 2);
            std::int32_t damage = damageRoll(dice, 6);
            if (saved) {
                damage = std::max<std::int32_t>(1, damage / 2);
            }
            if (info->alignment == 'E') {
                damage += 2;
            }
            const bool killed = entity.hitPoints <= damage;
            if (contains("STUN") || contains("STASIS") || contains("SLOW")
                || contains("DROID_DISABLE")) {
                const bool blocked = selfTarget
                    && ((contains("STASIS") && immuneParalysis)
                        || (contains("STUN") && immuneStun));
                if (!saved && !blocked) {
                    entity.stunTimer = 6.0f;
                }
            }
            if (contains("FEAR") || contains("HORROR") || contains("INSANITY")) {
                if (!saved && !(selfTarget && immuneFear)) {
                    entity.fearTimer = 6.0f;
                }
            }
            if (killed) {
                logEvent("force power defeats " + entity.tag);
            }
            damageEntity(targetIndex, damage, scriptWorld_.pc);
            logEvent("force " + info->label + " damage=" + std::to_string(damage)
                     + (saved ? " saved" : ""));
        }
    } else if (contains("STUN") || contains("STASIS") || contains("SLOW")
               || contains("DROID_STUN") || contains("AFFECT_MIND")
               || contains("DOMINATE")) {
        const bool blocked = selfTarget
            && ((contains("STASIS") && immuneParalysis)
                || (contains("STUN") && immuneStun));
        if (!selfTarget && !saved) {
            scene_.entities[targetIndex].stunTimer = 6.0f;
        } else if (selfTarget && !blocked) {
            // Self-targeted stun powers do not apply in normal play; keep the
            // immunity branch deterministic without adding a player stun.
        }
        logEvent("force " + info->label
                 + (saved ? " saved" : blocked ? " immune" : ""));
    } else if (contains("FEAR") || contains("HORROR") || contains("INSANITY")) {
        if (!selfTarget && !saved) {
            scene_.entities[targetIndex].fearTimer = 6.0f;
        }
        logEvent("force " + info->label
                 + (saved ? " saved" : (selfTarget && immuneFear) ? " immune" : ""));
    } else if (contains("BREACH")) {
        const std::size_t before = targetEffects->size();
        std::erase_if(*targetEffects, [](const kotor::ActiveEffect& effect) {
            return effect.dispellable;
        });
        logEvent("force breach removed " + std::to_string(before - targetEffects->size()));
    } else if (contains("SUPRESS") || contains("SUPPRESS")) {
        std::erase_if(*targetEffects, [](const kotor::ActiveEffect& effect) {
            return effect.dispellable;
        });
        if (selfTarget) {
            character_.forceSuppressedTimer = 15.0f;
        } else {
            scene_.entities[targetIndex].forceSuppressedTimer = 15.0f;
        }
        logEvent("force suppress");
    } else if (contains("IMMUNITY")) {
        kotor::ActiveEffect effect;
        effect.spellId = powerId;
        effect.label = info->label;
        effect.remaining = 30.0f;
        effect.force = true;
        effect.dispellable = true;
        addOrRefreshEffect(*targetEffects, std::move(effect));
        logEvent("force immunity");
    } else if (contains("ARMOR") || contains("AURA") || contains("SHIELD")
               || contains("RESIST")) {
        kotor::ActiveEffect effect;
        effect.spellId = powerId;
        effect.label = info->label;
        effect.remaining = 30.0f;
        effect.armorBonus = 4 + character_.level / 4;
        if (contains("SHIELD")) {
            effect.shield = 10 + character_.level * 2;
        }
        effect.force = true;
        effect.dispellable = true;
        addOrRefreshEffect(*targetEffects, std::move(effect));
        logEvent("force armor +" + std::to_string(4 + character_.level / 4));
    } else if (contains("VALOR")) {
        kotor::ActiveEffect effect;
        effect.spellId = powerId;
        effect.label = info->label;
        effect.remaining = 30.0f;
        const std::int32_t valor = 2 + character_.level / 5;
        effect.attackBonus = valor;
        effect.damageBonus = valor;
        effect.force = true;
        effect.dispellable = true;
        addOrRefreshEffect(*targetEffects, std::move(effect));
        logEvent("force valor +" + std::to_string(valor));
    } else if (contains("SPEED")) {
        kotor::ActiveEffect effect;
        effect.spellId = powerId;
        effect.label = info->label;
        effect.remaining = 20.0f;
        effect.force = true;
        effect.dispellable = true;
        addOrRefreshEffect(*targetEffects, std::move(effect));
        logEvent("force speed");
    } else {
        logEvent("force " + info->label);
    }

    if (info->alignment == 'G') {
        adjustAlignment(2);
    } else if (info->alignment == 'E') {
        adjustAlignment(-2);
    }
    syncPlayerToScriptWorld();
    return true;
}

std::optional<std::vector<script::Value>> GameSession::spaceMinigameAction(
    std::uint16_t actionId, const std::vector<std::vector<script::Value>>& args)
{
    const auto cell = [&](std::size_t index) -> script::Value {
        return index < args.size() && !args[index].empty() ? args[index].front() : script::Value{};
    };
    const auto intArg = [&](std::size_t index) { return cell(index).asInt(); };
    const auto objectArg = [&](std::size_t index) {
        const script::Value value = cell(index);
        return value.type == script::ValueType::Object
            ? static_cast<std::uint32_t>(value.integer) : script::kObjectInvalid;
    };
    const auto isEnemy = [&](std::uint32_t object) {
        return std::find(spaceEnemies_.begin(), spaceEnemies_.end(), object)
            != spaceEnemies_.end();
    };
    switch (actionId) {
    case 583: // SWMG_GetLastEvent
        return std::vector<script::Value>{script::Value::makeString(spaceLastEvent_)};
    case 584: // SWMG_GetLastEventModelName
        return std::vector<script::Value>{script::Value::makeString("")};
    case 585: { // SWMG_GetObjectByName
        const std::string name = cell(0).asString();
        if (lower(name) == "player") {
            return objectCells(scriptWorld_.pc);
        }
        return objectCells(scriptWorld_.byTag(name, 0));
    }
    case 586: // SWMG_PlayAnimation
    case 591: // SWMG_OnBulletHit
    case 592: // SWMG_OnObstacleHit
    case 598: // SWMG_OnDeath
    case 605: // SWMG_OnDamage
    case 607: // SWMG_RemoveAnimation
    case 610: // SWMG_SetCameraClip
        return std::vector<script::Value>{};
    case 599: // SWMG_IsFollower
    case 602: // SWMG_IsTrigger
    case 603: // SWMG_IsObstacle
        return intCells(0);
    case 600: // SWMG_IsPlayer
        return intCells(objectArg(0) == scriptWorld_.pc);
    case 601: // SWMG_IsEnemy
        return intCells(isEnemy(objectArg(0)) ? 1 : 0);
    case 611: // SWMG_GetPlayer
        return objectCells(scriptWorld_.pc);
    case 612: { // SWMG_GetEnemyCount
        if (spaceMode_ != SpaceMode::Turret) {
            startTurret();
            spaceMode_ = SpaceMode::Turret;
            spaceLastEvent_.clear();
        }
        ensureSpaceEnemies();
        return intCells(static_cast<std::int64_t>(spaceEnemies_.size()));
    }
    case 613: { // SWMG_GetEnemy
        if (spaceMode_ != SpaceMode::Turret) {
            startTurret();
            spaceMode_ = SpaceMode::Turret;
        }
        ensureSpaceEnemies();
        const std::size_t index = static_cast<std::size_t>(std::max<std::int64_t>(0, intArg(0)));
        return objectCells(index < spaceEnemies_.size() ? spaceEnemies_[index]
                                                        : script::kObjectInvalid);
    }
    case 614: // SWMG_GetObstacleCount
        return intCells(0);
    case 615: // SWMG_GetObstacle
        return objectCells(script::kObjectInvalid);
    case 616: { // SWMG_GetHitPoints
        const std::uint32_t object = objectArg(0);
        if (object == scriptWorld_.pc) {
            return intCells(playerHp_);
        }
        if (isEnemy(object)) {
            return intCells(100);
        }
        return intCells(0);
    }
    case 617: // SWMG_GetMaxHitPoints
        return intCells(100);
    case 618: // SWMG_SetMaxHitPoints
        return std::vector<script::Value>{};
    case 619: // SWMG_GetSphereRadius
        return floatCells(1.0);
    case 643: // SWMG_GetPlayerSpeed
        if (spaceMode_ != SpaceMode::Swoop) {
            startSwoop();
            spaceMode_ = SpaceMode::Swoop;
            spaceLastEvent_.clear();
        }
        return floatCells(swoop_.state().speed);
    case 644: // SWMG_GetPlayerMinSpeed
        return floatCells(0.0);
    case 645: // SWMG_GetPlayerAccelerationPerSecond
        return floatCells(70.0);
    case 667: // SWMG_GetPlayerMaxSpeed
        return floatCells(140.0);
    default:
        return std::nullopt;
    }
}

void GameSession::ensureSpaceEnemies()
{
    if (!spaceEnemies_.empty()) {
        return;
    }
    for (int i = 0; i < 5; ++i) {
        const std::string tag = "space_enemy_" + std::to_string(i);
        script::GameObject& object = scriptWorld_.addObject(tag, 100, 0, true);
        spaceEnemies_.push_back(object.id);
    }
}

std::uint32_t GameSession::spaceEnemy(std::size_t index)
{
    ensureSpaceEnemies();
    return index < spaceEnemies_.size() ? spaceEnemies_[index] : script::kObjectInvalid;
}

std::optional<std::vector<script::Value>> GameSession::action(
    std::uint16_t actionId, const std::vector<std::vector<script::Value>>& args,
    script::World& world, script::Vm& vm)
{
    if (const ActionHandler handler = sim_->handlers.get(actionId); handler != nullptr) {
        ActionCall call{*this, world, vm, args, actionId};
        if (auto handled = handler(call); handled.has_value()) {
            return handled;
        }
    }
    const auto arg = [&](std::size_t index, script::ParamKind kind) -> script::Value {
        (void)kind;
        return index < args.size() && !args[index].empty()
            ? args[index].front()
            : script::Value{};
    };
    const auto objectArg = [&](std::size_t index) -> std::uint32_t {
        const script::Value value = arg(index, script::ParamKind::Object);
        const std::uint32_t id = value.type == script::ValueType::Object
            ? static_cast<std::uint32_t>(value.integer)
            : script::kObjectInvalid;
        return id == 0 && runtime_ != nullptr ? runtime_->contextObject("self") : id;
    };
    const auto intArg = [&](std::size_t index) -> std::int64_t {
        return arg(index, script::ParamKind::Int).asInt();
    };
    const auto stringArg = [&](std::size_t index) -> std::string {
        return arg(index, script::ParamKind::String).asString();
    };
    const auto isPlayer = [&](std::uint32_t object) {
        return object == world.pc || object == script::kObjectInvalid;
    };
    const auto entityForObject = [&](std::uint32_t object) -> std::size_t {
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            if (objectForEntity(i) == object) {
                return i;
            }
        }
        return static_cast<std::size_t>(-1);
    };
    const auto vectorArg = [&](std::size_t index) -> kotor::Vec3 {
        if (index >= args.size() || args[index].size() < 3) {
            return {};
        }
        return {static_cast<float>(args[index][0].asFloat()),
                static_cast<float>(args[index][1].asFloat()),
                static_cast<float>(args[index][2].asFloat())};
    };
    if (actionId >= 520 && actionId <= 730) {
        if (auto handled = spaceMinigameAction(actionId, args); handled.has_value()) {
            return handled;
        }
    }

    switch (actionId) {
    case 418: // GetGold
        return intCells(gold_);
    case 393: // GiveXPToCreature
        if (isPlayer(objectArg(0))) {
            giveXp(static_cast<std::int32_t>(intArg(1)));
        }
        return std::vector<script::Value>{};
    case 394: // SetXP
        character_.xp = static_cast<std::int32_t>(std::max<std::int64_t>(0, intArg(1)));
        character_.level = rules_.loaded() ? rules_.levelForXp(character_.xp) : 1;
        return std::vector<script::Value>{};
    case 395: // GetXP
        if (isPlayer(objectArg(0))) {
            return intCells(character_.xp);
        }
        if (const script::GameObject* object = world.get(objectArg(0)); object != nullptr) {
            const auto it = object->attrs.find("xp");
            if (it != object->attrs.end() && std::holds_alternative<std::int64_t>(it->second)) {
                return intCells(std::get<std::int64_t>(it->second));
            }
        }
        return intCells(0);
    case 208: // GetReputation
        return intCells(factions_.reputation(factionOfObject(objectArg(0)),
                                             factionOfObject(objectArg(1))));
    case 209: // AdjustReputation
        adjustReputation(factionOfObject(objectArg(0)),
                         factionOfObject(objectArg(1)),
                         static_cast<int>(intArg(2)));
        return std::vector<script::Value>{};
    case 186: { // GetFactionAverageReputation
        const int target = factionOfObject(objectArg(1));
        std::int64_t sum = 0;
        std::int64_t count = 0;
        for (const std::uint32_t member : world.party) {
            if (world.get(member) == nullptr) {
                continue;
            }
            sum += factions_.reputation(factionOfObject(member), target);
            ++count;
        }
        return intCells(count > 0 ? sum / count : 0);
    }
    case 367: // AddJournalQuestEntry
        scriptLog_.push_back("journal action " + stringArg(0) + " = "
                             + std::to_string(intArg(1)));
        addJournalQuest(stringArg(0), static_cast<std::int32_t>(intArg(1)), intArg(2) != 0);
        return std::vector<script::Value>{};
    case 384: // GetJournalQuestExperience (0053a680)
        return intCells(journalQuestExperience(stringArg(0)));
    case 714: // GivePlotXP (00545e20 -> 00566600)
        givePlotXp(stringArg(0), static_cast<std::int32_t>(intArg(1)));
        return std::vector<script::Value>{};
    case 368: // RemoveJournalQuestEntry
        removeJournalEntry(stringArg(0));
        return std::vector<script::Value>{};
    case 369: // GetJournalEntry
        return intCells(journalState(stringArg(0)));
    // 572-577 and 126 (party membership, count and index) are table handlers in
    // ScriptParty.cpp; the old stubs here treated RemovePartyMember as a no-op.
    case 255: { // BeginConversation: the caller's conversation, or the given one.
        startConversation(runtime_ != nullptr ? runtime_->contextObject("self") : world.pc,
                          stringArg(0));
        return intCells(1);
    }
    case 509: { // StartNewModule(module, waypoint, up to six movies; 00544390)
        const std::string next = stringArg(0);
        if (!next.empty()) {
            pendingModule_ = next;
            sim_->pendingLinkedTo = stringArg(1);
            sim_->pendingMovies.clear();
            for (std::size_t i = 2; i < 8; ++i) {
                if (!stringArg(i).empty()) {
                    sim_->pendingMovies.push_back(stringArg(i));
                }
            }
        }
        return std::vector<script::Value>{};
    }
    case 241: { // DestroyObject
        script::GameObject* object = world.get(objectArg(0));
        if (object != nullptr) {
            object->valid = false;
            object->attrs["destroyed"] = static_cast<std::int64_t>(1);
            for (std::size_t i = 0; i < entityObjects_.size(); ++i) {
                if (entityObjects_[i] == object->id && i < scene_.entities.size()) {
                    scene_.entities[i].dead = true;
                    break;
                }
            }
        }
        return std::vector<script::Value>{};
    }
    case 166: // GetHitDice
        return intCells(character_.level);
    case 341: // GetClassByPosition
        return intCells(character_.classId);
    case 342: // GetLevelByPosition
        return intCells(character_.level);
    case 343: // GetLevelByClass
        return intCells(static_cast<std::int64_t>(intArg(0)) == character_.classId
                            ? character_.level
                            : 0);
    case 701: // GetIsConversationActive
        return intCells(dialogueView_.active ? 1 : 0);
    case 217: // GetIsPC
        return intCells(objectArg(0) == world.pc ? 1 : 0);
    case 711: // GetLastConversation
        return std::vector<script::Value>{script::Value::makeString(
            activeDialogue_.has_value() ? activeDialogue_->npcTag : std::string{})};
    case 25: // GetEnteringObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("entering")
                                               : script::kObjectInvalid);
    case 26: // GetExitingObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("exiting")
                                               : script::kObjectInvalid);
    case 47: // GetSpellTargetObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("spellTarget")
                                               : script::kObjectInvalid);
    case 238: // GetPCSpeaker
        return objectCells(world.pc);
    case 245: // GetLastSpellCaster
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastSpellCaster")
                                               : script::kObjectInvalid);
    case 254: // GetLastSpeaker
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastSpeaker")
                                               : script::kObjectInvalid);
    case 256: // GetLastPerceived
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastPerceived")
                                               : script::kObjectInvalid);
    case 260: // GetLastClosedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastClosedBy")
                                               : script::kObjectInvalid);
    case 291: // GetLastPlayerDied
        return objectCells(playerDead_ ? world.pc : script::kObjectInvalid);
    case 326: // GetClickingObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("clicking")
                                               : script::kObjectInvalid);
    case 330: // GetLastUsedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastUsedBy")
                                               : script::kObjectInvalid);
    case 346: // GetLastDamager
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastDamager")
                                               : script::kObjectInvalid);
    case 375: // GetAttemptedSpellTarget
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("attemptedTarget")
                                               : script::kObjectInvalid);
    case 376: // GetLastOpenedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastOpenedBy")
                                               : script::kObjectInvalid);
    case 410: // GetLastPlayerDying
        return objectCells(playerDead_ ? world.pc : script::kObjectInvalid);
    case 419: // GetLastRespawnButtonPresser
        return objectCells(script::kObjectInvalid);
    case 437: // GetLastKiller
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastKiller")
                                               : script::kObjectInvalid);
    case 439: // GetItemActivated
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemActivated")
                                               : script::kObjectInvalid);
    case 440: // GetItemActivator
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemActivator")
                                               : script::kObjectInvalid);
    case 442: // GetItemActivatedTarget
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemTarget")
                                               : script::kObjectInvalid);
    case 46: // PlaySound
        if (audio_) {
            audio_->playEffect(stringArg(0));
            logEvent("sound " + stringArg(0));
        }
        return std::vector<script::Value>{};
    case 413: // SoundObjectPlay
    case 414: // SoundObjectStop
    case 415: // SoundObjectSetVolume
    case 416: { // SoundObjectSetPosition
        if (!audio_) {
            return std::vector<script::Value>{};
        }
        const std::size_t entity = entityForObject(objectArg(0));
        if (entity == static_cast<std::size_t>(-1)) {
            return std::vector<script::Value>{};
        }
        if (actionId == 413) {
            audio_->playSoundObject(static_cast<std::uint32_t>(entity));
        } else if (actionId == 414) {
            audio_->stopSoundObject(static_cast<std::uint32_t>(entity));
        } else if (actionId == 415) {
            audio_->setSoundObjectVolume(static_cast<std::uint32_t>(entity),
                                         static_cast<float>(intArg(1)) / 100.0f);
        } else {
            audio_->setSoundObjectPosition(static_cast<std::uint32_t>(entity),
                                           vectorArg(1));
        }
        return std::vector<script::Value>{};
    }
    case 417: // SpeakOneLinerConversation
        if (audio_ && !stringArg(0).empty()) {
            audio_->playVoice(stringArg(0));
        }
        return std::vector<script::Value>{};
    case 671: { // BarkString
        const std::size_t entity = entityForObject(objectArg(0));
        if (audio_ && entity != static_cast<std::size_t>(-1)) {
            audio_->playCreatureSound(static_cast<std::uint32_t>(entity),
                                      static_cast<int>(intArg(1)));
        }
        return std::vector<script::Value>{};
    }
    case 733: // PlayMovie
        playMovie(stringArg(0));
        return std::vector<script::Value>{};
    case 364: // PlayPazaak
        startPazaak(static_cast<std::int32_t>(intArg(0)), stringArg(1),
                    static_cast<std::int32_t>(intArg(2)), intArg(3) != 0,
                    objectArg(4));
        return std::vector<script::Value>{};
    case 365: // GetLastPazaakResult
        return intCells(lastPazaakResult_ < 0 ? 0 : lastPazaakResult_);
    case 768: // IsMoviePlaying
        return intCells(moviePlaying_ ? 1 : 0);
    case 769: // QueueMovie
        queueMovie(stringArg(0), intArg(1) != 0);
        return std::vector<script::Value>{};
    case 770: // PlayMovieQueue
        playMovieQueue();
        return std::vector<script::Value>{};
    case 425: // MusicBackgroundPlay
    case 426: // MusicBackgroundStop
    case 427: // MusicBackgroundSetDelay
    case 428: // MusicBackgroundChangeDay
    case 429: { // MusicBackgroundChangeNight
        if (!audio_) {
            return std::vector<script::Value>{};
        }
        if (actionId == 425) {
            audio_->playMusic(scene_.areaAudio.musicDayId);
        } else if (actionId == 426) {
            audio_->stopMusic();
        } else if (actionId == 427) {
            scene_.areaAudio.musicDelay = static_cast<int>(intArg(1));
        } else if (actionId == 428) {
            scene_.areaAudio.musicDayId = static_cast<std::int32_t>(intArg(1));
            audio_->playMusic(scene_.areaAudio.musicDayId);
        } else {
            scene_.areaAudio.musicNightId = static_cast<std::int32_t>(intArg(1));
            audio_->playMusic(scene_.areaAudio.musicNightId);
        }
        return std::vector<script::Value>{};
    }
    case 433: // AmbientSoundPlay
    case 434: // AmbientSoundStop
    case 435: // AmbientSoundChangeDay
    case 436: { // AmbientSoundChangeNight
        if (!audio_) {
            return std::vector<script::Value>{};
        }
        if (actionId == 433) {
            audio_->playAmbient(scene_.areaAudio.ambientDayId);
        } else if (actionId == 434) {
            audio_->stopAmbient();
        } else if (actionId == 435) {
            scene_.areaAudio.ambientDayId = static_cast<std::int32_t>(intArg(1));
            audio_->playAmbient(scene_.areaAudio.ambientDayId);
        } else {
            scene_.areaAudio.ambientNightId = static_cast<std::int32_t>(intArg(1));
            audio_->playAmbient(scene_.areaAudio.ambientNightId);
        }
        return std::vector<script::Value>{};
    }
    case 558: // MusicBackgroundGetDayTrack
        return intCells(scene_.areaAudio.musicDayId);
    case 559: // MusicBackgroundGetNightTrack
        return intCells(scene_.areaAudio.musicNightId);
    case 569: // MusicBackgroundGetBattleTrack
        return intCells(scene_.areaAudio.musicBattleId);
    case 567: // AmbientSoundSetDayVolume
    case 568: { // AmbientSoundSetNightVolume
        const float volume = std::clamp(static_cast<float>(intArg(1)) / 100.0f,
                                        0.0f, 1.0f);
        if (actionId == 567) {
            scene_.areaAudio.ambientDayVolume = static_cast<int>(intArg(1));
        } else {
            scene_.areaAudio.ambientNightVolume = static_cast<int>(intArg(1));
        }
        if (audio_) {
            audio_->mixer().setBusVolume(audio::Bus::Ambient, volume);
        }
        return std::vector<script::Value>{};
    }
    case 300: { // PlayAnimation
        const std::int32_t animationId = static_cast<std::int32_t>(intArg(0));
        const std::string name = rules_.animationName(animationId);
        if (!name.empty()) {
            const std::uint32_t self = runtime_ != nullptr
                ? runtime_->contextObject("self") : world.pc;
            for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
                if (objectForEntity(i) == self) {
                    playAnimation(i, name, rules_.animationLooping(animationId));
                    break;
                }
            }
        }
        return std::vector<script::Value>{};
    }
    case 48: // ActionCastSpellAtObject
        useForcePower(static_cast<std::int32_t>(intArg(0)), objectArg(1));
        return std::vector<script::Value>{};
    case 55: // GetCurrentForcePoints
        return intCells(character_.fp);
    case 56: // GetMaxForcePoints
        return intCells(character_.maxFp);
    case 125: // GetGoodEvilValue
        return intCells(character_.alignment);
    case 127: // GetAlignmentGoodEvil
        return intCells(character_.alignment > 50 ? 1
                        : character_.alignment < 50 ? 2 : 0);
    case 201: // AdjustAlignment
        adjustAlignment(static_cast<std::int32_t>(intArg(1)));
        return std::vector<script::Value>{};
    case 285: // GetHasFeat
        return intCells(character_.feats.contains(static_cast<std::int32_t>(intArg(0))) ? 1 : 0);
    case 287: // ActionUseFeat
        useFeat(static_cast<std::int32_t>(intArg(0)), objectArg(1));
        return std::vector<script::Value>{};
    case 304: // GetHasSpellEffect
        return intCells(hasEffect(rules_.powerName(static_cast<std::int32_t>(intArg(0))))
                            ? 1 : 0);
    case 309: // ActionUseTalentOnObject
    case 310: { // ActionUseTalentAtLocation
        const std::int64_t talent = intArg(0);
        const std::int32_t type = static_cast<std::int32_t>((talent >> 29) & 0x7);
        const std::int32_t id = static_cast<std::int32_t>(talent & 0x1FFFFFFF);
        if (type == 1) {
            useForcePower(id, objectArg(1));
        } else if (type == 2) {
            useFeat(id, objectArg(1));
        } else {
            scriptLog_.push_back("talent type " + std::to_string(type)
                                 + " id " + std::to_string(id));
        }
        return std::vector<script::Value>{};
    }
    case 317: // GetLastAttackType
        return intCells(static_cast<std::int32_t>(combatMode_));
    case 318: // GetLastAttackMode
        return intCells(static_cast<std::int32_t>(combatMode_));
    case 377: // GetHasSpell
        return intCells(character_.powers.contains(static_cast<std::int32_t>(intArg(0))) ? 1 : 0);
    case 543: // GetHasFeatEffect
        return intCells(character_.feats.contains(static_cast<std::int32_t>(intArg(0))) ? 1 : 0);
    case 723: // GetLastForcePowerUsed
        return intCells(lastForcePower_);
    case 726: // GetWasForcePowerSuccessful
        return intCells(lastForcePowerSuccess_ ? 1 : 0);
    case 750: // SetGoodEvilValue
        setAlignment(static_cast<std::int32_t>(intArg(1)));
        return std::vector<script::Value>{};
    default:
        break;
    }
    return std::nullopt;
}

bool GameSession::save(const std::filesystem::path& path) const
{
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    // Enough digits that a saved position reads back the position it was.
    file << std::setprecision(9);
    file << "schema=kotor.engine.player/4\n";
    file << "module=" << module_ << "\n";
    file << "x=" << position_.x << "\ny=" << position_.y << "\nz=" << position_.z << "\n";
    file << "yaw=" << yaw_ << "\npitch=" << pitch_ << "\n";
    file << "hp=" << playerHp_ << "\n";
    file << "name=" << character_.name << "\n";
    file << "class=" << character_.classId << "\n";
    file << "level=" << character_.level << "\n";
    file << "xp=" << character_.xp << "\n";
    file << "gold=" << gold_ << "\n";
    file << "alignment=" << character_.alignment << "\n";
    file << "force_points=" << character_.fp << "\n";
    file << "max_force_points=" << character_.maxFp << "\n";
    file << "str=" << character_.str << "\ndex=" << character_.dex
         << "\ncon=" << character_.con << "\nint=" << character_.intel
         << "\nwis=" << character_.wis << "\ncha=" << character_.cha << "\n";
    file << "feat_points=" << character_.featPoints << "\n";
    file << "skill_points=" << character_.skillPoints << "\n";
    file << "attribute_points=" << character_.attributePoints << "\n";
    for (const auto& [skill, rank] : character_.skills) {
        file << "skill=" << skill << "|" << rank << "\n";
    }
    for (const std::int32_t feat : character_.feats) {
        file << "feat=" << feat << "\n";
    }
    for (const std::int32_t power : character_.powers) {
        file << "power=" << power << "\n";
    }
    for (const kotor::ActiveEffect& effect : character_.effects) {
        file << "effect=" << effect.spellId << "|" << effect.label << "|"
             << effect.remaining << "|" << effect.armorBonus << "|"
             << effect.attackBonus << "|" << effect.damageBonus << "|"
             << effect.shield << "|" << (effect.force ? 1 : 0) << "|"
             << (effect.dispellable ? 1 : 0) << "\n";
    }
    file << "force_suppressed=" << character_.forceSuppressedTimer << "\n";
    for (const auto& [plot, state] : journal_) {
        file << "journal=" << plot << ":" << state << "\n";
    }
    writePartyState(file);
    for (const MerchantState& merchant : merchants_) {
        file << "merchant_gold=" << merchant.resref << "|" << merchant.gold << "\n";
        for (std::size_t i = 0; i < merchant.stock.size(); ++i) {
            const MerchantStockItem& stock = merchant.stock[i];
            if (!stock.infinite) {
                file << "merchant=" << merchant.resref << "|" << i << "|"
                     << stock.remaining << "\n";
            }
        }
    }
    for (const InventoryItem& item : character_.inventory) {
        file << "item=" << item.resref << "|" << item.baseItem << "|" << item.cost << "|"
             << item.stack << "|" << item.slot << "|" << (item.equipped ? 1 : 0)
             << "|" << (item.identified ? 1 : 0) << "|" << item.object << "\n";
    }
    for (const auto& [name, value] : scriptWorld_.globals) {
        if (value.type == script::ValueType::Int) {
            file << "global_int=" << name << ":" << value.integer << "\n";
        } else if (value.type == script::ValueType::String) {
            file << "global_string=" << name << ":" << value.text << "\n";
        }
    }
    for (const auto& [id, object] : scriptWorld_.objects) {
        if (object.locals.empty()) {
            continue;
        }
        for (const auto& [name, value] : object.locals) {
            const char type = value.type == script::ValueType::String ? 's'
                : value.type == script::ValueType::Float                ? 'f'
                : value.type == script::ValueType::Object               ? 'o'
                                                                        : 'i';
            file << "local=" << object.tag << "|" << name << "|" << type << "|"
                 << (value.type == script::ValueType::String
                         ? value.text
                         : value.type == script::ValueType::Float
                         ? std::to_string(value.real)
                         : std::to_string(value.integer))
                 << "\n";
        }
    }
    writeWorldState(file);
    return static_cast<bool>(file);
}

bool GameSession::load(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string line;
    std::string module = module_;
    kotor::Vec3 position = position_;
    float yaw = yaw_;
    float pitch = pitch_;
    std::int32_t hp = playerHp_;
    CharacterSheet sheet = character_;
    std::int32_t gold = gold_;
    std::map<std::string, std::int32_t> journal;
    std::vector<std::string> party;
    std::vector<InventoryItem> inventory;
    std::map<std::string, script::Value> globals;
    struct MerchantDelta {
        std::string store;
        std::size_t index = 0;
        std::int32_t remaining = 0;
    };
    std::vector<MerchantDelta> merchantDeltas;
    std::vector<std::pair<std::string, std::int32_t>> merchantGolds;
    struct CompanionSave {
        std::string tag;
        std::string name;
        std::int32_t classId = 0;
        std::int32_t level = 1;
        std::int32_t xp = 0;
        std::int32_t hp = 1;
        std::int32_t maxHp = 1;
    };
    std::vector<CompanionSave> companionSaves;
    std::string leaderTag = "PC";
    struct LocalRecord {
        std::string tag;
        std::string name;
        char type = 'i';
        std::string value;
    };
    std::vector<LocalRecord> locals;
    bool schema2 = false;
    bool schema4 = false;
    LoadedWorldState worldState;
    PartyLoad partyLoad;
    while (std::getline(file, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "schema") {
                schema4 = value == "kotor.engine.player/4";
                schema2 = schema4 || value == "kotor.engine.player/2"
                    || value == "kotor.engine.player/3";
            } else if (key == "module") {
                module = value;
            } else if (key == "x") {
                position.x = std::stof(value);
            } else if (key == "y") {
                position.y = std::stof(value);
            } else if (key == "z") {
                position.z = std::stof(value);
            } else if (key == "yaw") {
                yaw = std::stof(value);
            } else if (key == "pitch") {
                pitch = std::stof(value);
            } else if (key == "hp") {
                hp = std::stoi(value);
            } else if (key == "name") {
                sheet.name = value;
            } else if (key == "class") {
                sheet.classId = std::stoi(value);
            } else if (key == "level") {
                sheet.level = std::stoi(value);
            } else if (key == "xp") {
                sheet.xp = std::stoi(value);
            } else if (key == "gold") {
                gold = std::stoi(value);
            } else if (key == "alignment") {
                sheet.alignment = std::clamp(std::stoi(value), 0, 100);
            } else if (key == "force_points") {
                sheet.fp = std::max(0, std::stoi(value));
            } else if (key == "max_force_points") {
                sheet.maxFp = std::max(0, std::stoi(value));
            } else if (key == "str") {
                sheet.str = std::stoi(value);
            } else if (key == "dex") {
                sheet.dex = std::stoi(value);
            } else if (key == "con") {
                sheet.con = std::stoi(value);
            } else if (key == "int") {
                sheet.intel = std::stoi(value);
            } else if (key == "wis") {
                sheet.wis = std::stoi(value);
            } else if (key == "cha") {
                sheet.cha = std::stoi(value);
            } else if (key == "feat_points") {
                sheet.featPoints = std::stoi(value);
            } else if (key == "skill_points") {
                sheet.skillPoints = std::stoi(value);
            } else if (key == "attribute_points") {
                sheet.attributePoints = std::max(0, std::stoi(value));
            } else if (key == "skill") {
                const auto parts = split(value, '|');
                if (parts.size() >= 2) {
                    sheet.skills[parts[0]] = std::stoi(parts[1]);
                }
            } else if (key == "feat") {
                sheet.feats.insert(std::stoi(value));
            } else if (key == "power") {
                sheet.powers.insert(std::stoi(value));
            } else if (key == "effect") {
                const auto parts = split(value, '|');
                if (parts.size() >= 9) {
                    kotor::ActiveEffect effect;
                    effect.spellId = std::stoi(parts[0]);
                    effect.label = parts[1];
                    effect.remaining = std::stof(parts[2]);
                    effect.armorBonus = std::stoi(parts[3]);
                    effect.attackBonus = std::stoi(parts[4]);
                    effect.damageBonus = std::stoi(parts[5]);
                    effect.shield = std::stoi(parts[6]);
                    effect.force = std::stoi(parts[7]) != 0;
                    effect.dispellable = std::stoi(parts[8]) != 0;
                    sheet.effects.push_back(std::move(effect));
                }
            } else if (key == "force_suppressed") {
                sheet.forceSuppressedTimer = std::stof(value);
            } else if (key == "journal") {
                const std::size_t colon = value.rfind(':');
                if (colon != std::string::npos) {
                    journal[value.substr(0, colon)] = std::stoi(value.substr(colon + 1));
                }
            } else if (key == "party") {
                party.push_back(value);
            } else if (key == "leader") {
                leaderTag = value;
            } else if (key == "companion") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 7) {
                    companionSaves.push_back({parts[0], parts[1], std::stoi(parts[2]),
                                              std::stoi(parts[3]), std::stoi(parts[4]),
                                              std::stoi(parts[5]), std::stoi(parts[6])});
                }
            } else if (key == "merchant") {
                const std::size_t first = value.find('|');
                const std::size_t second = value.find('|', first == std::string::npos
                                                              ? first
                                                              : first + 1);
                if (first != std::string::npos && second != std::string::npos) {
                    merchantDeltas.push_back({value.substr(0, first),
                                              static_cast<std::size_t>(
                                                  std::stoul(value.substr(first + 1,
                                                                          second - first - 1))),
                                              std::stoi(value.substr(second + 1))});
                }
            } else if (key == "merchant_gold") {
                const std::size_t bar = value.find('|');
                if (bar != std::string::npos) {
                    merchantGolds.emplace_back(value.substr(0, bar),
                                               std::stoi(value.substr(bar + 1)));
                }
            } else if (key == "item") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 6) {
                    InventoryItem item;
                    item.resref = parts[0];
                    item.baseItem = std::stoi(parts[1]);
                    item.cost = std::stoi(parts[2]);
                    item.stack = std::stoi(parts[3]);
                    item.slot = std::stoi(parts[4]);
                    item.equipped = std::stoi(parts[5]) != 0;
                    item.identified = parts.size() < 7 || std::stoi(parts[6]) != 0;
                    item.object = parts.size() >= 8
                        ? static_cast<std::uint32_t>(std::stoul(parts[7]))
                        : 0;
                    inventory.push_back(std::move(item));
                }
            } else if (key == "global_int") {
                const std::size_t colon = value.rfind(':');
                if (colon != std::string::npos) {
                    globals[value.substr(0, colon)] =
                        script::Value::makeInt(std::stoll(value.substr(colon + 1)));
                }
            } else if (key == "global_string") {
                const std::size_t colon = value.find(':');
                if (colon != std::string::npos) {
                    globals[value.substr(0, colon)] =
                        script::Value::makeString(value.substr(colon + 1));
                }
            } else if (key == "local") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 4) {
                    locals.push_back({parts[0], parts[1], parts[2][0], parts[3]});
                }
            } else if (!parsePartyLine(key, value, partyLoad)) {
                parseWorldStateLine(key, value, worldState);
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    // The saved party replaces the live one; the module load below sees an empty table.
    party_ = PartyTable{};
    companions_.clear();
    partyTags_.clear();
    if (schema4) {
        // The saved snapshots hold every object, so the world is rebuilt from them
        // even when the module is the one already loaded.
        scriptWorld_.globals.clear();
        locals.clear();
        if (!restoreWorldFromSave(std::move(worldState), module, position)) {
            return false;
        }
    } else {
        // An older save has no world state; nothing captured this session applies.
        sim_->snapshots.clear();
        if (module != module_) {
            if (!loadModule(module, &position, {})) {
                return false;
            }
        }
    }
    position_ = position;
    yaw_ = yaw;
    pitch_ = pitch;
    playerHp_ = std::max(1, hp);
    playerDead_ = false;
    if (schema2) {
        character_ = std::move(sheet);
        playerMaxHp_ = std::max<std::int32_t>(1, character_.maxHp);
        if (character_.maxHp <= 1 && rules_.loaded()) {
            character_.maxHp = rules_.maxHitPoints(character_.classId, character_.level,
                                                   character_.con);
            playerMaxHp_ = character_.maxHp;
        }
        playerHp_ = std::min(playerHp_, playerMaxHp_);
        gold_ = gold;
        journal_ = std::move(journal);
        selectedCompanion_ = -1;
        if (partyLoad.present) {
            restorePartyTable(std::move(partyLoad));
            party.clear();
            companionSaves.clear();
        }
        for (const std::string& tag : party) {
            addPartyMember(tag);
        }
        for (const CompanionSave& saved : companionSaves) {
            for (CompanionState& companion : companions_) {
                if (lower(companion.tag) != lower(saved.tag)) {
                    continue;
                }
                companion.sheet.name = saved.name;
                companion.sheet.classId = saved.classId;
                companion.sheet.level = saved.level;
                companion.sheet.xp = saved.xp;
                companion.sheet.hp = saved.hp;
                companion.sheet.maxHp = saved.maxHp;
            }
        }
        if (!partyLoad.present && leaderTag != "PC") {
            setPartyLeader(leaderTag);
        }
        syncPartyViews();
        for (const MerchantDelta& delta : merchantDeltas) {
            for (MerchantState& merchant : merchants_) {
                if (lower(merchant.resref) != lower(delta.store)) {
                    continue;
                }
                if (delta.index < merchant.stock.size()) {
                    merchant.stock[delta.index].infinite = false;
                    merchant.stock[delta.index].remaining = delta.remaining;
                }
            }
        }
        for (const auto& [store, value] : merchantGolds) {
            for (MerchantState& merchant : merchants_) {
                if (lower(merchant.resref) == lower(store)) {
                    merchant.gold = value;
                }
            }
        }
        character_.inventory = std::move(inventory);
        for (InventoryItem& item : character_.inventory) {
            if (!item.name.empty()) {
                continue;
            }
            auto cached = itemTemplates_.find(item.resref);
            if (cached == itemTemplates_.end()) {
                if (auto templ = kotor::loadItemTemplate(install_, item.resref);
                    templ.has_value()) {
                    cached = itemTemplates_.emplace(item.resref, *templ).first;
                }
            }
            if (cached != itemTemplates_.end()) {
                item.name = cached->second.name;
            }
        }
        for (const auto& [name, value] : globals) {
            scriptWorld_.globals[name] = value;
        }
        for (const LocalRecord& record : locals) {
            const std::uint32_t object = scriptWorld_.byTag(record.tag, 0);
            if (object == script::kObjectInvalid) {
                continue;
            }
            script::Value value;
            if (record.type == 's') {
                value = script::Value::makeString(record.value);
            } else if (record.type == 'f') {
                value = script::Value::makeFloat(std::stod(record.value));
            } else if (record.type == 'o') {
                value = script::Value::makeObject(
                    static_cast<std::uint32_t>(std::stoul(record.value)));
            } else {
                value = script::Value::makeInt(std::stoll(record.value));
            }
            scriptWorld_.setLocal(object, record.name, value);
        }
        syncPlayerToScriptWorld();
    }
    return true;
}

bool GameSession::loadOriginalSave(const std::filesystem::path& folder)
{
    kotor::OriginalSave saved;
    try {
        saved = kotor::readOriginalSave(folder, rsvRoot_);
    } catch (const std::exception& exception) {
        logEvent(std::string("original save read failed: ") + exception.what());
        return false;
    }
    if (saved.module.empty()) {
        logEvent("original save has no module");
        return false;
    }
    // SAVEGAME.sav mapping for per-object state is a later wave; what this
    // session captured does not describe the saved game.
    sim_->snapshots.clear();
    party_ = PartyTable{};
    companions_.clear();
    partyTags_.clear();
    sim_->restoringSave = true;
    try {
        if (!loadModule(saved.module, nullptr, {})) {
            sim_->restoringSave = false;
            return false;
        }
    } catch (const std::exception& exception) {
        sim_->restoringSave = false;
        logEvent(std::string("original save module failed: ") + exception.what());
        return false;
    }
    sim_->restoringSave = false;
    if (!saved.area.empty() && lower(saved.area) != lower(scene_.area)) {
        logEvent("original save area differs from module entry: " + saved.area);
    }
    character_.name = saved.pcName.empty() ? "Player" : saved.pcName;
    character_.classId = saved.classId;
    character_.level = std::max(1, saved.level);
    character_.xp = saved.xp;
    character_.alignment = std::clamp(saved.alignment, 0, 100);
    character_.fp = saved.forcePoints;
    character_.maxFp = std::max(saved.maxForcePoints, saved.forcePoints);
    character_.str = saved.str;
    character_.dex = saved.dex;
    character_.con = saved.con;
    character_.intel = saved.intel;
    character_.wis = saved.wis;
    character_.cha = saved.cha;
    character_.hp = std::max(1, saved.hp);
    character_.maxHp = std::max(character_.hp, saved.maxHp);
    character_.feats.clear();
    for (const std::int32_t feat : saved.feats) {
        character_.feats.insert(feat);
    }
    character_.powers.clear();
    for (const std::int32_t power : saved.powers) {
        character_.powers.insert(power);
    }
    character_.effects = saved.effects;
    character_.forceSuppressedTimer = saved.forceSuppressedTimer;
    character_.inventory.clear();
    for (const kotor::SavedInventoryItem& source : saved.inventory) {
        InventoryItem item;
        item.resref = source.resref;
        item.name = source.name;
        item.baseItem = source.baseItem;
        item.cost = source.cost;
        item.stack = std::max(1, source.stack);
        item.slot = source.slot;
        item.equipped = source.equipped;
        item.identified = source.identified;
        character_.inventory.push_back(std::move(item));
    }
    gold_ = saved.gold;
    journal_ = saved.journal;
    playerHp_ = character_.hp;
    playerMaxHp_ = character_.maxHp;
    playerDead_ = false;

    scriptWorld_.globals.clear();
    globalKinds_.clear();
    locationGlobals_.clear();
    for (const kotor::SavedGlobal& value : saved.globals) {
        if (value.type == kotor::SavedGlobal::Type::String) {
            scriptWorld_.globals[value.name] = script::Value::makeString(value.text);
            globalKinds_[value.name] = "string";
        } else if (value.type == kotor::SavedGlobal::Type::Boolean) {
            scriptWorld_.globals[value.name] = script::Value::makeInt(value.boolean ? 1 : 0);
            globalKinds_[value.name] = "boolean";
        } else if (value.type == kotor::SavedGlobal::Type::Location) {
            locationGlobals_[value.name] = value.location;
            globalKinds_[value.name] = "location";
        } else {
            scriptWorld_.globals[value.name] = script::Value::makeInt(value.number);
            globalKinds_[value.name] = "number";
        }
    }

    selectedCompanion_ = -1;
    readPartyFromOriginalSave(saved);

    std::vector<bool> objectUsed(scene_.entities.size(), false);
    for (const kotor::SavedObjectState& state : saved.objects) {
        const kotor::EntityKind kind = entityKindForSaved(state.kind);
        std::size_t index = static_cast<std::size_t>(-1);
        float bestDistance = 1.0e30f;
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& candidate = scene_.entities[i];
            if (objectUsed[i] || candidate.tag != state.tag
                || candidate.partyAvatar
                || (kind != kotor::EntityKind::Unknown && candidate.kind != kind)) {
                continue;
            }
            const float dx = candidate.position.x - state.position.x;
            const float dy = candidate.position.y - state.position.y;
            const float dz = candidate.position.z - state.position.z;
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                index = i;
            }
        }
        if (index == static_cast<std::size_t>(-1)) {
            continue;
        }
        objectUsed[index] = true;
        kotor::Entity& entity = scene_.entities[index];
        entity.position = state.position;
        entity.bearing = state.bearing;
        entity.hitPoints = state.hitPoints;
        entity.maxHitPoints = std::max(1, state.maxHitPoints);
        entity.dead = state.dead;
        entity.open = state.open;
        entity.locked = state.locked;
        if (entity.kind == kotor::EntityKind::Placeable) {
            entity.lootItems = state.loot;
        }
    }

    originalAreaName_ = saved.areaName;
    originalPortrait_ = saved.portrait;
    originalTimePlayed_ = saved.timePlayed;
    originalRepute_ = saved.repute;
    if (!originalRepute_.empty()) {
        std::string factionError;
        if (!factions_.loadFac(originalRepute_, factionError)) {
            logEvent("REPUTE: " + factionError);
        } else {
            refreshHostility();
        }
    }
    originalOuterExtras_ = saved.preservedOuter;
    originalPreservedModules_ = saved.preservedModules;
    position_ = saved.position;
    yaw_ = saved.yaw;
    float ground = position_.z;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    activeDialogue_.reset();
    dialogueView_ = {};
    combatTarget_ = UINT32_MAX;
    syncScriptWorld();
    logEvent("original save loaded: " + saved.module + " gold=" + std::to_string(gold_)
             + " inventory=" + std::to_string(character_.inventory.size())
             + " journal=" + std::to_string(journal_.size())
             + " globals=" + std::to_string(scriptWorld_.globals.size()));
    return true;
}

bool GameSession::saveOriginalSave(const std::filesystem::path& folder,
                                   const std::string& saveName)
{
    kotor::OriginalSave save;
    save.folder = folder;
    save.module = module_;
    save.area = scene_.area;
    save.areaName = originalAreaName_.empty()
        ? (scene_.areaTag.empty() ? scene_.area : scene_.areaTag)
        : originalAreaName_;
    save.saveName = saveName;
    save.pcName = character_.name;
    save.portrait = originalPortrait_.empty() ? "po_pmhc4" : originalPortrait_;
    save.position = position_;
    save.yaw = yaw_;
    save.gold = gold_;
    save.xp = character_.xp;
    save.hp = playerHp_;
    save.maxHp = std::max(playerHp_, playerMaxHp_);
    save.level = character_.level;
    save.classId = character_.classId;
    save.alignment = character_.alignment;
    save.forcePoints = character_.fp;
    save.maxForcePoints = character_.maxFp;
    save.str = character_.str;
    save.dex = character_.dex;
    save.con = character_.con;
    save.intel = character_.intel;
    save.wis = character_.wis;
    save.cha = character_.cha;
    save.playedSeconds = originalTimePlayed_ + static_cast<std::int32_t>(elapsed_);
    save.timePlayed = save.playedSeconds;
    save.repute = factions_.loaded() ? factions_.saveFac() : originalRepute_;
    save.preservedOuter = originalOuterExtras_;
    save.preservedModules = originalPreservedModules_;
    for (const std::int32_t feat : character_.feats) {
        save.feats.push_back(feat);
    }
    for (const std::int32_t power : character_.powers) {
        save.powers.push_back(power);
    }
    for (std::int32_t i = 0; i < 8; ++i) {
        const std::string name = rules_.skillName(i);
        const auto it = character_.skills.find(name);
        save.skills.push_back(it != character_.skills.end() ? it->second : 0);
    }
    save.effects = character_.effects;
    save.forceSuppressedTimer = character_.forceSuppressedTimer;
    for (const InventoryItem& item : character_.inventory) {
        kotor::SavedInventoryItem copy;
        copy.resref = item.resref;
        copy.name = item.name;
        copy.baseItem = item.baseItem;
        copy.cost = item.cost;
        copy.stack = item.stack;
        copy.slot = item.slot;
        copy.equipped = item.equipped;
        copy.identified = item.identified;
        save.inventory.push_back(std::move(copy));
    }
    writePartyToOriginalSave(save);
    save.journal = journal_;
    for (const auto& [name, value] : scriptWorld_.globals) {
        kotor::SavedGlobal global;
        global.name = name;
        const auto kind = globalKinds_.find(name);
        if (value.type == script::ValueType::String) {
            global.type = kotor::SavedGlobal::Type::String;
            global.text = value.text;
        } else if (kind != globalKinds_.end() && kind->second == "boolean") {
            global.type = kotor::SavedGlobal::Type::Boolean;
            global.boolean = value.integer != 0;
        } else {
            global.type = kotor::SavedGlobal::Type::Number;
            global.number = static_cast<std::int32_t>(value.integer);
        }
        save.globals.push_back(std::move(global));
    }
    for (const auto& [name, location] : locationGlobals_) {
        kotor::SavedGlobal global;
        global.type = kotor::SavedGlobal::Type::Location;
        global.name = name;
        global.location = location;
        save.globals.push_back(std::move(global));
    }
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.tag.empty() || entity.partyAvatar) {
            continue;
        }
        kotor::SavedObjectState state;
        switch (entity.kind) {
        case kotor::EntityKind::Creature: state.kind = kotor::SavedObjectState::Kind::Creature; break;
        case kotor::EntityKind::Door: state.kind = kotor::SavedObjectState::Kind::Door; break;
        case kotor::EntityKind::Placeable: state.kind = kotor::SavedObjectState::Kind::Placeable; break;
        case kotor::EntityKind::Trigger: state.kind = kotor::SavedObjectState::Kind::Trigger; break;
        case kotor::EntityKind::Store: state.kind = kotor::SavedObjectState::Kind::Store; break;
        case kotor::EntityKind::Encounter: state.kind = kotor::SavedObjectState::Kind::Encounter; break;
        case kotor::EntityKind::Waypoint: state.kind = kotor::SavedObjectState::Kind::Waypoint; break;
        case kotor::EntityKind::Sound: state.kind = kotor::SavedObjectState::Kind::Sound; break;
        default: state.kind = kotor::SavedObjectState::Kind::Other; break;
        }
        state.tag = entity.tag;
        state.templateResref = entity.templateResref;
        state.position = entity.position;
        state.bearing = entity.bearing;
        state.hitPoints = entity.hitPoints;
        state.maxHitPoints = entity.maxHitPoints;
        state.dead = entity.dead;
        state.open = entity.open;
        state.locked = entity.locked;
        state.loot = entity.lootItems;
        save.objects.push_back(std::move(state));
    }
    try {
        if (!kotor::writeOriginalSave(folder, save, install_, true)) {
            logEvent("original save write refused: " + folder.string());
            return false;
        }
    } catch (const std::exception& exception) {
        logEvent(std::string("original save write failed: ") + exception.what());
        return false;
    }
    logEvent("original save written: " + folder.string());
    return true;
}

} // namespace ke
