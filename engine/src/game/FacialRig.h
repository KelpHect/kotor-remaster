#pragma once

#include "kotor/Formats.h"

#include <map>
#include <string>
#include <vector>

namespace ke {

// One viseme's facial-channel weights, resolved from visemes.2da and applied
// to the shipped head rig's jaw/mouth/brow bones. Shipped heads carry no
// per-viseme clips, so the channels are procedural (documented in recon 54).
struct VisemePose {
    float jawOpen = 0.0f;
    float mouthWide = 0.0f;
    float lipRound = 0.0f;
    float browRaise = 0.0f;
};

class FacialRig {
public:
    bool load(kotor::Install& install, std::string& error);
    bool loaded() const { return !names_.empty(); }

    std::size_t visemeCount() const { return names_.size(); }
    const std::string& visemeName(int viseme) const;
    VisemePose pose(int viseme) const;
    float jawAngle(int viseme) const;

    // Local bone rotations keyed by the shipped head node names
    // (f_jaw_g, f_lmc_g, f_rmc_g, f_lbrw_g, f_rbrw_g, tongue).
    std::map<std::string, kotor::Mat3> rotations(int viseme) const;

    void setViseme(int viseme);
    int viseme() const { return viseme_; }
    const VisemePose& currentPose() const { return currentPose_; }
    const std::map<std::string, kotor::Mat3>& currentRotations() const {
        return currentRotations_;
    }

private:
    std::vector<std::string> names_;
    std::vector<VisemePose> poses_;
    int viseme_ = 0;
    VisemePose currentPose_{};
    std::map<std::string, kotor::Mat3> currentRotations_;
};

} // namespace ke
