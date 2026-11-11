#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <string>

namespace ke {

class GameSession;

enum class StanceMotion { Still, Walk, Run };

// The clip a creature shows for how it moves and what it holds (retail 0069f650). `wield` is
// kotor::wieldClass; `drawn` is whether its weapons are out. Empty when the model has none.
std::string stanceClipFor(const kotor::Mdl& model, std::int32_t wield, bool drawn, StanceMotion motion);
// The clip of an attack (retail 00613da0, 00614110); `index` picks among the class's clips.
std::string attackClipFor(const kotor::Mdl& model, std::int32_t wield, std::int32_t index);

// The clip a creature falls with and the one its body rests in. Empty when the model has none.
std::string deathClipFor(const kotor::Mdl& model);
std::string deadClipFor(const kotor::Mdl& model);

int runAnimationFamilyTest(GameSession& game);
// A gallery shot's action on a creature; false when the action is not one of these.
bool runAnimationCaptureAction(GameSession& game, const std::string& action);

} // namespace ke
