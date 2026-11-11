#pragma once

#include "kotor/Formats.h"

namespace ke {

struct SceneCamera {
    kotor::Vec3 eye;
    kotor::Vec3 forward;
    kotor::Vec3 up{0.0f, 0.0f, 1.0f};
    float fovYDegrees = 60.0f;
    // Module cameras keep these defaults; a minigame's ARE sets its own clip range.
    float nearClip = 0.1f; // Measured on retail world and dialogue cameras: 0.1.
    float farClip = 10000.0f; // Retail's camera far plane in play and in dialogue: 10000 in all 117 swept modules.
};

} // namespace ke
