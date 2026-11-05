#pragma once

// Owns the video-effect state behind ACT_508 EnableVideoEffect and ACT_510
// DisableVideoEffect and the dialogue CamVidEffect field (render/VideoEffect.h
// has the evidence and the shared maths). GameSession holds one; the render
// backends receive state() every frame.

#include "kotor/Formats.h"
#include "render/VideoEffect.h"

#include <optional>
#include <string>

namespace ke {

class GameSession;

class VideoEffectController {
public:
    // Retail 005f7710. Resets to "none", then applies the row when the
    // videoeffects.2da row exists and enables a pass.
    void enable(kotor::Install& install, std::int32_t row);
    // Retail 005f40e0.
    void disable();

    // What a dialogue node does to the state.
    enum class NodeResult {
        Unchanged, // the node leaves the effect as it is
        Applied,   // a row was enabled
        Cleared,   // the node's camera disables the effect
        Rejected   // a row the install does not define: no effect, counted
    };
    // Retail 006306e0 (camera set-up for a dialogue node): a static camera
    // (CameraAngle 6) enables the node's row unless the value is -1/-2, which
    // leave the state alone; any other camera disables the effect.
    NodeResult applyDialogueNode(kotor::Install& install, std::int32_t camVidEffect,
                                 bool staticCamera);

    const VideoEffectState& state() const { return state_; }
    int rejectedCount() const { return rejected_; }
    // Number of times a row actually became active (test visibility).
    int appliedCount() const { return applied_; }

private:
    void load(kotor::Install& install);

    VideoEffectState state_;
    VideoEffectState noiseDefaults_; // scale/translate from filmnoisetex.txi
    std::optional<kotor::TwoDa> table_;
    bool loaded_ = false;
    int rejected_ = 0;
    int applied_ = 0;
};

// --family-test videoeffect (ScriptVideoEffect.cpp): ACT_508/510 through NCS
// fixtures in `fixtureDir`.
int runVideoEffectFamilyTest(GameSession& game, const std::string& fixtureDir);

} // namespace ke
