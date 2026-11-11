#include "script/World.h"

#include <algorithm>
#include <cmath>

namespace ke::script {

namespace {

Value defaultValue(ValueType type)
{
    switch (type) {
    case ValueType::Float: return Value::makeFloat(0.0);
    case ValueType::String: return Value::makeString("");
    case ValueType::Object: return Value::makeObject(kObjectInvalid);
    case ValueType::Engine: return Value::makeEngine(0);
    default: return Value::makeInt(0);
    }
}

void setDefaultAttrs(std::map<std::string, AttrValue>& attrs)
{
    attrs.clear();
    attrs["module"] = std::string("end_m01aa");
    attrs["difficulty"] = std::int64_t{1};
    attrs["movie_playing"] = false;
    attrs["planets"] = std::map<std::string, std::int64_t>{{"Taris", 1}, {"Dantooine", 0}};
    attrs["encounter_spawns_max"] = std::int64_t{10};
    attrs["encounter_spawns_current"] = std::int64_t{0};
    attrs["encounter_difficulty"] = std::int64_t{1};
    attrs["stealth_xp"] = std::int64_t{0};
    attrs["stealth_enabled"] = std::int64_t{1};
    attrs["stealth_decrement"] = std::int64_t{5};
    attrs["max_stealth_xp"] = std::int64_t{100};
    attrs["journal_xp"] = std::map<std::string, std::int64_t>{};
    attrs["clicking_object"] = std::int64_t{0};
    attrs["last_speaker"] = std::int64_t{0};
    attrs["last_attacker"] = std::int64_t{0};
    attrs["transition_target"] = std::int64_t{0};
    attrs["starting_location"] = std::int64_t{0};
    attrs["last_item_equipped"] = std::int64_t{0};
    attrs["spell_cast_item"] = std::int64_t{0};
    attrs["item_activated_target"] = std::int64_t{0};
    attrs["item_activated_location"] = std::int64_t{0};
    attrs["last_conversation"] = std::string{};
    attrs["matched_substring"] = std::string{};
    attrs["matched_count"] = std::int64_t{0};
    attrs["last_player_dying"] = std::int64_t{0};
    attrs["last_respawn_button_presser"] = std::int64_t{0};
    attrs["last_trap_detected"] = std::int64_t{0};
    attrs["module_item_acquired_from"] = std::int64_t{0};
    attrs["module_item_lost"] = std::int64_t{0};
    attrs["module_item_lost_by"] = std::int64_t{0};
    attrs["last_perception_heard"] = std::int64_t{0};
    attrs["last_perception_inaudible"] = std::int64_t{0};
    attrs["last_closed_by"] = std::int64_t{0};
    attrs["aoe_creator"] = std::int64_t{0};
    attrs["last_associate_command"] = std::int64_t{0};
}

} // namespace

World::World(std::uint64_t seed)
{
    reset(seed);
}

void World::reset(std::uint64_t seed)
{
    nextId = 1;
    objects.clear();
    tags.clear();
    globals.clear();
    party.clear();
    actionQueue.clear();
    timers.clear();
    functors.clear();
    nextFunctor = 1;
    journal.clear();
    log.clear();
    events.clear();
    attrs.clear();
    time = 0.0;
    rng = PythonRandom(seed);
    cursors_.clear();
    setDefaultAttrs(attrs);
    GameObject& player = addObject("PC", 100);
    pc = player.id;
    party.push_back(pc);
}

void World::clearObjects()
{
    objects.clear();
    tags.clear();
    cursors_.clear();
    actionQueue.clear();
    timers.clear();
    functors.clear();
    events.clear();
    // Keep the PC as object id 1 and the persistent party/globals/journal.
    GameObject& player = objects[1];
    player.id = 1;
    player.tag = "PC";
    player.area = 100;
    player.valid = true;
    tags["PC"].push_back(1);
    pc = 1;
    std::erase(party, static_cast<std::uint32_t>(1));
    party.insert(party.begin(), 1);
}

GameObject& World::addObject(const std::string& tag, std::int64_t area,
                             std::int64_t baseItem, bool valid)
{
    const std::uint32_t id = nextId++;
    GameObject& object = objects[id];
    object.id = id;
    object.tag = tag;
    object.area = area;
    object.baseItem = baseItem;
    object.valid = valid;
    tags[tag].push_back(id);
    return object;
}

GameObject& World::restoreObject(GameObject object)
{
    const std::uint32_t id = object.id;
    nextId = std::max(nextId, id + 1);
    const auto existing = objects.find(id);
    if (existing != objects.end()) {
        std::erase(tags[existing->second.tag], id);
    }
    std::vector<std::uint32_t>& sameTag = tags[object.tag];
    if (std::find(sameTag.begin(), sameTag.end(), id) == sameTag.end()) {
        sameTag.push_back(id);
    }
    GameObject& stored = objects[id];
    stored = std::move(object);
    return stored;
}

GameObject* World::get(std::uint32_t id)
{
    const auto it = objects.find(id);
    return it == objects.end() ? nullptr : &it->second;
}

const GameObject* World::get(std::uint32_t id) const
{
    const auto it = objects.find(id);
    return it == objects.end() ? nullptr : &it->second;
}

bool World::valid(std::uint32_t id) const
{
    const GameObject* object = get(id);
    return object != nullptr && object->valid;
}

std::uint32_t World::byTag(const std::string& tag, std::int64_t nth) const
{
    if (nth < 0) {
        return kObjectInvalid;
    }
    // ACT_200 lowers the tag it is given (005e6080) before its table search, so any case finds
    // it: k_ptar_attack asks for "Droid1", whose template's tag is "droid1".
    const auto lowered = [](std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        return text;
    };
    const std::string wanted = lowered(tag);
    std::vector<std::uint32_t> found;
    std::size_t keys = 0;
    for (const auto& [key, ids] : tags) {
        if (key.size() == wanted.size() && lowered(key) == wanted) {
            found.insert(found.end(), ids.begin(), ids.end());
            ++keys;
        }
    }
    if (keys > 1) {
        std::sort(found.begin(), found.end());
    }
    return nth < static_cast<std::int64_t>(found.size()) ? found[static_cast<std::size_t>(nth)]
                                                         : kObjectInvalid;
}

std::vector<std::uint32_t> World::byArea(std::int64_t area) const
{
    std::vector<std::uint32_t> out;
    for (const auto& [id, object] : objects) {
        if (object.area == area && object.valid) {
            out.push_back(id);
        }
    }
    return out;
}

Value World::getLocal(std::uint32_t id, const std::string& name, ValueType type) const
{
    const GameObject* object = get(id);
    if (object == nullptr) {
        return defaultValue(type);
    }
    const auto it = object->locals.find(name);
    if (it == object->locals.end() || it->second.type != type) {
        return defaultValue(type);
    }
    return it->second;
}

void World::setLocal(std::uint32_t id, const std::string& name, Value value)
{
    GameObject* object = get(id);
    if (object != nullptr) {
        object->locals[name] = std::move(value);
    }
}

std::int32_t World::localBoolean(std::uint32_t id, std::int64_t index) const
{
    if (index < 0 || index > 95 || getLocal(id, localBooleanKey(index), ValueType::Int).asInt() == 0) {
        return 0;
    }
    return static_cast<std::int32_t>(std::uint32_t{1} << static_cast<unsigned>(index % 32));
}

void World::setLocalBoolean(std::uint32_t id, std::int64_t index, bool value)
{
    if (index < 0 || index > 95) {
        return;
    }
    setLocal(id, localBooleanKey(index), Value::makeInt(value ? 1 : 0));
}

std::int32_t World::localNumber(std::uint32_t id, std::int64_t index) const
{
    if (index < 0 || index > 7) {
        return 0;
    }
    return static_cast<std::int32_t>(getLocal(id, std::to_string(index), ValueType::Int).asInt() & 0xFF);
}

void World::setLocalNumber(std::uint32_t id, std::int64_t index, std::int64_t value)
{
    if (value < -128 || value > 127 || index < 0 || index > 7) {
        return;
    }
    setLocal(id, std::to_string(index), Value::makeInt(value & 0xFF));
}

Value World::getGlobal(const std::string& name, ValueType type) const
{
    const auto it = globals.find(name);
    if (it == globals.end() || it->second.type != type) {
        return defaultValue(type);
    }
    return it->second;
}

void World::setGlobal(const std::string& name, Value value)
{
    globals[name] = std::move(value);
}

std::int64_t World::cursor(const std::string& key) const
{
    const auto it = cursors_.find(key);
    return it == cursors_.end() ? 0 : it->second;
}

void World::setCursor(const std::string& key, std::int64_t value)
{
    cursors_[key] = value;
}

void World::queueAction(std::int64_t actionId, std::vector<std::vector<Value>> args,
                        const std::string& source)
{
    QueuedAction action;
    action.actionId = actionId;
    action.args = std::move(args);
    action.at = time;
    action.source = source;
    actionQueue.push_back(std::move(action));
}

// Retail DelayCommand (ACT_7 0052fe30) and AssignCommand (ACT_6 0052e720) post an event
// through CServerAIMaster::AddEventDeltaTime 004b08d0: the delay becomes whole milliseconds
// (float seconds times 1000.0f at 0073d6fc, truncated by _ftol 006fae8c) added to the world
// time, which is itself whole milliseconds (CWorldTimer::GetWorldTime 004ade40). A delay
// whose milliseconds read as unsigned are not below one day fails AddWorldTimes 004adf50
// and the event is freed, so a negative delay never runs. NaN converts to 0 ms.
std::optional<double> World::dueAfter(double delay) const
{
    const float scaled = static_cast<float>(delay) * 1000.0f;
    std::int64_t wide = 0;
    if (std::isfinite(scaled) && std::fabs(scaled) < 9.2e18f) {
        wide = static_cast<std::int64_t>(scaled);
    }
    const std::int32_t ms = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(wide)));
    if (ms < 0) {
        return std::nullopt;
    }
    return static_cast<double>(nowMs() + ms) / 1000.0;
}

std::int64_t World::nowMs() const
{
    return static_cast<std::int64_t>(std::floor(time * 1000.0 + 1e-6));
}

void World::schedule(double delay, std::int64_t actionId,
                     std::vector<std::vector<Value>> args, const std::string& source)
{
    const std::optional<double> due = dueAfter(delay);
    if (!due.has_value()) {
        return;
    }
    Timer timer;
    timer.due = *due;
    timer.actionId = actionId;
    timer.args = std::move(args);
    timer.source = source;
    timers.push_back(std::move(timer));
}

std::int64_t World::addFunctor(Functor functor)
{
    const std::int64_t id = nextFunctor++;
    functor.id = id;
    functors[id] = std::make_shared<Functor>(std::move(functor));
    return id;
}

std::shared_ptr<Functor> World::functor(std::int64_t id) const
{
    const auto it = functors.find(id);
    return it == functors.end() ? nullptr : it->second;
}

void World::queueFunctor(std::int64_t id, std::uint32_t target, const std::string& source)
{
    QueuedAction action;
    action.functor = id;
    action.target = target;
    action.at = time;
    action.source = source;
    actionQueue.push_back(std::move(action));
}

void World::scheduleFunctor(double delay, std::int64_t id, std::uint32_t target,
                            const std::string& source)
{
    const std::optional<double> due = dueAfter(delay);
    if (!due.has_value()) {
        functors.erase(id);
        return;
    }
    Timer timer;
    timer.due = *due;
    timer.functor = id;
    timer.target = target;
    timer.source = source;
    timers.push_back(std::move(timer));
}

bool World::timerDue(const Timer& timer) const
{
    return static_cast<std::int64_t>(std::llround(timer.due * 1000.0)) <= nowMs();
}

// CServerAIMaster::AddEventAbsoluteTime 004afdb0 keeps the queue sorted by time and puts a new
// event after every event of the same time; UpdateState 004b0b70 reads the world time once
// and runs the head while it is due, so events come out in time order, first posted first
// among equal times, and an event posted with no delay while the queue runs (an
// AssignCommand inside a delayed command) runs in the same pass.
std::vector<Timer> World::advance(double seconds)
{
    advanceClock(seconds);
    std::vector<Timer> due;
    while (std::optional<Timer> timer = popDue()) {
        due.push_back(std::move(*timer));
    }
    return due;
}

void World::advanceClock(double seconds)
{
    time += seconds;
}

std::optional<Timer> World::popDue()
{
    auto earliest = timers.end();
    for (auto it = timers.begin(); it != timers.end(); ++it) {
        if (timerDue(*it) && (earliest == timers.end() || it->due < earliest->due)) {
            earliest = it;
        }
    }
    if (earliest == timers.end()) {
        return std::nullopt;
    }
    Timer timer = std::move(*earliest);
    timers.erase(earliest);
    return timer;
}

std::int64_t World::attrInt(const std::string& name, std::int64_t fallback) const
{
    const auto it = attrs.find(name);
    if (it == attrs.end()) {
        return fallback;
    }
    if (const auto* value = std::get_if<std::int64_t>(&it->second)) {
        return *value;
    }
    if (const auto* value = std::get_if<bool>(&it->second)) {
        return *value ? 1 : 0;
    }
    if (const auto* value = std::get_if<double>(&it->second)) {
        return static_cast<std::int64_t>(*value);
    }
    return fallback;
}

double World::attrFloat(const std::string& name, double fallback) const
{
    const auto it = attrs.find(name);
    if (it == attrs.end()) {
        return fallback;
    }
    if (const auto* value = std::get_if<double>(&it->second)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int64_t>(&it->second)) {
        return static_cast<double>(*value);
    }
    if (const auto* value = std::get_if<bool>(&it->second)) {
        return *value ? 1.0 : 0.0;
    }
    return fallback;
}

std::string World::attrString(const std::string& name, const std::string& fallback) const
{
    const auto it = attrs.find(name);
    if (it == attrs.end()) {
        return fallback;
    }
    if (const auto* value = std::get_if<std::string>(&it->second)) {
        return *value;
    }
    return fallback;
}

void World::setAttr(const std::string& name, AttrValue value)
{
    attrs[name] = std::move(value);
}

bool World::hasAttr(const std::string& name) const
{
    return attrs.contains(name);
}

std::map<std::string, double> World::summary() const
{
    std::int64_t validObjects = 0;
    for (const auto& [id, object] : objects) {
        if (object.valid) {
            ++validObjects;
        }
    }
    return {
        {"objects", static_cast<double>(objects.size())},
        {"valid_objects", static_cast<double>(validObjects)},
        {"globals", static_cast<double>(globals.size())},
        {"party", static_cast<double>(party.size())},
        {"queued_actions", static_cast<double>(actionQueue.size())},
        {"timers", static_cast<double>(timers.size())},
        {"time", time},
    };
}

} // namespace ke::script
