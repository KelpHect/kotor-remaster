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
        // Retail 00627980 keeps top.gui (the eight page tabs) over the in-game menu's pages. Painted,
        // hit tested and laid out with the page; GameScreens binds it.
        static constexpr const char* kPagesWithTabs[] = {"equip", "inventory", "character", "abilities", "messages",
                                                         "journal", "map", "optionsingame"};
        if (std::find(std::begin(kPagesWithTabs), std::end(kPagesWithTabs), key) != std::end(kPagesWithTabs)) {
            if (auto top = install_.find("top", kotor::resType(kotor::ResType::Gui))) {
                const kotor::GuiScreen tabs = kotor::parseGui(std::move(*top), "top.gui");
                parsed.controls.insert(parsed.controls.end(), tabs.controls.begin(), tabs.controls.end());
            }
        }
        if (key == "optionsmain") {
            // RETAIL-PROVEN: CSWGuiOptionsMain 006e3e80 moves its button array (Gameplay, Feedback,
            // Auto-Pause, Graphics, Sound) 0x14 = 20 px down after loading the file. Paired: walk
            // 0004_options (retail Gameplay at y 273, the file says 109 + 144 = 253).
            for (kotor::GuiControl& control : parsed.controls) {
                if (control.tag == "BTN_GAMEPLAY" || control.tag == "BTN_FEEDBACK" || control.tag == "BTN_AUTOPAUSE"
                    || control.tag == "BTN_GRAPHICS" || control.tag == "BTN_SOUND") {
                    control.extent.top += 20;
                }
            }
        }
        auto inserted = screens_.emplace(key, std::move(parsed));
        return &inserted.first->second;
    } catch (const kotor::FormatError&) {
        return nullptr;
    }
}

kotor::GuiScreen* GuiSystem::variant(const std::string& base, const std::string& alias)
{
    const std::string key = lower(alias);
    if (const auto found = screens_.find(key); found != screens_.end()) {
        return &found->second;
    }
    const kotor::GuiScreen* shipped = load(base);
    if (shipped == nullptr) {
        return nullptr;
    }
    kotor::GuiScreen copy = *shipped;
    return &screens_.emplace(key, std::move(copy)).first->second;
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
    pumpX_ = pumpY_ = -1.0e9f;
    focused_.clear();
    lastItem_ = -1;
    pressed_.clear();
    return true;
}

bool GuiSystem::open(const std::string& screen)
{
    under_.clear();
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

bool GuiSystem::pushModal(const std::string& screen)
{
    kotor::GuiScreen* next = load(screen);
    if (next == nullptr) {
        return false;
    }
    if (screen_ == nullptr) {
        return open(screen);
    }
    Layer layer;
    layer.screen = screen_;
    layer.key = screenKey_;
    layer.states = std::move(states_);
    layer.hovered = hovered_;
    layer.focused = focused_;
    under_.push_back(std::move(layer));
    screen_ = next;
    screenKey_ = lower(screen);
    states_.clear();
    hovered_.clear();
    pumpX_ = pumpY_ = -1.0e9f;
    focused_.clear();
    pressed_.clear();
    return true;
}

bool GuiSystem::popModal()
{
    if (under_.empty()) {
        return false;
    }
    Layer layer = std::move(under_.back());
    under_.pop_back();
    screen_ = layer.screen;
    screenKey_ = layer.key;
    states_ = std::move(layer.states);
    hovered_.clear();
    pumpX_ = pumpY_ = -1.0e9f;
    focused_ = layer.focused;
    pressed_.clear();
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
    under_.clear();
    screen_ = nullptr;
    screenKey_.clear();
    history_.clear();
    states_.clear();
    hovered_.clear();
    pumpX_ = pumpY_ = -1.0e9f;
    focused_.clear();
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
    if (picker_) {
        return picker_(x, y);
    }
    std::vector<const kotor::GuiControl*> controls;
    for (const kotor::GuiControl& control : screen_->controls) {
        flatten(control, controls);
    }
    // A button, checkbox, slider, scrollbar or list box answers the pointer even where a label (the
    // equip screen's LBL_INV_* icons share their button's rectangle and come after it in the file)
    // or a panel is drawn over it; with none under the pointer the topmost control of any kind does.
    std::string fallback;
    for (auto it = controls.rbegin(); it != controls.rend(); ++it) {
        const kotor::GuiControl& control = **it;
        if (!stateOf(control).visible) {
            continue;
        }
        if (contains(control.extent, x, y)) {
            if (control.type == 6 || control.type == 7 || control.type == 8 || control.type == 9
                || control.type == 11) {
                return control.key;
            }
            if (fallback.empty()) {
                fallback = control.key;
            }
        }
    }
    return fallback;
}

bool GuiSystem::pointerMove(float x, float y)
{
    pointerX_ = x;
    pointerY_ = y;
    const std::string left = hovered_;
    hovered_ = controlAt(x, y);
    if (left != hovered_ && !left.empty() && left == focused_) {
        // CSWGuiManager::UpdateMouseOverControl 0040b300: the control the pointer leaves is told so
        // (vtable+0xc with 0) and a button drops its lit state (SetActive 00418e00). Paired:
        // minigame_pazaak_wager (retail Wager unlit after the pointer crossed it), walk cg09 (Recommended).
        // A focus set by the panel (optionsmain's Gameplay) stays until the pointer has been on it.
        if (const kotor::GuiControl* was = find(left); was != nullptr && was->type == 6) {
            focused_.clear();
        }
    }
    if (const kotor::GuiControl* hit = find(hovered_)) {
        // Only controls the player can act on take the keyboard focus.
        const bool actionable = hit->type == 6 || hit->type == 7 || hit->type == 8
            || hit->type == 11 || (hit->type == 4 && hit->hilight.present);
        // A list under the pointer does not take the lit state from a button: retail opens optionsmain
        // with Gameplay lit while the click that opened it rests over LB_DESC (paired walk 0004_options).
        const kotor::GuiControl* holder = focused_.empty() ? nullptr : find(focused_);
        const bool keepButton = hit->type == 11 && holder != nullptr && holder->type == 6;
        if (actionable && stateOf(*hit).enabled && !keepButton) {
            focused_ = hit->key;
        }
    }
    return !hovered_.empty();
}

GuiRowSlot guiListRowSlot(const kotor::GuiControl& list, float rowHeight, std::size_t fit, std::size_t index)
{
    const int item = static_cast<int>(rowHeight);
    const int step = item + list.padding;
    const int rows = static_cast<int>(std::max<std::size_t>(fit, 1));
    const int inner = list.extent.height - 2 * list.border.dimension;
    // RETAIL-PROVEN (OrganizeControls 0041b140, 0041b445..0041b535): the spare is signed and the remainder loop
    // tests only "remainder != 0" before it counts down, so a NEGATIVE remainder never reaches zero and every
    // row gets the extra pixel (height and the next top). The skillinfo list is the case: inner 132, three
    // rows of 42 + PADDING 2, spare -2, share 0, so its rows sit 45 apart, not 44 (paired walk 0057: row
    // frames at y 331, 376, 421). A positive remainder gives the first rows one more pixel each.
    const int spare = inner - rows * step - list.padding;
    const int share = spare / rows;
    const int extra = spare - rows * share;
    const int i = static_cast<int>(index);
    const int bonus = extra > 0 ? (i < extra ? 1 : 0) : (extra < 0 ? 1 : 0);
    const int lead = extra > 0 ? std::min(i, extra) : (extra < 0 ? i : 0);
    GuiRowSlot slot;
    slot.top = static_cast<float>(i * (step + share) + lead);
    slot.height = static_cast<float>(item + share + bonus);
    return slot;
}

std::size_t GuiSystem::visibleRows(const std::string& listKey) const
{
    const kotor::GuiControl* list = find(listKey);
    if (list == nullptr || list->type != 11 || list->proto.empty()) {
        return 0;
    }
    const kotor::GuiControl& proto = list->proto.front();
    const float height = stateOf(*list).rowHeight > 0.0f ? stateOf(*list).rowHeight
                                                         : static_cast<float>(proto.extent.height);
    const int row = std::max(1, static_cast<int>(height) + list->padding);
    const int room = list->extent.top + list->extent.height - proto.extent.top + list->padding;
    return static_cast<std::size_t>(std::max(1, room / row));
}

int GuiSystem::listCellAt(const std::string& listKey, float x, float y) const
{
    const int row = listItemAt(listKey, x, y);
    const kotor::GuiControl* list = find(listKey);
    if (row < 0 || list == nullptr) {
        return -1;
    }
    const GuiControlState state = stateOf(*list);
    const std::vector<GuiListCell>& cells = state.items[static_cast<std::size_t>(row)].cells;
    const float local = x - static_cast<float>(list->proto.front().extent.left);
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const float left = static_cast<float>(c) * kFeatColumnStep;
        if (cells[c].value >= 0 && local >= left && local < left + kFeatRowHeight) {
            return static_cast<int>(c);
        }
    }
    return -1;
}

int GuiSystem::listCellValue(const std::string& keyOrTag) const
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr) {
        return -1;
    }
    const GuiControlState state = stateOf(*list);
    if (state.listSelection >= state.items.size() || state.listCell < 0) {
        return -1;
    }
    const std::vector<GuiListCell>& cells = state.items[state.listSelection].cells;
    return static_cast<std::size_t>(state.listCell) < cells.size() ? cells[static_cast<std::size_t>(state.listCell)].value : -1;
}

void GuiSystem::setListCell(const std::string& keyOrTag, std::size_t row, int cell)
{
    if (const kotor::GuiControl* list = find(keyOrTag)) {
        GuiControlState& state = stateFor(*list);
        state.listSelection = row;
        state.listCell = cell;
    }
}

int GuiSystem::listItemAt(const std::string& listKey, float x, float y) const
{
    const kotor::GuiControl* list = find(listKey);
    if (list == nullptr || list->type != 11 || list->proto.empty()) {
        return -1;
    }
    const kotor::GuiControl& proto = list->proto.front();
    if (x < static_cast<float>(proto.extent.left)
        || x >= static_cast<float>(proto.extent.left + proto.extent.width)) {
        return -1;
    }
    if (const auto rects = entryRects_.find(listKey); rects != entryRects_.end() && stateOf(*list).entryList) {
        for (const EntryRect& rect : rects->second) {
            if (y >= rect.top && y < rect.top + rect.height && rect.index < stateOf(*list).items.size()) {
                return static_cast<int>(rect.index);
            }
        }
        return -1;
    }
    const GuiControlState listState = stateOf(*list);
    const float height = listState.rowHeight > 0.0f ? listState.rowHeight : static_cast<float>(proto.extent.height);
    const float local = y - static_cast<float>(proto.extent.top);
    if (local < 0.0f) {
        return -1;
    }
    const std::size_t fit = visibleRows(listKey);
    int index = -1;
    for (std::size_t i = 0; i < fit; ++i) {
        const GuiRowSlot slot = guiListRowSlot(*list, height, fit, i);
        if (local >= slot.top && local < slot.top + slot.height + static_cast<float>(list->padding)) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        return -1;
    }
    const GuiControlState state = stateOf(*list);
    const std::size_t absolute = state.scroll + static_cast<std::size_t>(index);
    return absolute < state.items.size() ? static_cast<int>(absolute) : -1;
}

void GuiSystem::setEntryRects(const std::string& listKey, std::vector<EntryRect> rects) const
{
    entryRects_[listKey] = std::move(rects);
}

void GuiSystem::setTextView(const std::string& listKey, std::size_t lines, std::size_t visible) const
{
    if (lines > visible && visible > 0) {
        textViews_[listKey] = {lines, visible};
    } else {
        textViews_.erase(listKey);
    }
}

GuiSystem::TextView GuiSystem::textView(const std::string& listKey) const
{
    const auto found = textViews_.find(listKey);
    return found == textViews_.end() ? TextView{} : found->second;
}

bool GuiSystem::scrollTo(const std::string& keyOrTag, std::size_t row)
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr || list->type != 11) {
        return false;
    }
    GuiControlState& state = stateFor(*list);
    const TextView view = textView(list->key);
    const std::size_t visible = view.lines > 0 ? view.visible : visibleRows(list->key);
    const std::size_t count = view.lines > 0 ? view.lines : state.items.size();
    const std::size_t maxScroll = count > visible ? count - visible : 0;
    state.scroll = std::min(row, maxScroll);
    return true;
}

bool GuiSystem::setFocused(const std::string& keyOrTag)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    focused_ = control->key;
    return true;
}

bool GuiSystem::setEntryList(const std::string& keyOrTag, bool entries, bool pin)
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr || list->type != 11) {
        return false;
    }
    GuiControlState& state = stateFor(*list);
    state.entryList = entries;
    // A list already pinned to its end stays pinned across a refresh; only the user's scroll releases it.
    state.pinEnd = entries && (pin || (state.entryList && state.pinEnd));
    return true;
}

bool GuiSystem::setRowHeight(const std::string& keyOrTag, float height)
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr || list->type != 11) {
        return false;
    }
    stateFor(*list).rowHeight = std::max(0.0f, height);
    return true;
}

bool GuiSystem::setScrollbarHidden(const std::string& keyOrTag, bool hidden)
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr || list->type != 11) {
        return false;
    }
    stateFor(*list).hideScrollbar = hidden;
    return true;
}

bool GuiSystem::setOffset(const std::string& keyOrTag, float dx, float dy)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.offsetX = dx;
    state.offsetY = dy;
    return true;
}

bool GuiSystem::setPixelScroll(const std::string& keyOrTag, float pixels)
{
    const kotor::GuiControl* list = find(keyOrTag);
    if (list == nullptr || list->type != 11) {
        return false;
    }
    stateFor(*list).pixelScroll = pixels;
    return true;
}

bool GuiSystem::wheel(float x, float y, float steps)
{
    if (screen_ == nullptr) {
        return false;
    }
    std::vector<const kotor::GuiControl*> controls;
    for (const kotor::GuiControl& control : screen_->controls) {
        flatten(control, controls);
    }
    for (auto it = controls.rbegin(); it != controls.rend(); ++it) {
        const kotor::GuiControl& control = **it;
        if (control.type == 11 && stateOf(control).visible && contains(control.extent, x, y)) {
            if (stateOf(control).pinEnd) {
                const TextView pinned = textView(control.key);
                GuiControlState& pinnedState = stateFor(control);
                pinnedState.scroll = pinned.lines > pinned.visible ? pinned.lines - pinned.visible : 0;
                pinnedState.pinEnd = false;
            }
            const GuiControlState state = stateOf(control);
            const long next = static_cast<long>(state.scroll) - static_cast<long>(steps);
            scrollTo(control.key, static_cast<std::size_t>(std::max(0L, next)));
            return true;
        }
    }
    return false;
}

bool GuiSystem::navigate(int direction)
{
    if (screen_ == nullptr) {
        return false;
    }
    std::vector<const kotor::GuiControl*> controls;
    for (const kotor::GuiControl& control : screen_->controls) {
        flatten(control, controls);
    }
    const kotor::GuiControl* current = find(focused_);
    if (current == nullptr) {
        // First press: focus the first enabled button.
        for (const kotor::GuiControl* control : controls) {
            if (control->type == 6 && stateOf(*control).visible && stateOf(*control).enabled) {
                focused_ = control->key;
                hovered_ = focused_;
                return true;
            }
        }
        return false;
    }
    int target = -1;
    switch (direction) {
    case 0: target = current->moveUp; break;
    case 1: target = current->moveDown; break;
    case 2: target = current->moveLeft; break;
    default: target = current->moveRight; break;
    }
    if (target < 0) {
        // List boxes use up and down for their rows.
        if (current->type == 11 && (direction == 0 || direction == 1)) {
            GuiControlState& state = stateFor(*current);
            if (!state.items.empty()) {
                const std::size_t last = state.items.size() - 1;
                if (direction == 0 && state.listSelection > 0) {
                    --state.listSelection;
                } else if (direction == 1 && state.listSelection < last) {
                    ++state.listSelection;
                }
                const std::size_t rows = visibleRows(current->key);
                if (state.listSelection < state.scroll) {
                    state.scroll = state.listSelection;
                } else if (state.listSelection >= state.scroll + rows) {
                    state.scroll = state.listSelection + 1 - rows;
                }
                return true;
            }
        }
        return false;
    }
    for (const kotor::GuiControl* control : controls) {
        if (control->id == target && stateOf(*control).visible && stateOf(*control).enabled) {
            focused_ = control->key;
            hovered_ = focused_;
            return true;
        }
    }
    return false;
}

bool GuiSystem::activateFocused()
{
    const kotor::GuiControl* control = find(focused_);
    if (control == nullptr) {
        return false;
    }
    if (control->type == 11) {
        lastItem_ = static_cast<int>(stateOf(*control).listSelection);
    }
    return activate(control->key);
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
    if (control->type == 9) {
        // A list box scrollbar: the arrows step one row, the track a page.
        const std::string parentKey = control->key.substr(0, control->key.rfind('/'));
        if (const kotor::GuiControl* list = find(parentKey); list != nullptr && list->type == 11) {
            if (stateOf(*list).pinEnd) {
                // The list was shown at its end: the scroll the user moves from is that one.
                const TextView pinned = textView(list->key);
                GuiControlState& pinnedState = stateFor(*list);
                pinnedState.scroll = pinned.lines > pinned.visible ? pinned.lines - pinned.visible : 0;
                pinnedState.pinEnd = false;
            }
            const GuiControlState listState = stateOf(*list);
            const TextView view = textView(list->key);
            const std::size_t rows = view.lines > 0 ? view.visible : visibleRows(list->key);
            const float arrow = static_cast<float>(control->extent.width);
            if (y < static_cast<float>(control->extent.top) + arrow) {
                scrollTo(list->key, listState.scroll > 0 ? listState.scroll - 1 : 0);
            } else if (y >= static_cast<float>(control->extent.top + control->extent.height) - arrow) {
                scrollTo(list->key, listState.scroll + 1);
            } else {
                const float middle = static_cast<float>(control->extent.top)
                    + 0.5f * static_cast<float>(control->extent.height);
                scrollTo(list->key, y < middle ? (listState.scroll > rows ? listState.scroll - rows : 0)
                                               : listState.scroll + rows);
            }
        }
    }
    if (control->type == 8) {
        // A slider takes the value under the pointer.
        const float span = static_cast<float>(std::max(1, control->extent.width));
        const float ratio = std::clamp((x - static_cast<float>(control->extent.left)) / span, 0.0f, 1.0f);
        stateFor(*control).value = ratio * control->maxValue;
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
    if (const kotor::GuiControl* control = find(pressed)) {
        if (control->type == 11) {
            const int row = listItemAt(control->key, x, y);
            if (row >= 0 && !stateOf(*control).items[static_cast<std::size_t>(row)].cells.empty()) {
                // A feat row picks the icon under the point; between icons nothing changes.
                const int cell = listCellAt(control->key, x, y);
                if (cell >= 0) {
                    setListCell(control->key, static_cast<std::size_t>(row), cell);
                    lastItem_ = row;
                }
            } else if (row >= 0) {
                stateFor(*control).listSelection = static_cast<std::size_t>(row);
                lastItem_ = row;
            }
        }
        if (control->type == 9 || control->type == 8) {
            return true; // handled on press; no action tag
        }
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
        // BTN_EXIT leaves the program on the main menu; on the in-game options it resumes.
        if (lastAction_ == "BTN_EXIT" && screenKey_.rfind("mainmenu", 0) == 0) {
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

bool GuiSystem::setMarked(const std::string& keyOrTag, bool marked)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).marked = marked;
    return true;
}

bool GuiSystem::setHilightOverFill(const std::string& keyOrTag, bool on)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).hilightOverFill = on;
    return true;
}

bool GuiSystem::setText(const std::string& keyOrTag, const std::string& text)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.hasText = true;
    state.text = text;
    return true;
}

bool GuiSystem::setFill(const std::string& keyOrTag, const std::string& resref)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.hasFill = true;
    state.fill = resref;
    return true;
}

bool GuiSystem::setTextAlign(const std::string& keyOrTag, int align)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).textAlign = align;
    return true;
}

bool GuiSystem::setTextColor(const std::string& keyOrTag, kotor::Vec3 color)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.hasColor = true;
    state.color = color;
    return true;
}

bool GuiSystem::setTextPulse(const std::string& keyOrTag, bool pulse)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).textPulse = pulse;
    return true;
}

bool GuiSystem::setBorderPulse(const std::string& keyOrTag, bool pulse)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).borderPulse = pulse;
    return true;
}

bool GuiSystem::setAlpha(const std::string& keyOrTag, float alpha)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    stateFor(*control).alpha = alpha;
    return true;
}

bool GuiSystem::setFillColor(const std::string& keyOrTag, kotor::Vec3 color)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.hasFillColor = true;
    state.fillColor = color;
    return true;
}

bool GuiSystem::setItems(const std::string& keyOrTag, std::vector<GuiListItem> items)
{
    const kotor::GuiControl* control = find(keyOrTag);
    if (control == nullptr) {
        return false;
    }
    GuiControlState& state = stateFor(*control);
    state.items = std::move(items);
    textViews_.erase(control->key);
    entryRects_.erase(control->key);
    if (state.listSelection >= state.items.size()) {
        state.listSelection = 0;
    }
    state.scroll = 0;
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
    if (control == nullptr) {
        return GuiControlState{};
    }
    GuiControlState state = stateOf(*control);
    state.hovered = control->key == hovered_;
    return state;
}

void GuiSystem::clearActions()
{
    lastAction_.clear();
    actionLog_.clear();
}

} // namespace ke
