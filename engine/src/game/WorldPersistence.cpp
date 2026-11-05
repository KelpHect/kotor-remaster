// Module snapshots: captured on leave, reapplied on return, written through
// engine saves (schema kotor.engine.player/4). The mapping onto the original
// SAVEGAME.sav is a later wave.
#include "game/GameSession.h"

#include "kotor/Formats.h"
#include "script/Ncs.h"

#include <algorithm>
#include <ostream>
#include <sstream>

namespace ke {

namespace {

// Save lines are `key=a|b|c`; sub-lists use ',' and ';'. Strings are escaped so
// no script-chosen text can break the framing.
std::string esc(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '|': out += "\\p"; break;
        case ',': out += "\\c"; break;
        case ';': out += "\\s"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string unesc(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        switch (text[++i]) {
        case 'p': out += '|'; break;
        case 'c': out += ','; break;
        case 's': out += ';'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        default: out += text[i]; break;
        }
    }
    return out;
}

std::vector<std::string> fields(const std::string& text, char separator)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t bar = text.find(separator, start);
        if (bar == std::string::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, bar - start));
        start = bar + 1;
    }
    return out;
}

std::vector<std::string> list(const std::string& text)
{
    return text.empty() ? std::vector<std::string>{} : fields(text, ',');
}

std::string writeValue(const script::Value& value)
{
    std::ostringstream out;
    out.precision(17);
    switch (value.type) {
    case script::ValueType::Float: out << "f:" << value.real; break;
    case script::ValueType::String: out << "s:" << esc(value.text); break;
    case script::ValueType::Object: out << "o:" << value.integer; break;
    case script::ValueType::Engine: out << "e:" << value.integer; break;
    case script::ValueType::Functor: out << "u:" << value.integer; break;
    case script::ValueType::Int: out << "i:" << value.integer; break;
    }
    return out.str();
}

script::Value readValue(const std::string& text)
{
    const std::string body = text.size() > 2 ? text.substr(2) : std::string{};
    switch (text.empty() ? 'i' : text[0]) {
    case 'f': return script::Value::makeFloat(std::stod(body));
    case 's': return script::Value::makeString(unesc(body));
    case 'o': return script::Value::makeObject(static_cast<std::uint32_t>(std::stoll(body)));
    case 'e': return script::Value::makeEngine(std::stoll(body));
    case 'u': return script::Value::makeFunctor(std::stoll(body));
    default: return script::Value::makeInt(body.empty() ? 0 : std::stoll(body));
    }
}

std::string writeContext(const ScriptContext& context)
{
    std::ostringstream out;
    bool first = true;
    for (const auto& [name, value] : context) {
        out << (first ? "" : ";") << esc(name) << ":" << value;
        first = false;
    }
    return out.str();
}

ScriptContext readContext(const std::string& text)
{
    ScriptContext context;
    if (text.empty()) {
        return context;
    }
    for (const std::string& item : fields(text, ';')) {
        const std::size_t colon = item.rfind(':');
        if (colon != std::string::npos) {
            context[unesc(item.substr(0, colon))] =
                static_cast<std::uint32_t>(std::stoul(item.substr(colon + 1)));
        }
    }
    return context;
}

void writeObject(std::ostream& file, const std::string& module, const script::GameObject& object)
{
    file << "sobj=" << esc(module) << "|" << object.id << "|" << esc(object.tag) << "|"
         << object.area << "|" << object.baseItem << "|" << (object.valid ? 1 : 0) << "|"
         << object.position[0] << "|" << object.position[1] << "|" << object.position[2] << "|"
         << object.hp << "|";
    for (std::size_t i = 0; i < object.inventory.size(); ++i) {
        file << (i == 0 ? "" : ",") << object.inventory[i];
    }
    file << "\n";
    for (const auto& [name, value] : object.locals) {
        file << "slocal=" << esc(module) << "|" << object.id << "|" << esc(name) << "|"
             << writeValue(value) << "\n";
    }
    for (const auto& [name, attr] : object.attrs) {
        std::string encoded;
        if (const auto* integer = std::get_if<std::int64_t>(&attr)) {
            encoded = writeValue(script::Value::makeInt(*integer));
        } else if (const auto* real = std::get_if<double>(&attr)) {
            encoded = writeValue(script::Value::makeFloat(*real));
        } else if (const auto* text = std::get_if<std::string>(&attr)) {
            encoded = writeValue(script::Value::makeString(*text));
        } else if (const auto* flag = std::get_if<bool>(&attr)) {
            encoded = writeValue(script::Value::makeInt(*flag ? 1 : 0));
        } else {
            continue; // Map-valued attrs are derived data no family persists yet.
        }
        file << "sattr=" << esc(module) << "|" << object.id << "|" << esc(name) << "|" << encoded
             << "\n";
    }
}

void writeSnapshot(std::ostream& file, const std::string& module, const ModuleSnapshot& snapshot)
{
    const std::string m = esc(module);
    file << "snapshot=" << m << "|" << snapshot.areaObject << "|" << snapshot.moduleObject << "\n";
    for (const auto& [id, object] : snapshot.objects) {
        writeObject(file, module, object);
    }
    for (const EntitySnapshot& entity : snapshot.entities) {
        file << "sent=" << m << "|" << entity.index << "|" << entity.object << "|"
             << (entity.spawned ? 1 : 0) << "|" << esc(entity.templateResref) << "|"
             << esc(entity.tag) << "|" << (entity.dead ? 1 : 0) << "|" << (entity.open ? 1 : 0)
             << "|" << (entity.locked ? 1 : 0) << "|" << (entity.hostile ? 1 : 0) << "|"
             << (entity.hostileOverride ? 1 : 0) << "|" << (entity.useable ? 1 : 0) << "|"
             << entity.position.x << "|" << entity.position.y << "|" << entity.position.z << "|"
             << entity.bearing << "|" << entity.hitPoints << "|" << entity.maxHitPoints << "|"
             << entity.faction << "|" << esc(entity.conversation) << "|";
        for (std::size_t i = 0; i < entity.lootItems.size(); ++i) {
            file << (i == 0 ? "" : ",") << esc(entity.lootItems[i]);
        }
        file << "\n";
    }
    for (std::size_t i = 0; i < snapshot.merchants.size(); ++i) {
        const MerchantState& merchant = snapshot.merchants[i];
        file << "smerchant=" << m << "|" << merchant.entity << "|" << esc(merchant.resref) << "|"
             << esc(merchant.name) << "|" << merchant.markUp << "|" << merchant.markDown << "|"
             << merchant.buySellFlag << "|" << merchant.gold << "|";
        for (std::size_t s = 0; s < merchant.stock.size(); ++s) {
            const MerchantStockItem& stock = merchant.stock[s];
            file << (s == 0 ? "" : ";") << esc(stock.item.resref) << ":" << stock.item.baseItem
                 << ":" << stock.item.cost << ":" << stock.item.stack << ":"
                 << (stock.item.identified ? 1 : 0) << ":" << (stock.infinite ? 1 : 0) << ":"
                 << stock.remaining;
        }
        file << "\n";
    }
    for (const EncounterState& encounter : snapshot.encounters) {
        file << "sencounter=" << m << "|" << (encounter.spawned ? 1 : 0) << "|"
             << (encounter.exhausted ? 1 : 0) << "|" << encounter.spawnedCount << "|"
             << encounter.respawnsRemaining << "|" << encounter.resetTime << "|"
             << encounter.resetTimer << "|";
        for (std::size_t i = 0; i < encounter.entities.size(); ++i) {
            file << (i == 0 ? "" : ",") << encounter.entities[i];
        }
        file << "\n";
    }
    file << "strigger=" << m << "|";
    for (std::size_t i = 0; i < snapshot.triggerInside.size(); ++i) {
        file << (snapshot.triggerInside[i] ? '1' : '0');
    }
    file << "\n";
    for (const TimerSnapshot& timer : snapshot.timers) {
        if (timer.functor == nullptr || timer.functor->script.empty()) {
            continue; // Cannot be rebuilt without the script it came from.
        }
        const script::Functor& functor = *timer.functor;
        file << "stimer=" << m << "|" << timer.remaining << "|" << timer.target << "|"
             << esc(timer.source) << "|" << timer.functorId << "|" << esc(functor.script) << "|"
             << esc(functor.source) << "|" << functor.ip << "|" << functor.bp << "|"
             << writeContext(functor.context) << "|";
        for (std::size_t i = 0; i < functor.stack.size(); ++i) {
            file << (i == 0 ? "" : ",") << writeValue(functor.stack[i]);
        }
        file << "\n";
    }
    for (const auto& [id, actor] : snapshot.actors) {
        if (!actor.commandable) {
            file << "sactor=" << m << "|" << id << "|0\n";
        }
    }
}

script::GameObject& objectIn(LoadedWorldState& out, const std::string& module, std::uint32_t id)
{
    return module == "*" ? out.carried[id] : out.snapshots[module].objects[id];
}

} // namespace

// ---- saving ----------------------------------------------------------------

void GameSession::writeWorldState(std::ostream& file) const
{
    file.precision(9);
    file << "world_time=" << scriptWorld_.time << "\n";
    file << "elapsed=" << elapsed_ << "\n";
    file << "run_script_var=" << sim_->runScriptVar << "\n";
    if (sim_->clockReady) {
        file << "clock_base_ms=" << sim_->clockBaseMs << "\n";
    }
    file << "next_object_id=" << scriptWorld_.nextId << "\n";
    file << "next_functor=" << scriptWorld_.nextFunctor << "\n";
    for (const auto& [name, location] : locationGlobals_) {
        file << "global_location=" << esc(name);
        for (const float value : location) {
            file << "|" << value;
        }
        file << "\n";
    }
    for (const auto& [id, record] : sim_->engine.records()) {
        if (const auto* location = std::get_if<LocationData>(&record)) {
            file << "engine_location=" << id << "|" << location->position.x << "|"
                 << location->position.y << "|" << location->position.z << "|" << location->facing
                 << "|" << location->area << "\n";
        } else if (const auto* event = std::get_if<EventData>(&record)) {
            file << "engine_event=" << id << "|" << esc(event->hook) << "|"
                 << (event->moduleEvent ? 1 : 0) << "|" << writeContext(event->context) << "\n";
        }
    }
    for (const auto& [id, object] : collectCarried()) {
        writeObject(file, "*", object);
    }
    for (const auto& [module, snapshot] : sim_->snapshots) {
        if (module != module_) {
            writeSnapshot(file, module, snapshot);
        }
    }
    writeSnapshot(file, module_, captureModuleSnapshot(false));
}

// ---- loading ---------------------------------------------------------------

bool parseWorldStateLine(const std::string& key, const std::string& value, LoadedWorldState& out)
{
    if (key == "world_time") {
        out.worldTime = std::stod(value);
    } else if (key == "elapsed") {
        out.elapsed = std::stof(value);
    } else if (key == "run_script_var") {
        out.runScriptVar = std::stoi(value);
    } else if (key == "clock_base_ms") {
        out.clockBaseMs = std::stod(value);
    } else if (key == "next_object_id") {
        out.nextObject = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "next_functor") {
        out.nextFunctor = std::stoll(value);
    } else if (key == "global_location") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 7) {
            std::array<float, 6> location{};
            for (std::size_t i = 0; i < 6; ++i) {
                location[i] = std::stof(p[i + 1]);
            }
            out.locationGlobals[unesc(p[0])] = location;
        }
    } else if (key == "engine_location") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 6) {
            LocationData location;
            location.position = {std::stof(p[1]), std::stof(p[2]), std::stof(p[3])};
            location.facing = std::stof(p[4]);
            location.area = static_cast<std::uint32_t>(std::stoul(p[5]));
            out.engine.emplace_back(std::stoll(p[0]), location);
        }
    } else if (key == "engine_event") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 4) {
            EventData event;
            event.hook = unesc(p[1]);
            event.moduleEvent = p[2] == "1";
            event.context = readContext(p[3]);
            out.engine.emplace_back(std::stoll(p[0]), std::move(event));
        }
    } else if (key == "snapshot") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 3) {
            ModuleSnapshot& snapshot = out.snapshots[unesc(p[0])];
            snapshot.areaObject = static_cast<std::uint32_t>(std::stoul(p[1]));
            snapshot.moduleObject = static_cast<std::uint32_t>(std::stoul(p[2]));
        }
    } else if (key == "sobj") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 11) {
            const std::uint32_t id = static_cast<std::uint32_t>(std::stoul(p[1]));
            script::GameObject& object = objectIn(out, unesc(p[0]), id);
            object.id = id;
            object.tag = unesc(p[2]);
            object.area = std::stoll(p[3]);
            object.baseItem = std::stoll(p[4]);
            object.valid = p[5] == "1";
            object.position = {std::stod(p[6]), std::stod(p[7]), std::stod(p[8])};
            object.hp = std::stoll(p[9]);
            for (const std::string& item : list(p[10])) {
                object.inventory.push_back(static_cast<std::uint32_t>(std::stoul(item)));
            }
        }
    } else if (key == "slocal") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 4) {
            objectIn(out, unesc(p[0]), static_cast<std::uint32_t>(std::stoul(p[1])))
                .locals[unesc(p[2])] = readValue(p[3]);
        }
    } else if (key == "sattr") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 4) {
            const script::Value decoded = readValue(p[3]);
            script::AttrValue attr;
            if (decoded.type == script::ValueType::Float) {
                attr = decoded.real;
            } else if (decoded.type == script::ValueType::String) {
                attr = decoded.text;
            } else {
                attr = decoded.integer;
            }
            objectIn(out, unesc(p[0]), static_cast<std::uint32_t>(std::stoul(p[1])))
                .attrs[unesc(p[2])] = std::move(attr);
        }
    } else if (key == "sent") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 21) {
            EntitySnapshot entity;
            entity.index = static_cast<std::size_t>(std::stoull(p[1]));
            entity.object = static_cast<std::uint32_t>(std::stoul(p[2]));
            entity.spawned = p[3] == "1";
            entity.templateResref = unesc(p[4]);
            entity.tag = unesc(p[5]);
            entity.dead = p[6] == "1";
            entity.open = p[7] == "1";
            entity.locked = p[8] == "1";
            entity.hostile = p[9] == "1";
            entity.hostileOverride = p[10] == "1";
            entity.useable = p[11] == "1";
            entity.position = {std::stof(p[12]), std::stof(p[13]), std::stof(p[14])};
            entity.bearing = std::stof(p[15]);
            entity.hitPoints = std::stoi(p[16]);
            entity.maxHitPoints = std::stoi(p[17]);
            entity.faction = static_cast<std::uint32_t>(std::stoul(p[18]));
            entity.conversation = unesc(p[19]);
            for (const std::string& item : list(p[20])) {
                entity.lootItems.push_back(unesc(item));
            }
            out.snapshots[unesc(p[0])].entities.push_back(std::move(entity));
        }
    } else if (key == "smerchant") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 9) {
            MerchantState merchant;
            merchant.entity = static_cast<std::size_t>(std::stoull(p[1]));
            merchant.resref = unesc(p[2]);
            merchant.name = unesc(p[3]);
            merchant.markUp = std::stoi(p[4]);
            merchant.markDown = std::stoi(p[5]);
            merchant.buySellFlag = std::stoi(p[6]);
            merchant.gold = std::stoi(p[7]);
            if (!p[8].empty()) {
                for (const std::string& item : fields(p[8], ';')) {
                    const std::vector<std::string> s = fields(item, ':');
                    if (s.size() >= 7) {
                        MerchantStockItem stock;
                        stock.item.resref = unesc(s[0]);
                        stock.item.baseItem = std::stoi(s[1]);
                        stock.item.cost = std::stoi(s[2]);
                        stock.item.stack = std::stoi(s[3]);
                        stock.item.identified = s[4] == "1";
                        stock.infinite = s[5] == "1";
                        stock.remaining = std::stoi(s[6]);
                        merchant.stock.push_back(std::move(stock));
                    }
                }
            }
            out.snapshots[unesc(p[0])].merchants.push_back(std::move(merchant));
        }
    } else if (key == "sencounter") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 8) {
            EncounterState encounter;
            encounter.spawned = p[1] == "1";
            encounter.exhausted = p[2] == "1";
            encounter.spawnedCount = std::stoi(p[3]);
            encounter.respawnsRemaining = std::stoi(p[4]);
            encounter.resetTime = std::stof(p[5]);
            encounter.resetTimer = std::stof(p[6]);
            for (const std::string& item : list(p[7])) {
                encounter.entities.push_back(static_cast<std::size_t>(std::stoull(item)));
            }
            out.snapshots[unesc(p[0])].encounters.push_back(std::move(encounter));
        }
    } else if (key == "strigger") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 2) {
            std::vector<bool>& inside = out.snapshots[unesc(p[0])].triggerInside;
            inside.clear();
            for (const char flag : p[1]) {
                inside.push_back(flag == '1');
            }
        }
    } else if (key == "stimer") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 11) {
            TimerSnapshot timer;
            timer.remaining = std::stod(p[1]);
            timer.target = static_cast<std::uint32_t>(std::stoul(p[2]));
            timer.source = unesc(p[3]);
            timer.functorId = std::stoll(p[4]);
            auto functor = std::make_shared<script::Functor>();
            functor->script = unesc(p[5]);
            functor->source = unesc(p[6]);
            functor->ip = static_cast<std::size_t>(std::stoull(p[7]));
            functor->bp = std::stoll(p[8]);
            functor->context = readContext(p[9]);
            for (const std::string& cell : list(p[10])) {
                functor->stack.push_back(readValue(cell));
            }
            timer.functor = std::move(functor);
            ModuleSnapshot& snapshot = out.snapshots[unesc(p[0])];
            snapshot.functors[timer.functorId] = timer.functor;
            snapshot.timers.push_back(std::move(timer));
        }
    } else if (key == "sactor") {
        const std::vector<std::string> p = fields(value, '|');
        if (p.size() >= 3) {
            out.snapshots[unesc(p[0])]
                .actors[static_cast<std::uint32_t>(std::stoul(p[1]))]
                .commandable = p[2] == "1";
        }
    } else {
        return false;
    }
    return true;
}

bool GameSession::restoreWorldFromSave(LoadedWorldState state, const std::string& module,
                                       const kotor::Vec3& position)
{
    sim_->snapshots = std::move(state.snapshots);
    sim_->carried = std::move(state.carried);
    sim_->engine.clear();
    for (auto& [id, record] : state.engine) {
        sim_->engine.restore(id, std::move(record));
    }
    sim_->actors.clear();
    sim_->events.clear();
    sim_->runScriptVar = state.runScriptVar;
    locationGlobals_ = std::move(state.locationGlobals);
    scriptWorld_.time = state.worldTime;
    if (state.clockBaseMs >= 0.0) {
        sim_->clockBaseMs = state.clockBaseMs;
        sim_->clockReady = true;
    }
    elapsed_ = state.elapsed;
    // The caller re-adds the party once the world exists.
    companions_.clear();
    partyTags_.clear();
    sim_->restoringSave = true;
    bool ok = false;
    try {
        ok = loadModule(module, &position, {});
    } catch (const std::exception& exception) {
        logEvent(std::string("save module failed: ") + exception.what());
    }
    sim_->restoringSave = false;
    scriptWorld_.nextId = std::max(scriptWorld_.nextId, state.nextObject);
    scriptWorld_.nextFunctor = std::max(scriptWorld_.nextFunctor, state.nextFunctor);
    return ok;
}

// ---- capture and apply -----------------------------------------------------

// Everything that travels with the player: the PC, the party, and what they hold.
std::map<std::uint32_t, script::GameObject> GameSession::collectCarried() const
{
    std::map<std::uint32_t, script::GameObject> carried;
    const auto carry = [&](std::uint32_t id) {
        const script::GameObject* object = scriptWorld_.get(id);
        if (object == nullptr) {
            return;
        }
        carried[id] = *object;
        for (const std::uint32_t item : object->inventory) {
            if (const script::GameObject* held = scriptWorld_.get(item); held != nullptr) {
                carried[item] = *held;
            }
        }
    };
    carry(scriptWorld_.pc);
    for (const std::uint32_t member : scriptWorld_.party) {
        carry(member);
    }
    return carried;
}

void GameSession::restoreCarried()
{
    for (const auto& [id, object] : sim_->carried) {
        scriptWorld_.restoreObject(object);
    }
    // Facts about objects that stayed behind travel in their module's snapshot.
    for (auto it = sim_->actors.begin(); it != sim_->actors.end();) {
        if (sim_->carried.contains(it->first)) {
            it->second.queue.clear();
            ++it;
        } else if (it->first == scriptWorld_.pc) {
            it->second.queue.clear();
            ++it;
        } else {
            it = sim_->actors.erase(it);
        }
    }
}

void GameSession::leaveModule()
{
    runSceneHook("Mod_OnClientLeav", scriptWorld_.pc);
    runSceneHook("OnExit", scriptWorld_.pc);
    sim_->events.clear();
    sim_->snapshots[module_] = captureModuleSnapshot(true);
    sim_->carried = collectCarried();
}

ModuleSnapshot GameSession::captureModuleSnapshot(bool leaving) const
{
    ModuleSnapshot snapshot;
    snapshot.areaObject = sim_->areaObject;
    snapshot.moduleObject = sim_->moduleObject;
    const std::map<std::uint32_t, script::GameObject> carried = collectCarried();
    for (const auto& [id, object] : scriptWorld_.objects) {
        if (!carried.contains(id)) {
            snapshot.objects[id] = object;
        }
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        // The player avatar and party avatars belong to the player, not the module.
        if (i == playerEntity_ || entity.partyAvatar) {
            continue;
        }
        EntitySnapshot record;
        record.index = i;
        record.object = objectForEntity(i);
        record.spawned = i >= sim_->baseEntityCount;
        record.templateResref = entity.templateResref;
        record.tag = entity.tag;
        record.dead = entity.dead;
        record.open = entity.open;
        record.locked = entity.locked;
        record.hostile = entity.hostile;
        record.hostileOverride = entity.hostileOverride;
        record.useable = entity.useable;
        record.position = entity.position;
        record.bearing = entity.bearing;
        record.hitPoints = entity.hitPoints;
        record.maxHitPoints = entity.maxHitPoints;
        record.faction = entity.faction;
        record.conversation = entity.conversation;
        record.lootItems = entity.lootItems;
        snapshot.entities.push_back(std::move(record));
    }
    snapshot.merchants = merchants_;
    snapshot.encounters = encounterStates_;
    // On return every trigger starts outside, so the player standing in one fires
    // it again; a save keeps the exact state.
    snapshot.triggerInside = leaving ? std::vector<bool>(triggerInside_.size(), false)
                                     : triggerInside_;
    snapshot.functors = scriptWorld_.functors;
    for (const script::Timer& timer : scriptWorld_.timers) {
        if (timer.functor < 0) {
            continue;
        }
        TimerSnapshot saved;
        saved.remaining = std::max(0.0, timer.due - scriptWorld_.time);
        saved.target = timer.target;
        saved.source = timer.source;
        saved.functorId = timer.functor;
        saved.functor = scriptWorld_.functor(timer.functor);
        if (saved.functor != nullptr) {
            snapshot.timers.push_back(std::move(saved));
        }
    }
    for (const auto& [id, actor] : sim_->actors) {
        if (!carried.contains(id)) {
            snapshot.actors[id] = actor;
        }
    }
    return snapshot;
}

void GameSession::applyModuleSnapshot(const ModuleSnapshot& snapshot)
{
    for (const auto& [id, object] : snapshot.objects) {
        scriptWorld_.restoreObject(object);
    }
    sim_->areaObject = snapshot.areaObject;
    sim_->moduleObject = snapshot.moduleObject;
    entityObjects_.assign(scene_.entities.size(), 0);
    std::map<std::size_t, std::size_t> remap;
    for (const EntitySnapshot& record : snapshot.entities) {
        std::size_t index = record.index;
        if (record.spawned) {
            index = kotor::spawnCreature(scene_, install_, record.templateResref, record.position,
                                         record.bearing);
            if (index == static_cast<std::size_t>(-1)) {
                logEvent("snapshot: cannot respawn " + record.templateResref);
                continue;
            }
            entityObjects_.resize(scene_.entities.size(), 0);
        } else if (index >= scene_.entities.size()) {
            continue;
        }
        remap[record.index] = index;
        kotor::Entity& entity = scene_.entities[index];
        entity.tag = record.tag;
        entity.dead = record.dead;
        entity.open = record.open;
        entity.locked = record.locked;
        entity.hostile = record.hostile;
        entity.hostileOverride = record.hostileOverride;
        entity.useable = record.useable;
        entity.position = record.position;
        entity.bearing = record.bearing;
        entity.hitPoints = record.hitPoints;
        entity.maxHitPoints = record.maxHitPoints;
        entity.faction = record.faction;
        entity.conversation = record.conversation;
        entity.lootItems = record.lootItems;
        entityObjects_[index] = record.object;
    }
    merchants_ = snapshot.merchants;
    activeMerchant_ = -1;
    encounterStates_ = snapshot.encounters;
    for (EncounterState& encounter : encounterStates_) {
        std::vector<std::size_t> entities;
        for (const std::size_t old : encounter.entities) {
            if (const auto it = remap.find(old); it != remap.end()) {
                entities.push_back(it->second);
            }
        }
        encounter.entities = std::move(entities);
    }
    encounterStates_.resize(scene_.encounters.size());
    triggerInside_ = snapshot.triggerInside;
    triggerInside_.resize(scene_.triggers.size(), false);
    for (const auto& [id, functor] : snapshot.functors) {
        scriptWorld_.functors[id] = functor;
    }
    for (const TimerSnapshot& saved : snapshot.timers) {
        // A timer read from a save has no code yet; its script is reachable now
        // that this module is bound.
        if (saved.functor == nullptr) {
            continue;
        }
        if (saved.functor->instructions.empty()) {
            const auto decoded = decodedScript(saved.functor->script);
            if (decoded == nullptr) {
                scriptLog_.push_back("snapshot: dropped timer from " + saved.functor->script);
                continue;
            }
            saved.functor->instructions = *decoded;
        }
        script::Timer timer;
        timer.due = scriptWorld_.time + saved.remaining;
        timer.functor = saved.functorId;
        timer.target = saved.target;
        timer.source = saved.source;
        scriptWorld_.timers.push_back(std::move(timer));
    }
    for (const auto& [id, actor] : snapshot.actors) {
        sim_->actors[id] = actor;
    }
}

// A script-created creature: same object shape as an encounter spawn.
std::uint32_t GameSession::spawnScriptCreature(const std::string& resref, kotor::Vec3 position,
                                               float facing)
{
    const std::size_t index = kotor::spawnCreature(scene_, install_, resref, position, facing);
    if (index == static_cast<std::size_t>(-1)) {
        return script::kObjectInvalid;
    }
    const kotor::Entity& spawned = scene_.entities[index];
    script::GameObject& object = scriptWorld_.addObject(spawned.tag, 100, 0, true);
    object.position = {spawned.position.x, spawned.position.y, spawned.position.z};
    object.hp = spawned.hitPoints;
    object.attrs["ac"] = static_cast<std::int64_t>(spawned.defense);
    object.attrs["faction"] = static_cast<std::int64_t>(spawned.faction);
    object.attrs["object_type"] = static_cast<std::int64_t>(1);
    const std::uint32_t id = object.id;
    entityObjects_.resize(scene_.entities.size(), 0);
    entityObjects_[index] = id;
    runHook("ScriptSpawn", index, scriptWorld_.pc);
    return id;
}

} // namespace ke
