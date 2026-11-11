#include "game/ChargenPreview.h"

#include "game/Chargen.h"
#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ke {

namespace {

// retail 006dc3c0 sets the camera field of view to 0x41b5ced9 (22.7 degrees); the
// axis it applies to is not recorded in the code that was read, vertical is assumed.
constexpr float kPreviewFovYDegrees = 22.7258f;

// The rig cameras sit on +X looking toward -X. A creature's bearing is its facing, and bearing 0 is
// +X (Entity::drawBearing), toward the camera; retail 006afda0 also sets the sheet's body to face +X.
constexpr float kFacingCamera = 0.0f;

// RETAIL-PROVEN: how far the sheet's camera stands above the room's hook. 006100f0 (and 006aebf0, 006e8ef0) attach
// the scene camera to charrec_light's camerahook, set the field of view to 22.73 (0x41b5ced9), then take the scene's
// type-0x3ea object (0049fdf0), set its +0x48 to 1 and its offset (+0x3c) to (0, 0, h). Each frame 0049f4f0 moves the
// camera to its own position plus that offset (world axes when +0x48 is 1). h is the preview body's vtable +0xd8,
// 0060f000 -> 006974d0: the z of the body model's CAMERAHOOK node (pmbbm 1.634, pfbam 1.593 at rest), or, with no
// such node, 006973b0's HEAD_G height. The room and the body stand at the origin; only the camera is raised.
// kSheetRaiseFallback is used only when the body has neither node (the value this file used to infer).
constexpr float kSheetRaiseFallback = 1.767f;

// Inferred: how far above the creature's headhook the middle of the face sits.
constexpr float kHeadCentreAboveHook = 0.13f;

// The preview's light is the room rig's own (BLEND): 00447b90 builds a scene light from each light node of a model
// put in a scene, and these rooms carry three (cgbody_light: AuroraLight01 0.87 above the camera side, 03 0.66 low
// on the left, 04 an ambient 0.15). A GUI scene has no area sun: gidy_sun is placed by the area load only
// (00608edd), so the scene's sun colours are cleared. The room's clip keys the colours (charrec_light AuroraLight01:
// 0.85,0.18,0 at rest and in evil, 0.95,0.90,0.84 in align11, 0.72,0.85,1 in good) and places (align11 lifts
// AuroraLight01 by 0.76 to chest height), so colour and position are read in that clip. Measured on the level-up pair
// with the bodies aligned: the vest at rest-pose lights was 0.78 of retail's, a scale, not an offset.
// Paired: cgx\pairs\cg_main_pair.png and levelup_main_pair.png, retail body lit in full, neutral colour.
void lightFromRoom(kotor::Scene& scene, kotor::Install& install, const char* model, kotor::Vec3 origin,
                   const std::string& clip)
{
    scene.ambient = {};
    scene.diffuse = {};
    scene.dynamicAmbient = {};
    const std::shared_ptr<kotor::Mdl> rig = kotor::loadWorldModel(install, model);
    if (!rig) {
        return;
    }
    // Light nodes in node order, as fxNodeParameters counts light and emitter nodes together.
    std::vector<std::size_t> lightOrdinals;
    for (std::size_t ordinal = 0;; ++ordinal) {
        const kotor::FxNodeParameters node = rig->fxNodeParameters(ordinal, std::string{}, 0.0f);
        if (node.name.empty() && node.type == 0) {
            break;
        }
        if ((node.type & 0x02u) != 0u && (node.type & 0x04u) == 0u) {
            lightOrdinals.push_back(ordinal);
        }
    }
    const std::vector<kotor::MdlLight> lights = rig->lights();
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const kotor::MdlLight& light = lights[i];
        kotor::Scene::WorldLight world;
        world.position = light.position + origin;
        world.colour = light.colour;
        if (!clip.empty() && i < lightOrdinals.size()) {
            const kotor::FxNodeParameters keyed = rig->fxNodeParameters(lightOrdinals[i], clip, 0.0f);
            world.colour = keyed.colour(76, light.colour);
            if (const std::optional<kotor::Mdl::Pose> pose = rig->nodePose(keyed.name, clip, 0.0f)) {
                world.position = pose->translation + origin;
            }
        }
        world.multiplier = light.multiplier;
        world.radius = light.radius;
        world.ambientOnly = light.ambientOnly;
        world.priority = light.priority;
        world.dynamicType = light.dynamicType;
        scene.lights.push_back(world);
    }
}

kotor::Vec3 rotate(const kotor::Mat3& r, kotor::Vec3 v)
{
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
            r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
            r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

} // namespace

ChargenPreview::ChargenPreview(kotor::Install& install, const Rules& rules)
    : install_(install)
{
    (void)rules;
    std::string error;
    bodyCamera_ = cameraFrom("cgbody_light", "camerahook", error);
    headCameraMale_ = cameraFrom("cghead_light", "camerahookm", error);
    headCameraFemale_ = cameraFrom("cghead_light", "camerahookf", error);
    error_ = error;
}

SceneCamera ChargenPreview::cameraFrom(const char* rigModel, const char* hook,
                                       std::string& error) const
{
    SceneCamera camera;
    const std::shared_ptr<kotor::Mdl> model = kotor::loadWorldModel(install_, rigModel);
    if (!model) {
        error += std::string(rigModel) + " is missing; ";
        return camera;
    }
    const std::optional<kotor::Mdl::Pose> pose = model->nodePose(hook, "", 0.0f);
    if (!pose.has_value()) {
        error += std::string(rigModel) + " has no " + hook + " node; ";
        return camera;
    }
    camera.eye = pose->translation;
    camera.forward = kotor::normalize(rotate(pose->rotation, {0.0f, 0.0f, -1.0f}));
    camera.up = kotor::normalize(rotate(pose->rotation, {0.0f, 1.0f, 0.0f}));
    camera.fovYDegrees = kPreviewFovYDegrees;
    camera.nearClip = 0.05f;
    camera.farClip = 60.0f;
    logInfo(concat("chargen preview camera ", rigModel, "/", hook, " eye=", camera.eye.x, ",",
                   camera.eye.y, ",", camera.eye.z, " fwd=", camera.forward.x, ",",
                   camera.forward.y, ",", camera.forward.z, " up=", camera.up.x, ",",
                   camera.up.y, ",", camera.up.z));
    return camera;
}

int ChargenPreview::spawn(std::int32_t appearance, kotor::Vec3 position)
{
    const std::size_t index = kotor::spawnPlayerAvatar(
        scene_, install_, static_cast<std::uint32_t>(appearance), "Preview", position,
        kFacingCamera, bodyVariant_);
    return index == static_cast<std::size_t>(-1) ? -1 : static_cast<int>(index);
}

void ChargenPreview::rebuild()
{
    scene_ = kotor::Scene{};
    slotEntity_.fill(-1);
    slotAppearance_.fill(-1);
    galaxyEntity_ = -1;
    sheetEntity_ = -1;
    roomEntity_ = -1;
    sheetClip_.clear();
    sheetRig_.reset();
    sheetTime_ = 0.0f;
    planetEntity_ = -1;
    planetModel_.clear();
    headEntity_ = -1;
    headAppearance_ = -1;
    ++revision_;
}

void ChargenPreview::startIdle()
{
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model) {
            continue;
        }
        for (const char* name : {"pause1", "pause", "cpause1", "default"}) {
            if (mesh.model->animationDuration(name) > 0.0f) {
                mesh.animation = name;
                mesh.animationTime = 0.0f;
                mesh.animationLoop = true;
                break;
            }
        }
        kotor::refreshEntityMesh(mesh);
    }
}

void ChargenPreview::showClassSelection(const Rules& rules)
{
    std::array<std::int32_t, 6> wanted{};
    for (int slot = 0; slot < Chargen::kSlotCount; ++slot) {
        const std::vector<std::int32_t> rows = rules.pcPortraitRows(Chargen::slotGender(slot));
        wanted[static_cast<std::size_t>(slot)] = rows.empty()
            ? -1
            : rules.portraitAppearance(rows.front(), Chargen::slotClass(slot));
    }
    if (headEntity_ < 0 && wanted == slotAppearance_) {
        return;
    }
    rebuild();
    // Retail 006dc3c0: the six bodies wear the class's clothes (chargenBodyVariant), not the row's bare a.
    bodyVariant_ = chargenBodyVariant();
    for (int slot = 0; slot < Chargen::kSlotCount; ++slot) {
        const std::int32_t appearance = wanted[static_cast<std::size_t>(slot)];
        if (appearance < 0) {
            continue;
        }
        slotEntity_[static_cast<std::size_t>(slot)] = spawn(appearance, {0.0f, 0.0f, 0.0f});
        slotAppearance_[static_cast<std::size_t>(slot)] = appearance;
    }
    startIdle();
    placeRoom("cgbody_light", bodyCamera_); // 006dc3c0: each slot's gui3D_room loads cgbody_light (string 007580e4)
}

void ChargenPreview::placeRoom(const char* model, const SceneCamera& camera)
{
    // 006dc3c0 / 006f9430: FUN_00417620 loads the rig into the control's gui3D_room and plays its "default" clip at
    // rate 1.0; its Mist emitters are the smoky grey behind the body. Every 3D_MODELn camera is the same hook, so one
    // camera-facing particle mesh serves all six boxes.
    const std::size_t room = kotor::spawnModelEntity(scene_, install_, model, {0.0f, 0.0f, 0.0f}, 0.0f);
    if (room == static_cast<std::size_t>(-1)) {
        return;
    }
    roomEntity_ = static_cast<int>(room);
    roomCamera_ = camera;
    lightFromRoom(scene_, install_, model, {0.0f, 0.0f, 0.0f}, "default");
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (static_cast<int>(mesh.entity) == roomEntity_ && mesh.model && mesh.model->animationDuration("default") > 0.0f) {
            mesh.animation = "default";
            mesh.animationTime = 0.0f;
            mesh.animationLoop = true;
            kotor::refreshEntityMesh(mesh);
        }
    }
    // ENGINE CHOICE, as for the sheet: retail's panel scene runs from construction, so its mist is already full on the
    // first frame shown. One full particle life is run here: the cgbody_light and cghead_light Mist emitters live 5 s.
    for (int step = 0; step < 300; ++step) {
        kotor::stepParticles(scene_, 1.0f / 60.0f, roomCamera_.eye);
    }
    kotor::buildParticleMesh(scene_, roomCamera_.eye, roomCamera_.forward, roomCamera_.up);
}

void ChargenPreview::showHead(std::int32_t appearance, std::int32_t gender)
{
    if (headEntity_ >= 0 && headAppearance_ == appearance && headGender_ == gender) {
        return;
    }
    rebuild();
    bodyVariant_ = chargenBodyVariant(); // the classsel creature carried on (006dc3c0)
    headGender_ = gender;
    if (appearance < 0) {
        return;
    }
    const int entity = spawn(appearance, {0.0f, 0.0f, 0.0f});
    if (entity < 0) {
        return;
    }
    headEntity_ = entity;
    headAppearance_ = appearance;
    const SceneCamera headCamera = gender == 1 ? headCameraFemale_ : headCameraMale_;
    // The head camera frames the area around the origin, so the creature is lowered
    // until its head hook sits there (inferred: only the head is meant to be in view).
    kotor::Entity& creature = scene_.entities[static_cast<std::size_t>(entity)];
    if (const std::shared_ptr<kotor::Mdl> body = kotor::loadWorldModel(install_, creature.model)) {
        if (const std::optional<kotor::Mdl::Pose> hook = body->nodePose("headhook", "", 0.0f)) {
            // The headhook is the neck; the face centre is about 0.13 m above it (measured
            // on the first capture), so the creature is lowered that much further.
            creature.position = {-hook->translation.x, -hook->translation.y,
                                 -hook->translation.z - kHeadCentreAboveHook};
        }
    }
    startIdle();
    placeRoom("cghead_light", headCamera); // 006f9430: LBL_HEAD's gui3D_room loads cghead_light
}

void ChargenPreview::showGalaxy(const std::string& planetModel)
{
    if (galaxyEntity_ >= 0 && planetModel == planetModel_) {
        return;
    }
    // The galaxy keeps turning while another planet is picked.
    float galaxyTime = 0.0f;
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (galaxyEntity_ >= 0 && static_cast<int>(mesh.entity) == galaxyEntity_) {
            galaxyTime = mesh.animationTime;
        }
    }
    // So does what it is made of: every part of the shipped galaxy model but the hook is an emitter, and
    // picking a planet does not restart them (retail 006933a0 leaves the galaxy's model alone).
    std::vector<kotor::EmitterInstance> galaxyEmitters;
    if (galaxyEntity_ >= 0) {
        galaxyEmitters = scene_.emitters;
    }
    rebuild();
    const auto place = [&](const std::string& model) {
        const std::size_t index = kotor::spawnModelEntity(scene_, install_, model, {0.0f, 0.0f, 0.0f}, 0.0f);
        return index == static_cast<std::size_t>(-1) ? -1 : static_cast<int>(index);
    };
    const bool firstOpen = galaxyEntity_ < 0;
    galaxyEntity_ = place("galaxy");
    for (std::size_t i = 0; i < galaxyEmitters.size() && i < scene_.emitters.size(); ++i) {
        if (static_cast<int>(galaxyEmitters[i].entity) == galaxyEntity_) {
            scene_.emitters[i].particles = std::move(galaxyEmitters[i].particles);
            scene_.emitters[i].birthDebt = galaxyEmitters[i].birthDebt;
            scene_.emitters[i].fired = galaxyEmitters[i].fired;
        }
    }
    if (firstOpen && galaxyEntity_ >= 0) {
        // Retail shows the whole galaxy on the frame the map opens (paired frame galaxy_start); its
        // Galaxy_01..03 particles fade in over their 5-6 s life (alphaStart 0), so a cold emitter is
        // black for seconds. The panel and its model are built once and kept (0062d040), so the
        // emitters run warm: one life's worth is simulated here before the first frame. Inferred
        // from the frames; 0062d040 and 00695180 do not show a warm-up of their own.
        const SceneCamera camera = hookCamera(galaxyEntity_);
        for (int step = 0; step < 100; ++step) {
            kotor::stepParticles(scene_, 0.08f, camera.eye);
        }
    }
    planetModel_ = planetModel;
    if (!planetModel.empty()) {
        planetEntity_ = place(planetModel);
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model) {
            continue;
        }
        const bool planet = static_cast<int>(mesh.entity) == planetEntity_;
        mesh.animation = planet ? "zoomin" : "default";
        mesh.animationTime = planet ? 0.0f : galaxyTime;
        mesh.animationLoop = !planet;
        kotor::refreshEntityMesh(mesh);
    }
}

SceneCamera ChargenPreview::hookCamera(int entity) const
{
    SceneCamera camera;
    camera.fovYDegrees = kPreviewFovYDegrees;
    camera.nearClip = 0.05f;
    camera.farClip = 2000.0f;
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (static_cast<int>(mesh.entity) != entity || !mesh.model) {
            continue;
        }
        // Shipped planet models: zoomin carries the camerahook 25 units in and rotate has no such node (only
        // the planet's own UV scroll moves), so the camera stays where the zoom left it (inferred: the planet
        // would otherwise jump back to its 30 pixel start the moment the zoom ends).
        const bool held = mesh.animation == "rotate";
        const std::optional<kotor::Mdl::Pose> pose = held
            ? mesh.model->nodePose("camerahook", "zoomin", mesh.model->animationDuration("zoomin"))
            : mesh.model->nodePose("camerahook", mesh.animation, mesh.animationTime);
        if (pose.has_value()) {
            camera.eye = pose->translation;
            camera.forward = kotor::normalize(rotate(pose->rotation, {0.0f, 0.0f, -1.0f}));
            camera.up = kotor::normalize(rotate(pose->rotation, {0.0f, 1.0f, 0.0f}));
        }
        return camera;
    }
    if (entity == galaxyEntity_) {
        // galaxy.mdl has no vertices, so no mesh: its hook's rest pose, as the galaxy has no animation of it.
        std::string error;
        return cameraFrom("galaxy", "camerahook", error);
    }
    return camera;
}

void ChargenPreview::showBody(std::int32_t appearance, kotor::BodyVariant body)
{
    if (slotEntity_[0] >= 0 && slotAppearance_[0] == appearance && headEntity_ < 0 && sheetEntity_ < 0 && body == bodyVariant_) {
        return;
    }
    rebuild();
    bodyVariant_ = body;
    if (appearance < 0) {
        return;
    }
    slotEntity_[0] = spawn(appearance, {0.0f, 0.0f, 0.0f});
    slotAppearance_[0] = appearance;
    startIdle();
}

void ChargenPreview::showSheet(std::int32_t appearance, kotor::BodyVariant body, std::int32_t alignment, const std::string& who)
{
    // Retail 006100f0: 0 "evil", 100 "good", otherwise "align" + alignment / 5 + 1 (20 has no clip in the shipped model,
    // so the rest pose stands as retail's failed play leaves it).
    const std::int32_t held = std::clamp(alignment, 0, 100);
    const std::string clip = held == 0 ? std::string{"evil"} : held == 100 ? std::string{"good"} : "align" + std::to_string(held / 5 + 1);
    if (sheetEntity_ >= 0 && slotAppearance_[0] == appearance && headEntity_ < 0 && body == bodyVariant_ && clip == sheetClip_) {
        return;
    }
    rebuild();
    bodyVariant_ = body;
    if (appearance < 0) {
        return;
    }
    slotEntity_[0] = spawn(appearance, {0.0f, 0.0f, 0.0f});
    slotAppearance_[0] = appearance;
    startIdle();
    // 006974d0: the camera's raise is the body's own CAMERAHOOK height (HEAD_G as 006973b0 falls back to).
    sheetRaise_ = kSheetRaiseFallback;
    for (const char* node : {"camerahook", "head_g"}) {
        bool found = false;
        for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
            if (static_cast<int>(mesh.entity) != slotEntity_[0] || !mesh.model) {
                continue;
            }
            if (const std::optional<kotor::Mdl::Pose> hook = mesh.model->nodePose(node, "", 0.0f)) {
                sheetRaise_ = hook->translation.z;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    // The body's stance by the same alignment (006100f0: below 40 evil, below 60 neutral, else good; t3m4 and hk47 pause1).
    const char* const stance = held < 40 ? "evil" : held < 60 ? "neutral" : "good";
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (mesh.model && mesh.model->animationDuration(stance) > 0.0f) {
            mesh.animation = stance;
            mesh.animationTime = 0.0f;
            mesh.animationLoop = true;
            kotor::refreshEntityMesh(mesh);
        }
    }
    // Retail 006100f0 plays the clip on charrec_light itself (rate 1.0) and attaches the camera to the room's
    // camerahook (camerahookt for t3m4, camerahookh for hk47, camerahookz for zaalbar). The clip keys the hook
    // (evil +1.50, align1..7 down to -0.45, align8..10 and 20 unkeyed, align11..19 -0.82 to -1.50, good -1.58),
    // and its pitch, and the camera follows the keyed hook (sheetHookCamera). The room stands with the body at
    // the origin; only the camera is raised, by the body's camerahook height (kSheetRaiseFallback, see there).
    const std::size_t room = kotor::spawnModelEntity(scene_, install_, "charrec_light", {0.0f, 0.0f, 0.0f}, 0.0f);
    if (room == static_cast<std::size_t>(-1)) {
        return;
    }
    sheetEntity_ = static_cast<int>(room);
    sheetClip_ = clip;
    lightFromRoom(scene_, install_, "charrec_light", {0.0f, 0.0f, 0.0f}, clip);
    sheetTime_ = 0.0f;
    sheetRig_ = kotor::loadWorldModel(install_, "charrec_light");
    std::string lower = who;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    sheetHook_ = lower.find("t3m4") != std::string::npos ? "camerahookt"
        : lower.find("hk47") != std::string::npos      ? "camerahookh"
        : lower.find("zaalbar") != std::string::npos   ? "camerahookz"
                                                       : "camerahook";
    if (sheetRig_ && !sheetRig_->nodePose(sheetHook_, "", 0.0f).has_value()) {
        sheetHook_ = "camerahook"; // 006100f0 falls back when the room has no such node
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (static_cast<int>(mesh.entity) == sheetEntity_ && mesh.model && mesh.model->animationIndex(clip) >= 0) {
            mesh.animation = clip;
            mesh.animationTime = 0.0f;
            mesh.animationLoop = true;
            kotor::refreshEntityMesh(mesh);
        }
    }
    for (kotor::EmitterInstance& emitter : scene_.emitters) {
        if (static_cast<int>(emitter.entity) == sheetEntity_) {
            emitter.ownerClip = clip;
        }
    }
    sheetCamera_ = sheetHookCamera();
    // ENGINE CHOICE: retail builds this scene once with its panel (006eb420, 006e8ef0, the sheet's own panel) and
    // the smoke keeps running from then on, so every retail frame shows it full; a scene built just now would
    // open on a black box while the emitters fill it. Four seconds of smoke (one 3.5 s gas life) at a frame rate near play, because births are per step (00496e40), are run here before the first frame.
    for (int step = 0; step < 240; ++step) {
        kotor::stepParticles(scene_, 1.0f / 60.0f, sheetCamera_.eye);
    }
    kotor::buildParticleMesh(scene_, sheetCamera_.eye, sheetCamera_.forward, sheetCamera_.up);
    logInfo(concat("character sheet scene: clip ", clip, " stance ", stance, " hook ", sheetHook_, " emitters ",
                   scene_.emitters.size(), " eye ", sheetCamera_.eye.x, ",", sheetCamera_.eye.y, ",", sheetCamera_.eye.z,
                   " fwd ", sheetCamera_.forward.x, ",", sheetCamera_.forward.y, ",", sheetCamera_.forward.z));
}

SceneCamera ChargenPreview::sheetHookCamera() const
{
    SceneCamera camera;
    camera.fovYDegrees = kPreviewFovYDegrees;
    camera.nearClip = 0.05f;
    camera.farClip = 60.0f;
    if (!sheetRig_) {
        return camera;
    }
    // 006100f0 attaches the camera to the room's hook (scene vt+0x74), so it rides the hook as the alignment clip
    // keys it, and the type-0x3ea scene object it sets up adds (0, 0, h) in world axes each update (0049f4f0 with
    // +0x48 = 1), h being the body's camerahook height (sheetRaise_). The keys aim the view at the body: align3
    // (alignment 10) is 1.4 m above align11 and pitched 15.7 degrees down, align19 (90) 0.68 m below it and pitched
    // 9 degrees up (paired cs_al10, cs_before, cs_al90). The align clips are single key poses, so the clip's
    // presence decides, not its length.
    const bool keyed = sheetRig_->animationIndex(sheetClip_) >= 0;
    const std::optional<kotor::Mdl::Pose> pose =
        sheetRig_->nodePose(sheetHook_, keyed ? sheetClip_ : std::string{}, keyed ? sheetTime_ : 0.0f);
    if (pose.has_value()) {
        camera.eye = pose->translation;
        camera.eye.z += sheetRaise_;
        camera.forward = kotor::normalize(rotate(pose->rotation, {0.0f, 0.0f, -1.0f}));
        camera.up = kotor::normalize(rotate(pose->rotation, {0.0f, 1.0f, 0.0f}));
    }
    return camera;
}

bool ChargenPreview::advance(float dt)
{
    bool moved = false;
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model || mesh.animation.empty()) {
            continue;
        }
        const float duration = mesh.model->animationDuration(mesh.animation);
        if (duration <= 0.0f) {
            continue;
        }
        if (!mesh.animationLoop && mesh.animation == "zoomin" && mesh.animationTime + dt >= duration) {
            // Retail 006933a0 queues rotate, looped, behind the zoom.
            mesh.animation = "rotate";
            mesh.animationLoop = true;
            mesh.animationTime = 0.0f;
        } else {
            const float length = mesh.model->animationDuration(mesh.animation);
            mesh.animationTime = length > 0.0f ? std::fmod(mesh.animationTime + dt, length) : 0.0f;
        }
        kotor::refreshEntityMesh(mesh);
        moved = true;
    }
    if (sheetEntity_ >= 0 && sheetRig_) {
        const float length = sheetRig_->animationDuration(sheetClip_);
        if (length > 0.0f) {
            sheetTime_ = std::fmod(sheetTime_ + dt, length);
            sheetCamera_ = sheetHookCamera();
            moved = true;
        }
    }
    if (galaxyEntity_ >= 0 || sheetEntity_ >= 0 || roomEntity_ >= 0) {
        const SceneCamera camera = sheetEntity_ >= 0 ? sheetCamera_ : roomEntity_ >= 0 ? roomCamera_ : hookCamera(galaxyEntity_);
        kotor::stepParticles(scene_, std::min(dt, 0.08f), camera.eye);
        kotor::buildParticleMesh(scene_, camera.eye, camera.forward, camera.up);
    }
    return moved;
}

int ChargenPreview::entityForTag(const std::string& tag) const
{
    if (tag == "LBL_3D" || tag == "LBL_3DCHAR" || tag == "MODEL_LBL") { // MODEL_LBL: MAINCG (006eb420, 006e8ef0)
        return slotEntity_[0];
    }
    if (tag == "LBL_HEAD") {
        return headEntity_;
    }
    if (tag == "3D_PlanetDisplay") {
        return galaxyEntity_;
    }
    if (tag == "3D_PlanetModel") {
        return planetEntity_;
    }
    if (tag.size() == 9 && tag.rfind("3D_MODEL", 0) == 0 && tag[8] >= '1' && tag[8] <= '6') {
        return slotEntity_[static_cast<std::size_t>(tag[8] - '1')];
    }
    return -1;
}

std::int32_t ChargenPreview::appearanceForTag(const std::string& tag) const
{
    if (tag == "LBL_3D" || tag == "LBL_3DCHAR" || tag == "MODEL_LBL") { // MODEL_LBL: MAINCG (006eb420, 006e8ef0)
        return slotAppearance_[0];
    }
    if (tag == "LBL_HEAD") {
        return headAppearance_;
    }
    if (tag.size() == 9 && tag.rfind("3D_MODEL", 0) == 0 && tag[8] >= '1' && tag[8] <= '6') {
        return slotAppearance_[static_cast<std::size_t>(tag[8] - '1')];
    }
    return -1;
}

std::vector<PreviewView> ChargenPreview::views(const std::vector<PreviewRect>& rects) const
{
    std::vector<PreviewView> out;
    for (const PreviewRect& rect : rects) {
        const int entity = entityForTag(rect.tag);
        if (entity < 0 || rect.width <= 0 || rect.height <= 0
            || static_cast<int>(out.size()) >= kMaxPreviewViews) {
            continue;
        }
        PreviewView view;
        const bool galaxy = rect.tag == "3D_PlanetDisplay" || rect.tag == "3D_PlanetModel";
        view.camera = galaxy ? hookCamera(entity)
            : rect.tag == "LBL_HEAD"
            ? (headGender_ == 1 ? headCameraFemale_ : headCameraMale_)
            : sheetEntity_ >= 0 ? sheetCamera_ : bodyCamera_;
        if (galaxy) {
            view.background = {0.0f, 0.0f, 0.0f};
        }
        // The galaxy model is its emitters, and the sheet's backdrop is too.
        view.drawParticles = rect.tag == "3D_PlanetDisplay" || (sheetEntity_ >= 0 && (rect.tag == "LBL_3DCHAR" || rect.tag == "MODEL_LBL"));
        view.x = rect.x;
        view.y = rect.y;
        view.width = rect.width;
        view.height = rect.height;
        view.entities = {static_cast<std::uint32_t>(entity)};
        const bool roomed = rect.tag == "LBL_HEAD" || rect.tag.rfind("3D_MODEL", 0) == 0;
        if (roomed && roomEntity_ >= 0) {
            view.entities.push_back(static_cast<std::uint32_t>(roomEntity_));
            view.drawParticles = true;
        }
        out.push_back(std::move(view));
    }
    return out;
}

} // namespace ke
