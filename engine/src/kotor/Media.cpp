#include "kotor/Media.h"

#include "kotor/Formats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ke::kotor {

float BikMovie::durationSeconds() const
{
    return framesPerSecond > 0.0f
        ? static_cast<float>(frames) / framesPerSecond : 0.0f;
}

BikMovie parseBik(const std::vector<std::uint8_t>& bytes, const std::string& name,
                  std::uint64_t fileSize)
{
    constexpr std::size_t kTableOffset = 0x38;
    if (bytes.size() < kTableOffset) {
        throw FormatError(name + ": shorter than Bink header");
    }
    if (std::memcmp(bytes.data(), "BIK", 3) != 0) {
        throw FormatError(name + ": not a Bink container");
    }
    const char version = static_cast<char>(bytes[3]);
    if (version < 'a' || version > 'i') {
        throw FormatError(name + ": unknown Bink version");
    }
    const std::uint64_t sizeField = readU32(bytes.data(), bytes.size(), 4, "BIK size");
    if (sizeField != fileSize - 8u) {
        throw FormatError(name + ": BIK size field mismatch");
    }
    BikMovie movie;
    movie.name = name;
    movie.version = version;
    movie.frames = readU32(bytes.data(), bytes.size(), 8, "BIK frames");
    movie.largestFrame = readU32(bytes.data(), bytes.size(), 12, "BIK largest frame");
    if (readU32(bytes.data(), bytes.size(), 16, "BIK frames2") != movie.frames) {
        throw FormatError(name + ": BIK duplicate frame count mismatch");
    }
    movie.width = readU32(bytes.data(), bytes.size(), 20, "BIK width");
    movie.height = readU32(bytes.data(), bytes.size(), 24, "BIK height");
    const float fpsMilli = readF32(bytes.data(), bytes.size(), 28, "BIK fps");
    movie.framesPerSecond = fpsMilli > 0.0f ? fpsMilli / 1000.0f : 30.0f;
    if (movie.frames == 0 || movie.width == 0 || movie.height == 0
        || movie.width > 8192 || movie.height > 8192) {
        throw FormatError(name + ": implausible BIK video header");
    }
    const std::size_t tableBytes = (static_cast<std::size_t>(movie.frames) + 1u) * 4u;
    if (kTableOffset + tableBytes > bytes.size()) {
        throw FormatError(name + ": BIK frame table truncated");
    }
    movie.frameOffsets.reserve(movie.frames + 1u);
    for (std::uint32_t i = 0; i <= movie.frames; ++i) {
        movie.frameOffsets.push_back(readU32(bytes.data(), bytes.size(),
                                             kTableOffset + i * 4u, "BIK frame offset"));
    }
    const std::uint64_t end = fileSize;
    const auto sentinel = std::find(movie.frameOffsets.begin() + 1,
                                    movie.frameOffsets.end(), end);
    if (sentinel == movie.frameOffsets.end()) {
        throw FormatError(name + ": BIK frame table has no EOF sentinel");
    }
    const std::size_t sentinelIndex = static_cast<std::size_t>(
        std::distance(movie.frameOffsets.begin(), sentinel));
    for (std::size_t i = 0; i + 1 < sentinelIndex; ++i) {
        if (movie.frameOffsets[i] >= movie.frameOffsets[i + 1]) {
            throw FormatError(name + ": BIK frame table not ascending");
        }
    }
    if (movie.frameOffsets.front() < kTableOffset + tableBytes) {
        throw FormatError(name + ": BIK frame table overlaps first frame");
    }
    movie.fileSize = fileSize;
    return movie;
}

std::uint8_t LipSync::visemeAt(float time) const
{
    if (frames.empty()) {
        return 0;
    }
    std::size_t lo = 0;
    std::size_t hi = frames.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (frames[mid].time <= time) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo == 0 ? frames.front().viseme : frames[lo - 1].viseme;
}

LipSync parseLip(const std::vector<std::uint8_t>& bytes, const std::string& name)
{
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "LIP V1.0", 8) != 0) {
        throw FormatError(name + ": not a LIP V1.0 resource");
    }
    LipSync lip;
    lip.name = name;
    lip.duration = readF32(bytes.data(), bytes.size(), 8, "LIP duration");
    const std::uint32_t count = readU32(bytes.data(), bytes.size(), 12, "LIP count");
    if (bytes.size() != 16u + static_cast<std::size_t>(count) * 5u) {
        throw FormatError(name + ": LIP size does not match frame count");
    }
    lip.frames.reserve(count);
    float previous = -1.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t offset = 16u + static_cast<std::size_t>(i) * 5u;
        LipFrame frame;
        frame.time = readF32(bytes.data(), bytes.size(), offset, "LIP time");
        frame.viseme = bytes[offset + 4u];
        if (frame.viseme > 15 || frame.time < previous
            || frame.time < -1.0e-6f || frame.time > lip.duration + 1.0e-6f) {
            throw FormatError(name + ": invalid LIP frame");
        }
        previous = frame.time;
        lip.frames.push_back(frame);
    }
    if (count != 0) {
        if (lip.frames.front().time != 0.0f
            || std::fabs(lip.frames.back().time - lip.duration) > 1.0e-6f) {
            throw FormatError(name + ": LIP timeline does not span its duration");
        }
    } else if (lip.duration != 0.0f) {
        throw FormatError(name + ": empty LIP has nonzero duration");
    }
    return lip;
}

} // namespace ke::kotor
