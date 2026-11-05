#include "game/VideoEffectController.h"

#include "core/Log.h"

#include <cstdlib>
#include <string>

namespace ke {

namespace {

constexpr std::uint32_t kTxiType = 2022;

float cellFloat(const kotor::TwoDa& table, std::size_t row, const char* column, float fallback)
{
    const int index = table.columnIndex(column);
    if (index < 0) {
        return fallback;
    }
    const std::string text = table.cell(row, static_cast<std::size_t>(index));
    if (text.empty() || text == "****") {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    return end == text.c_str() ? fallback : value;
}

bool cellFlag(const kotor::TwoDa& table, std::size_t row, const char* column)
{
    return cellFloat(table, row, column, 0.0f) != 0.0f;
}

} // namespace

void VideoEffectController::load(kotor::Install& install)
{
    if (loaded_) {
        return;
    }
    loaded_ = true;
    if (auto data = install.find("videoeffects", kotor::resType(kotor::ResType::TwoDa));
        data.has_value()) {
        try {
            table_ = kotor::TwoDa::parse(std::move(*data), "videoeffects");
        } catch (const kotor::FormatError& error) {
            logWarn(concat("videoeffects.2da unreadable: ", error.what()));
        }
    }
    if (auto txi = install.find("filmnoisetex", kTxiType); txi.has_value()) {
        applyNoiseTxi(std::string(txi->begin(), txi->end()), noiseDefaults_);
    }
}

void VideoEffectController::disable()
{
    state_ = VideoEffectState{};
    state_.noiseScale = noiseDefaults_.noiseScale;
    state_.noiseTranslate = noiseDefaults_.noiseTranslate;
}

void VideoEffectController::enable(kotor::Install& install, std::int32_t row)
{
    load(install);
    disable();
    if (!table_.has_value() || row < 0 || static_cast<std::size_t>(row) >= table_->rowCount()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(row);
    VideoEffectRow definition;
    definition.enableScanNoise = cellFlag(*table_, index, "EnableScanNoise");
    definition.enableSaturation = cellFlag(*table_, index, "EnableSaturation");
    definition.modulation[0] = cellFloat(*table_, index, "ModulationRed", 0.0f);
    definition.modulation[1] = cellFloat(*table_, index, "ModulationGreen", 0.0f);
    definition.modulation[2] = cellFloat(*table_, index, "ModulationBlue", 0.0f);
    definition.saturation = cellFloat(*table_, index, "Saturation", 0.0f);
    VideoEffectState next = makeVideoEffectState(row, definition);
    next.noiseScale = noiseDefaults_.noiseScale;
    next.noiseTranslate = noiseDefaults_.noiseTranslate;
    state_ = next;
    if (state_.active()) {
        ++applied_;
    }
}

VideoEffectController::NodeResult VideoEffectController::applyDialogueNode(
    kotor::Install& install, std::int32_t camVidEffect, bool staticCamera)
{
    if (!staticCamera) {
        const bool wasActive = state_.active();
        disable();
        return wasActive ? NodeResult::Cleared : NodeResult::Unchanged;
    }
    if (camVidEffect == -1 || camVidEffect == -2) {
        return NodeResult::Unchanged;
    }
    enable(install, camVidEffect);
    if (!state_.active()) {
        ++rejected_;
        logWarn(concat("CamVidEffect ", camVidEffect,
                       " is not a videoeffects.2da row with an enabled pass; effect ignored"));
        return NodeResult::Rejected;
    }
    return NodeResult::Applied;
}

} // namespace ke
