#include "game/GameSession.h"
#include "game/Ambience.h"
#include "game/Pathfinding.h"
#include "game/StateTrace.h"

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
    case UiPanel::Equipment: return "equipment";
    case UiPanel::Abilities: return "abilities";
    case UiPanel::Messages: return "messages";
    case UiPanel::Map: return "map";
    case UiPanel::LevelUp: return "levelup";
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
    // SetStealthState 00613930: entering stealth plays sound-set enum 0x15 (slot 20).
    if (effects_) {
        effects_->speak(controlledEntity(), 20, EffectDirector::VoiceKind::Direct);
    }
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
    if (entityIndex < scene_.entities.size() && scene_.entities[entityIndex].kind == kotor::EntityKind::Door) {
        logInfo(concat("door ", scene_.entities[entityIndex].tag, " plays ", name, " at ", elapsed_));
    }
    const float transition = mesh->model->animationTransition(name);
    if (!mesh->animation.empty() && mesh->animation != name && transition > 0.0f) {
        mesh->previousAnimation = mesh->animation;
        mesh->previousAnimationTime = mesh->animationTime;
        mesh->previousAnimationLoop = mesh->animationLoop;
        mesh->blendDuration = transition;
        mesh->blendRemaining = transition;
    } else {
        mesh->previousAnimation.clear();
        mesh->blendDuration = 0.0f;
        mesh->blendRemaining = 0.0f;
    }
    mesh->animation = name;
    mesh->animationTime = 0.0f;
    mesh->animationRate = 1.0f;
    mesh->animationLoop = loop;
    kotor::refreshEntityMesh(*mesh);
    ++animationRevision_;
    return true;
}

void GameSession::applyObjectStateClip(std::size_t entityIndex)
{
    kotor::EntityMesh* mesh = findEntityMesh(scene_, entityIndex);
    if (mesh == nullptr || !mesh->model || entityIndex >= scene_.entities.size()) {
        return;
    }
    kotor::ObjectClip clip = kotor::objectStateClip(scene_.entities[entityIndex], *mesh->model);
    if (!clip.name.empty()) {
        mesh->animation = std::move(clip.name);
        mesh->animationTime = clip.time;
        mesh->animationLoop = clip.loop;
        mesh->previousAnimation.clear();
        mesh->blendRemaining = 0.0f;
        kotor::refreshEntityMesh(*mesh);
        ++animationRevision_;
    }
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

std::uint32_t GameSession::effectsTopologyRevision() const
{
    return effects_ ? effects_->topologyRevision() : 0;
}

void GameSession::updateAnimations(float dt)
{
    if (!animationEnabled_) {
        // No pose is built, but a door's opening clip still runs on the logical clock: its done event commits the
        // open state (retail 00683f00 -> 00589680), which the walkers wait for, so a headless run follows the
        // same timeline as a drawn one.
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.model && !mesh.fx && !mesh.animationLoop && mesh.animation.rfind("opening", 0) == 0) {
                mesh.animationTime = std::min(mesh.animationTime + dt, mesh.model->animationDuration(mesh.animation));
            }
        }
        return;
    }
    updateCreatureStances(dt);
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
        if (effects_ && !mesh.attachModel && mesh.entity < scene_.entities.size()) {
            effects_->creatureClip(mesh.entity, mesh.animation);
        }
        const bool dangly = mesh.model->danglyNodeCount() > 0;
        if (mesh.animation.empty() && !dangly && mesh.face.weight <= 0.0f) {
            continue;
        }
        const float duration = mesh.model->animationDuration(mesh.animation);
        const float clipBefore = mesh.animationTime;
        if (!mesh.animation.empty() && duration > 0.0f) {
            mesh.animationTime += dt * mesh.animationRate;
            if (mesh.referenceHolder && !mesh.referenceClip.empty()) {
                const float holderLength = mesh.referenceHolder->animationDuration(mesh.referenceClip);
                mesh.referenceTime = holderLength > 0.0f ? std::fmod(mesh.referenceTime + dt, holderLength) : 0.0f;
            }
            if (mesh.animationLoop) {
                mesh.animationTime = std::fmod(mesh.animationTime, duration);
            } else {
                mesh.animationTime = std::min(mesh.animationTime, duration);
            }
        }
        // A room the camera cannot see needs no new pose this frame.
        if (mesh.room >= 0 && static_cast<std::size_t>(mesh.room) < scene_.visibleRooms.size()
            && scene_.visibleRooms[static_cast<std::size_t>(mesh.room)] == 0u) {
            continue;
        }
        if (mesh.clipPlaced && mesh.entity < scene_.entities.size()) {
            scene_.entities[mesh.entity].lightOffset = mesh.center;
        }
        if (effects_ && !mesh.attachModel && !mesh.animation.empty() && duration > 0.0f
            && mesh.entity < scene_.entities.size()) {
            effects_->animationAdvanced(mesh.entity, mesh.model, mesh.animation, clipBefore,
                                        mesh.animationTime, duration, mesh.animationLoop);
        }
        if (dangly) {
            mesh.danglyTime += dt;
        }
        if (mesh.blendRemaining > 0.0f && !mesh.previousAnimation.empty()) {
            const float previousDuration = mesh.model->animationDuration(mesh.previousAnimation);
            if (previousDuration > 0.0f) {
                mesh.previousAnimationTime += dt;
                // The clip blended from keeps its own looping: a finished die must not wrap back
                // to its standing first frame while dead blends in.
                if (mesh.previousAnimationLoop) {
                    mesh.previousAnimationTime = std::fmod(mesh.previousAnimationTime,
                                                           previousDuration);
                } else {
                    mesh.previousAnimationTime = std::min(mesh.previousAnimationTime,
                                                          previousDuration);
                }
            }
        }
        // Retail Gob::Animate 00486670 moves the entry's blend weight on by dt / blend time (and past 1.0
        // drops the clip under it) before AnimateHierarchy poses the model with that weight, so the
        // blend advances ahead of the pose, as the clip time does.
        if (mesh.blendRemaining > 0.0f) {
            mesh.blendRemaining = std::max(0.0f, mesh.blendRemaining - dt);
            if (mesh.blendRemaining <= 0.0f) {
                mesh.previousAnimation.clear();
            }
        }
        kotor::refreshEntityMesh(mesh);
        if (mesh.scriptLoop) {
            kotor::poseRoomScriptLights(scene_, mesh);
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
            const bool blending = parent->blendRemaining > 0.0f && !parent->previousAnimation.empty()
                && parent->blendDuration > 0.0f;
            mesh.attachPrevious = blending ? parent->previousAnimation : std::string{};
            mesh.attachPreviousTime = parent->previousAnimationTime;
            mesh.attachBlend = blending ? 1.0f - parent->blendRemaining / parent->blendDuration : 1.0f;
            mesh.attachHidden = hidesEquippedItems(parent->animation);
            mesh.attachHeadTurn = parent->headTurn; // a head item follows the head (kotor::attachHeadItem)
        }
        if (mesh.entity < scene_.entities.size()) {
            // A saber's swing trail (kotor::SaberTrail) ages by the frame and keeps world places.
            const kotor::Entity& owner = scene_.entities[mesh.entity];
            mesh.trailPlaced = true;
            mesh.trailPosition = owner.position;
            mesh.trailBearing = owner.drawBearing();
            mesh.trailDt = dt;
            mesh.trailStep = true;
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
        if (mesh.roomAnimated) {
            continue; // Plays its own clip from load.
        }
        if (mesh.entity < scene_.entities.size() && !mesh.attachModel) {
            kotor::ObjectClip clip = kotor::objectStateClip(scene_.entities[mesh.entity], *mesh.model);
            if (!clip.name.empty()) {
                mesh.animation = std::move(clip.name);
                mesh.animationTime = clip.time;
                mesh.animationLoop = clip.loop;
                continue;
            }
        }
        // A placeable with no idle plays its first looping clip.
        for (const char* name : {"pause1", "pause", "cpause1", "default", "animloop01", "animloop1"}) {
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
        // The default avatar is a portraits.2da PC row; its Sex is the PC's gender.
        if (const std::int32_t row = rules_.portraitRowForAppearance(
                static_cast<std::int32_t>(playerAppearance_));
            row >= 0) {
            character_.portraitRow = row;
            character_.gender = rules_.portraitRow(row).sex;
        }
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
    audio_->setObstructionProbe([this](kotor::Vec3 source, kotor::Vec3 listener) {
        return soundObstruction(source, listener);
    });
    effects_ = std::make_unique<EffectDirector>(*this);
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
    runtime_->setEngineEquals([this](const script::Value& a, const script::Value& b) { return engineValuesEqual(a, b); });
    loadGlobalCatalog();
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
    // Retail 004b3d10 (MovePlayerToArea) moves the player only when CSWSModule::GetWaypoint finds the tag; with no
    // waypoint the player stays where 004b3a90 (InitiateModuleForPlayer) put it, the IFO's Mod_Entry point, and no
    // error is raised (manm28aa has no from26c: k_pman_to28a still sends the party there).
    logInfo(concat("module link target ", linkedTo, " not found in ", module_, ": staying at the module entry"));
}

void GameSession::startNewGame(const std::string& module)
{
    journal_.clear();
    scriptWorld_.globals.clear();
    locationGlobals_.clear();
    sim_->snapshots.clear();
    sim_->carried.clear();
    sim_->pendingLinkedTo.clear();
    sim_->pendingMovies.clear();
    if (activeDialogue_) {
        videoEffects_.disable();
    }
    activeDialogue_.reset();
    dialogueView_ = {};
    newGameStart_ = true;
    pendingModule_ = module;
}

bool GameSession::loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor)
{
    const std::string previousModule = module_;
    // Leave hooks and the snapshot run first: loading the next module rebinds
    // the install, after which the old module's scripts can no longer be found.
    if (runtime_ != nullptr && !sim_->restoringSave && !previousModule.empty() && !newGameStart_) {
        leaveModule();
    }
    if (newGameStart_) {
        newGameStart_ = false;
        sim_->snapshots.clear();
        sim_->carried.clear();
        sim_->events.clear();
    }
    sim_->miniGame.reset();
    // Retail clears the global fade when the next area loads: 0062ac40 resets the fade panel and takes
    // it off the screen, called from the transition steps at 0058c1d3 and 0058f317 (beside the area
    // load calls 004aecd0 / 004aed30). The swoop race's heartbeat fades out, then StartNewModule
    // (tar_m03af), and nothing in tar_m03af fades back in.
    sim_->fade = FadeState{};
    // Retail 005f8290 (CClientExoAppInternal::UnloadModule) clears both pause states and the Enemy Sighted flag
    // (+0x324) and the autopause bit (+900 bit 0), then CGuiInGame::ShutDown (0062c310) deletes every in-game
    // panel, the status summary (+0xa8) with the rows it had gathered: a pause or popup does not carry over.
    enemySighted_ = false;
    autoPauseRequest_ = 0;
    statusOpen_ = false;
    statusPending_ = {};
    statusPopupOwed_ = false;
    arrivalPending_ = true;
    kotor::Scene loaded = kotor::loadModule(install_, module, savedGit_);
    module_ = module;
    scene_ = std::move(loaded);
    if (effects_) {
        effects_->resetScene();
    }
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
    playerBearing_ = yaw_; // Retail enters with the PC's back to the camera (frame spn).
    float ground = 0.0f;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    if (activeDialogue_) {
        videoEffects_.disable(); // A dialogue's effect must not outlive the module it ran in.
    }
    activeDialogue_.reset();
    dialogueView_ = {};
    sim_->dialoguePaused = false;
    ++dialogueGeneration_;
    combatTarget_ = UINT32_MAX;
    selectedEntity_ = UINT32_MAX;
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
        updateAudioListener();
        audio_->setListenerRoom(roomNameAt(controlledPosition()));
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
    const kotor::Vec3 arrival = controlledPosition();
    for (std::size_t i = 0; i < scene_.triggers.size() && i < triggerInside_.size(); ++i) {
        const std::size_t entity = scene_.triggers[i].entity;
        if (entity < scene_.entities.size() && !scene_.entities[entity].linkedModule.empty()
            && pointInTrigger(scene_.triggers[i], arrival.x, arrival.y)) {
            triggerInside_[i] = true;
        }
    }
    // A creature is created standing on the walkmesh: the GIT Z can sit above the floor (Taris tar_m02aa twilek is 3.13 in the
    // GIT and measured at floor height 0 on retail from the first row). Module load 00504a70 puts each creature at
    // ComputeSafeLocation 004be860 of its GIT position (20 m reach, line test on), one after the other.
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Creature || entity.partyAvatar) {
            continue;
        }
        kotor::Vec3 safe;
        float floorZ = 0.0f;
        if (computeSafeLocation(entity.position, 20.0f, entity.appearance, true, i, safe)) {
            entity.position = safe;
        } else if (floorHeight(scene_, entity.position.x, entity.position.y, entity.position.z + 2.0f, floorZ)) {
            entity.position.z = floorZ;
        }
    }
    initAnimations();
    if (stateTrace_ != nullptr) {
        stateTrace_->onModuleReady(*this);
    }
    return true;
}

bool GameSession::floorBelow(float x, float y, float maxZ, float& height) const
{
    return floorHeight(scene_, x, y, maxZ, height);
}

float GameSession::floorHeightAt(float x, float y) const
{
    float ground = 0.0f;
    const float reference = controlledPosition().z;
    if (floorHeight(scene_, x, y, reference + 2.0f, ground)) {
        return ground;
    }
    return reference;
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
    runHook(hook, index, entering == 0 ? controlledObject() : entering);
    return true;
}

// retail 0052c510: a location compares position and orientation (0052bf80), each float by
// float (004aa980), and a talent field by field (004cabb0). Effects (two dwords at the head of
// the retail value) and events (004d7270) have no field this engine's records map to, so they
// compare by identity, which a copy keeps.
bool GameSession::engineValuesEqual(const script::Value& a, const script::Value& b) const
{
    if (a.integer == b.integer) {
        return true;
    }
    const EngineStore& store = sim_->engine;
    if (const LocationData* x = store.get<LocationData>(a.integer); x != nullptr) {
        const LocationData* y = store.get<LocationData>(b.integer);
        return y != nullptr && x->position.x == y->position.x && x->position.y == y->position.y
            && x->position.z == y->position.z && x->facing == y->facing;
    }
    if (const TalentData* x = store.get<TalentData>(a.integer); x != nullptr) {
        const TalentData* y = store.get<TalentData>(b.integer);
        return y != nullptr && x->type == y->type && x->id == y->id;
    }
    // Measured on retail: two EventUserDefined(1) compare equal, 1 and 2 do not. Two effects made
    // alike do not, and stay out of this.
    if (const EventData* x = store.get<EventData>(a.integer); x != nullptr) {
        const EventData* y = store.get<EventData>(b.integer);
        return y != nullptr && x->hook == y->hook && x->context == y->context;
    }
    return false;
}

// Which globals exist is shipped data: retail keeps only what globalcat.2da declares
// (00529030), each with one type.
void GameSession::loadGlobalCatalog()
{
    auto data = install_.find("globalcat", 2017);
    if (!data.has_value()) {
        return;
    }
    const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "globalcat.2da");
    const int nameColumn = table.columnIndex("name");
    const int typeColumn = table.columnIndex("type");
    if (nameColumn < 0 || typeColumn < 0) {
        return;
    }
    std::map<std::string, script::GlobalKind, script::NoCaseLess> catalog;
    for (std::size_t row = 0; row < table.rowCount(); ++row) {
        const std::string name = table.cell(row, static_cast<std::size_t>(nameColumn));
        std::string type = table.cell(row, static_cast<std::size_t>(typeColumn));
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name.empty()) {
            continue;
        }
        catalog[name] = type == "boolean" ? script::GlobalKind::Boolean
            : type == "number"            ? script::GlobalKind::Number
            : type == "string"            ? script::GlobalKind::String
                                          : script::GlobalKind::Location;
    }
    runtime_->setGlobalCatalog(std::move(catalog));
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

void GameSession::declareGlobalForTest(const std::string& name)
{
    if (runtime_ != nullptr) {
        runtime_->declareGlobalForTest(name);
    }
}

void GameSession::addScriptForTest(const std::string& resref, std::vector<std::uint8_t> bytes)
{
    sim_->injectedScripts[resref] = std::move(bytes);
    sim_->decoded.erase(resref);
}

script::ProgramPtr GameSession::decodedScript(const std::string& resref)
{
    if (const auto found = sim_->decoded.find(resref); found != sim_->decoded.end()) {
        return found->second;
    }
    const std::string bytes = scriptSource(resref);
    if (bytes.empty()) {
        return nullptr;
    }
    try {
        script::ProgramPtr decoded = script::makeProgram(script::decodeNcs(
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
    script::Vm vm(std::move(instructions), runtime_.get(), script::kRetailInstructionBudget);
    vm.setScriptName(label);
    vm.setLiveObjectConstants(actualSelf);
    vm.keepObjectIdsForTest();
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
    ScriptContext context = functor.context;
    if (target != 0 && target != script::kObjectInvalid) {
        context["self"] = target;
        context["speaker"] = target;
    }
    const auto selfEntry = context.find("self");
    const std::uint32_t self = selfEntry != context.end() ? selfEntry->second : scriptWorld_.pc;
    ScriptContext saved = runtime_->swapContext(std::move(context));
    ++sim_->scriptDepth;
    // A closure is an outermost run with counters of its own (005d4ad0).
    script::Vm vm = script::Vm::shared(functor.program, runtime_.get(), script::kRetailInstructionBudget);
    vm.setEntry(functor.ip, functor.stack, functor.bp);
    vm.setScriptName(functor.script);
    vm.setLiveObjectConstants(self);
    const script::VmResult result = vm.run();
    --sim_->scriptDepth;
    runtime_->setContext(std::move(saved));
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
    // Retail UpdateState 004b0b70 runs due events in time order until the head is later than
    // the time it read, so a command posted with no delay by one of them runs in this pass.
    // The cap only stops a script that keeps posting itself from hanging the frame.
    scriptWorld_.advanceClock(dt);
    int ranThisPass = 0;
    while (ranThisPass++ < 4096) {
        const std::optional<script::Timer> next = scriptWorld_.popDue();
        if (!next.has_value()) {
            break;
        }
        const script::Timer& timer = *next;
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
    // Retail runs events from a queue, so the only script inside a script is ExecuteScript,
    // and it refuses a ninth nested run (005d2260). This engine also runs hooks and dialogue
    // scripts synchronously, which retail sees as separate outermost runs: those start fresh
    // counters here and answer only to the recursion guard.
    constexpr int kMaxScriptDepth = 32;
    const bool nested = std::exchange(sim_->executeNested, false);
    if (runtime_ == nullptr || resref.empty()) {
        return std::nullopt;
    }
    if (sim_->scriptDepth >= kMaxScriptDepth) {
        scriptLog_.push_back("script " + resref + " refused: nested too deep");
        return std::nullopt;
    }
    if (nested && sim_->retailRuns >= script::kRetailNestedRuns) {
        scriptLog_.push_back("script " + resref + " refused: nested run limit");
        return std::nullopt;
    }
    const auto instructions = decodedScript(resref);
    if (instructions == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t actualSelf = self == 0 ? scriptWorld_.pc : self;
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
    ScriptContext saved = runtime_->swapContext(std::move(merged));
    ++sim_->scriptDepth;
    const script::VmCounters outerCounters = sim_->vmCounters;
    const int outerRuns = sim_->retailRuns;
    if (!nested) {
        sim_->vmCounters = {};
        sim_->retailRuns = 0;
    }
    ++sim_->retailRuns;
    script::Vm vm = script::Vm::shared(instructions, runtime_.get(), script::kRetailInstructionBudget);
    vm.shareCounters(&sim_->vmCounters);
    vm.setScriptName(resref);
    vm.setLiveObjectConstants(actualSelf);
    const script::VmResult result = vm.run();
    if (nested) {
        --sim_->retailRuns;
    } else {
        sim_->vmCounters = outerCounters;
        sim_->retailRuns = outerRuns;
    }
    --sim_->scriptDepth;
    runtime_->setContext(std::move(saved));
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
        line << " error=" << result.error << " ip=" << result.errorIp;
    }
    if (!result.error.empty()) {
        line << " error=" << result.error;
    }
    scriptLog_.push_back(line.str());
    if (scriptLog_.size() > 8192) {
        scriptLog_.erase(scriptLog_.begin(), scriptLog_.begin() + 4096);
    }
    syncScriptWorld();
    // Only a clean run ending on exactly one int returns anything; a void or failed
    // conditional reads as false (005d45d0, 0059ec90).
    if (result.returned.has_value()) {
        return script::Value::makeInt(*result.returned);
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
    // A chosen portrait row decides the body (appearance.2da) by class, as retail
    // portcust does (006f90f0); the row's Sex is the avatar's gender.
    if (sheet.portraitRow >= 0 && rules_.loaded()) {
        const std::int32_t appearance =
            rules_.portraitAppearance(sheet.portraitRow, sheet.classId);
        if (appearance >= 0 && static_cast<std::uint32_t>(appearance) != playerAppearance_) {
            setPlayerAppearance(static_cast<std::uint32_t>(appearance));
        }
    }
    syncPlayerToScriptWorld();
    if (thirdPerson_) {
        spawnPlayerAvatar();
    }
    if (playerEntity_ != static_cast<std::size_t>(-1) && playerEntity_ < scene_.entities.size()) {
        scene_.entities[playerEntity_].stats.gender = character_.gender;
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

std::vector<std::size_t> GameSession::mapPins() const
{
    std::vector<std::size_t> pins;
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        const kotor::Entity& entity = scene_.entities[index];
        if (entity.kind != kotor::EntityKind::Waypoint || !entity.hasMapNote) {
            continue;
        }
        std::string tag = entity.tag;
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entity.mapNoteEnabled || sim_->mapPinsEnabled.contains(tag)) {
            if (mapPointSeen(entity.position)) // retail 00692c70: 00579210
                pins.push_back(index);
        }
    }
    return pins;
}

kotor::BodyVariant GameSession::wornBodyVariant()
{
    for (const InventoryItem& item : character_.inventory) {
        if (item.equipped && item.slot == 1) {
            return kotor::armorBodyVariant(install_, item.resref);
        }
    }
    return {};
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
                                             playerName_, position_, playerBearing_, wornBodyVariant());
    refreshHeldItems(scriptWorld_.pc); // The new body holds what the PC wears.
    if (playerEntity_ != static_cast<std::size_t>(-1)) {
        scene_.entities[playerEntity_].stats.gender = character_.gender;
        // The player's sound set: the class-select creatures the chargen builds carry soundset.2da row
        // 85 (Player_Male_W) for a male and 83 (Player_Female_W) for a female (table at 007a2684,
        // SetSoundSet 0060b7c0 called from 006dc9cf); nothing else sets the player's SoundSetFile.
        if (audio_) {
            const std::shared_ptr<const kotor::TwoDa> soundSets = install_.table("soundset");
            if (soundSets != nullptr) {
                const std::string row = character_.gender == 1 ? "83" : "85";
                if (soundSets->rowIndex(row) >= 0) {
                    scene_.entities[playerEntity_].soundSet = soundSets->cell(row, "resref");
                    audio_->setCreatureSoundSet(static_cast<std::uint32_t>(playerEntity_),
                                                scene_.entities[playerEntity_].soundSet);
                }
            }
        }
        syncScriptWorld();
        initAnimations();
    }
}

static float cameraHookHeight(const kotor::Scene& scene, std::size_t entity);
// CClientExoAppInternal::UpdateSoundEngine 005f5370: the listener is not the creature. Its position is
// the camera's position moved 0.75 of the way (007a22a0, clamped to 0..1) towards the followed creature
// (the vector sum at 005f54f3 / 005f55a7), and its orientation is the camera's.
void GameSession::updateAudioListener()
{
    if (!audio_) {
        return;
    }
    const SceneCamera eye = camera();
    if (miniGameActive()) {
        // 005f5370 in a minigame (GetMiniGame with a player): the minigame camera is neither the
        // chase camera (0x106a) nor 0x1070, so nothing pulls the listener towards a creature; it
        // stays at the camera with the camera's orientation.
        const float flat = std::hypot(eye.forward.x, eye.forward.y);
        audio_->setListener(eye.eye, flat > 1.0e-4f ? std::atan2(eye.forward.y, eye.forward.x) : yaw_);
        return;
    }
    kotor::Vec3 target = controlledPosition();
    if (thirdPerson_ && playerVisible_ && !dialogueView_.active) {
        // CSWCameraOnAStick::GetTargetHeadPosition 005f5180 (called at 005f5478): the followed
        // creature's position raised by +0x6c, its camera hook height (SetCameraFollow 0063f9d0); the
        // camerastyle HEIGHT is the eye's own lift (+0x120) and is not part of it.
        target.z += cameraHookHeight(scene_, controlledEntity());
    }
    constexpr float kFollowed = 0.75f;
    const kotor::Vec3 at{eye.eye.x + (target.x - eye.eye.x) * kFollowed,
                         eye.eye.y + (target.y - eye.eye.y) * kFollowed,
                         eye.eye.z + (target.z - eye.eye.z) * kFollowed};
    const float planar = std::hypot(eye.forward.x, eye.forward.y);
    audio_->setListener(at, planar > 1.0e-4f ? std::atan2(eye.forward.y, eye.forward.x) : yaw_);
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
    // The camera orbits the controlled creature (the PC's avatar, or the companion's).
    const std::size_t body = controlledEntity();
    // The chase camera's casts read the creatures' appearance spaces, and the world moves each tick.
    loadMovementTables();
    chaseEyeValid_ = false;
    // Retail never hides the followed creature for a close camera: CSWCameraOnAStick::Control 0063bcb0 places the
    // camera (0063b050 hit check, then the owner's SetPosition/SetOrientation at 0063bf48/0063bf80) and hides
    // nothing, and 0063b050 only moves the eye. 45 recorded loads and runs keep the followed Gob visible with the eye
    // as near as 0.72 m (Endar).
    const bool visible = true;
    if (visible != playerVisible_) {
        playerVisible_ = visible;
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == body && body != static_cast<std::size_t>(-1)) {
                mesh.visible = visible;
            }
        }
        ++animationRevision_;
    }
    (void)moving; // The stance tick reads the movement off the creature's position.
}

bool GameSession::entityHasFacialPose(std::size_t entityIndex) const
{
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity == entityIndex && mesh.face.weight > 0.0f) {
            return true;
        }
    }
    return false;
}

// The line's speaker wears the track: the Speaker field's object, or the stand-in a cutscene
// conversation shows in its place.
void GameSession::updateFacialMeshes()
{
    const std::size_t none = static_cast<std::size_t>(-1);
    std::size_t speaker = none;
    std::size_t stand = none;
    kotor::MdlFacePose face;
    if (dialogueView_.active && activeLip_.has_value()) {
        speaker = entityOfObject(dialogueView_.speakerObject);
        if (speaker == none && activeDialogue_.has_value()) {
            speaker = entityOfObject(activeDialogue_->npcObject);
        }
        stand = speaker != none ? dialogueStuntFor(speaker) : none;
        face = FacialRig::facePose(*activeLip_, lipTime_);
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.attachModel) {
            continue;
        }
        const kotor::MdlFacePose next = mesh.entity == speaker || mesh.entity == stand
            ? face : kotor::MdlFacePose{};
        const bool was = mesh.face.weight > 0.0f;
        if (!was && next.weight <= 0.0f) {
            continue;
        }
        mesh.face = next;
        ++animationRevision_;
        if (next.weight <= 0.0f) {
            kotor::refreshEntityMesh(mesh); // The last pose held the mouth; nothing else repaints it.
        }
    }
}

static float cameraHookHeight(const kotor::Scene& scene, std::size_t entity);

bool GameSession::appearanceRows()
{
    if (!appearanceHeads_.has_value()) {
        auto data = install_.find("appearance", 2017);
        if (!data.has_value()) {
            return false;
        }
        appearanceHeads_ = kotor::TwoDa::parse(std::move(*data), "appearance.2da");
    }
    return true;
}

// Retail 006306e0, for the speaker and for the listener. A creature whose orientation is locked
// (SetLockOrientationInDialog) is left alone. One with appearance.2da headtrack 1 turns its body
// only as far as keeps the other within head_arc_h plus a degree, and its head follows the other's
// camerahook out to 10 m (006146e0). Any other creature, or one whose head is locked
// (SetLockHeadFollowInDialog), faces the other fully. Read back at each line of tar02_preraid: the
// Bith and the Sith officer (headtrack 0) face their listener exactly, the PC stays 2.2 degrees off.
// Retail hands the body turn to the creature's desired orientation (0060b3c0); doing it at once is
// an engine choice.
void GameSession::faceDialogueParticipants()
{
    headFollow_.clear();
    const std::size_t none = static_cast<std::size_t>(-1);
    const std::size_t speaker = bodyOfObject(dialogueView_.speakerObject); // the PC is the avatar entity
    const std::size_t listener = bodyOfObject(dialogueView_.listenerObject);
    if (speaker == none || listener == none || speaker == listener) {
        logInfo(concat("dialogue facing: skipped, speaker ", speaker == none ? -1 : static_cast<int>(speaker), " listener ",
                       listener == none ? -1 : static_cast<int>(listener)));
        return;
    }
    if (!appearanceRows()) {
        return;
    }
    const kotor::TwoDa& table = *appearanceHeads_;
    const auto cellOf = [&](const kotor::Entity& entity, const char* column) {
        return table.cell(static_cast<std::size_t>(entity.appearance),
                          static_cast<std::size_t>(table.columnIndex(column)));
    };
    const auto degrees = [&](const kotor::Entity& entity, const char* column, float fallback) {
        const std::string text = cellOf(entity, column);
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        return (end != text.c_str() ? value : fallback) * kPi / 180.0f;
    };
    for (const auto& [self, other] : {std::pair{speaker, listener}, std::pair{listener, speaker}}) {
        kotor::Entity& entity = scene_.entities[self];
        const kotor::Entity& target = scene_.entities[other];
        const std::uint32_t object = objectOfBody(self);
        if (entity.kind != kotor::EntityKind::Creature || entity.dead
            || sim_->lockOrientation.contains(object)) {
            continue;
        }
        float& bearing = object == scriptWorld_.pc ? playerBearing_ : entity.bearing;
        const float to = std::atan2(target.position.y - entity.position.y,
                                    target.position.x - entity.position.x);
        logInfo(concat("dialogue facing: ", "[track ", cellOf(entity, "headtrack"), " lock ", headFollowLocked(object), "] ", entity.tag.empty() ? std::string("PC") : entity.tag, " bearing ", bearing * 180.0f / kPi,
                       " to ", to * 180.0f / kPi, " arc ", degrees(entity, "head_arc_h", 40.0f) * 180.0f / kPi, " at ", entity.position.x, ",", entity.position.y,
                       " other ", target.position.x, ",", target.position.y));
        if (headFollowLocked(object) || cellOf(entity, "headtrack") != "1") {
            bearing = to;
            continue;
        }
        const float arcH = degrees(entity, "head_arc_h", 40.0f); // 0060e250, 0060e2a0
        const float off = std::remainder(to - bearing, 2.0f * kPi);
        if (std::fabs(off) > arcH) {
            bearing = to - std::copysign(arcH + kPi / 180.0f, off);
        }
        headFollow_.push_back({self, other, arcH, degrees(entity, "head_arc_v", 30.0f)});
    }
}

// Retail's controller (vtable 00743290, tick 00481dc0) clamps the look to its arcs, drops it past
// the range and blends with a time of 0.25; in ours the blend is a simple approach (inferred). The
// angles are in model space, against the creature's body; retail works in hturn_g's parent's.
void GameSession::tickHeadFollow(float dt)
{
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.attachModel) {
            continue;
        }
        float yaw = 0.0f;
        float pitch = 0.0f;
        for (const HeadFollow& follow : headFollow_) {
            if (follow.entity != mesh.entity || follow.entity >= scene_.entities.size()
                || follow.target >= scene_.entities.size()) {
                continue;
            }
            const kotor::Entity& self = scene_.entities[follow.entity];
            const kotor::Entity& other = scene_.entities[follow.target];
            const kotor::Vec3 to{other.position.x - self.position.x,
                                 other.position.y - self.position.y,
                                 other.position.z + cameraHookHeight(scene_, follow.target)
                                     - self.position.z - cameraHookHeight(scene_, follow.entity)};
            if (kotor::length(to) > 10.0f) {
                continue;
            }
            const float body = std::atan2(to.y, to.x) - self.bearing;
            yaw = std::clamp(std::remainder(body, 2.0f * kPi), -follow.arcH, follow.arcH);
            pitch = std::clamp(std::atan2(to.z, std::hypot(to.x, to.y)), -follow.arcV, follow.arcV);
        }
        if (yaw == 0.0f && pitch == 0.0f && mesh.headYaw == 0.0f && mesh.headPitch == 0.0f) {
            continue;
        }
        const float step = std::min(1.0f, dt / 0.25f);
        mesh.headYaw += (yaw - mesh.headYaw) * step;
        mesh.headPitch += (pitch - mesh.headPitch) * step;
        if (yaw == 0.0f && pitch == 0.0f && std::fabs(mesh.headYaw) < 0.002f
            && std::fabs(mesh.headPitch) < 0.002f) {
            mesh.headYaw = 0.0f;
            mesh.headPitch = 0.0f;
        }
        const float cy = std::cos(mesh.headYaw);
        const float sy = std::sin(mesh.headYaw);
        const float cp = std::cos(mesh.headPitch);
        const float sp = std::sin(mesh.headPitch);
        mesh.headTurn.m[0][0] = cy;
        mesh.headTurn.m[0][1] = -sy * cp;
        mesh.headTurn.m[0][2] = sy * sp;
        mesh.headTurn.m[1][0] = sy;
        mesh.headTurn.m[1][1] = cy * cp;
        mesh.headTurn.m[1][2] = -cy * sp;
        mesh.headTurn.m[2][0] = 0.0f;
        mesh.headTurn.m[2][1] = sp;
        mesh.headTurn.m[2][2] = cp;
        ++animationRevision_;
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

bool GameSession::openNearestStore()
{
    const kotor::Vec3 at = controlledPosition();
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 0.0f;
    for (const MerchantState& merchant : merchants_) {
        if (merchant.entity >= scene_.entities.size()) {
            continue;
        }
        const kotor::Vec3& p = scene_.entities[merchant.entity].position;
        const float distance = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y);
        if (best == static_cast<std::size_t>(-1) || distance < bestDistance) {
            best = merchant.entity;
            bestDistance = distance;
        }
    }
    return best != static_cast<std::size_t>(-1) && openStore(best);
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
        const kotor::EncounterTemplate& data = volume.templateData;
        state.respawnsRemaining = data.respawns < 0
            ? -1
            : std::max(0, data.respawns - data.currentSpawns); // AIUpdate 00593fb0: a respawn needs CurrentSpawns < Respawns
        state.resetTime = static_cast<float>(std::max(0, data.resetTime));
        // Counters of a saved encounter (ReadEncounterFromGff 00592430). SpawnIfAppropriate 00593be0 spawns only while
        // Started is not 1; the wave's creatures are the AreaList objects. Exhausted (+0x27c) is written by
        // SaveEncounter 00591350 but no spawn code reads it, so it is not restored.
        state.spawnedCount = data.numberSpawned;
        state.triggered = data.started;
        state.spawned = data.started;
        for (const std::uint32_t id : data.areaObjects) {
            for (std::size_t e = 0; e < scene_.entities.size(); ++e) {
                if (id != 0 && scene_.entities[e].savedObjectId == id) {
                    state.entities.push_back(e);
                    break;
                }
            }
        }
        // 00593fb0 wakes a reset encounter once LastSpawnDay and LastSpawnTime are both set and ResetTime has passed.
        state.wiped = !data.active && data.lastSpawnDay != 0 && data.lastSpawnTime != 0;
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
    // Solo mode: nobody follows. Retail stores the flag (PT_SOLOMODE) and clears a
    // per-creature state bit on every party member when it is switched off
    // (retail 00547ce0 -> 00565500 -> 00563c60); what the bit does was not
    // recovered, so "followers stop following while solo is on" is an engine rule.
    if (party_.solo || companions_.empty() || controlledDead()) {
        return;
    }
    // Every creature that is not the controlled one follows it through the same
    // follow code: the members, and the PC when a companion is steered.
    const std::uint32_t leader = controlledObject();
    const kotor::Vec3 anchor = controlledPosition();
    if (followHold_) {
        if (std::fabs(anchor.x - followHoldAnchor_.x) + std::fabs(anchor.y - followHoldAnchor_.y) < 0.01f) {
            return;
        }
        followHold_ = false;
    }
    const float anchorYaw = yaw_;
    ScriptApi api{*this};
    int slot = 0;
    const auto follow = [&](std::uint32_t self) {
        // A creature fights through its queue and the shipped AI; following is for
        // when it has nothing to do.
        const ActorState& actor = api.actor(self);
        if (actor.queue.empty() && !actor.combat.inCombat) {
            followLeader(self, anchor, formationSlot(anchor, anchorYaw, slot, 1.6f), dt);
        }
        ++slot;
    };
    if (leader != scriptWorld_.pc && !playerDead_) {
        follow(scriptWorld_.pc);
    }
    for (CompanionState& companion : companions_) {
        const std::size_t index = findEntity(companion.tag);
        if (index == static_cast<std::size_t>(-1) || scene_.entities[index].dead) {
            continue;
        }
        const std::uint32_t self = objectForEntity(index);
        if (self == leader) {
            continue;
        }
        follow(self);
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
    sim_->areaUnescapable = scene_.unescapable; // A snapshot, when returning, overrides it below.
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
    createMiniGame(ScriptApi{*this});
    if (returning) {
        refreshHostility();
        if (!sim_->restoringSave) {
            runReturnScripts();
        }
        startMiniGame(ScriptApi{*this});
        return;
    }
    initMerchants();
    runSpawnAndEnterScripts();
    if (sim_->restoringSave) {
        // loadOriginalSave finishes the start once the save's state is in place.
        saveModuleStartPending_ = true;
        return;
    }
    spawnEncounters();
    // Retail's client loads the MiniGame after the server's module-load scripts have run:
    // k_pebo_mgload zeroes the fighter count that the fighters' OnCreate then counts up.
    startMiniGame(ScriptApi{*this});
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
    if (!sim_->restoringSave) {
        runSceneHook("Mod_OnModLoad", scriptWorld_.pc);
        runSceneHook("Mod_OnModStart", scriptWorld_.pc);
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (sim_->restoringSave && entity.kind == kotor::EntityKind::Creature && entity.spawnFired) {
            continue; // 00500350 / 004eb6e0: the saved spawned flag is set, OnSpawn does not run again
        }
        const auto spawn = entity.scripts.find("ScriptSpawn");
        if (spawn != entity.scripts.end() && !spawn->second.empty()) {
            if (spawnRan_.insert(objectForEntity(i)).second) {
                runScript(spawn->second, objectForEntity(i), scriptWorld_.pc,
                          scriptWorld_.pc, scriptWorld_.pc);
            }
        }
    }
    if (sim_->restoringSave) {
        primeLoadedCreatureClocks(ScriptApi{*this}); // saved creatures are already spawned: 004eb6e0
    }
    if (!sim_->restoringSave) { // loadOriginalSave runs the module and entry hooks last.
        runSceneHook("Mod_OnClientEntr", scriptWorld_.pc);
        runSceneHook("OnEnter", scriptWorld_.pc);
    }
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
    return startMovie(name, true);
}

bool GameSession::startMovie(const std::string& name, bool skippable)
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
    movieSkippable_ = skippable;
    movieTime_ = 0.0f;
    movieDuration_ = static_cast<float>(
        static_cast<double>(moviePlayer_->frameCount()) / moviePlayer_->framesPerSecond());
    moviePlaying_ = true;
    if (audio_) {
        audio_->setMovieMode(true);
    }
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
        track->samples = moviePlayer_->takeAudioSamples();
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
    if (audio_) {
        audio_->setMovieMode(false);
    }
    moviePlayer_.reset();
    movieFrame_ = {};
    movieFrameIndex_ = -1;
    movieTime_ = 0.0f;
    movieDuration_ = 0.0f;
    movieName_.clear();
}

// QueueMovie 00548210 only queues; the second argument is bSkippable (nwscript 769), not a
// "play now" request.
bool GameSession::queueMovie(const std::string& name, bool skippable)
{
    if (name.empty()) {
        return false;
    }
    movieQueue_.push_back({name, skippable});
    return true;
}

// Plays the first queued movie that opens; a missing file is skipped, as an empty slot is.
bool GameSession::playMovieQueue(bool allowSeparateSkips)
{
    movieSeparateSkips_ = allowSeparateSkips;
    while (!movieQueue_.empty()) {
        const QueuedMovie next = movieQueue_.front();
        movieQueue_.erase(movieQueue_.begin());
        if (startMovie(next.name, next.skippable)) {
            return true;
        }
    }
    return false;
}

// Clears the playing movie (frame, audio, player) and keeps the queue.
void GameSession::endCurrentMovie()
{
    if (audio_ && movieAudioVoice_ != 0) {
        audio_->mixer().stopVoice(movieAudioVoice_);
    }
    movieAudioVoice_ = 0;
    movieAudioData_.reset();
    moviePlaying_ = false;
    if (audio_) {
        audio_->setMovieMode(false);
    }
    moviePlayer_.reset();
    movieFrame_ = {};
    movieFrameIndex_ = -1;
}

// The documented Escape rule (nwscript 769/770): only a skippable movie can be cancelled;
// without separate skips the whole queue goes with it.
bool GameSession::skipMovie()
{
    if (!moviePlaying_ || !movieSkippable_) {
        return false;
    }
    if (!movieSeparateSkips_) {
        movieQueue_.clear();
    }
    endCurrentMovie();
    if (!movieQueue_.empty()) {
        playMovieQueue(movieSeparateSkips_);
    }
    return true;
}

bool GameSession::playStartupMovies()
{
    movieQueue_.clear();
    for (const char* name : {"leclogo", "biologo", "legal"}) {
        queueMovie(name, true);
    }
    return playMovieQueue(true);
}

void GameSession::advanceMovie(float dt)
{
    if (!moviePlaying_ || !moviePlayer_) {
        return;
    }
    dt = std::min(dt, 0.25f);
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
        movieTime_ = movieDuration_;
        endCurrentMovie();
        if (!movieQueue_.empty()) {
            playMovieQueue(movieSeparateSkips_);
        }
    }
}

PazaakCard GameSession::pazaakCardOfKind(int kind)
{
    return {std::clamp(kind, 0, 17), 0};
}

int GameSession::pazaakCardsOwned(int kind) const
{
    if (kind < 0 || kind > 17) {
        return 0;
    }
    return std::max(0, party_.pazaakCards[static_cast<std::size_t>(kind)]);
}

// AddPazaakCard 004efb40: the count of kind (ModelVariation + 11) % 18 grows by the stack; the first card
// brings the sideboard item (g_I_PazSidebd001, base item 87) when the party has none.
bool GameSession::absorbPazaakCard(const std::string& resref, std::int32_t baseItem, std::int32_t stack)
{
    if (baseItem != 86 || stack <= 0) {
        return false;
    }
    const kotor::ItemTemplate* templ = upgradeTemplate(resref);
    const int variation = templ != nullptr ? templ->modelVariation : 0;
    const int kind = ((variation + 11) % 18 + 18) % 18;
    party_.pazaakCards[static_cast<std::size_t>(kind)] += stack;
    const bool hasSideboard = std::any_of(character_.inventory.begin(), character_.inventory.end(),
                                          [](const InventoryItem& item) { return item.baseItem == 87; });
    if (!hasSideboard) {
        const kotor::ItemTemplate* board = upgradeTemplate("g_i_pazsidebd001");
        addInventoryItem("g_I_PazSidebd001", 87, 1, board != nullptr ? board->cost : 0);
    }
    return true;
}

// PlayPazaak (action 364) -> StartPazaakGame 005f3810 -> the start panel 00681a90.
bool GameSession::startPazaak(std::int32_t opponentDeck, std::string endScript,
                              std::int32_t maxWager, bool tutorial,
                              std::uint32_t opponent)
{
    if (pazaakStage_ != PazaakStage::None) {
        return false; // StartPazaakGame does nothing while a game is up (+0x70)
    }
    ++pazaakGames_;
    const std::uint32_t seed = 0x9E3779B9u ^ (static_cast<std::uint32_t>(opponentDeck) * 2654435761u) ^ opponent
        ^ (static_cast<std::uint32_t>(pazaakGames_) * 40503u);
    // LoadInitialSideDeck 006e4a40: pazaakdecks.2da row nDeck (a random row when negative), the first ten
    // cells: the second character is the digit; a first '*' is the +/- card, '-' a minus card.
    std::vector<PazaakCard> opponentSide;
    if (auto data = install_.find("pazaakdecks", kotor::resType(kotor::ResType::TwoDa)); data.has_value()) {
        try {
            const kotor::TwoDa decks = kotor::TwoDa::parse(std::move(*data), "pazaakdecks");
            std::size_t row = static_cast<std::size_t>(std::max(0, opponentDeck));
            if (opponentDeck < 0 && decks.rowCount() > 0) {
                row = static_cast<std::size_t>((seed >> 3) % decks.rowCount());
            }
            for (std::size_t column = 0; column < 10 && column < decks.columnCount() && row < decks.rowCount(); ++column) {
                const std::string cell = decks.cell(row, column);
                const char digit = cell.size() > 1 ? cell[1] : '1';
                int kind = digit - '1';
                if (!cell.empty() && cell[0] == '*') {
                    kind = digit - '%';
                } else if (!cell.empty() && cell[0] == '-') {
                    kind = digit - '+';
                }
                opponentSide.push_back({kind, 0});
            }
        } catch (const kotor::FormatError&) {
        }
    }
    if (opponentSide.empty()) {
        opponentSide = PazaakGame::defaultSide();
    }
    pazaakOpponentSide_ = std::move(opponentSide);
    pazaakSeed_ = seed;
    pazaak_.reset();
    pazaakEndScript_ = std::move(endScript);
    pazaakOpponent_ = opponent;
    pazaakDeckIndex_ = opponentDeck;
    pazaakTutorial_ = tutorial;
    // The start panel's help (00681a90 passes bShowTutorial to its CSWGuiTutorial at +0x7f8c): its
    // OnPanelAdded 0067f750 shows 0x96e3..0x96e5 after putting the wager popup up, so over it.
    pazaakSetupHelp_ = {};
    pazaakSetupHelp_.enable(tutorial);
    pazaakSetupHelp_.showPages({0x96e3, 0x96e4, 0x96e5});
    lastPazaakResult_ = 0; // +0x80 = 0
    // The wager is cut to the gold held when one is asked; the popup opens at it.
    pazaakMaxWager_ = maxWager > 0 ? std::min<std::int32_t>(maxWager, gold_) : maxWager;
    pazaakWager_ = pazaakMaxWager_;
    pazaakWagerChosen_ = pazaakMaxWager_;
    // OnPanelAdded 0067f750: the side list kept in PARTYTABLE is chosen again, each card only while one
    // is owned.
    pazaakChosen_.clear();
    std::array<int, 18> left{};
    for (int kind = 0; kind < 18; ++kind) {
        left[static_cast<std::size_t>(kind)] = pazaakCardsOwned(kind);
    }
    for (const std::int32_t kind : party_.pazaakSideList) {
        if (kind >= 0 && kind < 18 && left[static_cast<std::size_t>(kind)] > 0) {
            --left[static_cast<std::size_t>(kind)];
            pazaakChosen_.push_back(kind);
        }
    }
    pazaakStage_ = pazaakMaxWager_ > 0 ? PazaakStage::Wager : PazaakStage::Setup;
    return true;
}

int GameSession::pazaakMaxWager() const
{
    return std::max(0, pazaakMaxWager_);
}

// The wager popup's Less and More (0067e150): one at a time, 1 to the maximum.
void GameSession::setPazaakWager(int wager)
{
    if (pazaakStage_ == PazaakStage::Wager) {
        pazaakWagerChosen_ = std::clamp(wager, 1, std::max(1, pazaakMaxWager()));
    }
}

// The Wager button: HandleWagerExit 0067d3b0 stores the wager; the side deck screen is next.
void GameSession::confirmPazaakWager()
{
    if (pazaakStage_ != PazaakStage::Wager) {
        return;
    }
    pazaakWager_ = pazaakWagerChosen_;
    pazaakStage_ = PazaakStage::Setup;
}

bool GameSession::pazaakChoose(int kind)
{
    if (pazaakStage_ != PazaakStage::Setup || pazaakChosen_.size() >= kPazaakDeckSize || kind < 0 || kind > 17) {
        return false;
    }
    const int already = static_cast<int>(std::count(pazaakChosen_.begin(), pazaakChosen_.end(), kind));
    if (already >= pazaakCardsOwned(kind)) {
        return false;
    }
    pazaakChosen_.push_back(kind);
    return true;
}

bool GameSession::pazaakUnchoose(std::size_t slot)
{
    if (pazaakStage_ != PazaakStage::Setup || slot >= pazaakChosen_.size()) {
        return false;
    }
    pazaakChosen_.erase(pazaakChosen_.begin() + static_cast<std::ptrdiff_t>(slot));
    return true;
}

// OK on "use this side deck?" (HandleStartDialog 00681890): only a full ten (ShowDisabledButtons 0067e400
// keeps Play off otherwise); the ten go to PARTYTABLE, four are dealt, the table opens.
bool GameSession::pazaakPlay()
{
    if (pazaakStage_ != PazaakStage::Setup || pazaakChosen_.size() != kPazaakDeckSize) {
        return false;
    }
    std::vector<PazaakCard> side;
    for (std::size_t i = 0; i < kPazaakDeckSize; ++i) {
        party_.pazaakSideList[i] = pazaakChosen_[i];
        side.push_back(pazaakCardOfKind(pazaakChosen_[i]));
    }
    pazaak_ = std::make_unique<PazaakGame>();
    pazaakSetupHelp_.close();
    pazaak_->reset(pazaakSeed_, side, pazaakOpponentSide_, pazaakTutorial_);
    pazaakStage_ = PazaakStage::Game;
    return true;
}

void GameSession::stopPazaak()
{
    pazaak_.reset();
    pazaakSetupHelp_ = {};
    pazaakEndScript_.clear();
    pazaakWager_ = 0;
    pazaakStage_ = PazaakStage::None;
}

// EndPazaakGame 005f3950 (bPlayAgain is 0 on every path the panels take).
void GameSession::endPazaakGame(bool won, std::int32_t wager)
{
    if (pazaakStage_ == PazaakStage::None) {
        return;
    }
    lastPazaakResult_ = won ? 1 : 0;
    if (wager > 0) {
        const std::int32_t before = gold_;
        const std::int32_t after = won ? before + wager : std::max(0, before - wager);
        if (after != before) {
            gold_ = after;
            character_.gold = gold_;
            statusFeedback(1, after - before); // UpdateStatus(1, delta), no feedback line
        }
    }
    const std::string endScript = pazaakEndScript_;
    stopPazaak();
    if (!endScript.empty()) {
        runScript(endScript, script::kObjectInvalid); // RunScript(script, 0x7f000000)
    }
}

void GameSession::resolvePazaakIfOver()
{
    // DoGameSequence state 12 (0067e9a0, 0067ea10): the match is over or End Tutorial was chosen; the
    // last help box closes first.
    if (!pazaak_ || pazaak_->helpOpen()
        || (pazaak_->phase() != PazaakGame::Phase::MatchOver && !pazaak_->endDue())) {
        return;
    }
    endPazaakGame(pazaak_->playerWonMatch(), pazaakWager_);
}

std::string GameSession::pazaakOpponentName() const
{
    if (pazaakOpponent_ == 0 || pazaakOpponent_ == script::kObjectInvalid) {
        return {};
    }
    const std::size_t index = bodyOfObject(pazaakOpponent_);
    if (index >= scene_.entities.size() || scene_.entities[index].kind != kotor::EntityKind::Creature) {
        return {};
    }
    return scene_.entities[index].name;
}

// 0067d3d0: the wager and side deck screens; nothing was wagered yet.
void GameSession::quitPazaak()
{
    if (pazaakStage_ == PazaakStage::Wager || pazaakStage_ == PazaakStage::Setup) {
        endPazaakGame(false, 0);
    }
}

// 0067db50: at the table, a loss of the whole wager.
void GameSession::forfeitPazaak()
{
    if (pazaakStage_ == PazaakStage::Game) {
        endPazaakGame(false, pazaakWager_);
    }
}

bool GameSession::miniGameActive() const
{
    return sim_->miniGame != nullptr;
}

void GameSession::miniGameSetInput(const MiniGameInput& input)
{
    if (sim_->miniGame) {
        sim_->miniGame->input = input;
    }
}

MiniGameView GameSession::miniGameView() const
{
    if (!sim_->miniGame) {
        return {};
    }
    MiniGameView view = sim_->miniGame->view();
    view.endGamePending = sim_->ending.pending || sim_->ending.ended;
    view.endGameGui = sim_->ending.showGui;
    return view;
}

MiniGameWorld* GameSession::miniGame()
{
    return sim_->miniGame.get();
}

const MiniGameWorld* GameSession::miniGame() const
{
    return sim_->miniGame.get();
}

void GameSession::setMiniGameSeed(std::uint32_t seed)
{
    sim_->miniGameSeed = seed;
    if (sim_->miniGame) {
        sim_->miniGame->reseed(seed);
    }
}

void GameSession::setMiniGameMeshesHidden(bool hidden)
{
    if (sim_->miniGame) {
        sim_->miniGame->setMeshesHidden(hidden);
        bakeMiniGame();
    }
}

void GameSession::bakeMiniGame()
{
    if (sim_->miniGame && sim_->miniGame->bake(scene_) != 0) {
        ++animationRevision_;
    }
}

void GameSession::tickWhilePaused(float realDt)
{
    if (realDt <= 0.0f || gameEnded()) {
        return;
    }
    tickTempDeathRecovery(ScriptApi{*this}, realDt);
    advanceFade(ScriptApi{*this}, realDt);
    // Object light fades (004a3f70) take the game's frame time: a pause holds them (RETAIL row 40).
    scene_.lightFades = true;
    scene_.lightFadeStep = 0.0f;
    ++scene_.lightFrame;
    // The client keeps playing clips in a pause: the retail get-up capture (tar_m02af, Carth revived by
    // TempDeathUpdate in a paused game) shows him already pushing up on his arms at the burst's first
    // frames (PAUSED label up) and the PC's combat idle moving, so the clip a revive starts runs on.
    updateAnimations(std::min(realDt, 0.08f));
    runPendingModule();
}

void GameSession::update(float dt, const InputState& rawInput)
{
    // retail 00602eb0 has torn the world down; nothing ticks until a new game loads.
    if (dt <= 0.0f || gameEnded()) {
        return;
    }
    if (movieHoldsWorld_ && moviePlaying_) {
        advanceMovie(dt);
        return;
    }
    // A minigame's player follows its track: walking input never reaches the avatar.
    const InputState idleInput{};
    const InputState& input = miniGameActive() ? idleInput : rawInput;
    // The voice plays in real time, so the track it belongs to must not lose a long frame.
    const float voiceDt = dt;
    dt = std::min(dt, 0.08f);
    elapsed_ += dt;
    // Object light fades (004a3f70): 3 x frame time per frame (007a1940 = 3.0, 0078e574).
    scene_.lightFades = true;
    scene_.lightFadeStep = 3.0f * dt;
    ++scene_.lightFrame;
    if (activeLip_.has_value() && dialogueView_.active) {
        lipTime_ = std::min(lipTime_ + voiceDt, activeLip_->duration + FacialRig::kFadeSeconds);
        facialRig_.setViseme(lipViseme());
    } else {
        facialRig_.setViseme(0);
    }
    updateFacialMeshes();
    tickHeadFollow(dt);
    advanceMovie(dt);
    tickDialogueAdvance(dt);
    updateAnimations(dt);
    // Retail 004b4e80: each party member uncovers the map one cell around.
    markMapAt(position(), 1);
    for (const std::uint32_t member : scriptWorld_.party) {
        const std::size_t index = ScriptApi{*this}.entityFor(member);
        if (index < scene_.entities.size()) {
            markMapAt(scene_.entities[index].position, 1);
        }
    }
    tickEnemySighted(dt);
    tickPendingUse();
    if (effects_) {
        effects_->update(dt);
    }
    scene_.clock += dt;
    kotor::updateProceduralTextures(scene_, dt);
    if (!dialogueCameraClip_.empty()) {
        dialogueCameraTime_ = std::min(dialogueCameraTime_ + dt, dialogueCameraDuration_);
    }
    yaw_ += input.mouseDx * 0.0026f;
    pitch_ = clamp(pitch_ - input.mouseDy * 0.0022f, -1.35f, 1.35f);

    // The input steers the controlled creature. A companion leads through
    // stepControlledCompanion (PartyControl.cpp); the PC's own body keeps position_
    // and is moved by the follow code like any member.
    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    const bool companionLead = companionControlled();
    float magnitude = 0.0f;
    bool queueWalked = false;
    if (companionLead) {
        stepControlledCompanion(dt, input);
        queueWalked = nav_ && nav_->playerStepped;
        if (nav_) {
            nav_->playerStepped = false;
        }
        updatePlayerAvatar(dt, queueWalked);
    } else {
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
    const float cosYaw = std::cos(yaw_);
    const float sinYaw = std::sin(yaw_);
    kotor::Vec3 desired{cosYaw * forwardAxis + sinYaw * rightAxis,
                        sinYaw * forwardAxis - cosYaw * rightAxis, 0.0f};
    magnitude = std::sqrt(desired.x * desired.x + desired.y * desired.y);
    // retail 006227e0 ProcessInput: steering the controlled party member with input of at least
    // 0.25 (0x73edf0) sets its combat message to 42476; once the input drops, a 42476 message goes
    // back to 48208. The creature flag it also tests (+0x440 bit 0) is taken as "in combat": inferred.
    {
        const bool steering = magnitude >= 0.25f && pcActor.combat.inCombat;
        if (steering && !steeringMessage_) {
            raiseCombatMessage(42476);
        } else if (!steering && steeringMessage_) {
            raiseCombatMessage(48208);
        }
        steeringMessage_ = steering;
    }
    if (magnitude > 1.0e-5f) {
        desired = desired * (1.0f / magnitude);
    }
    // The drive controller (retail 00679940): the input is a direction of unit length, half of it with Shift held
    // (InputState::run false), and the body's velocity follows it with the appearance's DRIVEACCL / DRIVEMAXSPEED.
    // A conversation, a lock or a dead player holds the controls, and the velocity with them.
    const bool driveHeld = playerDead_ || dialogueView_.active || !pcActor.commandable || controlLocked();
    if (driveHeld) {
        driveStop(scriptWorld_.pc);
    }
    const kotor::Vec3 delta = driveHeld
        ? kotor::Vec3{0.0f, 0.0f, 0.0f}
        : driveStep(scriptWorld_.pc, desired.x, desired.y, magnitude > 1.0e-5f ? (input.run ? 1.0f : 0.5f) : 0.0f, dt);
    const bool driving = delta.x * delta.x + delta.y * delta.y > 1.0e-12f;

    const kotor::Vec3 start = position_;
    bool steered = false;
    const kotor::Vec3 candidate = driving
        ? slidePlayer(start, {position_.x + delta.x, position_.y + delta.y, position_.z}, steered)
        : start;
    if (driving && dt > 0.0f
        && (std::fabs(candidate.x - (start.x + delta.x)) > 1.0e-4f || std::fabs(candidate.y - (start.y + delta.y)) > 1.0e-4f)) {
        // The walkmesh kept the body from some of the move: it keeps the speed it really made.
        driveSetVelocity(scriptWorld_.pc, (candidate.x - start.x) / dt, (candidate.y - start.y) / dt);
    }
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
    queueWalked = nav_ && nav_->playerStepped;
    if (nav_) {
        nav_->playerStepped = false;
    }
    updatePlayerAvatar(dt, driving || queueWalked);
    }

    // Retail has no footstep timer: a step sounds only from the snd_Footstep event of the playing clip
    // (0061a970, EffectDirector::fireEvent), so nothing is played here.
    footstepTimer_ = 0.0f;
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
    if (!moviePlaying_) {
        tickMiniGame(ScriptApi{*this}, dt);
    }
    updateTimers(dt);
    tickEvents(ScriptApi{*this});
    tickActors(ScriptApi{*this}, dt);
    tickWorldState(ScriptApi{*this}, dt);
    tickTempDeathRecovery(ScriptApi{*this}, dt);
    updatePartyWipe(dt);
    updateScripts(dt);
    updateEncounters(dt);
    syncPlayerToScriptWorld();
    if (audio_) {
        updateAudioListener();
        audio_->setListenerRoom(roomNameAt(controlledPosition()));
        audio_->updateObstruction();
        tickAmbience(*this, dt);
    }
    if (scriptLog_.size() > 8192) {
        scriptLog_.erase(scriptLog_.begin(), scriptLog_.end() - 4096);
    }
    if (arrivalPending_) {
        arrivalPending_ = false;
        completeArrival();
    }
    runPendingModule();
}

// Retail 004babb0: the server loop takes a pending StartNewModule (+0x10080, set by 004aecc0) every frame with no
// pause test, so a paused game (autopause box, status summary, party selection, pause key) still changes module.
void GameSession::runPendingModule()
{
    if (!pendingModule_.empty()) {
        const std::string next = pendingModule_;
        const std::string linkedTo = sim_->pendingLinkedTo;
        pendingModule_.clear();
        sim_->pendingLinkedTo.clear();
        if (moduleLoadHook_) {
            moduleLoadHook_(next);
        }
        try {
            loadModule(next, nullptr, linkedTo);
        } catch (const std::exception& exception) {
            logEvent("module transition failed: " + next + ": " + exception.what());
            logWarn(concat("module transition failed: ", next, ": ", exception.what()));
        }
        const std::vector<std::string> movies = std::move(sim_->pendingMovies);
        sim_->pendingMovies.clear();
        for (const std::string& movie : movies) {
            queueMovie(movie, true);
        }
        if (!movies.empty()) {
            playMovieQueue(true);
        }
    }
}

bool GameSession::lineOfSight(kotor::Vec3 from, kotor::Vec3 to, bool doors, float fromLift, float toLift)
{
    from.z += fromLift;
    to.z += toLift;
    if (wallBlocked(scene_, from, to, 0.15f)) {
        return false;
    }
    if (!doors) {
        return true;
    }
    // Perception sight (00502ac0 -> 0050c330 -> 00506650) ray-tests the area including closed doors, whose
    // closed walkmesh (<model>0.dwk) blocks it; an open door has none. Placeables are not tested here.
    const kotor::Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    for (const SoundFacet& facet : soundObjectFacets()) {
        if (!facet.door) {
            continue;
        }
        const kotor::Entity& owner = scene_.entities[facet.entity];
        if (owner.dead || owner.open) {
            continue;
        }
        const kotor::Vec3 e1{facet.b.x - facet.a.x, facet.b.y - facet.a.y, facet.b.z - facet.a.z};
        const kotor::Vec3 e2{facet.c.x - facet.a.x, facet.c.y - facet.a.y, facet.c.z - facet.a.z};
        const kotor::Vec3 p{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
        const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
        if (std::fabs(det) < 1.0e-9f) {
            continue;
        }
        const kotor::Vec3 sv{from.x - facet.a.x, from.y - facet.a.y, from.z - facet.a.z};
        const float u = (sv.x * p.x + sv.y * p.y + sv.z * p.z) / det;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        const kotor::Vec3 q{sv.y * e1.z - sv.z * e1.y, sv.z * e1.x - sv.x * e1.z, sv.x * e1.y - sv.y * e1.x};
        const float v = (d.x * q.x + d.y * q.y + d.z * q.z) / det;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) / det;
        if (t > 1.0e-6f && t <= 1.0f) {
            return false;
        }
    }
    return true;
}

float GameSession::soundObstruction(kotor::Vec3 from, kotor::Vec3 to)
{
    float total = 0.0f;
    kotor::Vec3 start = from;
    const std::vector<SoundFacet>& objectFacets = soundObjectFacets();
    std::vector<std::uint32_t> hitObjects; // 0050c330 skips the object the previous cast hit.
    for (int pass = 0; pass < 100; ++pass) {
        const kotor::Vec3 d{to.x - start.x, to.y - start.y, to.z - start.z};
        float nearest = 2.0f;
        for (const kotor::CollisionFacet& facet : scene_.collision) {
            if (!facet.blocksSight) {
                continue;
            }
            const kotor::Vec3& a = scene_.collisionVertices[facet.a];
            const kotor::Vec3& b = scene_.collisionVertices[facet.b];
            const kotor::Vec3& c = scene_.collisionVertices[facet.c];
            // Moller-Trumbore on the segment start + t * d, t in (0, 1], front faces only.
            const kotor::Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
            const kotor::Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
            // Retail's triangle test 004ab9d0 is one sided: the segment must start on the normal's side
            // of the plane (distance >= 0, normal (p1 - p0) x (p2 - p1)) and end on the other (<= 0), so
            // a wall crossed the other way round does not count, and a wall costs one hit, not two.
            const kotor::Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            const float sideStart = n.x * (start.x - a.x) + n.y * (start.y - a.y) + n.z * (start.z - a.z);
            const float sideEnd = n.x * (to.x - a.x) + n.y * (to.y - a.y) + n.z * (to.z - a.z);
            if (sideStart < 0.0f || sideEnd > 0.0f || sideStart == sideEnd) {
                continue;
            }
            const kotor::Vec3 p{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
            const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
            if (std::fabs(det) < 1.0e-9f) {
                continue;
            }
            const kotor::Vec3 s{start.x - a.x, start.y - a.y, start.z - a.z};
            const float u = (s.x * p.x + s.y * p.y + s.z * p.z) / det;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const kotor::Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
            const float v = (d.x * q.x + d.y * q.y + d.z * q.z) / det;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) / det;
            if (t > 1.0e-6f && t <= 1.0f && t < nearest) {
                nearest = t;
            }
        }
        // Doors and placeables on the line (00506650 -> 004bbe20); an open door has no closed walkmesh to hit.
        float objectNearest = 2.0f;
        const SoundFacet* objectHit = nullptr;
        for (const SoundFacet& facet : objectFacets) {
            const kotor::Entity& owner = scene_.entities[facet.entity];
            if (owner.dead || (facet.door && owner.open)
                || std::find(hitObjects.begin(), hitObjects.end(), facet.entity) != hitObjects.end()) {
                continue;
            }
            const kotor::Vec3 e1{facet.b.x - facet.a.x, facet.b.y - facet.a.y, facet.b.z - facet.a.z};
            const kotor::Vec3 e2{facet.c.x - facet.a.x, facet.c.y - facet.a.y, facet.c.z - facet.a.z};
            const kotor::Vec3 p{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
            const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
            if (std::fabs(det) < 1.0e-9f) {
                continue;
            }
            const kotor::Vec3 s{start.x - facet.a.x, start.y - facet.a.y, start.z - facet.a.z};
            const float u = (s.x * p.x + s.y * p.y + s.z * p.z) / det;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const kotor::Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
            const float v = (d.x * q.x + d.y * q.y + d.z * q.z) / det;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) / det;
            if (t > 1.0e-6f && t <= 1.0f && t < objectNearest) {
                objectNearest = t;
                objectHit = &facet;
            }
        }
        if (nearest > 1.0f && objectNearest > 1.0f) {
            return total;
        }
        if (objectNearest < nearest) {
            // GetObstructionFactor 0050ccf0: a placeable adds 0.4 (_DAT_0073e320), a door 0.55 (_DAT_00747b84).
            total += objectHit->door ? 0.55f : 0.4f;
            hitObjects.push_back(objectHit->entity);
            nearest = objectNearest;
        } else {
            total += 0.4f;
        }
        if (total > 1.0f) {
            return 1.0f;
        }
        const kotor::Vec3 hit{start.x + d.x * nearest, start.y + d.y * nearest, start.z + d.z * nearest};
        const float rest = std::sqrt((to.x - hit.x) * (to.x - hit.x) + (to.y - hit.y) * (to.y - hit.y)
                                     + (to.z - hit.z) * (to.z - hit.z));
        if (rest <= 0.1f) {
            return total;
        }
        const float k = 0.1f / rest;
        start = {hit.x + (to.x - hit.x) * k, hit.y + (to.y - hit.y) * k, hit.z + (to.z - hit.z) * k};
    }
    return std::min(total, 1.0f);
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

// The creature's CAMERAHOOK above its feet. Shipped body models: 1.68 large, 1.634 medium, 1.59 small.
static float cameraHookHeight(const kotor::Scene& scene, std::size_t entity)
{
    for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity != entity || !mesh.model) {
            continue;
        }
        if (const std::optional<kotor::Mdl::Pose> hook = mesh.model->nodePose("camerahook", "", 0.0f)) {
            return hook->translation.z;
        }
        break;
    }
    return 1.634f;
}

namespace {

struct CameraCreature {
    kotor::Vec3 position;
    float personalSpace = 0.6f; // appearance PERSPACE (+0x340 +4)
    float cameraSpace = 0.6f;   // appearance CAMERASPACE (+0x340 +0x10)
};

// The segment-circle crossings of 004aab50 (2D here: both ends and the centre have z 0 at the caller): `nearOrOnly`
// gets the far root when both roots lie in [0, 1) and `nearer` then the near one; with one root in range it is
// that root. Returns how many roots lie in [0, 1).
int segmentCircle(kotor::Vec3 center, float radius, kotor::Vec3 from, kotor::Vec3 to, kotor::Vec3& nearOrOnly,
                  kotor::Vec3& nearer)
{
    const kotor::Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    const kotor::Vec3 f{from.x - center.x, from.y - center.y, from.z - center.z};
    const float a = d.x * d.x + d.y * d.y + d.z * d.z;
    const float b = 2.0f * (f.x * d.x + f.y * d.y + f.z * d.z);
    const float disc = b * b - ((f.x * f.x + f.y * f.y + f.z * f.z) - radius * radius) * a * 4.0f;
    const auto at = [&](float t) { return kotor::Vec3{d.x * t + from.x, d.y * t + from.y, d.z * t + from.z}; };
    if (disc == 0.0f) {
        const float t = -(b / (a + a));
        if (t >= 0.0f && t < 1.0f) {
            nearOrOnly = at(t);
            return 1;
        }
        return 0;
    }
    if (disc <= 0.0f) {
        return 0;
    }
    int count = 2;
    const float farRoot = (std::sqrt(disc) - b) / (a + a);
    if (farRoot < 0.0f || !(farRoot < 1.0f)) {
        count = 1;
    } else {
        nearOrOnly = at(farRoot);
    }
    const float nearRoot = (-b - std::sqrt(disc)) / (a + a);
    if (nearRoot >= 0.0f && nearRoot < 1.0f) {
        if (count == 2) {
            nearer = at(nearRoot);
            return 2;
        }
        nearOrOnly = at(nearRoot);
        return count;
    }
    return count - 1;
}

// CServerExoApp::ShrinkLineByCreatures 004aebb0 -> CSWSArea 004bf650: the creatures of the area (its x-sorted list,
// +0x190) within 3 m (00740c84) of the segment's x and y span, and whose PERSPACE circle does not hold `start`, are
// tested in turns: while `end` lies inside one's CAMERASPACE circle (x and y only), `end` moves to where the segment
// from `start` enters it, 0.001 (0073d708) back towards `start`, and its z becomes
// min z + (max z - min z) x (new 2D length / old 2D length) squared - 0.01 (0073dfec), over the original ends. The turns
// repeat until one moves nothing. Returns whether `end` moved.
bool shrinkLineByCreatures(const std::vector<CameraCreature>& creatures, kotor::Vec3 start, kotor::Vec3 endIn,
                           kotor::Vec3& end)
{
    end = endIn;
    const float xLow = std::min(start.x, endIn.x) - 3.0f;
    const float xHigh = std::max(start.x, endIn.x) + 3.0f;
    const float yLow = std::min(start.y, endIn.y) - 3.0f;
    const float yHigh = std::max(start.y, endIn.y) + 3.0f;
    std::vector<const CameraCreature*> nearby;
    for (const CameraCreature& creature : creatures) {
        const kotor::Vec3& at = creature.position;
        const float dx = at.x - start.x;
        const float dy = at.y - start.y;
        if (at.x >= xLow && at.x <= xHigh && yLow <= at.y && at.y < yHigh
            && creature.personalSpace * creature.personalSpace < dy * dy + dx * dx) {
            nearby.push_back(&creature);
        }
    }
    const float zLow = std::min(start.z, endIn.z);
    const float zHigh = std::max(start.z, endIn.z);
    bool shrunk = false;
    bool moved = !nearby.empty();
    for (int pass = 0; moved && pass < 64; ++pass) {
        moved = false;
        for (const CameraCreature* creature : nearby) {
            const float radius = creature->cameraSpace;
            const float ex = creature->position.x - end.x;
            const float ey = creature->position.y - end.y;
            if (!(ey * ey + ex * ex < radius * radius)) {
                continue;
            }
            const kotor::Vec3 from2{start.x, start.y, 0.0f};
            const kotor::Vec3 to2{end.x, end.y, 0.0f};
            const kotor::Vec3 dir = kotor::normalize(kotor::Vec3{to2.x - from2.x, to2.y - from2.y, 0.0f});
            const kotor::Vec3 back{dir.x * 0.001f, dir.y * 0.001f, dir.z * 0.001f};
            kotor::Vec3 first{};
            kotor::Vec3 second{};
            const int crossings = segmentCircle({creature->position.x, creature->position.y, 0.0f}, radius, from2, to2,
                                                first, second);
            bool changed = false;
            if (crossings == 2) {
                const auto dist2 = [&](const kotor::Vec3& p) {
                    return (start.z - p.z) * (start.z - p.z) + (start.y - p.y) * (start.y - p.y)
                        + (start.x - p.x) * (start.x - p.x);
                };
                const kotor::Vec3 pick = dist2(second) <= dist2(first) ? second : first;
                end = {pick.x - back.x, pick.y - back.y, pick.z - back.z};
                changed = true;
            } else if (crossings == 1 && (end.x != first.x || end.y != first.y)) {
                end = {first.x - back.x, first.y - back.y, first.z - back.z};
                changed = true;
            }
            if (!changed) {
                continue;
            }
            shrunk = true;
            moved = true;
            const float oldLength2 = (from2.x - to2.x) * (from2.x - to2.x) + (from2.y - to2.y) * (from2.y - to2.y);
            if (oldLength2 != 0.0f) {
                const float newLength2 = (0.0f - end.z) * (0.0f - end.z) + (from2.y - end.y) * (from2.y - end.y)
                    + (from2.x - end.x) * (from2.x - end.x);
                end.z = (zHigh - zLow) * (newLength2 / oldLength2) - 0.01f + zLow;
            }
        }
    }
    return shrunk;
}

} // namespace

// CSWCameraOnAStick::Control 0063bcb0 for a camera at rest. Each frame 006398f0 asks for the eye at
// D + 0.5 (007a243c) x (the camera's current flat distance from the head - D), D the style distance (+0x110), at the
// head's z plus HEIGHT, and Control_HitCheckCamera 0063b050 moves it; repeating that settles where the hit-checked
// distance gives itself back, which is the eye returned here (retail's eye is unchanged at 1 mm two samples after a
// load's HUD opens).
// 0063b050, one pass: ShrinkLineByCreatures may pull the end in. Then four casts (00456b60, ignoring the followed
// creature) run from the head to the end moved 0.35 (CameraPersonalSpace, 007a241c) either way along
// up x (head - end) and 0.35 up and down. With none hitting, the eye is the end. Otherwise d is the largest
// distance from a cast's hit point to that cast's own end, and the eye is the end moved d back along the line towards
// the head and raised along the followed object's local up by d / |end - head| x (0.35 + 0.15, 0074a604).
kotor::Vec3 GameSession::chaseCameraEye(kotor::Vec3 head, kotor::Vec3 back, float desired, float height) const
{
    const std::array<float, 7> key{head.x, head.y, head.z, back.x, back.y, desired, height};
    if (chaseEyeValid_ && key == chaseEyeKey_) {
        return chaseEye_;
    }
    const std::size_t followed = controlledEntity();
    std::vector<CameraCreature> creatures;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Creature) {
            continue;
        }
        CameraCreature creature;
        creature.position = entity.position;
        if (nav_) {
            if (const auto found = nav_->personalSpace.find(entity.appearance); found != nav_->personalSpace.end()) {
                creature.personalSpace = found->second;
            }
            if (const auto found = nav_->cameraSpace.find(entity.appearance); found != nav_->cameraSpace.end()) {
                creature.cameraSpace = found->second;
            }
        }
        creatures.push_back(creature);
    }
    std::sort(creatures.begin(), creatures.end(),
              [](const CameraCreature& a, const CameraCreature& b) { return a.position.x < b.position.x; });
    const std::uint32_t ignore = followed < scene_.entities.size() ? static_cast<std::uint32_t>(followed) : UINT32_MAX;
    constexpr float kPersonalSpace = 0.35f;
    // Every cast below runs from the head to a point at most `desired` away flat, within 0.35 of HEIGHT above or
    // below (plus the creature shrink's 0.01): the room faces of that box are gathered once.
    const float reach = desired + kPersonalSpace + 0.1f;
    const kotor::SceneHitFaces area = kotor::gatherSceneHitFaces(
        scene_, {head.x - reach, head.y - reach, std::min(head.z, head.z + height) - kPersonalSpace - 0.1f},
        {head.x + reach, head.y + reach, std::max(head.z, head.z + height) + kPersonalSpace + 0.1f});
    const auto hitCheck = [&](kotor::Vec3 wanted) {
        kotor::Vec3 end = wanted;
        kotor::Vec3 eye = wanted;
        kotor::Vec3 shrunk{};
        if (shrinkLineByCreatures(creatures, head, end, shrunk)) {
            end = shrunk;
            eye = shrunk;
        }
        const kotor::Vec3 side = kotor::normalize(kotor::Vec3{-(head.y - end.y), head.x - end.x, 0.0f});
        const kotor::Vec3 probes[4] = {
            {end.x + side.x * kPersonalSpace, end.y + side.y * kPersonalSpace, end.z + side.z * kPersonalSpace},
            {end.x - side.x * kPersonalSpace, end.y - side.y * kPersonalSpace, end.z - side.z * kPersonalSpace},
            {end.x, end.y, end.z + kPersonalSpace},
            {end.x, end.y, end.z - kPersonalSpace}};
        bool any = false;
        float pushed = 0.0f;
        for (const kotor::Vec3& probe : probes) {
            kotor::Vec3 at{};
            if (kotor::sceneHitCheck(scene_, head, probe, ignore, at, &area) != 0) {
                const float beyond = kotor::length(at - probe);
                if (!any || beyond >= pushed) {
                    pushed = beyond;
                }
                any = true;
            }
        }
        if (!any) {
            return eye;
        }
        const kotor::Vec3 away = end - head;
        const float span = kotor::length(away);
        const kotor::Vec3 line = kotor::normalize(away);
        const float lift = span > 0.0f ? pushed / span * (kPersonalSpace + 0.15f) : 0.0f;
        return kotor::Vec3{eye.x - pushed * line.x, eye.y - pushed * line.y, eye.z - pushed * line.z + lift};
    };
    kotor::Vec3 eye{};
    // Start from where the camera was (006398f0 reads the owner's current position); the first time, the full distance.
    float current = chaseDistance_ > 0.0f ? chaseDistance_ : desired;
    for (int frame = 0; frame < 64; ++frame) {
        const float distance = desired + (current - desired) * 0.5f;
        eye = hitCheck({head.x - back.x * distance, head.y - back.y * distance, head.z + height});
        const float next = std::hypot(eye.x - head.x, eye.y - head.y);
        // Faceted walls can leave the rest point flickering by a few 1e-5 m from frame to frame (as retail's own
        // camera would); 0.1 mm is far inside the trace's 1 mm.
        if (std::fabs(next - current) < 1.0e-4f) {
            break;
        }
        current = next;
    }
    chaseDistance_ = std::hypot(eye.x - head.x, eye.y - head.y);
    chaseEyeKey_ = key;
    chaseEye_ = eye;
    chaseEyeValid_ = true;
    return eye;
}

SceneCamera GameSession::camera() const
{
    if (sim_->miniGame && !dialogueView_.active) {
        return sim_->miniGame->camera();
    }
    if (dialogueView_.active && dialogueCameraModel_ && !dialogueCameraClip_.empty()) {
        const float time = std::min(dialogueCameraTime_, dialogueCameraDuration_);
        const std::optional<kotor::Mdl::Pose> pose = dialogueCameraModel_->nodePose(
            "camerahook", dialogueCameraClip_, time);
        if (pose.has_value()) {
            SceneCamera camera;
            camera.eye = pose->translation;
            // A shipped camerahook is a modeller's camera: it looks along local -Z with local
            // +Y up. Measured over 236 shipped camera-model clips, local +Y is within 26 degrees
            // of world up in 90% of them (the old forward) and local -Z is level in 91%.
            camera.forward = kotor::normalize(
                kotor::Vec3{-pose->rotation.m[0][2], -pose->rotation.m[1][2],
                            -pose->rotation.m[2][2]});
            camera.up = kotor::normalize(
                kotor::Vec3{pose->rotation.m[0][1], pose->rotation.m[1][1],
                            pose->rotation.m[2][1]});
            camera.fovYDegrees = scene_.cameraStyle.viewAngle; // Retail measured 55 in dialogue.
            // An animated shot with its own CamFieldOfView (0059eaa0; m01aa_c01 uses 34.5) is
            // drawn at it. INFERRED: the client's use of the field was not traced; the paired
            // opening's close-ups fill the frame as a narrower view would.
            if (dialogueView_.camFieldOfView > 0.0f) {
                camera.fovYDegrees = dialogueView_.camFieldOfView;
            }
            return camera;
        }
    }
    // The list's shot starts when the list does: SetReplies (006a86a0 -> 006a8de0 -> 0063432c)
    // runs once the line is over. Paired walk line00/line01: Trask framed while he speaks, the PC
    // once the replies are up.
    const bool listShot = dialogueView_.shotOnListener && !dialogueLineSpeaking();
    const std::int32_t shotAngle = listShot && dialogueView_.replyCameraAngle > 0 ? dialogueView_.replyCameraAngle
                                                                                  : dialogueView_.cameraAngle;
    if (dialogueView_.active && !dialogueView_.computer && shotAngle >= 1 && shotAngle <= 3) {
        // Retail 006bd260 frames the CAMERAHOOK nodes of the line's speaker and listener, not
        // their feet (the name is at 0x74f59c), and a coincident pair is replaced at 006bb80a.
        const auto anchorOf = [this](std::size_t entity) {
            const kotor::Vec3 at = scene_.entities[entity].position;
            return kotor::Vec3{at.x, at.y, at.z + cameraHookHeight(scene_, entity)};
        };
        const auto entityOr = [this](std::uint32_t object, std::size_t fallback) {
            const std::size_t named = entityOfObject(object);
            return named != static_cast<std::size_t>(-1) ? named : fallback;
        };
        const std::size_t listenerEntity = entityOr(dialogueView_.listenerObject, controlledEntity());
        const std::size_t speakerEntity = entityOr(dialogueView_.speakerObject, listenerEntity);
        if (speakerEntity < scene_.entities.size() && listenerEntity < scene_.entities.size()) {
            // Paired frames on end_trask01: a line with replies to choose from is shot on the PC
            // from its first moment (the NPC's text moves to the top bar); other lines on the speaker.
            const bool onListener = listShot;
            const std::size_t framed = onListener ? listenerEntity : speakerEntity;
            const std::size_t other = onListener ? speakerEntity : listenerEntity;
            const kotor::Vec3 speaker = anchorOf(framed);
            kotor::Vec3 listener = anchorOf(other);
            if (framed == other) {
                listener = {speaker.x + 1.5f, speaker.y + 1.5f, speaker.z + 0.1f};
            }
            // Retail 006bcec0 remembers a side per pair and 006bb610 gives the swapped pair the
            // other one, so both are shot from the same side of the line between them. Logged on
            // Trask: NPC framed is side 2, the PC framed is side 1. Which side a new pair starts on
            // (006bc600) is simplified to that, or the other side when it is blocked.
            const std::size_t led = controlledEntity();
            const std::int32_t preferred = framed == led ? 1 : other == led ? 2 : framed < other ? 2 : 1;
            SceneCamera preset = presetCamera(shotAngle, dialogueView_.camHeightOffset,
                                              dialogueView_.tarHeightOffset, speaker, listener, preferred);
            if (wallBlocked(scene_, speaker, preset.eye, 0.1f)) {
                const SceneCamera opposite = presetCamera(shotAngle, dialogueView_.camHeightOffset,
                                                          dialogueView_.tarHeightOffset, speaker, listener,
                                                          3 - preferred);
                if (!wallBlocked(scene_, speaker, opposite.eye, 0.1f)) {
                    preset = opposite;
                }
            }
            if (preset.fovYDegrees > 0.0f) {
                return preset;
            }
        }
    }
    if (dialogueView_.active && dialogueView_.cameraId >= 0) {
        const SceneCamera camera = cinematicCamera(dialogueView_.cameraId);
        if (camera.fovYDegrees > 0.0f) {
            return camera;
        }
    }
    // The camera follows the controlled creature (the PC, or a companion).
    const kotor::Vec3 focus = controlledPosition();
    if (thirdPerson_ && playerVisible_) {
        // camerastyle.2da with the creature's own camerahook height. Measured on retail in two
        // modules: the eye is 3.2000 behind the entry point and 2.1297 above it (pmbal's camerahook
        // 1.68 + height 0.45), the view 7 degrees below level (pitch 83), fovy 55.
        const kotor::Scene::CameraStyle& style = scene_.cameraStyle;
        const float hookHeight = cameraHookHeight(scene_, controlledEntity());
        const float belowLevel = (90.0f - style.pitch) * kPi / 180.0f;
        // Inferred: free look tilts the view about the eye, within the style's fl_ limits.
        const float elevation = std::clamp(pitch_, -style.lookDown * kPi / 180.0f,
                                           style.lookUp * kPi / 180.0f) - belowLevel;
        // GetTargetHeadPosition 005f5180: the followed object's position raised by +0x6c, which SetCameraFollow
        // 0063f9d0 sets to the creature's camera hook height (vfunc +0xd8) plus its appearance.2da CameraHeightOffset
        // (00697580, column 0x7a20c8). The eye sits camerastyle HEIGHT (+0x120, read at 006381c3) above that point
        // (006398f0 adds it to the head's z). CameraHeightOffset is not read here yet.
        const kotor::Vec3 head{focus.x, focus.y, focus.z + hookHeight};
        const kotor::Vec3 target{focus.x, focus.y, head.z + style.height};
        const kotor::Vec3 back{std::cos(yaw_), std::sin(yaw_), 0.0f};
        // ProgFX 1500 (Effects.cpp) drives the view angle; with its dolly on, CSWCameraOnAStick::Control
        // 0063bcb0 sets the distance to tan(style angle / 2) x style distance / tan(angle / 2).
        const std::optional<EffectDirector::CameraZoom> zoom = effects_->cameraZoom();
        const float viewAngle = zoom ? zoom->viewAngle : style.viewAngle;
        const float desired = zoom && zoom->dolly
            ? std::tan(style.viewAngle * kPi / 360.0f) * style.distance / std::tan(viewAngle * kPi / 360.0f)
            : style.distance;
        const kotor::Vec3 eye = cameraNoClip_
            ? kotor::Vec3{target.x - back.x * desired, target.y - back.y * desired, target.z}
            : chaseCameraEye(head, back, desired, style.height);
        SceneCamera camera;
        camera.eye = eye;
        camera.forward = {std::cos(elevation) * back.x, std::cos(elevation) * back.y,
                          std::sin(elevation)};
        // The camera's up is the world up made perpendicular to its forward (retail logs 0, 0.121, 0.993 for a 7 degree pitch).
        camera.up = kotor::normalize(kotor::Vec3{-std::sin(elevation) * camera.forward.x, -std::sin(elevation) * camera.forward.y,
                                                  1.0f - std::sin(elevation) * camera.forward.z});
        camera.fovYDegrees = viewAngle;
        return camera;
    }
    SceneCamera camera;
    camera.eye = {focus.x, focus.y, focus.z + kEyeHeight};
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
                                      kotor::Vec3 speaker, kotor::Vec3 target, std::int32_t side) const
{
    // Angle 0 never gets here: DialogueShots has replaced it, as retail does before 006bb7b0.
    if (angle < 1 || angle > 3) {
        return {};
    }
    // Retail 006bb7b0 in its mode 0. Checked against the eyes measured on end_trask01: each
    // formula lands within 2 mm. `d` runs from the listener to the framed creature, unnormalized.
    const kotor::Vec3 up{0.0f, 0.0f, 1.0f};
    const kotor::Vec3 d = speaker - target;
    const float range = kotor::length(d);
    // Retail 006bb861: 30 degrees, 90 for angle 3, negated for side 2 (006bb8fd).
    const float yaw = (angle == 3 ? 90.0f : 30.0f) * (side == 2 ? -1.0f : 1.0f) * kPi / 180.0f;
    const kotor::Vec3 turned = rotateZ(d, yaw);
    kotor::Vec3 targetPoint{};
    kotor::Vec3 eye{};
    if (angle == 1) {
        // Close-up from just off the line. The retail range term is read from the variable
        // CU_PB_Range (006310xx), 0 unless a script sets it; no shipped use is known, so it is 0.
        const kotor::Vec3 flat = kotor::normalize(kotor::Vec3{d.x, d.y, 0.0f});
        targetPoint = speaker - flat * 0.2f + up * (targetHeight - 0.04f);
        eye = targetPoint - rotateZ(flat, yaw) * 0.5f + up * camHeight;
    } else if (angle == 2) {
        const kotor::Vec3 a = speaker - up * (0.1f * range);
        const kotor::Vec3 b = target - up * (0.05f * range);
        const kotor::Vec3 lowered = a - b;
        const kotor::Vec3 turnedLow = rotateZ(lowered, yaw);
        targetPoint = b + lowered * 0.3f + up * targetHeight;
        eye = targetPoint - turnedLow * 0.8f - kotor::normalize(turnedLow) * 0.15f + up * camHeight;
    } else {
        targetPoint = target + d * 0.5f + up * (targetHeight - 0.2f * range);
        eye = targetPoint - turned * 1.5f + up * (camHeight + 0.3f * range);
    }
    SceneCamera camera;
    camera.eye = eye;
    camera.forward = kotor::normalize(targetPoint - eye);
    camera.up = up;
    camera.fovYDegrees = 55.0f; // Retail 006bd2fc sets it on the dialogue camera (0x425c0000).
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
        // Measured on retail: the GIT value is the projection's fovy. tar_m02aa cameras 1, 3, 5
        // draw at 50, 55, 45; end_m01aa cameras 5, 9 at 55.
        camera.fovYDegrees = definition.fov > 0.0f ? definition.fov : 55.0f;
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
        const float distance = distance2D(controlledPosition(), entity.position);
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
    // The steered creature cannot be the one it talks to.
    const std::size_t self = companionControlled() ? controlledEntity()
                                                   : static_cast<std::size_t>(-1);
    for (const kotor::Entity& entity : scene_.entities) {
        if ((entity.kind != kotor::EntityKind::Creature && entity.kind != kotor::EntityKind::Placeable)
            || entity.conversation.empty() || entity.dead
            || static_cast<std::size_t>(&entity - scene_.entities.data()) == self) {
            continue;
        }
        const float distance = distance2D(controlledPosition(), entity.position);
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
        if (distance2D(controlledPosition(), entity.position) < 2.5f) {
            return "E: use " + (entity.name.empty() ? entity.tag : entity.name);
        }
    }
    return {};
}

// Retail 006227e0 hit-tests the scene under the pointer and 00620350 HandleMouseClickInWorld acts on it. NOT TRACED
// here: the hit test itself (it takes the scene geometry and its ignore lists); the box of the entity model
// stands in. The nearest selectable object the ray (eye, dir) crosses, or npos.
std::size_t GameSession::pickEntity(kotor::Vec3 eye, kotor::Vec3 dir) const
{
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 1.0e9f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (!targetableKind(i) || i == controlledEntity()) {
            continue;
        }
        const kotor::Entity& entity = scene_.entities[i];
        // The meshes of the entity in model space, the box that holds them (a model-less one gets a body-sized box).
        float low[3] = {1.0e9f, 1.0e9f, 1.0e9f};
        float high[3] = {-1.0e9f, -1.0e9f, -1.0e9f};
        bool any = false;
        for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity != i || mesh.attachModel || !mesh.visible) {
                continue;
            }
            for (const kotor::SceneVertex& vertex : mesh.vertices) {
                const float p[3] = {vertex.position.x, vertex.position.y, vertex.position.z};
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], p[axis]);
                    high[axis] = std::max(high[axis], p[axis]);
                }
                any = true;
            }
        }
        if (!any) {
            const float box[6] = {-0.4f, -0.4f, 0.0f, 0.4f, 0.4f, 1.8f};
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = box[axis];
                high[axis] = box[axis + 3];
            }
        }
        // The ray in the model space of the entity (the model matrix is translation * rotation about Z).
        const float turn = -entity.drawBearing();
        const float c = std::cos(turn);
        const float s = std::sin(turn);
        const float ox = eye.x - entity.position.x;
        const float oy = eye.y - entity.position.y;
        const float origin[3] = {ox * c - oy * s, ox * s + oy * c, eye.z - entity.position.z};
        const float direction[3] = {dir.x * c - dir.y * s, dir.x * s + dir.y * c, dir.z};
        float near = 0.0f;
        float far = 1.0e9f;
        bool hit = true;
        for (int axis = 0; axis < 3 && hit; ++axis) {
            const float lo = low[axis] - 0.1f;
            const float hi = high[axis] + 0.1f;
            if (std::fabs(direction[axis]) < 1.0e-8f) {
                hit = origin[axis] >= lo && origin[axis] <= hi;
                continue;
            }
            float t0 = (lo - origin[axis]) / direction[axis];
            float t1 = (hi - origin[axis]) / direction[axis];
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            near = std::max(near, t0);
            far = std::min(far, t1);
            hit = near <= far;
        }
        if (hit && near < bestDistance) {
            bestDistance = near;
            best = i;
        }
    }
    return best;
}

// 00620350: the first click on an object selects it (SetLastTarget 005f4a20), a click on the selected object that
// was already clicked performs its first default action.
bool GameSession::clickWorld(float ndcX, float ndcY, float aspect)
{
    if (controlledDead() || dialogueView_.active) {
        return false;
    }
    const SceneCamera view = camera();
    const auto normalise = [](kotor::Vec3 v) {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return length > 0.0f ? kotor::Vec3{v.x / length, v.y / length, v.z / length} : v;
    };
    const kotor::Vec3 forward = normalise(view.forward);
    const kotor::Vec3 right = normalise(kotor::Vec3{forward.y * view.up.z - forward.z * view.up.y,
                                                    forward.z * view.up.x - forward.x * view.up.z,
                                                    forward.x * view.up.y - forward.y * view.up.x});
    const kotor::Vec3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                         right.x * forward.y - right.y * forward.x};
    const float t = std::tan(view.fovYDegrees * 3.14159265f / 360.0f);
    const kotor::Vec3 dir = normalise(kotor::Vec3{forward.x + right.x * ndcX * t * aspect + up.x * ndcY * t,
                                                  forward.y + right.y * ndcX * t * aspect + up.y * ndcY * t,
                                                  forward.z + right.z * ndcX * t * aspect + up.z * ndcY * t});
    const std::size_t hit = pickEntity(view.eye, dir);
    if (hit == static_cast<std::size_t>(-1)) {
        return false;
    }
    if (selectedEntity_ == hit && clickedEntity_ == hit) {
        performDefaultAction(hit);
    } else {
        selectedEntity_ = static_cast<std::uint32_t>(hit);
        clickedEntity_ = static_cast<std::uint32_t>(hit);
        targetLosTimer_ = 0.0f;
    }
    return true;
}

// 00620620 GetDefaultActions: a hostile creature offers Attack, a friendly one Talk, a closed door Open, a useable
// placeable Use. The object is walked to first when it is out of reach.
bool GameSession::performDefaultAction(std::size_t index)
{
    bool hostile = false;
    if (index >= scene_.entities.size() || controlLocked() || controlledDead() || dialogueView_.active
        || !targetableEntity(index, hostile)) {
        return false;
    }
    if (scene_.entities[index].kind == kotor::EntityKind::Creature && hostile) {
        attackEntity(index);
        return true;
    }
    const kotor::Entity& entity = scene_.entities[index];
    const float reach = entity.kind == kotor::EntityKind::Placeable ? 2.5f : 3.0f;
    if (inReach(index, reach)) {
        return finishUse(index);
    }
    pendingUse_ = static_cast<std::uint32_t>(index);
    return moveTo(entity.position, reach - 1.0f);
}

bool GameSession::finishUse(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    if (scene_.entities[index].kind == kotor::EntityKind::Creature) {
        // As interact() does: the OnDialogue handler of a creature decides, with none it converses.
        const auto hook = scene_.entities[index].scripts.find("ScriptDialogue");
        if (hook != scene_.entities[index].scripts.end() && !hook->second.empty()) {
            const ScriptContext context{{"lastSpeaker", controlledObject()},
                                        {"listenPattern", static_cast<std::uint32_t>(-1)}};
            runHook("ScriptDialogue", index, controlledObject(), &context);
            return true;
        }
        return startDialogue(index);
    }
    const UseOutcome outcome = useObject(index);
    lastUseOutcome_ = static_cast<int>(outcome);
    return outcome != UseOutcome::OutOfRange && outcome != UseOutcome::Invalid && outcome != UseOutcome::Refused;
}

void GameSession::tickPendingUse()
{
    if (pendingUse_ == UINT32_MAX) {
        return;
    }
    const std::size_t index = pendingUse_;
    if (index >= scene_.entities.size() || controlLocked() || controlledDead() || dialogueView_.active) {
        pendingUse_ = UINT32_MAX;
        return;
    }
    const float reach = scene_.entities[index].kind == kotor::EntityKind::Placeable ? 2.5f : 3.0f;
    if (inReach(index, reach)) {
        pendingUse_ = UINT32_MAX;
        finishUse(index);
    } else if (!movePending()) {
        pendingUse_ = UINT32_MAX; // the walk ended short of it
    }
}

// Key R (keymap DefaultAction) acts on the selected object.
bool GameSession::activateSelected()
{
    return selectedEntity_ != UINT32_MAX && performDefaultAction(selectedEntity_);
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
            const ScriptContext context{{"lastSpeaker", controlledObject()},
                                        {"listenPattern", static_cast<std::uint32_t>(-1)}};
            runHook("ScriptDialogue", index, controlledObject(), &context);
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
        if (distance2D(controlledPosition(), entity.position) < 3.0f && openStore(i)) {
            setUiPanel(UiPanel::Merchant);
            return true;
        }
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
            continue;
        }
        if (distance2D(controlledPosition(), entity.position) < 2.5f) {
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
    kotor::Vec3 at = controlledPosition();
    float travelled = 0.0f;
    for (int step = 0; step < 600; ++step) {
        const float dx = target.x - at.x;
        const float dy = target.y - at.y;
        const float remaining = std::sqrt(dx * dx + dy * dy);
        if (remaining <= 0.5f) {
            return travelled;
        }
        // A body's width from something against a wall is as near as walking gets
        // (kPlayerRadius stops the steps below), and no wall stands between: arm's length.
        // The 1 m is this engine's; retail's approach point was not traced.
        if (remaining <= 1.0f && !wallBlocked(scene_, at, target, 0.0f)) {
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
    if (distance2D(controlledPosition(), scene_.entities[entityIndex].position) > range) {
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
    const kotor::Vec3 from = controlledPosition();
    yaw_ = std::atan2(target.y - from.y, target.x - from.x);
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
        if (distance2D(controlledPosition(), target) < 0.6f) {
            return true;
        }
        if (!moveTo(target, 0.6f, distance2D(controlledPosition(), target) > 4.0f)) {
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
    return distance2D(controlledPosition(), target) < 1.2f;
}

bool GameSession::loadDialogue(const std::string& resref, kotor::Dlg& dlg, int speakerGender)
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
        // NPC lines read the speaker's gender, player lines the player's.
        dlg = kotor::Dlg::parse(gff, tlk_ ? &*tlk_ : nullptr, resref, speakerGender,
                                character_.gender);
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
        logInfo(concat("conversation not started: ", ownerTag, " has no dialogue"));
        return false;
    }
    kotor::Dlg dlg;
    const int speakerGender = entityIndex != static_cast<std::size_t>(-1)
        ? scene_.entities[entityIndex].stats.gender : 0;
    if (!loadDialogue(resref, dlg, speakerGender)) {
        logEvent("missing dialogue " + resref);
        logInfo(concat("conversation not started: missing dialogue ", resref));
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
    active.shots = DialogueShots(resref);
    active.npcObject = owner;
    active.dlg = std::move(dlg);
    activeDialogue_ = std::move(active);
    sim_->dialoguePaused = false;
    if (audio_ && entityIndex != static_cast<std::size_t>(-1)) {
        effects_->speak(entityIndex, 6, EffectDirector::VoiceKind::Direct); // select, 005f4d60
    }
    loadDialogueCameraModel(activeDialogue_->dlg.cameraModel);
    spawnDialogueStunts();
    if (audio_ && !activeDialogue_->dlg.ambientTrack.empty()) {
        audio_->playConversationTrack(activeDialogue_->dlg.ambientTrack);
        dialogueTrackStarted_ = true;
    }
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
        // retail 005a3640: a start link whose Speaker cannot be resolved is passed over,
        // before its condition runs.
        if (!dialogueSpeakerResolves(activeDialogue_->dlg.entries[candidate].speaker)) {
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
        logInfo(concat("conversation not started: ", activeDialogue_->resref, " has no passing start link"));
        clearDialogueState();
        return false;
    }
    scriptLog_.push_back("dialogue start " + activeDialogue_->resref + " entry="
                         + std::to_string(entry));
    logEvent("dialogue started with " + activeDialogue_->speaker);
    logInfo(concat("conversation started: ", activeDialogue_->resref, " entry ", entry));
    // 004cf490 stores the conversation owner in client +0x188; the selected object (SetLastTarget 005f4a20,
    // 005fa5a0) is +0x2b4, a different field, so a conversation leaves the selection alone (paired frame
    // req_swoop_finish_045014: the Sealed Door is selected after the mechanic's conversation).
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
    active.shots = DialogueShots(resref);
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
    // GetPCSpeaker and the entering object: the creature the player steers.
    const std::uint32_t pc = controlledObject();
    return {{"entering", pc}, {"exiting", pc}, {"caller", pc}, {"trigger", pc},
            {"lastSpeaker", pc}};
}

// retail 0059fc30: an empty Speaker or the owner's tag is the owner; any other tag is looked up
// once per conversation, and answers only while that object is within 1000 of the owner (same
// area is implied here: the engine holds one area).
bool GameSession::dialogueSpeakerResolves(const std::string& speaker)
{
    if (speaker.empty() || lower(speaker) == lower(activeDialogue_->npcTag)) {
        return true;
    }
    const std::string key = lower(speaker);
    const auto cached = activeDialogue_->speakers.find(key);
    const std::uint32_t object = cached != activeDialogue_->speakers.end() ? cached->second
                                                                           : scriptWorld_.byTag(speaker);
    activeDialogue_->speakers[key] = object;
    if (object == script::kObjectInvalid) {
        return false;
    }
    const kotor::Vec3 at = actorPosition(object);
    const kotor::Vec3 from = actorPosition(activeDialogue_->npcObject);
    const float dx = at.x - from.x;
    const float dy = at.y - from.y;
    const float dz = at.z - from.z;
    return dx * dx + dy * dy + dz * dz <= 1000.0f * 1000.0f;
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
        // retail 005a03c0: as at the start, an unresolved Speaker passes the link over.
        if (!dialogueSpeakerResolves(activeDialogue_->dlg.entries[entry].speaker)) {
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
// then the journal, then plot XP. Node scripts run on the conversation owner: retail passes the
// owner to 0059ed70 from 005a3640, 005a3820 and 005a4010, and resolves the Speaker separately
// for sound and animation only.
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
        const std::int32_t plot = plotXpForIndex(node.plotIndex, node.plotXpPercentage);
        giveXp(plot);
        statusFeedback(2, plot); // retail 0059ef00
    }
    return true;
}
template bool GameSession::applyDialogueNode<kotor::DlgEntry>(const kotor::DlgEntry&, std::uint32_t);
template bool GameSession::applyDialogueNode<kotor::DlgReply>(const kotor::DlgReply&, std::uint32_t);

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
    dialogueView_.computer = activeDialogue_->dlg.conversationType == 1;
    dialogueView_.npcTag = activeDialogue_->npcTag;
    dialogueView_.speaker = activeDialogue_->speaker;
    std::vector<std::uint32_t> replies;
    // Control entries carry scripts but no spoken line, and a single textless
    // link. They run and pass straight through (the shipped conversation starts
    // on such an entry).
    for (int guard = 0; guard < 32; ++guard) {
        // Copies: a node script can replace the conversation these live in.
        const kotor::DlgEntry entry = activeDialogue_->dlg.entries[entryIndex];
        if (traceDialogueTrail_) {
            dialogueTrail_.emplace_back(false, entryIndex);
        }
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
        // A terminal's static-camera node with a Delay is the live feed (computercamera.gui, ctor 006a95f0;
        // CGuiInGame::SetComputerCameraShot 0062c030): it stays up for its Delay (seconds, DLG "default is 5")
        // although it has no line, so the player sees the camera and can cancel it. The hold is the node's
        // Delay: CSWSDialog::SendDialogEntry 005a4010 passes a node whose flag 0x10 is set (an explicit Delay,
        // +0x24) to CSWSDialog::SetDialogDelay 0059ff50, which takes max(computed delay, Delay) seconds
        // (computed 0.01 s for a textless node) and arms CSWSObject::SetDialogDelay 004cb1a0;
        // CSWSObject::UpdateDialog 004cd580 moves on only once IsDialogDelay 004cd2d0 is false. Without a Delay
        // (0xFFFFFFFF) the textless node passes straight through, as the code below does.
        if (activeDialogue_->dlg.conversationType == 1 && entry.cameraAngle == 6 && entry.cameraId >= 0
            && entry.delay != 0xFFFFFFFFu && entry.delay > 0u && !replies.empty()) {
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
        // A control entry whose script pauses the conversation holds its shot until the resume
        // (measured at tar_m02aa, k_ptar_shoot: 5.5 s on this textless entry, then the next line).
        bool pauseQueued = sim_->dialoguePaused;
        for (const auto& [id, actor] : sim_->actors) {
            (void)id;
            for (const ActorAction& action : actor.queue) {
                pauseQueued = pauseQueued || action.kind == ActorActionKind::PauseConversation;
            }
        }
        if (pauseQueued) {
            break;
        }
        // A control entry and its textless link walk the shot table like any other
        // (inferred: retail's server path 005a13d0 and 005a1ecc does not look at the text).
        const std::int32_t passAngle = activeDialogue_->shots.enterEntry(entryIndex, entry.cameraAngle);
        if (stateTrace_ != nullptr) {
            stateTrace_->onDialogueControlEntry(*this, entryIndex, passAngle);
        }
        activeDialogue_->shots.beginReplies();
        activeDialogue_->shots.offerReply(replies.front(), reply.cameraAngle);
        if (traceDialogueTrail_) {
            dialogueTrail_.emplace_back(true, replies.front());
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
    dialogueView_.listenerObject = controlledObject();
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
    dialogueView_.text = entry.text.empty() ? std::string{} : substituteTokens(entry.text);
    dialogueView_.entry = entryIndex;
    dialogueView_.cameraId = entry.cameraId;
    const std::int32_t authoredAngle = entry.cameraAngle;
    dialogueView_.cameraAngle = activeDialogue_->shots.enterEntry(entryIndex, authoredAngle);
    if (stateTrace_ != nullptr) {
        stateTrace_->onDialogueLine(*this, entryIndex);
    }
    dialogueView_.shotOnListener = false;
    dialogueView_.camVidEffect = entry.camVidEffect;
    // Retail 006306e0: an entry's camera set-up applies its video effect on a
    // static camera and disables any effect on every other camera.
    videoEffects_.applyDialogueNode(install_, entry.camVidEffect,
                                    entry.cameraAngle == 6 && entry.cameraId >= 0);
    dialogueView_.camHeightOffset = entry.camHeightOffset;
    dialogueView_.tarHeightOffset = entry.tarHeightOffset;
    dialogueView_.camFieldOfView = entry.camFieldOfView;
    setDialogueCameraClip(entry.cameraAnimation);
    playDialoguePresentation(entry, entry.sound);
    faceDialogueParticipants();
    // How long an entry with no voice stays up: its Delay (seconds, inferred from the
    // shipped values 1..20) or a reading time.
    dialogueHold_ = entry.delay != 0xFFFFFFFFu
        ? static_cast<float>(entry.delay)
        : std::max(1.5f, 0.06f * static_cast<float>(entry.text.size()));
    // WaitFlags (read at 0059f5f0 into node+0x20, handed to the client by 005a13d0): bit 0 makes
    // the entry wait for its cutscene animation (the client polls IsAnimationPlayingInDialog
    // 0062d570). The camera clip runs with the participants' CUTnnnW clips, so its length stands
    // for theirs. INFERRED from those names and the paired walk (retail's m01aa_c01 opening lasts
    // about 16 s, not 1.5 s per entry); the client wait itself was not traced.
    if ((entry.waitFlags & 1u) != 0u && dialogueCameraDuration_ > 0.0f) {
        dialogueHold_ = std::max(dialogueHold_, dialogueCameraDuration_);
    }
    dialogueView_.options.clear();
    activeDialogue_->shots.beginReplies();
    std::int32_t firstReplyAngle = 0;
    for (const std::uint32_t replyIndex : replies) {
        const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
        const std::int32_t replyAngle = activeDialogue_->shots.offerReply(replyIndex, reply.cameraAngle);
        if (firstReplyAngle == 0) {
            firstReplyAngle = replyAngle;
        }
        DialogueOption option;
        option.text = reply.text.empty() ? "(Continue)" : substituteTokens(reply.text);
        option.reply = replyIndex;
        dialogueView_.options.push_back(std::move(option));
    }
    if (dialogueView_.options.empty()) {
        dialogueView_.options.push_back({"(End conversation)", UINT32_MAX});
    }
    // Paired frames on end_trask01: a line with replies to choose from is shot on the PC once the
    // list is up (camera(): the line itself stays on its speaker). Retail 0063432c shoots that list
    // from its first reply's queued angle. A line with an authored angle above 1 keeps it (inferred
    // from those same frames).
    dialogueView_.replyCameraAngle = 0;
    if (authoredAngle <= 1 && dialogueView_.options.size() > 1) {
        dialogueView_.shotOnListener = true;
        if (authoredAngle <= 0 && firstReplyAngle > 0) {
            dialogueView_.replyCameraAngle = firstReplyAngle;
        }
    }
    logEvent("dialog " + dialogueView_.speaker + ": " + dialogueView_.text);
}

std::size_t GameSession::dialogueStuntFor(std::size_t participant) const
{
    for (const DialogueStunt& stunt : dialogueStunts_) {
        if (stunt.participant == participant) {
            return stunt.stand;
        }
    }
    return participant;
}

// The shipped cutscene conversations (22 with a StuntList) animate stunt models with the
// CUT0nnW clips; the clips carry the world placement, so a stand-in sits at the origin
// with no bearing. Most stunt models have only a head of their own (often not the participant's)
// on a skeleton of hidden "_g" proxies, so the participant's body and head wear those clips.
// Inferred from that data: retail's stunt code past the reader 005a2ae0 was not traced.
void GameSession::spawnDialogueStunts()
{
    if (!activeDialogue_.has_value()) {
        return;
    }
    for (const kotor::Dlg::Stunt& stunt : activeDialogue_->dlg.stunts) {
        // PLAYER is the creature the player controls; the PC's avatar has no object of its own here.
        std::size_t participant = stunt.participant == "PLAYER"
            ? controlledEntity() : findEntity(stunt.participant);
        if (participant == static_cast<std::size_t>(-1) || stunt.model.empty()) {
            logInfo(concat("dialogue stunt: ", stunt.participant, " not found for ", stunt.model));
            continue;
        }
        const kotor::EntityMesh* own = nullptr;
        for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == participant && mesh.model && !mesh.attachModel) {
                own = &mesh;
                break;
            }
        }
        const std::shared_ptr<kotor::Mdl> clips = kotor::loadWorldModel(install_, stunt.model);
        std::size_t stand = static_cast<std::size_t>(-1);
        if (own != nullptr && clips && clips->hasProxyBody()) {
            const std::string body = scene_.entities[participant].model;
            const std::shared_ptr<kotor::Mdl> head = own->headModel;
            const std::string headNode = own->headNode;
            stand = kotor::spawnStuntBody(scene_, install_, body, head, headNode, *clips);
            own = nullptr; // spawning moved the mesh list
        } else {
            stand = kotor::spawnModelEntity(scene_, install_, stunt.model, {0.0f, 0.0f, 0.0f}, 0.0f);
        }
        logInfo(concat("dialogue stunt: ", stunt.participant, " entity ", participant, " model ", stunt.model,
                       " proxy body ", clips && clips->hasProxyBody() ? 1 : 0, " stand ",
                       stand == static_cast<std::size_t>(-1) ? std::string("none") : std::to_string(stand)));
        if (stand == static_cast<std::size_t>(-1)) {
            continue;
        }
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == stand) {
                mesh.clipPlaced = true;
            }
        }
        while (entityObjects_.size() < scene_.entities.size()) {
            entityObjects_.push_back(0);
        }
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == participant) {
                mesh.visible = false;
            }
        }
        dialogueStunts_.push_back({participant, stand});
    }
    if (!dialogueStunts_.empty()) {
        ++animationRevision_;
    }
}

void GameSession::removeDialogueStunts()
{
    for (const DialogueStunt& stunt : dialogueStunts_) {
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == stunt.participant) {
                mesh.visible = true;
            } else if (mesh.entity == stunt.stand) {
                mesh.visible = false;
            }
        }
    }
    if (!dialogueStunts_.empty()) {
        dialogueStunts_.clear();
        ++animationRevision_;
    }
}

void GameSession::restoreDialogueAnimations()
{
    for (const SavedAnimation& saved : dialogueSavedAnimations_) {
        if (saved.name.empty()) {
            stopAnimation(saved.entity);
        } else {
            playAnimation(saved.entity, saved.name, saved.loop);
        }
    }
    dialogueSavedAnimations_.clear();
}

// Retail reader 0059f5f0 lists the per-node fields (VO_ResRef, Sound, FadeType and its
// colour, delay and length, AnimList). The reader is evidence for the fields; what the
// engine does with them is inferred from the shipped values: the voice plays on the Voice
// bus and drives the lip track, FadeType 1 holds the colour, 3 fades in from it, 4 fades out
// to it (they open and close scenes in the shipped files), and an AnimList id is a
// dialoganimations.2da row (id below 1200, or id - 10000) or a cutscene clip (1200 and up).
void GameSession::playDialoguePresentation(const kotor::DlgNodePresentation& node,
                                           const std::string& fallbackSound)
{
    dialogueEntryAge_ = 0.0f;
    dialogueVoiceStarted_ = false;
    dialogueLineSkipped_ = false;
    restoreDialogueAnimations();
    const std::string voice = !node.voResRef.empty() ? node.voResRef : fallbackSound;
    activeLip_.reset();
    lipTime_ = 0.0f;
    if (audio_) {
        audio_->mixer().stopBus(audio::Bus::Voice);
        if (!voice.empty()) {
            dialogueVoiceStarted_ = audio_->playVoice(voice);
        }
        if (!node.voResRef.empty() && !fallbackSound.empty() && fallbackSound != node.voResRef) {
            audio_->playEffect(fallbackSound);
        }
    }
    if (!voice.empty()) {
        if (auto lipData = install_.find(voice, kotor::resType(kotor::ResType::Lip));
            lipData.has_value()) {
            try {
                activeLip_ = kotor::parseLip(*lipData, voice + ".lip");
            } catch (const kotor::FormatError&) {
                activeLip_.reset();
            }
        }
    }
    if (node.fadeType >= 1 && node.fadeType <= 4) {
        FadeState fade;
        fade.color = {node.fadeColor[0], node.fadeColor[1], node.fadeColor[2]};
        fade.wait = std::max(0.0f, node.fadeDelay);
        fade.length = std::max(0.0f, node.fadeLength);
        if (node.fadeType == 3) {
            fade.mode = FadeState::Mode::In;
            fade.opacity = 1.0f;
        } else {
            fade.mode = FadeState::Mode::Out;
            if (node.fadeType == 1) {
                fade.wait = 0.0f;
                fade.length = 0.0f;
            }
        }
        sim_->fade = fade;
    }
    std::vector<std::size_t> taken; // the bodies the node's AnimList names
    for (const kotor::DlgAnim& item : node.animList) {
        std::size_t target = static_cast<std::size_t>(-1);
        if (item.participant.empty() || item.participant == "PLAYER"
            || item.participant == "Player") {
            target = controlledEntity();
        } else {
            target = findEntity(item.participant);
        }
        if (target == static_cast<std::size_t>(-1)) {
            continue;
        }
        target = dialogueStuntFor(target);
        taken.push_back(target);
        std::string name;
        bool loop = false;
        if (item.animation >= 1200 && item.animation < 10000) {
            char clip[16]{};
            std::snprintf(clip, sizeof(clip), "CUT%03dW", item.animation - 1199);
            name = clip;
        } else {
            name = rules_.dialogAnimationName(item.animation);
            loop = rules_.dialogAnimationLooping(item.animation);
        }
        if (name.empty()) {
            continue;
        }
        const std::string before = animationName(target);
        const kotor::EntityMesh* beforeMesh = findEntityMesh(scene_, target);
        const bool beforeLoop = beforeMesh == nullptr || beforeMesh->animationLoop;
        const bool known = std::any_of(
            dialogueSavedAnimations_.begin(), dialogueSavedAnimations_.end(),
            [&](const SavedAnimation& saved) { return saved.entity == target; });
        if (playAnimation(target, name, loop) && !known) {
            dialogueSavedAnimations_.push_back({target, before, beforeLoop});
        }
    }
    // A speaker or listener the node's AnimList does not name gets the default (CGuiInGame 006313a0): the speaker
    // plays dialoganimations row 0x2736-10000 (Talk_Normal), the listener row 0x272e-10000 (Listen), and each its
    // injured twin (0x27aa Talk_Injured, 0x27ab Listen_Injured) when the creature is under 0.2 of its hit points
    // (00614b60 -> 004eff30, constant 007a1b38). The clips loop (the rows' looping flag) until the next node's
    // presentation restores what was playing. A node with no text and no voice is no spoken line (entry 54's
    // pause while Trask crosses the room): its speaker is about to run, so it keeps its own clip.
    if (dialogueView_.text.empty() && voice.empty()) {
        return;
    }
    const auto entityOf = [&](std::uint32_t object) {
        if (object == script::kObjectInvalid) {
            return static_cast<std::size_t>(-1);
        }
        if (object == scriptWorld_.pc || object == controlledObject()) {
            return controlledEntity();
        }
        return ScriptApi{*this}.entityFor(object);
    };
    const auto playDefault = [&](std::uint32_t object, std::int32_t healthy, std::int32_t injured) {
        std::size_t index = entityOf(object);
        if (index >= scene_.entities.size() || scene_.entities[index].kind != kotor::EntityKind::Creature) {
            return;
        }
        index = dialogueStuntFor(index);
        if (index >= scene_.entities.size() || std::find(taken.begin(), taken.end(), index) != taken.end()) {
            return;
        }
        const kotor::Entity& entity = scene_.entities[index];
        const bool player = index == playerEntity_;
        const std::int32_t hp = player ? playerHp_ : entity.hitPoints;
        const std::int32_t maxHp = player ? playerMaxHp_ : entity.maxHitPoints;
        const bool hurt = !entity.dead && maxHp > 0 && static_cast<float>(hp) / static_cast<float>(maxHp) < 0.2f;
        const std::int32_t id = hurt ? injured : healthy;
        const std::string name = rules_.dialogAnimationName(id);
        if (name.empty()) {
            return;
        }
        const std::string before = animationName(index);
        const kotor::EntityMesh* beforeMesh = findEntityMesh(scene_, index);
        const bool beforeLoop = beforeMesh == nullptr || beforeMesh->animationLoop;
        const bool known = std::any_of(
            dialogueSavedAnimations_.begin(), dialogueSavedAnimations_.end(),
            [&](const SavedAnimation& saved) { return saved.entity == index; });
        const bool played = playAnimation(index, name, rules_.dialogAnimationLooping(id));
        if (played && !known) {
            dialogueSavedAnimations_.push_back({index, before, beforeLoop});
        }
        if (!played) {
            logInfo(concat("dialogue default clip missing: ", entity.tag, " ", name));
        }
    };
    playDefault(dialogueView_.listenerObject, 10030, 10155);
    playDefault(dialogueView_.speakerObject, 10038, 10154);
}

// Auto-advance of entries that only continue (the application opts in).
void GameSession::tickDialogueAdvance(float dt)
{
    if (!dialogueView_.active || !activeDialogue_.has_value()) {
        return;
    }
    dialogueEntryAge_ += dt;
    if (!dialogueAutoAdvance_ || sim_->dialoguePaused || dialogueView_.options.size() != 1) {
        return;
    }
    // A pause the entry's script queued but its actor has not reached yet (end_trask01's k_pend_traskdl40:
    // pause, walk to the PC, resume) holds the entry the same as one that ran: retail shows nothing
    // between the textless entry and the line while Trask crosses the room (retail walk 0098 wait00).
    for (const auto& [id, actor] : sim_->actors) {
        (void)id;
        for (const ActorAction& action : actor.queue) {
            if (action.kind == ActorActionKind::PauseConversation) {
                return;
            }
        }
    }
    const std::uint32_t reply = dialogueView_.options.front().reply;
    if (reply != UINT32_MAX
        && (reply >= activeDialogue_->dlg.replies.size()
            || !activeDialogue_->dlg.replies[reply].text.empty())) {
        return;
    }
    if (!dialogueLineSpeaking()) {
        chooseDialogueOption(0);
    }
}

bool GameSession::dialogueLineSpeaking() const
{
    if (!dialogueView_.active || !activeDialogue_.has_value() || dialogueLineSkipped_) {
        return false;
    }
    const bool voiceBusy = audio_ && dialogueVoiceStarted_
        && (audio_->mixer().activeVoices(audio::Bus::Voice) > 0);
    return dialogueVoiceStarted_ ? voiceBusy || dialogueEntryAge_ <= 0.1f : dialogueEntryAge_ < dialogueHold_;
}

void GameSession::skipDialogueLine()
{
    if (!dialogueLineSpeaking()) {
        return;
    }
    dialogueLineSkipped_ = true;
    if (audio_) {
        audio_->mixer().stopBus(audio::Bus::Voice);
    }
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
    // The angle was settled when the list was offered, and a spoken reply is the PC's line
    // (logged at retail 006bd260: speaker PC, listener the NPC, on end_trask01's "Okay").
    dialogueView_.cameraAngle = activeDialogue_->shots.offerReply(replyIndex, reply.cameraAngle);
    if (!reply.text.empty()) {
        dialogueView_.listenerObject = dialogueView_.speakerObject;
        dialogueView_.speakerObject = controlledObject();
        dialogueView_.shotOnListener = false;
    }
    dialogueView_.camVidEffect = reply.camVidEffect;
    // Replies normally carry no camera and keep the entry's; only a reply with
    // its own static camera runs the set-up that applies an effect (inferred:
    // retail's per-node set-up for replies without a camera is not traced).
    if (reply.cameraAngle == 6 && reply.cameraId >= 0) {
        videoEffects_.applyDialogueNode(install_, reply.camVidEffect, true);
    }
    dialogueView_.camHeightOffset = reply.camHeightOffset;
    dialogueView_.tarHeightOffset = reply.tarHeightOffset;
    if (reply.camFieldOfView > 0.0f) {
        dialogueView_.camFieldOfView = reply.camFieldOfView;
    }
    if (reply.cameraAnimation >= 0) {
        setDialogueCameraClip(reply.cameraAnimation);
    }
    playDialoguePresentation(reply, reply.sound);
    if (!reply.text.empty()) {
        faceDialogueParticipants();
        spokenReplies_.push_back(playerName_ + ": " + dialogueView_.options[optionIndex].text);
    }
    dialogueHold_ = 0.0f;
    const std::uint32_t generation = dialogueGeneration_;
    if (traceDialogueTrail_) {
        dialogueTrail_.emplace_back(true, replyIndex);
    }
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

std::string GameSession::roomNameAt(kotor::Vec3 point)
{
    nav::NavMesh* mesh = navMesh();
    if (mesh == nullptr || mesh->empty()) {
        return {};
    }
    const int tri = mesh->locate(point);
    if (tri < 0) {
        return {};
    }
    const int room = mesh->tris()[static_cast<std::size_t>(tri)].room;
    return room >= 0 && static_cast<std::size_t>(room) < scene_.rooms.size()
        ? scene_.rooms[static_cast<std::size_t>(room)].room
        : std::string{};
}

// RETAIL-PROVEN: when the client finishes loading an area (00602eb0, the branch at 006031be) it
// fades in from black, wait 0.5 s over 1.0 s (0062abf0 mode 1), unless a conversation is
// running (client +0xb4, set by 0062ec60 on dialogue start) or a fade-in is already owed (+0xb98);
// then it only sets +0xb98, and the fade-in waits for the conversation's end (0062ec60(0): wait 0
// over 1.0 s; 004cf490 does the same when a conversation fails to start). The server runs the
// arrival first, so a trigger the PC lands in has already started its conversation by then:
// tar_m03af's tar03_postrace (k_ptar_postswoop) blacks the screen with SetGlobalFadeOut(0, 0) and
// starts the mechanic's conversation, and the area fades in when that conversation ends.
// Here the load completes after the first world update in the new module.
void GameSession::completeArrival()
{
    if (dialogueView_.active || activeDialogue_.has_value() || deferredFadeIn_) {
        deferredFadeIn_ = true;
        return;
    }
    FadeState fade;
    fade.mode = FadeState::Mode::In;
    fade.wait = 0.5f;
    fade.length = 1.0f;
    fade.opacity = 1.0f;
    sim_->fade = fade;
}

void GameSession::clearDialogueState()
{
    if (deferredFadeIn_) {
        // 0062ec60(0): the owed fade-in, from black over 1.0 s.
        deferredFadeIn_ = false;
        FadeState fade;
        fade.mode = FadeState::Mode::In;
        fade.length = 1.0f;
        fade.opacity = 1.0f;
        sim_->fade = fade;
    }
    // Retail 006332b0: taking the dialogue camera down disables the video effect, on every way out.
    videoEffects_.disable();
    activeDialogue_.reset();
    dialogueView_ = {};
    headFollow_.clear();
    sim_->dialoguePaused = false;
    ++dialogueGeneration_;
    dialogueCameraModel_.reset();
    dialogueCameraClip_.clear();
    dialogueCameraTime_ = 0.0f;
    dialogueCameraDuration_ = 0.0f;
    activeLip_.reset();
    lipTime_ = 0.0f;
    restoreDialogueAnimations();
    removeDialogueStunts();
    dialogueEntryAge_ = 0.0f;
    dialogueVoiceStarted_ = false;
    if (dialogueTrackStarted_ && audio_) {
        audio_->stopConversationTrack();
    }
    dialogueTrackStarted_ = false;
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
    // Retail 006332b0 (dialogue camera teardown) disables the video effect first.
    videoEffects_.disable();
    clearDialogueState();
    // 006200e0 mode 0 (the camera leaves the conversation): the chase camera comes back behind the PC. Read
    // back at end_trask01: the PC turns to 34.75 degrees (Trask's bearing 3.75 plus the 30 degree head arc plus
    // one) and retail's HUD camera looks along 34.8 degrees (retail walk 0132 trask_removed).
    if (controlledObject() == scriptWorld_.pc) {
        yaw_ = playerBearing_;
    }
    if (!script.empty()) {
        runScriptWith(script, owner, context);
    }
    ScriptApi{*this}.dispatchHook(owner, "ScriptEndDialogu", context);
    // A placeable's own field (retail 005860ed loads it, 005a0b30 -> 00585460 runs it).
    ScriptApi{*this}.dispatchHook(owner, "OnEndDialogue", context);
}

void GameSession::endDialogue()
{
    finishDialogue(true);
}

const MapFog* GameSession::mapFog() const
{
    const kotor::Scene::Minimap& map = scene_.minimap;
    if (!map.present || scene_.area.empty()) {
        return nullptr;
    }
    MapFog& fog = mapFogs_[scene_.area];
    if (fog.cols == 0) {
        fog.cols = map.resX <= 0 || map.resX > 88 ? 88 : map.resX; // retail 00578c60: 0 or over 88 is 88
        fog.rows = std::max(1, fog.cols * 256 / 440);
        fog.seen.assign(static_cast<std::size_t>(fog.cols + 1) * static_cast<std::size_t>(fog.rows + 1), char{0});
    }
    return &fog;
}

bool GameSession::mapPointSeen(const kotor::Vec3& at) const
{
    const MapFog* fog = mapFog();
    float u = 0.0f;
    float v = 0.0f;
    if (fog == nullptr || !scene_.minimap.project(at.x, at.y, u, v)) {
        return true;
    }
    // Retail 00579210: whole map pixel first (00578e00), then its grid cell (00578f10).
    const int px = static_cast<int>(u);
    const int py = static_cast<int>(v);
    const int width = static_cast<int>(kotor::Scene::Minimap::kViewWidth);
    const int height = static_cast<int>(kotor::Scene::Minimap::kViewHeight);
    if (px < 0 || py < 0 || px > width || py > height) {
        return false;
    }
    // 00578f10 turns the map pixel into the NEAREST grid point (the fog grid has cols + 1 by rows + 1
    // points, and the dots 00688100 draws sit on them). Retail's explored plus after the PC stands at
    // (15.4, 20.1) in end_m01aa is centred on point (2, 3), where the pixel (44, 83) lies in cell
    // (1, 2): retail's own cell reads at the three route frames (minimap_reads_all.jsonl).
    const int col = (2 * px * fog->cols + width) / (2 * width);
    const int row = (2 * py * fog->rows + height) / (2 * height);
    if (col < 0 || row < 0 || col > fog->cols || row > fog->rows) {
        return false;
    }
    return fog->seen[static_cast<std::size_t>(row) * static_cast<std::size_t>(fog->cols + 1) + static_cast<std::size_t>(col)] != 0;
}

void GameSession::markMapAt(const kotor::Vec3& at, int radius)
{
    if (mapFog() == nullptr) {
        return;
    }
    MapFog& fog = mapFogs_[scene_.area];
    if (radius < 0) {
        std::fill(fog.seen.begin(), fog.seen.end(), char{1});
        // Retail 00578fa0 fills the whole word array with 0xff.
        fog.words.assign(fog.seen.size() / 32u + 1u, 0xFFFFFFFFu);
        return;
    }
    float u = 0.0f;
    float v = 0.0f;
    if (!scene_.minimap.project(at.x, at.y, u, v)) {
        return;
    }
    // Retail 005792d0: a diamond of map pixels around the point, stepped by whole grid cells in
    // pixels (440 / ResX, 256 / ResY), each clamped to the 440x256 map and turned into its grid cell
    // (00579000 -> 00578f10). The map pixel itself is a whole number (00578e00).
    const int width = static_cast<int>(kotor::Scene::Minimap::kViewWidth);
    const int height = static_cast<int>(kotor::Scene::Minimap::kViewHeight);
    const int px = static_cast<int>(u);
    const int py = static_cast<int>(v);
    if (px < 0 || px > width || py < 0 || py > height) {
        return;
    }
    const int stepX = width / std::max(fog.cols, 1);
    const int stepY = height / std::max(fog.rows, 1);
    for (int dx = -radius; dx <= radius; ++dx) {
        const int reach = radius - std::abs(dx);
        for (int dy = -reach; dy <= reach; ++dy) {
            const int x = std::clamp(px + dx * stepX, 0, width);
            const int y = std::clamp(py + dy * stepY, 0, height);
            const int c = std::min((2 * x * fog.cols + width) / (2 * width), fog.cols); // nearest point, 00578f10
            const int r = std::min((2 * y * fog.rows + height) / (2 * height), fog.rows);
            fog.seen[static_cast<std::size_t>(r) * static_cast<std::size_t>(fog.cols + 1) + static_cast<std::size_t>(c)] = 1;
        }
    }
}

void GameSession::floatingText(std::uint32_t object, int type, std::int32_t value)
{
    if ((feedbackOptions_ & 0x10u) == 0u) {
        return;
    }
    // Retail 006027c0: strings, durations (1.5 s, 3 s for XP and level) and colours. The colours are the static
    // vectors in .data at 007a23a8 (white), 007a23d8 (damage), 007a23e4 (heal), 007a23f0 (level), 007a23fc (XP),
    // read from the file's initialised data.
    FloatingText line;
    line.object = object;
    const auto word = [this](std::uint32_t strref) {
        return localization_ ? localization_->tlk().text(strref) : std::string{};
    };
    switch (type) {
    case 0:
        line.text = std::to_string(value);
        line.color = {0.74f, 0.11f, 0.0f};
        break;
    case 1:
        line.text = std::to_string(value);
        line.color = {0.28f, 0.92f, 0.11f};
        break;
    case 2:
        line.text = word(1373);
        break;
    case 3:
        line.text = word(38551) + " " + std::to_string(value);
        line.color = {0.95f, 0.0f, 0.85f};
        line.seconds = 3.0f;
        break;
    case 4:
        line.text = word(32154) + " " + std::to_string(value);
        line.color = {0.98f, 0.45f, 0.0f};
        line.seconds = 3.0f;
        break;
    case 5:
        line.text = word(1391);
        break;
    case 6:
        line.text = word(42403);
        break;
    default:
        return;
    }
    // 0068b7c0: this object's older lines move up one; the sixth goes.
    for (std::size_t i = floating_.size(); i-- > 0;) {
        if (floating_[i].object == object && ++floating_[i].line >= 6) {
            floating_.erase(floating_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    floating_.push_back(std::move(line));
}

bool GameSession::floatingAnchor(std::uint32_t object, kotor::Vec3& at)
{
    ScriptApi api{*this};
    const std::size_t index = api.entityFor(object);
    if (index < scene_.entities.size()) {
        at = scene_.entities[index].position;
    } else if (object == scriptWorld_.pc) {
        at = position();
    } else {
        return false;
    }
    at.z += 2.0f; // inferred: the retail frame stands the line just over the head
    return true;
}

void GameSession::statusFeedback(int kind, std::int32_t value)
{
    if (statusSuppress_ > 0) {
        --statusSuppress_;
        return;
    }
    StatusSummary& s = statusPending_;
    switch (kind) {
    case 0:
        s.journal = true;
        break;
    case 1:
        if (value != 0 && ((s.credits > 0 && value < 0) || (s.credits < 0 && value > 0))) {
            s.creditsBothWays = true;
        }
        s.credits += value;
        break;
    case 2:
        s.xp += value;
        break;
    case 3:
        s.stealthXp += value;
        break;
    case 4:
    case 5:
        (value > 0 ? s.light : s.dark) += std::abs(value);
        break;
    case 7:
        s.received = true;
        break;
    case 8:
        s.lost = true;
        break;
    default:
        return;
    }
    statusPopupOwed_ = s.any();
}

void GameSession::stepFeedback(float dt, bool blocked)
{
    for (std::size_t i = floating_.size(); i-- > 0;) {
        floating_[i].age += dt;
        if (floating_[i].age >= floating_[i].seconds) {
            floating_.erase(floating_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (float& icon : feedbackIcons_) {
        icon = std::max(0.0f, icon - dt);
    }
    if (blocked || !statusPopupOwed_) {
        statusWait_ = 0.0f;
        return;
    }
    statusWait_ += dt;
    if (statusWait_ < 0.25f) {
        return;
    }
    statusWait_ = 0.0f;
    statusPopupOwed_ = false;
    const StatusSummary s = statusPending_;
    statusPending_ = {};
    if ((feedbackOptions_ & 0x20u) != 0u && !statusHeld_) {
        statusShown_ = s;
        statusOpen_ = true;
        // The summary's build (00625c60, reached from 0062ef90's 0040bc70) ends by queueing tutorial
        // row 14 (005edf40 -> 005f4120); with the option off 0062ef90 only raises the icons and no row.
        showTutorial(14);
    }
    // 0062ef90's loop and 00625c60's both raise the icon of each row; 00687fe0 gives it four seconds. 4 and 5 go to
    // the larger of the two shifts only.
    const bool raised[9] = {s.journal, s.credits != 0, s.xp != 0, s.stealthXp != 0, s.light < s.dark, s.dark < s.light,
                            false, s.received, s.lost};
    for (std::size_t i = 0; i < feedbackIcons_.size(); ++i) {
        if (raised[i]) {
            feedbackIcons_[i] = 4.0f;
        }
    }
}

void GameSession::feedback(const std::string& line)
{
    if (line.empty()) {
        return;
    }
    feedbackLog_.push_back(line);
    if (feedbackLog_.size() > 200) {
        feedbackLog_.erase(feedbackLog_.begin());
    }
}

void GameSession::cancelCombat()
{
    combatTarget_ = UINT32_MAX;
    // Retail's Clear (0068b0a0 -> ClearAllActions 006887d0) clears combat mode and the actions and
    // resets two timers; it does not touch the selected object, so the target block stays on it.
    ScriptApi{*this}.clearQueue(controlledObject());
}

void GameSession::clearSelection()
{
    selectedEntity_ = UINT32_MAX; // 005f9c60 with no object: SetTarget(invalid) (0062b000, 006855f0)
}

bool GameSession::controlledDebilitated()
{
    return !controlledDead() && isDebilitated(ScriptApi{*this}, controlledObject());
}

std::size_t GameSession::controlledQueueSize() const
{
    return actorQueueSize(controlledObject());
}

void GameSession::attackNearestHostile()
{
    leaveStealth();
    // The attack command belongs to the controlled creature.
    const std::uint32_t attacker = controlledObject();
    const kotor::Vec3 attackerAt = controlledPosition();
    if (controlledDead()) {
        return;
    }
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 20.0f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Creature || entity.dead
            || !isEnemy(ScriptApi{*this}, attacker, objectForEntity(i))) {
            continue;
        }
        const float distance = distance2D(attackerAt, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    if (best == static_cast<std::size_t>(-1)) {
        logEvent("no hostile target");
        return;
    }
    attackEntity(best);
}

// Retail 004f2c30 CSWSCreature::ValidNearestObjectType, which 004fc4c0 asks of every object near the
// scanner. A creature counts when it is alive and the scanner can perceive it (the line of sight is asked
// later, as retail's flag at +4 of the +0x2a8 entries is); a door when its Static flag (+0x3c0) is clear and
// it is closed (+0x2cc, the state it keeps, is 0); a placeable when it is Useable (+0x328). hostile is the
// creature's standing against the controlled creature.
// NOT YET: the trigger case (a detected trap or mine) and the creature perception rules.
bool GameSession::targetableKind(std::size_t index) const
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    const kotor::Entity& entity = scene_.entities[index];
    switch (entity.kind) {
    case kotor::EntityKind::Creature:
        return !entity.dead && index != controlledEntity();
    case kotor::EntityKind::Door:
        return !entity.isStatic && entity.openState == 0 && !entity.open && !entity.dead;
    case kotor::EntityKind::Placeable:
        return entity.useable && !entity.dead;
    default:
        return false;
    }
}

bool GameSession::targetableEntity(std::size_t index, bool& hostile)
{
    hostile = false;
    if (!targetableKind(index)) {
        return false;
    }
    if (scene_.entities[index].kind == kotor::EntityKind::Creature) {
        if (objectForEntity(index) == controlledObject()) {
            return false;
        }
        hostile = isEnemy(ScriptApi{*this}, controlledObject(), objectForEntity(index));
    }
    return true;
}

// Retail 004fc4c0 (the lists 005fa5a0 asks for with 30.0 for both ranges). Every valid object whose squared
// distance (3D, from the scanner's position) is under 900 goes into the angular list at the place its
// bearing turn gives: bearing = atan2(-x, y) in degrees (Yaw 004aa0f0, 57.2958 at 00742ef8) of the offset
// minus the same of the scanner's orientation vector (+0x9c), wrapped into [0, 359] (360.0 at 00741868,
// 359.0 at 007471b8); an equal turn goes after the ones already there. The forward list takes the objects
// whose offset has a non-negative dot with the orientation and whose direction from a point 4 m behind
// the scanner (007471bc) has a dot above 0.866 (007471b0) with it, ascending by squared distance.
// NOT TRACED: the order retail walks the area's objects in (its nearest-first array +0x190 from the
// scanner's own slot); entity order stands in.
void GameSession::buildTargetLists(std::vector<TargetCandidate>& angular, std::vector<std::size_t>& forward)
{
    angular.clear();
    forward.clear();
    const kotor::Vec3 at = controlledPosition();
    const float bearing = controlledBearing();
    const float fx = std::cos(bearing);
    const float fy = std::sin(bearing);
    const float faceDegrees = std::atan2(-fx, fy) * 57.29578f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        const float dx = entity.position.x - at.x;
        const float dy = entity.position.y - at.y;
        const float dz = entity.position.z - at.z;
        const float squared = dx * dx + dy * dy + dz * dz;
        bool hostile = false;
        if (!(squared < 30.0f * 30.0f) || !targetableEntity(i, hostile)) {
            continue;
        }
        float turn = 0.0f;
        if (dx != 0.0f || dy != 0.0f) {
            turn = std::atan2(-dx, dy) * 57.29578f - faceDegrees;
        }
        while (turn < 0.0f) {
            turn += 360.0f;
        }
        while (turn > 359.0f) {
            turn -= 360.0f;
        }
        TargetCandidate candidate;
        candidate.index = i;
        candidate.turn = turn;
        candidate.squared = squared;
        candidate.hostile = hostile;
        const auto place = std::find_if(angular.begin(), angular.end(), [&](const TargetCandidate& other) { return turn < other.turn; });
        angular.insert(place, candidate);
    }
    std::vector<std::pair<float, std::size_t>> ahead;
    for (std::size_t slot = 0; slot < angular.size(); ++slot) {
        const kotor::Entity& entity = scene_.entities[angular[slot].index];
        const float dx = entity.position.x - at.x;
        const float dy = entity.position.y - at.y;
        if (dx * fx + dy * fy < 0.0f) {
            continue;
        }
        const float bx = entity.position.x - (fx * -4.0f + at.x);
        const float by = entity.position.y - (fy * -4.0f + at.y);
        const float bz = entity.position.z - at.z;
        const float length = std::sqrt(bx * bx + by * by + bz * bz);
        if (length > 0.0f && (bx * fx + by * fy) / length > 0.866f) {
            ahead.emplace_back(angular[slot].squared, slot);
        }
    }
    std::stable_sort(ahead.begin(), ahead.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : ahead) {
        forward.push_back(entry.second);
    }
}

// 00617ad0 CanSee asks a scene hit test from the scanner to the object with both objects' own geometry
// left out (its first lines push both ids on the ignore list); a closed door in the way blocks and a
// placeable is ignored and the test repeated. Here the walkmesh wall test stands in, and the ray for a door
// or placeable stops 0.6 m short of it.
bool GameSession::targetVisible(std::size_t index)
{
    if (index >= scene_.entities.size()) {
        return false;
    }
    const kotor::Entity& entity = scene_.entities[index];
    const kotor::Vec3 at = controlledPosition();
    const kotor::Vec3 eye{at.x, at.y, at.z + 1.5f};
    kotor::Vec3 to{entity.position.x, entity.position.y, entity.position.z + 1.5f};
    if (entity.kind != kotor::EntityKind::Creature) {
        const float dx = to.x - eye.x;
        const float dy = to.y - eye.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 0.6f) {
            to.x -= dx / length * 0.6f;
            to.y -= dy / length * 0.6f;
        }
    }
    return lineOfSight(eye, to);
}

// Retail 005fa5a0, the client's per-frame scan (the lists above, 30 m for both ranges, entries kept at +0x2a8):
//  - While no enemy has been in sight (+0x324 clear), the first visible hostile creature in the angular list
//    sets it and, with the Enemy Sighted autopause option (bit 0x1000, the [Autopause Options] "Enemy
//    Sighted" key read at 0061c4f9), pauses with reason 1 (SetAutoPaused 005f3f10); the same branch
//    (005faa6d..005faaa5) makes that hostile the selected object (+0x2b4). The flag stays while a hostile is
//    in sight and clears 10 s (+0x394 = 10.0) after the last one.
//  - A selected object that is still in the angular list is kept; with no line of sight (00617ad0) for a
//    second (+0x368 against 1.0 at 0073d764) it is let go and a new one is taken.
//  - With nothing selected: out of combat (+0x320 clear) or with no hostile creature in the list, the first
//    object of the forward list the line of sight passes, whatever it is (a door, a container, a friendly
//    creature: this is why the Sealed Door is selected after the swoop run-in, paired frame
//    paired/retail/raw/req_swoop_finish_045014.png); in combat the first hostile in the forward list the line
//    of sight passes. Then, either way, the first hostile of the angular list it passes; in combat last the
//    first non-hostile of the forward list. With none, the selection is invalid (005f9c60 with no object).
// "In combat" (+0x320, NOT TRACED) is the controlled creature having a queued action, as the HUD's combat line does.
// NOT YET: the mine case (reason 11, bit 0x2000, message 0x15) and the trap-hostile test of vtable +0x138.
void GameSession::tickEnemySighted(float dt)
{
    // The caller (00602eb0 at 00603b61) runs the scan only while the in-game fade panel (client GUI +0x6c) is idle:
    // 0062ac60 (a fade still inside its duration, 006244b0) and 0062ded0 (the panel's +0x4c above 0.001) both skip it,
    // and a skipped scan leaves the selection and the sighted flag as they were. This is why retail shows no selected
    // object for the first moments after a load.
    // The frame that completes a load is skipped as well: the client's loading flag (+0x288) is still set (00602eb0).
    if (arrivalPending_ || sim_->fade.active() || sim_->fade.opacity > 0.001f) {
        return;
    }
    std::vector<TargetCandidate> angular;
    std::vector<std::size_t> forward;
    bool seen = false;
    if (!controlledDead() && !dialogueView_.active) {
        buildTargetLists(angular, forward);
        std::size_t sighted = static_cast<std::size_t>(-1);
        bool anyHostile = false;
        for (const TargetCandidate& candidate : angular) {
            if (!candidate.hostile) {
                continue;
            }
            anyHostile = true;
            if (!seen && targetVisible(candidate.index)) {
                seen = true;
                sighted = candidate.index;
            }
        }
        bool have = std::any_of(angular.begin(), angular.end(), [&](const TargetCandidate& candidate) {
            return candidate.index == selectedEntity_;
        });
        if (seen && !enemySighted_) {
            autoPauseRequest_ = 1;
            selectedEntity_ = static_cast<std::uint32_t>(sighted);
            have = true;
            targetLosTimer_ = 0.0f;
        } else if (have) {
            if (targetVisible(selectedEntity_)) {
                targetLosTimer_ = 0.0f;
            } else {
                targetLosTimer_ += dt;
                if (targetLosTimer_ >= 1.0f) {
                    targetLosTimer_ = 0.0f;
                    have = false;
                }
            }
        }
        if (!have) {
            const bool combat = controlledQueueSize() > 0;
            std::size_t pick = static_cast<std::size_t>(-1);
            std::size_t firstFriendly = static_cast<std::size_t>(-1);
            for (const std::size_t slot : forward) {
                const TargetCandidate& candidate = angular[slot];
                if (!combat || !anyHostile) {
                    if (targetVisible(candidate.index)) {
                        pick = candidate.index;
                        break;
                    }
                } else if (candidate.hostile) {
                    if (targetVisible(candidate.index)) {
                        pick = candidate.index;
                        break;
                    }
                } else if (firstFriendly == static_cast<std::size_t>(-1)) {
                    firstFriendly = candidate.index;
                }
            }
            if (pick == static_cast<std::size_t>(-1)) {
                for (const TargetCandidate& candidate : angular) {
                    if (candidate.hostile && targetVisible(candidate.index)) {
                        pick = candidate.index;
                        break;
                    }
                }
            }
            if (pick == static_cast<std::size_t>(-1) && combat && anyHostile) {
                pick = firstFriendly;
            }
            selectedEntity_ = pick == static_cast<std::size_t>(-1) ? UINT32_MAX : static_cast<std::uint32_t>(pick);
            targetLosTimer_ = 0.0f;
        }
    }
    if (seen) {
        enemySighted_ = true;
        enemySightedTimer_ = 10.0f;
    } else if (enemySighted_) {
        enemySightedTimer_ -= dt;
        if (enemySightedTimer_ < 0.0f) {
            enemySighted_ = false;
        }
    }
}

std::size_t GameSession::hostilesInReach()
{
    if (controlledDead()) {
        return 0;
    }
    const std::uint32_t attacker = controlledObject();
    const kotor::Vec3 attackerAt = controlledPosition();
    std::size_t count = 0;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind == kotor::EntityKind::Creature && !entity.dead
            && distance2D(attackerAt, entity.position) < 20.0f && isEnemy(ScriptApi{*this}, attacker, objectForEntity(i))) {
            ++count;
        }
    }
    return count;
}

// keymap.2da rows 204/205 (retail 00621210 cases 0xcc and 0xcd) call 005fb050(direction), which only moves the
// selection: no attack is ordered. It walks the angular list 005fa5a0 keeps at client +0x2a8 (every object
// within 30 m sorted by its counter-clockwise turn from the controlled creature's facing, see 004fc4c0:
// creatures, doors and placeables) keeping the entries the line-of-sight flag (00617ad0) passes. From the
// selected object's slot Previous takes the slot before (the last one from slot 0) and Next the slot after
// (slot 0 from the last); with nothing selected, or the selection out of the list, Previous takes the last
// slot and Next the first.
// NOT YET: the paused-game variant (mode 2 of 005fb050, which also needs the +0x138 test).
void GameSession::cycleCombatTarget(int direction)
{
    if (controlledDead()) {
        return;
    }
    std::vector<TargetCandidate> angular;
    std::vector<std::size_t> forward;
    buildTargetLists(angular, forward);
    std::vector<std::size_t> list;
    for (const TargetCandidate& candidate : angular) {
        if (targetVisible(candidate.index)) {
            list.push_back(candidate.index);
        }
    }
    if (list.empty()) {
        return;
    }
    const std::uint32_t current = selectedEntity_;
    std::size_t slot = list.size();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] == current) {
            slot = i;
        }
    }
    std::size_t pick = 0;
    if (slot >= list.size()) {
        pick = direction < 0 ? list.size() - 1 : 0;
    } else if (direction < 0) {
        pick = slot == 0 ? list.size() - 1 : slot - 1;
    } else {
        pick = slot + 1 < list.size() ? slot + 1 : 0;
    }
    selectedEntity_ = static_cast<std::uint32_t>(list[pick]);
    targetLosTimer_ = 0.0f;
}

void GameSession::attackEntity(std::size_t best)
{
    const std::uint32_t attacker = controlledObject();
    combatTarget_ = static_cast<std::uint32_t>(best);
    selectedEntity_ = combatTarget_; // the attacked creature becomes the selected object
    // The attack command: an Attack action the player can cancel by steering. It
    // walks to range and runs rounds until the target falls (ScriptCombat.cpp).
    {
        ScriptApi api{*this};
        std::erase_if(api.actor(attacker).queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
        ActorAction attack;
        attack.kind = ActorActionKind::Attack;
        attack.object = objectForEntity(best);
        attack.userIssued = true;
        api.enqueue(attacker, std::move(attack));
    }
    logEvent("combat target " + combatTargetName());
    const kotor::Entity& target = scene_.entities[best];
    const float distance = distance2D(position_, target.position);
    const std::int32_t jump = rules_.forceJumpBonus(character_);
    // The Force jump is the PC's own feat and sheet.
    if (!companionControlled() && distance > 10.0f && jump > 0 && rules_.hasLightsaber(character_)) {
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

std::uint32_t GameSession::hudTarget() const
{
    if (selectedEntity_ >= scene_.entities.size() || selectedEntity_ == controlledEntity()) {
        return combatTarget_ < scene_.entities.size() ? combatTarget_ : UINT32_MAX;
    }
    // The selected object while it is something the scan takes in: a living creature, a closed door, a
    // useable placeable (004f2c30); anything else falls back to the attack target.
    if (!targetableKind(selectedEntity_)) {
        return combatTarget_ < scene_.entities.size() ? combatTarget_ : UINT32_MAX;
    }
    return selectedEntity_;
}

std::string GameSession::hudTargetName() const
{
    const std::uint32_t index = hudTarget();
    if (index >= scene_.entities.size()) {
        return {};
    }
    const kotor::Entity& entity = scene_.entities[index];
    return entity.name.empty() ? entity.tag : entity.name;
}

float GameSession::hudTargetHealthRatio() const
{
    const std::uint32_t index = hudTarget();
    if (index >= scene_.entities.size()) {
        return 0.0f;
    }
    const kotor::Entity& entity = scene_.entities[index];
    return std::clamp(static_cast<float>(entity.hitPoints) / static_cast<float>(std::max(1, entity.maxHitPoints)),
                      0.0f, 1.0f);
}

bool GameSession::hudTargetHostile() const
{
    const std::uint32_t index = hudTarget();
    return index < scene_.entities.size() && scene_.entities[index].hostile;
}

float GameSession::combatTargetHealthRatio() const
{
    if (combatTarget_ >= scene_.entities.size()) {
        return 0.0f;
    }
    const kotor::Entity& entity = scene_.entities[combatTarget_];
    return std::clamp(static_cast<float>(entity.hitPoints) / static_cast<float>(std::max(1, entity.maxHitPoints)),
                      0.0f, 1.0f);
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
    entity.hitPoints = std::min(entity.hitPoints, 0);
    entity.dying = entity.kind == kotor::EntityKind::Creature;
    // Retail 0063eeb2: starting a death clip plays appearance.2da DeathVFX (the droids' explosion).
    if (entity.dying && effects_ && appearanceRows()) {
        const int column = appearanceHeads_->columnIndex("deathvfx");
        const std::string vfx = column >= 0 && entity.appearance < appearanceHeads_->rowCount()
            ? appearanceHeads_->cell(static_cast<std::size_t>(entity.appearance),
                                     static_cast<std::size_t>(column))
            : std::string{};
        if (!vfx.empty()) {
            EffectDirector::Spawn request;
            request.vfx = std::atoi(vfx.c_str());
            request.target = index;
            request.targetObject = objectForEntity(index);
            effects_->spawn(request);
        }
    }
    if (entity.partyAvatar) {
        showTutorial(15); // Party_Member_Dies, retail 006655e0
        // CSWSEffectListHandler::OnApplyDeath (004e0ac0): a party member's death autopauses with
        // reason 9 while another member still lives (the option bit is checked by the caller).
        bool anotherAlive = false;
        for (std::size_t other = 0; other < scene_.entities.size(); ++other) {
            anotherAlive = anotherAlive
                || (other != index && scene_.entities[other].partyAvatar && !scene_.entities[other].dead);
        }
        if (anotherAlive || !playerDead()) {
            autoPauseRequest_ = 9;
        }
    }
    if (combatTarget_ == index) {
        combatTarget_ = UINT32_MAX;
    }
    const std::uint32_t object = objectForEntity(index);
    ScriptApi api{*this};
    api.clearQueue(object);
    leaveCombatOnDeath(api, object);
    syncEntityToScriptWorld(index);
    if (!entity.partyAvatar && entity.kind == kotor::EntityKind::Creature) {
        api.actor(object).destroyDue = destroyObjectDelay(entity); // 004e0e2e, skipped for the party
    }
    const int standing = reputationBetween(api, scriptWorld_.pc, object);
    const std::int32_t award = !entity.partyAvatar && entity.kind == kotor::EntityKind::Creature && standing <= 10
        ? rules_.killXp(rules_.levelForXp(character_.xp), entity.stats.challengeRating,
                        static_cast<std::int32_t>(scriptWorld_.party.size()) + 1)
        : 0;
    logInfo(concat("kill: ", entity.tag, " standing ", standing, " cr ", entity.stats.challengeRating, " xp ", award));
    if (award > 0) {
        xpFloatObject_ = object;
        giveXp(award);
        xpFloatObject_ = 0xFFFFFFFFu;
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
    updateCorpses(dt);
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
    // combatTarget_ is the UI's view of the controlled creature's attack command.
    ActorState& steered = api.actor(controlledObject());
    const auto attacking = std::find_if(steered.queue.begin(), steered.queue.end(),
                                        [](const ActorAction& action) {
        return action.kind == ActorActionKind::Attack;
    });
    if (attacking == steered.queue.end()) {
        combatTarget_ = UINT32_MAX;
    } else if (const std::size_t index = api.entityFor(attacking->object);
               index != static_cast<std::size_t>(-1)) {
        // A newly ordered attack makes its creature the client's selected object (the target block follows
        // +0x188); after that E and Q (005fb050) move the selection while the attack goes on.
        if (combatTarget_ != static_cast<std::uint32_t>(index)) {
            selectedEntity_ = static_cast<std::uint32_t>(index);
        }
        combatTarget_ = static_cast<std::uint32_t>(index);
    }
    // Retail keeps combat rounds running through a conversation: the Endar duel ends and starts its rounds every
    // 3 s while the Trask dialogue plays (state trace rnd rows).
    tickCombat(api, dt);
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

// A linked door (CSWSDoor::GetIsLinked 005890d0: LinkedToFlags 1 or 2) that stands open is crossed by walking
// through it. Retail recording (user save tar-UpperCantina, tar02_cantdoor opened by its OnFailToOpen script
// k_ptar_sithalert, PC then ActionMoveToLocation 3 m past the door): the module changed with the PC 1.2 m past the
// door's centre line and not yet with the PC on it (0.01 m short). The retail path (a creature moving through the
// door's action area, 004bf470 -> 005897d0 InDoorArea?, 0051bb10 sending the door event 0x1f) was not traced, so
// this takes the observed side and distance: the far side of the door from where the PC approached, 1.0 m or more
// past the centre line, within 2.0 m of the door's axis.
void GameSession::updateDoorWalkThrough()
{
    if (!pendingModule_.empty() || controlLocked()) {
        return;
    }
    const kotor::Vec3 at = controlledPosition();
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& door = scene_.entities[i];
        if (door.kind != kotor::EntityKind::Door || door.linkedModule.empty()
            || (door.linkedFlags != 1 && door.linkedFlags != 2)) {
            continue;
        }
        const float nx = -std::sin(door.bearing);
        const float ny = std::cos(door.bearing);
        const float dx = at.x - door.position.x;
        const float dy = at.y - door.position.y;
        const float along = dx * nx + dy * ny;
        const float lateral = std::fabs(dx * ny - dy * nx);
        int& side = doorApproachSide_[i];
        if (lateral > 2.0f || std::fabs(along) > 6.0f) {
            side = 0;
            continue;
        }
        if (!door.open || side == 0) {
            if (std::fabs(along) > 0.3f && !door.open) {
                side = along > 0.0f ? 1 : -1;
            }
            continue;
        }
        if (along * static_cast<float>(side) <= -1.0f) {
            logEvent("door " + door.tag + ": walked through, leaving for " + door.linkedModule);
            pendingModule_ = door.linkedModule;
            sim_->pendingLinkedTo = door.linkedTo;
            return;
        }
    }
}

void GameSession::updateTriggers()
{
    if (triggerInside_.size() != scene_.triggers.size()) {
        triggerInside_.assign(scene_.triggers.size(), false);
    }
    updateDoorWalkThrough();
    for (std::size_t i = 0; i < scene_.triggers.size(); ++i) {
        const kotor::TriggerVolume& trigger = scene_.triggers[i];
        if (trigger.entity >= scene_.entities.size()) {
            continue;
        }
        if (scene_.entities[trigger.entity].dead) {
            continue;
        }
        // The creature the player steers is the one that enters and leaves (the PC,
        // or a companion once switched to). Whether retail also fires a trigger for
        // a party member that is not steered was not recovered.
        const kotor::Vec3 steeredAt = controlledPosition();
        const bool inside = pointInTrigger(trigger, steeredAt.x, steeredAt.y);
        if (inside == triggerInside_[i]) {
            continue;
        }
        triggerInside_[i] = inside;
        // Hooks can spawn entities, so nothing keeps a reference across them.
        const std::size_t index = trigger.entity;
        if (inside) {
            runHook("ScriptOnEnter", index, controlledObject());
            const kotor::Entity& entity = scene_.entities[index];
            logEvent("trigger " + entity.tag + ": enter " + entity.scriptOnEnter);
            // A trap trigger springs on the walk-in edge unless disarmed.
            if (entity.trapFlag) {
                springTrap(index, controlledObject());
            }
            // Honour LinkedToModule on the walk-in edge; the swap happens at the
            // end of the tick, never with this loop mid-flight. A KeyName that is
            // required must be held (and is consumed with AutoRemoveKey).
            if (index < scene_.entities.size()) {
                const kotor::Entity& transition = scene_.entities[index];
                const bool wantsLeave = !transition.linkedModule.empty()
                    && transition.linkedFlags != 0 && pendingModule_.empty();
                if (wantsLeave
                    && (!transition.keyRequired || transition.keyName.empty()
                        || keyUnlock(index) || playerHasKey(transition.keyName))) {
                    pendingModule_ = transition.linkedModule;
                    sim_->pendingLinkedTo = transition.linkedTo;
                } else if (wantsLeave) {
                    logEvent("trigger " + transition.tag + ": transition refused");
                }
            }
        } else {
            runHook("ScriptOnExit", index, controlledObject());
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
            if (const kotor::GffField* f = gff.field(structure, "Name");
                f != nullptr && f->type == kotor::GffType::LocString) {
                category.nameStrref = gff.locStringStrref(*f);
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
    const std::int32_t plot = plotXpByLabel(plotLabel, static_cast<double>(percent) * 0.01);
    giveXp(plot);
    statusFeedback(2, plot);
}

std::int32_t GameSession::journalQuestExperience(const std::string& plotId)
{
    const JournalCategory* category = journalCategory(plotId);
    return category != nullptr ? category->xp : 0;
}

std::string GameSession::journalCategoryName(const std::string& plotId)
{
    const JournalCategory* category = journalCategory(plotId);
    if (category == nullptr || !tlk_.has_value() || category->nameStrref == UINT32_MAX) {
        return {};
    }
    return substituteTokens(tlk_->text(category->nameStrref));
}

bool GameSession::journalQuestFinished(const std::string& plotId)
{
    const JournalCategory* category = journalCategory(plotId);
    if (category == nullptr) {
        return false;
    }
    const auto entry = category->entries.find(journalState(plotId));
    return entry != category->entries.end() && entry->second.end;
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
            const std::int32_t plot = plotXp(category->plotIndex, entry->second.xpPercentage);
            giveXp(plot);
            statusFeedback(2, plot);
        }
    }
    return true;
}

bool GameSession::addJournalEntry(const std::string& plotId, std::int32_t state)
{
    if (plotId.empty()) {
        return false;
    }
    const bool fresh = journal_.find(plotId) == journal_.end() || journal_[plotId] != state;
    journal_[plotId] = state;
    logEvent("journal " + plotId + " = " + std::to_string(state));
    if (fresh && localization_) {
        feedback(localization_->tlk().text(32296) + " " + journalCategoryName(plotId)); // "Journal Updated:"
    }
    if (fresh) {
        statusFeedback(0, 0);
    }
    return true;
}

// Quest tags compare without case (retail CExoString, 005e58c0), as journalState does.
bool GameSession::removeJournalEntry(const std::string& plotId)
{
    const std::string wanted = lower(plotId);
    for (auto it = journal_.begin(); it != journal_.end(); ++it) {
        if (lower(it->first) == wanted) {
            journal_.erase(it);
            return true;
        }
    }
    return false;
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

bool GameSession::learnLevelUpPower(std::int32_t powerId)
{
    if (character_.powerPoints <= 0 || !rules_.canLearnPowerAtLevelUp(character_, powerId)) {
        return false;
    }
    character_.powers.insert(powerId);
    --character_.powerPoints;
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

bool GameSession::spendSkillRank(std::int32_t index)
{
    const std::string name = rules_.skillName(index);
    if (name.empty() || !rules_.loaded()) {
        return false;
    }
    const std::int32_t cost = rules_.skillRankCost(character_.classId, index);
    std::int32_t& rank = character_.skills[name]; // a sheet that never trained the skill has no entry yet
    if (cost <= 0 || character_.skillPoints < cost
        || rank >= rules_.maxSkillRank(character_.classId, index, character_.level)) {
        return false;
    }
    ++rank;
    character_.skillPoints -= cost;
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
    floatingText(xpFloatObject_ != 0xFFFFFFFFu ? xpFloatObject_ : scriptWorld_.pc, 3, amount);
    if (localization_) {
        // 42438 "Experience Points (XP) Received: <CUSTOM0>" (the text; where retail raises it is not read).
        std::string text = localization_->tlk().text(42438);
        const std::size_t at = text.find("<CUSTOM0>");
        if (at != std::string::npos) {
            text.replace(at, 9, std::to_string(amount));
        }
        feedback(text);
    }
    const std::int32_t level = rules_.loaded() ? rules_.levelForXp(character_.xp)
                                               : character_.level;
    if (level > character_.level) {
        for (std::int32_t next = character_.level + 1; next <= level; ++next) {
            if (rules_.loaded()) {
                character_.skillPoints += rules_.skillPointsForLevel(character_.classId,
                                                                     character_.intel);
                character_.featPoints += rules_.featsForLevel(character_.classId, next);
                character_.powerPoints += rules_.powersForLevel(character_.classId, next);
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
        if (localization_) {
            feedback(localization_->tlk().text(1460)); // "Level Up!"
        }
        floatingText(scriptWorld_.pc, 4, level);
        ScriptApi{*this}.raiseHook(sim_->moduleObject, "Mod_OnPlrLvlUp",
                                   {{"pcLevellingUp", scriptWorld_.pc}});
    }
    awardPartyXp(amount);
    logEvent("xp +" + std::to_string(amount) + " = " + std::to_string(character_.xp));
}

bool GameSession::useFeat(std::int32_t featId, std::uint32_t target)
{
    if (!character_.feats.contains(featId) || !rules_.loaded()) {
        return false;
    }
    // A special attack is one combat round's attack action carrying the feat (the
    // attack data's feat field read by 005a4ec0 / 005a4fb0 / 005a5130 / 004d2a10); the
    // round after it is an ordinary attack. It is not a timed mode.
    static constexpr std::int32_t kSpecials[] = {8, 11, 17, 18, 19, 20, 26, 28, 29, 30, 31,
                                                 53, 77, 81, 82, 83, 91, 92};
    if (std::find(std::begin(kSpecials), std::end(kSpecials), featId) == std::end(kSpecials)) {
        return false;
    }
    const std::string label = rules_.featName(featId);
    combatMode_ = featId == 53 ? CombatMode::Whirlwind
        : (label.find("POWER_") != std::string::npos) ? CombatMode::PowerAttack
        : (label.find("CRITICAL") != std::string::npos || label.find("SNIPER") != std::string::npos)
        ? CombatMode::CriticalStrike
        : CombatMode::Flurry;
    combatModeRank_ = label.find("MASTER") != std::string::npos ? 3
        : label.find("IMPROVED") != std::string::npos ? 2 : 1;
    leaveStealth();
    const std::uint32_t attacker = controlledObject();
    std::uint32_t victim = target;
    if (victim == 0 || victim == attacker || victim == script::kObjectInvalid) {
        victim = combatTarget_ != UINT32_MAX ? objectForEntity(combatTarget_) : script::kObjectInvalid;
    }
    ScriptApi api{*this};
    if (victim != script::kObjectInvalid && victim != 0 && api.isAlive(victim)
        && api.entityFor(victim) != static_cast<std::size_t>(-1)) {
        std::erase_if(api.actor(attacker).queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
        ActorAction attack;
        attack.kind = ActorActionKind::Attack;
        attack.object = victim;
        attack.spell = featId;
        attack.userIssued = true;
        api.enqueue(attacker, std::move(attack));
    }
    logEvent("feat " + label);
    return true;
}

bool GameSession::useForcePower(std::int32_t powerId, std::uint32_t target)
{
    if (!rules_.loaded()) {
        return false;
    }
    // The radial menu issues a cast action on the controlled creature, which then runs
    // retail 00514af0 AIActionCastSpell (ScriptCombat.cpp runCastAction): conjure, impact
    // script from spells.2da (k_sp1_generic and friends), cast animation. Nothing about
    // the power's effect is decided here, and casting shifts no alignment (retail's only
    // ModifyAlignment 005ae610 callers are ACT_201, ACT_750 and the cheat console).
    // The menu refuses a power whose adjusted cost (0059b8a0, checked at 005a5597)
    // exceeds the Force points.
    const std::uint32_t caster = controlledObject();
    ScriptApi api{*this};
    std::int32_t points = 0;
    std::int32_t goodEvil = 50;
    if (caster == scriptWorld_.pc) {
        if (!character_.powers.contains(powerId)) {
            return false;
        }
        points = character_.fp;
        goodEvil = character_.alignment;
    } else {
        const std::size_t index = api.entityFor(caster);
        if (index == static_cast<std::size_t>(-1)) {
            return false;
        }
        const kotor::CreatureStats& stats = scene_.entities[index].stats;
        if (std::find(stats.powers.begin(), stats.powers.end(), powerId) == stats.powers.end()) {
            return false;
        }
        const std::int32_t held = api.actor(caster).combat.forcePoints;
        points = held >= 0 ? held : stats.forcePoints;
        goodEvil = stats.goodEvil;
    }
    const std::int32_t cost = rules_.adjustedForceCost(powerId, goodEvil, true);
    if (points < cost) {
        lastForcePower_ = powerId;
        lastForcePowerSuccess_ = false;
        return false;
    }
    ActorAction cast;
    cast.kind = ActorActionKind::CastSpell;
    cast.spell = powerId;
    cast.object = target == 0 ? caster : target;
    cast.userIssued = true;
    api.enqueue(caster, std::move(cast));
    leaveStealth();
    lastForcePower_ = powerId;
    lastForcePowerSuccess_ = true;
    logEvent("force cast queued " + rules_.powerName(powerId));
    return true;
}

void GameSession::traceRound(std::int32_t number, std::uint32_t owner, bool ended)
{
    if (stateTrace_ != nullptr) {
        stateTrace_->onRound(*this, number, owner, ended);
    }
}

std::optional<std::vector<script::Value>> GameSession::action(
    std::uint16_t actionId, const std::vector<std::vector<script::Value>>& args,
    script::World& world, script::Vm& vm)
{
    if (stateTrace_ != nullptr) {
        stateTrace_->onAction(actionId);
    }
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
    case 209: { // AdjustReputation
        // retail 0052e1a0 -> CSWSCreature::AdjustReputation 00514070: only a creature target; the
        // source's faction must be an NPC faction (GetIsNPCFaction 0052b280: 1..count) and differ from
        // the target's. The new value is read the other way round: Get 0052b420 is called with
        // (target, source) but indexes [source][target], Set 0052b3d0 writes [target][source], clamped
        // 0..100. A dead source (object +0xf8 set, 0052e20d) changes nothing.
        const std::uint32_t target = objectArg(0);
        const std::uint32_t source = objectArg(1);
        const ScriptApi api{*this};
        const bool sourceDead = api.world().valid(source) && !api.isAlive(source);
        const int targetFaction = factionOfObject(target);
        const int sourceFaction = factionOfObject(source);
        const int npcFactions = static_cast<int>(factions_.count()) - 1;
        if (!sourceDead && api.isCreature(target) && sourceFaction > 0 && sourceFaction <= npcFactions
            && targetFaction != sourceFaction) {
            setReputation(targetFaction, sourceFaction,
                                    factions_.reputation(sourceFaction, targetFaction)
                                        + static_cast<int>(intArg(2)));
        }
        return std::vector<script::Value>{};
    }
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
            // GetObjectByTag must stop finding it: k_pend_door01 compares the party
            // leader with the Trask the party screen spawned in place of this one.
            std::erase(world.tags[object->tag], object->id);
            for (std::size_t i = 0; i < entityObjects_.size(); ++i) {
                if (entityObjects_[i] == object->id && i < scene_.entities.size()) {
                    scene_.entities[i].dead = true;
                    scene_.entities[i].hitPoints = std::max(scene_.entities[i].hitPoints, 1);
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
    case 217: // GetIsPC: the player list holds the steered creature (retail 004b2a50)
        return intCells(objectArg(0) == controlledObject() ? 1 : 0);
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
        return objectCells(controlledObject());
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
            // ACT_46 00541170 queues the object action 0x17; the retail trace (Taris cutscene, n_genbith_comm1) shows
            // the sound as a 2D streaming source in priority group 9 (Non_Player_Chat, volume 127), like a spoken line.
            audio_->playVoice(stringArg(0));
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
    case 769: // QueueMovie(movie, bSkippable): queues only
        queueMovie(stringArg(0), intArg(1) != 0);
        return std::vector<script::Value>{};
    case 770: // PlayMovieQueue(bAllowSeparateSkips)
        playMovieQueue(intArg(0) != 0);
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
        const std::string name = rules_.scriptAnimationName(animationId);
        if (!name.empty()) {
            const std::uint32_t self = runtime_ != nullptr
                ? runtime_->contextObject("self") : world.pc;
            for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
                if (objectForEntity(i) == self) {
                    bool placeableLoop = false;
                    const char* placeable = scene_.entities[i].kind == kotor::EntityKind::Placeable
                        ? kotor::placeableScriptClip(animationId, placeableLoop) : nullptr;
                    if (placeable != nullptr) {
                        playAnimation(i, placeable, placeableLoop);
                    } else {
                        playAnimation(i, name, rules_.scriptAnimationLooping(animationId));
                    }
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
        statusFeedback(4, static_cast<std::int32_t>(intArg(1))); // ACT_201
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
    file << "class2=" << character_.secondClassId << "\nclass2level=" << character_.secondClassLevel << "\n";
    file << "level=" << character_.level << "\n";
    file << "xp=" << character_.xp << "\n";
    file << "gold=" << gold_ << "\n";
    file << "alignment=" << character_.alignment << "\n";
    file << "gender=" << character_.gender << "\n";
    file << "portrait_row=" << character_.portraitRow << "\n";
    file << "appearance=" << playerAppearance_ << "\n";
    file << "force_points=" << character_.fp << "\n";
    file << "max_force_points=" << character_.maxFp << "\n";
    file << "str=" << character_.str << "\ndex=" << character_.dex
         << "\ncon=" << character_.con << "\nint=" << character_.intel
         << "\nwis=" << character_.wis << "\ncha=" << character_.cha << "\n";
    file << "feat_points=" << character_.featPoints << "\n";
    file << "skill_points=" << character_.skillPoints << "\n";
    file << "attribute_points=" << character_.attributePoints << "\n";
    file << "power_points=" << character_.powerPoints << "\n";
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
                         ? value.text.str()
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
    long savedAppearance = -1;
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
            } else if (key == "class2") {
                sheet.secondClassId = std::stoi(value);
            } else if (key == "class2level") {
                sheet.secondClassLevel = std::stoi(value);
            } else if (key == "level") {
                sheet.level = std::stoi(value);
            } else if (key == "xp") {
                sheet.xp = std::stoi(value);
            } else if (key == "gold") {
                gold = std::stoi(value);
            } else if (key == "alignment") {
                sheet.alignment = std::clamp(std::stoi(value), 0, 100);
            } else if (key == "gender") {
                sheet.gender = std::clamp(std::stoi(value), 0, 1);
            } else if (key == "portrait_row") {
                sheet.portraitRow = std::stoi(value);
            } else if (key == "appearance") {
                savedAppearance = std::stol(value);
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
            } else if (key == "power_points") {
                sheet.powerPoints = std::max(0, std::stoi(value));
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
        if (savedAppearance >= 0
            && static_cast<std::uint32_t>(savedAppearance) != playerAppearance_) {
            setPlayerAppearance(static_cast<std::uint32_t>(savedAppearance));
        }
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

bool GameSession::loadOriginalSave(const std::filesystem::path& folder, bool asPlayer)
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
    if (asPlayer) {
        stopMovie(); // a movie from the session being left (the start module's crawl) must not hold the loaded game
    }
    sim_->snapshots.clear();
    party_ = PartyTable{};
    companions_.clear();
    partyTags_.clear();
    sim_->restoringSave = true;
    savedGit_ = saved.gitBytes.empty() ? nullptr : &saved.gitBytes;
    try {
        if (!loadModule(saved.module, nullptr, {})) {
            savedGit_ = nullptr;
            sim_->restoringSave = false;
            return false;
        }
    } catch (const std::exception& exception) {
        savedGit_ = nullptr;
        sim_->restoringSave = false;
        logEvent(std::string("original save module failed: ") + exception.what());
        return false;
    }
    savedGit_ = nullptr;
    sim_->restoringSave = false;
    if (!saved.area.empty() && lower(saved.area) != lower(scene_.area)) {
        logEvent("original save area differs from module entry: " + saved.area);
    }
    character_.name = saved.pcName.empty() ? "Player" : saved.pcName;
    if (!saved.pcName.empty()) {
        playerName_ = saved.pcName; // the Messages log names the PC by it
    }
    character_.classId = saved.classId;
    character_.level = std::max(1, saved.level);
    character_.xp = saved.xp;
    character_.alignment = std::clamp(saved.alignment, 0, 100);
    character_.gender = std::clamp(saved.gender, 0, 1);
    character_.portraitRow = saved.portraitId;
    if (saved.appearance >= 0 && saved.portraitId >= 0
        && static_cast<std::uint32_t>(saved.appearance) != playerAppearance_) {
        // Only a save that also names a portrait row carries a PC appearance; the
        // fixed value older engine saves wrote (race row 6) is not a body.
        setPlayerAppearance(static_cast<std::uint32_t>(saved.appearance));
    }
    character_.fp = saved.forcePoints;
    character_.maxFp = std::max(saved.maxForcePoints, saved.forcePoints);
    // The level-up points belong to the loaded creature, not to the session that was running (the
    // walk's start module left some, which drew the portrait's level-up badge that retail lacks on
    // this save: XP 0, level 1). NOT READ: the player's saved SkillPoints and pending feat or power
    // picks (OriginalSave carries none), so a save with points waiting loses them here.
    character_.attributePoints = 0;
    character_.skillPoints = 0;
    character_.featPoints = 0;
    character_.powerPoints = 0;
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
        item.upgrades = source.upgrades;
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
    // Party members come from AVAILNPC<n> and are put into the area by CSWPartyTable::SpawnNPC 00565130 (no
    // GIT spawn flag is applied), so the first update of each runs its OnSpawn (004eb6e0, flag +0x34c still 0).
    // Measured on retail: 2 spawn runs at load in a Taris save with Carth, 3 in a Kashyyyk save with two members.
    // The PC's own spawn run is not reproduced (its script is not known here).
    for (const CompanionState& companion : companions_) {
        const std::size_t index = ensureCompanionEntity(companion);
        if (index >= scene_.entities.size()) {
            continue;
        }
        const auto spawn = scene_.entities[index].scripts.find("ScriptSpawn");
        if (spawn != scene_.entities[index].scripts.end() && !spawn->second.empty()
            && spawnRan_.insert(objectForEntity(index)).second) {
            runScript(spawn->second, objectForEntity(index), scriptWorld_.pc, scriptWorld_.pc, scriptWorld_.pc);
        }
    }

    std::vector<std::pair<std::size_t, const decltype(kotor::SavedObjectState::loot)*>> savedLoot;
    std::vector<bool> objectUsed(scene_.entities.size(), false);
    std::vector<std::pair<std::size_t, const kotor::SavedObjectState*>> matchedObjects;
    for (const kotor::SavedObjectState& state : saved.objects) {
        const kotor::EntityKind kind = entityKindForSaved(state.kind);
        std::size_t index = static_cast<std::size_t>(-1);
        float bestDistance = 1.0e30f;
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& candidate = scene_.entities[i];
            if (objectUsed[i] || lower(candidate.tag) != lower(state.tag)
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
        matchedObjects.emplace_back(index, &state);
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
            savedLoot.emplace_back(index, &state.loot);
        }
        applyObjectStateClip(index);
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
    if (asPlayer) {
        thirdPerson_ = true;
        playerBearing_ = yaw_;
        spawnPlayerAvatar();
    }
    activeDialogue_.reset();
    dialogueView_ = {};
    combatTarget_ = UINT32_MAX;
    selectedEntity_ = UINT32_MAX;
    syncScriptWorld();
    // The area's script locals (the SWVarTable at the GIT root) go back before the entry scripts run: retail's area
    // OnEnter fires on a loaded game too (CSWSArea::AddObjectToArea 0050dfd0 queues event 0xc carrying
    // GetLoadFromSaveGame, handled at 0050d6c0) and the Endar area script k_pend_area01 guards its opening cutscene on a
    // local of the area that the save holds; the retail trace of that save shows the script reach its guard and stop.
    if (saved.areaVars.present && sim_->areaObject != script::kObjectInvalid) {
        for (std::int64_t i = 0; i < 96; ++i) {
            if (((saved.areaVars.bits[static_cast<std::size_t>(i / 32)] >> static_cast<unsigned>(i % 32)) & 1u) != 0u) {
                scriptWorld_.setLocalBoolean(sim_->areaObject, i, true);
            }
        }
        for (std::int64_t i = 0; i < 8; ++i) {
            if (saved.areaVars.bytes[static_cast<std::size_t>(i)] != 0) {
                scriptWorld_.setLocalNumber(sim_->areaObject, i, static_cast<std::int8_t>(saved.areaVars.bytes[static_cast<std::size_t>(i)]));
            }
        }
    }
    // Retail 004d1cf0 (LoadObjectState) gives each object back its script variables: the named ones
    // (0059aa80) and the 96 booleans and 8 numbers of CSWVarTable (0059b0f0).
    for (const auto& [index, state] : matchedObjects) {
        if (index >= entityObjects_.size() || entityObjects_[index] == 0) {
            continue;
        }
        const std::uint32_t object = entityObjects_[index];
        for (const kotor::SavedObjectState::Local& local : state->locals) {
            if (local.type == 1) {
                scriptWorld_.setLocal(object, local.name, script::Value::makeInt(local.integer));
            } else if (local.type == 2) {
                scriptWorld_.setLocal(object, local.name, script::Value::makeFloat(local.number));
            } else if (local.type == 3) {
                scriptWorld_.setLocal(object, local.name, script::Value::makeString(local.text));
            }
        }
        for (std::int64_t bit = 0; bit < 96; ++bit) {
            if ((state->localBits[static_cast<std::size_t>(bit) / 32] >> (bit % 32) & 1u) != 0) {
                scriptWorld_.setLocalBoolean(object, bit, true);
            }
        }
        for (std::int64_t number = 0; number < 8; ++number) {
            const std::uint8_t byte = state->localBytes[static_cast<std::size_t>(number)];
            if (byte != 0) {
                scriptWorld_.setLocalNumber(object, number, static_cast<std::int8_t>(byte));
            }
        }
    }
    // Retail 004d1be0 (LoadEffectList) applies the saved effects again, except SkipOnLoad ones and innate (duration
    // type 3) ones; the argument layout of an effect beyond its type is kept as the saved integers and floats.
    {
        ScriptApi api{*this};
        for (const auto& [index, state] : matchedObjects) {
            if (index >= entityObjects_.size() || entityObjects_[index] == 0) {
                continue;
            }
            for (const kotor::SavedEffect& held : state->effects) {
                const std::int32_t scriptType = kotor::savedEffectScriptType(held.type);
                const std::int32_t durationType = held.subType & 7;
                if (!kotor::savedEffectApplies(held) || scriptType < 0 || (durationType != 1 && durationType != 2)) {
                    continue;
                }
                EffectData data;
                data.type = scriptType;
                data.subType = held.subType;
                for (const std::int32_t value : held.ints) {
                    data.args.push_back(script::Value::makeInt(value));
                }
                for (const float value : held.floats) {
                    data.args.push_back(script::Value::makeFloat(value));
                }
                const std::int64_t id = api.sim().engine.add(std::move(data));
                applyEffectRecord(api, durationType, id, entityObjects_[index], held.duration, script::kObjectInvalid,
                                  static_cast<std::int32_t>(held.spellId));
            }
        }
    }
    // The delayed scripts still pending at the save (IFO EventQueue, retail's event queue) come back due at the
    // same distance from the saved world clock: Endar's k_pend_time01 starts the Trask conversation 19.4 s after
    // the save's clock, and the engine would otherwise never start it. Object ids in the queue are the save's;
    // they map onto the objects this load matched.
    logInfo(concat("original save: ", saved.events.size(), " pending scripts in the event queue"));
    if (!saved.events.empty()) {
        std::map<std::uint32_t, std::uint32_t> idMap;
        if (saved.areaObjectId != 0 && sim_->areaObject != script::kObjectInvalid) {
            idMap[saved.areaObjectId] = sim_->areaObject;
        }
        if (saved.playerObjectId != 0) {
            idMap[saved.playerObjectId] = scriptWorld_.pc;
        }
        for (const auto& [index, state] : matchedObjects) {
            if (const std::uint32_t object = objectForEntity(index);
                state->objectId != 0 && object != script::kObjectInvalid && object != 0) {
                idMap[state->objectId] = object;
            }
        }
        const auto mapObject = [&](std::uint32_t id) {
            if (id == 0 || id == script::kObjectInvalid) {
                return id;
            }
            const auto it = idMap.find(id);
            return it != idMap.end() ? it->second : script::kObjectInvalid;
        };
        for (const kotor::SavedScriptEvent& event : saved.events) {
            const std::uint32_t owner = mapObject(event.objectId);
            const script::ProgramPtr program = decodedScript(event.script);
            if (owner == script::kObjectInvalid || owner == 0 || program == nullptr) {
                logInfo(concat("original save: dropped pending ", event.script, " (object ", event.objectId, " mapped to ", owner,
                               program == nullptr ? ", no script)" : ")"));
                continue;
            }
            // The saved Code blob starts at the first instruction, without the 13-byte NCS header, so its
            // InstructionPtr is that much short of the file offset the decoder keeps.
            constexpr std::uint32_t kNcsHeaderBytes = 13;
            const auto entry = program->byOffset.find(static_cast<std::uint32_t>(event.instruction) + kNcsHeaderBytes);
            if (entry == program->byOffset.end()) {
                logInfo(concat("original save: dropped pending ", event.script, " (no instruction at offset ", event.instruction, ")"));
                continue;
            }
            script::Functor functor;
            functor.program = program;
            functor.ip = entry->second;
            functor.bp = event.basePointer;
            functor.script = event.script;
            functor.source = "save";
            functor.context["self"] = owner;
            for (const kotor::SavedStackCell& cell : event.stack) {
                switch (cell.type) {
                case 3: functor.stack.push_back(script::Value::makeInt(cell.integer)); break;
                case 4: functor.stack.push_back(script::Value::makeFloat(cell.real)); break;
                case 5: functor.stack.push_back(script::Value::makeString(cell.text)); break;
                case 6: functor.stack.push_back(script::Value::makeObject(mapObject(static_cast<std::uint32_t>(cell.integer)))); break;
                default: functor.stack.push_back(script::Value::makeEngine(0)); break;
                }
            }
            const double remainingMs = (static_cast<double>(event.time) - static_cast<double>(saved.pauseTime))
                + (static_cast<double>(event.day) - static_cast<double>(saved.pauseDay)) * 86400000.0;
            script::Timer timer;
            timer.due = scriptWorld_.time + std::max(0.0, remainingMs / 1000.0);
            logInfo(concat("original save: pending ", event.script, " restored, due in ", remainingMs / 1000.0, " s on object ", owner));
            timer.functor = scriptWorld_.addFunctor(std::move(functor));
            timer.target = owner;
            timer.source = "save";
            scriptWorld_.timers.push_back(std::move(timer));
        }
    }
    // Loading a game is not walking in. Measured on retail: the Taris save loads with the player inside
    // tar02_carstr and no conversation starts (its k_ptar_carstr_en would start one).
    triggerInside_.assign(scene_.triggers.size(), false);
    for (std::size_t i = 0; i < scene_.triggers.size(); ++i) {
        triggerInside_[i] = pointInTrigger(scene_.triggers[i], position_.x, position_.y);
    }
    // The module's start scripts see the save, not an empty party. Measured on retail, Taris save:
    // k_pebn_pophawk finds Carth available from the PARTYTABLE and SpawnAvailableNPC (698) runs.
    if (saveModuleStartPending_) {
        saveModuleStartPending_ = false;
        sim_->restoringSave = true; // GetLoadFromSaveGame (251)
        runSceneHook("Mod_OnModLoad", scriptWorld_.pc);
        runSceneHook("Mod_OnModStart", scriptWorld_.pc);
        runSceneHook("Mod_OnClientEntr", scriptWorld_.pc);
        runSceneHook("OnEnter", scriptWorld_.pc);
        sim_->restoringSave = false;
        // Engine choice: a container keeps what the save holds. k_pend_area01 fills end_locker01 again
        // because object locals aren't restored here.
        for (const auto& [index, loot] : savedLoot) {
            if (index < scene_.entities.size()) {
                scene_.entities[index].lootItems = *loot;
            }
        }
        spawnEncounters();
        startMiniGame(ScriptApi{*this});
    }
    // Retail 0055ccd0 reads each item's NewItem bit back.
    {
        std::vector<std::size_t> fresh;
        for (std::size_t i = 0; i < saved.inventory.size() && i < character_.inventory.size(); ++i) {
            if (saved.inventory[i].newItem) {
                fresh.push_back(i);
            }
        }
        newItems_.clear();
        if (!fresh.empty()) {
            syncInventoryObjects();
            for (const std::size_t i : fresh) {
                if (i < character_.inventory.size() && character_.inventory[i].object != 0) {
                    newItems_.insert(character_.inventory[i].object);
                }
            }
        }
    }
    // Retail 00505da0 / 00578fc0: the saved map bits are taken only when the live map has the same size.
    mapFogs_.clear(); // a loaded game starts from the save's map, not this session's
    if (saved.areaMap.present) {
        if (mapFog() != nullptr) {
            MapFog& fog = mapFogs_[scene_.area];
            const std::size_t cells = static_cast<std::size_t>(fog.cols + 1) * static_cast<std::size_t>(fog.rows + 1);
            if (saved.areaMap.resX == fog.cols && saved.areaMap.resY == fog.rows
                && saved.areaMap.words.size() == cells / 32u + 1u) {
                for (std::size_t i = 0; i < cells; ++i) {
                    fog.seen[i] = (saved.areaMap.words[i >> 5] >> (i & 31u) & 1u) != 0 ? char{1} : char{0};
                }
                fog.words = saved.areaMap.words;
            }
        }
    }
    followHold_ = true;
    followHoldAnchor_ = controlledPosition();
    logEvent("original save loaded: " + saved.module + " gold=" + std::to_string(gold_)
             + " inventory=" + std::to_string(character_.inventory.size())
             + " journal=" + std::to_string(journal_.size())
             + " globals=" + std::to_string(scriptWorld_.globals.size()));
    return true;
}

std::string GameSession::areaDisplayName()
{
    // savenfo AREANAME is "Planet - Area": modulesave.2da's areaname strref for the module (the
    // text a retail save shows as the planet and area lines); a module without a row keeps the
    // name the save was loaded with, then the area tag.
    std::string name = originalAreaName_.empty()
        ? (scene_.areaTag.empty() ? scene_.area : scene_.areaTag)
        : originalAreaName_;
    if (auto data = install_.find("modulesave", kotor::resType(kotor::ResType::TwoDa)); data.has_value()) {
        try {
            const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "modulesave");
            const int row = table.rowIndex(module_);
            const int column = table.columnIndex("areaname");
            if (row >= 0 && column >= 0 && localization_) {
                const std::string cell = table.cell(static_cast<std::size_t>(row), static_cast<std::size_t>(column));
                if (!cell.empty() && cell != "****") {
                    const std::string text = localization_->tlk().text(
                        static_cast<std::uint32_t>(std::strtoul(cell.c_str(), nullptr, 10)));
                    if (!text.empty()) {
                        name = text;
                    }
                }
            }
        } catch (const kotor::FormatError&) {
            // keep the fallback name
        }
    }
    return name;
}

bool GameSession::saveOriginalSave(const std::filesystem::path& folder,
                                   const std::string& saveName)
{
    kotor::OriginalSave save;
    save.folder = folder;
    save.module = module_;
    save.area = scene_.area;
    save.areaName = areaDisplayName();
    save.screenTga = saveScreenTga_;
    {
        // The other party members' portraits for the save screen's second and third boxes.
        std::string* targets[2] = {&save.partyPortrait1, &save.partyPortrait2};
        int filled = 0;
        for (const int npc : party_.members) {
            if (filled >= 2) {
                break;
            }
            const CharacterSheet* sheet = npc == kNpcPlayer ? nullptr : liveSheet(npc);
            if (sheet == nullptr || sheet->portraitRow < 0 || !rules_.loaded()) {
                continue;
            }
            const std::string resref = rules_.portraitResrefFor(rules_.portraitRow(sheet->portraitRow), sheet->alignment);
            if (!resref.empty()) {
                *targets[filled++] = resref;
            }
        }
    }
    save.saveName = saveName;
    save.pcName = character_.name;
    save.portrait = originalPortrait_.empty() ? "po_pmhc4" : originalPortrait_;
    if (character_.portraitRow >= 0 && rules_.loaded()) {
        const PortraitInfo info = rules_.portraitRow(character_.portraitRow);
        if (!info.resref.empty()) {
            save.portrait = info.resref;
        }
    }
    save.position = position_;
    save.yaw = yaw_;
    save.gold = gold_;
    save.xp = character_.xp;
    save.hp = playerHp_;
    save.maxHp = std::max(playerHp_, playerMaxHp_);
    save.level = character_.level;
    save.classId = character_.classId;
    save.alignment = character_.alignment;
    save.gender = character_.gender;
    save.portraitId = character_.portraitRow;
    save.appearance = static_cast<std::int32_t>(playerAppearance_);
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
        copy.upgrades = item.upgrades;
        copy.newItem = item.object != 0 && isNewItem(item.object);
        if (const auto cached = itemTemplates_.find(item.resref); cached != itemTemplates_.end()) {
            copy.properties = cached->second.properties;
        } else if (auto templ = kotor::loadItemTemplate(install_, item.resref); templ.has_value()) {
            copy.properties = templ->properties;
        }
        save.inventory.push_back(std::move(copy));
    }
    // Retail 005061d0: the area's explored map goes into the saved GIT.
    if (const MapFog* fog = mapFog(); fog != nullptr) {
        save.areaMap.present = true;
        save.areaMap.resX = fog->cols;
        save.areaMap.resY = fog->rows;
        const std::size_t cells = static_cast<std::size_t>(fog->cols + 1) * static_cast<std::size_t>(fog->rows + 1);
        save.areaMap.words.assign(cells / 32u + 1u, 0u); // 00578c60's word count
        if (fog->words.size() == save.areaMap.words.size()) {
            save.areaMap.words = fog->words;
        }
        for (std::size_t i = 0; i < cells && i < fog->seen.size(); ++i) {
            if (fog->seen[i] != 0) {
                save.areaMap.words[i >> 5] |= 1u << (i & 31u);
            }
        }
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
    // Script locals (retail CSWVarTable: 96 booleans, 8 number bytes) ride in each object's SWVarTable.
    const auto varsOf = [&](std::uint32_t id) {
        kotor::SavedVarTable vars;
        vars.present = true;
        for (std::int64_t i = 0; i < 96; ++i) {
            if (scriptWorld_.localBoolean(id, i) != 0) {
                vars.bits[static_cast<std::size_t>(i / 32)] |= std::uint32_t{1} << static_cast<unsigned>(i % 32);
            }
        }
        for (std::int64_t i = 0; i < 8; ++i) {
            vars.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(scriptWorld_.localNumber(id, i));
        }
        return vars;
    };
    save.areaVars = varsOf(sim_->areaObject);
    for (std::size_t entityIndex = 0; entityIndex < scene_.entities.size(); ++entityIndex) {
        const kotor::Entity& entity = scene_.entities[entityIndex];
        if (entity.tag.empty() || entity.partyAvatar) {
            continue;
        }
        kotor::SavedObjectState state;
        {
            const kotor::SavedVarTable vars = varsOf(objectForEntity(entityIndex));
            state.localTable = true;
            state.localBits = vars.bits;
            state.localBytes = vars.bytes;
        }
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
