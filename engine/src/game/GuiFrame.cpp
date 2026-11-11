#include "game/GuiFrame.h"

#include "imgui.h"

namespace ke {

void pumpGuiInput(GuiSystem& gui, const GuiView& view)
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMousePosValid()) {
        const bool moved = gui.pointerMovedSince(io.MousePos.x, io.MousePos.y);
        const float gx = view.guiX(io.MousePos.x);
        const float gy = view.guiY(io.MousePos.y);
        if (moved || ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            gui.pointerMove(gx, gy);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gui.pointerDown(gx, gy);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            gui.pointerUp(gx, gy);
        }
        if (io.MouseWheel != 0.0f) {
            gui.wheel(gx, gy, io.MouseWheel);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        gui.navigate(0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        gui.navigate(1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        gui.navigate(2);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        gui.navigate(3);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        gui.activateFocused();
    }
}

void pumpGuiPointer(GuiSystem& gui)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMousePosValid()) {
        return;
    }
    const bool moved = gui.pointerMovedSince(io.MousePos.x, io.MousePos.y);
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (moved || clicked || released) {
        gui.pointerMove(io.MousePos.x, io.MousePos.y);
    }
    if (clicked) {
        gui.pointerDown(io.MousePos.x, io.MousePos.y);
    }
    if (released) {
        gui.pointerUp(io.MousePos.x, io.MousePos.y);
    }
}

bool guiBack(GuiSystem& gui)
{
    for (const char* tag : {"BTN_BACK", "BTN_CANCEL", "BTN_EXIT_X"}) {
        if (const kotor::GuiControl* control = gui.control(tag)) {
            const GuiControlState state = gui.state(control->key);
            if (state.visible && state.enabled) {
                return gui.activate(control->key);
            }
        }
    }
    return gui.pop();
}

} // namespace ke
