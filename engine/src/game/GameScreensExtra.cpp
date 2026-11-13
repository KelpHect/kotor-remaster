#include "game/GameScreens.h"

#include "game/ScriptHost.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>

// Screens that are not tied to a panel the keyboard opens: message boxes (the tutorial popups and
// the game over message), the upgrade bench and the Pazaak screens. Same pattern as GameScreens.cpp:
// the session keeps the model, the screen binds the shipped .gui to it.

namespace ke {

namespace {

std::string lowerOf(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<kotor::TwoDa> loadTwoDa(kotor::Install& install, const char* name)
{
    auto data = install.find(name, kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        return kotor::TwoDa::parse(std::move(*data), name);
    } catch (const kotor::FormatError&) {
        return std::nullopt;
    }
}

} // namespace

// ---- message boxes -------------------------------------------------------------------------

bool GameScreens::messageBoxWanted(const GameSession& game)
{
    return game.gameOverMessage() || game.pendingTutorialWindow() >= 0;
}

// The shipped confirm.gui is one line tall (LB_MESSAGE is 278x20 in a 290x87 panel); the retail
// message box that carries the tutorial (006aa670: "Message0".."Message2" pages, button strref
// 1580 "OK" or 38623 "Continue" when another page follows) has to hold paragraphs, so this copy of
// the panel grows: the panel, the list and the button move down together.
bool GameScreens::prepareMessageBox()
{
    kotor::GuiScreen* box = gui_.variant("confirm", "confirm_box");
    if (box == nullptr) {
        return false;
    }
    constexpr int kWidth = 520;
    constexpr int kHeight = 330;
    box->extent.left = (640 - kWidth) / 2;
    box->extent.top = (480 - kHeight) / 2;
    box->extent.width = kWidth;
    box->extent.height = kHeight;
    for (kotor::GuiControl& control : box->controls) {
        if (control.tag == "LB_MESSAGE") {
            // The prototype and the scrollbar keep their places relative to the list (the file's
            // prototype sits 2 in from the list's corner and ends 20 short of its right edge, where
            // the 16 wide bar starts 6 from the end).
            const kotor::GuiExtent old = control.extent;
            control.extent = {12, 12, kWidth - 24, kHeight - 70};
            for (kotor::GuiControl& item : control.proto) {
                item.extent.left = control.extent.left + (item.extent.left - old.left);
                item.extent.top = control.extent.top + (item.extent.top - old.top);
                item.extent.width = control.extent.width - (old.width - item.extent.width);
            }
            for (kotor::GuiControl& child : control.children) {
                if (child.type == 9) {
                    child.extent.left = control.extent.left + control.extent.width - (old.left + old.width - child.extent.left);
                    child.extent.top = control.extent.top;
                    child.extent.height = control.extent.height;
                }
            }
        } else if (control.tag == "BTN_OK") {
            control.extent = {(kWidth - 120) / 2, kHeight - 50, 120, 28};
        } else if (control.tag == "BTN_CANCEL") {
            control.extent = {(kWidth - 120) / 2, kHeight - 50, 120, 28};
        }
    }
    return true;
}

void GameScreens::bindMessageBox()
{
    if (!prepareMessageBox() || !gui_.open("confirm_box")) {
        game_.dismissTutorialWindow();
        return;
    }
    bind("confirm_box");
    tutorialRow_ = game_.pendingTutorialWindow();
    tutorialPage_ = 0;
}

// Retail opens the tutorial from the panel's own constructor (inventory 006b4c30, map 00693650, messages
// 00626d90), so its message box is added over the panel; the panel stays drawn behind it. Returns true
// while the box holds the input.
bool GameScreens::updateTutorialModal()
{
    if (tutorialModal_) {
        if (!gui_.modalActive() || gui_.screenName() != "confirm_box") {
            tutorialModal_ = false; // closed from outside
            tutorialRow_ = -1;
            tutorialPage_ = 0;
            key_.clear();
            return false;
        }
        // The box holds the page keys too: the panel below stays the one shown.
        const UiPanel below = bound_ == "inventory" ? UiPanel::Inventory : bound_ == "map" ? UiPanel::Map : UiPanel::Messages;
        if (game_.activePanel() != below) {
            game_.setUiPanel(below);
        }
        const std::string now = std::to_string(tutorialRow_) + "," + std::to_string(tutorialPage_);
        if (now != tutorialKey_) {
            tutorialKey_ = now;
            refreshMessageBox();
        }
        return true;
    }
    const bool panel = bound_ == "inventory" || bound_ == "map" || bound_ == "messages";
    if (!panel || game_.pendingTutorialWindow() < 0 || game_.gameOverMessage() || !gui_.active()
        || gui_.screenName() != bound_) {
        return false;
    }
    if (!prepareMessageBox() || !gui_.pushModal("confirm_box")) {
        game_.dismissTutorialWindow();
        return false;
    }
    tutorialModal_ = true;
    tutorialRow_ = game_.pendingTutorialWindow();
    tutorialPage_ = 0;
    tutorialKey_.clear();
    return true;
}

void GameScreens::refreshMessageBox()
{
    std::string text;
    std::string ok = tlk(1580);
    const bool gameOver = game_.gameOverMessage();
    if (gameOver) {
        // Shipped text the retail popup shows when the whole party is dead (strref 42351).
        text = tlk(42351);
    } else if (tutorialRow_ >= 0) {
        // tutorial.2da: message0..message2 are the pages (006aa670), the last one ends the popup.
        if (auto table = loadTwoDa(game_.install(), "tutorial"); table.has_value()) {
            const std::string row = std::to_string(tutorialRow_);
            std::vector<std::string> pages;
            for (const char* column : {"message0", "message1", "message2"}) {
                const std::string cell = table->cell(row, column);
                if (!cell.empty() && cell != "****") {
                    // CTlkTable::ParseTokens 0041dd30: <MGActionLeft> and the like are the bound keys.
                    pages.push_back(game_.substituteTokens(tlk(static_cast<std::uint32_t>(std::atoi(cell.c_str())))));
                }
            }
            if (pages.empty()) {
                pages.push_back({});
            }
            tutorialPage_ = std::clamp(tutorialPage_, 0, static_cast<int>(pages.size()) - 1);
            text = pages[static_cast<std::size_t>(tutorialPage_)];
            if (static_cast<std::size_t>(tutorialPage_) + 1 < pages.size()) {
                ok = tlk(38623); // "Continue"
            }
        }
    }
    GuiListItem item;
    item.text = text;
    item.selectable = false;
    gui_.setItems("LB_MESSAGE", {item});
    gui_.setText("BTN_OK", ok);
    gui_.setVisible("BTN_CANCEL", false);
}

// Retail 00625c60: the rows that have something, closed up from y 10 at 37 a row; the panel fits them and OK sits under
// them. Rows 0, 4, 5, 7, 8 keep the file's text; the others say how much. Width stays the file's (retail fits it to the
// longest line).
void GameScreens::bindStatusSummary()
{
    const StatusSummary s = game_.statusSummaryShown();
    kotor::GuiScreen* box = gui_.variant("statussummary", "statussummary_fit");
    if (box == nullptr) {
        game_.dismissStatusSummary();
        return;
    }
    const auto counted = [this](std::uint32_t strref, std::int32_t amount) {
        std::string text = tlk(strref);
        const std::size_t at = text.find("<CUSTOM0>");
        if (at != std::string::npos) {
            text.replace(at, 9, std::to_string(amount < 0 ? -amount : amount));
        }
        return text;
    };
    std::string credits = s.credits < 0 ? counted(42627, s.credits) : counted(42437, s.credits);
    if (s.creditsBothWays) {
        credits = tlk(47933) + credits;
    }
    const bool mixedShift = s.light != 0 && s.dark != 0 && s.light != s.dark;
    struct Row {
        const char* icon;
        const char* text;
        bool shown;
        std::string line;
    };
    const Row rows[] = {
        {"LBL_JOURNAL", "LBL_JOURNAL_DESC", s.journal, {}},
        {"LBL_CREDITS", "LBL_CREDITS_DESC", s.credits != 0 || s.creditsBothWays, credits},
        {"LBL_XP", "LBL_XP_DESC", s.xp != 0, counted(42438, s.xp)},
        {"LBL_STEALTH", "LBL_STEALTH_DESC", s.stealthXp != 0, counted(42439, s.stealthXp)},
        {"LBL_DARKSIDE", "LBL_DARKSIDE_DESC", s.dark != 0, {}},
        {"LBL_LIGHTSIDE", "LBL_LIGHTSIDE_DESC", s.light != 0, {}},
        {"LBL_NETSHIFT", "LBL_NETSHIFT_DESC", mixedShift, tlk(s.light > s.dark ? 47935 : 47934)},
        {"LBL_RECEIVED", "LBL_RECEIVED_DESC", s.received, {}},
        {"LBL_LOST", "LBL_LOST_DESC", s.lost, {}},
    };
    // AddPanel (0040bc70) logs the panel with the file's extent: OnPanelAdded (00625c60) fits it afterwards, so the
    // trace row of an open shows the shipped one (retail row: 159 74 322 332, modal push).
    if (const kotor::GuiScreen* shipped = gui_.variant("statussummary", "statussummary"); shipped != nullptr) {
        box->openExtent = shipped->extent;
        box->hasOpenExtent = true;
        // A fresh panel each time: the previous fit does not carry over.
        box->extent = shipped->extent;
        for (kotor::GuiControl& control : box->controls) {
            for (const kotor::GuiControl& original : shipped->controls) {
                if (original.tag == control.tag) {
                    control.extent = original.extent;
                    break;
                }
            }
        }
    }
    // 00625c60 (006260d7..00626222): each shown row's text control gets the running width W (starts at 150, 0x96);
    // when its text is not empty and W is under 440 (0x1b8), W grows by 20 until the text wraps to one line
    // (the wrapped height equals one font height) and the control takes the new width. W is not reset by the next
    // row. Paired with the retail rows of tar_m02ac: "Journal Entry Added" fits at 210 (control 8 is 210 wide).
    int fitWidth = 150;
    int textLeft = 52;
    int y = 10;
    for (const Row& row : rows) {
        for (kotor::GuiControl& control : box->controls) {
            if (row.shown && control.tag == row.icon) {
                control.extent.top = y;
            } else if (row.shown && control.tag == row.text) {
                control.extent.top = y - 1;
                std::string text = row.line;
                if (text.empty() && control.strref != 0xFFFFFFFFu) {
                    text = tlk(control.strref);
                }
                control.extent.width = fitWidth;
                if (!text.empty() && fitWidth < 440) {
                    const float oneLine = std::max(1.0f, painter_.measureText(control.font, "Ay", 1000.0f));
                    do {
                        const int lines = std::max(1, static_cast<int>(std::lround(
                            painter_.measureText(control.font, text, static_cast<float>(fitWidth)) / oneLine)));
                        if (lines <= 1) {
                            break;
                        }
                        fitWidth += 20;
                        control.extent.width = fitWidth;
                    } while (fitWidth < 440);
                }
            }
        }
        if (row.shown) {
            y += 37;
        }
    }
    for (const kotor::GuiControl& control : box->controls) {
        if (control.tag == rows[0].text) {
            textLeft = control.extent.left; // param_1[0x391]: the first text control's left
        }
    }
    const int width = textLeft + 10 + fitWidth;
    const int height = y + 25;
    box->extent.width = width;
    box->extent.left = (640 - width) / 2;
    box->extent.height = height;
    box->extent.top = (480 - height) / 2;
    for (kotor::GuiControl& control : box->controls) {
        if (control.tag == "BTN_OK") {
            control.extent.top = y - 7;
            control.extent.left = (width - control.extent.width) / 2;
        }
    }
    if (!gui_.open("statussummary_fit")) {
        game_.dismissStatusSummary();
        return;
    }
    bind("statussummary_fit");
    for (const Row& row : rows) {
        gui_.setVisible(row.icon, row.shown);
        gui_.setVisible(row.text, row.shown);
        if (row.shown && !row.line.empty()) {
            gui_.setText(row.text, row.line);
        }
    }
    if (mixedShift) {
        gui_.setFill("LBL_NETSHIFT", s.light > s.dark ? "lbl_ilside" : "lbl_idside");
    }
}

bool GameScreens::actionStatusSummary(const std::string& tag)
{
    if (tag != "BTN_OK") {
        return false;
    }
    game_.dismissStatusSummary();
    gui_.close();
    bound_.clear();
    key_.clear();
    return true;
}

bool GameScreens::actionMessageBox(const std::string& tag)
{
    if (tag != "BTN_OK") {
        return tag == "LB_MESSAGE" || tag == "BTN_CANCEL";
    }
    if (game_.gameOverMessage()) {
        game_.acknowledgeGameOver();
        gui_.close();
        bound_.clear();
        key_.clear();
        return true;
    }
    if (tutorialRow_ >= 0) {
        if (auto table = loadTwoDa(game_.install(), "tutorial"); table.has_value()) {
            const std::string row = std::to_string(tutorialRow_);
            int pages = 0;
            for (const char* column : {"message0", "message1", "message2"}) {
                const std::string cell = table->cell(row, column);
                pages += !cell.empty() && cell != "****" ? 1 : 0;
            }
            if (tutorialPage_ + 1 < pages) {
                ++tutorialPage_;
                return true;
            }
        }
    }
    game_.dismissTutorialWindow();
    tutorialRow_ = -1;
    tutorialPage_ = 0;
    if (tutorialModal_) {
        tutorialModal_ = false; // back to the panel below
        gui_.popModal();
        key_.clear();
        return true;
    }
    gui_.close();
    bound_.clear();
    key_.clear();
    return true;
}


// ---- the upgrade bench ---------------------------------------------------------------------
//
// ShowUpgradeScreen (retail 00543990 -> 0062e760, constructor 006c78d0) opens three shipped screens:
// upgradesel (the four item kinds), upgradeitems (the items of that kind in the pack) and upgrade (the
// slots of one item and the upgrade items that fit). Which control does what is read from the .gui
// files; how retail moves between them is not known (ENGINE CHOICE: Upgrade Items, Upgrade Item and
// Assemble go forward or apply, Back and Escape go back one screen, the first closes).

std::string GameScreens::upgradeScreenName() const
{
    if (upgradeStage_ == 0) {
        return "upgradesel";
    }
    if (upgradeStage_ == 1) {
        return "upgradeitems";
    }
    static const char* const names[4] = {"upgrade_saber", "upgrade_ranged", "upgrade_melee", "upgrade_armor"};
    return names[std::clamp(upgradeCategory_, 0, 3)];
}

const InventoryItem* GameScreens::upgradeTargetItem(std::size_t* index) const
{
    const std::vector<InventoryItem>& pack = game_.inventory();
    for (std::size_t i = 0; i < pack.size(); ++i) {
        if (pack[i].object == upgradeObject_ && lowerOf(pack[i].resref) == upgradeResref_) {
            if (index != nullptr) {
                *index = i;
            }
            return &pack[i];
        }
    }
    return nullptr;
}

InventoryItem GameScreens::upgradeRowItem(int row)
{
    InventoryItem item;
    const std::vector<GameSession::UpgradeRow>& rows = game_.upgradeRows();
    if (row >= 0 && static_cast<std::size_t>(row) < rows.size()) {
        item.resref = rows[static_cast<std::size_t>(row)].templ;
        if (const kotor::ItemTemplate* templ = game_.upgradeTemplate(item.resref)) {
            item.baseItem = templ->baseItem;
        }
    }
    return item;
}

std::string GameScreens::upgradedDescription(const InventoryItem& item)
{
    std::string text = describe(item);
    std::string names;
    const std::size_t count = std::min<std::size_t>(game_.upgradeRows().size(), 32);
    for (std::size_t row = 0; row < count; ++row) {
        if ((item.upgrades >> row & 1u) != 0) {
            names += "\n" + itemInfo(upgradeRowItem(static_cast<int>(row))).name;
        }
    }
    if (!names.empty()) {
        text += "\n\n" + tlk(42026) + names; // "Upgrades:"
    }
    return text;
}

void GameScreens::updateUpgrade()
{
    if (!game_.upgradeScreenOpen()) {
        upgradeActive_ = false;
        return;
    }
    if (!upgradeActive_) {
        upgradeActive_ = true;
        upgradeStage_ = 0;
        upgradeCategory_ = -1;
        upgradePending_.clear();
        upgradeSlot_ = 0;
        upgradeObject_ = 0;
        upgradeResref_.clear();
        // ShowUpgradeScreen(oItem): with an item the bench opens on that item's upgrade screen (a
        // guess; without one it opens on the item kinds).
        const std::uint32_t wanted = game_.upgradeTarget();
        if (wanted != script::kObjectInvalid) {
            for (const InventoryItem& item : game_.inventory()) {
                const int category = game_.upgradeCategory(item);
                if (item.object == wanted && category >= 0) {
                    upgradeObject_ = item.object;
                    upgradeResref_ = lowerOf(item.resref);
                    upgradeCategory_ = category;
                    upgradeStage_ = 2;
                    upgradeSlot_ = game_.upgradeSlots(item).front().type;
                    break;
                }
            }
        }
    }
    if (upgradeStage_ == 2 && upgradeTargetItem() == nullptr) {
        upgradeStage_ = 1;
    }
}

void GameScreens::bindUpgradeScreen(const std::string& screen)
{
    // The list sits inside the property frame (non-saber) or under the saber's slot name; the shipped
    // list spans the whole left column behind the slot buttons, so this copy of the screen shrinks it.
    kotor::GuiScreen* copy = gui_.variant("upgrade", screen);
    if (copy == nullptr) {
        game_.closeUpgradeScreen();
        upgradeActive_ = false;
        return;
    }
    // LB_DESC has no row prototype in the file; it takes the lightsaber description's.
    const kotor::GuiControl* descriptionProto = nullptr;
    for (const kotor::GuiControl& control : copy->controls) {
        if (control.tag == "LB_DESC_LS" && !control.proto.empty()) {
            descriptionProto = &control;
        }
    }
    for (kotor::GuiControl& control : copy->controls) {
        if (control.tag == "LB_DESC" && control.proto.empty() && descriptionProto != nullptr) {
            control.proto = descriptionProto->proto;
            for (kotor::GuiControl& proto : control.proto) {
                proto.extent.left = control.extent.left + 4;
                proto.extent.top = control.extent.top + 2;
                proto.extent.width = control.extent.width - 18;
            }
        }
        if (control.tag == "LB_ITEMS") {
            const kotor::GuiExtent now = control.extent;
            const kotor::GuiExtent wanted = screen == "upgrade_saber" ? kotor::GuiExtent{60, 313, 265, 91}
                                                                      : kotor::GuiExtent{60, 122, 265, 187};
            // The rows and the scroll bar are placed by their own extents, in screen coordinates: they
            // move with the list.
            const int dx = wanted.left - now.left;
            const int dy = wanted.top - now.top;
            const int dh = wanted.height - now.height;
            control.extent = wanted;
            for (kotor::GuiControl& proto : control.proto) {
                proto.extent.left += dx;
                proto.extent.top += dy;
            }
            for (kotor::GuiControl& child : control.children) {
                child.extent.left += dx;
                child.extent.top += dy;
                child.extent.height += dh;
            }
        }
    }
    if (!gui_.open(screen)) {
        game_.closeUpgradeScreen();
        upgradeActive_ = false;
        return;
    }
    bind(screen);
}

std::string GameScreens::upgradeStateKey() const
{
    std::string key = std::to_string(upgradeStage_) + "," + std::to_string(upgradeCategory_) + ","
        + std::to_string(upgradeSlot_) + "," + std::to_string(upgradeObject_);
    for (const auto& [type, row] : upgradePending_) {
        key += "," + std::to_string(type) + ":" + std::to_string(row);
    }
    if (const InventoryItem* target = upgradeTargetItem()) {
        key += ",b" + std::to_string(target->upgrades);
    }
    int stacks = 0;
    for (const InventoryItem& item : game_.inventory()) {
        stacks += item.stack;
    }
    return key + ",s" + std::to_string(stacks);
}

void GameScreens::upgradeBack()
{
    if (upgradeStage_ > 0) {
        --upgradeStage_;
        upgradePending_.clear();
    } else {
        closeScreen();
    }
}

void GameScreens::refreshUpgrade()
{
    const std::vector<InventoryItem>& pack = game_.inventory();
    if (bound_ == "upgradesel") {
        int count[4] = {0, 0, 0, 0};
        for (const InventoryItem& item : pack) {
            const int category = game_.upgradeCategory(item);
            if (category >= 0) {
                ++count[category];
            }
        }
        static const char* const buttons[4] = {"BTN_LIGHTSABER", "BTN_RANGED", "BTN_MELEE", "BTN_ARMOR"};
        for (int category = 0; category < 4; ++category) {
            gui_.setEnabled(buttons[category], count[category] > 0);
            gui_.setSelected(buttons[category], category == upgradeCategory_);
        }
        gui_.setEnabled("BTN_UPGRADEITEMS", upgradeCategory_ >= 0 && count[upgradeCategory_] > 0);
        return;
    }
    if (bound_ == "upgradeitems") {
        rows_.clear();
        std::vector<GuiListItem> list;
        for (std::size_t i = 0; i < pack.size(); ++i) {
            if (game_.upgradeCategory(pack[i]) != upgradeCategory_) {
                continue;
            }
            const ItemInfo& info = itemInfo(pack[i]);
            GuiListItem row;
            row.text = info.name;
            row.icon = info.icon;
            row.value = static_cast<int>(i);
            list.push_back(std::move(row));
            rows_.push_back(i);
        }
        const bool empty = list.empty();
        const std::size_t selection = std::min(gui_.listSelection("LB_ITEMS"), empty ? std::size_t{0} : list.size() - 1);
        gui_.setItems("LB_ITEMS", std::move(list));
        gui_.setListSelection("LB_ITEMS", selection);
        shownRow_ = selection;
        GuiListItem description;
        description.selectable = false;
        if (!empty) {
            description.text = upgradedDescription(pack[rows_[selection]]);
        }
        gui_.setItems("LB_DESCRIPTION", {description});
        gui_.setEnabled("BTN_UPGRADEITEM", !empty);
        return;
    }

    // upgrade: the slots of the chosen item.
    const InventoryItem* target = upgradeTargetItem();
    if (target == nullptr) {
        return;
    }
    const bool saber = bound_ == "upgrade_saber";
    const std::vector<GameSession::UpgradeSlot> slots = game_.upgradeSlots(*target);
    if (slots.empty()) {
        return;
    }
    if (std::none_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.type == upgradeSlot_; })) {
        upgradeSlot_ = slots.front().type;
    }
    const auto effective = [&](int type) {
        const auto chosen = upgradePending_.find(type);
        return chosen != upgradePending_.end() ? chosen->second : game_.upgradeInstalled(*target, type);
    };
    const GameSession::UpgradeSlot* slot = &slots.front();
    for (const GameSession::UpgradeSlot& candidate : slots) {
        if (candidate.type == upgradeSlot_) {
            slot = &candidate;
        }
    }

    // The slot names: no strref ships for them, so (ENGINE CHOICE) upgradetypes.2da's label with the
    // underscores read as spaces.
    const auto slotName = [&](int type) {
        std::string name;
        if (auto table = loadTwoDa(game_.install(), "upgradetypes"); table.has_value()) {
            name = table->cell(std::to_string(type), "label");
        }
        std::replace(name.begin(), name.end(), '_', ' ');
        return name;
    };

    for (const char* id : {"31", "32", "33", "41", "42", "43", "44"}) {
        gui_.setVisible(std::string("BTN_UPGRADE") + id, false);
        gui_.setVisible(std::string("LBL_UPGRADE") + id, false);
    }
    for (const char* tag : {"LBL_SLOTNAME", "LBL_PROPERTY", "LBL_UPGRADES", "LBL_UPGRADE_COUNT", "LB_DESC", "LBL_DESCBG",
                            "3D_MODEL"}) {
        gui_.setVisible(tag, !saber);
    }
    for (const char* tag : {"LBL_LSSLOTNAME", "LB_DESC_LS", "LBL_DESCBG_LS", "3D_MODEL_LS"}) {
        gui_.setVisible(tag, saber);
    }
    int filled = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const int row = effective(slots[i].type);
        filled += row >= 0 ? 1 : 0;
        if (saber) {
            continue;
        }
        // Four slots use the 4x row of buttons, fewer the 3x row (which of the two armor's two slots
        // take is a guess: the shipped layout has no two-slot row).
        const bool four = slots.size() == 4;
        const std::string id = std::string(four ? "4" : "3") + std::to_string(i + 1);
        gui_.setVisible("BTN_UPGRADE" + id, true);
        gui_.setVisible("LBL_UPGRADE" + id, true);
        const bool picked = slots[i].type == upgradeSlot_;
        gui_.setSelected("BTN_UPGRADE" + id, picked);
        // lbl_top2 / lbl_bot2 are the shipped slot frames in their lit state.
        gui_.setFill("BTN_UPGRADE" + id, std::string(four ? "lbl_top" : "lbl_bot") + (picked ? "2" : ""));
        gui_.setFill("LBL_UPGRADE" + id,
                     row >= 0 ? itemInfo(upgradeRowItem(row)).icon : std::string(four ? "icon_filltop" : "icon_fillbot"));
    }
    // The item's 3D model is not rendered; the shipped silhouette of its kind (the picture the item
    // kind buttons carry) stands in the model frame.
    static const char* const silhouettes[4] = {"lbl_wuplsaber", "lbl_wuprange", "lbl_wupmelee", "lbl_wuparmor"};
    gui_.setFill(saber ? "3D_MODEL_LS" : "3D_MODEL", silhouettes[std::clamp(upgradeCategory_, 0, 3)]);
    gui_.setText(saber ? "LBL_LSSLOTNAME" : "LBL_SLOTNAME", slotName(slot->type));
    gui_.setVisible("LBL_PROPERTY", false); // no text source in the data or retail notes found; the description box carries it
    gui_.setText("LBL_UPGRADE_COUNT", std::to_string(filled));

    // The upgrade items that fit the slot: nothing, what is installed, then the pack's.
    const int installed = game_.upgradeInstalled(*target, slot->type);
    upgradeListRows_.clear();
    std::vector<GuiListItem> list;
    GuiListItem none;
    none.text = tlk(363); // "None"
    list.push_back(none);
    upgradeListRows_.push_back(-1);
    if (installed >= 0) {
        const ItemInfo& info = itemInfo(upgradeRowItem(installed));
        GuiListItem row;
        row.text = info.name + " (" + tlk(32346) + ")"; // "Equipped" stands for installed (a guess)
        row.icon = info.icon;
        list.push_back(std::move(row));
        upgradeListRows_.push_back(installed);
    }
    for (const InventoryItem& item : pack) {
        const int row = game_.upgradeRowOf(item.resref);
        if (row < 0 || row == installed || std::find(slot->rows.begin(), slot->rows.end(), row) == slot->rows.end()
            || std::find(upgradeListRows_.begin(), upgradeListRows_.end(), row) != upgradeListRows_.end()) {
            continue;
        }
        const ItemInfo& info = itemInfo(item);
        GuiListItem entry;
        entry.text = item.stack > 1 ? info.name + " (" + std::to_string(item.stack) + ")" : info.name;
        entry.icon = info.icon;
        list.push_back(std::move(entry));
        upgradeListRows_.push_back(row);
    }
    std::size_t selection = 0;
    const int chosen = effective(slot->type);
    for (std::size_t i = 0; i < upgradeListRows_.size(); ++i) {
        if (upgradeListRows_[i] == chosen) {
            selection = i;
        }
    }
    gui_.setRowHeight("LB_ITEMS", saber ? 34.0f : 34.0f); // two text lines fit a row (installed items wrap with "(Equipped)")
    gui_.setItems("LB_ITEMS", std::move(list));
    gui_.setListSelection("LB_ITEMS", selection);
    shownRow_ = selection;

    GuiListItem description;
    description.selectable = false;
    description.text = chosen >= 0 ? describe(upgradeRowItem(chosen)) : upgradedDescription(*target);
    gui_.setItems(saber ? "LB_DESC_LS" : "LB_DESC", {description});
    gui_.setEnabled("BTN_ASSEMBLE", !upgradePending_.empty());
}

bool GameScreens::actionUpgrade(const std::string& tag)
{
    if (tag == "BTN_BACK" || tag == "BTN_EXIT" || tag == "BTN_CANCEL") {
        upgradeBack();
        return true;
    }
    const std::vector<InventoryItem>& pack = game_.inventory();
    const auto categoryCount = [&](int category) {
        int count = 0;
        for (const InventoryItem& item : pack) {
            count += game_.upgradeCategory(item) == category ? 1 : 0;
        }
        return count;
    };
    const auto doubleClicked = [&]() {
        const int row = gui_.lastItem();
        if (row < 0) {
            return false;
        }
        const bool again = clickedRow_ == static_cast<std::size_t>(row);
        clickedRow_ = static_cast<std::size_t>(row);
        gui_.setListSelection("LB_ITEMS", static_cast<std::size_t>(row));
        return again;
    };
    const auto chooseItem = [&](std::size_t index) {
        const InventoryItem& item = pack[index];
        upgradeObject_ = item.object;
        upgradeResref_ = lowerOf(item.resref);
        upgradePending_.clear();
        const auto slots = game_.upgradeSlots(item);
        upgradeSlot_ = slots.empty() ? 0 : slots.front().type;
        upgradeStage_ = 2;
    };

    if (bound_ == "upgradesel") {
        static const char* const buttons[4] = {"BTN_LIGHTSABER", "BTN_RANGED", "BTN_MELEE", "BTN_ARMOR"};
        for (int category = 0; category < 4; ++category) {
            if (tag == buttons[category]) {
                if (categoryCount(category) > 0) {
                    upgradeCategory_ = category;
                }
                return true;
            }
        }
        if (tag == "BTN_UPGRADEITEMS" && upgradeCategory_ >= 0 && categoryCount(upgradeCategory_) > 0) {
            upgradeStage_ = 1;
        }
        return true;
    }
    if (bound_ == "upgradeitems") {
        if (tag == "LB_ITEMS") {
            const int row = gui_.lastItem();
            if (doubleClicked() && row >= 0 && static_cast<std::size_t>(row) < rows_.size()) {
                chooseItem(rows_[static_cast<std::size_t>(row)]);
            }
            return true;
        }
        if (tag == "BTN_UPGRADEITEM") {
            const std::size_t selected = gui_.listSelection("LB_ITEMS");
            if (selected < rows_.size()) {
                chooseItem(rows_[selected]);
            }
        }
        return true;
    }

    // upgrade
    std::size_t index = 0;
    const InventoryItem* target = upgradeTargetItem(&index);
    if (target == nullptr) {
        return true;
    }
    if (tag == "LB_ITEMS") {
        const int row = gui_.lastItem();
        if (row >= 0 && static_cast<std::size_t>(row) < upgradeListRows_.size()) {
            const int installed = game_.upgradeInstalled(*target, upgradeSlot_);
            const int wanted = upgradeListRows_[static_cast<std::size_t>(row)];
            if (wanted == installed) {
                upgradePending_.erase(upgradeSlot_);
            } else {
                upgradePending_[upgradeSlot_] = wanted;
            }
        }
        return true;
    }
    if (tag.size() == 13 && tag.rfind("BTN_UPGRADE", 0) == 0 && std::isdigit(static_cast<unsigned char>(tag[11]))
        && std::isdigit(static_cast<unsigned char>(tag[12]))) {
        const auto slots = game_.upgradeSlots(*target);
        const std::size_t slotIndex = static_cast<std::size_t>(tag[12] - '1');
        if (slotIndex < slots.size()) {
            upgradeSlot_ = slots[slotIndex].type;
        }
        return true;
    }
    if (tag == "BTN_ASSEMBLE") {
        game_.assembleUpgrades(index, upgradePending_);
        upgradePending_.clear();
    }
    return true;
}


std::vector<GameScreens::IconGroup> GameScreens::iconAudit()
{
    std::vector<IconGroup> groups;
    const auto note = [&](IconGroup& group, const std::string& key, const std::string& icon) {
        ++group.total;
        if (icon.empty() || painter_.texture(icon) == nullptr) {
            ++group.missing;
            if (group.failures.size() < 400) {
                group.failures.push_back(key + " '" + icon + "'");
            }
        }
    };
    // Items: every UTI, through itemInfo (the inventory, equip, store and upgrade rows).
    {
        IconGroup items;
        items.name = "items";
        for (const std::string& resref : game_.install().resourceNames(2025)) {
            int baseItem = -1;
            if (auto data = game_.install().find(resref, 2025)) {
                try {
                    kotor::Gff uti = kotor::Gff::parse(std::move(*data), resref + ".uti");
                    if (const kotor::GffField* field = uti.field(uti.rootIndex(), "BaseItem")) {
                        baseItem = static_cast<int>(uti.integer(*field));
                    }
                } catch (const kotor::FormatError&) {
                }
            }
            InventoryItem item;
            item.resref = resref;
            item.baseItem = baseItem;
            note(items, resref, itemInfo(item).icon);
        }
        groups.push_back(std::move(items));
    }
    // The 2DA icon columns the ability screens read.
    const std::pair<const char*, const char*> columns[] = {
        {"feat", "icon"}, {"spells", "iconresref"}, {"skills", "icon"}, {"classes", "icon"}};
    for (const auto& [table, column] : columns) {
        IconGroup group;
        group.name = table;
        if (const auto data = loadTwoDa(game_.install(), table)) {
            const int icon = data->columnIndex(column);
            const int label = data->columnIndex("label");
            for (std::size_t row = 0; icon >= 0 && row < data->rowCount(); ++row) {
                if (label >= 0 && data->cell(row, static_cast<std::size_t>(label)).empty()
                    && data->cell(row, static_cast<std::size_t>(label)) != "****") {
                    continue; // an unused row
                }
                // Only the force powers are listed on the ability screens; the rows for item, special
                // and monster abilities (and the retired "_XXX" rows) are never asked for an icon.
                if (group.name == "spells" && label >= 0) {
                    const std::string name = data->cell(row, static_cast<std::size_t>(label));
                    if (name.rfind("FORCE_POWER", 0) != 0 || name.find("XXX") != std::string::npos) {
                        continue;
                    }
                }
                note(group, std::to_string(row), data->cell(row, static_cast<std::size_t>(icon)));
            }
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

} // namespace ke
