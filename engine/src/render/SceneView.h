#pragma once

#include "kotor/Formats.h"

namespace ke {

struct SceneCamera {
    kotor::Vec3 eye;
    kotor::Vec3 forward;
    kotor::Vec3 up{0.0f, 0.0f, 1.0f};
    float fovYDegrees = 60.0f;
};

} // namespace ke
