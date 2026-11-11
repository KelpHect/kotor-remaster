#pragma once

#include "kotor/Formats.h"
#include "kotor/Media.h"

#include <string>
#include <vector>

namespace ke {

// The lip-sync shapes. Their names come from visemes.2da; the shapes themselves are the models'
// shipped `talk` clip: a shape picks a time in that clip (retail 00616310).
class FacialRig {
public:
    bool load(kotor::Install& install, std::string& error);
    bool loaded() const { return !names_.empty(); }

    std::size_t visemeCount() const { return names_.size(); }
    const std::string& visemeName(int viseme) const;

    // Where in the talk clip a shape sits, as a fraction of the clip's length.
    static float clipFraction(int viseme) { return static_cast<float>(viseme + 1) * 0.0625f; }
    // A LIP track's pose at a moment: the two keys around it and how far between them
    // (retail 00486670), fading out over the half second after the last key.
    static kotor::MdlFacePose facePose(const kotor::LipSync& lip, float time);
    static constexpr float kFadeSeconds = 0.5f;

    void setViseme(int viseme);
    int viseme() const { return viseme_; }

private:
    std::vector<std::string> names_;
    int viseme_ = 0;
};

} // namespace ke
