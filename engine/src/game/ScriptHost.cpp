#include "game/ScriptHost.h"

#include "game/GameSession.h"

#include <algorithm>
#include <cmath>

namespace ke {

std::int64_t EngineStore::add(EngineRecord record)
{
    const std::int64_t id = next_++;
    records_.emplace(id, std::move(record));
    return id;
}

void EngineStore::restore(std::int64_t id, EngineRecord record)
{
    records_[id] = std::move(record);
    next_ = std::max(next_, id + 1);
}

void EngineStore::clear()
{
    records_.clear();
    next_ = 0x10000;
}

WorldSim::WorldSim()
{
    registerObjectActions(handlers);
    registerEffectActions(handlers);
    registerActorActions(handlers);
    registerItemActions(handlers);
    registerPartyActions(handlers);
    registerCombatActions(handlers);
    registerWorldStateActions(handlers);
    registerEventActions(handlers);
    registerVideoEffectActions(handlers);
    registerMiniGameActions(handlers);
}

// ---- ActionCall ------------------------------------------------------------

ScriptApi ActionCall::api() const
{
    return ScriptApi{session};
}

std::uint32_t ActionCall::self() const
{
    return api().self();
}

bool ActionCall::hasArg(std::size_t index) const
{
    return index < args.size() && !args[index].empty();
}

std::int64_t ActionCall::intArg(std::size_t index, std::int64_t fallback) const
{
    return hasArg(index) ? args[index].front().asInt() : fallback;
}

double ActionCall::floatArg(std::size_t index, double fallback) const
{
    return hasArg(index) ? args[index].front().asFloat() : fallback;
}

std::string ActionCall::stringArg(std::size_t index) const
{
    return hasArg(index) ? args[index].front().asString() : std::string{};
}

std::uint32_t ActionCall::objectArg(std::size_t index) const
{
    if (!hasArg(index) || args[index].front().type != script::ValueType::Object) {
        return self();
    }
    const std::uint32_t object = static_cast<std::uint32_t>(args[index].front().integer);
    return object == 0 ? self() : object;
}

std::uint32_t ActionCall::objectArgOrInvalid(std::size_t index) const
{
    return hasArg(index) ? objectArg(index) : script::kObjectInvalid;
}

kotor::Vec3 ActionCall::vectorArg(std::size_t index) const
{
    if (index >= args.size() || args[index].size() < 3) {
        return {};
    }
    return {static_cast<float>(args[index][0].asFloat()),
            static_cast<float>(args[index][1].asFloat()),
            static_cast<float>(args[index][2].asFloat())};
}

std::int64_t ActionCall::engineArg(std::size_t index) const
{
    return hasArg(index) ? args[index].front().integer : 0;
}

std::int64_t ActionCall::functorArg(std::size_t index) const
{
    if (!hasArg(index) || args[index].front().type != script::ValueType::Functor) {
        return -1;
    }
    return args[index].front().integer;
}

// ---- ScriptApi -------------------------------------------------------------

WorldSim& ScriptApi::sim() const
{
    return *session.sim_;
}

script::World& ScriptApi::world() const
{
    return session.scriptWorld_;
}

kotor::Scene& ScriptApi::scene() const
{
    return session.scene_;
}

std::uint32_t ScriptApi::pc() const
{
    return session.scriptWorld_.pc;
}

std::uint32_t ScriptApi::contextObject(const std::string& name) const
{
    return session.runtime_ != nullptr ? session.runtime_->contextObject(name)
                                       : script::kObjectInvalid;
}

const ScriptContext& ScriptApi::context() const
{
    return session.runtime_->contextSnapshot();
}

std::size_t ScriptApi::entityFor(std::uint32_t object) const
{
    const std::vector<std::uint32_t>& objects = session.entityObjects_;
    const auto it = std::find(objects.begin(), objects.end(), object);
    if (it == objects.end() || object == 0) {
        return static_cast<std::size_t>(-1);
    }
    const std::size_t index = static_cast<std::size_t>(it - objects.begin());
    return index < session.scene_.entities.size() ? index : static_cast<std::size_t>(-1);
}

std::uint32_t ScriptApi::objectFor(std::size_t entity) const
{
    return session.objectForEntity(entity);
}

bool ScriptApi::isCreature(std::uint32_t object) const
{
    if (object == pc()) {
        return true;
    }
    const std::size_t index = entityFor(object);
    return index != static_cast<std::size_t>(-1)
        && session.scene_.entities[index].kind == kotor::EntityKind::Creature;
}

bool ScriptApi::isAlive(std::uint32_t object) const
{
    if (object == pc()) {
        return !session.playerDead_;
    }
    const std::size_t index = entityFor(object);
    if (index != static_cast<std::size_t>(-1)) {
        return !session.scene_.entities[index].dead;
    }
    return session.scriptWorld_.get(object) != nullptr;
}

kotor::Vec3 ScriptApi::position(std::uint32_t object) const
{
    if (object == pc()) {
        return session.position_;
    }
    const std::size_t index = entityFor(object);
    if (index != static_cast<std::size_t>(-1)) {
        return session.scene_.entities[index].position;
    }
    if (const script::GameObject* found = session.scriptWorld_.get(object); found != nullptr) {
        return {static_cast<float>(found->position[0]), static_cast<float>(found->position[1]),
                static_cast<float>(found->position[2])};
    }
    return {};
}

float ScriptApi::facing(std::uint32_t object) const
{
    if (object == pc()) {
        return session.thirdPerson_ || session.companionControlled() ? session.playerBearing_
                                                                      : session.yaw_;
    }
    const std::size_t index = entityFor(object);
    return index != static_cast<std::size_t>(-1) ? session.scene_.entities[index].bearing : 0.0f;
}

void ScriptApi::setPosition(std::uint32_t object, kotor::Vec3 position) const
{
    if (object == pc()) {
        session.position_ = position;
        session.syncPlayerToScriptWorld();
        return;
    }
    const std::size_t index = entityFor(object);
    if (index != static_cast<std::size_t>(-1)) {
        session.scene_.entities[index].position = position;
        session.syncEntityToScriptWorld(index);
        return;
    }
    if (script::GameObject* found = session.scriptWorld_.get(object); found != nullptr) {
        found->position = {position.x, position.y, position.z};
    }
}

void ScriptApi::setPosition(std::uint32_t object, kotor::Vec3 position, float facing) const
{
    if (object == pc()) {
        session.playerBearing_ = facing;
        // The camera heading follows the PC only while the PC is the steered body.
        if (!session.companionControlled()) {
            session.yaw_ = facing;
        }
    } else if (const std::size_t index = entityFor(object);
               index != static_cast<std::size_t>(-1)) {
        session.scene_.entities[index].bearing = facing;
    }
    setPosition(object, position);
}

ActorState& ScriptApi::actor(std::uint32_t object) const
{
    return session.sim_->actors[object];
}

bool ScriptApi::enqueue(std::uint32_t object, ActorAction action) const
{
    ActorState& state = actor(object);
    if (!state.commandable) {
        if (action.functor >= 0) {
            dropFunctor(action.functor);
        }
        return false;
    }
    state.queue.push_back(std::move(action));
    return true;
}

bool ScriptApi::enqueueFront(std::uint32_t object, ActorAction action) const
{
    ActorState& state = actor(object);
    if (!state.commandable) {
        return false;
    }
    state.queue.push_front(std::move(action));
    return true;
}

void ScriptApi::clearQueue(std::uint32_t object) const
{
    ActorState& state = actor(object);
    for (const ActorAction& action : state.queue) {
        if (action.functor >= 0) {
            dropFunctor(action.functor);
        }
    }
    state.queue.clear();
}

ActorStep ScriptApi::step(std::uint32_t object, kotor::Vec3 target, float speed, float dt,
                          float stopDistance) const
{
    return session.stepActorToward(object, target, speed, dt, stopDistance);
}

float ScriptApi::moveSpeed(std::uint32_t object, bool run) const
{
    return session.creatureMoveRate(object, run);
}

float ScriptApi::approachRange(std::uint32_t self, std::uint32_t target, float range) const
{
    return session.creatureApproachRange(self, target, range);
}

bool ScriptApi::hasLineOfSight(std::uint32_t a, std::uint32_t b) const
{
    return session.lineOfSight(position(a), position(b));
}

void ScriptApi::raiseHook(std::uint32_t target, std::string hook, ScriptContext context) const
{
    session.sim_->events.push_back({target, std::move(hook), std::move(context)});
}

namespace {

// Logical hook name to the labels used by each kind of GFF template.
const std::vector<std::string>& labelsFor(const std::string& hook)
{
    static const std::map<std::string, std::vector<std::string>> labels = {
        {"UserDefined", {"ScriptUserDefine", "OnUserDefined"}},
        {"Damaged", {"ScriptDamaged", "OnDamaged"}},
        {"Attacked", {"ScriptAttacked", "OnMeleeAttacked"}},
        {"SpellAt", {"ScriptSpellAt", "OnSpellCastAt"}},
        {"Disturbed", {"ScriptDisturbed", "OnInvDisturbed"}},
        {"Death", {"ScriptDeath", "OnDeath"}},
        {"Heartbeat", {"ScriptHeartbeat", "OnHeartbeat"}},
        {"EndRound", {"ScriptEndRound"}},
        {"Notice", {"ScriptOnNotice"}},
        {"Dialogue", {"ScriptDialogue"}},
        {"Blocked", {"ScriptOnBlocked"}},
    };
    const auto it = labels.find(hook);
    if (it != labels.end()) {
        return it->second;
    }
    static thread_local std::vector<std::string> literal(1);
    literal[0] = hook;
    return literal;
}

} // namespace

bool ScriptApi::dispatchHook(std::uint32_t target, const std::string& hook,
                             const ScriptContext& context) const
{
    if (target == script::kObjectInvalid || target == 0) {
        return false;
    }
    WorldSim& state = *session.sim_;
    if (target == state.moduleObject || target == state.areaObject) {
        const std::string label = hook == "UserDefined"
            ? (target == state.moduleObject ? "Mod_OnUsrDefined" : "OnUserDefined")
            : hook;
        return session.runSceneHook(label, session.controlledObject(), &context);
    }
    const std::size_t index = entityFor(target);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    for (const std::string& label : labelsFor(hook)) {
        const auto& scripts = session.scene_.entities[index].scripts;
        const auto it = scripts.find(label);
        if (it != scripts.end() && !it->second.empty()) {
            session.runHook(label, index, session.controlledObject(), &context);
            return true;
        }
    }
    return false;
}

std::optional<script::Value> ScriptApi::runScript(const std::string& resref, std::uint32_t self,
                                                  const ScriptContext& context) const
{
    return session.runScriptWith(resref, self, context);
}

bool ScriptApi::runFunctor(std::int64_t functor, std::uint32_t self) const
{
    const std::shared_ptr<script::Functor> found = session.scriptWorld_.functor(functor);
    if (found == nullptr) {
        return false;
    }
    const bool ok = session.runFunctor(*found, self);
    session.scriptWorld_.functors.erase(functor);
    return ok;
}

void ScriptApi::dropFunctor(std::int64_t functor) const
{
    session.scriptWorld_.functors.erase(functor);
}

bool ScriptApi::startConversation(std::uint32_t owner, const std::string& resref) const
{
    return session.startConversation(owner, resref);
}

bool ScriptApi::dialogueActive() const
{
    return session.dialogueView_.active;
}

void ScriptApi::setDialoguePaused(bool paused) const
{
    session.sim_->dialoguePaused = paused;
}

bool ScriptApi::playAnimation(std::size_t entity, const std::string& name, bool loop) const
{
    return session.playAnimation(entity, name, loop);
}

void ScriptApi::stopAnimation(std::size_t entity) const
{
    session.stopAnimation(entity);
}

std::string ScriptApi::animationName(std::int32_t animation) const
{
    return session.rules_.animationName(animation);
}

bool ScriptApi::animationLooping(std::int32_t animation) const
{
    return session.rules_.animationLooping(animation);
}

std::uint32_t ScriptApi::spawnCreature(const std::string& resref, kotor::Vec3 position,
                                       float facing) const
{
    return session.spawnScriptCreature(resref, position, facing);
}

bool ScriptApi::playerStealthed() const
{
    return session.stealthActive_;
}

std::array<float, 6>* ScriptApi::globalLocation(const std::string& name) const
{
    const auto it = session.locationGlobals_.find(name);
    return it == session.locationGlobals_.end() ? nullptr : &it->second;
}

void ScriptApi::setGlobalLocation(const std::string& name, const std::array<float, 6>& value) const
{
    session.locationGlobals_[name] = value;
    session.globalKinds_[name] = "location";
}

void ScriptApi::log(std::string line) const
{
    session.scriptLog_.push_back(std::move(line));
}

kotor::Install& ScriptApi::install() const
{
    return session.install_;
}

} // namespace ke
