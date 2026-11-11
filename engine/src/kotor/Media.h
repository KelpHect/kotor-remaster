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

// Where a movie frame lands in a window: scaled by the smaller axis ratio and centred, so
// the picture keeps its own aspect and the rest of the window is black (letterbox for the
// 640x272 cinema frames, pillarbox for the 640x480 logos on a wide window).
struct MovieRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

inline MovieRect fitMovie(int frameWidth, int frameHeight, float displayWidth,
                          float displayHeight)
{
    MovieRect rect;
    if (frameWidth <= 0 || frameHeight <= 0 || displayWidth <= 0.0f || displayHeight <= 0.0f) {
        return rect;
    }
    const float scaleX = displayWidth / static_cast<float>(frameWidth);
    const float scaleY = displayHeight / static_cast<float>(frameHeight);
    const float scale = scaleX < scaleY ? scaleX : scaleY;
    rect.w = static_cast<float>(frameWidth) * scale;
    rect.h = static_cast<float>(frameHeight) * scale;
    rect.x = (displayWidth - rect.w) * 0.5f;
    rect.y = (displayHeight - rect.h) * 0.5f;
    return rect;
}

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
