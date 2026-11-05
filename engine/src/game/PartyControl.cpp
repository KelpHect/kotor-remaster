// Companion control: which party creature the player steers.
//
// Retail keeps the party table's controlled-NPC field (PT_CONTROLLED_NP) and the
// server's player list holds the controlled creature: GetFirstPC walks that list
// (retail 0053c920 -> 004b4c50) and GetIsPC asks whether the object is on it
// (retail 0053f1c0 -> 004b2a50), so a companion that is switched to is the
// "PC" every script sees, and the original PC is not. The switch itself is
// retail 005667c0 (party table SwitchPlayerCharacter): it clears per-creature
// player state on the old body and hands the player to the new one. What that
// state is was not recovered; this file clears the player's own queued commands.
// The Tab key is keymap.2da's ChangeChar row (Tab); SelectPrev / SelectNext (Q, E)
// are target selection, not party selection.
//
// Everything below the retail facts is an engine rule and labelled:
//   - the PC's body stays in position_ / playerHp_ and follows like a member;
//   - control passes to a living creature when the controlled one falls;
//   - an uncontrolled PC defends itself (retail runs the party AI there).

#include "game/GameSession.h"
#include "game/Pathfinding.h"
#include "game/ScriptHost.h"
#include "core/Log.h"
#include "kotor/SaveGame.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ke {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

bool validNpcId(int npc)
{
    return npc >= 0 && npc < kNpcCount;
}

// A tiny compiled script for the gate's probe hooks: constants, action calls, return.
class ProbeScript {
public:
    ProbeScript& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    ProbeScript& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    std::vector<std::uint8_t> bytes()
    {
        op(0x20, 0x00);
        const std::string magic = "NCS V1.0";
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        out.push_back(0x42);
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code_.size());
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

private:
    void op(std::uint8_t code, std::uint8_t qualifier)
    {
        code_.push_back(code);
        code_.push_back(qualifier);
    }
    std::vector<std::uint8_t> code_;
};

} // namespace

// ---- who is controlled -------------------------------------------------------------

bool GameSession::companionControlled() const
{
    // controlledObject() falls back to the PC when the companion has no body in
    // this module, so a missing body never leaves the player controlling nothing.
    return controlledObject() != scriptWorld_.pc;
}

std::size_t GameSession::controlledEntity() const
{
    if (companionControlled()) {
        return entityOfObject(controlledObject());
    }
    return playerEntity_ < scene_.entities.size() ? playerEntity_ : kNone;
}

kotor::Vec3 GameSession::controlledPosition() const
{
    if (companionControlled()) {
        if (const std::size_t index = controlledEntity(); index != kNone) {
            return scene_.entities[index].position;
        }
    }
    return position_;
}

float GameSession::controlledBearing() const
{
    if (companionControlled()) {
        if (const std::size_t index = controlledEntity(); index != kNone) {
            return scene_.entities[index].bearing;
        }
    }
    return thirdPerson_ ? playerBearing_ : yaw_;
}

std::int32_t GameSession::controlledHp() const
{
    if (companionControlled()) {
        if (const std::size_t index = controlledEntity(); index != kNone) {
            return scene_.entities[index].hitPoints;
        }
    }
    return playerHp_;
}

std::int32_t GameSession::controlledMaxHp() const
{
    if (companionControlled()) {
        if (const std::size_t index = controlledEntity(); index != kNone) {
            return scene_.entities[index].maxHitPoints;
        }
    }
    return playerMaxHp_;
}

bool GameSession::controlledDead() const
{
    if (companionControlled()) {
        const std::size_t index = controlledEntity();
        return index == kNone || scene_.entities[index].dead
            || scene_.entities[index].hitPoints <= 0;
    }
    return playerDead_;
}

// ---- choosing who to control -----------------------------------------------------------

std::vector<int> GameSession::selectableLeaders() const
{
    std::vector<int> out;
    if (!playerDead_) {
        out.push_back(kNpcPlayer);
    }
    for (const int npc : party_.members) {
        const PartyNpc& slot = party_.npcs[static_cast<std::size_t>(npc)];
        // PT_NPC_SELECT: nonzero is selectable in the party GUI (see Party.h). A
        // dead creature cannot be selected (engine rule, as the party death rule).
        if (!slot.selectable() || partyMemberDown(npc)) {
            continue;
        }
        const std::size_t index = entityOfObject(slot.object);
        if (index == kNone || scene_.entities[index].dead) {
            continue;
        }
        out.push_back(npc);
    }
    return out;
}

bool GameSession::selectLeaderNpc(int npc)
{
    // Engine rules: no switching while solo mode holds the party apart, while a
    // conversation or a cutscene has the controls, or at a menu-less game over.
    if (party_.solo || dialogueView_.active || controlLocked()) {
        return false;
    }
    const std::vector<int> choices = selectableLeaders();
    if (std::find(choices.begin(), choices.end(), npc) == choices.end()) {
        return false;
    }
    if (npc == party_.controlled) {
        return true;
    }
    return setPartyLeaderNpc(npc);
}

bool GameSession::cycleLeader()
{
    const std::vector<int> choices = selectableLeaders();
    if (choices.size() < 2) {
        return false;
    }
    const auto here = std::find(choices.begin(), choices.end(), party_.controlled);
    const std::size_t next = here == choices.end()
        ? 0
        : static_cast<std::size_t>(here - choices.begin() + 1) % choices.size();
    return selectLeaderNpc(choices[next]);
}

// ---- the switch -------------------------------------------------------------------------

void GameSession::showControlledBodies()
{
    const auto show = [&](std::size_t entity, bool visible) {
        if (entity == kNone || entity >= scene_.entities.size()) {
            return;
        }
        for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (mesh.entity == entity) {
                mesh.visible = visible;
            }
        }
    };
    show(playerEntity_, !scene_.entities.empty() && playerEntity_ < scene_.entities.size()
                            && !scene_.entities[playerEntity_].dead);
    for (const int npc : party_.members) {
        const std::size_t index = entityOfObject(party_.npcs[static_cast<std::size_t>(npc)].object);
        show(index, !party_.solo || npc == party_.controlled);
    }
    playerVisible_ = true;
    ++animationRevision_;
}

void GameSession::onControlChanged(int previous, int next)
{
    ScriptApi api{*this};
    const auto objectOf = [&](int npc) -> std::uint32_t {
        if (validNpcId(npc)) {
            const std::uint32_t object = party_.npcs[static_cast<std::size_t>(npc)].object;
            if (scriptWorld_.get(object) != nullptr) {
                return object;
            }
        }
        return scriptWorld_.pc;
    };
    // Retail 005667c0 clears player state on the creature being left; what it
    // clears was not recovered. Here the commands the player gave (a click walk,
    // an attack order) end with the control that gave them; scripted actions stay.
    for (const int npc : {previous, next}) {
        const std::uint32_t object = objectOf(npc);
        ActorState& actor = api.actor(object);
        for (auto it = actor.queue.begin(); it != actor.queue.end();) {
            if (it->userIssued) {
                if (it->functor >= 0) {
                    api.dropFunctor(it->functor);
                }
                it = actor.queue.erase(it);
            } else {
                ++it;
            }
        }
        if (nav_) {
            nav_->actors.erase(object);
        }
    }
    combatTarget_ = UINT32_MAX;
    moveOutcome_ = MoveOutcome::None;
    moveBlockingDoor_ = kNone;
    verticalVelocity_ = 0.0f;
    // A member that was spawned or left off the walkmesh stands on it before it is
    // steered (the followers walk themselves back on; the player's input cannot).
    if (companionControlled() && !scene_.entities.empty() && !module_.empty()) {
        if (nav::NavMesh* mesh = navMesh(); mesh != nullptr && !mesh->empty()) {
            const std::size_t body = controlledEntity();
            if (body != kNone && mesh->locate(scene_.entities[body].position) < 0) {
                kotor::Vec3 snapped{};
                if (mesh->nearest(scene_.entities[body].position, snapped) >= 0) {
                    scene_.entities[body].position = snapped;
                    syncEntityToScriptWorld(body);
                }
            }
        }
    }
    // The camera sits behind the body it now follows.
    yaw_ = controlledBearing();
    // Standing in a trigger when control arrives is not entering it.
    triggerInside_.assign(scene_.triggers.size(), false);
    const kotor::Vec3 at = controlledPosition();
    for (std::size_t i = 0; i < scene_.triggers.size(); ++i) {
        triggerInside_[i] = pointInTrigger(scene_.triggers[i], at.x, at.y);
    }
    showControlledBodies();
    logEvent("control " + (validNpcId(next) ? party_.npcs[static_cast<std::size_t>(next)].tag
                                            : std::string("PC")));
}

// Engine rule (retail's party-death handling was not found): when the creature the
// player steers falls, the player gets the first living member, PC first.
void GameSession::passControlIfDown()
{
    if (!controlledDead()) {
        return;
    }
    if (!playerDead_ && party_.controlled != kNpcPlayer) {
        setPartyLeaderNpc(kNpcPlayer);
        logEvent("control passes to the PC (engine rule)");
        return;
    }
    for (const int npc : party_.members) {
        if (npc == party_.controlled || partyMemberDown(npc)) {
            continue;
        }
        const std::size_t index = entityOfObject(party_.npcs[static_cast<std::size_t>(npc)].object);
        if (index == kNone || scene_.entities[index].dead) {
            continue;
        }
        setPartyLeaderNpc(npc);
        logEvent("control passes to " + party_.npcs[static_cast<std::size_t>(npc)].tag
                 + " (engine rule)");
        return;
    }
}

// Engine rule: retail's party AI also runs on the PC when another creature is
// steered, and the engine has no AI script for the PC. An idle PC that has been
// hit, or that is in combat, attacks whoever last struck it (else the creature the
// steered body is fighting); otherwise it follows.
void GameSession::controlPcDefence()
{
    if (!companionControlled() || playerDead_) {
        return;
    }
    ScriptApi api{*this};
    ActorState& pc = api.actor(scriptWorld_.pc);
    if (!pc.queue.empty() || !pc.combat.inCombat || dialogueView_.active) {
        return;
    }
    std::uint32_t target = pc.combat.lastHostileActor;
    if (target == script::kObjectInvalid || target == 0 || !api.isAlive(target)
        || target == scriptWorld_.pc || api.entityFor(target) == kNone) {
        const std::uint32_t lead = controlledObject();
        const ActorState& leader = api.actor(lead);
        target = leader.combat.attackTarget;
    }
    if (target == script::kObjectInvalid || target == 0 || !api.isAlive(target)
        || api.entityFor(target) == kNone) {
        return;
    }
    ActorAction attack;
    attack.kind = ActorActionKind::Attack;
    attack.object = target;
    api.enqueue(scriptWorld_.pc, std::move(attack));
}

// ---- the steered companion's body -----------------------------------------------------------

// The keyboard drives the companion the way it drives the PC: the camera's yaw
// gives the heading, a steer drops what was queued, the walkmesh slides it along
// walls. The PC's body is not touched.
void GameSession::stepControlledCompanion(float dt, const InputState& input)
{
    const std::size_t index = controlledEntity();
    if (index == kNone) {
        return;
    }
    ScriptApi api{*this};
    const std::uint32_t self = controlledObject();
    ActorState& actor = api.actor(self);
    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    if (!controlledDead() && !dialogueView_.active && actor.commandable && !controlLocked()) {
        if (input.forward) forwardAxis += 1.0f;
        if (input.back) forwardAxis -= 1.0f;
        if (input.right) rightAxis += 1.0f;
        if (input.left) rightAxis -= 1.0f;
        if ((forwardAxis != 0.0f || rightAxis != 0.0f) && !actor.queue.empty()) {
            api.clearQueue(self);
        }
    }
    const float cosYaw = std::cos(yaw_);
    const float sinYaw = std::sin(yaw_);
    kotor::Vec3 desired{cosYaw * forwardAxis + sinYaw * rightAxis,
                        sinYaw * forwardAxis - cosYaw * rightAxis, 0.0f};
    const float magnitude = std::sqrt(desired.x * desired.x + desired.y * desired.y);
    if (magnitude <= 1.0e-5f) {
        return;
    }
    desired = desired * (1.0f / magnitude);
    const float speed = creatureMoveRate(self, input.run);
    kotor::Entity& body = scene_.entities[index];
    const kotor::Vec3 start = body.position;
    bool steered = false;
    const kotor::Vec3 end = slidePlayer(
        start, {start.x + desired.x * speed * dt, start.y + desired.y * speed * dt, start.z},
        steered, creatureRadius(self));
    body.position = end;
    body.bearing = std::atan2(desired.y, desired.x);
    syncEntityToScriptWorld(index);
}

// ---- the family gate (--family-test companion) --------------------------------------------
//
// Everything below goes through GameSession::update() with an InputState, the way the
// window drives it, on the shipped Endar Spire module (and one module change).

int runCompanionFamilyTest(GameSession& game)
{
    using Vec3 = kotor::Vec3;
    constexpr std::size_t kNoEntity = static_cast<std::size_t>(-1);
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("companion family: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto note = [&](const std::string& line) { logInfo("companion family: " + line); };
    constexpr float kFrame = 1.0f / 60.0f;
    const auto flat = [](Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); };
    // Shipped scripts open real conversations while the module runs; a conversation holds
    // the controls, so the gate aborts them (as the playthrough gate does) unless it is
    // the one that started it.
    bool keepDialogue = false;
    int lastControlled = game.partyTable().controlled;
    const auto step = [&](GameSession& session, int frames, const InputState& input = InputState{}) {
        for (int i = 0; i < frames; ++i) {
            session.update(kFrame, input);
            if (&session != &game) {
                continue;
            }
            if (!keepDialogue && session.dialogue().active) {
                note(concat("aborted a shipped conversation with ", session.dialogue().npcTag));
                session.endDialogue();
            }
            if (session.partyTable().controlled != lastControlled) {
                note(concat("control moved ", lastControlled, " -> ", session.partyTable().controlled));
                const auto& log = session.combatLog();
                for (std::size_t n = log.size() > 6 ? log.size() - 6 : 0; n < log.size(); ++n) {
                    note("  log " + log[n]);
                }
                const auto& actions = session.actionTrace();
                for (std::size_t n = actions.size() > 6 ? actions.size() - 6 : 0; n < actions.size(); ++n) {
                    note(concat("  action ", actions[n].id, " ", actions[n].name, " ", actions[n].kind));
                }
                lastControlled = session.partyTable().controlled;
            }
        }
    };
    const auto eventSeen = [](const GameSession& session, const std::string& needle) {
        for (const std::string& line : session.combatLog()) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    script::Vm dummy({});
    using Args = std::vector<std::vector<script::Value>>;
    const auto O = [](std::uint32_t v) {
        return std::vector<script::Value>{script::Value::makeObject(v)};
    };
    const auto run = [&](std::uint16_t id, Args args) {
        const auto result = game.action(id, args, game.scriptWorld(), dummy);
        return result.has_value() ? *result : std::vector<script::Value>{};
    };
    const auto ro = [&](std::uint16_t id, Args args) -> std::uint32_t {
        const auto cells = run(id, std::move(args));
        return cells.empty() ? script::kObjectInvalid : static_cast<std::uint32_t>(cells[0].integer);
    };
    const auto ri = [&](std::uint16_t id, Args args) -> std::int64_t {
        const auto cells = run(id, std::move(args));
        return cells.empty() ? std::int64_t{-9999} : cells[0].integer;
    };

    ScriptApi api{game};
    const std::uint32_t pc = game.scriptWorld().pc;
    nav::NavMesh* mesh = game.navMesh();
    if (mesh == nullptr || mesh->empty()) {
        logError("companion family: no walkmesh");
        return 40;
    }
    // The PC stands still for a stretch (a scripted Wait keeps the follow code off it),
    // so a PC that moved can only have been moved by the input.
    const auto holdPc = [&](float seconds) {
        api.clearQueue(pc);
        ActorAction wait;
        wait.kind = ActorActionKind::Wait;
        wait.remaining = seconds;
        api.enqueue(pc, std::move(wait));
    };
    // The heading with the longest clear walkmesh run from a point.
    const auto openHeading = [&](GameSession& session, Vec3 from, float want, float& length) {
        float bestYaw = 0.0f;
        length = -1.0f;
        nav::NavMesh* current = session.navMesh();
        const int tri = current->locate(from);
        for (int k = 0; k < 16; ++k) {
            const float angle = static_cast<float>(k) * 0.3926991f;
            const Vec3 to{from.x + std::cos(angle) * want, from.y + std::sin(angle) * want, from.z};
            const nav::RayResult ray = current->ray(from, tri, to, session.nav_->closed);
            const float reach = flat(from, ray.end);
            if (reach > length) {
                length = reach;
                bestYaw = angle;
            }
        }
        return bestYaw;
    };
    // The click walk of the steered creature: a route, a door opened where one blocks it.
    const auto walk = [&](Vec3 target, float seconds) {
        for (int leg = 0; leg < 8; ++leg) {
            const Vec3 here = game.controlledPosition();
            if (flat(here, target) < 0.6f) {
                return true;
            }
            if (!game.moveTo(target, 0.6f, flat(here, target) > 4.0f)) {
                return false;
            }
            for (int i = 0; i < static_cast<int>(seconds / kFrame) && game.movePending(); ++i) {
                step(game, 1);
            }
            if (flat(game.controlledPosition(), target) < 1.2f) {
                return true;
            }
            if (game.moveOutcome() == GameSession::MoveOutcome::DoorBlocked) {
                game.openDoorAsPlayer(game.moveBlockingDoor());
            }
        }
        return flat(game.controlledPosition(), target) < 1.2f;
    };
    const auto triCentre = [&](const nav::Tri& tri) {
        return (tri.v[0] + tri.v[1] + tri.v[2]) * (1.0f / 3.0f);
    };

    // ---- the party: Carth and Bastila follow the PC --------------------------------------------
    check(!game.companionControlled() && game.partyTable().controlled == kNpcPlayer
              && ro(548, {}) == pc && ri(217, {O(pc)}) == 1,
          "pc_leads_at_start");
    check(game.addPartyMember("Carth"), "carth_joined");
    check(game.addPartyMember("Bastila"), "bastila_joined");
    game.setThirdPerson(true);
    step(game, 3);
    const std::uint32_t carth = game.partyTable().npcs[kNpcCarth].object;
    const std::uint32_t bastila = game.partyTable().npcs[kNpcBastila].object;
    const std::size_t carthEntity = api.entityFor(carth);
    const std::size_t bastilaEntity = api.entityFor(bastila);
    check(carth != script::kObjectInvalid && carthEntity != kNoEntity
              && bastila != script::kObjectInvalid && bastilaEntity != kNoEntity,
          "members_have_bodies");
    if (carthEntity == kNoEntity || bastilaEntity == kNoEntity) {
        return 41;
    }
    const std::string carthTag = game.scene().entities[carthEntity].tag;
    const auto state = [&](const char* where) {
        const kotor::Entity& body = game.scene().entities[carthEntity];
        std::string choices;
        for (const int npc : game.selectableLeaders()) {
            choices += std::to_string(npc) + " ";
        }
        note(concat("state ", where, ": controlled=", game.partyTable().controlled,
                    " solo=", game.soloMode() ? 1 : 0, " dialogue=", game.dialogue().active ? 1 : 0,
                    " locked=", game.controlLocked() ? 1 : 0, " choices=", choices,
                    " carth tri=", mesh->locate(body.position), " hp=", body.hitPoints,
                    " dead=", body.dead ? 1 : 0, " at ", body.position.x, ",", body.position.y, ",",
                    body.position.z, " pc ", game.position().x, ",", game.position().y, ",",
                    game.position().z, " pc tri=", mesh->locate(game.position())));
    };
    {
        const std::vector<int> choices = game.selectableLeaders();
        check(choices.size() == 3 && choices[0] == kNpcPlayer, "selectable_pc_then_members");
    }

    // ---- switching: the input, the PC's own state, the camera ------------------------------------
    const std::int32_t pcHpBefore = game.playerHp();
    check(game.selectLeaderNpc(kNpcCarth), "select_carth");
    check(game.companionControlled() && game.controlledObject() == carth
              && game.partyTable().controlled == kNpcCarth && game.partyTable().leader == kNpcCarth,
          "carth_controlled");
    // Retail's player list holds the steered creature (0053c920, 0053f1c0).
    check(ro(548, {}) == carth && ri(217, {O(carth)}) == 1 && ri(217, {O(pc)}) == 0
              && ro(238, {}) == carth,
          "scripts_see_carth_as_the_pc");
    check(game.playerHp() == pcHpBefore, "pc_hp_kept");
    state("after switch");
    {
        float length = 0.0f;
        const Vec3 start = game.scene().entities[carthEntity].position;
        game.yaw_ = openHeading(game, start, 8.0f, length);
        const Vec3 pcBefore = game.position();
        const SceneCamera eyeBefore = game.camera();
        holdPc(5.0f);
        InputState forward;
        forward.forward = true;
        forward.run = true;
        step(game, 90, forward);
        const Vec3 carthAfter = game.scene().entities[carthEntity].position;
        const float moved = flat(start, carthAfter);
        const float want = std::min(2.0f, 0.5f * length);
        note(concat("input run length=", length, " carth moved=", moved,
                    " pc moved=", flat(pcBefore, game.position())));
        check(moved >= want && moved > 0.5f, "forward_input_moves_the_companion");
        check(flat(pcBefore, game.position()) < 0.01f, "forward_input_leaves_the_pc");
        const SceneCamera eye = game.camera();
        const float eyeToCarth = flat(eye.eye, carthAfter);
        note(concat("camera eye to carth=", eyeToCarth, " eye moved=", flat(eyeBefore.eye, eye.eye)));
        check(eyeToCarth <= 3.4f && flat(eyeBefore.eye, eye.eye) >= 0.5f * moved,
              "camera_tracks_the_companion");
        // In this corridor the wall collision pulls the camera into the body (first-person
        // fallback); with the clip off the orbit shows, and it circles the companion.
        game.setCameraNoClip(true);
        step(game, 1);
        const float orbit = flat(game.camera().eye, game.scene().entities[carthEntity].position);
        note(concat("orbit distance to carth=", orbit));
        check(orbit > 2.5f && orbit <= 3.4f, "third_person_orbit_circles_the_companion");
        game.setCameraNoClip(false);
        step(game, 1);
        api.clearQueue(pc);
    }

    // ---- the PC follows by itself --------------------------------------------------------------------
    {
        const Vec3 carthAt = game.scene().entities[carthEntity].position;
        Vec3 target = carthAt;
        bool found = false;
        for (const nav::Tri& tri : mesh->tris()) {
            const Vec3 centre = triCentre(tri);
            const float apart = flat(centre, game.position());
            if (apart >= 9.0f && apart <= 16.0f && game.pathExists(carthAt, centre)) {
                target = centre;
                found = true;
                break;
            }
        }
        check(found, "follow_target_found");
        const Vec3 pcStart = game.position();
        float travelled = 0.0f;
        float maxStep = 0.0f;
        const float limit = game.creatureMoveRate(pc, true) * 1.5f;
        Vec3 last = pcStart;
        const auto trackPc = [&]() {
            const float stepLength = flat(last, game.position());
            travelled += stepLength;
            maxStep = std::max(maxStep, stepLength);
            last = game.position();
        };
        if (found && game.moveTo(target, 0.6f, true)) {
            for (int i = 0; i < 1800 && game.movePending(); ++i) {
                step(game, 1);
                trackPc();
            }
        }
        for (int i = 0; i < 1200; ++i) {
            step(game, 1);
            trackPc();
            if (i > 120
                && flat(game.position(), game.scene().entities[carthEntity].position) < 3.2f) {
                break;
            }
        }
        const float gap = flat(game.position(), game.scene().entities[carthEntity].position);
        note(concat("pc followed travelled=", travelled, " max step=", maxStep, " gap=", gap,
                    " carth to target=", flat(game.scene().entities[carthEntity].position, target)));
        check(travelled >= 5.0f && gap < 3.2f && maxStep <= limit * kFrame * 1.05f + 1.0e-3f,
              "pc_follows_across_the_room");
    }

    // ---- a trigger fires for the creature the player steers ---------------------------------------------
    state("before trigger");
    {
        const std::size_t trigger = game.findEntity("end_trig02");
        check(trigger != kNoEntity, "trigger_fixture");
        if (trigger != kNoEntity) {
            Vec3 centre{};
            float count = 0.0f;
            for (const kotor::TriggerVolume& volume : game.scene().triggers) {
                if (volume.entity != trigger) {
                    continue;
                }
                for (const Vec3& vertex : volume.vertices) {
                    centre = centre + vertex;
                    count += 1.0f;
                }
            }
            centre = count > 0.0f ? centre * (1.0f / count) : game.scene().entities[trigger].position;
            if (const int tri = mesh->locate(centre); tri >= 0) {
                centre.z = mesh->height(tri, centre.x, centre.y);
            }
            // What a trigger script sees: the entering object, and whether it is the player.
            game.addScriptForTest("cc_enter",
                                  ProbeScript()
                                      .action(25, 0).action(272, 1).constString("CC_ENTER").action(160, 2)
                                      .action(25, 0).action(217, 1).constString("CC_ISPC").action(581, 2)
                                      .bytes());
            std::string& hook = game.scene().entities[trigger].scripts["ScriptOnEnter"];
            const std::string shipped = hook;
            hook = "cc_enter";
            const auto reset = [&]() {
                game.scriptWorld().setGlobal("CC_ISPC", script::Value::makeInt(-1));
                game.scriptWorld().setGlobal("CC_ENTER", script::Value::makeString(""));
            };
            const auto enterer = [&]() {
                return game.scriptWorld().getGlobal("CC_ENTER", script::ValueType::String).text;
            };
            const auto isPc = [&]() { return game.scriptWorld().getGlobal("CC_ISPC").integer; };
            reset();
            // The PC stands in the trigger while Carth is steered: nothing fires for it.
            holdPc(2.0f);
            game.setPositionForTest(centre);
            step(game, 20);
            check(isPc() == -1, "trigger_ignores_the_uncontrolled_pc");
            api.clearQueue(pc);
            const bool arrived = walk(centre, 45.0f);
            note(concat("trigger walk outcome=", static_cast<int>(game.moveOutcome()), " apart=",
                        flat(game.controlledPosition(), centre), " centre tri=", mesh->locate(centre)));
            step(game, 3);
            std::string carthText;
            if (const auto cells = run(272, {O(carth)}); !cells.empty()) {
                carthText = cells[0].text;
            }
            note(concat("trigger walked=", arrived ? 1 : 0, " entering=", enterer(), " carth=", carthText,
                        " isPc=", isPc()));
            check(arrived && isPc() == 1 && enterer() == carthText && !carthText.empty(),
                  "trigger_fires_for_the_controlled_creature");
            state("after trigger");
            // The probe stays on the trigger: the shipped script would hand the controls back
            // to the PC and lock them for a tutorial cutscene, which is not under test here.
            (void)shipped;
            if (game.dialogue().active) {
                game.clearDialogueForTest();
            }
        }
    }

    // ---- a placeable: the companion walks up to it and uses it --------------------------------------------
    {
        const std::size_t locker = game.findEntity("end_locker01");
        check(locker != kNoEntity, "locker_fixture");
        if (locker != kNoEntity) {
            // The walk ends beside the placeable; a side whose straight reach line is blocked
            // by its own hull is skipped (the same reach rule the PC meets).
            const Vec3 lockerAt = game.scene().entities[locker].position;
            bool arrived = false;
            for (int k = 0; k < 8 && !arrived; ++k) {
                const float angle = static_cast<float>(k) * 0.7853982f;
                Vec3 spot{lockerAt.x + std::cos(angle) * 1.2f, lockerAt.y + std::sin(angle) * 1.2f,
                          game.controlledPosition().z};
                const int tri = mesh->locate(spot);
                if (tri < 0) {
                    continue;
                }
                spot.z = mesh->height(tri, spot.x, spot.y);
                if (!game.pathExists(game.controlledPosition(), spot) || !walk(spot, 30.0f)) {
                    continue;
                }
                arrived = game.inReach(locker, 2.5f);
                note(concat("locker side ", k, " reach=", arrived ? 1 : 0));
            }
            const UseOutcome outcome = game.useObject(locker);
            const bool used = outcome == UseOutcome::Used || outcome == UseOutcome::Looted;
            note(concat("locker outcome=", static_cast<int>(outcome), " path=", game.pathDistanceTo(locker),
                        " carth ", game.controlledPosition().x, ",", game.controlledPosition().y, ",",
                        game.controlledPosition().z, " locker ", game.scene().entities[locker].position.x,
                        ",", game.scene().entities[locker].position.y, ",",
                        game.scene().entities[locker].position.z));
            note(concat("locker arrived=", arrived ? 1 : 0, " used=", used ? 1 : 0, " apart=",
                        flat(game.controlledPosition(), game.scene().entities[locker].position),
                        " logged=", eventSeen(game, "use end_locker01") ? 1 : 0));
            check(arrived && used && eventSeen(game, "use end_locker01"), "companion_uses_a_placeable");
            if (game.dialogue().active) {
                game.clearDialogueForTest();
            }
        }
    }

    // ---- talking: the dialogue starts from the companion -----------------------------------------------------
    {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
            const kotor::Entity& entity = game.scene().entities[i];
            if (entity.kind == kotor::EntityKind::Creature && !entity.conversation.empty()
                && !entity.dead && !entity.partyAvatar && !entity.hostile && i != carthEntity
                && i != bastilaEntity) {
                candidates.push_back(i);
            }
        }
        const Vec3 from = game.scene().entities[carthEntity].position;
        std::sort(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) {
            return flat(from, game.scene().entities[a].position)
                < flat(from, game.scene().entities[b].position);
        });
        note(concat("conversation candidates=", candidates.size()));
        bool talked = false;
        for (std::size_t n = 0; n < candidates.size() && n < 4 && !talked; ++n) {
            const kotor::Entity npc = game.scene().entities[candidates[n]];
            if (!walk(npc.position, 40.0f)) {
                continue;
            }
            // Put the PC out of reach of the NPC and hold it there.
            for (const nav::Tri& tri : mesh->tris()) {
                const Vec3 centre = triCentre(tri);
                const float apart = flat(centre, npc.position);
                if (apart > 10.0f && apart < 30.0f && game.pathExists(npc.position, centre)) {
                    game.setPositionForTest(centre);
                    break;
                }
            }
            holdPc(3.0f);
            const bool pcFar = flat(game.position(), npc.position) > 3.0f;
            keepDialogue = true;
            const bool started = game.interact() && game.dialogue().active;
            keepDialogue = false;
            if (!started) {
                api.clearQueue(pc);
                continue;
            }
            talked = true;
            note(concat("dialogue with ", npc.tag, " listener=", game.dialogue().listenerObject,
                        " carth=", carth));
            check(pcFar && game.dialogue().npcTag == npc.tag, "dialogue_started_by_proximity_to_carth");
            check(game.dialogue().listenerObject == carth && ro(238, {}) == carth,
                  "dialogue_listener_is_the_companion");
            game.endDialogue();
            step(game, 3);
            api.clearQueue(pc);
        }
        check(talked, "companion_talks_to_an_npc");
    }

    // ---- solo mode: nobody follows --------------------------------------------------------------------------
    state("before solo");
    {
        game.setSoloMode(true);
        check(!game.selectLeaderNpc(kNpcPlayer) && !game.cycleLeader(), "solo_refuses_switching");
        float length = 0.0f;
        const Vec3 start = game.scene().entities[carthEntity].position;
        game.yaw_ = openHeading(game, start, 6.0f, length);
        const Vec3 pcBefore = game.position();
        const Vec3 bastilaBefore = game.scene().entities[bastilaEntity].position;
        note(concat("solo bastila queued kind=", api.actor(bastila).queue.empty()
                        ? -1 : static_cast<int>(api.actor(bastila).queue.front().kind)));
        api.clearQueue(bastila);
        note(concat("solo bastila queue=", api.actor(bastila).queue.size(), " inCombat=",
                    api.actor(bastila).combat.inCombat ? 1 : 0, " carth inCombat=",
                    api.actor(carth).combat.inCombat ? 1 : 0, " pc inCombat=",
                    api.actor(pc).combat.inCombat ? 1 : 0, " pc queue=", api.actor(pc).queue.size()));
        InputState forward;
        forward.forward = true;
        forward.run = true;
        step(game, 90, forward);
        step(game, 60);
        note(concat("solo carth moved=", flat(start, game.scene().entities[carthEntity].position),
                    " pc moved=", flat(pcBefore, game.position()), " bastila moved=",
                    flat(bastilaBefore, game.scene().entities[bastilaEntity].position),
                    " run length=", length));
        check(flat(start, game.scene().entities[carthEntity].position) > 0.5f
                  && flat(pcBefore, game.position()) < 0.01f
                  && flat(bastilaBefore, game.scene().entities[bastilaEntity].position) < 0.01f,
              "solo_followers_stay_put");
        game.setSoloMode(false);
        step(game, 240);
        check(flat(game.position(), game.scene().entities[carthEntity].position)
                  < flat(pcBefore, game.scene().entities[carthEntity].position),
              "followers_resume_after_solo");
    }

    // ---- Tab: the next living party member; the PC's state is kept ---------------------------------------
    state("before tab");
    {
        const Vec3 pcAt = game.position();
        const std::vector<int> order = game.selectableLeaders();
        check(game.partyTable().controlled == kNpcCarth && order.size() == 3, "tab_starts_on_carth");
        const auto after = [&](int current) {
            const auto here = std::find(order.begin(), order.end(), current);
            return order[static_cast<std::size_t>(here - order.begin() + 1) % order.size()];
        };
        const int expectFirst = after(kNpcCarth);
        check(game.cycleLeader() && game.partyTable().controlled == expectFirst, "tab_next");
        const int expectSecond = after(expectFirst);
        check(game.cycleLeader() && game.partyTable().controlled == expectSecond, "tab_next_again");
        for (int guard = 0; guard < 4 && game.partyTable().controlled != kNpcPlayer; ++guard) {
            game.cycleLeader();
        }
        check(!game.companionControlled() && ro(548, {}) == pc && ri(217, {O(pc)}) == 1
                  && ri(217, {O(carth)}) == 0 && flat(pcAt, game.position()) < 0.5f,
              "tab_back_to_the_pc");
        check(game.selectLeaderNpc(kNpcCarth) && game.controlledObject() == carth,
              "click_selects_carth");
    }

    // ---- an attack command belongs to the companion ------------------------------------------------------------
    {
        std::size_t hostile = kNoEntity;
        const Vec3 from = game.scene().entities[carthEntity].position;
        for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
            const kotor::Entity& entity = game.scene().entities[i];
            if (entity.kind == kotor::EntityKind::Creature && entity.hostile && !entity.dead
                && !entity.partyAvatar
                && (hostile == kNoEntity
                    || flat(from, entity.position)
                        < flat(from, game.scene().entities[hostile].position))) {
                hostile = i;
            }
        }
        check(hostile != kNoEntity, "hostile_fixture");
        if (hostile != kNoEntity) {
            kotor::Entity& target = game.scene().entities[hostile];
            // A target that outlasts the check, so the exchange of blows can be seen.
            target.hitPoints = 400;
            target.maxHitPoints = 400;
            note(concat("hostile ", target.tag, " is ", flat(from, target.position), " away"));
            // The shipped hostiles stand behind closed doors: put the companion on the
            // walkmesh a few steps from the nearest one (a fixture, not a movement test).
            for (const nav::Tri& tri : mesh->tris()) {
                const Vec3 spot = triCentre(tri);
                const float apart = flat(spot, target.position);
                if (apart < 4.0f || apart > 8.0f || std::fabs(spot.z - target.position.z) > 0.5f) {
                    continue;
                }
                const nav::RayResult line = mesh->ray(spot, mesh->locate(spot), target.position,
                                                      game.nav_->closed);
                if (!line.clear) {
                    continue;
                }
                game.scene().entities[carthEntity].position = spot;
                game.syncEntityToScriptWorld(carthEntity);
                break;
            }
            step(game, 2);
            api.clearQueue(pc);
            state("before attack");
            game.attackNearestHostile();
            bool queued = false;
            for (const ActorAction& action : api.actor(carth).queue) {
                queued = queued
                    || (action.kind == ActorActionKind::Attack && action.userIssued
                        && api.entityFor(action.object) != kNoEntity
                        && game.scene().entities[api.entityFor(action.object)].kind
                            == kotor::EntityKind::Creature);
            }
            bool pcOrdered = false;
            for (const ActorAction& action : api.actor(pc).queue) {
                pcOrdered = pcOrdered || (action.kind == ActorActionKind::Attack && action.userIssued);
            }
            state("after attack command");
            note(concat("attack queue carth=", api.actor(carth).queue.size(), " pc=",
                        api.actor(pc).queue.size(), " hostile at ",
                        flat(game.controlledPosition(), game.scene().entities[hostile].position),
                        " tag=", game.scene().entities[hostile].tag, " target hp=",
                        game.scene().entities[hostile].hitPoints));
            for (const std::string& line : game.combatLog()) {
                if (line.find("target") != std::string::npos) {
                    note("  log " + line);
                }
            }
            check(queued && !pcOrdered, "attack_command_queued_on_the_companion");
            bool swung = false;
            for (int i = 0; i < 1500 && !swung; ++i) {
                step(game, 1);
                swung = eventSeen(game, carthTag + " hits ") || eventSeen(game, carthTag + " misses ");
            }
            check(swung, "companion_attacks");
        }
    }

    // ---- death: control moves to a living member, a dead member is not selectable ----------------------
    {
        check(game.selectLeaderNpc(kNpcBastila), "select_bastila");
        game.scene().entities[bastilaEntity].hitPoints = 0;
        step(game, 10);
        check(game.partyTable().controlled != kNpcBastila && !game.controlledDead(),
              "control_moves_when_the_controlled_creature_dies");
        const std::vector<int> choices = game.selectableLeaders();
        check(std::find(choices.begin(), choices.end(), kNpcBastila) == choices.end()
                  && !game.selectLeaderNpc(kNpcBastila),
              "dead_member_not_selectable");
        note(concat("after death controlled=", game.partyTable().controlled));
        step(game, 30);
        check(game.selectLeaderNpc(kNpcCarth) && game.controlledObject() == carth,
              "carth_back_in_control");
    }

    // ---- saves ---------------------------------------------------------------------------------------------------
    {
        float length = 0.0f;
        const Vec3 start = game.scene().entities[carthEntity].position;
        game.yaw_ = openHeading(game, start, 6.0f, length);
        InputState forward;
        forward.forward = true;
        step(game, 45, forward);
        step(game, 5);
        const Vec3 carthAt = game.scene().entities[carthEntity].position;
        const Vec3 pcAt = game.position();
        const std::filesystem::path savePath =
            std::filesystem::temp_directory_path() / "kotor_family_companion.save";
        check(game.save(savePath), "engine_save_written");
        {
            GameSession restored(game.installPath(), game.module());
            check(restored.load(savePath), "engine_save_loaded");
            const PartyTable& a = game.partyTable();
            const PartyTable& b = restored.partyTable();
            check(b.controlled == kNpcCarth && b.leader == a.leader && b.members == a.members,
                  "engine_roundtrip_controlled_leader");
            check(restored.companionControlled()
                      && restored.controlledObject() == restored.partyTable().npcs[kNpcCarth].object
                      && restored.controlledObject() != restored.scriptWorld().pc,
                  "engine_roundtrip_control_is_live");
            note(concat("restored carth ", flat(restored.controlledPosition(), carthAt),
                        " from saved, pc ", flat(restored.position(), pcAt)));
            check(flat(restored.controlledPosition(), carthAt) < 0.05f
                      && flat(restored.position(), pcAt) < 0.05f,
                  "engine_roundtrip_positions");
            // The restored session still steers the companion.
            const std::size_t body = restored.controlledEntity();
            const Vec3 before = body == kNoEntity ? Vec3{} : restored.scene().entities[body].position;
            float runLength = 0.0f;
            restored.yaw_ = openHeading(restored, before, 4.0f, runLength);
            InputState go;
            go.forward = true;
            step(restored, 30, go);
            check(body != kNoEntity && flat(before, restored.scene().entities[body].position) > 0.3f,
                  "engine_roundtrip_input_still_steers_the_companion");
        }

        const std::filesystem::path folder =
            std::filesystem::temp_directory_path() / "kotor_family_companion_original";
        std::error_code removeError;
        std::filesystem::remove_all(folder, removeError);
        check(game.saveOriginalSave(folder, "companion gate"), "original_save_written");
        const kotor::OriginalSave written = kotor::readOriginalSave(folder);
        bool carthLeads = false;
        for (const auto& [id, leads] : written.party) {
            carthLeads = carthLeads || (id == kNpcCarth && leads);
        }
        note(concat("original controlled=", written.controlledNpc, " carthLeads=", carthLeads ? 1 : 0));
        // PT_CONTROLLED_NP and PT_IS_LEADER carry the steered NPC id.
        check(written.controlledNpc == kNpcCarth && carthLeads,
              "original_save_maps_the_controlled_field");
        GameSession reloaded(game.installPath(), game.module());
        check(reloaded.loadOriginalSave(folder), "original_save_loaded");
        check(reloaded.partyTable().controlled == kNpcCarth && reloaded.companionControlled()
                  && reloaded.controlledObject() != reloaded.scriptWorld().pc,
              "original_roundtrip_controlled");
        std::filesystem::remove_all(folder, removeError);
        std::filesystem::remove(savePath, removeError);
    }

    // ---- a module change while a companion leads ----------------------------------------------------------------
    {
        const std::string from = game.module();
        check(game.partyTable().controlled == kNpcCarth, "leads_before_the_module_change");
        run(509, {std::vector<script::Value>{script::Value::makeString("tar_m02aa")},
                  std::vector<script::Value>{script::Value::makeString("")}});
        for (int i = 0; i < 600 && game.module() == from; ++i) {
            step(game, 1);
        }
        step(game, 30);
        check(game.module() == "tar_m02aa", "module_changed");
        nav::NavMesh* next = game.navMesh();
        const std::size_t body = game.controlledEntity();
        const std::uint32_t lead = game.controlledObject();
        const bool bodyOk = body != kNoEntity && lead != game.scriptWorld().pc
            && game.scriptWorld().get(lead) != nullptr;
        check(game.partyTable().controlled == kNpcCarth && game.companionControlled() && bodyOk,
              "control_survives_the_module_change");
        if (bodyOk && next != nullptr) {
            const Vec3 at = game.controlledPosition();
            check(next->locate(at) >= 0, "controlled_creature_stands_on_the_new_walkmesh");
            check(flat(game.position(), at) < 6.0f, "pc_stands_beside_the_leader");
            check(flat(game.camera().eye, at) <= 3.4f, "camera_follows_after_the_change");
            check(ro(548, {}) == lead && ri(217, {O(lead)}) == 1,
                  "scripts_see_the_leader_after_the_change");
            float length = 0.0f;
            game.yaw_ = openHeading(game, at, 4.0f, length);
            InputState go;
            go.forward = true;
            step(game, 45, go);
            check(flat(at, game.controlledPosition()) > 0.3f, "input_steers_after_the_change");
        }
    }

    logInfo(concat("companion family: ", ok ? "PASS" : "FAIL"));
    return ok ? 0 : 1;
}

} // namespace ke
