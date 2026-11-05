// Per-object action queue and Action*. Evidence cited by address; see
// docs/recon/63_WORLD_SIM.md. Every queued Action* blocks the actions behind it.
#include "game/ScriptHost.h"

#include "game/GameSession.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace ke {

namespace {

constexpr float kDegrees = 180.0f / 3.14159265358979f;
constexpr float kInteractRange = 2.5f;
constexpr float kConversationRange = 3.0f;
constexpr float kArrivalTolerance = 0.25f;
constexpr float kStuckGiveUp = 2.0f;
constexpr float kForceStuckJump = 3.0f;
// Non-looping animations with no duration block about one clip. Clip length is
// not known here for every actor (the PC may have no avatar).
constexpr float kOneShotAnimation = 1.0f;
constexpr int kInstantActionsPerTick = 32;
// Stops at this gap from the followed creature when the script names no distance
// (ActionFollowLeader). No retail value was recovered.
constexpr float kDefaultFollowDistance = 2.0f;
// How far one leg of ActionRandomWalk wanders. No retail value was recovered.
constexpr float kRandomWalkRadius = 5.0f;
constexpr float kRandomWalkPauseMax = 3.0f;

// nwscript ACTION_* constants.
constexpr std::int64_t kActionMoveToPoint = 0;
constexpr std::int64_t kActionOpenDoor = 5;
constexpr std::int64_t kActionCloseDoor = 6;
constexpr std::int64_t kActionDialogObject = 7;
constexpr std::int64_t kActionOpenLock = 13;
constexpr std::int64_t kActionLock = 14;
constexpr std::int64_t kActionWait = 36;
// Retail's internal DoCommand type is 0x25 (0057cb10), numerically ACTION_SIT.
// Scripts cannot tell them apart in retail either.
constexpr std::int64_t kActionDoCommand = 37;
constexpr std::int64_t kActionFollow = 35;
constexpr std::int64_t kActionInvalid = 65535;
constexpr std::int64_t kActionQueueEmpty = 65534;

using Cells = std::optional<std::vector<script::Value>>;

// ---- queue execution -------------------------------------------------------

enum class Progress { Running, Finished };

void jumpActor(ScriptApi api, std::uint32_t self, kotor::Vec3 point, std::optional<float> degrees)
{
    const float facing = degrees.has_value() ? *degrees / kDegrees : api.facing(self);
    api.setPosition(self, point, facing);
    // Settle on the walkmesh at the destination: step with zero distance snaps z.
    api.step(self, point, 0.0f, 0.0f, -1.0f);
}

Progress walkAction(ScriptApi api, std::uint32_t self, ActorAction& action, kotor::Vec3 target,
                    float stopDistance, float dt)
{
    const ActorStep result =
        api.step(self, target, api.moveSpeed(self, action.run), dt, stopDistance);
    if (result == ActorStep::Arrived) {
        return Progress::Finished;
    }
    if (action.force) {
        action.remaining -= dt;
    }
    action.stuck = result == ActorStep::Blocked ? action.stuck + dt : 0.0f;
    if (action.force && (action.remaining <= 0.0f || action.stuck >= kForceStuckJump)) {
        jumpActor(api, self, target, action.facingDegrees);
        return Progress::Finished;
    }
    if (!action.force && action.stuck >= kStuckGiveUp) {
        api.log("actor move gave up: stuck");
        return Progress::Finished;
    }
    return Progress::Running;
}

// Doors and locks are worked from arm's length, so walk there first.
bool reachObject(ScriptApi api, std::uint32_t self, ActorAction& action, float range, float dt,
                 Progress& out)
{
    const kotor::Vec3 target = api.position(action.object);
    out = walkAction(api, self, action, target, range, dt);
    return out == Progress::Finished;
}

void doorHook(ScriptApi api, std::size_t index, const char* label, const char* contextKey,
              std::uint32_t self)
{
    if (index == static_cast<std::size_t>(-1)) {
        return;
    }
    api.dispatchHook(api.objectFor(index), label, {{contextKey, self}});
}

// Runs the front action. Actions that can run a script pop themselves first,
// since that script may clear or extend this queue. Returns true when another
// action may run in the same tick.
bool runFrontAction(ScriptApi api, std::uint32_t self, float dt)
{
    ActorAction& action = api.actor(self).queue.front();
    const auto pop = [&]() {
        ActorAction taken = std::move(api.actor(self).queue.front());
        api.actor(self).queue.pop_front();
        return taken;
    };
    switch (action.kind) {
    case ActorActionKind::DoCommand: {
        const ActorAction taken = pop();
        api.runFunctor(taken.functor, self);
        return true;
    }
    case ActorActionKind::Wait:
        action.remaining -= dt;
        if (action.remaining > 0.0f) {
            return false;
        }
        pop();
        return true;
    case ActorActionKind::MoveToObject: {
        if (!api.world().valid(action.object) && api.entityFor(action.object) == static_cast<std::size_t>(-1)) {
            pop();
            return true;
        }
        const kotor::Vec3 target = api.position(action.object);
        const float reach = api.approachRange(self, action.object, action.range);
        if (walkAction(api, self, action, target, std::max(reach, kArrivalTolerance), dt)
            == Progress::Finished) {
            pop();
            return true;
        }
        return false;
    }
    case ActorActionKind::MoveToPoint:
        // GameSession::moveTo carries its stop distance in `range`; a script's move
        // to a location has none.
        if (walkAction(api, self, action, action.point,
                       action.userIssued ? std::max(action.range, kArrivalTolerance)
                                         : kArrivalTolerance,
                       dt)
            == Progress::Finished) {
            pop();
            return true;
        }
        return false;
    case ActorActionKind::JumpToObject: {
        const ActorAction taken = pop();
        if (api.world().get(taken.object) != nullptr) {
            jumpActor(api, self, api.position(taken.object), api.facing(taken.object) * kDegrees);
        }
        return true;
    }
    case ActorActionKind::JumpToPoint: {
        const ActorAction taken = pop();
        jumpActor(api, self, taken.point, taken.facingDegrees);
        return true;
    }
    case ActorActionKind::OpenDoor:
    case ActorActionKind::CloseDoor:
    case ActorActionKind::LockObject:
    case ActorActionKind::UnlockObject: {
        const std::size_t index = api.entityFor(action.object);
        if (index == static_cast<std::size_t>(-1)) {
            pop();
            return true;
        }
        Progress progress = Progress::Running;
        if (!reachObject(api, self, action, kInteractRange, dt, progress)) {
            return false;
        }
        const ActorAction taken = pop();
        if (taken.stuck >= kStuckGiveUp) {
            return true;
        }
        kotor::Entity& door = api.scene().entities[index];
        // Lock state does not refuse a scripted open: the retail handler was not
        // traced that far and shipped scripts unlock through SetLocked, which
        // belongs to the world-state family.
        if (taken.kind == ActorActionKind::OpenDoor && door.kind == kotor::EntityKind::Door) {
            // World-state family: a locked door opens only with its key, else OnFailToOpen.
            api.session.openDoor(index, self);
        } else if (taken.kind == ActorActionKind::OpenDoor) {
            door.open = true;
            api.playAnimation(index, "opening1", false);
            doorHook(api, index, "OnOpen", "lastOpenedBy", self);
        } else if (taken.kind == ActorActionKind::CloseDoor) {
            door.open = false;
            api.playAnimation(index, "closing1", false);
            doorHook(api, index, "OnClosed", "lastClosedBy", self);
        } else if (taken.kind == ActorActionKind::LockObject) {
            door.locked = true;
            doorHook(api, index, "OnLock", "lastLocked", self);
        } else {
            door.locked = false;
            doorHook(api, index, "OnUnlock", "lastUnlocked", self);
        }
        return true;
    }
    case ActorActionKind::StartConversation: {
        if (!action.started) {
            // Wait out any running conversation; retail's dialog action also
            // holds the queue until the conversation is over.
            if (api.dialogueActive()) {
                return false;
            }
            if (!action.ignoreRange && api.world().get(action.object) != nullptr) {
                Progress progress = Progress::Running;
                if (!reachObject(api, self, action, kConversationRange, dt, progress)) {
                    return false;
                }
            }
            action.started = true;
            const std::string resref = action.text;
            const std::uint32_t target = action.object;
            // Node scripts may touch this queue, so `action` is not used after.
            const std::uint32_t owner = self == api.pc() ? target : self;
            api.startConversation(owner, resref);
            return false;
        }
        if (api.dialogueActive()) {
            return false;
        }
        pop();
        return true;
    }
    case ActorActionKind::PauseConversation:
        pop();
        api.setDialoguePaused(true);
        return true;
    case ActorActionKind::ResumeConversation:
        pop();
        api.setDialoguePaused(false);
        return true;
    case ActorActionKind::PlayAnimation: {
        const std::size_t index = api.entityFor(self);
        if (!action.started) {
            action.started = true;
            const std::string name = api.animationName(action.animation);
            if (!name.empty() && index != static_cast<std::size_t>(-1)) {
                api.playAnimation(index, name, api.animationLooping(action.animation));
            }
            if (action.remaining <= 0.0f) {
                action.remaining = api.animationLooping(action.animation) ? 0.0f
                                                                          : kOneShotAnimation;
            }
        }
        action.remaining -= dt;
        if (action.remaining > 0.0f) {
            return false;
        }
        if (index != static_cast<std::size_t>(-1)
            && api.animationLooping(action.animation)) {
            api.stopAnimation(index);
        }
        pop();
        return true;
    }
    case ActorActionKind::RandomWalk: {
        // retail 0052d440 queues action 0x2d, which only ClearAllActions ends. The leg
        // length and the pause between legs are this engine's: see the constants.
        if (action.remaining > 0.0f) {
            action.remaining -= dt;
            return false;
        }
        if (!action.started) {
            action.started = true;
            const float angle = static_cast<float>(api.world().rng.randint(0, 359)) / kDegrees;
            const float reach = kRandomWalkRadius
                * static_cast<float>(api.world().rng.randint(20, 100)) / 100.0f;
            const kotor::Vec3 from = api.position(self);
            action.point = {from.x + std::cos(angle) * reach, from.y + std::sin(angle) * reach, from.z};
            action.stuck = 0.0f;
        }
        if (walkAction(api, self, action, action.point, kArrivalTolerance, dt) == Progress::Finished) {
            action.started = false;
            action.remaining = kRandomWalkPauseMax
                * static_cast<float>(api.world().rng.randint(0, 100)) / 100.0f;
        }
        return false;
    }
    case ActorActionKind::FollowObject: {
        // ActionForceFollowObject (0052c960) names the creature; ActionFollowLeader
        // (0052cbe0) leaves it invalid and tracks whoever leads. Neither finishes by
        // itself: the queue holds until the target goes or ClearAllActions runs.
        const std::uint32_t target = action.object != script::kObjectInvalid
            ? action.object : api.session.controlledObject();
        if (target == self) {
            return false;
        }
        if (api.world().get(target) == nullptr) {
            pop();
            return true;
        }
        const kotor::Vec3 at = api.position(self);
        const kotor::Vec3 there = api.position(target);
        const float gap = std::hypot(there.x - at.x, there.y - at.y);
        const float stop = std::max(action.range, api.approachRange(self, target, 0.0f));
        if (gap > stop + kArrivalTolerance) {
            api.step(self, there, api.moveSpeed(self, gap > stop * 2.0f), dt, stop);
        }
        return false;
    }
    case ActorActionKind::MoveAwayFromObject: {
        // retail 0053f990: walk straight off until fMoveAwayRange of clearance.
        if (api.world().get(action.object) == nullptr) {
            pop();
            return true;
        }
        const kotor::Vec3 at = api.position(self);
        const kotor::Vec3 threat = api.position(action.object);
        const float dx = at.x - threat.x;
        const float dy = at.y - threat.y;
        const float gap = std::hypot(dx, dy);
        if (gap >= action.range) {
            pop();
            return true;
        }
        if (!action.started) {
            action.started = true;
            const float nx = gap > 1.0e-3f ? dx / gap : std::cos(api.facing(self));
            const float ny = gap > 1.0e-3f ? dy / gap : std::sin(api.facing(self));
            action.point = {threat.x + nx * action.range, threat.y + ny * action.range, at.z};
        }
        if (walkAction(api, self, action, action.point, kArrivalTolerance, dt) == Progress::Finished) {
            pop();
            return true;
        }
        return false;
    }
    case ActorActionKind::BarkString: {
        const ActorAction taken = pop();
        PresentationState& shown = api.sim().presentation;
        shown.lastBarkStrref = taken.animation;
        shown.lastBarkSpeaker = self;
        api.log("bark strref " + std::to_string(taken.animation));
        return true;
    }
    case ActorActionKind::EquipMostDamaging: {
        const ActorAction taken = pop();
        equipMostDamaging(api.session, self, taken.result != 0, taken.force);
        return true;
    }
    case ActorActionKind::UseSkill: {
        // retail 0052d9b0 was not traced past its arguments. Security works a lock and
        // Demolitions a trap, the two skills shipped scripts name on objects.
        const std::size_t index = api.entityFor(action.object);
        if (index == static_cast<std::size_t>(-1)) {
            pop();
            return true;
        }
        Progress progress = Progress::Running;
        if (!reachObject(api, self, action, kInteractRange, dt, progress)) {
            return false;
        }
        const ActorAction taken = pop();
        if (taken.spell == 6) {
            api.session.unlockDoor(index, self);
        } else if (taken.spell == 1) {
            api.session.disarmTrap(index);
        } else {
            api.log("ActionUseSkill: skill " + std::to_string(taken.spell) + " has no object effect");
        }
        return true;
    }
    case ActorActionKind::CutsceneAttack:
        return runCutsceneAttackAction(api, self, dt);
    case ActorActionKind::Attack:
        return runAttackAction(api, self, dt);
    case ActorActionKind::CastSpell:
        return runCastAction(api, self, dt);
    }
    pop();
    return true;
}

void tickOneActor(ScriptApi api, std::uint32_t self, float dt)
{
    if (!api.isAlive(self)) {
        api.clearQueue(self);
        return;
    }
    if (isDebilitated(api, self)) {
        return; // A disabled creature's queue waits. retail 005b4880
    }
    // Only actions queued before this tick run in it, so a script that re-queues itself
    // from an instant action (seen on dan13_jedif2 in danm13) waits for the next tick
    // rather than rebuilding a VM many times per frame. An engine bound: no retail
    // limit was recovered.
    const std::size_t queued = api.actor(self).queue.size();
    const std::size_t limit = std::min<std::size_t>(queued, kInstantActionsPerTick);
    for (std::size_t ran = 0; ran < limit; ++ran) {
        if (api.actor(self).queue.empty() || !runFrontAction(api, self, dt)) {
            return;
        }
    }
    static int warnings = 0;
    if (queued > kInstantActionsPerTick && warnings < 40) {
        ++warnings;
        const script::GameObject* object = api.world().get(self);
        logWarn(concat("actor queue: ", object != nullptr ? object->tag : std::string("?"),
                       " has ", queued, " actions queued, over the ", kInstantActionsPerTick,
                       " instant actions run per tick"));
    }
}

// ---- handlers --------------------------------------------------------------

// ACT_6 AssignCommand 0052e720 posts a zero-delay event to the target carrying
// the script situation. It runs on a later tick with OBJECT_SELF = target, and
// any Action* inside it queue on the target. An invalid target discards it.
Cells assignCommand(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t target = call.objectArgOrInvalid(0);
    const std::int64_t functor = call.functorArg(1);
    if (functor < 0) {
        return ActionCall::none();
    }
    if (call.world.get(target) == nullptr) {
        api.dropFunctor(functor);
        return ActionCall::none();
    }
    if (const auto found = call.world.functor(functor)) {
        found->source = "AssignCommand";
    }
    call.world.scheduleFunctor(0.0, functor, target, "AssignCommand");
    return ActionCall::none();
}

// ACT_9 ClearAllActions 0052f4a0 -> 004ccd80: creature-kind objects only, and
// only while commandable.
Cells clearAllActions(ActionCall& call)
{
    ScriptApi api = call.api();
    const std::uint32_t self = call.self();
    if (api.isCreature(self) && api.actor(self).commandable) {
        api.clearQueue(self);
    }
    return ActionCall::none();
}

// ACT_294 ActionDoCommand 0052c740 -> 0057cb10: dropped when not commandable.
Cells actionDoCommand(ActionCall& call)
{
    const std::int64_t functor = call.functorArg(0);
    if (functor < 0) {
        return ActionCall::none();
    }
    if (const auto found = call.world.functor(functor)) {
        found->source = "ActionDoCommand";
    }
    ActorAction action;
    action.kind = ActorActionKind::DoCommand;
    action.functor = functor;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_202 ActionWait 00545800 reads its float and queues; it does not stop the
// VM, so the rest of the calling script runs immediately.
Cells actionWait(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::Wait;
    action.remaining = static_cast<float>(call.floatArg(0));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionMoveToObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::MoveToObject;
    action.object = call.objectArgOrInvalid(0);
    call.api().actor(call.self()).attemptedMoveTarget = action.object; // retail +0x508
    action.run = call.intArg(1) != 0;
    action.range = static_cast<float>(call.floatArg(2, 1.0));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionMoveToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(0));
    if (location == nullptr) {
        return ActionCall::none();
    }
    ActorAction action;
    action.kind = ActorActionKind::MoveToPoint;
    action.point = location->position;
    action.facingDegrees = location->facing;
    action.run = call.intArg(1) != 0;
    api.enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionForceMoveToObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::MoveToObject;
    action.object = call.objectArgOrInvalid(0);
    call.api().actor(call.self()).attemptedMoveTarget = action.object; // retail +0x508
    action.run = call.intArg(1) != 0;
    action.range = static_cast<float>(call.floatArg(2, 1.0));
    action.force = true;
    action.remaining = static_cast<float>(call.floatArg(3, 30.0));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionForceMoveToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(0));
    if (location == nullptr) {
        return ActionCall::none();
    }
    ActorAction action;
    action.kind = ActorActionKind::MoveToPoint;
    action.point = location->position;
    action.facingDegrees = location->facing;
    action.run = call.intArg(1) != 0;
    action.force = true;
    action.remaining = static_cast<float>(call.floatArg(2, 30.0));
    api.enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionJumpToObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::JumpToObject;
    action.object = call.objectArgOrInvalid(0);
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionJumpToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(0));
    if (location == nullptr) {
        return ActionCall::none();
    }
    ActorAction action;
    action.kind = ActorActionKind::JumpToPoint;
    action.point = location->position;
    action.facingDegrees = location->facing;
    api.enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// JumpTo* go in front of whatever is queued rather than behind it; the action
// names differ from ActionJumpTo* only in that respect. The retail handlers
// (ACT_313 0052cdc0) were not traced past argument reading: this ordering is the
// nwscript.nss description, so treat it as documented rather than proven.
Cells jumpToObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::JumpToObject;
    action.object = call.objectArgOrInvalid(0);
    call.api().enqueueFront(call.self(), std::move(action));
    return ActionCall::none();
}

Cells jumpToLocation(ActionCall& call)
{
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(0));
    if (location == nullptr) {
        return ActionCall::none();
    }
    ActorAction action;
    action.kind = ActorActionKind::JumpToPoint;
    action.point = location->position;
    action.facingDegrees = location->facing;
    api.enqueueFront(call.self(), std::move(action));
    return ActionCall::none();
}

template <ActorActionKind Kind>
Cells objectAction(ActionCall& call)
{
    ActorAction action;
    action.kind = Kind;
    action.object = call.objectArgOrInvalid(0);
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_162 SetCommandable 005426c0 writes +0xe8 on the target (default self).
Cells setCommandable(ActionCall& call)
{
    call.api().actor(call.objectArg(1)).commandable = call.intArg(0) != 0;
    return ActionCall::none();
}

Cells getCommandable(ActionCall& call)
{
    return ActionCall::retInt(call.api().actor(call.objectArg(0)).commandable ? 1 : 0);
}

Cells getCurrentAction(ActionCall& call)
{
    const ActorState& state = call.api().actor(call.objectArg(0));
    if (state.queue.empty()) {
        return ActionCall::retInt(kActionQueueEmpty);
    }
    switch (state.queue.front().kind) {
    case ActorActionKind::MoveToObject:
    case ActorActionKind::MoveToPoint:
        return ActionCall::retInt(kActionMoveToPoint);
    case ActorActionKind::OpenDoor:
        return ActionCall::retInt(kActionOpenDoor);
    case ActorActionKind::CloseDoor:
        return ActionCall::retInt(kActionCloseDoor);
    case ActorActionKind::StartConversation:
        return ActionCall::retInt(kActionDialogObject);
    case ActorActionKind::UnlockObject:
        return ActionCall::retInt(kActionOpenLock);
    case ActorActionKind::LockObject:
        return ActionCall::retInt(kActionLock);
    case ActorActionKind::Wait:
        return ActionCall::retInt(kActionWait);
    case ActorActionKind::DoCommand:
        return ActionCall::retInt(kActionDoCommand);
    case ActorActionKind::FollowObject:
        return ActionCall::retInt(kActionFollow);
    case ActorActionKind::RandomWalk:
    case ActorActionKind::MoveAwayFromObject:
        return ActionCall::retInt(kActionMoveToPoint);
    case ActorActionKind::CutsceneAttack:
    case ActorActionKind::Attack:
        return ActionCall::retInt(3); // ACTION_ATTACKOBJECT
    case ActorActionKind::CastSpell:
        return ActionCall::retInt(4); // ACTION_CASTSPELL
    default:
        return ActionCall::retInt(kActionInvalid);
    }
}

Cells getUserActionsPending(ActionCall& call)
{
    for (const ActorAction& action : call.api().actor(call.self()).queue) {
        if (action.userIssued) {
            return ActionCall::retInt(1);
        }
    }
    return ActionCall::retInt(0);
}

// ActionStartConversation(oObjectToConverse, sDialogResRef, bPrivate, nType,
// bIgnoreStartRange, 6 strings, bUseLeader). A blank resref means the owner's
// own conversation.
Cells actionStartConversation(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::StartConversation;
    action.object = call.objectArgOrInvalid(0);
    action.text = call.stringArg(1);
    action.ignoreRange = call.intArg(4) != 0;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_20 ActionRandomWalk 0052d440.
Cells actionRandomWalk(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::RandomWalk;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_23 ActionMoveAwayFromObject(oFleeFrom, bRun, fMoveAwayRange = 40) 0053f990.
Cells actionMoveAwayFromObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::MoveAwayFromObject;
    action.object = call.objectArgOrInvalid(0);
    action.run = call.intArg(1) != 0;
    action.range = static_cast<float>(call.floatArg(2, 40.0));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_167 ActionForceFollowObject(oFollow, fFollowDistance = 0) 0052c960.
Cells actionForceFollowObject(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::FollowObject;
    action.object = call.objectArgOrInvalid(0);
    if (action.object == script::kObjectInvalid) {
        return ActionCall::none();
    }
    action.range = static_cast<float>(call.floatArg(1, 0.0));
    action.force = true;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_730 ActionFollowLeader 0052cbe0, action 0x3d.
Cells actionFollowLeader(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::FollowObject;
    action.range = kDefaultFollowDistance;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_700 ActionBarkString(strRef) 0052c6a0: queued, so it waits its turn.
Cells actionBarkString(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::BarkString;
    action.animation = static_cast<std::int32_t>(call.intArg(0));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_399 / ACT_400 ActionEquipMostDamaging* 0052c7b0, 0052c820. oVersus is read and
// not used here: the weighing against a target was not traced.
template <bool Ranged>
Cells actionEquipMostDamaging(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::EquipMostDamaging;
    action.result = Ranged ? 1 : 0;
    action.force = !Ranged && call.intArg(1) != 0; // bOffHand
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

// ACT_288 ActionUseSkill(nSkill, oTarget, nSubSkill, oItemUsed) 0052d9b0.
Cells actionUseSkill(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::UseSkill;
    action.spell = static_cast<std::int32_t>(call.intArg(0));
    action.object = call.objectArgOrInvalid(1);
    action.amount = static_cast<std::int32_t>(call.intArg(2));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

template <ActorActionKind Kind>
Cells plainAction(ActionCall& call)
{
    ActorAction action;
    action.kind = Kind;
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

Cells actionPlayAnimation(ActionCall& call)
{
    ActorAction action;
    action.kind = ActorActionKind::PlayAnimation;
    action.animation = static_cast<std::int32_t>(call.intArg(0));
    action.remaining = static_cast<float>(call.floatArg(2));
    call.api().enqueue(call.self(), std::move(action));
    return ActionCall::none();
}

} // namespace

void tickActors(ScriptApi api, float dt)
{
    std::vector<std::uint32_t> busy;
    bool anyPending = !api.world().timers.empty();
    for (const auto& [id, state] : api.sim().actors) {
        if (!state.queue.empty()) {
            busy.push_back(id);
        }
    }
    for (const std::uint32_t id : busy) {
        tickOneActor(api, id, dt);
    }
    for (const std::uint32_t id : busy) {
        anyPending = anyPending || !api.actor(id).queue.empty();
    }
    // A pause nothing will ever resume would freeze the conversation, so release
    // it once the world has nothing left in flight.
    if (api.sim().dialoguePaused && !anyPending) {
        api.setDialoguePaused(false);
    }
}

void registerActorActions(HandlerTable& table)
{
    table.add(6, assignCommand);
    table.add(20, actionRandomWalk);
    table.add(23, actionMoveAwayFromObject);
    table.add(167, actionForceFollowObject);
    table.add(288, actionUseSkill);
    table.add(399, actionEquipMostDamaging<false>);
    table.add(400, actionEquipMostDamaging<true>);
    table.add(700, actionBarkString);
    table.add(730, actionFollowLeader);
    table.add(9, clearAllActions);
    table.add(21, actionMoveToLocation);
    table.add(22, actionMoveToObject);
    table.add(40, actionPlayAnimation);
    table.add(43, objectAction<ActorActionKind::OpenDoor>);
    table.add(44, objectAction<ActorActionKind::CloseDoor>);
    table.add(162, setCommandable);
    table.add(163, getCommandable);
    table.add(196, actionJumpToObject);
    table.add(202, actionWait);
    table.add(204, actionStartConversation);
    table.add(205, plainAction<ActorActionKind::PauseConversation>);
    table.add(206, plainAction<ActorActionKind::ResumeConversation>);
    table.add(214, actionJumpToLocation);
    table.add(294, actionDoCommand);
    table.add(313, jumpToLocation);
    table.add(382, actionForceMoveToLocation);
    table.add(383, actionForceMoveToObject);
    table.add(385, jumpToObject);
    table.add(483, objectAction<ActorActionKind::UnlockObject>);
    table.add(484, objectAction<ActorActionKind::LockObject>);
    table.add(514, getUserActionsPending);
    table.add(522, getCurrentAction);
}

} // namespace ke
