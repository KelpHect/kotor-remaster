// ACT_508 EnableVideoEffect and ACT_510 DisableVideoEffect.
#include "core/Log.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ke {

namespace {

using Cells = std::optional<std::vector<script::Value>>;

// ACT_508 005471e0 pops the row and calls 005f7710 on the client's renderer
// object: the state is reset, then the row's EnableScanNoise / EnableSaturation
// columns decide which passes run (see render/VideoEffect.h).
Cells enableVideoEffect(ActionCall& call)
{
    call.session.enableVideoEffect(static_cast<std::int32_t>(call.intArg(0, -1)));
    return ActionCall::none();
}

// ACT_510 00547220 calls 005f40e0: effect off, both passes off.
Cells disableVideoEffect(ActionCall& call)
{
    call.session.disableVideoEffect();
    return ActionCall::none();
}

} // namespace

void registerVideoEffectActions(HandlerTable& table)
{
    // Another family may already own a slot (the world-state work touches
    // DisableVideoEffect); never override it silently.
    if (table.get(508) == nullptr) {
        table.add(508, enableVideoEffect);
    }
    if (table.get(510) == nullptr) {
        table.add(510, disableVideoEffect);
    }
}

// --family-test videoeffect: ACT_508/510 driven through compiled NCS fixtures on
// the live action runtime, with the state checked against values derived by hand
// from the shipped videoeffects.2da rows and the retail formula, plus the CPU
// reference of the render pass on a synthetic image (the GL/Vulkan frames are
// checked by scripts/check_video_effect.py).
int runVideoEffectFamilyTest(GameSession& game, const std::string& fixtureDir)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("video effect: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    if (fixtureDir.empty()) {
        logError("video effect: --video-effect-fixtures <dir> is required");
        return 2;
    }
    const auto run = [&](const char* stem) {
        const std::filesystem::path path =
            std::filesystem::path(fixtureDir) / (std::string("video_effect_") + stem + ".ncs");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            logError(concat("video effect: missing fixture ", path.string()));
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return game.runScriptNcsForTest(std::string("video_effect_") + stem, bytes,
                                        game.scriptWorld().pc);
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

    check(!game.videoEffect().active() && game.videoEffect().row == -2, "initial_off");

    // Row 0 (security camera): modulation 1 / 1.4 / 2, saturation 0.15, scan noise on.
    check(run("row0"), "row0_script");
    {
        const VideoEffectState& fx = game.videoEffect();
        check(fx.row == 0 && fx.saturation && fx.scanNoise, "row0_flags");
        const float w0 = 0.3086f * 0.85f;
        const float w1 = 0.6094f * 0.85f;
        const float w2 = 0.0820f * 0.85f;
        check(near(fx.matrix[0], 0.15f + w0) && near(fx.matrix[1], w1) && near(fx.matrix[2], w2),
              "row0_red_row");
        check(near(fx.matrix[3], 1.4f * w0) && near(fx.matrix[4], 1.4f * (w1 + 0.15f))
                  && near(fx.matrix[5], 1.4f * w2),
              "row0_green_row");
        // 2 * w1 exceeds 1.0 and is clamped like retail's register constants.
        check(near(fx.matrix[6], 2.0f * w0) && near(fx.matrix[7], 1.0f)
                  && near(fx.matrix[8], 2.0f * (w2 + 0.15f)),
              "row0_blue_row_clamped");
        check(near(fx.noiseScale[0], 0.02f) && near(fx.noiseTranslate[0], 0.0f),
              "noise_txi_params");
    }
    // Rendered effect on a synthetic ramp: the colours change and obey the matrix.
    {
        std::vector<std::uint8_t> frame(64 * 64 * 4, 255);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                std::uint8_t* px = &frame[(static_cast<std::size_t>(y) * 64 + x) * 4];
                px[0] = static_cast<std::uint8_t>(x * 4);
                px[1] = static_cast<std::uint8_t>(y * 4);
                px[2] = 128;
            }
        }
        std::vector<std::uint8_t> on = frame;
        applyVideoEffectCpu(game.videoEffect(), on.data(), 64, 64);
        double meanDiff = 0.0;
        for (std::size_t i = 0; i < frame.size(); ++i) {
            meanDiff += std::abs(static_cast<int>(frame[i]) - static_cast<int>(on[i]));
        }
        meanDiff /= static_cast<double>(frame.size());
        logInfo(concat("video effect: row0 mean abs channel diff on ramp = ", meanDiff));
        check(meanDiff > 8.0, "frame_changed");
        // Gray input keeps the luma and scales per channel; with scan noise
        // disabled the result is exactly the matrix product.
        VideoEffectState noNoise = game.videoEffect();
        noNoise.scanNoise = false;
        std::uint8_t gray[4] = {128, 128, 128, 255};
        applyVideoEffectCpu(noNoise, gray, 1, 1);
        const float c = 128.0f / 255.0f;
        const auto& m = noNoise.matrix;
        check(gray[0] == static_cast<std::uint8_t>(std::min(1.0f, (m[0] + m[1] + m[2]) * c) * 255.0f + 0.5f)
                  && gray[2] == static_cast<std::uint8_t>(std::min(1.0f, (m[6] + m[7] + m[8]) * c) * 255.0f + 0.5f),
              "cpu_matrix_product");
    }

    check(run("off"), "off_script");
    check(!game.videoEffect().active() && game.videoEffect().row == -2, "disable_restores_state");
    {
        std::vector<std::uint8_t> frame(16 * 16 * 4, 90);
        const std::vector<std::uint8_t> before = frame;
        applyVideoEffectCpu(game.videoEffect(), frame.data(), 16, 16);
        check(frame == before, "disable_restores_frame");
    }

    // Row 2 (HK-47 free-look): red modulation 2.4 clamps the red row's coefficients.
    check(run("row2"), "row2_script");
    {
        const VideoEffectState& fx = game.videoEffect();
        check(fx.row == 2 && fx.saturation && fx.scanNoise, "row2_flags");
        check(near(fx.matrix[0], 1.0f) && near(fx.matrix[1], 1.0f), "row2_red_clamped");
        check(near(fx.matrix[3], 0.4f * 0.3086f * 0.8f), "row2_green_row");
    }
    // A row the 2DA does not define leaves nothing running (retail 005f7710).
    check(run("row99"), "row99_script");
    check(!game.videoEffect().active() && game.videoEffect().row == -2, "undefined_row_off");
    // Enable resets before applying: row 1 after row 2 is exactly row 1.
    check(run("row2") && run("row1"), "row1_after_row2");
    check(game.videoEffect().row == 1 && near(game.videoEffect().matrix[0], 0.15f + 0.3086f * 0.85f),
          "enable_resets_previous");
    check(run("off") && !game.videoEffect().active(), "final_off");

    logInfo(ok ? "video effect: PASS" : "video effect: FAIL");
    return ok ? 0 : 70;
}

} // namespace ke
