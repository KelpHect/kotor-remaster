#include "game/GuiSystem.h"

#include <algorithm>
#include <cctype>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool contains(const kotor::GuiExtent& extent, float x, float y)
{
    return x >= static_cast<float>(extent.left)
        && y >= static_cast<float>(extent.top)
        && x < static_cast<float>(extent.left + extent.width)
        && y < static_cast<float>(extent.top + extent.height);
}

void flatten(const kotor::GuiControl& control, std::vector<const kotor::GuiControl*>& out)
{
    out.push_back(&control);
    for (const kotor::GuiControl& child : control.children) {
        flatten(child, out);
    }
}

const kotor::GuiControl* findIn(const kotor::GuiControl& control, const std::string& key)
{
    if (control.key == key || control.tag == key) {
        return &control;
    }
    for (const kotor::GuiControl& child : control.children) {
        if (const kotor::GuiControl* found = findIn(child, key)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

GuiSystem::GuiSystem(kotor::Install& install)
    : install_(install)
{
}

kotor::GuiScreen* GuiSystem::load(const std::string& screen)
{
    const std::string key = lower(screen);
    if (const auto cached = screens_.find(key); cached != screens_.end()) {
        return &cached->second;
    }
    auto data = install_.find(screen, kotor::resType(kotor::ResType::Gui));
    if (!data.has_value()) {
        return nullptr;
    }
    try {
        auto parsed = kotor::parseGui(std::move(*data), screen + ".gui");
        auto inserted = screens_.emplace(key, std::move(parsed));
        return &inserted.first->second;
    } catch (const kotor::FormatError&) {
        return nullptr;
    }
}

bool GuiSystem::setScreen(const std::string& screen, bool resetHistory)
{
    kotor::GuiScreen* loaded = load(screen);
    if (loaded == nullptr) {
        return false;
    }
    if (resetHistory) {
        history_.clear();
    }
    screen_ = loaded;
    screenKey_ = lower(screen);
    states_.clear();
    hovered_.clear();
    pressed_.clear();
    return true;
}

bool GuiSystem::open(const std::string& screen)
{
    return setScreen(screen, true);
}

bool GuiSystem::push(const std::string& screen)
{
    if (screen_ == nullptr) {
        return open(screen);
    }
    const std::string previous = screenKey_;
    if (!setScreen(screen, false)) {
        return false;
    }
    history_.push_back(previous);
    return true;
}

bool GuiSystem::pop()
{
    if (history_.empty()) {
        return false;
    }
    const std::string previous = history_.back();
    history_.pop_back();
    return setScreen(previous, false);
}

void GuiSystem::close()
{
    screen_ = nullptr;
    screenKey_.clear();
    history_.clear();
    states_.clear();
    hovered_.clear();
    pressed_.clear();
}

const std::string& GuiSystem::screenName() const
{
    return screenKey_;
}

const kotor::GuiControl* GuiSystem::find(const std::string& keyOrTag) const
{
    if (screen_ == nullptr) {
        return nullptr;
    }
    for (const kotor::GuiControl& control : screen_->controls) {
        if (const kotor::GuiControl* found = findIn(control, keyOrTag)) {
            return found;
        }
    }
    return nullptr;
}

GuiControlState& GuiSystem::stateFor(const kotor::GuiControl& control)
{
    auto [it, inserted] = states_.try_emplace(control.key);
    if (inserted) {
        it->second.value = control.curValue;
        it->second.selected = control.selected || control.isSelected;
    }
    return it->second;
}

GuiControlState GuiSystem::stateOf(const kotor::GuiControl& control) const
{
    const auto it = states_.find(control.key);
    if (it != states_.end()) {
        return it->second;
    }
    GuiControlState state;
    state.value = control.curValue;
    state.selected = control.selected || control.isSelected;
    return state;
}

std::string GuiSystem::controlAt(float x, float y) const
{
    if (screen_ == nullptr) {
        return {};
    }
    std::vector<const kotor::GuiControl*> controls;
    for (const kotor::GuiControl& control : screen_->controls) {
        flatten(control, controls);
    }
    for (auto it = controls.rbegin(); it != controls.rend(); ++it) {
        const kotor::GuiControl& control = **it;
        if (!stateOf(control).visible) {
            continue;
        }
        if (contains(control.extent, x, y)) {
            return control.key;
        }
    }
    return {};
}

bool GuiSystem::pointerMove(float x, float y)
{
    hovered_ = controlAt(x, y);
    return !hovered_.empty();
}

bool GuiSystem::pointerDown(float x, float y)
{
    const std::string hit = controlAt(x, y);
    if (hit.empty()) {
        pressed_.clear();
        return false;
    }
    const kotor::GuiControl* control = find(hit);
    if (control == nullptr) {
        return false;
    }
    pressed_ = hit;
    stateFor(*control).pressed = true;
    return true;
}

bool GuiSystem::pointerUp(float x, float y)
{
    const std::string released = controlAt(x, y);
    const std::string pressed = pressed_;
    pressed_.clear();
    if (const kotor::GuiControl* control = find(pressed)) {
        stateFor(*control).pressed = false;
    }
    if (pressed.empty() || released != pressed) {
        return false;
    }
    return activate(pressed);
}

bool GuiSystem::activate(const std::string& keyOrTag)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    if (!state.visible || !state.enabled) {
        return false;
    }
    if (control->type == 7) {
        state.selected = !state.selected;
    }
    if (control->type == 2 || control->type == 4 || control->type == 5
        || control->type == 6 || control->type == 7 || control->type == 11) {
        lastAction_ = control->tag.empty() ? control->key : control->tag;
        actionLog_.push_back(lastAction_);
        if (actionLog_.size() > 64) {
            actionLog_.erase(actionLog_.begin());
        }
        if (lastAction_ == "BTN_EXIT") {
            quitRequested_ = true;
        }
    }
    return true;
}

bool GuiSystem::setVisible(const std::string& keyOrTag, bool visible)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).visible = visible;
    return true;
}

bool GuiSystem::setEnabled(const std::string& keyOrTag, bool enabled)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).enabled = enabled;
    return true;
}

bool GuiSystem::setSelected(const std::string& keyOrTag, bool selected)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).selected = selected;
    return true;
}

bool GuiSystem::setValue(const std::string& keyOrTag, float value)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).value = std::clamp(value, 0.0f, control->maxValue);
    return true;
}

float GuiSystem::value(const std::string& keyOrTag) const
{
    const kotor::GuiControl* control = find(keyOrTag);
    return control != nullptr ? stateOf(*control).value : 0.0f;
}

bool GuiSystem::setListSelection(const std::string& keyOrTag, std::size_t index)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).listSelection = index;
    return true;
}

std::size_t GuiSystem::listSelection(const std::string& keyOrTag) const
{
    const kotor::GuiControl* control = find(keyOrTag);
    return control != nullptr ? stateOf(*control).listSelection : 0;
}

GuiControlState GuiSystem::state(const std::string& keyOrTag) const
{
    const kotor::GuiControl* control = find(keyOrTag);
    return control != nullptr ? stateOf(*control) : GuiControlState{};
}

void GuiSystem::clearActions()
{
    lastAction_.clear();
    actionLog_.clear();
}

} // namespace ke
