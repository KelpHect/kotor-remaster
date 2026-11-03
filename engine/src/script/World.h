#pragma once

#include "script/Random.h"
#include "script/ScriptTypes.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace ke::script {

using AttrValue = std::variant<std::int64_t, double, std::string, bool,
                               std::map<std::string, std::int64_t>>;

struct GameObject {
    std::uint32_t id = 0;
    std::string tag;
    std::int64_t area = 100;
    std::map<std::string, Value> locals;
    std::vector<std::uint32_t> inventory;
    std::int64_t baseItem = 0;
    bool valid = true;
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::int64_t hp = 10;
    std::map<std::string, AttrValue> attrs;
};

struct Timer {
    double due = 0.0;
    std::int64_t actionId = -1;
    std::vector<std::vector<Value>> args;
    std::string source = "DelayCommand";
};

struct QueuedAction {
    std::int64_t actionId = -1;
    std::vector<std::vector<Value>> args;
    double at = 0.0;
    std::string source = "Action";
};

struct WorldEvent {
    double time = 0.0;
    std::int64_t id = 0;
    std::string name;
    std::string kind;
};

// Deterministic object/world model used by the script runtime. It mirrors the
// verified Sprint 3 Python model; the engine binds its real entities to these
// objects instead of inventing a second object system.
class World {
public:
    explicit World(std::uint64_t seed = 0x4B4F544F52ULL);

    void reset(std::uint64_t seed = 0x4B4F544F52ULL);
    void clearObjects();

    GameObject& addObject(const std::string& tag, std::int64_t area = 100,
                          std::int64_t baseItem = 0, bool valid = true);
    GameObject* get(std::uint32_t id);
    const GameObject* get(std::uint32_t id) const;
    bool valid(std::uint32_t id) const;
    std::uint32_t byTag(const std::string& tag, std::int64_t nth = 0) const;
    std::vector<std::uint32_t> byArea(std::int64_t area) const;

    Value getLocal(std::uint32_t id, const std::string& name, ValueType type = ValueType::Int) const;
    void setLocal(std::uint32_t id, const std::string& name, Value value);
    Value getGlobal(const std::string& name, ValueType type = ValueType::Int) const;
    void setGlobal(const std::string& name, Value value);

    std::int64_t cursor(const std::string& key) const;
    void setCursor(const std::string& key, std::int64_t value);

    void queueAction(std::int64_t actionId, std::vector<std::vector<Value>> args,
                     const std::string& source = "Action");
    void schedule(double delay, std::int64_t actionId, std::vector<std::vector<Value>> args,
                  const std::string& source = "DelayCommand");
    std::vector<Timer> advance(double seconds);

    std::int64_t attrInt(const std::string& name, std::int64_t fallback = 0) const;
    double attrFloat(const std::string& name, double fallback = 0.0) const;
    std::string attrString(const std::string& name, const std::string& fallback = {}) const;
    void setAttr(const std::string& name, AttrValue value);
    bool hasAttr(const std::string& name) const;

    std::map<std::string, double> summary() const;

    std::uint32_t nextId = 1;
    std::map<std::uint32_t, GameObject> objects;
    std::map<std::string, std::vector<std::uint32_t>> tags;
    std::map<std::string, Value> globals;
    std::vector<std::uint32_t> party;
    std::deque<QueuedAction> actionQueue;
    std::vector<Timer> timers;
    std::set<std::int64_t> journal;
    std::vector<std::pair<double, std::string>> log;
    std::vector<WorldEvent> events;
    std::map<std::string, AttrValue> attrs;
    double time = 0.0;
    PythonRandom rng;
    std::uint32_t pc = 0;

private:
    std::map<std::string, std::int64_t> cursors_;
};

} // namespace ke::script
