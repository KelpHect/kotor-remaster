#pragma once

#include <cstdint>

namespace ke {

// This and the background rate in Args.h are engine choices, not values recovered from retail.
inline constexpr int kDefaultMaxFps = 60;

// The rate one frame should run at; 0 means unpaced.
//   maxFps         --max-fps: -1 when not given, 0 for no foreground cap.
//   backgroundFps  --background-fps: 0 turns the background rate off.
//   offline        a run with its own clock or frame budget (--frames, --fixed-dt, a gate).
//                  It stays as fast as it was unless --max-fps asks otherwise.
//   background     the window is minimized, occluded or has lost focus.
constexpr double pacedFps(int maxFps, int backgroundFps, bool offline, bool background)
{
    if (offline) {
        return maxFps > 0 ? static_cast<double>(maxFps) : 0.0;
    }
    const int foreground = maxFps < 0 ? kDefaultMaxFps : maxFps;
    if (background && backgroundFps > 0) {
        return static_cast<double>(foreground > 0 && foreground < backgroundFps ? foreground : backgroundFps);
    }
    return static_cast<double>(foreground);
}

// When the next frame is due. Deadlines advance by whole periods so rounding does not drift;
// a frame later than one whole period restarts from now instead of rushing to catch up.
constexpr std::uint64_t nextFrameDeadline(std::uint64_t deadline, std::uint64_t now, std::uint64_t period)
{
    if (deadline == 0 || now > deadline + period) {
        return now + period;
    }
    return deadline + period;
}

} // namespace ke
