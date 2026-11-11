#include "game/DialogueScreen.h"

#include "core/Log.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>

namespace ke {

namespace {

// A row of a shipped 2DA as a strref (UINT32_MAX when empty).
std::uint32_t skillNameStrref(kotor::Install& install, std::size_t row)
{
    auto data = install.find("skills", kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        return UINT32_MAX;
    }
    try {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "skills");
        const int column = table.columnIndex("name");
        if (column < 0 || row >= table.rowCount()) {
            return UINT32_MAX;
        }
        const std::string cell = table.cell(row, static_cast<std::size_t>(column));
        return cell.empty() || cell == "****" ? UINT32_MAX
                                              : static_cast<std::uint32_t>(std::strtoul(cell.c_str(), nullptr, 10));
    } catch (const kotor::FormatError&) {
        return UINT32_MAX;
    }
}

// 006a86a0 (SetReplies): reply 1..9 are numbered with "%d. " (007 55a6c); every later one gets the fixed
// "-. " (00755a68), so a tenth reply is not "10. ". (Kotor Patch Manager's DialogueRepliesPast9 patches
// the `cmp ebx, 9` at 006a8741 to undo this.)
std::string replyPrefix(std::size_t index)
{
    return index < 9 ? std::to_string(index + 1) + ". " : std::string("-. ");
}

} // namespace

DialogueScreen::DialogueScreen(GuiSystem& gui, GameSession& game, GuiPainter* painter, GuiSystem* computer,
                               GuiSystem* camera)
    : gui_(gui), game_(game), painter_(painter), computer_(computer), camera_(camera)
{
}

bool DialogueScreen::cameraFeedShown() const
{
    // 00633660 / 00631d80: the node's CameraAngle is 6 and the panel on screen is not the cinematic
    // one (a Computer conversation); 0062c030 then hides the terminal and shows the camera panel.
    // The feed is up for as long as the node is (a voiced line, else its Delay); the replies that
    // follow (634340 -> 0062d8c0) bring the terminal back.
    const DialogueView& view = game_.dialogue();
    return camera_ != nullptr && computer_ != nullptr && view.active && view.computer && view.cameraAngle == 6
        && view.cameraId >= 0 && game_.dialogueLineSpeaking();
}

bool DialogueScreen::cancelFeed()
{
    if (!cameraFeedShown()) {
        return false;
    }
    logInfo(concat("dialogue screen: live feed cancelled on entry ", game_.dialogue().entry, " camera ", game_.dialogue().cameraId));
    game_.skipDialogueLine();
    return true;
}

void DialogueScreen::fitFeed(int windowWidth, int windowHeight)
{
    if (camera_ == nullptr || windowWidth <= 0 || windowHeight <= 0
        || (windowWidth == fittedFeedW_ && windowHeight == fittedFeedH_)) {
        return;
    }
    kotor::GuiScreen* screen = camera_->variant("computercamera", "computercamera");
    if (screen == nullptr) {
        return;
    }
    fittedFeedW_ = windowWidth;
    fittedFeedH_ = windowHeight;
    // 006a95f0: the panel's extent becomes (left, top, DAT_0078d1d4, DAT_0078d1d8), the window's size, and
    // LBL_RETURN, keeping its own width and height, moves to x = (W - width) / 2, y = H - 0x4b.
    screen->extent.left = 0;
    screen->extent.top = 0;
    screen->extent.width = windowWidth;
    screen->extent.height = windowHeight;
    for (kotor::GuiControl& control : screen->controls) {
        if (control.tag == "LBL_RETURN") {
            control.extent.left = (windowWidth - control.extent.width) / 2;
            control.extent.top = windowHeight - 75;
        }
    }
}

bool DialogueScreen::computerShown() const
{
    return computer_ != nullptr && game_.dialogue().active && game_.dialogue().computer;
}

GuiSystem& DialogueScreen::activeGui()
{
    if (cameraFeedShown()) {
        return *camera_;
    }
    return computerShown() ? *computer_ : gui_;
}

// The terminal: computer.gui lists the line and every reply at once (LB_MESSAGE over LB_REPLIES),
// with the skills a terminal is worked with and the spikes in the pack beside them. ENGINE CHOICE:
// the labels' strrefs (the skills.2da names, 42414 "Computer Spikes"), the spike count (every stack
// whose item is a g_i_progspike) and the PC's ranks are what the screen shows; retail's source for
// these was not traced, and the repair-unit rows (LBL_REP_UNITS*) are hidden for the same reason.
void DialogueScreen::updateComputer()
{
    GuiSystem& gui = *computer_;
    const DialogueView& view = game_.dialogue();
    const CharacterSheet& sheet = game_.character();
    // Retail 006a8240: the stacks of the party's items whose tag is k_computer_spike and k_repair_part.
    std::int32_t spikes = 0;
    std::int32_t parts = 0;
    for (const InventoryItem& item : game_.inventory()) {
        auto cached = itemTags_.find(item.resref);
        if (cached == itemTags_.end()) {
            std::string tag;
            if (auto data = game_.install().find(item.resref, 2025)) { // UTI
                try {
                    const kotor::Gff uti = kotor::Gff::parse(std::move(*data), item.resref + ".uti");
                    if (const kotor::GffField* field = uti.field(uti.rootIndex(), "Tag")) {
                        tag = uti.text(*field);
                    }
                } catch (const kotor::FormatError&) {
                }
            }
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            cached = itemTags_.emplace(item.resref, std::move(tag)).first;
        }
        if (cached->second == "k_computer_spike") {
            spikes += std::max(1, item.stack);
        } else if (cached->second == "k_repair_part") {
            parts += std::max(1, item.stack);
        }
    }
    const auto rank = [&](int skill) {
        const auto found = sheet.skills.find(game_.rules().skillName(skill));
        return found == sheet.skills.end() ? 0 : found->second;
    };
    // Retail 00631d80, 006a70a0: subtitles off hides what is spoken; a silent line and the replies stay.
    const std::string shownLine = game_.dialogueLineShown() ? view.text : std::string{};
    std::string key = shownLine + "|" + std::to_string(spikes) + "|" + std::to_string(parts) + "|" + std::to_string(rank(0)) + "|"
        + std::to_string(rank(5));
    for (const DialogueOption& option : view.options) {
        key += "\n" + option.text;
    }
    if (key == shown_) {
        return;
    }
    shown_ = key;
    const auto text = [&](std::uint32_t strref) { return game_.localization().tlk().text(strref); };
    const auto skillLabel = [&](int skill) {
        const std::uint32_t ref = skillNameStrref(game_.install(), static_cast<std::size_t>(skill));
        return ref != UINT32_MAX ? text(ref) : game_.rules().skillName(skill);
    };
    gui.setText("LBL_COMP_SKILL", skillLabel(0));
    gui.setText("LBL_COMP_SKILL_VAL", std::to_string(rank(0)));
    gui.setText("LBL_COMP_SPIKES", text(42414));
    gui.setText("LBL_COMP_SPIKES_VAL", std::to_string(spikes));
    gui.setText("LBL_REP_SKILL", skillLabel(5));
    gui.setText("LBL_REP_SKILL_VAL", std::to_string(rank(5)));
    gui.setText("LBL_REP_UNITS", text(42415));
    gui.setText("LBL_REP_UNITS_VAL", std::to_string(parts));

    // Retail lists the terminal's line and replies as entries of their own height (AddDialogMessages style,
    // OrganizeUnequal 00419ee0): each is as tall as its wrapped text, and the list's scrollbar flag is set only
    // when they overflow (006a86a0 SetReplies), as in the paired frame ccS_37 (rows 22/21/40/21 px apart).
    const auto entries = [&](const char* tag, const std::vector<std::string>& lines) {
        bool overflow = false;
        if (painter_ != nullptr) {
            if (const kotor::GuiControl* list = gui.control(tag); list != nullptr && !list->proto.empty()) {
                const kotor::GuiControl& proto = list->proto.front();
                const float dim = proto.hilight.present ? static_cast<float>(proto.hilight.dimension) : 0.0f;
                float total = 0.0f;
                for (const std::string& text : lines) {
                    total += painter_->measureText(proto.font, text, static_cast<float>(proto.extent.width) - 2.0f * dim)
                        + 2.0f * dim;
                }
                overflow = total > static_cast<float>(list->extent.top + list->extent.height - proto.extent.top);
            }
        }
        gui.setRowHeight(tag, 0.0f);
        gui.setEntryList(tag, true, false);
        gui.setScrollbarHidden(tag, !overflow);
    };
    GuiListItem line;
    line.text = shownLine;
    line.selectable = false;
    entries("LB_MESSAGE", {shownLine});
    gui.setItems("LB_MESSAGE", {line});
    gui.setListSelection("LB_MESSAGE", std::numeric_limits<std::size_t>::max());
    std::vector<std::string> labels;
    std::vector<GuiListItem> rows;
    for (std::size_t i = 0; i < view.options.size(); ++i) {
        GuiListItem row;
        row.text = replyPrefix(i) + view.options[i].text;
        row.value = static_cast<int>(i);
        labels.push_back(row.text);
        rows.push_back(std::move(row));
    }
    entries("LB_REPLIES", labels);
    gui.setItems("LB_REPLIES", std::move(rows));
    gui.setListSelection("LB_REPLIES", std::numeric_limits<std::size_t>::max());
    gui.scrollTo("LB_REPLIES", 0);
}

bool DialogueScreen::actionComputer(const std::string& tag)
{
    const DialogueView& view = game_.dialogue();
    if (tag == "LB_REPLIES") {
        const int row = computer_->lastItem();
        if (row >= 0 && static_cast<std::size_t>(row) < view.options.size()) {
            game_.chooseDialogueOption(static_cast<std::size_t>(row));
        }
        return true;
    }
    return false;
}

void DialogueScreen::update()
{
    const DialogueView& view = game_.dialogue();
    if (!view.active) {
        if (!shown_.empty()) {
            logInfo("dialogue screen: closed");
        }
        shown_.clear();
        return;
    }
    if (computerShown()) {
        updateComputer();
        // The entry under the pointer is the lit one (the enter event, 006a6fc0); none while it is elsewhere.
        computer_->setListSelection("LB_REPLIES",
            [&] {
                const int hot = computer_->listItemAt("LB_REPLIES", computer_->pointerX(), computer_->pointerY());
                return hot >= 0 ? static_cast<std::size_t>(hot) : std::numeric_limits<std::size_t>::max();
            }());
        const bool feed = cameraFeedShown();
        if (feed) {
            // 006a95f0 builds the label from computercamera.gui; its text is strref 48226.
            camera_->setText("LBL_RETURN", game_.localization().tlk().text(48226));
            if (!feedLogged_) {
                logInfo(concat("dialogue screen: live feed up on entry ", view.entry, " camera ", view.cameraId));
            }
        }
        feedLogged_ = feed;
        return;
    }
    // While the entry is spoken the line is up (006a70a0); once it is over, a list with a reply to
    // say replaces it (SetReplies 006a86a0) and the line moves to the top bar (006a8de0). An entry
    // that only continues never shows a list: the session moves on by itself.
    bool sayable = false;
    for (const DialogueOption& option : view.options) {
        sayable = sayable || (option.reply != UINT32_MAX && option.text != "(Continue)");
    }
    repliesShown_ = sayable && !game_.dialogueLineSpeaking();
    std::string key = repliesShown_ ? "R" : "L";
    const std::string shownLine = game_.dialogueLineShown() ? view.text : std::string{};
    key += shownLine;
    for (const DialogueOption& option : view.options) {
        key += "\n" + option.text;
    }
    if (key == shown_) {
        return;
    }
    shown_ = key;
    logInfo(concat("dialogue screen: entry ", view.entry, " ", repliesShown_ ? "replies " : "line ", view.options.size(), " options, line \"",
                   shownLine.substr(0, 48), "\""));
    // CSWGuiDialogCinematic::SetReplies (006a8de0) hands the line to the top bar (006a7ae0) while
    // the replies take the bottom one.
    topLine_ = repliesShown_ ? shownLine : std::string{};
    // 006a8b40: the spoken line is drawn in (0, 0.66, 0.98) (007a23b4), whatever dialog.gui holds.
    gui_.setTextColor("LBL_MESSAGE", kotor::Vec3{0.0f, 0.66f, 0.98f});
    gui_.setVisible("LB_REPLIES", repliesShown_);
    gui_.setVisible("LBL_MESSAGE", !repliesShown_);
    if (repliesShown_) {
        // Replies wrap: every row is as tall as the longest one (the list steps by a fixed row).
        float height = 0.0f;
        if (painter_ != nullptr) {
            if (const kotor::GuiControl* list = gui_.control("LB_REPLIES"); list != nullptr && !list->proto.empty()) {
                const kotor::GuiControl& proto = list->proto.front();
                for (std::size_t i = 0; i < view.options.size(); ++i) {
                    height = std::max(height, painter_->measureText(proto.font, replyPrefix(i) + view.options[i].text,
                                                                     static_cast<float>(proto.extent.width)));
                }
                height = std::max(height, static_cast<float>(proto.extent.height));
            }
        }
        gui_.setRowHeight("LB_REPLIES", height);
        // 006a86a0 ends SetReplies by setting the list's scrollbar flag (0x20 at +0x2bc) only when the last
        // visible row (+0x2ca, from OrganizeUnequal 00419ee0) is short of the last reply: no arrows while
        // every reply fits.
        if (const kotor::GuiControl* list = gui_.control("LB_REPLIES"); list != nullptr && height > 0.0f) {
            const float used = static_cast<float>(view.options.size()) * (height + static_cast<float>(list->padding))
                - static_cast<float>(list->padding);
            gui_.setScrollbarHidden("LB_REPLIES", used <= static_cast<float>(list->extent.height));
        }
        std::vector<GuiListItem> rows;
        for (std::size_t i = 0; i < view.options.size(); ++i) {
            GuiListItem row;
            row.text = replyPrefix(i) + view.options[i].text;
            row.value = static_cast<int>(i);
            // 006a8b40 colours the replies (0, 0.66, 0.98) (007a23b4); the row under the pointer takes
            // (0.98, 1, 0) (007a23c0) through the enter event (006a6fc0), the leave event restores the blue.
            row.hasColor = true;
            row.color = kotor::Vec3{0.0f, 0.66f, 0.98f};
            row.hasHoverColor = true;
            row.hoverColor = kotor::Vec3{0.98f, 1.0f, 0.0f};
            rows.push_back(std::move(row));
        }
        gui_.setItems("LB_REPLIES", std::move(rows));
        gui_.setListSelection("LB_REPLIES", 0);
        gui_.scrollTo("LB_REPLIES", 0);
    } else {
        gui_.setText("LBL_MESSAGE", shownLine);
    }
}

void DialogueScreen::fitWidth(int windowWidth)
{
    constexpr int kMargin = 48; // 00409ee0
    const int width = windowWidth - 2 * kMargin;
    if (width == fittedWidth_ || width <= 0) {
        return;
    }
    kotor::GuiScreen* screen = gui_.variant("dialog", "dialog"); // the screen in use
    if (screen == nullptr) {
        return;
    }
    fittedWidth_ = width;
    // 006a8b40 sets the panel's extent (x = margin, width = W - 2 * margin, height 100), then
    // 006a7ca0 gives LB_REPLIES and LBL_MESSAGE that width, each keeping its x, y and height, and
    // each of the 20 reply rows the list's width less its border.
    screen->extent.width = width;
    for (kotor::GuiControl& control : screen->controls) {
        if (control.tag != "LB_REPLIES" && control.tag != "LBL_MESSAGE") {
            continue;
        }
        const int grow = width - control.extent.width;
        control.extent.width = width;
        for (kotor::GuiControl& item : control.proto) {
            item.extent.width += grow;
        }
    }
    shown_.clear(); // the rows are measured again at the new width
}

void DialogueScreen::paintTop(ImDrawList* draw, float width, float barTop)
{
    if (painter_ == nullptr || draw == nullptr || topLine_.empty() || !repliesShown_ || computerShown()) {
        return;
    }
    // 006a78c0: a label in LBL_MESSAGE's font, coloured (0, 0.66, 0.98) (007a23b4), placed inside
    // the safe margin (00409ee0: 48, 36) and ending 5 px above the band, text bottom-centred (0x22).
    const kotor::GuiControl* message = gui_.control("LBL_MESSAGE");
    if (message == nullptr) {
        return;
    }
    const GuiFont* font = painter_->font(message->font);
    if (font == nullptr) {
        return;
    }
    constexpr float kMarginX = 48.0f;
    constexpr float kMarginY = 36.0f;
    const ImU32 colour = IM_COL32(0, static_cast<int>(0.66f * 255.0f + 0.5f), static_cast<int>(0.98f * 255.0f + 0.5f), 255);
    painter_->drawText(draw, *font, topLine_, kMarginX, kMarginY, width - kMarginX, barTop - 5.0f, 0x22, colour, 1.0f, true);
}

bool DialogueScreen::action(const std::string& tag)
{
    const DialogueView& view = game_.dialogue();
    if (!view.active) {
        return false;
    }
    if (computerShown()) {
        return actionComputer(tag);
    }
    if (tag == "LB_REPLIES") {
        const int row = gui_.lastItem();
        if (row >= 0 && static_cast<std::size_t>(row) < view.options.size()) {
            game_.chooseDialogueOption(static_cast<std::size_t>(row));
        }
        return true;
    }
    if (tag == "LBL_MESSAGE") {
        // 006a7230 input 0x27: a click while the line is spoken skips it (SkipPastDialog 004ae970).
        if (game_.dialogueLineSpeaking()) {
            game_.skipDialogueLine();
        } else if (view.options.size() == 1) {
            game_.chooseDialogueOption(0);
        }
        return true;
    }
    return false;
}

} // namespace ke
