#include "game/MenuScene.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace ke {

namespace {

// retail 0067c4c0 attaches the camera with the field of view 0x41b5ced9 (22.7 degrees);
// like the chargen rigs the axis it applies to is assumed to be vertical.
constexpr float kMenuFovYDegrees = 22.7258f;

// The scene's only light is the model's own: 00447b90 makes a scene light from each light node of a model put in a
// scene (mainmenu: AuroraLight01, priority 1, and AuroraLight04, ambient only, priority 2). A GUI scene has no area
// sun or ambient (its constructor 004587a0 zeroes them and only the area load sets them, 00608edd / 00609207), so
// Malak is lit by those two alone. Same rule as the chargen rooms (ChargenPreview.cpp lightFromRoom); colour and
// place are read in the clip retail plays, "default" (0067c4c0, string 0073df6c).
void lightFromModel(kotor::Scene& scene, const kotor::Mdl& rig, const std::string& clip)
{
    scene.ambient = {};
    scene.diffuse = {};
    scene.dynamicAmbient = {};
    std::vector<std::size_t> lightOrdinals;
    for (std::size_t ordinal = 0;; ++ordinal) {
        const kotor::FxNodeParameters node = rig.fxNodeParameters(ordinal, std::string{}, 0.0f);
        if (node.name.empty() && node.type == 0) {
            break;
        }
        if ((node.type & 0x02u) != 0u && (node.type & 0x04u) == 0u) {
            lightOrdinals.push_back(ordinal);
        }
    }
    const std::vector<kotor::MdlLight> lights = rig.lights();
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const kotor::MdlLight& light = lights[i];
        kotor::Scene::WorldLight world;
        world.position = light.position;
        world.colour = light.colour;
        if (i < lightOrdinals.size()) {
            const kotor::FxNodeParameters keyed = rig.fxNodeParameters(lightOrdinals[i], clip, 0.0f);
            world.colour = keyed.colour(76, light.colour);
            if (const std::optional<kotor::Mdl::Pose> pose = rig.nodePose(keyed.name, clip, 0.0f)) {
                world.position = pose->translation;
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

MenuScene::MenuScene(kotor::Install& install)
{
    install.addRim("mainmenu");
    const std::size_t index =
        kotor::spawnModelEntity(scene_, install, "mainmenu", {0.0f, 0.0f, 0.0f}, 0.0f);
    if (index == static_cast<std::size_t>(-1) || scene_.entityMeshes.empty()) {
        error_ = scene_.warnings.empty() ? std::string("mainmenu model has no geometry")
                                         : scene_.warnings.front();
        return;
    }
    const std::shared_ptr<kotor::Mdl>& model = scene_.entityMeshes.front().model;
    if (!model->hasNode("camerahook")) {
        error_ = "the mainmenu model has no camerahook node";
        return;
    }
    // The shipped model carries one or more animations; play the longest one in a loop.
    float best = 0.0f;
    for (const kotor::MdlAnimationInfo& info : model->animations()) {
        logInfo(concat("main menu animation ", info.name, " duration=", info.duration));
        if (info.duration > best) {
            best = info.duration;
            animation_ = info.name;
        }
    }
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        mesh.animation = animation_;
        mesh.animationTime = 0.0f;
        mesh.animationLoop = true;
        kotor::refreshEntityMesh(mesh);
    }
    for (const kotor::EntityMesh& mesh : scene_.entityMeshes) {
        kotor::Vec3 low{1e9f, 1e9f, 1e9f};
        kotor::Vec3 high{-1e9f, -1e9f, -1e9f};
        for (const kotor::SceneVertex& vertex : mesh.vertices) {
            low = {std::min(low.x, vertex.position.x), std::min(low.y, vertex.position.y),
                   std::min(low.z, vertex.position.z)};
            high = {std::max(high.x, vertex.position.x), std::max(high.y, vertex.position.y),
                    std::max(high.z, vertex.position.z)};
        }
        logInfo(concat("main menu mesh vertices=", mesh.vertices.size(), " indices=", mesh.indices.size(),
                       " batches=", mesh.batches.size(), " visible=", mesh.visible ? 1 : 0, " bounds=",
                       low.x, ",", low.y, ",", low.z, " .. ", high.x, ",", high.y, ",", high.z,
                       " textures=", scene_.textures.size()));
    }
    lightFromModel(scene_, *model, animation_);
    camera_ = cameraAt(0.0f);
    // The Mist emitters under Dummy02 are the fog over the floor. ENGINE CHOICE, as for the character sheet: retail's
    // scene runs from when the panel is built, behind the legal screens and movies, so its first frame already shows
    // the mist full. Its particles live 10 s (Mist and Mist01, birthrate 7), so one full life is run here before
    // the first frame; with less the mist near the camera is still missing (paired 0001_main: 1.4 against 16.5).
    for (int step = 0; step < 600; ++step) {
        kotor::stepParticles(scene_, 1.0f / 60.0f, camera_.eye);
    }
    kotor::buildParticleMesh(scene_, camera_.eye, camera_.forward, camera_.up);
    logInfo(concat("main menu lights ", scene_.lights.size(), " emitters ", scene_.emitters.size()));
    logInfo(concat("main menu camera eye=", camera_.eye.x, ",", camera_.eye.y, ",", camera_.eye.z,
                   " fwd=", camera_.forward.x, ",", camera_.forward.y, ",", camera_.forward.z));
}

SceneCamera MenuScene::cameraAt(float time) const
{
    SceneCamera camera;
    if (scene_.entityMeshes.empty() || !scene_.entityMeshes.front().model) {
        return camera;
    }
    const auto pose = scene_.entityMeshes.front().model->nodePose("camerahook", animation_, time);
    if (!pose.has_value()) {
        return camera;
    }
    camera.eye = pose->translation;
    camera.forward = kotor::normalize(rotate(pose->rotation, {0.0f, 0.0f, -1.0f}));
    camera.up = kotor::normalize(rotate(pose->rotation, {0.0f, 1.0f, 0.0f}));
    camera.fovYDegrees = kMenuFovYDegrees;
    camera.nearClip = 0.05f;
    camera.farClip = 200.0f;
    return camera;
}

bool MenuScene::advance(float dt)
{
    if (!ready() || animation_.empty()) {
        return false;
    }
    float duration = 0.0f;
    for (kotor::EntityMesh& mesh : scene_.entityMeshes) {
        if (!mesh.model) {
            continue;
        }
        duration = mesh.model->animationDuration(animation_);
        if (duration <= 0.0f) {
            continue;
        }
        mesh.animationTime = std::fmod(mesh.animationTime + dt, duration);
        kotor::refreshEntityMesh(mesh);
    }
    if (duration > 0.0f) {
        time_ = std::fmod(time_ + dt, duration);
        camera_ = cameraAt(time_);
    }
    kotor::stepParticles(scene_, std::min(dt, 0.08f), camera_.eye);
    kotor::buildParticleMesh(scene_, camera_.eye, camera_.forward, camera_.up);
    return duration > 0.0f;
}

PreviewView MenuScene::view(int x, int y, int width, int height) const
{
    PreviewView out;
    out.camera = camera_;
    out.x = x;
    out.y = y;
    out.width = width;
    out.height = height;
    for (std::uint32_t i = 0; i < scene_.entities.size(); ++i) {
        out.entities.push_back(i);
    }
    out.drawStatic = false;
    out.drawParticles = true;
    out.background = {0.0f, 0.0f, 0.0f};
    return out;
}

} // namespace ke
