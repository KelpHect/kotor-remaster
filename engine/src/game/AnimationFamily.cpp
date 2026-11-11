// What a creature's body does for how it moves and what it holds, and the gate behind
// --family-test animation.
#include "game/AnimationFamily.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"
#include "script/World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace ke {

namespace {

// The first clip the model has, in the model's own spelling.
std::string firstClip(const kotor::Mdl& model, std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        if (name[0] != '\0') {
            if (std::string found = model.clipNamed(name); !found.empty()) {
                return found;
            }
        }
    }
    return {};
}

} // namespace

// retail 0069f650, the clip rows by wield class. "Drawn" stands for the flags of 00646ba0, which
// 006182a0 sets as combat mode changes: taking it as being in combat is inferred from those.
// The c-prefixed names are the other model type's rows (the same function's second branch).
std::string stanceClipFor(const kotor::Mdl& model, std::int32_t wield, bool drawn, StanceMotion motion)
{
    if (motion == StanceMotion::Still) {
        static constexpr std::array<const char*, 10> kReady{
            "", "g1r1", "g2r1", "g3r1", "g4r1", "g5r1", "g6r1", "g7r1", "g8r1", "g7r1"};
        const char* ready = drawn && wield >= 1 && wield <= 9 ? kReady[static_cast<std::size_t>(wield)] : "";
        return firstClip(model, {ready, drawn ? "creadyr" : "", "pause1", "cpause1"});
    }
    const bool rifle = wield == 7 || wield == 9;
    if (motion == StanceMotion::Walk) {
        const char* variant = rifle ? "walkRF"
            : !drawn ? ""
            : wield == 2 ? "walkSS" : wield == 3 ? "walkST" : wield == 4 ? "walkDS" : "";
        return firstClip(model, {variant, "walk", "cwalk"});
    }
    const char* variant = rifle ? "runRF"
        : !drawn ? ""
        : wield == 2 ? "runSS" : wield == 3 ? "runST" : wield == 4 ? "runDS" : "";
    return firstClip(model, {variant, "run", "crun", "walk", "cwalk"});
}

// retail 00613da0 (melee, by attack index) and 00614110 (ranged, its unfeated row). Which index an
// attack takes is the combat round's in retail; cycling them is this engine's stand-in. The ca*
// rows for creatures without these clips are inferred from animations.2da's naming.
std::string attackClipFor(const kotor::Mdl& model, std::int32_t wield, std::int32_t index)
{
    const auto pick = [&](std::initializer_list<const char*> names) {
        const auto count = static_cast<std::int32_t>(names.size());
        const char* name = *(names.begin() + (index % count + count) % count);
        return model.clipNamed(name);
    };
    std::string clip;
    switch (wield) {
    case 1: clip = pick({"g1a1", "g1a2"}); break;
    case 2: clip = pick({"c2a1", "c2a2", "c2a3", "c2a4", "c2a5"}); break;
    case 3: clip = pick({"c3a1", "c3a2", "c3a3", "c3a4", "c3a5"}); break;
    case 4: clip = pick({"c4a1", "c4a2", "c4a3", "c4a4", "c4a5"}); break;
    case 5: clip = pick({"b5a1"}); break;
    case 6: clip = pick({"b6a1"}); break;
    case 7: clip = pick({"b7a1"}); break;
    case 8: clip = pick({"g8a1", "g8a2"}); break;
    case 9: clip = pick({"b9a1"}); break;
    default: break;
    }
    if (!clip.empty()) {
        return clip;
    }
    // The other model type's rows: 00614110 takes b0a1 for ranged without a feat, 00613da0 g0a1 plus
    // the attack index for melee. A war droid has only these.
    const bool ranged = wield == 5 || wield == 6 || wield == 7 || wield == 9;
    clip = ranged ? model.clipNamed("b0a1") : pick({"g0a1", "g0a2"});
    return clip.empty() ? firstClip(model, {"ca1slfh", "ca2slfh"}) : clip;
}

// retail 0069f650: rows 0x50 die and 0x51 dead, 0x112 cdie and 0x113 cdead on the other model type.
// A war droid's cdie is its explosion and its cdead the wreck left on the floor (frames o6, o8).
std::string deathClipFor(const kotor::Mdl& model) { return firstClip(model, {"die", "cdie"}); }
std::string deadClipFor(const kotor::Mdl& model) { return firstClip(model, {"dead", "cdead"}); }

namespace {

// retail 00647130: a powered item lights when the weapons come out and goes out when they go away.
void stepItemPower(kotor::EntityMesh& item, bool drawn)
{
    const kotor::Mdl& model = *item.model;
    if (model.animationIndex("off") < 0 || model.animationIndex("powered") < 0) {
        return;
    }
    const bool done = item.animationTime >= model.animationDuration(item.animation);
    std::string next;
    if (drawn) {
        if (item.animation == "powerup") {
            next = done ? "powered" : "";
        } else if (item.animation != "powered") {
            next = model.animationIndex("powerup") >= 0 ? "powerup" : "powered";
        }
    } else if (item.animation == "powerdown") {
        next = done ? "off" : "";
    } else if (item.animation != "off") {
        next = model.animationIndex("powerdown") >= 0 ? "powerdown" : "off";
    }
    if (!next.empty()) {
        item.animation = next;
        item.animationTime = 0.0f;
        item.animationLoop = false;
    }
}

} // namespace

bool GameSession::hidesEquippedItems(const std::string& clip)
{
    if (clip.empty()) {
        return false;
    }
    if (hideItemClips_.empty()) {
        hideItemClips_.emplace("", false); // Loaded, whatever the table holds.
        if (const std::shared_ptr<const kotor::TwoDa> table = install_.table("animations")) {
            const int name = table->columnIndex("name");
            const int hide = table->columnIndex("hideequippeditems");
            for (std::size_t row = 0; name >= 0 && hide >= 0 && row < table->rowCount(); ++row) {
                if (table->cell(row, static_cast<std::size_t>(hide)) == "1") {
                    std::string key = table->cell(row, static_cast<std::size_t>(name));
                    std::transform(key.begin(), key.end(), key.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    hideItemClips_.emplace(std::move(key), true);
                }
            }
        }
    }
    std::string key = clip;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hideItemClips_.count(key) != 0;
}

void GameSession::playAttackClip(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()) {
        return;
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity != entityIndex || mesh.attachModel || !mesh.model) {
            continue;
        }
        const kotor::Entity& entity = scene_.entities[entityIndex];
        const std::string clip = attackClipFor(
            *mesh.model, kotor::wieldClass(entity.rightWield, entity.leftWield, !entity.weaponModel.empty(),
                                           !entity.offhandModel.empty()),
            mesh.stanceAttack++);
        if (!clip.empty()) {
            mesh.stanceClip = clip;
            playAnimation(entityIndex, clip, false);
        }
        return;
    }
}

float GameSession::nextAttackImpact(std::size_t entityIndex, int subAttack) const
{
    if (entityIndex >= scene_.entities.size()) {
        return -1.0f;
    }
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity != entityIndex || mesh.attachModel || !mesh.model) {
            continue;
        }
        const kotor::Entity& entity = scene_.entities[entityIndex];
        const std::string clip = attackClipFor(
            *mesh.model, kotor::wieldClass(entity.rightWield, entity.leftWield, !entity.weaponModel.empty(),
                                           !entity.offhandModel.empty()),
            mesh.stanceAttack);
        const std::int32_t ms = rules_.attackImpactMs(clip, subAttack);
        return ms >= 0 ? static_cast<float>(ms) / 1000.0f : -1.0f;
    }
    return -1.0f;
}

void GameSession::updateCreatureStances(float dt)
{
    if (dt <= 0.0f || miniGameActive()) {
        return;
    }
    std::vector<kotor::EntityMesh*> bodies(scene_.entities.size(), nullptr);
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.entity < bodies.size() && bodies[mesh.entity] == nullptr && !mesh.attachModel
            && !mesh.fx && !mesh.roomAnimated && mesh.model) {
            bodies[mesh.entity] = &mesh;
        }
    }
    std::vector<char> drawn(scene_.entities.size(), 0);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        kotor::EntityMesh* body = bodies[i];
        if (entity.kind != kotor::EntityKind::Creature || body == nullptr) {
            continue;
        }
        const std::uint32_t object = objectOfBody(i);
        if (const auto actor = sim_->actors.find(object); actor != sim_->actors.end()) {
            drawn[i] = actor->second.combat.inCombat ? 1 : 0;
        }
        const kotor::Vec3 now = entity.position;
        const kotor::Vec3 was = body->stancePlaced ? body->stancePosition : now;
        body->stancePosition = now;
        body->stancePlaced = true;
        if (entity.dead) {
            if (!entity.corpse()) {
                continue;
            }
            const std::string die = deathClipFor(*body->model);
            const std::string dead = deadClipFor(*body->model);
            if (entity.dying) {
                scene_.entities[i].dying = false;
                if (!die.empty()) {
                    logInfo(concat("stance: ", entity.tag, " falls with ", die, " at ", elapsed_));
                    body->stanceClip = die;
                    playAnimation(i, die, false);
                    continue;
                }
            }
            const bool falling = !die.empty() && body->animation == die
                && body->animationTime < body->model->animationDuration(die);
            if (!falling && !dead.empty() && body->animation != dead) {
                body->stanceClip = dead;
                playAnimation(i, dead, true);
            }
            continue;
        }
        // A creature back from a temporary death (004b6da0 -> 004e13c0) rises with the model's
        // getupdead clip (animations.2da row 381; row 382 getupdead1 after die1/dead1), 3.0 s on
        // s_male02. Retail capture tar_m02af: still ~1.3 s, push-up, kneel, upright ~2.3 s in.
        // Only the death this controller played (a script's dead pose is the script's).
        std::string lyingLower = body->animation;
        std::transform(lyingLower.begin(), lyingLower.end(), lyingLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (const std::string& lying = body->animation;
            !lying.empty() && lying == body->stanceClip && (lying == deadClipFor(*body->model) || lying == deathClipFor(*body->model)
                               || lyingLower == "dead1" || lyingLower == "die1")) {
            const bool second = lyingLower == "dead1" || lyingLower == "die1";
            const std::string up = second ? firstClip(*body->model, {"getupdead1", "getupdead"})
                                          : firstClip(*body->model, {"getupdead"});
            if (!up.empty()) {
                logInfo(concat("stance: ", entity.tag, " rises with ", up, " from ", lying, " at ", elapsed_));
                body->stanceClip = up;
                playAnimation(i, up, false);
                continue;
            }
        }
        bool staged = false;
        for (const DialogueStunt& stunt : dialogueStunts_) {
            staged = staged || stunt.participant == i || stunt.stand == i;
        }
        if (staged) {
            continue;
        }
        const float dx = now.x - was.x;
        const float dy = now.y - was.y;
        const float speed = std::sqrt(dx * dx + dy * dy) / dt;
        // A jump across the area is no step. This engine's reading of "moving": a position that
        // changed within the last moments, so a frame without a step does not flick to idle.
        if (speed > 0.05f && speed < 30.0f) {
            body->stanceStill = 0.0f;
            const float walkRate = creatureMoveRate(object, false);
            const float runRate = creatureMoveRate(object, true);
            body->stanceRunning = speed >= (runRate > walkRate ? (walkRate + runRate) * 0.5f : 2.6f);
        } else {
            body->stanceStill += dt;
        }
        const StanceMotion motion = body->stanceStill >= 0.12f ? StanceMotion::Still
            : body->stanceRunning ? StanceMotion::Run : StanceMotion::Walk;
        const std::string& current = body->animation;
        const bool ownClip = current.empty() || current == body->stanceClip
            || current == "pause1" || current == "cpause1";
        if (!ownClip) {
            continue; // A script's or a cutscene's clip.
        }
        if (!body->animationLoop && !current.empty()
            && body->animationTime < body->model->animationDuration(current)) {
            continue; // An attack plays out.
        }
        const std::string want = stanceClipFor(
            *body->model, kotor::wieldClass(entity.rightWield, entity.leftWield, !entity.weaponModel.empty(),
                                            !entity.offhandModel.empty()),
            drawn[i] != 0, motion);
        body->stanceClip = want;
        if (!want.empty() && want != current) {
            playAnimation(i, want, true);
        }
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.attachModel && mesh.model && mesh.entity < drawn.size()) {
            stepItemPower(mesh, drawn[mesh.entity] != 0);
        }
    }
}

namespace {

std::string lowerOf(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

float distance(kotor::Vec3 a, kotor::Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

kotor::Vec3 applyPose(const kotor::Mdl::Pose& pose, kotor::Vec3 v)
{
    const kotor::Mat3& m = pose.rotation;
    return {pose.translation.x + m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
            pose.translation.y + m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            pose.translation.z + m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

kotor::EntityMesh* bodyMesh(kotor::Scene& scene, std::size_t entity)
{
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entity && !mesh.attachModel && mesh.model) {
            return &mesh;
        }
    }
    return nullptr;
}

kotor::EntityMesh* handMesh(kotor::Scene& scene, std::size_t entity, const std::string& node)
{
    for (kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entity && mesh.attachModel && mesh.visible && mesh.attachNode == node) {
            return &mesh;
        }
    }
    return nullptr;
}

kotor::MdlPoseRequest bodyRequest(const kotor::EntityMesh& mesh)
{
    kotor::MdlPoseRequest request;
    request.animation = mesh.animation;
    request.time = mesh.animationTime;
    if (mesh.blendRemaining > 0.0f && !mesh.previousAnimation.empty() && mesh.blendDuration > 0.0f) {
        request.previous = mesh.previousAnimation;
        request.previousTime = mesh.previousAnimationTime;
        request.blend = 1.0f - mesh.blendRemaining / mesh.blendDuration;
    }
    // The head-follow turn (tickHeadFollow) is part of the body pose, as in bodyPoseRequest.
    request.headTurn = mesh.headYaw != 0.0f || mesh.headPitch != 0.0f ? &mesh.headTurn : nullptr;
    return request;
}

// The head as the mesh should show it: the head's own clip and lip track, on the body's hook.
std::vector<kotor::Vec3> expectedHead(const kotor::EntityMesh& mesh)
{
    std::vector<kotor::Vec3> out;
    const kotor::MdlPoseRequest body = bodyRequest(mesh);
    const auto hook = mesh.model->nodePoseFor(mesh.headNode, body);
    kotor::MdlPoseRequest head = body;
    head.headRootOnly = true;
    head.face = mesh.face.weight > 0.0f ? &mesh.face : nullptr;
    // A body with its own hturn_g turns the head through it; one without (the player's) turns the
    // head model instead, with the turn put in the hook's frame (hook^-1 * turn * hook).
    kotor::Mat3 headFrameTurn;
    if (body.headTurn != nullptr) {
        if (mesh.model->hasNode("hturn_g")) {
            head.headTurn = nullptr;
        } else if (hook.has_value()) {
            const auto mul = [](const kotor::Mat3& a, const kotor::Mat3& b) {
                kotor::Mat3 r;
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
                    }
                }
                return r;
            };
            kotor::Mat3 inverse;
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    inverse.m[c][r] = hook->rotation.m[r][c];
                }
            }
            headFrameTurn = mul(inverse, mul(mesh.headTurn, hook->rotation));
            head.headTurn = &headFrameTurn;
        }
    }
    const float own = mesh.headModel->animationDuration(mesh.animation);
    if (mesh.animationLoop && own > 0.0f) {
        head.time = std::fmod(mesh.animationTime, own);
    }
    for (const kotor::MdlMeshTriangle& triangle : mesh.headModel->posedTrianglesFor(head)) {
        for (const kotor::MdlMeshVertex& vertex : triangle.v) {
            out.push_back(hook.has_value() ? applyPose(*hook, vertex.position) : vertex.position);
        }
    }
    return out;
}

// The share of those head vertices the drawn mesh really holds, to half a millimetre.
float headMatch(const kotor::EntityMesh& mesh)
{
    const auto key = [](kotor::Vec3 v) {
        return std::array<int, 3>{static_cast<int>(std::lround(v.x * 2000.0f)),
                                  static_cast<int>(std::lround(v.y * 2000.0f)),
                                  static_cast<int>(std::lround(v.z * 2000.0f))};
    };
    std::set<std::array<int, 3>> drawn;
    for (const kotor::SceneVertex& vertex : mesh.vertices) {
        const std::array<int, 3> k = key(vertex.position);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    drawn.insert({k[0] + dx, k[1] + dy, k[2] + dz});
                }
            }
        }
    }
    const std::vector<kotor::Vec3> wanted = expectedHead(mesh);
    std::size_t found = 0;
    for (const kotor::Vec3& v : wanted) {
        found += drawn.count(key(v));
    }
    return wanted.empty() ? 0.0f : static_cast<float>(found) / static_cast<float>(wanted.size());
}

} // namespace

// A fixture's sheet must not stop the weapon reaching the hand: every base item's required feats.
void grantWeaponFeats(GameSession& game, std::uint32_t object)
{
    const std::shared_ptr<const kotor::TwoDa> table = game.install_.table("baseitems");
    CharacterSheet* sheet = nullptr;
    if (object == game.scriptWorld_.pc) {
        sheet = &game.character_;
    } else if (const script::GameObject* found = game.scriptWorld_.get(object); found != nullptr) {
        for (CompanionState& companion : game.companions_) {
            if (lowerOf(companion.tag) == lowerOf(found->tag)) {
                sheet = &companion.sheet;
            }
        }
    }
    if (table == nullptr || sheet == nullptr) {
        return;
    }
    for (std::size_t row = 0; row < table->rowCount(); ++row) {
        for (const char* column : {"reqfeat0", "reqfeat1", "reqfeat2", "reqfeat3", "reqfeat4"}) {
            const int at = table->columnIndex(column);
            const std::string cell = at >= 0 ? table->cell(row, static_cast<std::size_t>(at)) : std::string{};
            if (!cell.empty() && cell != "****") {
                sheet->feats.insert(std::atoi(cell.c_str()));
            }
        }
    }
}

namespace {

struct WieldCase {
    const char* label;
    const char* right;
    const char* left;
    std::int32_t wield;
    const char* rightModel;
    const char* ready;
    const char* walk;
    const char* run;
    const char* walkDrawn;
    const char* runDrawn;
    const char* attack;
    bool powered;
};

// The expected rows, written out from retail 0069f650, 00613710, 00613da0 and 00614110.
const WieldCase kWields[] = {
    {"unarmed", "", "", 8, "", "g8r1", "walk", "run", "walk", "run", "g8a1", false},
    {"baton", "g_w_stunbaton01", "", 1, "w_stunbaton_001", "g1r1", "walk", "run", "walk", "run", "g1a1", false},
    {"sword", "g_w_lngswrd01", "", 2, "w_lngswrd_001", "g2r1", "walk", "run", "walkss", "runss", "c2a1", false},
    {"staff", "g_w_qtrstaff01", "", 3, "w_qtrstaff_001", "g3r1", "walk", "run", "walkst", "runst", "c3a1", false},
    {"saber", "g_w_lghtsbr01", "", 2, "w_lghtsbr_001", "g2r1", "walk", "run", "walkss", "runss", "c2a1", true},
    {"saber_variant", "g_w_lghtsbr02", "", 2, "w_lghtsbr_002", "g2r1", "walk", "run", "walkss", "runss", "c2a1", true},
    {"double_saber", "g_w_dblsbr001", "", 3, "w_dblsbr_001", "g3r1", "walk", "run", "walkst", "runst", "c3a1", true},
    {"two_swords", "g_w_shortswrd01", "g_w_shortswrd01", 4, "w_shortswrd_001", "g4r1", "walk", "run", "walkds", "runds", "c4a1", false},
    {"pistol", "g_w_blstrpstl001", "", 5, "w_blstrpstl_001", "g5r1", "walk", "run", "walk", "run", "b5a1", false},
    {"two_pistols", "g_w_blstrpstl001", "g_w_blstrpstl001", 6, "w_blstrpstl_001", "g6r1", "walk", "run", "walk", "run", "b6a1", false},
    {"rifle", "g_w_blstrrfl001", "", 7, "w_blstrrfl_001", "g7r1", "walkrf", "runrf", "walkrf", "runrf", "b7a1", false},
    {"repeater", "g_w_rptnblstr01", "", 9, "w_rptnblstr_001", "g7r1", "walkrf", "runrf", "walkrf", "runrf", "b9a1", false},
};

} // namespace

int runAnimationFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("animation family: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto note = [](const std::string& line) { logInfo("animation family: " + line); };
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    game.setAnimationEnabled(true);
    kotor::Scene& scene = game.scene_;

    // ---- lip sync: a shipped line through the real conversation path -------------------------
    const std::size_t trask = game.findEntity("end_trask");
    check(trask != kNone, "lip_fixture");
    check(trask != kNone && game.startDialogue(trask), "lip_dialogue");
    game.update(1.0f / 30.0f, InputState{});
    // The conversation's first line that ships a track, shown the way the conversation shows it.
    if (!game.lipSyncActive() && game.activeDialogue_.has_value()) {
        int withVoice = 0;
        int withLip = 0;
        const kotor::DlgEntry* voiced = nullptr;
        for (const kotor::DlgEntry& entry : game.activeDialogue_->dlg.entries) {
            const std::string voice = !entry.voResRef.empty() ? entry.voResRef : entry.sound;
            withVoice += voice.empty() ? 0 : 1;
            if (!voice.empty() && game.install_.find(voice, kotor::resType(kotor::ResType::Lip)).has_value()) {
                ++withLip;
                voiced = voiced == nullptr ? &entry : voiced;
            }
        }
        note(concat("conversation ", game.activeDialogue_->dlg.entries.size(), " lines, voiced ", withVoice,
                    " with a track ", withLip));
        if (voiced != nullptr) {
            game.playDialoguePresentation(*voiced, voiced->sound);
        }
    }
    check(game.lipSyncActive(), "lip_shipped_line");
    if (game.lipSyncActive() && trask != kNone) {
        const kotor::LipSync lip = *game.activeLip_;
        note(concat("line ", lip.name, " duration ", lip.duration, " keys ", lip.frames.size()));
        bool wellFormed = lip.frames.size() >= 2 && lip.frames.front().time == 0.0f
            && std::fabs(lip.frames.back().time - lip.duration) < 0.05f;
        for (const kotor::LipFrame& frame : lip.frames) {
            wellFormed = wellFormed && frame.viseme < 16;
        }
        check(wellFormed, "lip_track_parse");
        const std::size_t speaker = game.entityOfObject(game.dialogueView_.speakerObject);
        check(speaker == trask, "lip_speaker");
        kotor::EntityMesh* mesh = bodyMesh(scene, trask);
        check(mesh != nullptr && mesh->headModel != nullptr, "lip_separate_head");
        if (mesh != nullptr && mesh->headModel != nullptr) {
            const kotor::Mdl& head = *mesh->headModel;
            check(!head.clipNamed("talk").empty(), "lip_talk_on_chain");
            check(lowerOf(head.headRootName()) == "neck_g", "lip_head_root");
            const auto faceAt = [](int shape) {
                kotor::MdlFacePose face;
                face.fractionA = face.fractionB = FacialRig::clipFraction(shape);
                face.weight = 1.0f;
                return face;
            };
            const auto nodeAt = [&](const std::string& node, const kotor::MdlFacePose* face) {
                kotor::MdlPoseRequest request;
                request.face = face;
                request.headRootOnly = true;
                return head.nodePoseFor(node, request)->translation;
            };
            check(head.hasNode("f_jaw_g") && head.hasNode("f_um_g") && head.hasNode("f_Llm_g")
                      && head.hasNode("f_Rlm_g"), "lip_mouth_nodes");
            // How far the lips part, shape by shape. Bump (the lips together) against the two
            // named for an open mouth.
            std::array<float, 16> gap{};
            std::string gaps;
            for (int shape = 0; shape < 16; ++shape) {
                const kotor::MdlFacePose face = faceAt(shape);
                const kotor::Vec3 l = nodeAt("f_Llm_g", &face);
                const kotor::Vec3 r = nodeAt("f_Rlm_g", &face);
                gap[static_cast<std::size_t>(shape)] = distance(nodeAt("f_um_g", &face), (l + r) * 0.5f);
                gaps += concat(" ", gap[static_cast<std::size_t>(shape)]);
            }
            note("lip gap by shape:" + gaps);
            check(gap[3] > gap[11] + 0.002f, "lip_open_ox_over_bump");
            check(gap[13] > gap[11] + 0.002f, "lip_open_roar_over_bump");
            const float spread = *std::max_element(gap.begin(), gap.end())
                - *std::min_element(gap.begin(), gap.end());
            check(spread > 0.004f && spread < 0.08f, "lip_gap_range");

            // The clock and the pose against the track's keys, on the voice's real time.
            const float start = game.lipTime();
            float elapsed = 0.0f;
            bool keysMatch = true;
            bool clockMatch = true;
            int sampled = 0;
            int between = 0;
            constexpr float kFrame = 1.0f / 30.0f;
            const float span = std::min(lip.duration - 0.05f, 2.5f);
            while (elapsed < span && game.lipSyncActive() && game.activeLip_->name == lip.name) {
                game.update(kFrame, InputState{});
                elapsed += kFrame;
                mesh = bodyMesh(scene, trask);
                const float t = game.lipTime();
                clockMatch = clockMatch && std::fabs(t - (start + elapsed)) < kFrame; // The tolerance: one frame.
                std::size_t next = 0;
                while (next + 1 < lip.frames.size() && lip.frames[next].time < t) {
                    ++next;
                }
                const std::size_t before = next == 0 ? 0 : next - 1;
                const float a = (static_cast<float>(lip.frames[before].viseme) + 1.0f) / 16.0f;
                const float b = (static_cast<float>(lip.frames[next].viseme) + 1.0f) / 16.0f;
                const float gapTime = lip.frames[next].time - lip.frames[before].time;
                const float f = gapTime > 0.0f ? (t - lip.frames[before].time) / gapTime : 1.0f;
                keysMatch = keysMatch && mesh->face.weight == 1.0f && std::fabs(mesh->face.fractionA - a) < 1e-4f
                    && std::fabs(mesh->face.fractionB - b) < 1e-4f && std::fabs(mesh->face.blend - f) < 1e-3f;
                ++sampled;
                between += a != b && f > 0.1f && f < 0.9f ? 1 : 0;
            }
            note(concat("lip samples ", sampled, " between keys ", between));
            check(sampled > 0 && keysMatch, "lip_pose_follows_keys");
            check(clockMatch, "lip_clock_on_time");
            check(between > 0, "lip_blends_between_keys");
            check(game.lipSyncActive(), "lip_still_this_line");
            const float beforeLong = game.lipTime();
            game.update(0.25f, InputState{});
            check(std::fabs(game.lipTime() - std::min(beforeLong + 0.25f, lip.duration + FacialRig::kFadeSeconds)) < 1e-3f,
                  "lip_clock_survives_long_frame");

            // The drawn head carries the pose: the head vertices, on the hook, posed by the track.
            mesh = bodyMesh(scene, trask);
            const kotor::MdlFacePose saved = mesh->face;
            const auto snapshot = [&](const kotor::MdlFacePose& face) {
                mesh->face = face;
                kotor::refreshEntityMesh(*mesh);
                std::vector<kotor::Vec3> out;
                for (const kotor::SceneVertex& vertex : mesh->vertices) {
                    out.push_back(vertex.position);
                }
                return out;
            };
            const std::vector<kotor::Vec3> rest = snapshot(kotor::MdlFacePose{});
            check(headMatch(*mesh) > 0.999f, "lip_rest_head_in_mesh");
            const std::vector<kotor::Vec3> roar = snapshot(faceAt(13));
            check(headMatch(*mesh) > 0.999f, "lip_posed_head_in_mesh");
            const auto hook = mesh->model->nodePoseFor(mesh->headNode, bodyRequest(*mesh));
            std::size_t moved = 0;
            float largest = 0.0f;
            float reach = 0.0f;
            for (std::size_t i = 0; i < rest.size() && i < roar.size(); ++i) {
                const float d = distance(rest[i], roar[i]);
                if (d > 0.001f) {
                    ++moved;
                    largest = std::max(largest, d);
                    reach = std::max(reach, distance(roar[i], hook->translation));
                }
            }
            note(concat("lip vertices moved ", moved, " largest ", largest, " reach ", reach));
            check(rest.size() == roar.size() && moved >= 30, "lip_mesh_moves");
            check(largest < 0.06f, "lip_no_thrown_vertices");
            check(reach < 0.45f, "lip_within_head");
            // Only the mouth's vertices: most of the head holds still.
            check(moved < rest.size() / 2, "lip_only_the_mouth");
            snapshot(saved);

            // The speaker field picks the mouth, a stand-in wears it, and nobody else talks.
            const std::size_t pc = game.playerEntity();
            std::size_t other = kNone;
            for (std::size_t i = 0; i < scene.entities.size(); ++i) {
                if (i != trask && i != pc && scene.entities[i].kind == kotor::EntityKind::Creature
                    && bodyMesh(scene, i) != nullptr) {
                    other = i;
                    break;
                }
            }
            check(other != kNone, "lip_second_creature");
            if (other != kNone) {
                const float lipAt = game.lipTime();
                game.lipTime_ = 0.1f;
                game.update(kFrame, InputState{});
                check(bodyMesh(scene, trask)->face.weight > 0.0f && bodyMesh(scene, other)->face.weight == 0.0f,
                      "lip_only_speaker");
                const std::uint32_t was = game.dialogueView_.speakerObject;
                game.dialogueView_.speakerObject = game.objectForEntity(other);
                game.update(kFrame, InputState{});
                check(bodyMesh(scene, other)->face.weight > 0.0f && bodyMesh(scene, trask)->face.weight == 0.0f,
                      "lip_speaker_field");
                game.dialogueView_.speakerObject = was;
                game.dialogueStunts_.push_back({trask, other});
                game.update(kFrame, InputState{});
                check(bodyMesh(scene, other)->face.weight > 0.0f, "lip_stunt_stand");
                game.dialogueStunts_.pop_back();
                game.lipTime_ = lipAt;
            }

            // The end of the track fades over half a second (retail 00486670), then lets go.
            game.lipTime_ = lip.duration;
            game.update(0.25f, InputState{});
            const float fading = bodyMesh(scene, trask)->face.weight;
            note(concat("lip weight a quarter second after the end ", fading));
            check(fading > 0.3f && fading < 0.7f, "lip_fade");
            game.update(0.3f, InputState{});
            check(!game.entityHasFacialPose(trask), "lip_fade_done");
            note(concat("head follow after the fade: yaw ", bodyMesh(scene, trask)->headYaw, " pitch ",
                        bodyMesh(scene, trask)->headPitch));
            check(headMatch(*bodyMesh(scene, trask)) > 0.999f, "lip_after_fade_rest");
        }
        game.endDialogue();
    }

    // ---- the head follows its own clips and the body's blend ----------------------------------
    if (trask != kNone) {
        kotor::EntityMesh* mesh = bodyMesh(scene, trask);
        if (mesh != nullptr && mesh->headModel) {
            const kotor::Mdl& head = *mesh->headModel;
            const std::string idle = head.clipNamed("pause1");
            check(!idle.empty(), "head_idle_clip");
            float most = 0.0f;
            float bestTime = 0.0f;
            kotor::MdlPoseRequest request;
            request.animation = idle;
            request.headRootOnly = true;
            const std::vector<kotor::MdlMeshTriangle> first = head.posedTrianglesFor(request);
            for (int i = 1; i < 24; ++i) {
                request.time = head.animationDuration(idle) * static_cast<float>(i) / 24.0f;
                const std::vector<kotor::MdlMeshTriangle> later = head.posedTrianglesFor(request);
                for (std::size_t t = 0; t < first.size() && t < later.size(); ++t) {
                    for (int v = 0; v < 3; ++v) {
                        const float d = distance(first[t].v[v].position, later[t].v[v].position);
                        if (d > most) {
                            most = d;
                            bestTime = request.time;
                        }
                    }
                }
            }
            note(concat("head idle motion ", most, " at ", bestTime));
            check(most > 0.002f, "head_idle_moves");
            game.playAnimation(trask, "pause1", true);
            mesh->animationTime = bestTime;
            kotor::refreshEntityMesh(*mesh);
            check(headMatch(*mesh) > 0.999f, "head_idle_in_mesh");
            // Into a clip with a transition: the head stays on the hook of the blended body.
            game.playAnimation(trask, "walk", true);
            mesh->blendDuration = mesh->blendRemaining = 0.25f;
            mesh->previousAnimation = "pause1";
            mesh->previousAnimationTime = bestTime;
            mesh->previousAnimationLoop = true;
            mesh->blendRemaining = 0.125f;
            mesh->stanceClip = "walk";
            kotor::refreshEntityMesh(*mesh);
            check(headMatch(*mesh) > 0.999f, "head_blend_in_mesh");
            const auto hook = mesh->model->nodePoseFor(mesh->headNode, bodyRequest(*mesh));
            float farthest = 0.0f;
            for (const kotor::Vec3& v : expectedHead(*mesh)) {
                farthest = std::max(farthest, distance(v, hook->translation));
            }
            note(concat("head reach from hook under walk ", farthest));
            check(farthest < 0.45f, "head_on_hook");
            game.playAnimation(trask, "pause1", true);
        }
    }

    // ---- what the hands hold ------------------------------------------------------------------
    game.setPlayerAppearance(92, "Revan");
    game.setThirdPerson(true);
    check(game.addPartyMember("Carth"), "wield_companion_joined");
    const std::size_t carth = game.findEntity("Carth");
    check(carth != kNone, "wield_companion");
    const auto setDrawn = [&](std::size_t entity, bool drawn) {
        game.sim_->actors[game.objectOfBody(entity)].combat.inCombat = drawn;
    };
    constexpr float kTick = 1.0f / 30.0f;
    // Moves the creature itself and runs the animation tick, so no script or follower steers it.
    const auto pump = [&](std::size_t entity, float speed, int frames) {
        for (int i = 0; i < frames; ++i) {
            scene.entities[entity].position.x += speed * kTick;
            game.updateAnimations(kTick);
        }
    };
    const auto clipOf = [&](std::size_t entity) { return lowerOf(bodyMesh(scene, entity)->animation); };
    const auto equipCase = [&](std::size_t entity, const WieldCase& c) {
        const std::uint32_t object = game.objectOfBody(entity);
        grantWeaponFeats(game, object); // Whether the sheet may hold it is the items gate's matter.
        for (const std::int32_t slot : {4, 5}) {
            const std::uint32_t worn = game.wornItemObject(object, slot);
            if (worn != script::kObjectInvalid) {
                game.unequipItemObject(object, worn);
            }
        }
        bool done = true;
        const auto wear = [&](const char* resref, std::int32_t slot) {
            if (resref[0] != '\0') {
                const std::uint32_t item = game.giveItem(resref, object, 1, script::kObjectInvalid);
                done = done && item != script::kObjectInvalid && game.equipItemObject(object, item, slot);
            }
        };
        wear(c.right, 4);
        wear(c.left, 5);
        return done;
    };
    const auto itemCentre = [](const kotor::EntityMesh& mesh) {
        kotor::Vec3 sum{};
        for (const kotor::SceneVertex& vertex : mesh.vertices) {
            sum = sum + vertex.position;
        }
        return sum * (1.0f / static_cast<float>(std::max<std::size_t>(mesh.vertices.size(), 1)));
    };

    std::vector<std::pair<std::string, std::size_t>> holders{{"pc", game.playerEntity()}, {"npc", trask}};
    if (carth != kNone) {
        holders.emplace_back("companion", carth);
    }
    for (const auto& [who, entity] : holders) {
        if (entity == kNone || bodyMesh(scene, entity) == nullptr) {
            check(false, who + "_body");
            continue;
        }
        const kotor::Vec3 home = scene.entities[entity].position;
        // Each model's weapon bones: the gate holds every hook to the bone it hangs from.
        for (const WieldCase& c : kWields) {
            const std::string tag = who + "_" + c.label;
            setDrawn(entity, false);
            check(equipCase(entity, c), tag + "_equip");
            const kotor::Entity& held = scene.entities[entity];
            const std::int32_t wield = kotor::wieldClass(held.rightWield, held.leftWield, !held.weaponModel.empty(),
                                                         !held.offhandModel.empty());
            check(wield == c.wield, tag + "_wield_class");
            check(lowerOf(held.weaponModel) == c.rightModel, tag + "_item_model");
            const float walkRate = game.creatureMoveRate(game.objectOfBody(entity), false);
            const float runRate = game.creatureMoveRate(game.objectOfBody(entity), true);
            const auto expectClip = [&](const std::string& what, const char* wanted, float speed) {
                pump(entity, speed, 8);
                const std::string got = clipOf(entity);
                if (got != wanted) {
                    note(concat(tag, " ", what, " clip '", got, "' expected '", wanted, "'"));
                }
                check(got == wanted, tag + "_" + what);
            };
            expectClip("idle", "pause1", 0.0f);
            expectClip("walk", c.walk, walkRate);
            expectClip("run", c.run, runRate);
            expectClip("idle_again", "pause1", 0.0f);
            setDrawn(entity, true);
            expectClip("walk_drawn", c.walkDrawn, walkRate);
            expectClip("run_drawn", c.runDrawn, runRate);
            expectClip("ready", c.ready, 0.0f);

            // The item is in the hand at every moment of the clip.
            if (c.right[0] != '\0') {
                kotor::EntityMesh* item = handMesh(scene, entity, "rhand");
                check(item != nullptr, tag + "_on_rhand_hook");
                if (item == nullptr) {
                    setDrawn(entity, false);
                    scene.entities[entity].position = home;
                    continue;
                }
                {
                    const kotor::Mdl& model = *item->attachModel;
                    check(model.hasNode("rhand") && model.hasNode("Rhand_g"), tag + "_hand_nodes");
                    if (c.left[0] != '\0') {
                        check(handMesh(scene, entity, "lhand") != nullptr, tag + "_on_lhand_hook");
                    }
                    pump(entity, runRate, 8);
                    float worstGrip = 0.0f;
                    float worstFollow = 0.0f;
                    kotor::Vec3 low{1e9f, 1e9f, 1e9f};
                    kotor::Vec3 high{-1e9f, -1e9f, -1e9f};
                    for (int frame = 0; frame < 20; ++frame) {
                        pump(entity, runRate, 1);
                        item = handMesh(scene, entity, "rhand");
                        kotor::MdlPoseRequest hand;
                        hand.animation = item->attachAnimation;
                        hand.time = item->attachTime;
                        hand.previous = item->attachPrevious;
                        hand.previousTime = item->attachPreviousTime;
                        hand.blend = item->attachBlend;
                        const auto hook = model.nodePoseFor("rhand", hand);
                        const auto bone = model.nodePoseFor("Rhand_g", hand);
                        worstGrip = std::max(worstGrip, distance(hook->translation, bone->translation));
                        // The drawn item is the item's own pose carried by this hook's rotation.
                        kotor::MdlPoseRequest own;
                        own.animation = item->animation;
                        own.time = item->animationTime;
                        // A saber draws its swing trail (kotor::SaberTrail): pose it from the same
                        // samples, placed as refreshEntityMesh places them; a zero step adds none.
                        std::vector<kotor::SaberTrail> trails = item->saberTrails;
                        kotor::SaberTrailFeed feed;
                        if (item->trailPlaced && hook.has_value()) {
                            const float bc = std::cos(item->trailBearing);
                            const float bs = std::sin(item->trailBearing);
                            const kotor::Mat3 bearing{{{bc, -bs, 0.0f}, {bs, bc, 0.0f}, {0.0f, 0.0f, 1.0f}}};
                            const kotor::Vec3 at = applyPose({bearing, {}}, hook->translation);
                            feed.trails = &trails;
                            for (int r = 0; r < 3; ++r) {
                                for (int k = 0; k < 3; ++k) {
                                    feed.rotation.m[r][k] = bearing.m[r][0] * hook->rotation.m[0][k]
                                        + bearing.m[r][1] * hook->rotation.m[1][k] + bearing.m[r][2] * hook->rotation.m[2][k];
                                }
                            }
                            feed.origin = item->trailPosition + at;
                            feed.scale = item->attachHidden ? 0.0f : item->attachScale;
                            own.saberTrail = &feed;
                        }
                        kotor::Vec3 sum{};
                        std::size_t count = 0;
                        for (const kotor::MdlMeshTriangle& triangle : item->model->posedTrianglesFor(own)) {
                            for (const kotor::MdlMeshVertex& vertex : triangle.v) {
                                sum = sum + vertex.position * item->attachScale;
                                ++count;
                            }
                        }
                        const kotor::Vec3 wanted = applyPose(*hook, sum * (1.0f / static_cast<float>(std::max<std::size_t>(count, 1))));
                        const kotor::Vec3 got = itemCentre(*item);
                        worstFollow = std::max(worstFollow, distance(wanted, got));
                        low = {std::min(low.x, hook->translation.x), std::min(low.y, hook->translation.y),
                               std::min(low.z, hook->translation.z)};
                        high = {std::max(high.x, hook->translation.x), std::max(high.y, hook->translation.y),
                                std::max(high.z, hook->translation.z)};
                    }
                    const float swing = distance(low, high);
                    note(concat(tag, " grip ", worstGrip, " follow ", worstFollow, " swing ", swing));
                    check(worstGrip < 0.12f, tag + "_hook_at_bone");
                    check(worstFollow < 0.002f, tag + "_item_on_hook");
                    check(swing > 0.02f, tag + "_item_moves_with_hand");
                }
            }

            // Into the ready stance the item stays with the blended arm: no jump in a frame.
            pump(entity, 0.0f, 12);
            pump(entity, runRate, 8);
            if (kotor::EntityMesh* item = handMesh(scene, entity, "rhand")) {
                bool sawBlend = false;
                float worstStep = 0.0f;
                kotor::Vec3 before = itemCentre(*item);
                for (int frame = 0; frame < 16; ++frame) {
                    pump(entity, 0.0f, 1);
                    item = handMesh(scene, entity, "rhand");
                    sawBlend = sawBlend || item->attachBlend < 1.0f;
                    const kotor::Vec3 now = itemCentre(*item);
                    worstStep = std::max(worstStep, distance(before, now));
                    before = now;
                }
                note(concat(tag, " transition step ", worstStep, " blended ", sawBlend ? 1 : 0));
                if (c.powered) {
                    // A saber's centre moves as the blade lights; the blend is asserted elsewhere.
                } else {
                    check(worstStep < 0.25f, tag + "_no_jump");
                }
            }

            // A saber is unlit until the weapons come out (retail 00647130).
            if (c.powered) {
                const auto bladeReach = [&] {
                    const kotor::EntityMesh* item = handMesh(scene, entity, "rhand");
                    const auto hook = item->attachModel->nodePose("rhand", item->attachAnimation, item->attachTime);
                    float far = 0.0f;
                    for (const kotor::SceneVertex& vertex : item->vertices) {
                        far = std::max(far, distance(vertex.position, hook->translation));
                    }
                    return far;
                };
                pump(entity, 0.0f, 40);
                check(handMesh(scene, entity, "rhand")->animation == "powered", tag + "_lit_state");
                const float lit = bladeReach();
                setDrawn(entity, false);
                pump(entity, 0.0f, 2);
                check(handMesh(scene, entity, "rhand")->animation == "powerdown", tag + "_powers_down");
                pump(entity, 0.0f, 60);
                check(handMesh(scene, entity, "rhand")->animation == "off", tag + "_off_state");
                const float unlit = bladeReach();
                note(concat(tag, " blade lit ", lit, " unlit ", unlit));
                check(lit > 0.6f, tag + "_blade_long");
                check(unlit < lit * 0.5f, tag + "_blade_gone");
                setDrawn(entity, true);
                pump(entity, 0.0f, 40);
            }

            // An attack plays its class's clip and returns to the stance.
            game.playAttackClip(entity);
            bodyMesh(scene, entity)->stanceAttack = 0;
            const std::string attack = clipOf(entity);
            // The first attack this mesh has played may not be index 0 by now; compare the family.
            const std::string family(c.attack, 2);
            check(attack.rfind(family, 0) == 0, tag + "_attack_clip");
            const float attackLength = bodyMesh(scene, entity)->model->animationDuration(bodyMesh(scene, entity)->animation);
            check(attackLength > 0.0f, tag + "_attack_length");
            pump(entity, 0.0f, 3);
            check(clipOf(entity) == attack, tag + "_attack_plays_out");
            pump(entity, 0.0f, static_cast<int>(attackLength / kTick) + 3);
            check(clipOf(entity) == c.ready, tag + "_attack_returns");

            // A clip that puts the items away collapses them into the hand.
            if (c.right[0] != '\0') {
                game.playAnimation(entity, "inject", false);
                pump(entity, 0.0f, 2);
                const kotor::EntityMesh* item = handMesh(scene, entity, "rhand");
                check(item->attachHidden, tag + "_hidden_flag");
                float far = 0.0f;
                kotor::MdlPoseRequest hand;
                hand.animation = item->attachAnimation;
                hand.time = item->attachTime;
                hand.previous = item->attachPrevious;
                hand.previousTime = item->attachPreviousTime;
                hand.blend = item->attachBlend;
                const auto hook = item->attachModel->nodePoseFor("rhand", hand);
                for (const kotor::SceneVertex& vertex : item->vertices) {
                    far = std::max(far, distance(vertex.position, hook->translation));
                }
                check(far < 1e-3f, tag + "_hidden_in_hand");
                game.playAnimation(entity, "pause1", true);
                pump(entity, 0.0f, 2);
            }
            setDrawn(entity, false);
            pump(entity, 0.0f, 60);
            scene.entities[entity].position = home;
            pump(entity, 0.0f, 2);
        }
    }

    // ---- the real paths: an equipped spawn, a queued walk, a combat swing, a control switch ----
    {
        // A creature whose template holds a weapon arrives wielding it from the load.
        std::size_t armed = 0;
        std::size_t onHook = 0;
        std::size_t classed = 0;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            const kotor::Entity& entity = scene.entities[i];
            if (entity.kind == kotor::EntityKind::Creature && !entity.weaponModel.empty()) {
                ++armed;
                onHook += handMesh(scene, i, "rhand") != nullptr ? 1 : 0;
                classed += entity.rightWield > 0 ? 1 : 0;
            }
        }
        std::map<std::string, int> heldModels;
        for (const kotor::Entity& entity : scene.entities) {
            if (entity.kind == kotor::EntityKind::Creature && !entity.weaponModel.empty()) {
                ++heldModels[entity.weaponModel];
            }
        }
        for (const auto& [name, count] : heldModels) {
            note(concat("held model ", name, " x", count));
        }
        note(concat("armed creatures ", armed, " on the hook ", onHook, " classed ", classed));
        check(armed > 0 && onHook == armed && classed == armed, "spawn_armed_on_hook");

        const std::uint32_t npc = game.objectForEntity(trask);
        const kotor::Vec3 from = scene.entities[trask].position;
        ActorAction walk;
        walk.kind = ActorActionKind::MoveToPoint;
        walk.point = {from.x + 4.0f, from.y, from.z};
        walk.run = false;
        walk.range = 0.2f;
        game.sim_->actors[npc].queue.clear();
        game.sim_->actors[npc].queue.push_back(walk);
        std::string seen;
        for (int i = 0; i < 40; ++i) {
            game.update(kTick, InputState{});
            if (game.dialogue().active) {
                game.endDialogue();
            }
            const std::string clip = clipOf(trask);
            if (clip != "pause1" && seen.empty()) {
                seen = clip;
            }
        }
        const float travelled = distance(from, scene.entities[trask].position);
        note(concat("queued walk moved ", travelled, " clip '", seen, "'"));
        check(travelled > 0.2f, "npc_queue_moves");
        check(seen == "walk" || seen == "walkrf", "npc_walk_clip");
        game.sim_->actors[npc].queue.clear();

        kotor::EntityMesh* body = bodyMesh(scene, trask);
        const int attacksBefore = body->stanceAttack;
        // Trask's UTC faction is 5 (neutral), which retail 004f2610 never lets into the
        // combat state; the swing fixture makes him hostile (faction 1) for the check.
        const int traskFaction = static_cast<int>(scene.entities[trask].faction);
        game.setObjectFaction(npc, 1);
        game.attackPlayerForTest(trask);
        body = bodyMesh(scene, trask);
        check(body->stanceAttack == attacksBefore + 1, "combat_swing_plays_clip");
        check(!body->animationLoop, "combat_swing_one_shot");
        check(game.sim_->actors[npc].combat.inCombat, "combat_swing_draws");
        game.sim_->actors[npc].combat.inCombat = false;
        game.setObjectFaction(npc, traskFaction);
        check(game.bodyOfObject(game.scriptWorld_.pc) == game.playerEntity(), "pc_object_has_body");

        if (carth != kNone) {
            const int member = game.partyTable().controlled;
            (void)member;
            const std::size_t before = game.controlledEntity();
            game.cycleLeader();
            const std::size_t after = game.controlledEntity();
            check(after != before && after != kNone, "control_switch");
            if (after != kNone && after != before) {
                InputState go;
                go.forward = true;
                go.run = true;
                const kotor::Vec3 at = scene.entities[after].position;
                for (int i = 0; i < 30; ++i) {
                    game.update(kTick, go);
                    if (game.dialogue().active) {
                        game.endDialogue();
                    }
                }
                note(concat("controlled body moved ", distance(at, scene.entities[after].position),
                            " clip '", clipOf(after), "'"));
                const std::string clip = clipOf(after);
                check(clip == "run" || clip == "runrf" || clip == "runss" || clip == "runst" || clip == "runds",
                      "control_switch_runs");
                check(bodyMesh(scene, before) != nullptr, "control_switch_old_body");
            }
        }
    }

    // ---- the tables' clips exist on the shipped humanoid chain ----------------------------------
    if (const kotor::EntityMesh* body = bodyMesh(scene, game.playerEntity())) {
        for (const WieldCase& c : kWields) {
            for (const char* clip : {c.ready, c.walk, c.run, c.walkDrawn, c.runDrawn, c.attack}) {
                if (body->model->clipNamed(clip).empty()) {
                    note(concat("missing clip ", clip, " for ", c.label));
                    check(false, std::string("chain_has_") + clip);
                }
            }
        }
    }

    // ---- a war droid attacks on its own rows, and a killed creature leaves a body ---------------
    {
        auto mdl = game.install_.find("c_drdwar", 2002);
        auto mdx = game.install_.find("c_drdwar", 3008);
        check(mdl.has_value() && mdx.has_value(), "droid_model");
        if (mdl.has_value() && mdx.has_value()) {
            const kotor::Mdl droid = kotor::Mdl::parse(std::move(*mdl), std::move(*mdx), "c_drdwar.mdl");
            check(lowerOf(attackClipFor(droid, 7, 0)) == "b0a1", "droid_rifle_attack_b0a1");
            check(lowerOf(attackClipFor(droid, 8, 1)) == "g0a2", "droid_melee_attack_g0a2");
            check(lowerOf(deathClipFor(droid)) == "cdie" && lowerOf(deadClipFor(droid)) == "cdead",
                  "droid_death_clips");
        }
        const std::size_t victim = game.findEntity("end_trask");
        kotor::EntityMesh* body = victim == kNone ? nullptr : bodyMesh(scene, victim);
        check(body != nullptr, "death_fixture");
        if (body != nullptr) {
            game.endDialogue();
            scene.entities[victim].plot = false; // Trask is plot-protected in the shipped data.
            scene.entities[victim].min1Hp = false;
            // Kept as a body (SetIsDestroyable FALSE), so the death destroy event (004ff1a4) leaves it.
            ScriptApi{game}.actor(game.objectOfBody(victim)).destroyable = false;
            killCreature(ScriptApi{game}, game.objectOfBody(victim), 0);
            check(scene.entities[victim].corpse(), "killed_creature_is_a_corpse");
            game.update(1.0f / 30.0f, InputState{});
            check(clipOf(victim) == "die", "killed_creature_plays_die");
            for (int tick = 0; tick < 300; ++tick) {
                game.update(1.0f / 30.0f, InputState{});
            }
            check(clipOf(victim) == "dead", "killed_creature_rests_dead");
            check(scene.entities[victim].corpse(), "body_stays_after_ten_seconds");
            // Back from a temporary death the body rises with getupdead (animations.2da row 381)
            // and stands after it. Old rule: the stance controller cut from dead straight to idle.
            resurrectCreature(ScriptApi{game}, game.objectOfBody(victim));
            // Shots land as a ranged round starts now (005b4f30), so keep the risen body out of harm.
            scene.entities[victim].plot = true;
            game.update(1.0f / 30.0f, InputState{});
            const std::string rising = clipOf(victim);
            for (int tick = 0; tick < 45; ++tick) {
                game.update(1.0f / 30.0f, InputState{});
            }
            const std::string midway = clipOf(victim);
            for (int tick = 0; tick < 90; ++tick) {
                game.update(1.0f / 30.0f, InputState{});
            }
            logInfo(concat("animation family: getup ", rising, " ", midway, " ", clipOf(victim)));
            check(rising == "getupdead" && midway == "getupdead" && clipOf(victim) != "getupdead"
                      && clipOf(victim) != "dead",
                  "revived_creature_plays_getupdead");
        }
    }

    // ---- a control entry that pauses the conversation holds its shot (k_ptar_shoot) -------------
    check(game.enterMiniGameModule("tar_m02aa"), "pause_fixture_module");
    {
        float held = 0.0f;
        bool reached = false;
        float offListener = 7.0f;
        // Voices run on the wall clock and these ticks don't, so each line holds for its reading
        // time instead. k_ptar_shoot resumes after 5.2 s.
        game.setDialogueAutoAdvance(true);
        game.setTutorialWindowsEnabledForTest(false);
        for (int tick = 0; tick < 120 * 30 && (tick < 300 || game.dialogueView_.active); ++tick) {
            game.dialogueVoiceStarted_ = false;
            game.update(1.0f / 30.0f, InputState{});
            if (game.dialogueView_.active && game.dialogueView_.entry == 2) {
                reached = true;
                held += 1.0f / 30.0f;
            }
            // Entry 3's speaker has headtrack 0, so it faces its listener (retail 006306e0).
            if (game.dialogueView_.active && game.dialogueView_.entry == 3) {
                const std::size_t speaker = game.entityOfObject(game.dialogueView_.speakerObject);
                const std::size_t listener = game.entityOfObject(game.dialogueView_.listenerObject);
                if (speaker < game.scene_.entities.size() && listener < game.scene_.entities.size()) {
                    const kotor::Entity& a = game.scene_.entities[speaker];
                    const kotor::Entity& b = game.scene_.entities[listener];
                    offListener = std::fabs(std::remainder(
                        std::atan2(b.position.y - a.position.y, b.position.x - a.position.x) - a.bearing,
                        6.2831853f));
                }
            }
        }
        note(concat("textless entry 2 of tar02_preraid held ", held, " s, speaker of entry 3 is off its listener by ",
                    offListener, " rad"));
        check(offListener < 0.01f, "speaker_faces_listener");

        // A war droid (appearance 66, DeathVFX 4034) explodes as it dies; a Sith soldier doesn't.
        std::size_t droid = static_cast<std::size_t>(-1);
        std::size_t soldier = droid;
        for (std::size_t i = 0; i < game.scene_.entities.size(); ++i) {
            const kotor::Entity& entity = game.scene_.entities[i];
            if (entity.kind == kotor::EntityKind::Creature && !entity.dead) {
                if (entity.appearance == 66) {
                    droid = i;
                } else if (entity.appearance == 28 || entity.appearance == 297) {
                    soldier = i;
                }
            }
        }
        check(droid != static_cast<std::size_t>(-1) && soldier != static_cast<std::size_t>(-1),
              "death_vfx_fixtures");
        if (droid != static_cast<std::size_t>(-1) && soldier != static_cast<std::size_t>(-1)) {
            const std::size_t before = game.effects().liveInstances();
            game.onCreatureKilled(soldier, 0);
            const std::size_t afterSoldier = game.effects().liveInstances();
            game.onCreatureKilled(droid, 0);
            check(afterSoldier == before && game.effects().liveInstances() > afterSoldier,
                  "droid_death_plays_its_deathvfx");
            // The explosion throws solid pieces: chunk emitters, with a mesh and no sprite.
            bool pieces = false;
            for (int frame = 0; frame < 120 && !pieces; ++frame) {
                game.update(1.0f / 60.0f, InputState{});
                kotor::stepParticles(game.scene(), 1.0f / 60.0f, game.scene().entities[droid].position);
                pieces = std::any_of(game.scene().emitters.begin(), game.scene().emitters.end(),
                                     [](const kotor::EmitterInstance& emitter) {
                                         return emitter.chunk != nullptr && !emitter.particles.empty();
                                     });
            }
            check(pieces, "droid_explosion_throws_pieces");
        }
        check(reached && held > 5.0f && held < 6.0f, "paused_control_entry_holds");
    }

    logInfo(ok ? "animation family: PASS" : "animation family: FAIL");
    return ok ? 0 : 1;
}

// --capture-action: what a gallery shot does to a creature's hands and mouth. Each goes through
// the same equip, combat flag, clip table and lip track as the game.
bool runAnimationCaptureAction(GameSession& game, const std::string& action)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= action.size()) {
        const std::size_t colon = action.find(':', start);
        parts.push_back(action.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1u;
    }
    if (parts[0] == "lip" && parts.size() == 2) {
        kotor::LipSync lip;
        lip.name = "capture_shape";
        lip.duration = 600.0f;
        const auto shape = static_cast<std::uint8_t>(std::strtoul(parts[1].c_str(), nullptr, 10));
        lip.frames.push_back({0.0f, shape});
        lip.frames.push_back({600.0f, shape});
        game.setLipSyncForTest(std::move(lip));
        return true;
    }
    if (parts[0] == "join" && parts.size() == 2) {
        return game.addPartyMember(parts[1]);
    }
    if (parts.size() < 2 || (parts[0] != "wield" && parts[0] != "combat" && parts[0] != "attack" && parts[0] != "stance")) {
        return false;
    }
    const std::size_t entity = game.findEntity(parts[1]);
    if (entity == static_cast<std::size_t>(-1)) {
        logWarn("capture action: no creature " + parts[1]);
        return true;
    }
    const std::uint32_t object = game.objectOfBody(entity);
    kotor::Entity& held = game.scene_.entities[entity];
    if (parts[0] == "wield") {
        grantWeaponFeats(game, object);
        for (const std::int32_t slot : {4, 5}) {
            const std::uint32_t worn = game.wornItemObject(object, slot);
            if (worn != script::kObjectInvalid) {
                game.unequipItemObject(object, worn);
            }
        }
        for (std::size_t hand = 2; hand < parts.size() && hand < 4; ++hand) {
            const std::uint32_t item = game.giveItem(parts[hand], object, 1, script::kObjectInvalid);
            if (item == script::kObjectInvalid
                || !game.equipItemObject(object, item, static_cast<std::int32_t>(hand) + 2)) {
                logWarn("capture action: cannot wield " + parts[hand]);
            }
        }
        logInfo(concat("capture action: ", parts[1], " holds '", held.weaponModel, "' '", held.offhandModel, "'"));
    } else if (parts[0] == "combat" && parts.size() == 3) {
        ActorCombat& combat = game.sim_->actors[object].combat;
        combat.inCombat = parts[2] == "1";
        combat.combatTimer = combat.inCombat ? 1.0e6f : 0.0f;
    } else if (parts[0] == "attack") {
        game.playAttackClip(entity);
    } else if (parts[0] == "stance" && parts.size() == 3) {
        const kotor::EntityMesh* body = bodyMesh(game.scene_, entity);
        if (body != nullptr) {
            const StanceMotion motion = parts[2] == "run" ? StanceMotion::Run
                : parts[2] == "walk" ? StanceMotion::Walk : StanceMotion::Still;
            const auto actor = game.sim_->actors.find(object);
            const bool drawn = actor != game.sim_->actors.end() && actor->second.combat.inCombat;
            const std::string clip = stanceClipFor(
                *body->model, kotor::wieldClass(held.rightWield, held.leftWield, !held.weaponModel.empty(),
                                                !held.offhandModel.empty()), drawn, motion);
            logInfo(concat("capture action: ", parts[1], " stance clip '", clip, "'"));
            game.playAnimation(entity, clip, true);
        }
    }
    return true;
}

} // namespace ke
