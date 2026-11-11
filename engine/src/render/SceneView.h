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
    // The area's world camera: 00641010 and 0063f660 call the camera's SetClip (vtable +0x40,
    // 0045bec0: near +0x1dc, far +0x1e0) with (0.1, 10000); the retail state trace logs 10000
    // on every module (cam rows, SC trace diff rank 12).
    float farClip = 10000.0f;
};

} // namespace ke
