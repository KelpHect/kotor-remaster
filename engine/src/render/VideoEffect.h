#pragma once

// Retail "video frame buffer effects": ACT_508 EnableVideoEffect(row) and
// ACT_510 DisableVideoEffect() drive a post-processing state that the renderer
// applies to the 3D scene. Rows come from videoeffects.2da; the engine ships
// three (security camera, T3-M4 free-look, HK-47 free-look). Everything here is
// backend independent: GameSession owns the state, the GL and Vulkan backends
// consume it, and the CPU reference below is what the unit gate and both GPU
// shaders are checked against.
//
// Evidence (swkotor.exe 1.0.3.0):
//   005f7710  enable: clears the row marker (-2) and both flags, then reads the
//             row's EnableScanNoise and EnableSaturation columns. A missing 2DA,
//             a negative row or a missing row leaves everything off.
//   005f40e0  disable: marker -2 and both flags off.
//   004343d0 / 00434890  the colour pass. Per pixel, out_c = M[c] . rgb with
//             M[c] = mod_c * ((1 - s) * luma + s * e_c), luma = (0.3086, 0.6094,
//             0.0820) read from the binary's constants, then each matrix
//             coefficient is clamped to at most 1.0 (register-constant limit).
//   00432780 / 00432ad0  the scan-noise pass: the scene copy plus a repeating
//             filmnoisetex sample whose s coordinate is fixed at 0 and whose t
//             spans 7.5 repeats over the screen height (horizontal scan lines).
//
// Not verified against retail: the order of the two passes (colour first here),
// and whether the procedural "random" noise texture is regenerated each frame
// (it is generated once, deterministically, here).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ke {

struct VideoEffectState {
    std::int32_t row = -2; // retail marker: -2 none, otherwise the videoeffects.2da row
    bool saturation = false; // colour matrix pass enabled
    bool scanNoise = false;  // scan-noise pass enabled
    // Row-major 3x3, out.rgb = matrix * in.rgb.
    std::array<float, 9> matrix{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    // filmnoisetex.txi channelscale / channeltranslate (read from the install).
    std::array<float, 3> noiseScale{0.02f, 0.02f, 0.02f};
    std::array<float, 3> noiseTranslate{0.0f, 0.0f, 0.0f};

    // The options' Frame Buffer box (CB_FRAMEBUFF; retail 0061d750 sets 0078d98c, the gate at the top of
    // FrameBufferModifications 00437520 / 00435bd0). Off: neither pass below draws.
    bool frameBuffer = true;
    // Speed blur (FrameBufferModifications flag 0x10; RenderAccumulationBlur 00433da0 NV / 00433970 ATI):
    // out = ratio x the last blurred frame + (1 - ratio) x this frame, kept for the next frame. The first
    // frame after it starts (007a698d clear) is only kept.
    bool speedBlur = false;
    float speedBlurRatio = 0.98f;
    // ProgFX 1800 (flag 0x20; RenderForceDistortion 004331d0 NV / 00432d70 ATI): these entities draw
    // no colour; where they cover the frame it shows the frame moved by the ring texture. The phase
    // is RingDistortTextureController's (00460a90).
    std::vector<std::uint32_t> distortEntities;
    float distortPhase = 0.0f;
    // Stencil shadows (kotor::buildShadowFrame), handed over with the effect each frame: the
    // options' Shadows box (0078e3a8) and the scene focus (scene +0xa8).
    bool shadows = false;
    std::array<float, 3> shadowFocus{0.0f, 0.0f, 0.0f};

    bool active() const { return saturation || scanNoise; }
    bool blurring() const { return frameBuffer && speedBlur; }
    bool distorting() const { return frameBuffer && !distortEntities.empty(); }
    bool hides(std::uint32_t entity) const
    {
        for (const std::uint32_t one : distortEntities) {
            if (one == entity) {
                return true;
            }
        }
        return false;
    }
    bool operator==(const VideoEffectState&) const = default;
};

// One videoeffects.2da row as the retail reader consumes it.
struct VideoEffectRow {
    bool enableSaturation = false;
    bool enableScanNoise = false;
    float modulation[3]{1.0f, 1.0f, 1.0f};
    float saturation = 1.0f;
};

constexpr int kScanNoiseSize = 32;      // filmnoisetex defaultwidth/defaultheight
constexpr float kScanNoiseRepeats = 7.5f; // t span over the screen height

// Retail colour matrix (see the evidence block above).
std::array<float, 9> videoEffectMatrix(const float modulation[3], float saturation);

// The state retail's enable routine ends in for a row.
VideoEffectState makeVideoEffectState(std::int32_t row, const VideoEffectRow& definition);

// Parses `channelscale 4` / `channeltranslate 4` blocks of a procedural TXI.
// Unknown or absent blocks keep the defaults already in `state`.
void applyNoiseTxi(const std::string& txi, VideoEffectState& state);

// Deterministic 32x32 RGBA8 "random" procedural texture (unscaled 0..255
// channels; scale and translate are applied when sampling).
std::vector<std::uint8_t> makeScanNoiseTexture(std::uint32_t seed = 0x4B4F544Fu);

// CPU reference of the whole effect on a top-down RGBA8 image, in place. The
// noise sample mirrors the GPU path (REPEAT + LINEAR, s = 0).
void applyVideoEffectCpu(const VideoEffectState& state, std::uint8_t* rgba, int width, int height);

} // namespace ke
