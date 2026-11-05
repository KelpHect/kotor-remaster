// The SWMG_* script actions, EndGame, and the kotor_family_minigame3d gate. Handlers
// are thin: the rules live in MiniGame3D.cpp. Addresses are retail's action handlers.
#include "game/MiniGame3D.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ke {

namespace {

using Result = std::optional<std::vector<script::Value>>;
constexpr std::uint32_t kInvalid = script::kObjectInvalid;

MiniGameWorld* mg(ActionCall& call)
{
    return call.api().sim().miniGame.get();
}

MgFollower* followerArg(ActionCall& call, std::size_t index = 0)
{
    MiniGameWorld* world = mg(call);
    return world != nullptr ? world->follower(call.objectArg(index)) : nullptr;
}

MgObject* selfObject(ActionCall& call)
{
    MiniGameWorld* world = mg(call);
    return world != nullptr ? world->object(call.self()) : nullptr;
}

kotor::MgGunBankData* bankArg(ActionCall& call)
{
    MgFollower* follower = followerArg(call);
    const std::int64_t bank = call.intArg(1);
    if (follower == nullptr || bank < 0 || static_cast<std::size_t>(bank) >= follower->banks.size()) {
        return nullptr;
    }
    return &follower->banks[static_cast<std::size_t>(bank)];
}

// No minigame world: fall through to the legacy path, which keeps the VM corpus oracle.
#define MG_WORLD(name)                     \
    MiniGameWorld* name = mg(call);        \
    if ((name) == nullptr) {               \
        return std::nullopt;               \
    }

#define HANDLER [](ActionCall & call) -> Result

std::string lowerName(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// retail 0066c9f0 and neighbours keep these per sound; nothing shipped calls them.
std::size_t soundSlot(std::int64_t sound)
{
    return sound == 1 ? 1u : 0u;
}

} // namespace

void registerMiniGameActions(HandlerTable& table)
{
    // ---- lateral tuning, 005ccc60 family ----
    table.add(520, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (value >= 0.0f) {
            world->lateralAccel = value;
        }
        return ActionCall::none();
    });
    table.add(521, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->lateralAccel);
    });
    table.add(563, HANDLER { // 0044f130. The state is kept; no post pass draws the blur yet.
        MG_WORLD(world);
        world->speedBlur = call.intArg(0) != 0;
        world->speedBlurRatio = static_cast<float>(call.floatArg(1, 0.75));
        return ActionCall::none();
    });

    // ---- events ----
    table.add(583, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retString(self != nullptr ? self->lastEvent : std::string{});
    });
    table.add(584, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retString(self != nullptr ? self->lastEventModel : std::string{});
    });
    table.add(585, HANDLER { // 005cb650, 0066bfb0
        MG_WORLD(world);
        const MgObject* found = world->byName(call.stringArg(0));
        return ActionCall::retObject(found != nullptr ? found->object : kInvalid);
    });
    table.add(586, HANDLER { // 005cb730
        MG_WORLD(world);
        if (MgObject* target = world->object(call.objectArg(0))) {
            unsigned flags = call.intArg(2, 1) == 0 ? 1u : 0u;
            if (call.intArg(4) != 0) {
                flags |= 8u;
            }
            if (call.intArg(3) != 0) {
                flags |= 0x20u;
            }
            world->play(*target, call.stringArg(1), 1.0f, flags);
        }
        return ActionCall::none();
    });

    // ---- bullets and collisions ----
    table.add(587, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retInt(self != nullptr ? self->lastBulletDamage : 0);
    });
    table.add(588, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retInt(self != nullptr ? self->lastBulletTarget : 0);
    });
    table.add(589, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retObject(self != nullptr ? self->lastBulletShooter : kInvalid);
    });
    table.add(590, HANDLER { // nwscript: nAbsolute names the value to set.
        MG_WORLD(world);
        MgFollower* target = followerArg(call);
        if (target == nullptr) {
            return ActionCall::retInt(0);
        }
        const std::int32_t amount = static_cast<std::int32_t>(call.intArg(1));
        const bool absolute = call.intArg(2) != 0;
        world->adjustHitPoints(call.api(), *target, absolute ? amount - target->hitPoints : amount);
        return ActionCall::retInt(target->hitPoints);
    });
    table.add(591, HANDLER { // 0066c190(1): the default, from inside a replacing script.
        MG_WORLD(world);
        if (MgObject* self = selfObject(call)) {
            world->onBulletHit(call.api(), *self, true);
        }
        return ActionCall::none();
    });
    table.add(592, HANDLER { // 0066e5c0(1)
        MG_WORLD(world);
        if (MgFollower* self = world->follower(call.self())) {
            world->onObstacleHit(call.api(), *self, true);
        }
        return ActionCall::none();
    });
    table.add(593, HANDLER {
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retObject(self != nullptr ? self->lastFollowerHit : kInvalid);
    });
    table.add(594, HANDLER {
        MG_WORLD(world);
        const MgFollower* self = world->follower(call.self());
        return ActionCall::retObject(self != nullptr ? self->lastObstacleHit : kInvalid);
    });
    table.add(595, HANDLER {
        MG_WORLD(world);
        const MgFollower* self = world->follower(call.self());
        return ActionCall::retInt(self != nullptr ? self->lastFiredDamage : 0);
    });
    table.add(596, HANDLER {
        MG_WORLD(world);
        const MgFollower* self = world->follower(call.self());
        return ActionCall::retInt(self != nullptr ? self->lastFiredTarget : 0);
    });
    table.add(597, HANDLER {
        MG_WORLD(world);
        const MgObject* target = world->object(call.objectArg(0));
        return ActionCall::retString(target != nullptr ? target->name : std::string{});
    });
    table.add(598, HANDLER { // 0066e2a0(1)
        MG_WORLD(world);
        if (MgFollower* self = world->follower(call.self())) {
            world->onDeath(call.api(), *self, true);
        }
        return ActionCall::none();
    });

    // ---- classification ----
    table.add(599, HANDLER {
        MG_WORLD(world);
        return ActionCall::retInt(world->follower(call.objectArg(0)) != nullptr ? 1 : 0);
    });
    table.add(600, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = world->follower(call.objectArg(0));
        return ActionCall::retInt(target != nullptr && target->isPlayer ? 1 : 0);
    });
    table.add(601, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = world->follower(call.objectArg(0));
        return ActionCall::retInt(target != nullptr && !target->isPlayer ? 1 : 0);
    });
    table.add(602, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = world->follower(call.objectArg(0));
        return ActionCall::retInt(target != nullptr && !target->isPlayer && target->trigger ? 1 : 0);
    });
    table.add(603, HANDLER {
        MG_WORLD(world);
        const MgObject* target = world->object(call.objectArg(0));
        return ActionCall::retInt(target != nullptr && !target->isFollower ? 1 : 0);
    });

    // ---- hit points ----
    table.add(604, HANDLER { // 0066c900: only a positive value is taken.
        MG_WORLD(world);
        const std::int32_t value = static_cast<std::int32_t>(call.intArg(1));
        if (MgFollower* target = followerArg(call); target != nullptr && value > 0) {
            target->hitPoints = value;
        }
        return ActionCall::none();
    });
    table.add(605, HANDLER { // 0066e7a0(1)
        MG_WORLD(world);
        if (MgFollower* self = world->follower(call.self())) {
            world->onDamage(call.api(), *self, true);
        }
        return ActionCall::none();
    });
    table.add(606, HANDLER {
        MG_WORLD(world);
        const MgFollower* self = world->follower(call.self());
        return ActionCall::retInt(self != nullptr ? self->lastHpChange : 0);
    });
    table.add(607, HANDLER {
        MG_WORLD(world);
        if (MgObject* target = world->object(call.objectArg(0))) {
            world->removeAnimation(*target, call.stringArg(1));
        }
        return ActionCall::none();
    });

    // ---- camera ----
    table.add(608, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->nearClip);
    });
    table.add(609, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->farClip);
    });
    table.add(610, HANDLER { // 00670b50
        MG_WORLD(world);
        world->nearClip = static_cast<float>(call.floatArg(0));
        world->farClip = static_cast<float>(call.floatArg(1));
        return ActionCall::none();
    });

    // ---- rosters ----
    table.add(611, HANDLER {
        MG_WORLD(world);
        return ActionCall::retObject(world->player() != nullptr ? world->player()->object : kInvalid);
    });
    table.add(612, HANDLER {
        MG_WORLD(world);
        return ActionCall::retInt(static_cast<std::int64_t>(world->enemies().size()));
    });
    table.add(613, HANDLER {
        MG_WORLD(world);
        const std::int64_t entry = call.intArg(0);
        if (entry < 0 || static_cast<std::size_t>(entry) >= world->enemies().size()) {
            return ActionCall::retObject(kInvalid);
        }
        return ActionCall::retObject(world->enemies()[static_cast<std::size_t>(entry)]->object);
    });
    table.add(614, HANDLER {
        MG_WORLD(world);
        return ActionCall::retInt(static_cast<std::int64_t>(world->obstacles().size()));
    });
    table.add(615, HANDLER {
        MG_WORLD(world);
        const std::int64_t entry = call.intArg(0);
        if (entry < 0 || static_cast<std::size_t>(entry) >= world->obstacles().size()) {
            return ActionCall::retObject(kInvalid);
        }
        return ActionCall::retObject(world->obstacles()[static_cast<std::size_t>(entry)]->object);
    });

    // ---- follower fields ----
    table.add(616, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->hitPoints : 0);
    });
    table.add(617, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->maxHitPoints : 0);
    });
    table.add(618, HANDLER {
        MG_WORLD(world);
        const std::int32_t value = static_cast<std::int32_t>(call.intArg(1));
        if (MgFollower* target = followerArg(call); target != nullptr && value > 0) {
            target->maxHitPoints = value;
        }
        return ActionCall::none();
    });
    table.add(619, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retFloat(target != nullptr ? target->sphereRadius : 0.0f);
    });
    table.add(620, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(1));
        if (MgFollower* target = followerArg(call); target != nullptr && value >= 0.0f) {
            target->sphereRadius = value;
        }
        return ActionCall::none();
    });
    table.add(621, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->numLoops : 0);
    });
    table.add(622, HANDLER { // 0066cf20 for the player, 0066d090 for an enemy.
        MG_WORLD(world);
        const std::int32_t value = static_cast<std::int32_t>(call.intArg(1));
        if (MgFollower* target = followerArg(call)) {
            const bool valid = target->isPlayer ? value >= -1 && value != 0 : std::abs(value) == 1;
            if (valid) {
                target->numLoops = value;
            }
        }
        return ActionCall::none();
    });
    table.add(623, HANDLER { // 0066d5d0: the first model's world position.
        MG_WORLD(world);
        const MgObject* target = world->object(call.objectArg(0));
        if (target == nullptr || target->models.empty()) {
            return ActionCall::retVector({});
        }
        return ActionCall::retVector(target->models.front()->position);
    });

    // ---- gun banks: the getters read, the setters keep retail's >= 0 validation ----
    table.add(624, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? static_cast<std::int64_t>(target->banks.size()) : 0);
    });
    table.add(625, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retString(bank != nullptr ? bank->bulletModel : std::string{});
    });
    table.add(626, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retString(bank != nullptr ? bank->gunModel : std::string{});
    });
    table.add(627, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retInt(bank != nullptr ? bank->damage : 0);
    });
    table.add(628, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->rateOfFire : 0.0f);
    });
    table.add(629, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->lifespan : 0.0f);
    });
    table.add(630, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->speed : 0.0f);
    });
    table.add(631, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retInt(bank != nullptr ? bank->targetType : 0);
    });
    table.add(632, HANDLER {
        MG_WORLD(world);
        MgFollower* target = followerArg(call);
        if (target != nullptr && call.intArg(1) >= 0) {
            world->setBulletModel(*target, static_cast<std::size_t>(call.intArg(1)), call.stringArg(2));
        }
        return ActionCall::none();
    });
    table.add(633, HANDLER { // The name reads back; the gun's model stays what was loaded.
        MG_WORLD(world);
        if (kotor::MgGunBankData* bank = bankArg(call)) {
            bank->gunModel = call.stringArg(2);
        }
        return ActionCall::none();
    });
    table.add(634, HANDLER {
        MG_WORLD(world);
        const std::int32_t value = static_cast<std::int32_t>(call.intArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0) {
            bank->damage = value;
        }
        return ActionCall::none();
    });
    table.add(635, HANDLER { // 00673970
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->rateOfFire = value;
        }
        return ActionCall::none();
    });
    table.add(636, HANDLER { // 006739c0: -1 is also taken.
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && (value >= 0.0f || value == -1.0f)) {
            bank->lifespan = value;
        }
        return ActionCall::none();
    });
    table.add(637, HANDLER { // 006739f0
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->speed = value;
        }
        return ActionCall::none();
    });
    table.add(638, HANDLER {
        MG_WORLD(world);
        if (kotor::MgGunBankData* bank = bankArg(call)) {
            bank->targetType = static_cast<std::int32_t>(call.intArg(2));
        }
        return ActionCall::none();
    });
    table.add(639, HANDLER { // 005ccbb0
        MG_WORLD(world);
        const MgObject* self = selfObject(call);
        return ActionCall::retString(self != nullptr ? self->lastBulletPart : std::string{});
    });
    table.add(640, HANDLER { // The enemy gun's in-range flag, retail +0x4c.
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        const std::int64_t bank = call.intArg(1);
        std::int64_t targeting = 0;
        if (target != nullptr) {
            for (const std::unique_ptr<MgGun>& gun : target->guns) {
                if (static_cast<std::int64_t>(gun->bank) == bank && gun->inRange) {
                    targeting = 1;
                }
            }
        }
        return ActionCall::retInt(targeting);
    });

    // ---- the player's track and tunnel ----
    table.add(641, HANDLER {
        MG_WORLD(world);
        return ActionCall::retVector(world->offset);
    });
    table.add(642, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->player() != nullptr ? world->player()->invulnerable : 0.0f);
    });
    table.add(643, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->player() != nullptr ? world->player()->speed : 0.0f);
    });
    table.add(644, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->minimumSpeed);
    });
    table.add(645, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->acceleration);
    });
    table.add(646, HANDLER {
        MG_WORLD(world);
        return ActionCall::retVector(world->tunnelPositive);
    });
    table.add(647, HANDLER {
        MG_WORLD(world);
        world->offset = call.vectorArg(0);
        world->clampOffset();
        return ActionCall::none();
    });
    table.add(648, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (world->player() != nullptr && value >= 0.0f) {
            world->player()->invulnerable = value;
        }
        return ActionCall::none();
    });
    table.add(649, HANDLER { // 005cce70, 0066c870
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (world->player() != nullptr && value >= 0.0f) {
            world->setTrackSpeed(*world->player(), value);
        }
        return ActionCall::none();
    });
    table.add(650, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (value >= 0.0f) {
            world->minimumSpeed = value;
        }
        return ActionCall::none();
    });
    table.add(651, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (value >= 0.0f) {
            world->acceleration = value;
        }
        return ActionCall::none();
    });
    table.add(652, HANDLER {
        MG_WORLD(world);
        world->tunnelPositive = call.vectorArg(0);
        return ActionCall::none();
    });
    table.add(653, HANDLER {
        MG_WORLD(world);
        return ActionCall::retVector(world->tunnelNegative);
    });
    table.add(654, HANDLER {
        MG_WORLD(world);
        world->tunnelNegative = call.vectorArg(0);
        return ActionCall::none();
    });
    table.add(655, HANDLER {
        MG_WORLD(world);
        return ActionCall::retVector(world->origin);
    });
    table.add(656, HANDLER {
        MG_WORLD(world);
        world->origin = call.vectorArg(0);
        return ActionCall::none();
    });

    // ---- enemy aim ----
    table.add(657, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->horizontalSpread : 0.0f);
    });
    table.add(658, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->verticalSpread : 0.0f);
    });
    table.add(659, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->sensingRadius : 0.0f);
    });
    table.add(660, HANDLER {
        MG_WORLD(world);
        const kotor::MgGunBankData* bank = bankArg(call);
        return ActionCall::retFloat(bank != nullptr ? bank->inaccuracy : 0.0f);
    });
    table.add(661, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->horizontalSpread = value;
        }
        return ActionCall::none();
    });
    table.add(662, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->verticalSpread = value;
        }
        return ActionCall::none();
    });
    table.add(663, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->sensingRadius = value;
        }
        return ActionCall::none();
    });
    table.add(664, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(2));
        if (kotor::MgGunBankData* bank = bankArg(call); bank != nullptr && value >= 0.0f) {
            bank->inaccuracy = value;
        }
        return ActionCall::none();
    });
    table.add(665, HANDLER { // 005cb440: invulnerable while the +0x9c timer runs.
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr && target->invulnerable > 0.0f ? 1 : 0);
    });
    table.add(666, HANDLER { // +0x9c = +0xa0, the Invince_Period.
        MG_WORLD(world);
        if (MgFollower* target = followerArg(call)) {
            target->invulnerable = target->invincePeriod;
        }
        return ActionCall::none();
    });
    table.add(667, HANDLER {
        MG_WORLD(world);
        return ActionCall::retFloat(world->maximumSpeed);
    });
    table.add(668, HANDLER {
        MG_WORLD(world);
        const float value = static_cast<float>(call.floatArg(0));
        if (value >= 0.0f) {
            world->maximumSpeed = value;
        }
        return ActionCall::none();
    });

    // ---- sound fields: kept and read back; the mixer plays fixed-frequency effects ----
    table.add(683, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->soundFrequency[soundSlot(call.intArg(1))] : 0);
    });
    table.add(684, HANDLER {
        MG_WORLD(world);
        if (MgFollower* target = followerArg(call)) {
            target->soundFrequency[soundSlot(call.intArg(1))] = static_cast<std::int32_t>(call.intArg(2));
        }
        return ActionCall::none();
    });
    table.add(685, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->soundFrequencyRandom[soundSlot(call.intArg(1))] : 0);
    });
    table.add(686, HANDLER {
        MG_WORLD(world);
        if (MgFollower* target = followerArg(call)) {
            target->soundFrequencyRandom[soundSlot(call.intArg(1))] = static_cast<std::int32_t>(call.intArg(2));
        }
        return ActionCall::none();
    });
    table.add(687, HANDLER {
        MG_WORLD(world);
        const MgFollower* target = followerArg(call);
        return ActionCall::retInt(target != nullptr ? target->soundVolume[soundSlot(call.intArg(1))] : 0);
    });
    table.add(688, HANDLER {
        MG_WORLD(world);
        if (MgFollower* target = followerArg(call)) {
            target->soundVolume[soundSlot(call.intArg(1))] = static_cast<std::int32_t>(call.intArg(2));
        }
        return ActionCall::none();
    });
    table.add(717, HANDLER {
        MG_WORLD(world);
        return ActionCall::retVector(world->tunnelInfinite);
    });
    table.add(718, HANDLER {
        MG_WORLD(world);
        world->tunnelInfinite = call.vectorArg(0);
        return ActionCall::none();
    });

}

void createMiniGame(ScriptApi api)
{
    api.sim().miniGame.reset();
    api.sim().miniGame = MiniGameWorld::create(api);
    if (api.sim().miniGame) {
        api.sim().miniGame->seed = api.sim().miniGameSeed;
    }
}

void startMiniGame(ScriptApi api)
{
    if (MiniGameWorld* world = api.sim().miniGame.get()) {
        world->start(api);
    }
}

void tickMiniGame(ScriptApi api, float dt)
{
    if (MiniGameWorld* world = api.sim().miniGame.get()) {
        world->tick(api, dt);
    }
}

// ---- the gate -------------------------------------------------------------------

namespace {

constexpr float kTick = 1.0f / 30.0f;

struct GateLog {
    bool ok = true;
    void check(bool condition, const std::string& label)
    {
        logInfo(concat("minigame3d: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    }
};

std::int64_t globalNumber(GameSession& game, const std::string& name)
{
    const auto found = game.scriptWorld().globals.find(name);
    return found != game.scriptWorld().globals.end() ? found->second.asInt() : 0;
}

void setGlobal(GameSession& game, const std::string& name, std::int64_t value)
{
    game.scriptWorld().globals[name] = script::Value::makeInt(value);
}

// One tick with player-level input only.
void step(GameSession& game, const MiniGameInput& input)
{
    if (game.moviePlaying()) {
        game.stopMovie();
    }
    game.miniGameSetInput(input);
    game.update(kTick, InputState{});
}

struct RaceResult {
    bool finishedOnTrack = false; // The player passed the shipped finish line.
    bool reachedGearSix = false;
    std::string returnModule;
    float finishY = 0.0f;
    float peakSpeed = 0.0f;
    std::size_t triggerHits = 0;
    std::size_t obstacleHits = 0;
    int ticks = 0;
    int ticksAtFinish = 0;
    std::uint64_t hashAtFinish = 0;
    std::int64_t raceSeconds = 0;
    float offsetMaxX = 0.0f;
};

// Plays a race. `steer` picks routes around obstacles and over boost pads; without it
// the swoop runs straight down the tunnel and takes whatever it meets. `maxGear` caps
// how far the bot shifts, so a slow run is a real slow run.
RaceResult playRace(GameSession& game, bool steer, std::int64_t maxGear, const std::string& timeGlobal)
{
    RaceResult result;
    const std::string startModule = game.module();
    int fireCooldown = 0;
    for (int tick = 0; tick < 9000; ++tick) {
        if (game.module() != startModule) {
            result.returnModule = game.module();
            break;
        }
        const MiniGameView view = game.miniGameView();
        if (!view.active) {
            break;
        }
        const std::int64_t gear = globalNumber(game, "MIN_RACE_GEAR");
        MiniGameInput input;
        if (gear >= 0 && gear < maxGear && --fireCooldown <= 0) {
            input.fire = true;
            fireCooldown = 8;
        }
        if (steer && gear >= 1) {
            const float x = view.playerPosition.x;
            const float y = view.playerPosition.y;
            float wanted = x;
            float bestPad = 1e9f;
            for (const MiniGameFollowerView& pad : view.enemies) {
                const float ahead = pad.position.y - y;
                if (pad.trigger && ahead > 10.0f && ahead < bestPad) {
                    bestPad = ahead;
                    wanted = pad.position.x;
                }
            }
            float bestObstacle = 1e9f;
            for (const MiniGameObstacleView& obstacle : view.obstacles) {
                const float ahead = obstacle.position.y - y;
                if (ahead > 0.0f && ahead < 150.0f && ahead < bestObstacle
                    && std::fabs(obstacle.position.x - wanted) < 8.0f) {
                    bestObstacle = ahead;
                    wanted = obstacle.position.x + (x >= obstacle.position.x ? 9.0f : -9.0f);
                }
            }
            input.axisX = std::clamp((wanted - x) * 0.25f, -1.0f, 1.0f);
        }
        step(game, input);
        result.ticks = tick + 1;
        if (!game.miniGameActive()) {
            continue;
        }
        const MiniGameView after = game.miniGameView();
        result.peakSpeed = std::max(result.peakSpeed, after.speed);
        result.triggerHits = after.triggerHits;
        result.obstacleHits = after.obstacleHits;
        result.offsetMaxX = std::max(result.offsetMaxX, std::fabs(after.playerOffset.x));
        if (!result.reachedGearSix && globalNumber(game, "MIN_RACE_GEAR") == 6) {
            result.reachedGearSix = true;
            result.finishY = after.playerPosition.y;
            result.finishedOnTrack = after.playerPosition.y > 3800.0f;
            result.ticksAtFinish = tick;
            result.hashAtFinish = game.miniGame()->traceHash();
            result.raceSeconds = globalNumber(game, timeGlobal + "_MIN") * 60 + globalNumber(game, timeGlobal + "_SEC");
        }
    }
    return result;
}

struct TurretResult {
    std::size_t kills = 0;
    std::size_t bulletHits = 0;
    std::size_t fired = 0;
    bool damageMultiples = true;
    std::int32_t finalHitPoints = 0;
    bool lost = false;
    std::uint64_t hash = 0;
    std::string endModule;
    int ticks = 0;
};

// Aims with the mouse axes and holds the trigger, seeing only the public view: shots cost
// nothing and the bank's Rate_Of_Fire paces them. It leads each fighter by the bullet's
// flight time.
TurretResult playTurret(GameSession& game, int tickLimit)
{
    TurretResult result;
    const std::string startModule = game.module();
    const float radiansToDegrees = 57.29578f;
    std::size_t initial = game.miniGameView().enemies.size();
    for (int tick = 0; tick < tickLimit; ++tick) {
        if (game.module() != startModule || !game.miniGameActive()) {
            result.endModule = game.module();
            break;
        }
        const MiniGameView view = game.miniGameView();
        const float rate = game.miniGame()->movementPerSec * kTick;
        MiniGameInput input;
        // The lowest angular error among reachable fighters wins.
        float bestScore = 1e9f;
        float headingError = 0.0f;
        float pitchError = 0.0f;
        for (const MiniGameFollowerView& enemy : view.enemies) {
            const kotor::Vec3 to = enemy.position - view.aimOrigin;
            // The intercept converges in a few rounds at the fighters' speeds.
            kotor::Vec3 intercept = to;
            for (int round = 0; round < 3; ++round) {
                const float flight = kotor::length(intercept) / std::max(view.bulletSpeed, 1.0f);
                intercept = to + enemy.velocity * flight;
            }
            const kotor::Vec3 lead = kotor::normalize(intercept);
            const float wantHeading = std::atan2(lead.x, lead.y) * radiansToDegrees;
            const float wantPitch = std::asin(std::clamp(lead.z, -1.0f, 1.0f)) * radiansToDegrees;
            const float haveHeading = std::atan2(view.aimDirection.x, view.aimDirection.y) * radiansToDegrees;
            const float havePitch = std::asin(std::clamp(view.aimDirection.z, -1.0f, 1.0f)) * radiansToDegrees;
            const float dh = std::remainder(wantHeading - haveHeading, 360.0f);
            const float dp = wantPitch - havePitch;
            const float distance = kotor::length(to);
            const bool reachable = wantPitch >= -5.0f && wantPitch <= 47.0f && distance < view.bulletSpeed * 3.0f;
            const float score = std::fabs(dh) + std::fabs(dp) + (reachable ? 0.0f : 1000.0f);
            if (score < bestScore) {
                bestScore = score;
                headingError = dh;
                pitchError = dp;
            }
            if ((enemy.maxHitPoints - enemy.hitPoints) % 30 != 0) {
                result.damageMultiples = false;
            }
        }
        if (rate > 0.0f && bestScore < 1000.0f) {
            // Retail 00671020 does not clamp a mouse delta. The limit is this bot's own: a
            // hand's flick of six times MovementPerSec, not a rule of the game.
            const float flick = 6.0f;
            input.mouseX = std::clamp(headingError / rate, -flick, flick);
            input.mouseY = std::clamp(-pitchError / rate, -flick, flick);
            input.fire = true;
        }
        step(game, input);
        result.ticks = tick + 1;
        if (game.gameEnded()) {
            break;
        }
        if (game.miniGameActive()) {
            const MiniGameView after = game.miniGameView();
            result.kills = initial - after.enemies.size();
            result.bulletHits = after.playerBulletHits;
            result.fired = after.playerBulletsFired;
            result.finalHitPoints = after.hitPoints;
            result.lost = result.lost || globalNumber(game, "M12AB_END_SYNC") != 0;
            if (tick % 300 == 0) {
                logInfo(concat("minigame3d: turret t=", tick, " hp=", after.hitPoints, " enemies=",
                               after.enemies.size(), " shots=", after.playerBulletsFired, " onEnemy=",
                               after.playerBulletHits, " score=", bestScore, " dh=", headingError, " dp=",
                               pitchError));
            }
            result.hash = game.miniGame()->traceHash();
        }
    }
    if (game.module() != startModule) {
        result.endModule = game.module();
    }
    return result;
}

bool dialogueNamesScript(GameSession& game, const std::string& dlg, const std::string& scriptName)
{
    std::optional<std::vector<std::uint8_t>> bytes = ScriptApi{game}.install().find(dlg, 2029);
    if (!bytes.has_value()) {
        return false;
    }
    const kotor::Gff gff = kotor::Gff::parse(std::move(*bytes), dlg + ".dlg");
    for (const char* list : {"EntryList", "ReplyList"}) {
        const kotor::GffField* field = gff.field(gff.rootIndex(), list);
        if (field == nullptr) {
            continue;
        }
        for (const std::uint32_t row : gff.list(*field)) {
            const kotor::GffField* name = gff.field(row, "Script");
            if (name != nullptr && lowerName(gff.text(*name)) == scriptName) {
                return true;
            }
        }
    }
    return false;
}

// Runs the shipped entry script from a normal module and waits for its StartNewModule.
bool enterThroughScript(GameSession& game, const std::string& scriptName, const std::string& target)
{
    ScriptApi api{game};
    api.runScript(scriptName, game.scriptWorld().pc, ScriptContext{});
    for (int tick = 0; tick < 300; ++tick) {
        if (game.moviePlaying()) {
            game.stopMovie();
        }
        game.update(kTick, InputState{});
        if (lowerName(game.module()) == lowerName(target)) {
            return game.miniGameActive();
        }
    }
    return false;
}

void checkMath(GateLog& log, const std::string& installPath)
{
    GameSession game(installPath, "tar_m03mg");
    MiniGameWorld* world = game.miniGame();
    log.check(world != nullptr, "math_world");
    if (world == nullptr) {
        return;
    }
    // Lateral integrator against the closed form of dv = a*u - (a/m)*v.
    world->lateralAccel = 300.0f;
    world->movementPerSec = 100.0f;
    const int steps = 45;
    for (int i = 0; i < steps; ++i) {
        world->stepLateral({1.0f, 0.0f, 0.0f}, kTick);
    }
    const float t = static_cast<float>(steps) * kTick;
    const float expectedV = 100.0f * (1.0f - std::exp(-3.0f * t));
    const float expectedP = 100.0f * t - (100.0f / 3.0f) * (1.0f - std::exp(-3.0f * t));
    log.check(std::fabs(world->lateralVelocity().x - expectedV) < 0.05f, "math_rk4_velocity");
    log.check(std::fabs(world->lateralPosition().x - expectedP) < 0.05f, "math_rk4_position");
    log.check(world->lateralVelocity().y == 0.0f && world->lateralVelocity().z == 0.0f, "math_rk4_other_axes");

    // The track: y advances by the hook's keyed span at speed * 0.01 clip rate.
    world->tunnelPositive = {20.0f, 0.0f, 0.0f};
    world->tunnelNegative = {-20.0f, 0.0f, 0.0f};
    world->maximumSpeed = 100.0f;
    world->acceleration = 0.0f;
    world->setTrackSpeed(*world->player(), 100.0f);
    const float y0 = game.miniGameView().playerPosition.y;
    for (int i = 0; i < 90; ++i) {
        step(game, {});
    }
    const float travelled = game.miniGameView().playerPosition.y - y0;
    const float expectedY = 4980.0f / 48.0f * (90.0f * kTick);
    logInfo(concat("minigame3d: track travelled=", travelled, " expected=", expectedY));
    log.check(std::fabs(travelled - expectedY) < 1.0f, "math_track_rate");

    // The speed ramp: toward maximum at the acceleration, then stopping there.
    world->maximumSpeed = 120.0f;
    world->acceleration = 10.0f;
    for (int i = 0; i < 30; ++i) {
        step(game, {});
    }
    log.check(std::fabs(game.miniGameView().speed - 110.0f) < 0.05f, "math_speed_ramp");
    world->acceleration = 1000.0f;
    step(game, {});
    log.check(game.miniGameView().speed == 120.0f, "math_speed_ramp_stops");

    // Holding a direction ends against the tunnel wall, and no further.
    MiniGameInput right;
    right.axisX = 1.0f;
    for (int i = 0; i < 60; ++i) {
        step(game, right);
    }
    log.check(game.miniGameView().playerOffset.x == 20.0f, "math_tunnel_clamp");

    // Boost pads: a trigger met in the player's path runs the shipped accelpad script.
    const float accelBefore = world->acceleration;
    const std::size_t hitsBefore = game.miniGameView().triggerHits;
    world->player()->invulnerable = 0.0f;
    MgFollower* pad = nullptr;
    const float y = game.miniGameView().playerPosition.y;
    for (MgFollower* candidate : world->enemies()) {
        const kotor::Vec3 at = candidate->models.front()->position;
        if (candidate->trigger && at.y > y && at.y < y + 150.0f
            && (pad == nullptr || at.y < pad->models.front()->position.y)) {
            pad = candidate;
        }
    }
    log.check(pad != nullptr, "pad_ahead");
    if (pad != nullptr) {
        // Sit on the pad's line and wait for the track to carry the player over it.
        world->tunnelPositive = {60.0f, 0.0f, 0.0f};
        world->tunnelNegative = {-60.0f, 0.0f, 0.0f};
        for (int i = 0; i < 90 && game.miniGameView().triggerHits == hitsBefore; ++i) {
            world->setPlayerOffsetForTest(pad->models.front()->position);
            step(game, {});
        }
        log.check(game.miniGameView().triggerHits > hitsBefore, "pad_trigger_collision");
        const std::vector<std::string>& ran = world->scriptLog();
        log.check(!ran.empty() && ran.back().rfind("accelpad:", 0) == 0, "pad_runs_shipped_script");
        log.check(world->player()->invulnerable > 0.0f, "pad_shipped_script_started_invulnerability");
        log.check(world->acceleration >= accelBefore, "pad_acceleration_not_reduced");
    }
}

void checkRace(GateLog& log, const std::string& installPath, const std::string& normalModule,
               const std::string& entryDialogue, const std::string& entryScript, const std::string& raceModule,
               const std::string& returnModule, const std::string& returnWaypoint, const std::string& timeGlobal)
{
    const std::string tag = raceModule + "_";
    std::uint64_t firstHash = 0;
    std::int64_t firstSeconds = 0;
    {
        // Entry through the shipped dialogue's script, from the module the story uses.
        GameSession game(installPath, normalModule);
        log.check(dialogueNamesScript(game, entryDialogue, entryScript), tag + "entry_dialogue_names_script");
        log.check(enterThroughScript(game, entryScript, raceModule), tag + "entry_script_loads_module");
        const MiniGameView start = game.miniGameView();
        log.check(start.active && start.type == 1 && !start.enemies.empty() && !start.obstacles.empty(),
                  tag + "shipped_track_loaded");
        const RaceResult race = playRace(game, true, 5, timeGlobal);
        logInfo(concat("minigame3d: ", raceModule, " finishY=", race.finishY, " peak=", race.peakSpeed,
                       " pads=", race.triggerHits, " obstacles=", race.obstacleHits, " ticks=", race.ticks,
                       " seconds=", race.raceSeconds));
        log.check(race.reachedGearSix && race.finishedOnTrack, tag + "bot_crossed_shipped_finish");
        log.check(race.peakSpeed > 100.0f, tag + "bot_shifted_gears");
        log.check(race.triggerHits > 0, tag + "bot_hit_boost_pads");
        log.check(race.offsetMaxX > 0.0f && race.offsetMaxX <= 20.0f, tag + "lateral_within_shipped_tunnel");
        log.check(lowerName(race.returnModule) == lowerName(returnModule), tag + "shipped_return_module");
        const kotor::Vec3 at = game.position();
        bool nearWaypoint = false;
        for (const kotor::Entity& entity : game.scene().entities) {
            if (lowerName(entity.tag) == lowerName(returnWaypoint)) {
                nearWaypoint = kotor::length(entity.position - at) < 3.0f;
            }
        }
        log.check(nearWaypoint, tag + "shipped_return_waypoint");
        firstHash = race.hashAtFinish;
        firstSeconds = race.raceSeconds;
    }
    {
        // The same entry and inputs on a fresh session reproduce the simulation, exactly.
        GameSession game(installPath, normalModule);
        log.check(enterThroughScript(game, entryScript, raceModule), tag + "replay_entry");
        const RaceResult race = playRace(game, true, 5, timeGlobal);
        logInfo(concat("minigame3d: ", raceModule, " replay hash=", race.hashAtFinish, " first=", firstHash));
        log.check(race.hashAtFinish != 0 && race.hashAtFinish == firstHash, tag + "deterministic");
    }
    {
        // The slow path: a lower gear and no steering still ends through the shipped
        // finish, with a worse recorded time. The shipped race has no failure state of
        // its own; what a poor time means is decided later by shipped dialogue.
        GameSession game(installPath, raceModule);
        const RaceResult race = playRace(game, false, 1, timeGlobal);
        logInfo(concat("minigame3d: ", raceModule, " slow seconds=", race.raceSeconds, " obstacles=",
                       race.obstacleHits, " ticks=", race.ticks));
        log.check(race.reachedGearSix && lowerName(race.returnModule) == lowerName(returnModule),
                  tag + "slow_run_shipped_return");
        log.check(race.raceSeconds > firstSeconds, tag + "slow_run_recorded_time_worse");
    }
}

void checkTurret(GateLog& log, const std::string& installPath)
{
    {
        // Entry from the shipped STUNT_07 dialogue's script, with the skybox it sets.
        GameSession game(installPath, "STUNT_07");
        log.check(dialogueNamesScript(game, "scene_start", "k_ren_taris03"), "turret_entry_dialogue_names_script");
        setGlobal(game, "K_KOTOR_MASTER", 10);
        setGlobal(game, "K_STAR_MAP", 0);
        log.check(enterThroughScript(game, "k_ren_taris03", "M12ab"), "turret_entry_script_loads_module");
        log.check(globalNumber(game, "K_TURRET_SKYBOX") == 5, "turret_entry_sets_skybox");
        const MiniGameView start = game.miniGameView();
        log.check(start.type == 2 && start.enemies.size() == 6 && start.hitPoints == 3000,
                  "turret_shipped_enemies_loaded");
        log.check(globalNumber(game, "ebo_num_fighters") == 6, "turret_oncreate_counted_fighters");
        const std::vector<std::string>& created = game.miniGame()->scriptLog();
        log.check(std::count_if(created.begin(), created.end(),
                                [](const std::string& line) { return line.rfind("k_pebo_sthcreate:", 0) == 0; }) == 6,
                  "turret_oncreate_ran_for_each");
        const float headingBefore = start.playerOffset.z;
        MiniGameInput swing;
        swing.mouseX = -1.0f;
        for (int i = 0; i < 15; ++i) {
            step(game, swing);
        }
        const float moved = game.miniGameView().playerOffset.z - headingBefore;
        logInfo(concat("minigame3d: turret heading moved=", moved));
        log.check(std::fabs(moved - 50.0f) < 0.5f, "turret_mouse_moves_heading_at_movement_rate");
        // Past the pitch limits the tunnel holds.
        MiniGameInput up;
        up.mouseY = -1.0f;
        for (int i = 0; i < 40; ++i) {
            step(game, up);
        }
        log.check(game.miniGameView().playerOffset.x == 45.0f, "turret_pitch_clamped_by_tunnel");
        // The infinite axis wraps, as a heading.
        MiniGameInput around;
        around.mouseX = 1.0f;
        // Retail 0066cb40 adds a turn below 0 and subtracts one above 359 without
        // normalizing, so the value stays within [-1, 360] and crosses the seam.
        bool wrapped = true;
        int seams = 0;
        float headingMin = 1e9f;
        float headingMax = -1e9f;
        float previousHeading = game.miniGameView().playerOffset.z;
        for (int i = 0; i < 300; ++i) {
            step(game, around);
            const float z = game.miniGameView().playerOffset.z;
            wrapped = wrapped && z >= -1.0f && z <= 360.0f;
            seams += std::fabs(z - previousHeading) > 180.0f ? 1 : 0;
            headingMin = std::min(headingMin, z);
            headingMax = std::max(headingMax, z);
            previousHeading = z;
        }
        logInfo(concat("minigame3d: turret heading min=", headingMin, " max=", headingMax, " seams=", seams));
        log.check(wrapped && seams >= 2, "turret_heading_wrap");
        // A held trigger: 2 banks at Rate_Of_Fire 0.3 s.
        const std::size_t firedBefore = game.miniGameView().playerBulletsFired;
        MiniGameInput hold;
        hold.fire = true;
        for (int i = 0; i < 90; ++i) {
            step(game, hold);
        }
        const std::size_t fired = game.miniGameView().playerBulletsFired - firedBefore;
        logInfo(concat("minigame3d: turret held trigger fired=", fired));
        log.check(fired >= 16 && fired <= 22, "turret_rate_of_fire");
    }
    {
        // The fight, on its own session: the checks above stand idle under enemy fire.
        GameSession game(installPath, "STUNT_07");
        setGlobal(game, "K_KOTOR_MASTER", 10);
        setGlobal(game, "K_STAR_MAP", 0);
        log.check(enterThroughScript(game, "k_ren_taris03", "M12ab"), "turret_fight_entry");
        const TurretResult run = playTurret(game, 9000);
        logInfo(concat("minigame3d: turret kills=", run.kills, " hits=", run.bulletHits, " fired=", run.fired,
                       " ticks=", run.ticks, " end=", run.endModule, " hp=", run.finalHitPoints, " lost=", run.lost ? 1 : 0));
        log.check(run.kills == 6, "turret_all_fighters_destroyed");
        log.check(run.bulletHits >= 24, "turret_kills_came_from_bullet_collisions");
        log.check(run.damageMultiples, "turret_hit_damage_is_bank_damage");
        log.check(lowerName(run.endModule) == "ebo_m12aa", "turret_shipped_return_module");
        log.check(globalNumber(game, "K_TURRET_SKYBOX") == -1, "turret_shipped_end_script_ran");
    }
    {
        // Two runs with one seed match; another seed changes the enemies' scatter.
        const auto idleRun = [&installPath](std::uint32_t seed) {
            GameSession game(installPath, "M12ab");
            game.setMiniGameSeed(seed);
            for (int i = 0; i < 900; ++i) {
                step(game, {});
            }
            return game.miniGame()->traceHash();
        };
        const std::uint64_t first = idleRun(7);
        log.check(first == idleRun(7), "turret_deterministic_same_seed");
        log.check(first != idleRun(8), "turret_seed_changes_scatter");
    }
    {
        // Losing: nobody shoots back. The shipped OnDamage script ends the game.
        GameSession game(installPath, "M12ab");
        bool multiples = true;
        bool sawSync = false;
        bool pending = false;
        for (int i = 0; i < 16000 && game.miniGameActive() && !game.gameEnded(); ++i) {
            step(game, {});
            if (!game.miniGameActive()) {
                break;
            }
            const MiniGameView view = game.miniGameView();
            multiples = multiples && view.hitPoints % 10 == 0;
            sawSync = sawSync || globalNumber(game, "M12AB_END_SYNC") != 0;
            pending = pending || view.endGamePending;
        }
        logInfo(concat("minigame3d: turret loss sync=", sawSync ? 1 : 0, " pending=", pending ? 1 : 0));
        log.check(multiples, "turret_loss_damage_is_bank_damage");
        log.check(sawSync, "turret_loss_shipped_onDamage_threshold");
        log.check(pending, "turret_loss_shipped_endgame");
    }
}

} // namespace

int runMiniGame3DFamilyTest(GameSession& game)
{
    GateLog log;
    const std::string install = game.installPath();
    log.check(!game.miniGameActive(), "normal_module_has_no_minigame");

    // Shipped data, parsed in full, for each of the four modules the install has.
    {
        struct Expected {
            const char* module;
            std::int32_t type;
            std::size_t enemies;
            std::size_t obstacles;
            float far;
        };
        for (const Expected& expected : {Expected{"tar_m03mg", 1, 29, 22, 40000.0f},
                                         Expected{"tat_m17mg", 1, 30, 22, 40000.0f},
                                         Expected{"manm26mg", 1, 30, 22, 40000.0f},
                                         Expected{"M12ab", 2, 6, 1, 2000.0f}}) {
            GameSession session(install, expected.module);
            const MiniGameWorld* world = session.miniGame();
            const std::string tag = std::string(expected.module) + "_";
            log.check(world != nullptr, tag + "loaded");
            if (world == nullptr) {
                continue;
            }
            log.check(world->data.type == expected.type, tag + "type");
            log.check(world->data.farClip == expected.far, tag + "far_clip");
            log.check(world->data.enemies.size() == expected.enemies, tag + "enemies");
            log.check(world->data.obstacles.size() == expected.obstacles, tag + "obstacles");
            logInfo(concat("minigame3d: ", expected.module, " enemies=", world->data.enemies.size(),
                           " obstacles=", world->data.obstacles.size(), " track=", world->data.player.track,
                           " music=", world->data.music));
            // The render path: pooled world meshes exist, and hit nodes are not drawn.
            std::size_t worldMeshes = 0;
            for (const kotor::EntityMesh& mesh : session.scene().entityMeshes) {
                worldMeshes += mesh.entity == UINT32_MAX ? 1u : 0u;
            }
            log.check(worldMeshes > 0, tag + "world_meshes");
            const SceneCamera camera = session.camera();
            log.check(camera.farClip == expected.far && camera.nearClip > 0.0f, tag + "camera_clip_from_are");
            // A swoop looks down its track with Z up; a turret looks where its guns fire.
            const bool cameraAligned = expected.type == 1
                ? camera.forward.y > 0.9f && camera.up.z > 0.9f
                : kotor::dot(camera.forward, session.miniGameView().aimDirection) > 0.95f;
            logInfo(concat("minigame3d: ", expected.module, " camera forward=", camera.forward.x, ",", camera.forward.y,
                           ",", camera.forward.z, " up=", camera.up.x, ",", camera.up.y, ",", camera.up.z));
            log.check(cameraAligned, tag + "camera_from_shipped_hook");
        }
        const std::shared_ptr<kotor::Mdl> fighter = kotor::loadWorldModel(ScriptApi{game}.install(), "mgf_sithfighter");
        log.check(fighter && !fighter->nodeFaces("hitbullet").empty(), "hit_mesh_read");
        log.check(fighter && fighter->posedTrianglesLayered({}, true).size() < fighter->posedTrianglesLayered({}, false).size(),
                  "hit_mesh_not_drawn");
        log.check(fighter && !fighter->animationEvents("die").empty(), "animation_events_read");
    }

    checkMath(log, install);
    checkRace(log, install, "tar_m03af", "tar03_announcer", "k_ptar_swooprun", "tar_m03mg", "tar_m03af",
              "tar03_wpmechanic", "TAR_SWOOP");
    checkRace(log, install, "tat_m17ae", "tat17_04race_01", "k_ptat_swoop04", "tat_m17mg", "tat_m17ae",
              "tat17ae_swoop_return", "TAT_SWOOP");
    checkRace(log, install, "manm26ab", "man26_swpreg", "k_pman_swoop42", "manm26mg", "manm26ab", "from26mg",
              "MAN_SWOOP");
    checkTurret(log, install);

    logInfo(log.ok ? "minigame3d: PASS" : "minigame3d: FAIL");
    return log.ok ? 0 : 73;
}

} // namespace ke
