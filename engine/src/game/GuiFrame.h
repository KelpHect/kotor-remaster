#pragma once

// Per-frame glue between the window's input (ImGui's IO, fed by real SDL events) and a
// GuiSystem: pointer move, press and release, wheel and keyboard navigation.

#include "game/GuiPaint.h"
#include "game/GuiSystem.h"

namespace ke {

// Reads ImGui's mouse and keyboard state and forwards it to `gui` in design space.
// Pointer moves are forwarded only when the pointer actually moved, so keyboard focus
// is not undone by a resting pointer.
void pumpGuiInput(GuiSystem& gui, const GuiView& view);

// The pointer half of pumpGuiInput for a screen that answers in window pixels (GuiSystem::setPicker):
// no keyboard, so a key the world uses never reaches the screen's last hovered button.
void pumpGuiPointer(GuiSystem& gui);

// Escape: activates BTN_BACK / BTN_CANCEL when the screen has one that is visible and
// enabled, otherwise pops the screen stack. False when there is nothing to go back to.
bool guiBack(GuiSystem& gui);

} // namespace ke
