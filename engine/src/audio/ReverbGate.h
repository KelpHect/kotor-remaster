#pragma once

#include <string>
#include <vector>

namespace ke::audio {

// Parameter-measured gate for the environment reverb (audio/Reverb.h), driven
// through the real Mixer with unit-impulse voices. PARAMETER-FAITHFUL, NOT
// BIT-EXACT MILES/EAX: the gate checks the published EAX parameters against
// measurements of this engine's own output, not against Creative's DSP.
struct ReverbGateResult {
    bool passed = true;
    std::vector<std::string> lines;  // human-readable measured numbers + verdicts
};

ReverbGateResult runReverbGate();

} // namespace ke::audio
