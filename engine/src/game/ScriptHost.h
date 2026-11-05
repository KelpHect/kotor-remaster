#pragma once

// World-simulation layer behind GameSession::action. docs/recon/63_WORLD_SIM.md
// is the contract: a 772-entry handler table consulted before the legacy switch,
// one register function per action family, engine values behind EngineStore,
// per-object action queues, a deferred event queue and per-module snapshots.

#include "game/Rules.h"
#include "kotor/Formats.h"
#include "script/ActionRuntime.h"
#include "script/World.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace ke {

class GameSession;
struct WorldSim;
struct ScriptApi;

using ScriptArgs = script::ActionRuntime::Args;
// Named event/context objects a script reads back (GetLastAttacker and friends).
using ScriptContext = std::map<std::string, std::uint32_t>;

constexpr std::size_t kScriptActionCount = 772;

// ---- engine values ---------------------------------------------------------

// Effect kinds with no public EFFECT_TYPE_* constant; they read back as 0.
namespace effect {
constexpr std::int32_t kHeal = 200;
constexpr std::int32_t kDamage = 201;
constexpr std::int32_t kDeath = 202;
constexpr std::int32_t kKnockdown = 203;
constexpr std::int32_t kHealForce = 204;
constexpr std::int32_t kDamageForce = 205;
constexpr std::int32_t kTemporaryForce = 206;
constexpr std::int32_t kChoke = 207;
constexpr std::int32_t kDroidStun = 208;
constexpr std::int32_t kForcePushed = 209;
constexpr std::int32_t kForcePushTargeted = 210;
constexpr std::int32_t kHorrified = 211;
constexpr std::int32_t kForceShield = 212;
constexpr std::int32_t kBodyFuel = 213;
constexpr std::int32_t kForceResisted = 214;
constexpr std::int32_t kForceFizzle = 215;
constexpr std::int32_t kForceDrain = 216;
constexpr std::int32_t kPsychicStatic = 217;
constexpr std::int32_t kWhirlwind = 218;
constexpr std::int32_t kDeflectionIncrease = 219;
constexpr std::int32_t kDeflectionDecrease = 220;
constexpr std::int32_t kModifyAttacks = 221;
constexpr std::int32_t kDamageShield = 222;
constexpr std::int32_t kDyingHitPoints = 223;
constexpr std::int32_t kCutsceneHorrified = 224;
constexpr std::int32_t kCutsceneParalyze = 225;
constexpr std::int32_t kCutsceneStunned = 226;
} // namespace effect

struct LocationData {
    kotor::Vec3 position;
    float facing = 0.0f; // Degrees, as nwscript exposes it.
    std::uint32_t area = script::kObjectInvalid;
};

struct EffectData {
    std::int32_t type = 0;
    std::vector<script::Value> args;
    std::string label;
    std::vector<std::int64_t> links; // EffectLinkEffects: applied and removed together.
    std::int32_t subType = 8;        // SUBTYPE_MAGICAL by default; retail effect ctor.
    std::int32_t icon = 0;           // SetEffectIcon
};

// A constructed event. `hook` is the logical hook name raiseHook resolves.
struct EventData {
    std::string hook;
    ScriptContext context;
    bool moduleEvent = false;
};

struct TalentData {
    std::int32_t type = 0;
    std::int32_t id = 0;
};

using EngineRecord = std::variant<LocationData, EffectData, EventData, TalentData>;

// Backing store for Value::makeEngine ids; 0 is the invalid value. Ids start
// above the range legacy ActionRuntime effect tags and small talents use, so an
// old value can never read as a store record. Records are not collected: they
// live for the session, which is a known limit until the effects wave.
class EngineStore {
public:
    std::int64_t add(EngineRecord record);
    template <typename T>
    const T* get(std::int64_t id) const
    {
        const auto it = records_.find(id);
        return it == records_.end() ? nullptr : std::get_if<T>(&it->second);
    }
    const std::map<std::int64_t, EngineRecord>& records() const { return records_; }
    void restore(std::int64_t id, EngineRecord record);
    void clear();

private:
    std::map<std::int64_t, EngineRecord> records_;
    std::int64_t next_ = 0x10000;
};

// ---- actors ----------------------------------------------------------------

enum class ActorActionKind {
    DoCommand,
    Wait,
    MoveToObject,
    MoveToPoint,
    JumpToObject,
    JumpToPoint,
    OpenDoor,
    CloseDoor,
    LockObject,
    UnlockObject,
    StartConversation,
    PauseConversation,
    ResumeConversation,
    PlayAnimation,
    Attack,
    CastSpell,
};

struct ActorAction {
    ActorActionKind kind = ActorActionKind::Wait;
    std::uint32_t object = script::kObjectInvalid;
    kotor::Vec3 point;
    std::optional<float> facingDegrees;
    bool run = false;
    bool force = false;       // ActionForceMove*: jump when the timeout expires.
    bool ignoreRange = false; // ActionStartConversation.
    bool started = false;
    bool userIssued = false;
    float range = 1.0f;
    float timeout = 30.0f;
    float remaining = 0.0f;
    float stuck = 0.0f;
    std::int64_t functor = -1;
    std::string text;
    std::int32_t animation = 0;
    std::int32_t spell = -1;   // CastSpell: spells.2da row. Attack: combat feat, or -1.
    bool fake = false;         // ActionCastFakeSpell*: no cost, no impact script.
    std::int64_t location = 0; // CastSpell at a location: the LocationData record.
};

// One effect on a creature, as ApplyEffectToObject left it. `effect` keeps the
// store record so GetFirstEffect hands scripts the id they applied.
struct AppliedEffect {
    std::int64_t effect = 0;
    std::int32_t type = 0;
    std::int32_t durationType = 0; // DURATION_TYPE_TEMPORARY 1, PERMANENT 2.
    float remaining = 0.0f;
    float interval = 0.0f;         // Regenerate: seconds until the next tick.
    std::int32_t pool = 0;         // Temporary HP left, or a resistance's damage limit.
    std::uint32_t creator = script::kObjectInvalid;
    std::int32_t spellId = -1;
};

// Per-creature combat fields retail keeps on the creature (offsets in ScriptCombat.cpp).
struct ActorCombat {
    bool inCombat = false;
    float combatTimer = 0.0f;
    bool roundActive = false;
    float roundTimer = 0.0f;
    std::vector<float> attackTimes;
    std::size_t nextAttack = 0;
    std::uint32_t roundTarget = script::kObjectInvalid;
    std::int32_t roundFeat = -1;
    std::uint32_t attemptedTarget = script::kObjectInvalid;  // +0x504
    std::uint32_t attackTarget = script::kObjectInvalid;     // +0x50c
    std::uint32_t attackedBy = script::kObjectInvalid;       // +0x520
    std::uint32_t attemptedSpell = script::kObjectInvalid;   // +0x524
    std::uint32_t lastHostileTarget = script::kObjectInvalid; // +0x53c
    std::uint32_t lastHostileActor = script::kObjectInvalid;
    std::uint32_t lastSpellTarget = script::kObjectInvalid;
    std::int32_t pendingAction = -1;     // +0x540
    std::int32_t lastAttackAction = -1;  // +0x544
    std::int32_t lastAttackResult = 0;
    std::int32_t lastCombatFeat = -1;
    std::int32_t lastForcePower = -1;
    bool forcePowerSuccess = true;
    std::int32_t lastDamage = 0;
    std::map<std::int32_t, std::int32_t> lastDamageByType;
    std::int32_t npcAiStyle = 0;
    bool plot = false;   // PC only; creatures use Entity::plot.
    bool min1Hp = false; // PC only; creatures use Entity::min1Hp.
    std::int32_t forcePoints = -1; // Live FP of a non-PC; -1 until first read.
};

struct ActorState {
    std::deque<ActorAction> queue;
    bool commandable = true; // Retail keeps this at object +0xe8.
    ScriptContext last;      // GetLast* survive between events, as object fields.
    bool seenPc = false;
    ActorCombat combat;
    std::vector<AppliedEffect> effects;
    std::size_t effectCursor = 0;              // GetFirstEffect / GetNextEffect
    std::map<std::uint32_t, std::uint8_t> perceived; // bit 0 seen, bit 1 heard. retail 00517a20
};

enum class ActorStep { Arrived, Moving, Blocked };

// ---- events ----------------------------------------------------------------

struct ScriptEvent {
    std::uint32_t target = script::kObjectInvalid;
    std::string hook;
    ScriptContext context;
};

// ---- journal ---------------------------------------------------------------

struct JournalEntryRecord {
    bool end = false;
    std::uint32_t textStrref = UINT32_MAX;
    float xpPercentage = 0.0f;
};

struct JournalCategory {
    std::string tag;
    std::int32_t plotIndex = -1;
    std::int32_t xp = 0;
    std::map<std::int32_t, JournalEntryRecord> entries;
};

// ---- module persistence ----------------------------------------------------

struct EntitySnapshot {
    std::size_t index = 0;
    std::uint32_t object = 0;
    bool spawned = false; // Past the base GIT list: recreate from `templateResref`.
    std::string templateResref;
    std::string tag;
    bool dead = false;
    bool open = false;
    bool locked = false;
    bool hostile = false;
    bool hostileOverride = false;
    bool useable = false;
    kotor::Vec3 position;
    float bearing = 0.0f;
    std::int32_t hitPoints = 1;
    std::int32_t maxHitPoints = 1;
    std::uint32_t faction = 0;
    std::string conversation;
    std::vector<std::string> lootItems;
};

struct TimerSnapshot {
    double remaining = 0.0;
    std::uint32_t target = 0;
    std::string source;
    std::int64_t functorId = -1;
    std::shared_ptr<script::Functor> functor;
};

struct ModuleSnapshot {
    std::vector<EntitySnapshot> entities;
    std::map<std::uint32_t, script::GameObject> objects;
    std::vector<MerchantState> merchants;
    std::vector<EncounterState> encounters;
    std::vector<bool> triggerInside;
    std::vector<TimerSnapshot> timers;
    std::map<std::int64_t, std::shared_ptr<script::Functor>> functors;
    std::map<std::uint32_t, ActorState> actors;
    std::uint32_t areaObject = script::kObjectInvalid;
    std::uint32_t moduleObject = script::kObjectInvalid;
};

// World state read back from a save file, applied once the save's module loads.
struct LoadedWorldState {
    std::map<std::string, ModuleSnapshot> snapshots;
    std::map<std::uint32_t, script::GameObject> carried;
    std::map<std::string, std::array<float, 6>> locationGlobals;
    std::vector<std::pair<std::int64_t, EngineRecord>> engine;
    double worldTime = 0.0;
    double clockBaseMs = -1.0; // < 0: the save predates the saved clock.
    float elapsed = 0.0f;
    std::int32_t runScriptVar = -1;
    std::uint32_t nextObject = 1;
    std::int64_t nextFunctor = 1;
};

// One key=value save line. Returns false when the key is not world state.
bool parseWorldStateLine(const std::string& key, const std::string& value, LoadedWorldState& out);

// ---- the action table ------------------------------------------------------

struct ActionCall;
using ActionHandler = std::optional<std::vector<script::Value>> (*)(ActionCall&);

class HandlerTable {
public:
    void add(std::uint16_t id, ActionHandler handler)
    {
        if (id < handlers_.size()) {
            handlers_[id] = handler;
        }
    }
    ActionHandler get(std::uint16_t id) const
    {
        return id < handlers_.size() ? handlers_[id] : nullptr;
    }

private:
    std::array<ActionHandler, kScriptActionCount> handlers_{};
};

// One register function per family file. Each file is the only one its owner
// edits, so adding a handler is a one-line add() in that file.
void registerObjectActions(HandlerTable& table);
void registerEffectActions(HandlerTable& table);
void registerActorActions(HandlerTable& table);
void registerItemActions(HandlerTable& table);
void registerPartyActions(HandlerTable& table);
void registerCombatActions(HandlerTable& table);
void registerWorldStateActions(HandlerTable& table);
void registerEventActions(HandlerTable& table);

// Everything a handler sees: typed argument access, return helpers and the
// session. A handler returns std::nullopt to fall through to the legacy switch
// and then to ActionRuntime.
struct ActionCall {
    GameSession& session;
    script::World& world;
    script::Vm& vm;
    const ScriptArgs& args;
    std::uint16_t id;

    ScriptApi api() const;

    std::uint32_t self() const;
    std::int64_t intArg(std::size_t index, std::int64_t fallback = 0) const;
    double floatArg(std::size_t index, double fallback = 0.0) const;
    std::string stringArg(std::size_t index) const;
    // Missing or OBJECT_SELF (0) resolves to the running script's self.
    std::uint32_t objectArg(std::size_t index) const;
    // As objectArg, but a missing argument is OBJECT_INVALID.
    std::uint32_t objectArgOrInvalid(std::size_t index) const;
    kotor::Vec3 vectorArg(std::size_t index) const;
    std::int64_t engineArg(std::size_t index) const; // 0 when absent.
    std::int64_t functorArg(std::size_t index) const; // -1 when absent.
    bool hasArg(std::size_t index) const;

    static std::vector<script::Value> none() { return {}; }
    static std::vector<script::Value> retInt(std::int64_t value)
    {
        return {script::Value::makeInt(value)};
    }
    static std::vector<script::Value> retFloat(double value)
    {
        return {script::Value::makeFloat(value)};
    }
    static std::vector<script::Value> retString(std::string value)
    {
        return {script::Value::makeString(std::move(value))};
    }
    static std::vector<script::Value> retObject(std::uint32_t value)
    {
        return {script::Value::makeObject(value)};
    }
    static std::vector<script::Value> retEngine(std::int64_t value)
    {
        return {script::Value::makeEngine(value)};
    }
    static std::vector<script::Value> retVector(kotor::Vec3 value)
    {
        return {script::Value::makeFloat(value.x), script::Value::makeFloat(value.y),
                script::Value::makeFloat(value.z)};
    }
};

// The only door from family files into GameSession's private state.
struct ScriptApi {
    GameSession& session;

    WorldSim& sim() const;
    script::World& world() const;
    kotor::Scene& scene() const;
    std::uint32_t pc() const;
    std::uint32_t contextObject(const std::string& name) const;
    std::uint32_t self() const { return contextObject("self"); }
    const ScriptContext& context() const;

    // Object <-> entity. entityFor returns size_t(-1) for objects without one
    // (the PC, areas, the module, items).
    std::size_t entityFor(std::uint32_t object) const;
    std::uint32_t objectFor(std::size_t entity) const;
    bool isCreature(std::uint32_t object) const; // The PC or a creature entity.
    bool isAlive(std::uint32_t object) const;

    // Position of any object; the PC's lives in GameSession::position_ and these
    // hide the difference. Facing here is radians (entity bearing).
    kotor::Vec3 position(std::uint32_t object) const;
    float facing(std::uint32_t object) const;
    void setPosition(std::uint32_t object, kotor::Vec3 position) const;
    void setPosition(std::uint32_t object, kotor::Vec3 position, float facing) const;

    // Per-object action queue.
    ActorState& actor(std::uint32_t object) const;
    // Appends to the object's queue; drops the action (returning false) when the
    // object is not commandable, as retail does (0057cb10).
    bool enqueue(std::uint32_t object, ActorAction action) const;
    bool enqueueFront(std::uint32_t object, ActorAction action) const;
    void clearQueue(std::uint32_t object) const;
    // One walkmesh step. All scripted movement goes through this one call.
    ActorStep step(std::uint32_t object, kotor::Vec3 target, float speed, float dt,
                   float stopDistance) const;
    float moveSpeed(std::uint32_t object, bool run) const;
    // How close `self` must get to `target`: retail 004ee440 floors fRange by
    // personal space and, for creatures, both bodies.
    float approachRange(std::uint32_t self, std::uint32_t target, float range) const;
    bool hasLineOfSight(std::uint32_t a, std::uint32_t b) const;

    // Events: raiseHook defers to the next event pass and dispatches with the
    // right OBJECT_SELF and context. Logical names are resolved per object kind
    // (UserDefined, Damaged, Attacked, EndRound, Notice, Disturbed, SpellAt,
    // Dialogue, Death); a literal label such as Mod_OnAcquirItem also works.
    void raiseHook(std::uint32_t target, std::string hook, ScriptContext context = {}) const;
    // Runs the hook now. Returns false when the object has no such script.
    bool dispatchHook(std::uint32_t target, const std::string& hook,
                      const ScriptContext& context) const;

    std::optional<script::Value> runScript(const std::string& resref, std::uint32_t self,
                                           const ScriptContext& context) const;
    bool runFunctor(std::int64_t functor, std::uint32_t self) const;
    void dropFunctor(std::int64_t functor) const;

    bool startConversation(std::uint32_t owner, const std::string& resref) const;
    bool dialogueActive() const;
    void setDialoguePaused(bool paused) const;

    bool playAnimation(std::size_t entity, const std::string& name, bool loop) const;
    void stopAnimation(std::size_t entity) const;
    std::string animationName(std::int32_t animation) const;
    bool animationLooping(std::int32_t animation) const;

    // Spawns a creature template as a live, script-visible object.
    std::uint32_t spawnCreature(const std::string& resref, kotor::Vec3 position,
                                float facing) const;
    bool playerStealthed() const;
    std::array<float, 6>* globalLocation(const std::string& name) const;
    void setGlobalLocation(const std::string& name, const std::array<float, 6>& value) const;
    void log(std::string line) const;
    // The mounted install (2DA tables, module IFO).
    kotor::Install& install() const;
};

// ---- world state (ScriptWorldState.cpp) ------------------------------------

// SetGlobalFadeOut / SetGlobalFadeIn: the renderer reads opacity() each frame.
struct FadeState {
    enum class Mode { None, Out, In };
    Mode mode = Mode::None;
    float wait = 0.0f;
    float length = 0.0f;
    float elapsed = 0.0f;
    std::array<float, 3> color{0.0f, 0.0f, 0.0f};
    float opacity = 0.0f; // 0 clear .. 1 solid
    bool active() const { return mode != Mode::None; }
};

// RevealMap: a disc of the area map made visible. radius < 0 reveals the whole map.
struct MapRevealRecord {
    std::string module;
    kotor::Vec3 point;
    std::int32_t radius = -1;
};

// Galaxy map state (planetary.2da rows). Selection is set by the UI or the bot.
constexpr std::size_t kPlanetSlots = 32;
struct GalaxyMapState {
    std::array<bool, kPlanetSlots> available{};
    std::array<bool, kPlanetSlots> selectable{};
    std::int32_t selected = -1;    // GetSelectedPlanet
    std::int32_t highlighted = -1; // ShowGalaxyMap(nPlanet)
    bool open = false;
};

// What a click on a door, placeable or trigger did.
enum class UseOutcome {
    Invalid,    // not a usable object
    OutOfRange, // no walkable path within reach
    Refused,    // plot-protected, destroyed, or the player is not in control
    Locked,     // locked and not opened: OnFailToOpen ran
    Opened,
    Closed,
    Transition, // a module change was started
    Looted,
    Used,
};

// Per-tick drivers, called from GameSession::update.
void tickWorldState(ScriptApi api, float dt);
// The world-state family gate, behind --family-test worldstate.
int runWorldStateFamilyTest(GameSession& game);
// The items and stores family gate, behind --family-test items.
int runItemsFamilyTest(GameSession& game);
// The object-queries family gate, behind --family-test objects.
int runObjectsFamilyTest(GameSession& game);
void tickActors(ScriptApi api, float dt);
void tickEvents(ScriptApi api);
void tickHeartbeats(ScriptApi api, float dt);
void tickPerception(ScriptApi api, float dt);

// ---- combat and effects (ScriptCombat.cpp, ScriptEffects.cpp) ---------------

// ActorActionKind::Attack and CastSpell, run from the actor's queue. Both return
// what runFrontAction returns: true when another action may run this tick.
bool runAttackAction(ScriptApi api, std::uint32_t self, float dt);
bool runCastAction(ScriptApi api, std::uint32_t self, float dt);
void tickCombat(ScriptApi api, float dt);
void tickScriptEffects(ScriptApi api, float dt);
// The one damage path. Plot, Min1HP, temporary HP and resistances apply here; it
// raises Damaged and routes a kill through onCreatureKilled or the PC death path.
void applyScriptDamage(ScriptApi api, std::uint32_t target, std::int32_t amount,
                       std::int32_t damageType, std::uint32_t attacker);
// Retail reputation bands (0057cb80): enemy <= 10, friend >= 90.
int reputationBetween(ScriptApi api, std::uint32_t source, std::uint32_t target);
bool isEnemy(ScriptApi api, std::uint32_t source, std::uint32_t target);
bool creatureHasPerceived(ScriptApi api, std::uint32_t observer, std::uint32_t target,
                          std::uint8_t bits);
bool isDebilitated(ScriptApi api, std::uint32_t object);
bool isDisablingEffect(std::int32_t type);
void healCreature(ScriptApi api, std::uint32_t object, std::int32_t amount);
void killCreature(ScriptApi api, std::uint32_t object, std::uint32_t killer);
void resurrectCreature(ScriptApi api, std::uint32_t object);
void changeForcePoints(ScriptApi api, std::uint32_t object, std::int32_t delta);
// Sum of arg 0 of every held effect of `type` (arg `arg` for two-argument ones).
std::int32_t effectTotal(ScriptApi api, std::uint32_t object, std::int32_t type,
                         std::size_t arg = 0);
bool hasEffectOfType(ScriptApi api, std::uint32_t object, std::int32_t type);
// The combat family gate, behind --family-test combat.
int runCombatFamilyTest(GameSession& game);

// ---- session state ---------------------------------------------------------

struct WorldSim {
    WorldSim();

    HandlerTable handlers;
    EngineStore engine;
    std::map<std::uint32_t, ActorState> actors;
    std::deque<ScriptEvent> events;
    // Retail's global DAT_00832828: set by ExecuteScript, never restored (00535b70).
    std::int32_t runScriptVar = -1;

    // Combat family. TalentSpell and friends hand out one record per talent.
    std::map<std::pair<std::int32_t, std::int32_t>, std::int64_t> talents;
    std::int32_t partyAiStyle = 0; // Set/GetPartyAIStyle
    std::int32_t stealthXpCurrent = 0;
    std::int32_t stealthXpMax = 0;
    std::int32_t stealthXpDecrement = 0;
    bool stealthXpEnabled = false;
    std::int32_t combatRoundCounter = 0;

    bool journalLoaded = false;
    std::map<std::string, JournalCategory> journal; // Keyed by lower-case tag.
    bool plotLoaded = false;
    std::optional<kotor::TwoDa> plot;

    std::map<std::string, ModuleSnapshot> snapshots;
    std::map<std::uint32_t, script::GameObject> carried;
    std::uint32_t areaObject = script::kObjectInvalid;
    std::uint32_t moduleObject = script::kObjectInvalid;
    std::size_t baseEntityCount = 0;
    std::vector<double> heartbeatDue;

    int scriptDepth = 0;
    float perceptionTimer = 0.0f;
    std::string pendingLinkedTo;
    std::vector<std::string> pendingMovies;
    bool dialoguePaused = false;
    bool noStartLink = false;
    bool restoringSave = false;

    std::map<std::string, std::shared_ptr<const std::vector<script::Instruction>>> decoded;
    std::map<std::string, std::vector<std::uint8_t>> injectedScripts;

    // World-state family. Cutscene and UI state lives here, not in the renderer.
    FadeState fade;
    std::map<std::int32_t, std::string> customTokens; // SetCustomToken
    GalaxyMapState galaxy;
    std::set<std::string> mapPinsEnabled;              // lower-case waypoint tags
    std::vector<MapRevealRecord> mapReveals;
    float noClicksRemaining = 0.0f;                    // NoClicksFor
    bool restrictMode = false;                         // SetPlayerRestrictMode
    float restrictHeld = 0.0f;                         // seconds restrictMode has been on
    bool areaUnescapable = false;                      // SetAreaUnescapable
    std::int32_t cameraMode = 0;                       // SetCameraMode
    std::set<std::uint32_t> listening;                 // SetListening
    std::map<std::uint32_t, std::vector<std::pair<std::string, std::int32_t>>> listenPatterns;
    std::set<std::uint32_t> lockOrientation;           // SetLockOrientationInDialog
    float cameraFacing = 0.0f;                         // SetCameraFacing, degrees
    bool cameraFacingSet = false;
    float trapScanTimer = 0.0f;                        // passive trap detection cadence

    // Game clock (ScriptObjects.cpp). Time of day is `clockBaseMs` plus the world
    // time in milliseconds, modulo a day of 24 * clockMinPerHour * 60000 ms: retail's
    // clock counts real minutes per game hour (Mod_MinPerHour).
    bool clockReady = false;
    double clockBaseMs = 0.0;
    std::int32_t clockMinPerHour = 2;
    std::int32_t clockDawnHour = 0;
    std::int32_t clockDuskHour = 0;
    std::string clockModule; // The module whose IFO supplied the dawn and dusk hours.
};

} // namespace ke
