#include "game/GameSession.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>

namespace ke {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEyeHeight = 1.55f;
constexpr float kPlayerRadius = 0.28f;
constexpr float kStepHeight = 0.55f;
constexpr float kGravity = 18.0f;
constexpr std::int32_t kPlayerDefense = 15;
constexpr std::int32_t kPlayerAttackBonus = 5;
constexpr std::int32_t kPlayerDamageMin = 4;
constexpr std::int32_t kPlayerDamageMax = 14;

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

// Barycentric floor height at (x, y); the highest triangle below maxZ wins.
bool floorHeight(const kotor::Scene& scene, float x, float y, float maxZ, float& out)
{
    bool found = false;
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
            found = true;
        }
    }
    if (found) {
        out = best;
    }
    return found;
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
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (facet.floor) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
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

} // namespace

GameSession::GameSession(std::string installPath, std::string module)
    : installPath_(std::move(installPath))
    , install_(installPath_)
{
    const std::filesystem::path tlkPath = std::filesystem::path(installPath_) / "dialog.tlk";
    std::ifstream tlkFile(tlkPath, std::ios::binary | std::ios::ate);
    if (tlkFile) {
        const std::streamoff size = tlkFile.tellg();
        tlkFile.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (bytes.empty() || tlkFile.read(reinterpret_cast<char*>(bytes.data()), size)) {
            try {
                tlk_ = kotor::Tlk::parse(std::move(bytes), "dialog.tlk");
            } catch (const kotor::FormatError&) {
                tlk_.reset();
            }
        }
    }
    if (!loadModule(module, nullptr, {})) {
        throw kotor::FormatError("cannot load module " + module);
    }
}

bool GameSession::loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor)
{
    kotor::Scene loaded = kotor::loadModule(install_, module);
    module_ = module;
    scene_ = std::move(loaded);
    if (spawn != nullptr) {
        position_ = *spawn;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    } else {
        position_ = scene_.entry;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    }
    if (!linkedDoor.empty()) {
        for (const kotor::Entity& entity : scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door && entity.tag == linkedDoor) {
                position_ = entity.position;
                position_.x += std::cos(entity.bearing) * 0.9f;
                position_.y += std::sin(entity.bearing) * 0.9f;
                yaw_ = entity.bearing + kPi;
                break;
            }
        }
    }
    float ground = 0.0f;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    activeDialogue_.reset();
    dialogueView_ = {};
    combatTarget_ = UINT32_MAX;
    playerAttackTimer_ = 0.0f;
    npcAttackTimers_.assign(scene_.entities.size(), 0.0f);
    triggerInside_.assign(scene_.triggers.size(), false);
    if (playerHp_ <= 0) {
        playerHp_ = playerMaxHp_;
        playerDead_ = false;
    }
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

void GameSession::logEvent(std::string line)
{
    combatLog_.push_back(concat("[t=", elapsed_, "] ", line));
    if (combatLog_.size() > 32) {
        combatLog_.erase(combatLog_.begin());
    }
}

void GameSession::update(float dt, const InputState& input)
{
    if (dt <= 0.0f) {
        return;
    }
    dt = std::min(dt, 0.08f);
    elapsed_ += dt;
    yaw_ += input.mouseDx * 0.0026f;
    pitch_ = clamp(pitch_ - input.mouseDy * 0.0022f, -1.35f, 1.35f);

    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    if (!playerDead_ && !dialogueView_.active) {
        if (input.forward) forwardAxis += 1.0f;
        if (input.back) forwardAxis -= 1.0f;
        if (input.right) rightAxis += 1.0f;
        if (input.left) rightAxis -= 1.0f;
    }
    const float speed = input.run ? 7.5f : 3.8f;
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
    kotor::Vec3 candidate{position_.x + delta.x, position_.y + delta.y, position_.z};
    if (wallBlocked(scene_, start, candidate, kPlayerRadius)) {
        kotor::Vec3 xOnly{start.x + delta.x, start.y, start.z};
        kotor::Vec3 yOnly{start.x, start.y + delta.y, start.z};
        if (!wallBlocked(scene_, start, xOnly, kPlayerRadius)) {
            candidate = xOnly;
        } else if (!wallBlocked(scene_, start, yOnly, kPlayerRadius)) {
            candidate = yOnly;
        } else {
            candidate = start;
        }
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

    updateTriggers();
    updateCombat(dt);
}

SceneCamera GameSession::camera() const
{
    SceneCamera camera;
    camera.eye = {position_.x, position_.y, position_.z + kEyeHeight};
    camera.forward = {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_),
                      std::sin(pitch_)};
    camera.up = {0.0f, 0.0f, 1.0f};
    return camera;
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
    if (const kotor::Entity* talk = nearestConversation(3.0f); talk != nullptr) {
        return startDialogue(static_cast<std::size_t>(talk - scene_.entities.data()));
    }
    const kotor::Entity* door = nearestDoor(2.8f);
    if (door != nullptr && !door->linkedModule.empty()) {
        return loadModule(door->linkedModule, nullptr, door->linkedTo);
    }
    if (door != nullptr) {
        logEvent("door " + door->tag + ": no transition");
        return true;
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
    if (entityIndex >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[entityIndex];
    if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
        return false;
    }
    if (distance2D(position_, entity.position) > 2.5f) {
        return false;
    }
    logEvent("use " + entity.tag);
    return true;
}

bool GameSession::testTransition(std::string& from, std::string& to)
{
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Door || entity.linkedModule.empty()) {
            continue;
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
    const int steps = std::max(1, static_cast<int>(maxSeconds * 60.0f));
    for (int i = 0; i < steps; ++i) {
        if (distance2D(position_, target) < 0.6f) {
            return true;
        }
        yaw_ = std::atan2(target.y - position_.y, target.x - position_.x);
        InputState input;
        input.forward = true;
        input.run = distance2D(position_, target) > 4.0f;
        update(1.0f / 60.0f, input);
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
    kotor::Entity& entity = scene_.entities[entityIndex];
    if (entity.conversation.empty()) {
        logEvent((entity.name.empty() ? entity.tag : entity.name) + " has nothing to say");
        return false;
    }
    kotor::Dlg dlg;
    if (!loadDialogue(entity.conversation, dlg)) {
        logEvent("missing dialogue " + entity.conversation);
        return false;
    }
    ActiveDialogue active;
    active.npcTag = entity.tag;
    active.speaker = entity.name.empty() ? entity.tag : entity.name;
    active.dlg = std::move(dlg);
    activeDialogue_ = std::move(active);
    std::uint32_t entry = UINT32_MAX;
    // Skip empty conditional stubs when a real line is available; the full
    // condition VM is P5 work, but the slice should open on spoken text.
    for (const std::uint32_t candidate : activeDialogue_->dlg.starting) {
        if (candidate < activeDialogue_->dlg.entries.size()
            && !activeDialogue_->dlg.entries[candidate].text.empty()) {
            entry = candidate;
            break;
        }
    }
    if (entry == UINT32_MAX) {
        for (const std::uint32_t candidate : activeDialogue_->dlg.starting) {
            if (candidate < activeDialogue_->dlg.entries.size()) {
                entry = candidate;
                break;
            }
        }
    }
    if (entry == UINT32_MAX && !activeDialogue_->dlg.entries.empty()) {
        entry = 0;
    }
    if (entry == UINT32_MAX) {
        endDialogue();
        return false;
    }
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

void GameSession::showDialogueEntry(std::uint32_t entryIndex)
{
    if (!activeDialogue_.has_value() || entryIndex >= activeDialogue_->dlg.entries.size()) {
        endDialogue();
        return;
    }
    const kotor::DlgEntry& entry = activeDialogue_->dlg.entries[entryIndex];
    dialogueView_.active = true;
    dialogueView_.npcTag = activeDialogue_->npcTag;
    dialogueView_.speaker = activeDialogue_->speaker;
    dialogueView_.text = entry.text.empty() ? "(No text)" : entry.text;
    dialogueView_.entry = entryIndex;
    dialogueView_.options.clear();
    for (const std::uint32_t replyIndex : entry.replies) {
        if (replyIndex >= activeDialogue_->dlg.replies.size()) {
            continue;
        }
        const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
        DialogueOption option;
        option.text = reply.text.empty() ? "(Continue)" : reply.text;
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
    const std::uint32_t replyIndex = dialogueView_.options[optionIndex].reply;
    if (replyIndex == UINT32_MAX || replyIndex >= activeDialogue_->dlg.replies.size()) {
        endDialogue();
        return;
    }
    const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
    if (!reply.script.empty()) {
        logEvent("dialog script " + reply.script);
    }
    if (!reply.quest.empty()) {
        logEvent("dialog quest " + reply.quest);
    }
    const bool startsCombat = lower(reply.quest) == "end_attack"
        || lower(reply.script).find("attack") != std::string::npos;
    if (startsCombat) {
        logEvent("dialogue triggers combat");
    }
    if (!reply.entries.empty() && reply.entries.front() < activeDialogue_->dlg.entries.size()) {
        showDialogueEntry(reply.entries.front());
    } else {
        endDialogue();
    }
    if (startsCombat) {
        attackNearestHostile();
    }
}

void GameSession::endDialogue()
{
    activeDialogue_.reset();
    dialogueView_ = {};
}

void GameSession::attackNearestHostile()
{
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 20.0f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (!entity.hostile || entity.dead) {
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
    logEvent("combat target " + combatTargetName());
}

std::string GameSession::combatTargetName() const
{
    if (combatTarget_ >= scene_.entities.size()) {
        return {};
    }
    const kotor::Entity& entity = scene_.entities[combatTarget_];
    return entity.name.empty() ? entity.tag : entity.name;
}

void GameSession::playerAttack(std::size_t target)
{
    kotor::Entity& entity = scene_.entities[target];
    const std::uint32_t roll = 1u + random() % 20u;
    const std::int32_t total = static_cast<std::int32_t>(roll) + kPlayerAttackBonus;
    if (total < entity.defense) {
        logEvent("player misses " + (entity.name.empty() ? entity.tag : entity.name)
                 + " (" + std::to_string(total) + " vs " + std::to_string(entity.defense) + ")");
        return;
    }
    const std::int32_t damage = kPlayerDamageMin
        + static_cast<std::int32_t>(random() % static_cast<std::uint32_t>(kPlayerDamageMax - kPlayerDamageMin + 1));
    entity.hitPoints -= damage;
    logEvent("player hits " + (entity.name.empty() ? entity.tag : entity.name)
             + " for " + std::to_string(damage) + " ("
             + std::to_string(std::max(0, entity.hitPoints)) + "/" + std::to_string(entity.maxHitPoints) + ")");
    if (entity.hitPoints <= 0) {
        entity.dead = true;
        logEvent("player defeats " + (entity.name.empty() ? entity.tag : entity.name));
        combatTarget_ = UINT32_MAX;
    }
}

void GameSession::npcAttack(std::size_t attacker)
{
    kotor::Entity& entity = scene_.entities[attacker];
    const std::uint32_t roll = 1u + random() % 20u;
    const std::int32_t total = static_cast<std::int32_t>(roll) + entity.attackBonus;
    if (total < kPlayerDefense) {
        logEvent((entity.name.empty() ? entity.tag : entity.name) + " misses player");
        return;
    }
    const std::int32_t span = std::max(1, entity.damageMax - entity.damageMin + 1);
    const std::int32_t damage = entity.damageMin + static_cast<std::int32_t>(random() % static_cast<std::uint32_t>(span));
    playerHp_ -= damage;
    logEvent((entity.name.empty() ? entity.tag : entity.name) + " hits player for "
             + std::to_string(damage) + " (" + std::to_string(std::max(0, playerHp_)) + "/"
             + std::to_string(playerMaxHp_) + ")");
    if (playerHp_ <= 0) {
        playerHp_ = 0;
        playerDead_ = true;
        logEvent("player defeated");
    }
}

void GameSession::updateCombat(float dt)
{
    if (playerDead_ || dialogueView_.active) {
        return;
    }
    if (npcAttackTimers_.size() != scene_.entities.size()) {
        npcAttackTimers_.resize(scene_.entities.size(), 0.0f);
    }
    if (combatTarget_ != UINT32_MAX
        && (combatTarget_ >= scene_.entities.size() || scene_.entities[combatTarget_].dead)) {
        combatTarget_ = UINT32_MAX;
    }
    if (combatTarget_ == UINT32_MAX) {
        std::size_t best = static_cast<std::size_t>(-1);
        float bestDistance = 14.0f;
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& entity = scene_.entities[i];
            if (!entity.hostile || entity.dead) {
                continue;
            }
            const float distance = distance2D(position_, entity.position);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        if (best != static_cast<std::size_t>(-1)) {
            combatTarget_ = static_cast<std::uint32_t>(best);
            logEvent("combat started: " + combatTargetName());
        }
    }

    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        kotor::Entity& entity = scene_.entities[i];
        if (!entity.hostile || entity.dead) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < 14.0f && distance > 1.5f) {
            const float step = std::min(2.5f * dt, distance - 1.4f);
            const float dx = (position_.x - entity.position.x) / distance;
            const float dy = (position_.y - entity.position.y) / distance;
            entity.position.x += dx * step;
            entity.position.y += dy * step;
            entity.bearing = std::atan2(dy, dx);
        }
        npcAttackTimers_[i] = std::max(0.0f, npcAttackTimers_[i] - dt);
        if (distance <= 2.2f && npcAttackTimers_[i] <= 0.0f) {
            npcAttack(i);
            npcAttackTimers_[i] = 2.5f;
        }
    }

    playerAttackTimer_ = std::max(0.0f, playerAttackTimer_ - dt);
    if (combatTarget_ != UINT32_MAX
        && distance2D(position_, scene_.entities[combatTarget_].position) <= 2.2f
        && playerAttackTimer_ <= 0.0f) {
        playerAttack(combatTarget_);
        playerAttackTimer_ = 1.8f;
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
        const kotor::Entity& entity = scene_.entities[trigger.entity];
        const bool inside = pointInTrigger(trigger, position_.x, position_.y);
        if (inside == triggerInside_[i]) {
            continue;
        }
        triggerInside_[i] = inside;
        if (inside) {
            logEvent("trigger " + entity.tag + ": enter " + entity.scriptOnEnter);
        } else {
            logEvent("trigger " + entity.tag + ": exit " + entity.scriptOnExit);
        }
    }
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
    file << "schema=kotor.engine.player/1\n";
    file << "module=" << module_ << "\n";
    file << "x=" << position_.x << "\ny=" << position_.y << "\nz=" << position_.z << "\n";
    file << "yaw=" << yaw_ << "\npitch=" << pitch_ << "\n";
    file << "hp=" << playerHp_ << "\n";
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
    while (std::getline(file, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "module") {
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
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    if (module != module_) {
        if (!loadModule(module, &position, {})) {
            return false;
        }
    }
    position_ = position;
    yaw_ = yaw;
    pitch_ = pitch;
    playerHp_ = std::max(1, hp);
    playerDead_ = false;
    return true;
}

} // namespace ke
