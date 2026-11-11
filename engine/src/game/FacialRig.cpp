#include "game/FacialRig.h"

#include <algorithm>
#include <cstdlib>

namespace ke {

bool FacialRig::load(kotor::Install& install, std::string& error)
{
    error.clear();
    names_.clear();
    viseme_ = 0;
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
            seen[static_cast<std::size_t>(id)] = true;
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            error = "visemes.2da does not cover ids 0..15";
            names_.clear();
            return false;
        }
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

kotor::MdlFacePose FacialRig::facePose(const kotor::LipSync& lip, float time)
{
    kotor::MdlFacePose pose;
    if (lip.frames.empty()) {
        return pose;
    }
    const auto shape = [&](std::size_t key) { return clipFraction(static_cast<int>(lip.frames[key].viseme)); };
    pose.weight = 1.0f;
    const std::size_t last = lip.frames.size() - 1;
    if (time >= lip.frames[last].time) {
        pose.fractionA = pose.fractionB = shape(last);
        const float end = std::max(lip.frames[last].time, lip.duration);
        pose.weight = std::clamp(1.0f - (time - end) / kFadeSeconds, 0.0f, 1.0f);
        return pose;
    }
    std::size_t next = 0;
    while (next < last && lip.frames[next].time < time) {
        ++next;
    }
    const std::size_t before = next == 0 ? 0 : next - 1;
    pose.fractionA = shape(before);
    pose.fractionB = shape(next);
    const float span = lip.frames[next].time - lip.frames[before].time;
    pose.blend = span > 0.0f ? std::clamp((time - lip.frames[before].time) / span, 0.0f, 1.0f) : 1.0f;
    return pose;
}

void FacialRig::setViseme(int viseme)
{
    viseme_ = names_.empty() ? 0 : std::clamp(viseme, 0, static_cast<int>(names_.size()) - 1);
}

} // namespace ke
