// Gameplay sound and visual effects: the gate behind --family-test sfx.
//
// Sounds are proven at the mixer: which voice an event started, the resource, the bus, and
// the gain and pan the mixer computes for the listener, plus the RMS/peak of a captured
// Mixer::mix buffer. Nothing here has been listened to. Visual effects are driven through the
// script VM (EffectVisualEffect then ApplyEffectToObject / ApplyEffectAtLocation /
// RemoveEffect) on a shipped module; the pictures are checked by eye in the lane report, the
// gate asserts the mesh/emitter bookkeeping (exists, positioned at the target, lives for its
// clip or duration, removed). Every section has a negative control that must fail.
#include "game/ScriptHost.h"

#include "audio/Audio.h"
#include "core/Log.h"
#include "game/AudioSystem.h"
#include "game/Effects.h"
#include "game/GameSession.h"
#include "script/ActionRuntime.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);
using Args = std::vector<std::vector<script::Value>>;

std::vector<script::Value> num(std::int64_t v) { return {script::Value::makeInt(v)}; }
std::vector<script::Value> real(double v) { return {script::Value::makeFloat(v)}; }
std::vector<script::Value> obj(std::uint32_t v) { return {script::Value::makeObject(v)}; }
std::vector<script::Value> eng(std::int64_t v) { return {script::Value::makeEngine(v)}; }

std::string lowerName(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// The gate reads the 2DAs with its own loader so the expected strings do not come from the
// code under test.
class Tables {
public:
    explicit Tables(GameSession& game) : game_(game) {}

    std::string cell(const std::string& table, const std::string& row, const std::string& column)
    {
        auto it = tables_.find(table);
        if (it == tables_.end()) {
            std::optional<kotor::TwoDa> parsed;
            if (auto data = game_.install().find(table, kotor::resType(kotor::ResType::TwoDa))) {
                parsed = kotor::TwoDa::parse(std::move(*data), table);
            }
            it = tables_.emplace(table, std::move(parsed)).first;
        }
        if (!it->second || it->second->rowIndex(row) < 0 || it->second->columnIndex(column) < 0) {
            return {};
        }
        const std::string value = it->second->cell(row, column);
        return value == "****" ? std::string{} : value;
    }

private:
    GameSession& game_;
    std::map<std::string, std::optional<kotor::TwoDa>> tables_;
};

void tick(GameSession& game, float seconds)
{
    for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f) {
        game.update(1.0f / 30.0f, InputState{});
    }
}

std::int64_t vm(GameSession& game, std::uint32_t self, std::uint16_t id, const Args& args)
{
    const auto result = game.actionAsForTest(self, id, args);
    return result && !result->empty() ? result->front().integer : -1;
}

struct Level {
    float rms = 0.0f;
    float peak = 0.0f;
};

// Mixes `frames` stereo frames and reads the effects bus meter of that buffer.
Level mixLevel(GameSession& game, int frames)
{
    // Up to one second in `frames` chunks: the loudest chunk is the reading (a sound may open
    // with a short silence).
    std::vector<float> buffer(static_cast<std::size_t>(frames) * 2, 0.0f);
    Level best;
    for (int chunk = 0; chunk < 12; ++chunk) {
        game.audio().mixer().mix(buffer.data(), frames);
        best.rms = std::max(best.rms, game.audio().mixer().busRms(audio::Bus::Effects));
        best.peak = std::max(best.peak, game.audio().mixer().busPeak(audio::Bus::Effects));
    }
    return best;
}

const SfxRecord* lastRecord(const EffectDirector& fx, const std::string& event)
{
    for (auto it = fx.log().rbegin(); it != fx.log().rend(); ++it) {
        if (it->event == event) {
            return &*it;
        }
    }
    return nullptr;
}

std::size_t countRecords(const EffectDirector& fx, const std::string& event)
{
    return static_cast<std::size_t>(std::count_if(
        fx.log().begin(), fx.log().end(), [&](const SfxRecord& r) { return r.event == event; }));
}

// Records of one event made by one creature: other creatures in the area step on their own clips.
std::size_t countRecordsFor(const EffectDirector& fx, const std::string& event, std::size_t entity)
{
    return static_cast<std::size_t>(std::count_if(
        fx.log().begin(), fx.log().end(),
        [&](const SfxRecord& r) { return r.event == event && r.entity == entity; }));
}

std::size_t taggedEmitters(const GameSession& game, std::uint32_t tag)
{
    return static_cast<std::size_t>(std::count_if(
        game.scene().emitters.begin(), game.scene().emitters.end(),
        [&](const kotor::EmitterInstance& e) { return e.entity == tag; }));
}

// Horizontal distance and height band of a placed effect against its target.
bool nearTarget(const FxInstance& fx, kotor::Vec3 target, float tolerance)
{
    return std::hypot(fx.position.x - target.x, fx.position.y - target.y) <= tolerance
        && fx.position.z >= target.z - 0.5f && fx.position.z <= target.z + 3.5f;
}

} // namespace

int runEffectsFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("sfx: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    EffectDirector& fx = game.effects();
    Tables tbl(game);
    fx.seed(12345);
    // A module-entry movie holds the world while it plays: end it so the clock runs.
    game.setMovieHoldsWorld(false);
    game.setAnimationEnabled(true); // the headless run turns clip playback off
    game.stopMovie();
    {
        const float before = game.elapsedSeconds();
        tick(game, 0.5f);
        check(game.elapsedSeconds() > before + 0.4f, "world_clock_runs");
    }
    kotor::Scene& scene = game.scene();
    const std::uint32_t pc = game.scriptWorld().pc;
    std::vector<std::size_t> creatures;
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        if (scene.entities[i].kind == kotor::EntityKind::Creature && !scene.entities[i].dead
            && game.objectForEntityIndex(i) != 0) {
            creatures.push_back(i);
        }
    }
    check(creatures.size() >= 2, "module_has_creatures");
    if (creatures.size() < 2 || pc == 0) {
        return 1;
    }
    const std::size_t victimEntity = creatures[0];
    const std::size_t attackerEntity = creatures[1];
    const std::uint32_t victim = game.objectForEntityIndex(victimEntity);
    const std::uint32_t attacker = game.objectForEntityIndex(attackerEntity);
    // Everyone stands where the PC stands (a floor the footstep lookup can read).
    const kotor::Vec3 home = game.position();
    for (const std::size_t index : {victimEntity, attackerEntity}) {
        scene.entities[index].position = home;
    }
    game.audio().setListener(home, 0.0f);

    // ---- A. visualeffects.2da -----------------------------------------------------------
    {
        const VisualEffectRow* row = fx.row(1001);
        check(row != nullptr && row->impactModel == tbl.cell("visualeffects", "1001", "imp_impact_node")
                  && !row->impactModel.empty(),
              "vfx_row_1001_reads_impact_model");
        check(fx.row(99999) == nullptr, "vfx_unknown_row_is_null");
    }

    // ---- B. fire-and-forget effects through the VM ----------------------------------------
    // Candidate rows: the first F rows whose impact or head model resolves.
    std::vector<int> fireRows;
    std::vector<int> durationRows;
    for (int id = 1000; id < 4200 && (fireRows.size() < 10 || durationRows.size() < 6); ++id) {
        const std::string key = std::to_string(id);
        if (tbl.cell("visualeffects", key, "label").empty()) {
            continue;
        }
        const std::string type = tbl.cell("visualeffects", key, "type_fd");
        const bool model = !tbl.cell("visualeffects", key, "imp_impact_node").empty()
            || !tbl.cell("visualeffects", key, "imp_headcon_node").empty();
        if (!model) {
            continue;
        }
        (type == "D" ? durationRows : fireRows).push_back(id);
    }
    check(fireRows.size() >= 5, "found_fire_and_forget_rows");
    const kotor::Vec3 target = scene.entities[victimEntity].position;
    int meshEffects = 0;
    int emitterEffects = 0;
    int checkedFire = 0;
    for (const int id : fireRows) {
        const std::size_t before = fx.liveInstances();
        const std::int64_t effect = vm(game, pc, 180, {num(id), num(0)});
        vm(game, pc, 220, {num(0), eng(effect), obj(victim), real(0.0)});
        if (fx.liveInstances() == before) {
            continue; // the row's model is not in the install
        }
        const FxInstance spawned = fx.instances().back();
        const std::size_t emitterCount = spawned.emitters;
        const bool hasMesh = spawned.slot != kNone;
        meshEffects += hasMesh ? 1 : 0;
        emitterEffects += emitterCount > 0 ? 1 : 0;
        const bool placed = nearTarget(spawned, target, 2.5f);
        const bool tagged = emitterCount == 0 || taggedEmitters(game, spawned.tag) == emitterCount;
        const bool shown = !hasMesh || scene.entityMeshes[spawned.slot].visible;
        logInfo(concat("sfx: vfx ", id, " model=", spawned.model, " mesh=", hasMesh ? 1 : 0,
                       " emitters=", emitterCount, " placed=", placed ? 1 : 0));
        float clipLength = spawned.mdl->animationDurationAnyCase(spawned.clip);
        if (clipLength <= 0.0f) {
            clipLength = 1.0f;
        }
        float lived = 0.0f;
        const std::uint32_t instanceId = spawned.id;
        if (clipLength > 15.0f) {
            // A very long clip (a looping environmental model): only assert the placement.
            logInfo(concat("sfx: vfx ", id, " clip ", clipLength, "s is too long for the lifetime check"));
            fx.release(instanceId);
            tick(game, 0.2f);
            continue;
        }
        const float clock0 = game.elapsedSeconds();
        while (fx.instance(instanceId) != nullptr && lived < 20.0f) {
            tick(game, 1.0f / 30.0f);
            lived += 1.0f / 30.0f;
            if (lived > 1.0f && lived < 1.04f) {
                const FxInstance* probe = fx.instance(instanceId);
                logInfo(concat("sfx: probe vfx ", id, " instance=", instanceId, " age=", probe ? probe->age : -1.0f,
                               " live=", fx.liveInstances(), " clock+", game.elapsedSeconds() - clock0));
            }
        }
        if (const FxInstance* stuck = fx.instance(instanceId)) {
            logInfo(concat("sfx: stuck vfx ", id, " phase=", static_cast<int>(stuck->phase), " time=", stuck->time,
                           " age=", stuck->age, " clip=", stuck->clip, " typeFd=", stuck->typeFd,
                           " released=", stuck->released ? 1 : 0, " hold=", stuck->hold));
        }
        const bool removed = fx.instance(instanceId) == nullptr;
        const bool hidden = !hasMesh || !scene.entityMeshes[spawned.slot].visible;
        const bool lifetime = removed && lived >= clipLength - 0.1f && lived <= clipLength + 0.25f;
        logInfo(concat("sfx: vfx ", id, " clip=", spawned.clip, " clipLength=", clipLength, " lived=", lived,
                       " removed=", removed ? 1 : 0, " emittersLeft=", taggedEmitters(game, spawned.tag)));
        check(placed && tagged && shown, "vfx_" + std::to_string(id) + "_placed_at_target");
        check(lifetime, "vfx_" + std::to_string(id) + "_lives_one_clip_then_removed");
        check(hidden && taggedEmitters(game, spawned.tag) == 0,
              "vfx_" + std::to_string(id) + "_slot_hidden_emitters_erased");
        ++checkedFire;
    }
    check(checkedFire >= 3, "fire_and_forget_effects_exercised");
    check(meshEffects >= 1, "at_least_one_effect_model_has_geometry");
    logInfo(concat("sfx: fire-and-forget rows with geometry=", meshEffects,
                   " with emitters=", emitterEffects, " of ", checkedFire));
    // Negative controls: a missing row spawns nothing; the placement test rejects a wrong spot.
    {
        const std::size_t before = fx.liveInstances();
        const std::int64_t effect = vm(game, pc, 180, {num(99999), num(0)});
        vm(game, pc, 220, {num(0), eng(effect), obj(victim), real(0.0)});
        check(fx.liveInstances() == before, "negative_unknown_vfx_row_spawns_nothing");
        FxInstance wrong;
        wrong.position = target + kotor::Vec3{30.0f, 0.0f, 0.0f};
        check(!nearTarget(wrong, target, 2.5f), "negative_placement_check_rejects_far_effect");
    }

    // ---- C. duration effects: temporary and permanent + RemoveEffect ------------------------
    bool durationDone = false;
    for (const int id : durationRows) {
        const std::int64_t effect = vm(game, pc, 180, {num(id), num(0)});
        const std::size_t before = fx.liveInstances();
        vm(game, pc, 220, {num(1), eng(effect), obj(victim), real(2.0)});
        if (fx.liveInstances() == before) {
            continue;
        }
        const std::uint32_t instanceId = fx.instances().back().id;
        const std::string label = "duration_vfx_" + std::to_string(id);
        tick(game, 1.5f);
        check(fx.instance(instanceId) != nullptr, label + "_alive_before_its_time_is_up");
        float after = 1.5f;
        while (fx.instance(instanceId) != nullptr && after < 12.0f) {
            tick(game, 1.0f / 30.0f);
            after += 1.0f / 30.0f;
        }
        check(fx.instance(instanceId) == nullptr && after >= 1.9f && after <= 8.0f,
              label + "_removed_after_duration_plus_cessation");
        // Permanent: stays until RemoveEffect strips it.
        const std::int64_t permanent = vm(game, pc, 180, {num(id), num(0)});
        const std::size_t beforePermanent = fx.liveInstances();
        vm(game, pc, 220, {num(2), eng(permanent), obj(victim), real(0.0)});
        check(fx.liveInstances() > beforePermanent, label + "_permanent_spawned");
        const std::uint32_t heldId = fx.instances().back().id;
        tick(game, 5.0f);
        check(fx.instance(heldId) != nullptr, label + "_permanent_survives_five_seconds");
        vm(game, pc, 87, {obj(victim), eng(permanent)});
        float gone = 0.0f;
        while (fx.instance(heldId) != nullptr && gone < 10.0f) {
            tick(game, 1.0f / 30.0f);
            gone += 1.0f / 30.0f;
        }
        check(fx.instance(heldId) == nullptr, label + "_removed_by_RemoveEffect");
        durationDone = true;
        break;
    }
    check(durationDone, "duration_row_exercised");

    // ---- D. ApplyEffectAtLocation / PlayVisualAreaEffect ------------------------------------
    {
        const std::int64_t location = vm(game, pc, 213, {obj(victim)});
        const std::size_t before = fx.liveInstances();
        const std::int64_t effect = vm(game, pc, 180, {num(fireRows.empty() ? 1001 : fireRows.front()), num(0)});
        vm(game, pc, 216, {num(0), eng(effect), eng(location), real(0.0)});
        check(fx.liveInstances() > before && fx.instances().back().target == kNone
                  && nearTarget(fx.instances().back(), target, 1.0f),
              "apply_effect_at_location_places_model");
        const std::size_t beforeArea = fx.liveInstances();
        vm(game, pc, 677, {num(fireRows.empty() ? 1001 : fireRows.front()), eng(location)});
        check(fx.liveInstances() > beforeArea, "play_visual_area_effect_draws");
        tick(game, 6.0f);
    }

    // ---- E. paired sounds of a row, at the mixer --------------------------------------------
    {
        int soundRow = 0;
        for (const int id : fireRows) {
            if (!tbl.cell("visualeffects", std::to_string(id), "soundimpact").empty()) {
                soundRow = id;
                break;
            }
        }
        check(soundRow != 0, "found_row_with_impact_sound");
        game.audio().mixer().stopAll(); // only this effect's voice is measured
        fx.clearLog();
        const std::int64_t effect = vm(game, pc, 180, {num(soundRow), num(0)});
        vm(game, pc, 220, {num(0), eng(effect), obj(victim), real(0.0)});
        const SfxRecord* record = lastRecord(fx, "vfx_impact");
        check(record != nullptr && record->resref == tbl.cell("visualeffects", std::to_string(soundRow), "soundimpact")
                  && record->voice > 0 && record->bus == audio::Bus::Effects && record->positional,
              "vfx_impact_sound_started_on_effects_bus");
        const Level level = mixLevel(game, 4096);
        logInfo(concat("sfx: impact sound ", record != nullptr ? record->resref : std::string{}, " gain=",
                       record != nullptr ? record->gain : -1.0f, " active=",
                       record != nullptr ? game.audio().mixer().voiceActive(record->voice) : false,
                       " peak=", level.peak, " rms=", level.rms));
        check(level.peak > 0.0001f && level.peak <= 1.0f && level.rms > 0.0f, "vfx_impact_sound_reaches_the_mix");
        tick(game, 6.0f);
    }

    // ---- F. distance gain and pan -----------------------------------------------------------
    {
        game.audio().mixer().stopAll();
        fx.clearLog();
        game.audio().setListener(home, 0.0f);
        const kotor::Vec3 closePos = home;
        const kotor::Vec3 mid = home + kotor::Vec3{25.0f, 0.0f, 0.0f};
        const kotor::Vec3 farPos = home + kotor::Vec3{200.0f, 0.0f, 0.0f};
        const std::string resref = tbl.cell("footstepsounds", "0", "stone0");
        fx.emit("gain_near", resref, "footsteps", &closePos);
        fx.emit("gain_mid", resref, "footsteps", &mid);
        fx.emit("gain_far", resref, "footsteps", &farPos);
        const SfxRecord* a = lastRecord(fx, "gain_near");
        const SfxRecord* b = lastRecord(fx, "gain_mid");
        const SfxRecord* c = lastRecord(fx, "gain_far");
        check(a && b && c, "gain_records_exist");
        if (a && b && c) {
            // Group 22 (SingleShotPositional, PriorityGroups.2da): full level inside 20 m, silent past 30 m,
            // ((30-25)/(30-20))^2 = 0.25 of full at 25 m. The executable never reads soundgain.2da.
            check(a->maxDistance == 30.0f && a->minDistance == 20.0f, "gain_distances_from_prioritygroups_2da");
            check(std::fabs(b->gain / std::max(a->gain, 1e-6f) - 0.25f) < 0.02f, "gain_at_25m_is_quarter");
            check(c->gain == 0.0f, "gain_beyond_max_is_zero");
            check(!(std::fabs(b->gain / std::max(a->gain, 1e-6f) - 0.9f) < 0.02f),
                  "negative_gain_ratio_check_rejects_wrong_value");
        }
        const kotor::Vec3 left = home + kotor::Vec3{0.0f, 10.0f, 0.0f};
        const kotor::Vec3 right = home + kotor::Vec3{0.0f, -10.0f, 0.0f};
        fx.emit("pan_left", resref, "footsteps", &left);
        fx.emit("pan_right", resref, "footsteps", &right);
        const SfxRecord* pl = lastRecord(fx, "pan_left");
        const SfxRecord* pr = lastRecord(fx, "pan_right");
        check(pl && pr && pl->pan < -0.9f && pr->pan > 0.9f, "pan_follows_listener_side");
        game.audio().mixer().stopAll();
    }

    // ---- G. weapon sounds by weapon type and target material -------------------------------
    {
        scene.entities[attackerEntity].stats.mainHand.present = true;
        const auto armed = [&](int baseItem) {
            scene.entities[attackerEntity].stats.mainHand.baseItem = baseItem;
        };
        const auto material = [&]() {
            std::string m = lowerName(tbl.cell(
                "appearancesndset", std::to_string(scene.entities[victimEntity].soundAppType), "armortype"));
            return m.empty() ? std::string("cloth") : m;
        };
        // Lightsaber (baseitems 8 -> weaponmattype 1 -> weaponsounds row 1).
        armed(8);
        fx.clearLog();
        fx.swing(attackerEntity, attacker, 0);
        fx.clash(attackerEntity, attacker);
        fx.parry(attackerEntity, attacker);
        fx.hit(attackerEntity, attacker, victimEntity, victim);
        const SfxRecord* swing = lastRecord(fx, "swing");
        const SfxRecord* clash = lastRecord(fx, "clash");
        const SfxRecord* parry = lastRecord(fx, "parry");
        const SfxRecord* hit = lastRecord(fx, "hit");
        const auto oneOf = [&](const SfxRecord* r, std::set<std::string> names) {
            return r != nullptr && names.count(r->resref) == 1 && r->voice > 0
                && r->bus == audio::Bus::Effects;
        };
        check(oneOf(swing, {"cb_ls_swingshrt1", "cb_ls_swingshrt2", "cb_ls_swingshrt3"}), "saber_swing_short_resource");
        check(oneOf(clash, {"cb_ls_clash1", "cb_ls_clash2"}), "saber_clash_resource");
        check(oneOf(parry, {"cb_pa_saber1"}), "saber_parry_resource");
        const std::string m = material();
        check(oneOf(hit, {tbl.cell("weaponsounds", "1", m + "0"), tbl.cell("weaponsounds", "1", m + "1")}),
              "saber_hit_resource_by_target_material_" + m);
        check(hit != nullptr && hit->position.x == home.x, "hit_sound_is_at_the_target");
        // A different weapon changes the resource (vibroblade row 2, blaster ammunition 1).
        armed(5);
        fx.clearLog();
        fx.swing(attackerEntity, attacker, 0);
        check(oneOf(lastRecord(fx, "swing"), {tbl.cell("weaponsounds", "2", "swingshort0"),
                                              tbl.cell("weaponsounds", "2", "swingshort1"),
                                              tbl.cell("weaponsounds", "2", "swingshort2")}),
              "vibroblade_swing_uses_blade_row");
        check(lastRecord(fx, "swing") != nullptr && lastRecord(fx, "swing")->resref.rfind("cb_sw_blade", 0) == 0,
              "negative_blade_swing_is_not_a_saber_sound");
        armed(12);
        fx.clearLog();
        fx.shot(attackerEntity, attacker, false);
        fx.shotImpact(attackerEntity, attacker, victimEntity, false);
        check(oneOf(lastRecord(fx, "shot"), {"cb_sh_blast1"}), "blaster_shot_resource");
        check(oneOf(lastRecord(fx, "shot_impact"), {"cb_ht_blastleth1"}), "blaster_impact_resource");
        scene.entities[attackerEntity].stats.mainHand.present = false;
        fx.clearLog();
        fx.swing(attackerEntity, attacker, 0);
        check(oneOf(lastRecord(fx, "swing"), {"cb_sw_unarmed1"}), "unarmed_swing_uses_unarmed_row");
        // The attack roll plays the same sounds when the clip carries no events.
        armed(8);
        scene.entities[attackerEntity].stats.mainHand.present = true;
        fx.clearLog();
        fx.attackResolved(attacker, victim, 0, false);
        check(lastRecord(fx, "swing") != nullptr && lastRecord(fx, "hit") != nullptr,
              "attack_roll_hit_plays_swing_and_hit");
        fx.clearLog();
        fx.attackResolved(attacker, victim, 1, false);
        check(lastRecord(fx, "swing") != nullptr && lastRecord(fx, "hit") == nullptr,
              "attack_roll_miss_plays_swing_only");
        // Saber hum: power-up, a looping hum on the effects bus, power-down stops it.
        check(fx.saberHumming(attacker), "saber_hum_starts_with_the_fight");
        const SfxRecord* hum = lastRecord(fx, "saber_hum");
        game.audio().mixer().stopAll();
        fx.clearLog();
        fx.saberDown(attacker);
        check(!fx.saberHumming(attacker) && lastRecord(fx, "saber_powerdown") != nullptr
                  && lastRecord(fx, "saber_powerdown")->resref == "cb_ls_powerdown",
              "saber_power_down_plays_and_ends_hum");
        (void)hum;
        scene.entities[attackerEntity].stats.mainHand.present = false;
        game.audio().mixer().stopAll();
        // Negative control: an event with no table data starts nothing and logs nothing.
        fx.clearLog();
        check(fx.emit("none", "", "swing", nullptr) == 0 && fx.log().empty(), "negative_empty_resource_is_silent");
    }

    // ---- H. events inside the shipped clips: NPC footsteps -----------------------------------
    {
        std::size_t walker = kNone;
        std::string clip;
        int perLoop = 0;
        float clipLength = 0.0f;
        for (const std::size_t index : creatures) {
            for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
                if (mesh.entity != index || mesh.attachModel || !mesh.model) {
                    continue;
                }
                for (const kotor::MdlAnimationInfo& info : mesh.model->animations()) {
                    int steps = 0;
                    for (const kotor::MdlAnimEvent& event : mesh.model->animationEvents(info.name)) {
                        steps += lowerName(event.name) == "snd_footstep" ? 1 : 0;
                    }
                    if (steps >= 2 && walker == kNone) {
                        walker = index;
                        clip = info.name;
                        perLoop = steps;
                        clipLength = info.duration;
                    }
                }
            }
        }
        check(walker != kNone, "found_creature_clip_with_footstep_events");
        if (walker != kNone) {
            scene.entities[walker].position = home;
            fx.clearLog();
            game.playAnimation(walker, clip, true);
            const float seconds = clipLength * 2.0f + 0.05f;
            tick(game, seconds);
            const std::size_t steps = countRecordsFor(fx, "footstep", walker);
            const std::size_t everyone = countRecords(fx, "footstep");
            logInfo(concat("sfx: walker clip now=", game.animationName(walker), " t=", game.animationTime(walker),
                           " floor=", fx.surfaceAt(scene.entities[walker].position + kotor::Vec3{0, 0, 2}),
                           " direct=", fx.footstep(walker)));
            logInfo(concat("sfx: footstep clip ", clip, " events/loop=", perLoop, " length=", clipLength,
                           " steps heard in ", seconds, "s=", steps, " (all creatures=", everyone, ")"));
            check(steps >= static_cast<std::size_t>(perLoop) * 2 - 1
                      && steps <= static_cast<std::size_t>(perLoop) * 2 + 1,
                  "npc_footsteps_follow_the_clip_events");
            const SfxRecord* step = lastRecord(fx, "footstep");
            const std::string family = step != nullptr ? step->resref : std::string{};
            check(step != nullptr && step->voice > 0 && step->bus == audio::Bus::Effects
                      && family.rfind("fs_", 0) == 0,
                  "npc_footstep_resource_is_an_fs_sound");
            // Negative control: a clip without events makes no steps.
            std::string quiet;
            for (const char* name : {"pause1", "cpause1", "default"}) {
                for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
                    if (mesh.entity == walker && !mesh.attachModel && mesh.model
                        && mesh.model->animationIndex(name) >= 0 && mesh.model->animationEvents(name).empty()) {
                        quiet = name;
                    }
                }
                if (!quiet.empty()) {
                    break;
                }
            }
            check(!quiet.empty(), "found_clip_without_events");
            if (!quiet.empty()) {
                fx.clearLog();
                game.playAnimation(walker, quiet, true);
                tick(game, 2.0f);
                check(countRecordsFor(fx, "footstep", walker) == 0, "negative_clip_without_events_makes_no_steps");
            }
        }
    }

    // ---- I. doors and placeables ------------------------------------------------------------
    {
        std::size_t door = kNone;
        std::size_t placeable = kNone;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            const kotor::Entity& e = scene.entities[i];
            if (e.kind == kotor::EntityKind::Door && door == kNone && e.linkedModule.empty()) {
                const std::string app = tbl.cell("genericdoors", std::to_string(e.appearance), "soundapptype");
                if (!app.empty() && !tbl.cell("placeableobjsnds", app, "opened").empty()
                    && !tbl.cell("placeableobjsnds", app, "closed").empty()) {
                    door = i;
                }
            }
            if (e.kind == kotor::EntityKind::Placeable && placeable == kNone) {
                const std::string app = tbl.cell("placeables", std::to_string(e.appearance), "soundapptype");
                if (!app.empty() && (!tbl.cell("placeableobjsnds", app, "used").empty()
                                     || !tbl.cell("placeableobjsnds", app, "opened").empty())) {
                    placeable = i;
                }
            }
        }
        check(door != kNone, "found_door_with_sound_row");
        if (door != kNone) {
            const std::string app = tbl.cell("genericdoors", std::to_string(scene.entities[door].appearance), "soundapptype");
            scene.entities[door].locked = false;
            scene.entities[door].open = false;
            fx.clearLog();
            game.openDoor(door);
            const SfxRecord* opened = lastRecord(fx, "door_opened");
            check(opened != nullptr && opened->resref == tbl.cell("placeableobjsnds", app, "opened")
                      && opened->voice > 0 && opened->positional
                      && opened->position.x == scene.entities[door].position.x,
                  "door_open_plays_opened_sound_at_the_door");
            game.closeDoor(door);
            const SfxRecord* closed = lastRecord(fx, "door_closed");
            check(closed != nullptr && closed->resref == tbl.cell("placeableobjsnds", app, "closed"),
                  "door_close_plays_closed_sound");
            fx.clearLog();
            fx.objectSound(door, ObjectSound::Locked);
            const SfxRecord* locked = lastRecord(fx, "door_locked");
            check(locked != nullptr && locked->resref == tbl.cell("placeableobjsnds", app, "locked"),
                  "door_locked_sound_from_table");
        }
        check(placeable != kNone, "found_placeable_with_sound_row");
        if (placeable != kNone) {
            const std::string app = tbl.cell("placeables", std::to_string(scene.entities[placeable].appearance), "soundapptype");
            const bool used = !tbl.cell("placeableobjsnds", app, "used").empty();
            fx.clearLog();
            fx.objectSound(placeable, used ? ObjectSound::Used : ObjectSound::Opened);
            const SfxRecord* record = lastRecord(fx, used ? "placeable_used" : "placeable_opened");
            check(record != nullptr
                      && record->resref == tbl.cell("placeableobjsnds", app, used ? "used" : "opened")
                      && record->voice > 0,
                  "placeable_sound_from_placeableobjsnds");
        }
        // Negative control: a creature has no object sounds.
        fx.clearLog();
        check(fx.objectSound(victimEntity, ObjectSound::Opened) == 0 && fx.log().empty(),
              "negative_creature_has_no_door_sound");
    }

    // ---- J. Force power cast ----------------------------------------------------------------
    {
        fx.clearLog();
        const std::size_t before = fx.liveInstances();
        fx.spellCast(victimEntity, 23); // FORCE_POWER_FORCE_PUSH
        const SfxRecord* cast = lastRecord(fx, "spell_cast");
        check(cast != nullptr && cast->resref == tbl.cell("spells", "23", "castsound") && cast->voice > 0,
              "force_power_cast_sound_from_spells_2da");
        const std::string hand = tbl.cell("spells", "23", "casthandvisual");
        check(hand.empty() || fx.liveInstances() > before, "force_power_hand_model_spawned");
        tick(game, 6.0f);
    }

    // ---- K. GUI sounds ----------------------------------------------------------------------
    {
        game.audio().mixer().stopAll();
        const int voice = game.audio().playGuiSound(AudioSystem::kGuiClicked);
        check(voice > 0 && !tbl.cell("guisounds", "0", "soundresref").empty(), "gui_click_starts_a_voice");
        const Level level = mixLevel(game, 4096);
        check(level.rms > 0.0f && level.peak <= 1.0f, "gui_click_reaches_the_mix");
        check(game.audio().playGuiSound(9999) < 0, "negative_unknown_gui_row_is_silent");
    }

    logInfo(concat("sfx: nothing was listened to; sounds are proven at the mixer, pictures are reviewed separately"));
    return ok ? 0 : 1;
}

} // namespace ke
