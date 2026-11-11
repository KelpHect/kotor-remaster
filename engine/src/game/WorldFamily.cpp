// The 3D world: the gate behind --family-test world3d, and the shipped-texture audit behind
// --family-test worldtextures.
//
// Every assertion reads a Scene built from the install. A pass proves the data reached the
// renderer's inputs; what the frames look like is checked by viewing the gallery's "world" shots.
#include "game/WorldFamily.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "kotor/Formats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// Vertices at one position in one room should read about the same light. Summed RGB, 0-765.
float lightmapSeam(const kotor::Scene& scene)
{
    struct Key {
        int x, y, z;
        std::uint32_t lightmap;
        bool operator<(const Key& o) const
        {
            return std::tie(x, y, z, lightmap) < std::tie(o.x, o.y, o.z, o.lightmap);
        }
    };
    std::map<Key, std::array<int, 3>> seen;
    double total = 0.0;
    std::size_t pairs = 0;
    for (const kotor::TextureBatch& batch : scene.batches) {
        if (batch.lightmap == kotor::kNoTexture) {
            continue;
        }
        const kotor::Image& image = scene.textures[batch.lightmap].image;
        for (std::uint32_t i = 0; i < batch.indexCount; ++i) {
            const kotor::SceneVertex& vertex = scene.vertices[scene.indices[batch.firstIndex + i]];
            const float u = vertex.uv1.x - std::floor(vertex.uv1.x);
            const float v = vertex.uv1.y - std::floor(vertex.uv1.y);
            const int px = std::clamp(static_cast<int>(u * static_cast<float>(image.width)), 0, image.width - 1);
            const int py = std::clamp(static_cast<int>(v * static_cast<float>(image.height)), 0, image.height - 1);
            const std::uint8_t* texel = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4u;
            const Key key{static_cast<int>(std::lround(vertex.position.x * 100.0f)),
                          static_cast<int>(std::lround(vertex.position.y * 100.0f)),
                          static_cast<int>(std::lround(vertex.position.z * 100.0f)), batch.lightmap};
            const std::array<int, 3> colour{texel[0], texel[1], texel[2]};
            const auto [it, inserted] = seen.try_emplace(key, colour);
            if (!inserted) {
                total += std::abs(it->second[0] - colour[0]) + std::abs(it->second[1] - colour[1])
                    + std::abs(it->second[2] - colour[2]);
                ++pairs;
            }
        }
    }
    return pairs != 0 ? static_cast<float>(total / static_cast<double>(pairs)) : 1000.0f;
}

} // namespace

int runWorldFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("world3d: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    kotor::Install& install = game.install();

    // ---- Lightmaps: the second UV set reaches a shipped lightmap, in file row order.
    kotor::Scene ship = kotor::loadModule(install, "end_m01aa");
    std::size_t lightmapped = 0;
    std::set<std::uint32_t> lightmaps;
    for (const kotor::TextureBatch& batch : ship.batches) {
        if (batch.lightmap != kotor::kNoTexture) {
            ++lightmapped;
            lightmaps.insert(batch.lightmap);
        }
    }
    logInfo(concat("world3d: end_m01aa batches ", ship.batches.size(), " lightmapped ", lightmapped,
                   " lightmaps ", lightmaps.size()));
    check(lightmapped * 2u > ship.batches.size(), "most_room_batches_lightmapped");
    check(!lightmaps.empty(), "lightmaps_loaded");
    bool fileOrder = !lightmaps.empty();
    for (const std::uint32_t index : lightmaps) {
        const kotor::Texture& texture = ship.textures[index];
        std::uint32_t type = 0;
        const auto data = install.findTexture(texture.name, &type);
        if (!data.has_value() || type != kotor::resType(kotor::ResType::Tga) || data->size() < 22u
            || (*data)[2] != 2u || (*data)[17] != 0u || (*data)[16] != 32u) {
            continue;
        }
        // First stored row (BGRA after the 18-byte header) is row 0 of the scene image.
        const std::uint8_t* stored = data->data() + 18u + (*data)[0];
        for (int x = 0; x < texture.image.width; ++x) {
            const std::uint8_t* out = texture.image.rgba.data() + static_cast<std::size_t>(x) * 4u;
            fileOrder = fileOrder && out[0] == stored[x * 4 + 2] && out[1] == stored[x * 4 + 1]
                && out[2] == stored[x * 4];
        }
    }
    check(fileOrder, "lightmap_rows_in_file_order");
    const float seam = lightmapSeam(ship);
    logInfo(concat("world3d: lightmap seam ", seam));
    // Upright rows measured 184 on these rooms; file order 49.
    check(seam < 100.0f, "lightmap_seams_continuous");

    // ---- ARE: fog and light come from the fields retail reads (00508c50).
    check(!ship.fogOn, "ship_fog_off_as_authored");
    check(length(ship.dynamicAmbient) > 0.1f, "dynamic_ambient_read");
    kotor::Scene dantooine = kotor::loadModule(install, "danm13");
    check(dantooine.fogOn && std::abs(dantooine.fogNear - 25.0f) < 0.01f && std::abs(dantooine.fogFar - 500.0f) < 0.01f,
          "danm13_fog_fields");
    check(std::abs(dantooine.fogColor.x - 214.0f / 255.0f) < 0.01f && std::abs(dantooine.fogColor.y - 171.0f / 255.0f) < 0.01f,
          "danm13_fog_colour");

    // ---- Grass: only where the ARE and walkmesh ask for it, lit by the floor's lightmap.
    const auto grassBatches = [](const kotor::Scene& scene) {
        std::size_t count = 0;
        std::size_t lit = 0;
        for (const kotor::TextureBatch& batch : scene.batches) {
            if (batch.maxDistance > 0.0f) {
                ++count;
                lit += batch.lightmap != kotor::kNoTexture ? 1u : 0u;
            }
        }
        return std::pair<std::size_t, std::size_t>(count, lit);
    };
    const auto [grassCount, grassLit] = grassBatches(dantooine);
    logInfo(concat("world3d: danm13 grass batches ", grassCount, " lit ", grassLit));
    check(grassCount > 0u, "danm13_grass_present");
    check(grassLit * 2u > grassCount, "grass_uses_floor_lightmap");
    check(grassBatches(ship).first == 0u, "ship_has_no_grass");

    // ---- VIS: the camera's room and its list are drawn, and rooms outside it are not.
    check(!ship.visibility.empty(), "vis_loaded");
    kotor::updateVisibleRooms(ship, ship.entry + kotor::Vec3{0.0f, 0.0f, 1.5f});
    const auto visible = std::count_if(ship.visibleRooms.begin(), ship.visibleRooms.end(),
                                       [](std::uint8_t room) { return room != 0u; });
    logInfo(concat("world3d: visible rooms ", visible, "/", ship.rooms.size()));
    check(visible > 0 && static_cast<std::size_t>(visible) < ship.rooms.size(), "vis_culls_some_rooms");

    // ---- Materials: TXI state on the batch.
    const kotor::TxiInfo additive = kotor::parseTxi("blending additive\r\nnumx 2\nnumy 4\nfps 8\nproceduretype cycle\n");
    check(additive.blending == kotor::TxiInfo::Blend::Additive && additive.numX == 2 && additive.numY == 4
              && additive.procedure == kotor::TxiInfo::Procedure::Cycle, "txi_parse");
    kotor::TextureBatch cycle;
    cycle.cycleX = 2;
    cycle.cycleY = 4;
    cycle.cycleFps = 8.0f;
    const kotor::BatchUv at0 = kotor::batchUvAt(cycle, 0.0f);
    const kotor::BatchUv at1 = kotor::batchUvAt(cycle, 0.25f); // Frame 2 of 8: second column wraps.
    check(at0.scaleU == 0.5f && at0.scaleV == 0.25f && at0.offsetU == 0.0f, "cycle_frame_scale");
    check(at1.offsetU == 0.0f && at1.offsetV == 0.25f, "cycle_frame_advances");
    kotor::TextureBatch scroll;
    scroll.scrollU = 0.5f;
    // Retail 00483630 adds the uv direction once per drawn frame (60 a second): one frame moves 0.5.
    check(std::abs(kotor::batchUvAt(scroll, 1.0f / 60.0f).offsetU - 0.5f) < 1e-4f, "uv_scroll");
    std::size_t env = 0;
    std::size_t alphaTested = 0;
    for (const kotor::TextureBatch& batch : ship.batches) {
        env += batch.envMask && batch.envmap != kotor::kNoTexture ? 1u : 0u;
        alphaTested += batch.alphaRef >= 0.0f ? 1u : 0u;
    }
    logInfo(concat("world3d: env batches ", env, " alpha tested ", alphaTested));
    check(env > 0u, "ship_floor_has_env_map");

    // ---- Render=0 nodes stay out, and invented quads are gone.
    bool noFakeTextures = true;
    for (const kotor::Texture& texture : ship.textures) {
        noFakeTextures = noFakeTextures && texture.name.rfind("__fx_", 0) != 0;
    }
    check(noFakeTextures, "no_generated_effect_textures");

    // ---- Room loop clips: the moving meshes left the static batches and really move.
    std::size_t roomMeshes = 0;
    bool roomMoves = false;
    for (kotor::EntityMesh& mesh : ship.entityMeshes) {
        if (!mesh.roomAnimated) {
            continue;
        }
        ++roomMeshes;
        const std::vector<kotor::SceneVertex> before = mesh.vertices;
        mesh.animationTime = std::max(mesh.model->animationDuration(mesh.animation) * 0.4f, 0.01f);
        kotor::refreshEntityMesh(mesh);
        for (std::size_t i = 0; i < before.size() && i < mesh.vertices.size() && !roomMoves; ++i) {
            roomMoves = length(before[i].position - mesh.vertices[i].position) > 1e-3f
                || length(before[i].selfIllum - mesh.vertices[i].selfIllum) > 1e-3f;
        }
    }
    logInfo(concat("world3d: room loop meshes ", roomMeshes));
    check(roomMeshes > 0u, "room_loop_meshes_found");
    check(roomMoves, "room_loop_meshes_change");

    // ---- Emitters: nodes come from the MDL, spawn their shipped texture, and die.
    logInfo(concat("world3d: emitters ", ship.emitters.size()));
    check(!ship.emitters.empty(), "emitters_from_models");
    bool emitterTextures = true;
    for (const kotor::EmitterInstance& emitter : ship.emitters) {
        emitterTextures = emitterTextures && emitter.texture < ship.textures.size() && emitter.texture != 0u;
    }
    check(emitterTextures, "emitters_use_shipped_textures");
    std::size_t alive = 0;
    kotor::Vec3 focus = ship.entry;
    kotor::stepParticles(ship, 0.0f, focus); // Places every node.
    if (!ship.emitters.empty()) {
        std::size_t fountain = ship.emitters.size();
        for (std::size_t i = 0; i < ship.emitters.size(); ++i) {
            if (lower(ship.emitters[i].header.update) == "fountain"
                && ship.emitters[i].entity == UINT32_MAX) {
                fountain = i;
                break;
            }
        }
        if (fountain < ship.emitters.size()) {
            focus = ship.emitters[fountain].worldPosition + ship.emitters[fountain].origin;
        }
    }
    for (int step = 0; step < 60; ++step) {
        kotor::stepParticles(ship, 0.033f, focus);
    }
    bool particlesFinite = true;
    for (const kotor::EmitterInstance& emitter : ship.emitters) {
        alive += emitter.particles.size();
        for (const kotor::Particle& particle : emitter.particles) {
            particlesFinite = particlesFinite && std::isfinite(particle.position.x) && std::isfinite(particle.position.z);
        }
    }
    logInfo(concat("world3d: particles alive ", alive));
    check(alive > 0u, "particles_spawn");
    check(particlesFinite, "particles_finite");
    kotor::buildParticleMesh(ship, focus, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    check(!ship.particleMesh.vertices.empty() && !ship.particleMesh.batches.empty(), "particle_mesh_built");

    // ---- Weapons: changing the worn model replaces the hand mesh in its slot.
    std::size_t creature = ship.entities.size();
    std::size_t weaponSlot = ship.entityMeshes.size();
    for (std::size_t i = 0; i < ship.entityMeshes.size(); ++i) {
        const kotor::EntityMesh& mesh = ship.entityMeshes[i];
        if (mesh.attachModel && mesh.entity < ship.entities.size()) {
            creature = mesh.entity;
            weaponSlot = i;
            break;
        }
    }
    check(creature < ship.entities.size(), "armed_creature_found");
    if (creature < ship.entities.size()) {
        const std::size_t meshCount = ship.entityMeshes.size();
        const std::uint32_t revision = ship.meshRevision;
        const std::string oldModel = ship.entityMeshes[weaponSlot].model->name();
        ship.entities[creature].weaponModel = lower(oldModel).find("w_blstrpstl") == std::string::npos
            ? "w_blstrpstl_001" : "w_vbroswrd_001";
        ship.entities[creature].offhandModel.clear();
        kotor::rebuildEntityWeapons(ship, install, creature);
        check(ship.meshRevision != revision, "weapon_change_bumps_revision");
        check(ship.entityMeshes.size() == meshCount, "weapon_change_keeps_mesh_slots");
        check(ship.entityMeshes[weaponSlot].visible && ship.entityMeshes[weaponSlot].model->name() != oldModel,
              "weapon_mesh_replaced");
        ship.entities[creature].weaponModel.clear();
        kotor::rebuildEntityWeapons(ship, install, creature);
        check(!ship.entityMeshes[weaponSlot].visible, "unequip_hides_weapon");
    }
    // ---- Doors and placeables rest in the clip of their state; meshes keep an identity.
    {
        bool loop = false;
        const char* activate = kotor::placeableScriptClip(200, loop);
        check(activate != nullptr && std::string(activate) == "off2on" && !loop, "placeable_activate_clip");
        const char* close = kotor::placeableScriptClip(203, loop);
        check(close != nullptr && std::string(close) == "open2close" && !loop, "placeable_close_clip");
        const char* idle = kotor::placeableScriptClip(204, loop);
        check(idle != nullptr && std::string(idle) == "animloop01" && loop, "placeable_animloop_clip");
        check(kotor::placeableScriptClip(199, loop) == nullptr, "other_animations_are_not_placeable_clips");
        bool doorChecked = false;
        for (const kotor::EntityMesh& mesh : ship.entityMeshes) {
            if (mesh.entity >= ship.entities.size() || !mesh.model
                || ship.entities[mesh.entity].kind != kotor::EntityKind::Door
                || mesh.model->animationDuration("opened1") <= 0.0f) {
                continue;
            }
            kotor::Entity door = ship.entities[mesh.entity];
            door.open = true;
            door.openState = 1;
            check(kotor::objectStateClip(door, *mesh.model).name == "opened1", "open_door_rests_open");
            door.open = false;
            door.openState = 0;
            check(kotor::objectStateClip(door, *mesh.model).name != "opened1", "closed_door_is_not_open");
            doorChecked = true;
            break;
        }
        check(doorChecked, "door_with_open_clip_found");
        kotor::EntityMesh first;
        kotor::EntityMesh second;
        const std::uint64_t uid = first.uid;
        check(uid != second.uid, "mesh_ids_are_distinct");
        kotor::EntityMesh moved = std::move(first);
        check(moved.uid == uid, "mesh_id_survives_a_move");
    }
    // ---- Creature light: at most three lights, in retail's order, with its attenuation.
    {
        kotor::Scene lit;
        lit.ambient = {};
        lit.diffuse = {};
        const auto add = [&](kotor::Vec3 at, float radius, int priority, int type) {
            kotor::Scene::WorldLight light;
            light.position = at;
            light.colour = {1.0f, 1.0f, 1.0f};
            light.radius = radius;
            light.priority = priority;
            light.dynamicType = type;
            lit.lights.push_back(light);
        };
        add({1.0f, 0.0f, 0.0f}, 10.0f, 5, 2);
        add({2.0f, 0.0f, 0.0f}, 10.0f, 1, 2);
        add({3.0f, 0.0f, 0.0f}, 10.0f, 1, 1);
        add({4.0f, 0.0f, 0.0f}, 10.0f, 1, 2);
        add({0.5f, 0.0f, 0.0f}, 10.0f, 9, 2);
        add({50.0f, 0.0f, 0.0f}, 10.0f, 0, 1);
        const kotor::SampledLight sampled = kotor::sampleSceneLight(lit, {0.0f, 0.0f, 0.0f});
        check(sampled.lights[0].position.x == 3.0f, "light_order_type_one_first");
        check(sampled.lights[1].position.x == 2.0f, "light_order_nearer_next");
        check(sampled.lights[2].position.x == 4.0f, "light_capped_at_three");
        check(std::abs(sampled.lights[0].quadratic - 0.045f) < 1e-5f, "light_attenuation_from_radius");
        kotor::Scene glow;
        glow.ambient = {};
        glow.diffuse = {};
        kotor::Scene::WorldLight ambientLight;
        ambientLight.position = {0.0f, 0.0f, 2.0f};
        ambientLight.colour = {1.0f, 1.0f, 1.0f};
        ambientLight.radius = 2.0f;
        ambientLight.ambientOnly = true;
        glow.lights.push_back(ambientLight);
        const kotor::SampledLight glowing = kotor::sampleSceneLight(glow, {0.0f, 0.0f, 0.0f});
        check(std::abs(glowing.ambient.x - 1.0f / 5.5f) < 1e-4f, "ambient_only_light_adds_ambient");
        check(glowing.lights[0].colour.x == 0.0f, "ambient_only_light_has_no_diffuse");
    }
    return ok ? 0 : 1;
}

int runWorldTextureAudit(GameSession& game)
{
    kotor::Install& install = game.install();
    const std::filesystem::path modules = install.root() / "modules";
    std::set<std::string> stems;
    for (const auto& entry : std::filesystem::directory_iterator(modules)) {
        const std::string name = entry.path().filename().string();
        const std::string low = lower(name);
        if (low.ends_with(".rim") && !low.ends_with("_s.rim")) {
            stems.insert(entry.path().stem().string());
        }
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>> totals; // lookups, modules
    std::size_t lookups = 0;
    std::size_t scenes = 0;
    std::size_t failedScenes = 0;
    for (const std::string& stem : stems) {
        try {
            kotor::Scene scene = kotor::loadModule(install, stem);
            ++scenes;
            for (const auto& [name, count] : scene.missingTextures) {
                totals[name].first += count;
                ++totals[name].second;
                lookups += count;
            }
            logInfo(concat("worldtextures: ", stem, " textures ", scene.textures.size(), " missing names ",
                           scene.missingTextures.size()));
        } catch (const std::exception& exception) {
            ++failedScenes;
            logWarn(concat("worldtextures: ", stem, " failed: ", exception.what()));
        }
    }
    for (const auto& [name, count] : totals) {
        logInfo(concat("worldtextures: missing ", name, " lookups ", count.first, " modules ", count.second));
    }
    logInfo(concat("worldtextures: modules ", scenes, " failed ", failedScenes, " failed lookups ", lookups,
                   " distinct names ", totals.size()));
    // Measured over the 117 shipped modules: lightmaps the MDLs name but the install does not
    // carry, plus fx_lsabre01 and lkofx_ripple2. More than this means a new lookup went wrong.
    return failedScenes == 0u && scenes == 117u && lookups <= 65u ? 0 : 1;
}

// --family-test worldposes: every creature and placeable of every shipped module, after a second
// of the module's own start: how its posed mesh sits on the walkmesh, and how far the pose is from
// the model's rest height. Offenders are logged; the limit is what the shipped data measures.
int runWorldPoseSurvey(GameSession& first)
{
    const std::filesystem::path modules = first.install().root() / "modules";
    std::set<std::string> stems;
    for (const auto& entry : std::filesystem::directory_iterator(modules)) {
        const std::string low = lower(entry.path().filename().string());
        if (low.ends_with(".rim") && !low.ends_with("_s.rim")) {
            stems.insert(entry.path().stem().string());
        }
    }
    std::size_t bodies = 0;
    std::size_t floating = 0;
    std::size_t sunk = 0;
    std::size_t folded = 0;
    std::size_t failed = 0;
    std::size_t standIns = 0;
    std::size_t roomLoopMeshes = 0;
    std::size_t roomLoopMoved = 0;
    std::size_t roomLoopTinted = 0;
    std::size_t roomLoopStill = 0;
    double roomLoopWorstMs = 0.0;
    double roomLoopTotalMs = 0.0;
    double lightSampleUs = 0.0;
    double standInWorstMs = 0.0;
    std::size_t standInWorstCount = 0;
    bool skyReached = false;
    std::size_t lightSamples = 0;
    std::map<std::string, std::size_t> objectStates;
    for (const std::string& stem : stems) {
        try {
            GameSession game(first.installPath(), stem);
            for (int step = 0; step < 30; ++step) {
                game.update(1.0f / 30.0f, InputState{});
            }
            const kotor::Scene& scene = game.scene();
            // What the room clips cost per frame, every room visible; and one creature's light.
            double moduleLoopMs = 0.0;
            for (kotor::EntityMesh& mesh : game.scene().entityMeshes) {
                if (!mesh.roomAnimated) {
                    continue;
                }
                const auto begin = std::chrono::steady_clock::now();
                kotor::refreshEntityMesh(mesh);
                moduleLoopMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                const float duration = mesh.model->animationDuration(mesh.animation);
                const std::vector<kotor::MdlMeshTriangle> rest = mesh.model->loopMeshTriangles(mesh.animation, 0.0f, mesh.origin);
                bool moved = false;
                bool tinted = false;
                for (int sample = 1; sample <= 8; ++sample) {
                    const std::vector<kotor::MdlMeshTriangle> posed = mesh.model->loopMeshTriangles(
                        mesh.animation, duration * static_cast<float>(sample) / 9.0f, mesh.origin);
                    for (std::size_t t = 0; t < rest.size() && t < posed.size(); ++t) {
                        moved = moved || kotor::length(rest[t].v[0].position - posed[t].v[0].position) > 1e-3f;
                        tinted = tinted || kotor::length(rest[t].selfIllum - posed[t].selfIllum) > 1e-3f
                            || std::abs(rest[t].alpha - posed[t].alpha) > 1e-3f;
                    }
                }
                roomLoopMoved += moved ? 1u : 0u;
                roomLoopTinted += !moved && tinted ? 1u : 0u;
                roomLoopStill += !moved && !tinted ? 1u : 0u;
                logInfo(concat("worldposes: room loop ", stem, " ", mesh.model->name(), " ", mesh.animation,
                               " vertices ", mesh.vertices.size(), moved ? " moved" : tinted ? " tinted" : " still"));
            }
            roomLoopWorstMs = std::max(roomLoopWorstMs, moduleLoopMs);
            // What a cutscene's stand-ins cost per frame: each is a skinned body posed on the CPU.
            {
                double standInMs = 0.0;
                std::size_t count = 0;
                for (kotor::EntityMesh& mesh : game.scene().entityMeshes) {
                    if (mesh.clipPlaced) {
                        const auto begin = std::chrono::steady_clock::now();
                        kotor::refreshEntityMesh(mesh);
                        standInMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                        ++count;
                    }
                }
                if (standInMs > standInWorstMs) {
                    standInWorstMs = standInMs;
                    standInWorstCount = count;
                }
            }
            // The Taris sky box must fit inside the far plane the world camera uses.
            if (lower(stem) == "tar_m02aa") {
                skyReached = scene.viewExtent > 1000.0f;
            }
            roomLoopTotalMs += moduleLoopMs;
            {
                const auto begin = std::chrono::steady_clock::now();
                const kotor::SampledLight sampled = kotor::sampleSceneLight(scene, scene.entry);
                lightSampleUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
                lightSamples += sampled.lights.size() != 0u ? 1u : 0u;
            }
            for (const kotor::EmitterInstance& emitter : scene.emitters) {
                const std::string texture = lower(scene.textures.size() > emitter.texture ? scene.textures[emitter.texture].name : std::string{});
                if (texture.find("fire") != std::string::npos || texture.find("flame") != std::string::npos) {
                    logInfo(concat("worldposes: fire ", stem, " ", emitter.model ? emitter.model->name() : std::string{}, " ", emitter.node,
                                   " texture ", texture, " owner ", emitter.entity != UINT32_MAX ? "entity" : "room",
                                   " at ", emitter.origin.x, ",", emitter.origin.y, ",", emitter.origin.z));
                }
            }
            for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
                roomLoopMeshes += mesh.roomAnimated ? 1u : 0u;
                if (mesh.entity < scene.entities.size() && mesh.model && !mesh.attachModel) {
                    const kotor::Entity& object = scene.entities[mesh.entity];
                    if (object.kind == kotor::EntityKind::Door || object.kind == kotor::EntityKind::Placeable) {
                        std::string key = concat(object.kind == kotor::EntityKind::Door ? "door open " : "placeable state ",
                                                 object.kind == kotor::EntityKind::Door ? object.openState : object.animationState, ":");
                        for (const char* clip : {"default", "closed", "opened1", "open", "close", "on", "off", "dead"}) {
                            if (mesh.model->animationDuration(clip) > 0.0f) {
                                key += concat(" ", clip);
                            }
                        }
                        ++objectStates[key];
                    }
                }
                if (mesh.entity >= scene.entities.size() || !mesh.visible || !mesh.model || mesh.vertices.empty() || mesh.attachModel) {
                    continue;
                }
                const kotor::Entity& entity = scene.entities[mesh.entity];
                if (entity.kind != kotor::EntityKind::Creature && entity.kind != kotor::EntityKind::Placeable) {
                    continue;
                }
                float low = mesh.vertices.front().position.z;
                float high = low;
                for (const kotor::SceneVertex& vertex : mesh.vertices) {
                    low = std::min(low, vertex.position.z);
                    high = std::max(high, vertex.position.z);
                }
                float restLow = 0.0f;
                float restHigh = 0.0f;
                bool restFirst = true;
                for (const kotor::MdlMeshTriangle& triangle : mesh.model->posedTriangles({}, 0.0f)) {
                    for (const kotor::MdlMeshVertex& vertex : triangle.v) {
                        restLow = restFirst ? vertex.position.z : std::min(restLow, vertex.position.z);
                        restHigh = restFirst ? vertex.position.z : std::max(restHigh, vertex.position.z);
                        restFirst = false;
                    }
                }
                float ground = 0.0f;
                // A cutscene stand-in sits at the origin and its clip places it.
                const kotor::Vec3 place = mesh.clipPlaced ? mesh.center : entity.position;
                const bool hasFloor = game.floorBelow(place.x, place.y, place.z + 2.0f, ground);
                const float gap = hasFloor ? entity.position.z + low - ground : 0.0f;
                standIns += mesh.clipPlaced ? 1u : 0u;
                const float height = high - low;
                const float restHeight = restHigh - restLow;
                ++bodies;
                const bool isFloating = gap > 0.5f;
                const bool isSunk = gap < -0.5f;
                const bool isFolded = restHeight > 0.5f && height < restHeight * 0.6f;
                floating += isFloating ? 1u : 0u;
                sunk += isSunk ? 1u : 0u;
                folded += isFolded ? 1u : 0u;
                const bool listAll = lower(stem).rfind("stunt_", 0) == 0 && mesh.clipPlaced;
                if (isFloating || isSunk || isFolded || listAll) {
                    logInfo(concat("worldposes: ", stem, " ", entity.tag, " ", mesh.model->name(),
                                   entity.kind == kotor::EntityKind::Creature ? " creature" : " placeable",
                                   " clip '", mesh.animation, "' gap ", gap, " floor ", hasFloor ? 1 : 0,
                                   " height ", height, " rest ", restHeight,
                                   " at ", entity.position.x, ",", entity.position.y, ",", entity.position.z));
                }
            }
        } catch (const std::exception& exception) {
            ++failed;
            logWarn(concat("worldposes: ", stem, " failed: ", exception.what()));
        }
    }
    for (const auto& [state, count] : objectStates) {
        logInfo(concat("worldposes: ", state, " x", count));
    }
    logInfo(concat("worldposes: room loop meshes ", roomLoopMeshes, " moved ", roomLoopMoved, " tinted ", roomLoopTinted,
                   " still ", roomLoopStill));
    logInfo(concat("worldposes: room clips per frame, all rooms visible: worst module ", roomLoopWorstMs, " ms, average ",
                   roomLoopTotalMs / static_cast<double>(std::max<std::size_t>(stems.size(), 1u)), " ms; one light sample ",
                   lightSampleUs / static_cast<double>(std::max<std::size_t>(lightSamples, 1u)), " us"));
    logInfo(concat("worldposes: stand-ins per frame: worst module ", standInWorstMs, " ms for ", standInWorstCount, " meshes"));
    if (roomLoopMeshes == 0u || standIns == 0u || !skyReached) {
        ++failed; // The shipped rooms animate, the cutscenes stand in and Taris has a sky box.
    }
    logInfo(concat("worldposes: modules ", stems.size(), " failed ", failed, " bodies ", bodies,
                   " stand-ins ", standIns, " floating ", floating, " sunk ", sunk, " folded ", folded));
    return failed == 0u ? 0 : 1;
}

} // namespace ke
