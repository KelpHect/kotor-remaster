#include "game/ChargenGui.h"

#include <algorithm>

namespace ke {

const char* chargenScreenName(int step)
{
    switch (step) {
    case 0: return "classsel";
    case 1: return "portcust";
    case 2: return "abchrgen";
    case 3: return "skchrgen";
    case 4: return "ftchrgen";
    case 5: return "name";
    default: return "classsel";
    }
}

bool handleChargenGuiAction(const std::string& action, ChargenGuiState& state, GuiSystem& gui,
                            GameSession& game)
{
    if (!state.active) {
        return false;
    }
    const auto open = [&](int step) {
        state.step = step;
        gui.open(chargenScreenName(step));
    };
    if (state.step == 0) {
        // BTN_SEL1..6: the six class/gender slots (retail 006dc3c0).
        if (action.rfind("BTN_SEL", 0) == 0 && action.size() == 8) {
            const int index = action[7] - '1';
            if (index >= 0 && index < Chargen::kSlotCount) {
                state.chargen.selectSlot(index);
                open(1);
            }
            return true;
        }
        if (action == "BTN_BACK") {
            state.active = false;
            gui.close();
            return true;
        }
        return false;
    }
    if (action == "BTN_BACK") {
        open(state.step - 1);
        return true;
    }
    if (state.step == 1) {
        if (action == "BTN_ARRL") {
            state.chargen.cyclePortrait(-1);
            return true;
        }
        if (action == "BTN_ARRR") {
            state.chargen.cyclePortrait(1);
            return true;
        }
        if (action == "BTN_ACCEPT") {
            open(2);
            return true;
        }
        return false;
    }
    if (state.step == 2) {
        static const char* names[] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
        for (int i = 0; i < 6; ++i) {
            if (action == std::string(names[i]) + "_PLUS_BTN") {
                state.chargen.adjustAttribute(i, 1);
                return true;
            }
            if (action == std::string(names[i]) + "_MINUS_BTN") {
                state.chargen.adjustAttribute(i, -1);
                return true;
            }
        }
        if (action == "BTN_ACCEPT") {
            open(3);
            return true;
        }
        return false;
    }
    if (state.step == 3) {
        static const char* names[] = {"COM", "DEM", "STE", "AWA", "PER", "REP", "SEC", "TRE"};
        for (int i = 0; i < 8; ++i) {
            if (action == std::string(names[i]) + "_PLUS_BTN") {
                state.chargen.adjustSkill(i, 1);
                return true;
            }
            if (action == std::string(names[i]) + "_MINUS_BTN") {
                state.chargen.adjustSkill(i, -1);
                return true;
            }
        }
        if (action == "BTN_ACCEPT") {
            open(4);
            return true;
        }
        return false;
    }
    if (state.step == 4) {
        if (action == "BTN_SELECT") {
            const std::vector<std::int32_t> feats = state.chargen.availableFeats();
            if (!feats.empty()) {
                state.chargen.toggleFeat(feats.front());
            }
            return true;
        }
        if (action == "BTN_ACCEPT") {
            open(5);
            return true;
        }
        return false;
    }
    if (state.step == 5) {
        if (action == "END_BTN") {
            state.chargen.setName(state.nameBuffer);
            const CharacterSheet sheet = state.chargen.build();
            // The sheet carries gender and portrait row; the session derives the body
            // (appearance.2da by class size) from them.
            game.applyCharacter(sheet);
            game.setPlayerAppearance(static_cast<std::uint32_t>(
                                         std::max(0, state.chargen.appearanceId())),
                                     state.chargen.name());
            game.setThirdPerson(true);
            state.active = false;
            state.finished = true;
            gui.close();
            return true;
        }
        return false;
    }
    return false;
}

const kotor::Image* PortraitImages::image(const std::string& resref)
{
    if (resref.empty()) {
        return nullptr;
    }
    auto found = cache_.find(resref);
    if (found == cache_.end()) {
        std::optional<kotor::Image> decoded;
        std::uint32_t type = 0;
        if (auto data = install_.findTexture(resref, &type); data.has_value()) {
            try {
                decoded = kotor::decodeTexture(*data, type, resref);
                if (type == static_cast<std::uint32_t>(kotor::ResType::Tpc)) {
                    // TPC rows are stored bottom-up (the scene shaders flip V); a UI image
                    // is sampled top-down, so turn the rows over once here.
                    const std::size_t row = static_cast<std::size_t>(decoded->width) * 4u;
                    for (int y = 0; y < decoded->height / 2; ++y) {
                        std::swap_ranges(
                            decoded->rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
                            decoded->rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                            decoded->rgba.begin()
                                + static_cast<std::ptrdiff_t>((decoded->height - 1 - y) * row));
                    }
                }
            } catch (const kotor::FormatError&) {
                decoded.reset();
            }
        }
        found = cache_.emplace(resref, std::move(decoded)).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

std::uint64_t PortraitImages::digest(const std::string& resref)
{
    const kotor::Image* picture = image(resref);
    if (picture == nullptr) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : picture->rgba) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(picture->width) << 32 | static_cast<std::uint32_t>(picture->height);
    return hash == 0 ? 1 : hash;
}

} // namespace ke
