#pragma once

#include "script/Functor.h"
#include "script/Random.h"
#include "script/ScriptTypes.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace ke::script {

using AttrValue = std::variant<std::int64_t, double, std::string, bool,
                               std::map<std::string, std::int64_t>>;

// Script global names compare without case in retail: the catalogue lookup hashes the
// lower-cased name (00528e50) and compares with _stricmp (00529030, 005e58c0). Shipped
// scripts rely on it: they spell 69 globals in more than one case.
struct NoCaseLess {
    bool operator()(const std::string& a, const std::string& b) const
    {
        const std::size_t count = a.size() < b.size() ? a.size() : b.size();
        for (std::size_t i = 0; i < count; ++i) {
            const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
            const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
            if (x != y) {
                return x < y;
            }
        }
        return a.size() < b.size();
    }
};

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
    std::int64_t functor = -1;
    std::uint32_t target = 0;
    std::vector<std::vector<Value>> args;
    std::string source = "DelayCommand";
};

struct QueuedAction {
    std::int64_t actionId = -1;
    std::int64_t functor = -1;
    std::uint32_t target = 0;
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
enum class GlobalKind : std::uint8_t { Boolean, Number, String, Location };

class World {
public:
    explicit World(std::uint64_t seed = 0x4B4F544F52ULL);

    void reset(std::uint64_t seed = 0x4B4F544F52ULL);
    void clearObjects();

    GameObject& addObject(const std::string& tag, std::int64_t area = 100,
                          std::int64_t baseItem = 0, bool valid = true);
    // Reinsert an object under its own id (module return, save load). Keeps the
    // id allocator ahead of every restored id.
    GameObject& restoreObject(GameObject object);
    GameObject* get(std::uint32_t id);
    const GameObject* get(std::uint32_t id) const;
    bool valid(std::uint32_t id) const;
    std::uint32_t byTag(const std::string& tag, std::int64_t nth = 0) const;
    std::vector<std::uint32_t> byArea(std::int64_t area) const;

    Value getLocal(std::uint32_t id, const std::string& name, ValueType type = ValueType::Int) const;
    void setLocal(std::uint32_t id, const std::string& name, Value value);
    // An object's script variables as retail's CSWVarTable keeps them: 96 booleans (three words) and
    // 8 number bytes, apart from each other. SetLocalBoolean 0059b040 ignores an index outside 0..95;
    // GetLocalBoolean (0053b818 -> 0059b000) gives the raw bit, so index 5 reads 32 and index 31 reads
    // 0x80000000. SetLocalNumber's handler 00543130 ignores a value outside -128..127 and 0059b0d0 an
    // index outside 0..7; GetLocalNumber pushes the byte zero-extended (0053b80c), so -1 reads 255.
    // Stored in `locals` as "b<index>" (0 or 1) and "<index>" (the byte), which the save writes as is.
    static std::string localBooleanKey(std::int64_t index) { return "b" + std::to_string(index); }
    std::int32_t localBoolean(std::uint32_t id, std::int64_t index) const;
    void setLocalBoolean(std::uint32_t id, std::int64_t index, bool value);
    std::int32_t localNumber(std::uint32_t id, std::int64_t index) const;
    void setLocalNumber(std::uint32_t id, std::int64_t index, std::int64_t value);
    Value getGlobal(const std::string& name, ValueType type = ValueType::Int) const;
    void setGlobal(const std::string& name, Value value);

    std::int64_t cursor(const std::string& key) const;
    void setCursor(const std::string& key, std::int64_t value);

    void queueAction(std::int64_t actionId, std::vector<std::vector<Value>> args,
                     const std::string& source = "Action");
    void schedule(double delay, std::int64_t actionId, std::vector<std::vector<Value>> args,
                  const std::string& source = "DelayCommand");
    std::int64_t addFunctor(Functor functor);
    std::shared_ptr<Functor> functor(std::int64_t id) const;
    void queueFunctor(std::int64_t id, std::uint32_t target = 0,
                      const std::string& source = "AssignCommand");
    void scheduleFunctor(double delay, std::int64_t id, std::uint32_t target = 0,
                         const std::string& source = "DelayCommand");
    // Moves the clock and takes every due timer, in retail event order (see World.cpp).
    std::vector<Timer> advance(double seconds);
    void advanceClock(double seconds);
    // The earliest due timer, first scheduled first among equal times; none when nothing is due.
    std::optional<Timer> popDue();
    std::int64_t nowMs() const;
    // When a delay posted now comes due, or nothing when retail drops it (negative delay).
    std::optional<double> dueAfter(double delay) const;
    bool timerDue(const Timer& timer) const;

    std::int64_t attrInt(const std::string& name, std::int64_t fallback = 0) const;
    double attrFloat(const std::string& name, double fallback = 0.0) const;
    std::string attrString(const std::string& name, const std::string& fallback = {}) const;
    void setAttr(const std::string& name, AttrValue value);
    bool hasAttr(const std::string& name) const;

    std::map<std::string, double> summary() const;

    std::uint32_t nextId = 1;
    std::map<std::uint32_t, GameObject> objects;
    std::map<std::string, std::vector<std::uint32_t>> tags;
    std::map<std::string, Value, NoCaseLess> globals;
    std::vector<std::uint32_t> party;
    std::deque<QueuedAction> actionQueue;
    std::vector<Timer> timers;
    std::map<std::int64_t, std::shared_ptr<Functor>> functors;
    std::int64_t nextFunctor = 1;
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
