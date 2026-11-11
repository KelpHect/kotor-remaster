#pragma once

// The main menu's 3D scene: the shipped `mainmenu` model (rims/mainmenu.rim) with its
// own animation, seen from the model's `camerahook` node. Retail builds the same scene
// (swkotor.exe 0067c4c0: model "mainmenu" at the origin, camera attached at
// "camerahook", field of view 22.7 degrees) and shows it in the panel's LBL_3DVIEW area.
// The backend draws it through IRenderBackend::setPreviewViews.

#include "kotor/Formats.h"
#include "render/Backend.h"

#include <string>

namespace ke {

class MenuScene {
public:
    // Mounts the front-end rims on `install` and builds the scene.
    explicit MenuScene(kotor::Install& install);

    bool ready() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    const kotor::Scene& scene() const { return scene_; }
    kotor::Scene& scene() { return scene_; }
    std::uint32_t revision() const { return revision_; }
    const std::string& animation() const { return animation_; }

    // Advances the model's animation; true when the meshes changed.
    bool advance(float dt);

    // The view for a window rectangle in physical pixels.
    PreviewView view(int x, int y, int width, int height) const;

private:
    SceneCamera cameraAt(float time) const;

    kotor::Scene scene_;
    std::string error_;
    std::uint32_t revision_ = 1;
    std::string animation_;
    float time_ = 0.0f;
    SceneCamera camera_;
};

} // namespace ke
