// SignalEvent, event constructors, ExecuteScript and hook dispatch.
#include "game/ScriptHost.h"

#include <algorithm>

namespace ke {

namespace {

constexpr double kHeartbeatSeconds = 6.0;
constexpr int kEventsPerTick = 256;

using Cells = std::optional<std::vector<script::Value>>;

// ACT_8 ExecuteScript 00535b70: when the third argument is present it writes
// the global GetRunScriptVar reads and never restores it, then runs the script
// synchronously (005d0fc0 -> 005d45d0). Event objects are not on the stack in
// retail; they are read off the objects, so an executed script keeps seeing them.
Cells executeScript(ActionCall& call)
{
    ScriptApi api = call.api();
    if (call.hasArg(2)) {
        api.sim().runScriptVar = static_cast<std::int32_t>(call.intArg(2));
    }
    const std::string script = call.stringArg(0);
    const std::uint32_t target = call.objectArg(1);
    if (!script.empty() && call.world.get(target) != nullptr) {
        ScriptContext context = api.context();
        context.erase("self");
        api.runScript(script, target, context);
    }
    return ActionCall::none();
}

// ACT_565 GetRunScriptVar 0053d020 returns that global.
Cells getRunScriptVar(ActionCall& call)
{
    return ActionCall::retInt(call.api().sim().runScriptVar);
}

// ACT_131 SignalEvent 005439d0 posts a zero-delay event, so the hook runs on a
// later tick and never inside the signalling script.
Cells signalEvent(ActionCall& call)
{
    ScriptApi api = call.api();
    const EventData* event = api.sim().engine.get<EventData>(call.engineArg(1));
    if (event == nullptr) {
        return ActionCall::none();
    }
    const std::uint32_t target =
        event->moduleEvent ? api.sim().moduleObject : call.objectArgOrInvalid(0);
    if (call.world.get(target) == nullptr) {
        return ActionCall::none();
    }
    ScriptContext context = event->context;
    context["eventSource"] = call.self();
    api.raiseHook(target, event->hook, std::move(context));
    return ActionCall::none();
}

// ACT_132 EventUserDefined 00535a90 builds event type 0xb.
Cells eventUserDefined(ActionCall& call)
{
    EventData event;
    event.hook = "UserDefined";
    event.context["userDefinedNumber"] = static_cast<std::uint32_t>(call.intArg(0));
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(event)));
}

Cells getUserDefinedEventNumber(ActionCall& call)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find("userDefinedNumber");
    return ActionCall::retInt(it != context.end() ? static_cast<std::int32_t>(it->second) : 0);
}

Cells eventConversation(ActionCall& call)
{
    EventData event;
    event.hook = "Dialogue";
    event.context["lastSpeaker"] = call.self();
    event.context["listenPattern"] = static_cast<std::uint32_t>(-1);
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(event)));
}

Cells eventSpellCastAt(ActionCall& call)
{
    EventData event;
    event.hook = "SpellAt";
    event.context["lastSpellCaster"] = call.objectArgOrInvalid(0);
    event.context["lastSpell"] = static_cast<std::uint32_t>(call.intArg(1));
    event.context["lastSpellHarmful"] = call.intArg(2, 1) != 0 ? 1u : 0u;
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(event)));
}

// The module owns item activation whatever object the event is signalled to.
Cells eventActivateItem(ActionCall& call)
{
    EventData event;
    event.hook = "Mod_OnActvtItem";
    event.moduleEvent = true;
    event.context["itemActivated"] = call.objectArgOrInvalid(0);
    event.context["itemActivator"] = call.self();
    event.context["itemLocation"] = static_cast<std::uint32_t>(call.engineArg(1));
    event.context["itemTarget"] = call.objectArgOrInvalid(2);
    return ActionCall::retEngine(call.api().sim().engine.add(std::move(event)));
}

// The interact path fires ScriptDialogue with pattern -1, the value shipped
// handlers test before calling BeginConversation.
Cells getListenPatternNumber(ActionCall& call)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find("listenPattern");
    return ActionCall::retInt(it != context.end() ? static_cast<std::int32_t>(it->second) : -1);
}

Cells contextObjectGetter(ActionCall& call, const char* key)
{
    return ActionCall::retObject(call.api().contextObject(key));
}

Cells contextIntGetter(ActionCall& call, const char* key)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find(key);
    return ActionCall::retInt(it != context.end() ? static_cast<std::int32_t>(it->second) : 0);
}

// Fields retail reads off the named object rather than the running script.
Cells getLastAttacker(ActionCall& call)
{
    const ScriptContext& last = call.api().actor(call.objectArg(0)).last;
    const auto it = last.find("lastAttacker");
    return ActionCall::retObject(it != last.end() ? it->second : script::kObjectInvalid);
}

Cells itemActivatedLocation(ActionCall& call)
{
    const ScriptContext& context = call.api().context();
    const auto it = context.find("itemLocation");
    return ActionCall::retEngine(it != context.end() ? static_cast<std::int64_t>(it->second) : 0);
}

} // namespace

void tickEvents(ScriptApi api)
{
    // Only what is queued now runs now: an event raised by a hook is deferred to
    // the next tick, as retail's zero-delay events are.
    std::size_t pending = std::min<std::size_t>(api.sim().events.size(), kEventsPerTick);
    while (pending-- > 0 && !api.sim().events.empty()) {
        ScriptEvent event = std::move(api.sim().events.front());
        api.sim().events.pop_front();
        api.dispatchHook(event.target, event.hook, event.context);
    }
}

// Retail runs one heartbeat per object every six seconds, not all at once. The
// phase comes from the object id so the order is deterministic.
void tickHeartbeats(ScriptApi api, float dt)
{
    (void)dt;
    WorldSim& sim = api.sim();
    const double now = api.world().time;
    for (std::size_t i = 0; i < api.scene().entities.size(); ++i) {
        if (sim.heartbeatDue.size() <= i) {
            const std::uint32_t object = api.objectFor(i);
            sim.heartbeatDue.push_back(now + kHeartbeatSeconds * ((object % 60u) + 1.0) / 60.0);
        }
        if (sim.heartbeatDue[i] > now) {
            continue;
        }
        sim.heartbeatDue[i] = now + kHeartbeatSeconds;
        if (api.scene().entities[i].dead) {
            continue;
        }
        api.dispatchHook(api.objectFor(i), "Heartbeat", {});
    }
}

void registerEventActions(HandlerTable& table)
{
    table.add(8, executeScript);
    table.add(36, getLastAttacker);
    table.add(131, signalEvent);
    table.add(132, eventUserDefined);
    table.add(195, getListenPatternNumber);
    table.add(244, eventSpellCastAt);
    table.add(246, [](ActionCall& call) -> Cells { return contextIntGetter(call, "lastSpell"); });
    table.add(247, getUserDefinedEventNumber);
    table.add(257, [](ActionCall& call) -> Cells { return contextIntGetter(call, "perceptionHeard"); });
    table.add(258, [](ActionCall& call) -> Cells { return contextIntGetter(call, "perceptionInaudible"); });
    table.add(259, [](ActionCall& call) -> Cells { return contextIntGetter(call, "perceptionSeen"); });
    table.add(261, [](ActionCall& call) -> Cells { return contextIntGetter(call, "perceptionVanished"); });
    table.add(282, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "moduleItemAcquired"); });
    table.add(283, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "moduleItemAcquiredFrom"); });
    table.add(292, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "moduleItemLost"); });
    table.add(293, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "moduleItemLostBy"); });
    table.add(295, eventConversation);
    table.add(348, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "lastDisturbed"); });
    table.add(352, [](ActionCall& call) -> Cells { return contextIntGetter(call, "disturbType"); });
    table.add(353, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "disturbItem"); });
    table.add(423, [](ActionCall& call) -> Cells { return contextIntGetter(call, "lastSpellHarmful"); });
    table.add(424, eventActivateItem);
    table.add(441, itemActivatedLocation);
    table.add(542, [](ActionCall& call) -> Cells { return contextObjectGetter(call, "pcLevellingUp"); });
    table.add(556, getLastAttacker);
    table.add(565, getRunScriptVar);
}

} // namespace ke
