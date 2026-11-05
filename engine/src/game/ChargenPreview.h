#pragma once

// Character-generation 3D model controls: the classsel 3D_MODEL1..6 bodies and the
// portcust LBL_HEAD preview. A ChargenPreview owns a small private kotor::Scene whose
// creature entities are real appearance.2da models (body and head merged, idle clip
// "pause1" playing); the render backend draws them into the control rectangles
// through IRenderBackend::setPreviewViews.
//
// Camera placement is read from the shipped rig models at runtime (cgbody_light's
// "camerahook", cghead_light's "camerahookm"/"camerahookf"; retail 006dc3c0 and
// 006f9430 attach the camera there with a 22.7 degree field of view). The camera
// looks along the hook's local -Z with +Y up and the creature stands at the origin
// facing +X; those two conventions are inferred from the hook poses (the camera
// ends up looking at the body's mid height from the front), not read from code.

#include "game/Rules.h"
#include "kotor/Formats.h"
#include "render/Backend.h"

#include <array>
#include <string>
#include <vector>

namespace ke {

struct PreviewRect {
    std::string tag; // "3D_MODEL1".."3D_MODEL6" or "LBL_HEAD"
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class ChargenPreview {
public:
    ChargenPreview(kotor::Install& install, const Rules& rules);

    // False when the shipped camera rigs could not be read; error() says why.
    bool ready() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // classsel: slot s (BTN_SEL(s+1)) shows its class and gender's body, taken from
    // the first portrait row of that gender (retail picks a random row; see Chargen).
    void showClassSelection(const Rules& rules);
    // portcust: one creature, positioned so the head fills the view.
    void showHead(std::int32_t appearance, std::int32_t gender);

    // Advances the idle clip by dt and re-poses every mesh. True when anything moved.
    bool advance(float dt);

    // Incremented whenever the scene contents were rebuilt (backend must re-upload).
    std::uint32_t revision() const { return revision_; }
    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }

    // One view per rectangle whose tag has a model; coordinates pass through.
    std::vector<PreviewView> views(const std::vector<PreviewRect>& rects) const;

    // Entity index shown by 3D_MODELn (n = 1..6) or LBL_HEAD, -1 when none.
    int entityForTag(const std::string& tag) const;
    std::int32_t appearanceForTag(const std::string& tag) const;

private:
    void rebuild();
    int spawn(std::int32_t appearance, kotor::Vec3 position);
    void startIdle();
    SceneCamera cameraFrom(const char* rigModel, const char* hook, std::string& error) const;

    kotor::Install& install_;
    kotor::Scene scene_;
    std::string error_;
    std::uint32_t revision_ = 0;
    SceneCamera bodyCamera_;
    SceneCamera headCameraMale_;
    SceneCamera headCameraFemale_;
    std::array<int, 6> slotEntity_{{-1, -1, -1, -1, -1, -1}};
    std::array<std::int32_t, 6> slotAppearance_{{-1, -1, -1, -1, -1, -1}};
    int headEntity_ = -1;
    std::int32_t headAppearance_ = -1;
    std::int32_t headGender_ = 0;
};

} // namespace ke
