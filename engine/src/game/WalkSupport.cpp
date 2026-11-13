#include "game/GameSession.h"

// Walk tooling: the one session-side hook the walk steps need beyond the public API. Per-module probing
// only (--start-wp); the continuous route never calls it.

namespace ke {

bool GameSession::walkPlaceLeader(kotor::Vec3 at, float yaw)
{
    if (scene_.rooms.empty()) {
        return false;
    }
    position_ = at;
    float ground = 0.0f;
    if (floorBelow(position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    yaw_ = yaw;
    playerBearing_ = yaw;
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    syncPlayerToScriptWorld();
    placePartyAtEntry();
    return true;
}

} // namespace ke
