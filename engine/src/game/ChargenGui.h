#pragma once

// The shipped character-generation screens (classsel, portcust, abchrgen, skchrgen,
// ftchrgen, name) driven by GuiSystem actions. The window loop and the headless
// gate share this state machine; both feed it the same BTN_* / *_PLUS_BTN actions
// that a pointer click on the control produces.

#include "game/Chargen.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"
#include "kotor/Formats.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace ke {

struct ChargenGuiState {
    explicit ChargenGuiState(const Rules& rules) : chargen(rules) {}
    Chargen chargen;
    int step = 0; // classsel, portcust, abchrgen, skchrgen, ftchrgen, name
    std::string nameBuffer = "Player";
    bool active = true;
    // The moment the flow finished and handed the character to the session.
    bool finished = false;
};

const char* chargenScreenName(int step);

// Returns true when the action belonged to the chargen flow.
bool handleChargenGuiAction(const std::string& action, ChargenGuiState& state, GuiSystem& gui,
                            GameSession& game);

// Decoded portrait art (portraits.2da BaseResRef*, TPC or TGA via the VFS), cached by
// resref. Used by the window overlay (uploaded as a UI image) and the headless gate.
class PortraitImages {
public:
    explicit PortraitImages(kotor::Install& install) : install_(install) {}
    // Null when the resref names no texture in the install.
    const kotor::Image* image(const std::string& resref);
    // A stable digest of the decoded pixels, 0 when missing; lets a gate compare art.
    std::uint64_t digest(const std::string& resref);

private:
    kotor::Install& install_;
    std::map<std::string, std::optional<kotor::Image>> cache_;
};

} // namespace ke
