#include "render/VideoEffect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ke {

namespace {

// Luma weights stored in the binary next to the colour pass (rwgt, gwgt,
// bwgt): 0.3086, 0.6094, 0.0820.
constexpr float kLuma[3] = {0.3086f, 0.6094f, 0.0820f};

float clampCoefficient(float value)
{
    return value > 1.0f ? 1.0f : value;
}

} // namespace

std::array<float, 9> videoEffectMatrix(const float modulation[3], float saturation)
{
    const float inverse = 1.0f - saturation;
    const float w0 = kLuma[0] * inverse;
    const float w1 = kLuma[1] * inverse;
    const float w2 = kLuma[2] * inverse;
    std::array<float, 9> m{
        modulation[0] * (saturation + w0), modulation[0] * w1, modulation[0] * w2,
        modulation[1] * w0, modulation[1] * (w1 + saturation), modulation[1] * w2,
        modulation[2] * w0, modulation[2] * w1, modulation[2] * (w2 + saturation)};
    // Retail clamps every coefficient to at most 1.0 before loading the
    // combiner constants. The last one tests the blue row's first coefficient
    // (m[6]) rather than itself; that retail quirk is reproduced on purpose.
    const float blueRowFirst = m[6];
    for (int i = 0; i < 8; ++i) {
        m[i] = clampCoefficient(m[i]);
    }
    if (blueRowFirst > 1.0f) {
        m[8] = 1.0f;
    }
    return m;
}

VideoEffectState makeVideoEffectState(std::int32_t row, const VideoEffectRow& definition)
{
    VideoEffectState state;
    if (row < 0) {
        return state;
    }
    if (definition.enableScanNoise) {
        state.row = row;
        state.scanNoise = true;
    }
    if (definition.enableSaturation) {
        state.row = row;
        state.saturation = true;
        state.matrix = videoEffectMatrix(definition.modulation, definition.saturation);
    }
    return state;
}

void applyNoiseTxi(const std::string& txi, VideoEffectState& state)
{
    std::istringstream stream(txi);
    std::string token;
    const auto readBlock = [&](std::array<float, 3>& target) {
        int count = 0;
        stream >> count;
        for (int i = 0; i < count; ++i) {
            std::string value;
            if (!(stream >> value)) {
                return;
            }
            if (i < 3) {
                target[static_cast<std::size_t>(i)] = std::strtof(value.c_str(), nullptr);
            }
        }
    };
    while (stream >> token) {
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (token == "channelscale") {
            readBlock(state.noiseScale);
        } else if (token == "channeltranslate") {
            readBlock(state.noiseTranslate);
        }
    }
}

std::vector<std::uint8_t> makeScanNoiseTexture(std::uint32_t seed)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kScanNoiseSize) * kScanNoiseSize * 4u);
    std::uint32_t x = seed != 0 ? seed : 1u;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        pixels[i] = (i % 4 == 3) ? 255u : static_cast<std::uint8_t>(x >> 24);
    }
    return pixels;
}

void applyVideoEffectCpu(const VideoEffectState& state, std::uint8_t* rgba, int width, int height)
{
    if (!state.active() || rgba == nullptr || width <= 0 || height <= 0) {
        return;
    }
    static const std::vector<std::uint8_t> noise = makeScanNoiseTexture();
    const auto texel = [&](int column, int row, int channel) {
        const int c = ((column % kScanNoiseSize) + kScanNoiseSize) % kScanNoiseSize;
        const int r = ((row % kScanNoiseSize) + kScanNoiseSize) % kScanNoiseSize;
        return static_cast<float>(noise[(static_cast<std::size_t>(r) * kScanNoiseSize + c) * 4u
                                        + static_cast<std::size_t>(channel)])
            / 255.0f;
    };
    for (int y = 0; y < height; ++y) {
        // Linear filtering at s = 0 blends the last and first columns equally.
        float rowNoise[3]{};
        if (state.scanNoise) {
            const float t = kScanNoiseRepeats * (static_cast<float>(y) + 0.5f)
                / static_cast<float>(height);
            const float v = t * static_cast<float>(kScanNoiseSize) - 0.5f;
            const int v0 = static_cast<int>(std::floor(v));
            const float f = v - static_cast<float>(v0);
            for (int channel = 0; channel < 3; ++channel) {
                const float a = 0.5f * (texel(0, v0, channel) + texel(kScanNoiseSize - 1, v0, channel));
                const float b = 0.5f * (texel(0, v0 + 1, channel) + texel(kScanNoiseSize - 1, v0 + 1, channel));
                const float sample = a + (b - a) * f;
                rowNoise[channel] = sample * state.noiseScale[static_cast<std::size_t>(channel)]
                    + state.noiseTranslate[static_cast<std::size_t>(channel)];
            }
        }
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pixel = rgba + (static_cast<std::size_t>(y) * width + x) * 4u;
            const float in[3] = {pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f};
            float out[3] = {in[0], in[1], in[2]};
            if (state.saturation) {
                for (int c = 0; c < 3; ++c) {
                    out[c] = state.matrix[static_cast<std::size_t>(c * 3)] * in[0]
                        + state.matrix[static_cast<std::size_t>(c * 3 + 1)] * in[1]
                        + state.matrix[static_cast<std::size_t>(c * 3 + 2)] * in[2];
                }
            }
            for (int c = 0; c < 3; ++c) {
                const float value = std::clamp(out[c] + rowNoise[c], 0.0f, 1.0f);
                pixel[c] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
        }
    }
}

} // namespace ke
