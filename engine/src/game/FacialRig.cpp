#include "game/FacialRig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ke {

namespace {

kotor::Mat3 identity()
{
    kotor::Mat3 matrix;
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    return matrix;
}

kotor::Mat3 rotationX(float angle)
{
    kotor::Mat3 matrix = identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    matrix.m[1][1] = c;
    matrix.m[1][2] = -s;
    matrix.m[2][1] = s;
    matrix.m[2][2] = c;
    return matrix;
}

kotor::Mat3 rotationZ(float angle)
{
    kotor::Mat3 matrix = identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    matrix.m[0][0] = c;
    matrix.m[0][1] = -s;
    matrix.m[1][0] = s;
    matrix.m[1][1] = c;
    return matrix;
}

const VisemePose kPoses[16] = {
    {0.00f, 0.00f, 0.00f, 0.00f}, // Silence
    {0.35f, 0.15f, 0.25f, 0.00f}, // Eat
    {0.18f, 0.65f, 0.10f, 0.10f}, // If
    {0.55f, -0.35f, 0.80f, 0.00f}, // Ox
    {0.62f, -0.20f, 0.70f, 0.00f}, // Oat
    {0.22f, 0.45f, 0.20f, 0.05f}, // Wet
    {0.12f, 0.75f, 0.10f, 0.15f}, // Size
    {0.30f, 0.15f, 0.55f, 0.00f}, // Church
    {0.20f, 0.50f, 0.15f, 0.00f}, // Fave
    {0.32f, 0.25f, 0.30f, 0.00f}, // Though
    {0.40f, -0.10f, 0.50f, 0.00f}, // Told
    {0.00f, 0.00f, 0.40f, 0.00f}, // Bump
    {0.30f, -0.20f, 0.35f, 0.00f}, // New
    {0.85f, 0.35f, 0.15f, 0.20f}, // Roar
    {0.50f, 0.40f, 0.20f, 0.00f}, // Cage
    {0.35f, 0.25f, 0.20f, 0.00f}, // Earth
};

} // namespace

bool FacialRig::load(kotor::Install& install, std::string& error)
{
    error.clear();
    names_.clear();
    poses_.clear();
    currentRotations_.clear();
    viseme_ = 0;
    currentPose_ = {};
    auto data = install.find("visemes", kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        error = "visemes.2da is missing";
        return false;
    }
    try {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "visemes");
        const int idColumn = table.columnIndex("visemeid");
        const int nameColumn = table.columnIndex("animname");
        if (idColumn < 0 || nameColumn < 0) {
            error = "visemes.2da lacks visemeid/animname";
            return false;
        }
        names_.assign(16, {});
        poses_.assign(16, {});
        std::vector<bool> seen(16, false);
        for (std::size_t row = 0; row < table.rowCount(); ++row) {
            const std::string idText = table.cell(row, static_cast<std::size_t>(idColumn));
            int id = std::atoi(idText.c_str());
            // The shipped visemes.2da shifts animname into the first data
            // column, so fall back to column 0 when animname is empty.
            std::string name = table.cell(row, static_cast<std::size_t>(nameColumn));
            if (name.empty()) {
                name = table.cell(row, 0);
            }
            if (id < 0 || id >= 16) {
                id = std::atoi(table.rowLabel(row).c_str()) - 1;
            }
            if (id < 0 || id >= 16 || seen[static_cast<std::size_t>(id)]) {
                continue;
            }
            names_[static_cast<std::size_t>(id)] = std::move(name);
            poses_[static_cast<std::size_t>(id)] = kPoses[id];
            seen[static_cast<std::size_t>(id)] = true;
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            error = "visemes.2da does not cover ids 0..15";
            names_.clear();
            poses_.clear();
            return false;
        }
        currentPose_ = poses_[0];
        return true;
    } catch (const kotor::FormatError& exception) {
        error = exception.what();
        return false;
    }
}

const std::string& FacialRig::visemeName(int viseme) const
{
    static const std::string empty;
    return viseme >= 0 && static_cast<std::size_t>(viseme) < names_.size()
        ? names_[static_cast<std::size_t>(viseme)] : empty;
}

VisemePose FacialRig::pose(int viseme) const
{
    return viseme >= 0 && static_cast<std::size_t>(viseme) < poses_.size()
        ? poses_[static_cast<std::size_t>(viseme)] : VisemePose{};
}

float FacialRig::jawAngle(int viseme) const
{
    return -0.55f * pose(viseme).jawOpen;
}

std::map<std::string, kotor::Mat3> FacialRig::rotations(int viseme) const
{
    std::map<std::string, kotor::Mat3> out;
    const VisemePose channels = pose(viseme);
    if (channels.jawOpen > 0.0f) {
        out.emplace("f_jaw_g", rotationX(-0.55f * channels.jawOpen));
        out.emplace("tongue", rotationX(-0.18f * channels.jawOpen));
    }
    if (channels.mouthWide != 0.0f) {
        out.emplace("f_lmc_g", rotationZ(0.22f * channels.mouthWide));
        out.emplace("f_rmc_g", rotationZ(-0.22f * channels.mouthWide));
    }
    if (channels.lipRound > 0.0f) {
        out.emplace("f_um_g", rotationX(0.12f * channels.lipRound));
    }
    if (channels.browRaise > 0.0f) {
        out.emplace("f_lbrw_g", rotationX(0.12f * channels.browRaise));
        out.emplace("f_rbrw_g", rotationX(0.12f * channels.browRaise));
    }
    return out;
}

void FacialRig::setViseme(int viseme)
{
    if (poses_.empty()) {
        viseme_ = 0;
        currentPose_ = {};
        currentRotations_.clear();
        return;
    }
    viseme_ = std::clamp(viseme, 0, static_cast<int>(poses_.size()) - 1);
    currentPose_ = pose(viseme_);
    currentRotations_ = rotations(viseme_);
}

} // namespace ke
