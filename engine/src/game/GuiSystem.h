#pragma once

#include "kotor/Formats.h"
#include "kotor/Gui.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ke {

struct GuiControlState {
    bool visible = true;
    bool enabled = true;
    bool hovered = false;
    bool pressed = false;
    bool selected = false;
    float value = 0.0f;
    std::size_t listSelection = 0;
};

// Engine-side GUI interaction: hit-testing, per-control state, actions and a
// screen stack. The shipped .gui files carry no script fields, so actions are
// tags consumed by GameSession/Application (main menu, HUD icons, exit).
class GuiSystem {
public:
    explicit GuiSystem(kotor::Install& install);

    bool open(const std::string& screen);
    bool push(const std::string& screen);
    bool pop();
    void close();
    bool active() const { return screen_ != nullptr; }
    const kotor::GuiScreen* screen() const { return screen_; }
    const std::string& screenName() const;
    std::size_t historyDepth() const { return history_.size(); }

    std::string controlAt(float x, float y) const;
    bool pointerMove(float x, float y);
    bool pointerDown(float x, float y);
    bool pointerUp(float x, float y);
    const std::string& hovered() const { return hovered_; }
    bool activate(const std::string& keyOrTag);

    bool setVisible(const std::string& keyOrTag, bool visible);
    bool setEnabled(const std::string& keyOrTag, bool enabled);
    bool setSelected(const std::string& keyOrTag, bool selected);
    bool setValue(const std::string& keyOrTag, float value);
    float value(const std::string& keyOrTag) const;
    bool setListSelection(const std::string& keyOrTag, std::size_t index);
    std::size_t listSelection(const std::string& keyOrTag) const;
    GuiControlState state(const std::string& keyOrTag) const;

    const std::string& lastAction() const { return lastAction_; }
    const std::vector<std::string>& actionLog() const { return actionLog_; }
    void clearActions();
    bool quitRequested() const { return quitRequested_; }

private:
    const kotor::GuiControl* find(const std::string& keyOrTag) const;
    kotor::GuiScreen* load(const std::string& screen);
    bool setScreen(const std::string& screen, bool resetHistory);
    GuiControlState& stateFor(const kotor::GuiControl& control);
    GuiControlState stateOf(const kotor::GuiControl& control) const;

    kotor::Install& install_;
    std::map<std::string, kotor::GuiScreen> screens_;
    kotor::GuiScreen* screen_ = nullptr;
    std::string screenKey_;
    std::vector<std::string> history_;
    std::map<std::string, GuiControlState> states_;
    std::string hovered_;
    std::string pressed_;
    std::string lastAction_;
    std::vector<std::string> actionLog_;
    bool quitRequested_ = false;
};

} // namespace ke
