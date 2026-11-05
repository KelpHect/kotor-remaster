#include "game/ChargenPreview.h"

#include "game/Chargen.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace ke {

namespace {

// retail 006dc3c0 sets the camera field of view to 0x41b5ced9 (22.7 degrees); the
// axis it applies to is not recorded in the code that was read, vertical is assumed.
constexpr float kPreviewFovYDegrees = 22.7258f;

// The rig cameras sit on +X looking toward -X. A creature with bearing 0 faces +Y in this
// renderer (seen as a profile in the first capture), so a quarter turn clockwise turns it
// to face +X, toward the camera. Inferred from the rendered result, not from retail code.
constexpr float kFacingCamera = -1.5707964f;

// Inferred: how far above the creature's headhook the middle of the face sits.
constexpr float kHeadCentreAboveHook = 0.13f;

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
        kFacingCamera);
    return index == static_cast<std::size_t>(-1) ? -1 : static_cast<int>(index);
}

void ChargenPreview::rebuild()
{
    scene_ = kotor::Scene{};
    slotEntity_.fill(-1);
    slotAppearance_.fill(-1);
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
    for (int slot = 0; slot < Chargen::kSlotCount; ++slot) {
        const std::int32_t appearance = wanted[static_cast<std::size_t>(slot)];
        if (appearance < 0) {
            continue;
        }
        slotEntity_[static_cast<std::size_t>(slot)] = spawn(appearance, {0.0f, 0.0f, 0.0f});
        slotAppearance_[static_cast<std::size_t>(slot)] = appearance;
    }
    startIdle();
}

void ChargenPreview::showHead(std::int32_t appearance, std::int32_t gender)
{
    if (headEntity_ >= 0 && headAppearance_ == appearance && headGender_ == gender) {
        return;
    }
    rebuild();
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
        mesh.animationTime = std::fmod(mesh.animationTime + dt, duration);
        kotor::refreshEntityMesh(mesh);
        moved = true;
    }
    return moved;
}

int ChargenPreview::entityForTag(const std::string& tag) const
{
    if (tag == "LBL_HEAD") {
        return headEntity_;
    }
    if (tag.size() == 9 && tag.rfind("3D_MODEL", 0) == 0 && tag[8] >= '1' && tag[8] <= '6') {
        return slotEntity_[static_cast<std::size_t>(tag[8] - '1')];
    }
    return -1;
}

std::int32_t ChargenPreview::appearanceForTag(const std::string& tag) const
{
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
        view.camera = rect.tag == "LBL_HEAD"
            ? (headGender_ == 1 ? headCameraFemale_ : headCameraMale_)
            : bodyCamera_;
        view.x = rect.x;
        view.y = rect.y;
        view.width = rect.width;
        view.height = rect.height;
        view.entities = {static_cast<std::uint32_t>(entity)};
        out.push_back(std::move(view));
    }
    return out;
}

} // namespace ke
