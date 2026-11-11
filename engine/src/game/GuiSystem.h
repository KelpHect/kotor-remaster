#pragma once

#include "kotor/Formats.h"
#include "kotor/Gui.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ke {

// One row of a list box (inventory item, save slot, journal entry...). `icon` is a
// texture resref drawn in the row; `tag` and `value` are the screen's own handle.
// One icon of a feat row (retail 006cccc0 holds three; 006cda80 fills them). `status` is what 006ccfd0 takes:
// 0 can be taken now, 1 owned, 2 granted, 3 out of reach, 4 picked on this screen.
// Retail 006cda80 gives a row 242x40: the icons squares of its height at its left, middle and right.
inline constexpr float kFeatRowHeight = 40.0f;
inline constexpr float kFeatRowWidth = 242.0f;
inline constexpr float kFeatColumnStep = (kFeatRowWidth - kFeatRowHeight) * 0.5f;

struct GuiListCell {
    std::string icon;
    int value = -1;
    int status = 3;
};

struct GuiListItem {
    std::string text;
    std::string icon;
    std::string tag;
    int value = 0;
    bool hasColor = false;
    kotor::Vec3 color{1.0f, 1.0f, 1.0f};
    // The colour while the pointer is on the row (a conversation reply turns yellow, 006a6fc0 -> 007a23c0).
    bool hasHoverColor = false;
    kotor::Vec3 hoverColor{1.0f, 1.0f, 1.0f};
    bool selectable = true;
    // A checkbox row (a type 7 PROTOITEM, optfeedback's LB_OPTIONS): -1 none, else 0 off / 1 on. The
    // list's own selection is then the row the cursor is on, not the check.
    int check = -1;
    // A feat row: the base feat and the two that build on it, not a line of text.
    std::vector<GuiListCell> cells;
    // An item row as retail's CSWGuiInGameItemEntry draws it (Initialize 006b4db0, SetItem 006b6710):
    // the icon inside a hex frame at the row's left, the stack size on the frame from 2 up, and the
    // name in its own box beside it. count is the stack size.
    bool itemEntry = false;
    int count = 0;
};

// The item entry's height: CSWGuiInGameItemEntry::Initialize 006b4db0 sets its extent height 0x38.
// The list spreads its spare height over the rows (guiListRowSlot): 57/58 px apart in the inventory
// and equip lists, 59/60 in the store's (retail frames ui_inventory, walk 0032, store).
inline constexpr float kItemEntryHeight = 56.0f;


// RETAIL-PROVEN (CSWGuiListBox::OrganizeControls 0041b140): rows of one height are spread over the
// list's inner height (its extent less DIMENSION on each side). With step = row height + PADDING and
// fit the rows that show, the spare (inner - fit * step - PADDING) is split evenly; each row grows by
// the share and the first (spare mod fit) rows by one more pixel, the next row starting below it.
// Paired frames: optfeedback rows 49, 49, 48 ... apart (290 / 43 rows), store rows 60 apart.
struct GuiRowSlot {
    float top = 0.0f;    // from the first row's top
    float height = 0.0f;
};
GuiRowSlot guiListRowSlot(const kotor::GuiControl& list, float rowHeight, std::size_t fit, std::size_t index);

struct GuiControlState {
    bool visible = true;
    bool enabled = true;
    bool hovered = false;
    bool pressed = false;
    bool selected = false;
    // Painted with its hilight art as when the pointer is over it (a card picked on the Pazaak setup).
    bool marked = false;
    // With a screen-supplied fill: the hilight art is drawn over that fill (hovered or marked) instead of
    // replacing it, so a card keeps its face under the selection brackets.
    bool hilightOverFill = false;
    float value = 0.0f;
    std::size_t listSelection = 0;
    int listCell = -1; // on a row with cells: which of them
    // Screen-supplied content overriding what the .gui carries.
    bool hasText = false;
    std::string text;
    bool hasFill = false;
    std::string fill;
    bool hasColor = false;
    kotor::Vec3 color{1.0f, 1.0f, 1.0f};
    // The text alpha follows the shared GUI pulse (text flag 0x40: CSWGuiButton::SetActive 00418e00, the focused skill
    // row of 006f4bf0; drawn through 00416240 with the value UpdateGuiPulse 00414a50 keeps at 0078d3d4).
    bool textPulse = false;
    // The border drawn for the control breathes with the same pulse (the lit step ring of MAINCG, walk 0041..0081).
    bool borderPulse = false;
    // Over the border COLOR that tints the fill (a Pazaak card greyed out, RefreshDisplay 0067d4b0).
    bool hasFillColor = false;
    kotor::Vec3 fillColor{1.0f, 1.0f, 1.0f};
    int textAlign = 0; // over the file's when not 0
    std::vector<GuiListItem> items;
    std::size_t scroll = 0;
    // Smooth scroll of a one-row list (the credits), in design pixels; may be negative.
    float pixelScroll = 0.0f;
    // Row height of a list whose rows wrap onto several lines (the conversation replies), in
    // design pixels; 0 keeps the screen's PROTOITEM height.
    float rowHeight = 0.0f;
    // The list box's scrollbar flag 0x20 cleared: CSWGuiDialog::SetReplies (006a86a0) sets it only when
    // the rows overflow, so the reply list draws no arrows while every reply fits.
    bool hideScrollbar = false;
    // A list of text entries as the Messages screen builds it (AddDialogMessages 00626b10): one
    // control per entry, as tall as its wrapped text plus twice the HILIGHT border's dimension, stacked
    // with no padding; the last one is selected and the list opens scrolled to it.
    bool entryList = false;
    bool pinEnd = false; // shown scrolled to the end until the user moves the scrollbar
    // Where the screen moved the control from its file position, in design pixels (the HUD's
    // target block floats; retail sets the extent at run time).
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    // A run-time alpha over the file's ALPHA (retail labels carry one at +0x110; the HUD's combat
    // line fades through it, 00686ba0).
    float alpha = 1.0f;
};

// Engine-side GUI interaction: hit-testing, per-control state, actions and a
// screen stack. The shipped .gui files carry no script fields, so actions are
// tags consumed by GameSession/Application (main menu, HUD icons, exit).
class GuiSystem {
public:
    explicit GuiSystem(kotor::Install& install);

    bool open(const std::string& screen);
    // A private copy of a shipped screen under another name, so a screen class can change its
    // geometry (a message box that grows with its text) without touching the shipped layout other
    // screens open. Returns the copy, or nullptr when `base` does not exist.
    kotor::GuiScreen* variant(const std::string& base, const std::string& alias);
    bool push(const std::string& screen);
    bool pop();
    // A popup (confirm, tooltip) over the current screen: the screen below stays painted
    // and inert until popModal(). close() and open() drop every layer.
    bool pushModal(const std::string& screen);
    bool popModal();
    bool modalActive() const { return !under_.empty(); }
    std::size_t underlayCount() const { return under_.size(); }
    const kotor::GuiScreen* underlayScreen(std::size_t index) const { return under_[index].screen; }
    const std::map<std::string, GuiControlState>& underlayStates(std::size_t index) const
    {
        return under_[index].states;
    }
    void close();
    bool active() const { return screen_ != nullptr; }
    const kotor::GuiScreen* screen() const { return screen_; }
    // The open screen for a caller that sizes controls at run time (pause.gui, 006c00c0).
    kotor::GuiScreen* editableScreen() { return screen_; }
    const std::string& screenName() const;
    std::size_t historyDepth() const { return history_.size(); }

    std::string controlAt(float x, float y) const;
    // A screen whose controls the painter moves (the edge-anchored HUD) answers pointer hits
    // itself: the pointer coordinates are then window pixels and `picker` returns the key.
    void setPicker(std::function<std::string(float, float)> picker) { picker_ = std::move(picker); }
    bool pointerMove(float x, float y);
    bool pointerDown(float x, float y);
    bool pointerUp(float x, float y);
    const std::string& hovered() const { return hovered_; }
    // The control the keyboard acts on (set by pointer hover and by navigate()).
    const std::string& focused() const { return focused_; }
    // Gives a control the focus as a panel constructor does (its vtable+8 call): drawn with its hilight
    // until the pointer moves the focus elsewhere. False when the tag is not on the screen.
    bool setFocused(const std::string& keyOrTag);
    float pointerX() const { return pointerX_; }
    float pointerY() const { return pointerY_; }
    // True when the window pointer is not where this GuiSystem last saw it. Kept per
    // GuiSystem (the HUD menu bar and a panel can both pump input in one frame) and forgotten
    // whenever a screen opens or closes, so a panel that appears under a still pointer hovers.
    bool pointerMovedSince(float windowX, float windowY)
    {
        const bool moved = windowX != pumpX_ || windowY != pumpY_;
        pumpX_ = windowX;
        pumpY_ = windowY;
        return moved;
    }
    // Arrow key step along the controls' MOVETO links (0 up, 1 down, 2 left, 3 right).
    bool navigate(int direction);
    bool activateFocused();
    // Mouse wheel over a list box scrolls it. `x`,`y` in design space.
    bool wheel(float x, float y, float steps);
    // Row of `listKey` under a design-space point, -1 when none.
    int listItemAt(const std::string& listKey, float x, float y) const;
    std::size_t visibleRows(const std::string& listKey) const;
    // A list of non-selectable text taller than its box scrolls by line, as retail's text list
    // boxes do (their scrollbar steps one line). The painter measures the wrapped lines each frame
    // (it owns the fonts) and reports them here; `state.scroll` is then a line index.
    struct TextView {
        std::size_t lines = 0;
        std::size_t visible = 0;
    };
    // An entry list's shown entries (index and design-space top/height), reported by the painter so a click
    // picks the entry under the pointer (entries are as tall as their wrapped text, not a uniform row).
    struct EntryRect {
        std::size_t index = 0;
        float top = 0.0f;
        float height = 0.0f;
    };
    void setEntryRects(const std::string& listKey, std::vector<EntryRect> rects) const;
    void setTextView(const std::string& listKey, std::size_t lines, std::size_t visible) const;
    TextView textView(const std::string& listKey) const;
    // Last list row picked by a click (for the screen that handles the action).
    int lastItem() const { return lastItem_; }
    bool activate(const std::string& keyOrTag);

    bool setVisible(const std::string& keyOrTag, bool visible);
    bool setEnabled(const std::string& keyOrTag, bool enabled);
    bool setSelected(const std::string& keyOrTag, bool selected);
    bool setMarked(const std::string& keyOrTag, bool marked);
    bool setHilightOverFill(const std::string& keyOrTag, bool on);
    bool setValue(const std::string& keyOrTag, float value);
    float value(const std::string& keyOrTag) const;
    bool setListSelection(const std::string& keyOrTag, std::size_t index);
    std::size_t listSelection(const std::string& keyOrTag) const;
    // A row with cells: the cell under the point, -1 for none, and the picked cell's value, -1 for none.
    int listCellAt(const std::string& listKey, float x, float y) const;
    int listCellValue(const std::string& keyOrTag) const;
    void setListCell(const std::string& keyOrTag, std::size_t row, int cell);
    GuiControlState state(const std::string& keyOrTag) const;
    bool setText(const std::string& keyOrTag, const std::string& text);
    bool setFill(const std::string& keyOrTag, const std::string& resref);
    bool setTextColor(const std::string& keyOrTag, kotor::Vec3 color);
    bool setTextPulse(const std::string& keyOrTag, bool pulse);
    bool setBorderPulse(const std::string& keyOrTag, bool pulse);
    bool setFillColor(const std::string& keyOrTag, kotor::Vec3 color);
    bool setTextAlign(const std::string& keyOrTag, int align); // 0 keeps the file's
    bool setItems(const std::string& keyOrTag, std::vector<GuiListItem> items);
    bool scrollTo(const std::string& keyOrTag, std::size_t row);
    bool setPixelScroll(const std::string& keyOrTag, float pixels);
    bool setRowHeight(const std::string& keyOrTag, float height);
    // Marks the list as entry boxes (GuiControlState::entryList) and, when `pin`, scrolled to its end.
    bool setEntryList(const std::string& keyOrTag, bool entries, bool pin);
    bool setScrollbarHidden(const std::string& keyOrTag, bool hidden);
    bool setOffset(const std::string& keyOrTag, float dx, float dy);
    bool setAlpha(const std::string& keyOrTag, float alpha);
    const kotor::GuiControl* control(const std::string& keyOrTag) const { return find(keyOrTag); }

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

    struct Layer {
        kotor::GuiScreen* screen = nullptr;
        std::string key;
        std::map<std::string, GuiControlState> states;
        std::string hovered;
        std::string focused;
    };
    std::vector<Layer> under_;

    kotor::Install& install_;
    std::map<std::string, kotor::GuiScreen> screens_;
    kotor::GuiScreen* screen_ = nullptr;
    std::string screenKey_;
    std::vector<std::string> history_;
    std::map<std::string, GuiControlState> states_;
    mutable std::map<std::string, TextView> textViews_;
    mutable std::map<std::string, std::vector<EntryRect>> entryRects_;
    std::string hovered_;
    std::string focused_;
    int lastItem_ = -1;
    float pointerX_ = -1.0f;
    float pointerY_ = -1.0f;
    float pumpX_ = -1.0e9f;
    float pumpY_ = -1.0e9f;
    std::string pressed_;
    std::string lastAction_;
    std::vector<std::string> actionLog_;
    bool quitRequested_ = false;
    std::function<std::string(float, float)> picker_;
};

} // namespace ke
