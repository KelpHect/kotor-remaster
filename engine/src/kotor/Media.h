#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ke::kotor {

// Bink 1 container header plus the validated frame-offset table. Pixel payload
// decoding is intentionally outside the container layer (FFmpeg is the oracle).
struct BikMovie {
    std::string name;
    char version = 'i';
    std::uint32_t frames = 0;
    std::uint32_t largestFrame = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float framesPerSecond = 30.0f;
    std::vector<std::uint32_t> frameOffsets;
    std::uint64_t fileSize = 0;

    float durationSeconds() const;
};

BikMovie parseBik(const std::vector<std::uint8_t>& bytes, const std::string& name,
                  std::uint64_t fileSize);

struct LipFrame {
    float time = 0.0f;
    std::uint8_t viseme = 0;
};

// KotOR LIP V1.0 viseme timeline.
struct LipSync {
    std::string name;
    float duration = 0.0f;
    std::vector<LipFrame> frames;

    std::uint8_t visemeAt(float time) const;
};

LipSync parseLip(const std::vector<std::uint8_t>& bytes, const std::string& name);

} // namespace ke::kotor
