#include "game/FrontEnd.h"

#include "core/Log.h"
#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// Slider/checkbox control tag -> setting key. Sliders run 0..100 like the shipped
// MAXVALUE; checkboxes are 0/1.
struct Mapping {
    const char* screen;
    const char* tag;
    const char* key;
    bool slider;
};

constexpr Mapping kMappings[] = {
    {"optgameplay", "CB_INVERTCAM", "gameplay.invertcamera", false},
    {"optgameplay", "CB_LEVELUP", "gameplay.autolevelup", false},
    {"optgameplay", "CB_AUTOSAVE", "gameplay.autosave", false},
    {"optgameplay", "CB_REVERSE", "gameplay.reversecamera", false},
    {"optgameplay", "CB_DISABLEMOVE", "gameplay.disablemouse", false},
    {"optsound", "SLI_MUSIC", "sound.music", true},
    {"optsound", "SLI_VO", "sound.voice", true},
    {"optsound", "SLI_FX", "sound.effects", true},
    {"optsound", "SLI_MOVIE", "sound.movie", true},
    {"optsoundadv", "CB_FORCESOFTWARE", "sound.forcesoftware", false},
    {"optgraphics", "SLI_GAMMA", "graphics.gamma", true},
    {"optgraphics", "CB_SHADOWS", "graphics.shadows", false},
    {"optgraphics", "CB_GRASS", "graphics.grass", false},
    {"optgraphicsadv", "CB_FRAMEBUFF", "graphics.framebuffer", false},
    {"optgraphicsadv", "CB_VSYNC", "graphics.vsync", false},
    {"optgraphicsadv", "CB_SOFTSHADOWS", "graphics.softshadows", false},
    {"optautopause", "CB_ENEMYSIGHTED", "autopause.enemysighted", false},
    {"optautopause", "CB_PARTYKILLED", "autopause.partykilled", false},
    {"optautopause", "CB_ACTIONMENU", "autopause.actionmenu", false},
    {"optautopause", "CB_ENDROUND", "autopause.endround", false},
    {"optautopause", "CB_TRIGGERS", "autopause.triggers", false},
    {"optautopause", "CB_MINESIGHTED", "autopause.minesighted", false},
    {"optmouse", "SLI_MOUSESEN", "mouse.sensitivity", true},
    {"optmouse", "CB_REVBUTTONS", "mouse.reversebuttons", false},
};

// The help text each option control shows in the screen's description list when it takes the focus,
// from retail's panel constructors: each sets control+0x58 to the strref and registers the panel's
// focus handler (event 0, a tail call into the routine that fills LB_DESC) on that control. A control
// listed there without a focus handler (BTN_KEYMAP, BTN_MOUSE, BTN_ADVANCED, BTN_DIFFLEFT...) leaves
// the text alone, however much its strref field says, so it has no row here.
//   optionsmain 006e3e80 (handler 006e0010), optionsingame 006ab2f0 (006ab220, a switch on the button),
//   optgameplay 006e69a0 (006e6710 for the difficulty, 006e68a0), optgraphics 006e2e70 (006e0150, 006e0140),
//   optgraphicsadv 006e16e0 (006e0460), optsound 006e3550 (006dfaa0), optsoundadv 006e20a0 (006ddfa0),
//   optmouse 006e2600 (006de200, 006dfe60), optautopause 006e76a0 (006e7690, filling LB_DETAILS).
constexpr OptionHelpRow kOptionHelp[] = {
    {"optionsmain", "BTN_GAMEPLAY", 48006},
    {"optionsmain", "BTN_FEEDBACK", 48537},
    {"optionsmain", "BTN_AUTOPAUSE", 48572},
    {"optionsmain", "BTN_GRAPHICS", 48010},
    {"optionsmain", "BTN_SOUND", 48012},
    {"optionsingame", "BTN_LOADGAME", 42300},
    {"optionsingame", "BTN_SAVEGAME", 42301},
    {"optionsingame", "BTN_GAMEPLAY", 48006},
    {"optionsingame", "BTN_FEEDBACK", 48537},
    {"optionsingame", "BTN_AUTOPAUSE", 48572},
    {"optionsingame", "BTN_GRAPHICS", 48010},
    {"optionsingame", "BTN_SOUND", 48012},
    {"optionsingame", "BTN_QUIT", 42302},
    {"optgameplay", "BTN_DIFFICULTY", 42265},
    {"optgameplay", "CB_LEVELUP", 42266},
    {"optgameplay", "CB_INVERTCAM", 48014},
    {"optgameplay", "CB_AUTOSAVE", 38038},
    {"optgameplay", "CB_REVERSE", 48213},
    {"optgameplay", "CB_DISABLEMOVE", 42484},
    {"optgraphics", "SLI_GAMMA", 42310},
    {"optgraphics", "BTN_RESOLUTION", 47967},
    {"optgraphics", "CB_SHADOWS", 47951},
    {"optgraphics", "CB_GRASS", 47955},
    {"optgraphicsadv", "BTN_ANTIALIAS", 47971},
    {"optgraphicsadv", "BTN_ANTIALIASLEFT", 47971},
    {"optgraphicsadv", "BTN_ANTIALIASRIGHT", 47971},
    {"optgraphicsadv", "BTN_TEXQUAL", 47965},
    {"optgraphicsadv", "BTN_TEXQUALLEFT", 47965},
    {"optgraphicsadv", "BTN_TEXQUALRIGHT", 47965},
    {"optgraphicsadv", "CB_FRAMEBUFF", 47957},
    {"optgraphicsadv", "BTN_ANISOTROPY", 47961},
    {"optgraphicsadv", "BTN_ANISOTROPYLEFT", 47961},
    {"optgraphicsadv", "BTN_ANISOTROPYRIGHT", 47961},
    {"optgraphicsadv", "CB_SOFTSHADOWS", 47953},
    {"optgraphicsadv", "CB_VSYNC", 47969},
    {"optsound", "SLI_MUSIC", 42311},
    {"optsound", "SLI_VO", 42312},
    {"optsound", "SLI_FX", 42313},
    {"optsound", "SLI_MOVIE", 47921},
    {"optsoundadv", "CB_FORCESOFTWARE", 48577},
    {"optsoundadv", "BTN_EAX", 48578},
    {"optsoundadv", "BTN_EAXLEFT", 48578},
    {"optsoundadv", "BTN_EAXRIGHT", 48578},
    {"optmouse", "SLI_MOUSESEN", 48013},
    {"optmouse", "CB_REVBUTTONS", 48450},
    {"optautopause", "CB_ENDROUND", 42445},
    {"optautopause", "CB_ENEMYSIGHTED", 42446},
    {"optautopause", "CB_MINESIGHTED", 49117},
    {"optautopause", "CB_PARTYKILLED", 42447},
    {"optautopause", "CB_ACTIONMENU", 48416},
    {"optautopause", "CB_TRIGGERS", 48214},
};

// The control each constructor selects last (FUN_0040a630, or the panel's vtable+8 which is the same
// routine), which fires its focus event: the description is already filled when the screen opens.
struct OptionHelpFirst {
    const char* screen;
    const char* tag;
};
constexpr OptionHelpFirst kOptionHelpFirst[] = {
    {"optionsmain", "BTN_GAMEPLAY"}, {"optionsingame", "BTN_LOADGAME"}, {"optgameplay", "BTN_DIFFICULTY"},
    {"optgraphics", "SLI_GAMMA"}, {"optgraphicsadv", "BTN_ANTIALIAS"}, {"optsound", "SLI_MUSIC"},
    {"optsoundadv", "CB_FORCESOFTWARE"}, {"optmouse", "SLI_MOUSESEN"}, {"optautopause", "CB_ENDROUND"},
};

std::uint32_t optionHelpRef(const std::string& screen, const std::string& tag)
{
    for (const OptionHelpRow& row : kOptionHelp) {
        if (screen == row.screen && tag == row.tag) {
            return row.strref;
        }
    }
    return UINT32_MAX;
}

// Retail 006e1300; defaults are 0061d950's reset value (word & 0xfffffcbe | 0x4be).
constexpr FeedbackRow kFeedback[] = {
    {42279, 42286, "feedback.hideunequippable", 0x001, false},
    {42280, 42287, "feedback.tutorialpopups", 0x002, true},
    {42281, 42288, "feedback.subtitles", 0x004, true},
    {42283, 42289, "feedback.minimap", 0x008, true},
    {42285, 42291, "feedback.floatingnumbers", 0x010, true},
    {42450, 42451, "feedback.statussummary", 0x020, true},
    {49061, 49065, "feedback.hidequickmenu", 0x200, false},
    {49062, 49066, "feedback.smallfont", 0x100, false},
    {49115, 49114, "feedback.tooltips", 0x400, true},
};

std::string timePlayedText(const std::string& format, std::size_t number, std::int32_t seconds,
                           const std::string& name = {})
{
    // TLK 1594: "Game <CUSTOM0> - <CUSTOM1>h <CUSTOM2>m". TLK 48519 is the same with a line
    // break and <CUSTOM3> (the save's name) after it: the second line of retail's load row.
    std::string out = format;
    const auto replace = [&](const std::string& token, const std::string& value) {
        const std::size_t at = out.find(token);
        if (at != std::string::npos) {
            out.replace(at, token.size(), value);
        }
    };
    replace("<CUSTOM0>", std::to_string(number));
    replace("<CUSTOM1>", std::to_string(seconds / 3600));
    replace("<CUSTOM2>", std::to_string((seconds / 60) % 60));
    replace("<CUSTOM3>", name);
    return out;
}

std::optional<kotor::TwoDa> loadTable(kotor::Install& install, const char* name)
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

std::string cellOf(const kotor::TwoDa& table, std::size_t row, const char* column)
{
    const int index = table.columnIndex(column);
    return index < 0 ? std::string{} : table.cell(row, static_cast<std::size_t>(index));
}

} // namespace

FrontEnd::FrontEnd(GuiSystem& gui, GameSession& game, GuiPainter& painter, UserSettings& settings,
                   std::filesystem::path saveRoot)
    : gui_(gui), game_(game), painter_(painter), settings_(settings), saveRoot_(std::move(saveRoot))
{
}

std::string FrontEnd::tlk(std::uint32_t strref) const
{
    return game_.localization().tlk().text(strref);
}

void FrontEnd::applyAudio()
{
    AudioSystem::UserVolumes volumes;
    volumes.music = settings_.get("sound.music") / 100.0f;
    volumes.voice = settings_.get("sound.voice") / 100.0f;
    volumes.effects = settings_.get("sound.effects") / 100.0f;
    volumes.movie = settings_.get("sound.movie") / 100.0f;
    game_.audio().setUserVolumes(volumes);
}

std::vector<FrontEnd::SaveSlot> FrontEnd::listSaves(const std::filesystem::path& root)
{
    std::vector<SaveSlot> slots;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return slots;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path info = entry.path() / "savenfo.res";
        std::ifstream file(info, std::ios::binary);
        if (!file) {
            continue;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
        try {
            kotor::Gff gff = kotor::Gff::parse(std::move(bytes), info.string());
            SaveSlot slot;
            slot.folder = entry.path();
            const auto text = [&](const char* label) {
                const kotor::GffField* field = gff.field(gff.rootIndex(), label);
                return field != nullptr ? gff.text(*field) : std::string{};
            };
            slot.name = text("SAVEGAMENAME");
            slot.area = text("AREANAME");
            slot.module = text("LASTMODULE");
            slot.portrait = text("PORTRAIT0");
            slot.portrait2 = text("PORTRAIT1");
            slot.portrait3 = text("PORTRAIT2");
            slot.screenshot = text("SCREENSHOT");
            if (const kotor::GffField* played = gff.field(gff.rootIndex(), "TIMEPLAYED")) {
                slot.playedSeconds = gff.integer(*played);
            }
            const std::string folderName = lower(entry.path().filename().string());
            slot.autosave = folderName.find("autosave") != std::string::npos;
            if (!folderName.empty() && std::isdigit(static_cast<unsigned char>(folderName[0])) != 0) {
                slot.id = std::atoi(folderName.c_str());
            }
            if (slot.name.empty()) {
                slot.name = entry.path().filename().string();
            }
            slots.push_back(std::move(slot));
        } catch (const kotor::FormatError&) {
            // an unreadable folder is not a save slot
        }
    }
    // RETAIL-PROVEN (006cc160): rows follow the directory list, ascending; no sort by time.
    // Paired: paired/walk/out_ig/0001_load_picked_pair.png.
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) {
        return a.folder.filename().string() < b.folder.filename().string();
    });
    return slots;
}

void FrontEnd::update(float dt)
{
    const std::string& now = gui_.screenName();
    if (now != bound_) {
        bound_ = now;
        bind(now);
    }
    if (now == "credits" && gui_.control("LB_CREDITS") != nullptr) {
        // The credits text is one tall row; it scrolls up past the list's window and the
        // screen closes when the last line has left.
        creditsScroll_ += dt * 32.0f;
        gui_.setPixelScroll("LB_CREDITS", creditsScroll_);
        if (creditsScroll_ > creditsHeight_ + 8.0f) {
            gui_.pop();
        }
    }
    if (now == "saveload") {
        const int selected = static_cast<int>(gui_.listSelection("LB_GAMES"));
        if (selected != shownSlot_) {
            showSlot(selected);
        }
    }
    pollSettings();
    updateOptionHelp(now);
}

std::span<const FeedbackRow> FrontEnd::feedbackTable()
{
    return kFeedback;
}

void FrontEnd::bindFeedbackRows()
{
    std::vector<GuiListItem> rows;
    for (const FeedbackRow& row : kFeedback) {
        GuiListItem item;
        item.text = tlk(row.name);
        item.check = settings_.get(row.key) != 0.0f ? 1 : 0;
        rows.push_back(std::move(item));
    }
    const std::size_t selection = gui_.listSelection("LB_OPTIONS");
    gui_.setItems("LB_OPTIONS", std::move(rows));
    gui_.setListSelection("LB_OPTIONS", selection);
}

void FrontEnd::showFeedbackHelp(int row)
{
    if (row < 0 || row >= static_cast<int>(std::size(kFeedback)) || row == feedbackHelpRow_) {
        return;
    }
    feedbackHelpRow_ = row;
    GuiListItem item;
    item.text = tlk(kFeedback[static_cast<std::size_t>(row)].help);
    item.selectable = false;
    gui_.setItems("LB_DESC", {std::move(item)});
    gui_.setListSelection("LB_OPTIONS", static_cast<std::size_t>(row)); // 006e0130: the row under the pointer
}

std::span<const OptionHelpRow> FrontEnd::optionHelpTable()
{
    return kOptionHelp;
}

void FrontEnd::updateOptionHelp(const std::string& screen)
{
    if (screen != helpScreen_) {
        feedbackHelpRow_ = -1;
    }
    if (screen == "optfeedback") {
        // Retail 006e0130 then 006de9b0: the hovered row's help, the first row's when the screen opens.
        helpScreen_ = screen;
        if (feedbackHelpRow_ < 0) {
            showFeedbackHelp(0);
        }
        if (gui_.state("LB_OPTIONS").hovered) {
            showFeedbackHelp(gui_.listItemAt("LB_OPTIONS", gui_.pointerX(), gui_.pointerY()));
        }
        return;
    }
    if (screen.rfind("opt", 0) != 0) {
        helpStack_.clear(); // left the options: the next visit builds its panels anew
    }
    if (screen != helpScreen_) {
        helpScreen_ = screen;
        helpControl_.clear();
        // RETAIL-PROVEN: a sub-screen is a panel pushed over the options panel, which is not built
        // again when it closes, so its description list keeps the help it last showed (paired frame
        // walk 0027_options_back2: Sound's help after returning from Sound, nothing lit).
        const auto revealed = std::find_if(helpStack_.begin(), helpStack_.end(),
                                           [&](const auto& entry) { return entry.first == screen; });
        if (revealed != helpStack_.end()) {
            const std::string tag = revealed->second;
            helpStack_.erase(revealed + 1, helpStack_.end());
            if (!tag.empty()) {
                showOptionHelp(screen, tag);
            }
        } else {
            helpStack_.emplace_back(screen, std::string{});
            for (const OptionHelpFirst& first : kOptionHelpFirst) {
                if (screen == first.screen && gui_.control(first.tag) != nullptr) {
                    showOptionHelp(screen, first.tag);
                    // RETAIL-PROVEN: the constructor's SetActiveControl (0040a630) also lights the control:
                    // optsoundadv 006e20a0 selects CB_FORCESOFTWARE, optgraphicsadv 006e16e0 BTN_ANTIALIAS
                    // (paired walk 0024_sound_advanced and 0020_graphics_advanced: both open lit). A button
                    // or checkbox only; the sliders are not paired.
                    const kotor::GuiControl* control = gui_.control(first.tag);
                    if (gui_.focused().empty() && (control->type == 6 || control->type == 7)) {
                        gui_.setFocused(first.tag);
                    }
                }
            }
        }
    }
    // focused() is the control's full key; the table names tags.
    const std::string& focusKey = gui_.focused();
    const std::string focus = focusKey.substr(focusKey.rfind('/') + 1);
    if (focus != helpControl_) {
        // The focus moving to a control with no handler leaves the text as it is (retail keeps it too).
        showOptionHelp(screen, focus);
    }
}

void FrontEnd::showOptionHelp(const std::string& screen, const std::string& tag)
{
    const std::uint32_t strref = optionHelpRef(screen, tag);
    if (strref == UINT32_MAX) {
        return;
    }
    helpControl_ = tag;
    if (!helpStack_.empty() && helpStack_.back().first == screen) {
        helpStack_.back().second = tag;
    }
    GuiListItem item;
    item.text = tlk(strref);
    item.selectable = false;
    gui_.setItems(screen == "optautopause" ? "LB_DETAILS" : "LB_DESC", {std::move(item)});
}

void FrontEnd::pollSettings()
{
    for (const SettingBinding& binding : bindings_) {
        if (!binding.slider) {
            continue;
        }
        const float value = gui_.value(binding.tag);
        if (std::abs(value - settings_.get(binding.key)) > 0.01f) {
            settings_.set(binding.key, value);
            if (binding.key.rfind("sound.", 0) == 0) {
                applyAudio();
            }
        }
    }
}

void FrontEnd::bind(const std::string& screen)
{
    bindings_.clear();
    for (const Mapping& mapping : kMappings) {
        if (screen == mapping.screen && gui_.control(mapping.tag) != nullptr) {
            bindings_.push_back({mapping.tag, mapping.key, mapping.slider});
            if (mapping.slider) {
                gui_.setValue(mapping.tag, settings_.get(mapping.key));
            } else {
                gui_.setSelected(mapping.tag, settings_.get(mapping.key) != 0.0f);
            }
        }
    }
    if (screen.rfind("mainmenu", 0) == 0) {
        bindMainMenu();
    } else if (screen.rfind("opt", 0) == 0) {
        bindOptions(screen);
    } else if (screen == "saveload") {
        bindSaveLoad();
    } else if (screen == "titlemovie") {
        bindMovies();
    } else if (screen == "credits") {
        bindCredits();
    } else if (screen == "confirm") {
        bindConfirm();
    }
}

void FrontEnd::bindMainMenu()
{
    // Retail's debug Warp button, the module list and the downloadable-content banner are
    // not part of the shipped menu (no content to announce, no debug build).
    gui_.setVisible("BTN_WARP", false);
    gui_.setVisible("LB_MODULES", false);
    gui_.setVisible("LBL_NEWCONTENT", false);
    // RETAIL-PROVEN (main menu ctor 0067ace0 binds controls through 0040b930 and never binds LBL_BW
    // or LBL_LUCAS, and only bound controls are built): the BioWare and LucasArts logos are not drawn.
    // Paired: paired\walk\out_fe\0000_menu_main_pair.png.
    gui_.setVisible("LBL_BW", false);
    gui_.setVisible("LBL_LUCAS", false);
    // RETAIL-PROVEN (constructor 0067c4c0 ends on CSWGuiPanel::SetActiveControl 0040a630 with the first button
    // and no sound; that calls HandleInputEvent(0, 1) on it, CSWGuiControl::HandleInputEvent 00418750 turns that
    // into SetActive(1) 00418e00, the same call the pointer's HandleMouseOver 0041a980 makes). The menu opens with
    // New Game lit; its text pulses like any lit button's (paintControl). The panel is built once: coming back from
    // Options or Load (retail 0028_main_again) it keeps whatever the pointer left, so the focus is given once.
    if (!mainMenuFocused_) {
        mainMenuFocused_ = true;
        gui_.setFocused("BTN_NEWGAME");
    }
    // 005f9af0 starts the menu theme (mus_theme_cult) at the player's music volume.
    game_.audio().playMusicResource("mus_theme_cult", 1.0f);
}

std::string FrontEnd::difficultyLabel() const
{
    // difficultyopt.2da rows 0..2: name strref 42335..42337
    static const std::uint32_t names[] = {42335, 42336, 42337};
    const int level = std::clamp(static_cast<int>(settings_.get("gameplay.difficulty")), 0, 2);
    return tlk(names[level]);
}

void FrontEnd::bindOptions(const std::string& screen)
{
    if (screen == "optgameplay") {
        gui_.setText("BTN_DIFFICULTY", difficultyLabel());
    }
    if (screen == "optfeedback") {
        bindFeedbackRows();
    }
    // RETAIL-PROVEN: the value buttons show the current setting, not their file text.
    // SetTexQualText 006df070 (texpacks.2da strrefname of the pack in use), SetAntiAliasText 006df1c0
    // (0 -> 47996), SetAnisotropyText 006df2c0 (1 -> 49079), CSWGuiOptionsSoundAdvanced::SetEAXText
    // 006ddd70 (0 -> 48573). ENGINE CHOICE: those settings are not modelled, so the values are the ones
    // this renderer runs with: the high pack (tpa, row 2), no anti-aliasing, anisotropy off, no EAX.
    // Paired frames walk 0020_graphics_advanced and 0025_sound_advanced.
    if (screen == "optgraphicsadv") {
        std::uint32_t pack = 48005;
        if (auto table = loadTable(game_.install(), "texpacks"); table && table->rowCount() > 2) {
            const std::string cell = cellOf(*table, 2, "strrefname");
            if (!cell.empty()) {
                pack = static_cast<std::uint32_t>(std::strtoul(cell.c_str(), nullptr, 10));
            }
        }
        gui_.setText("BTN_TEXQUAL", tlk(pack));
        gui_.setText("BTN_ANTIALIAS", tlk(47996));
        gui_.setText("BTN_ANISOTROPY", tlk(49079));
        // The same routines hide the arrow that would step past the end (flag 2 cleared): the
        // highest pack hides the right arrow, no anti-aliasing and anisotropy off the left ones.
        gui_.setVisible("BTN_TEXQUALRIGHT", false);
        gui_.setVisible("BTN_ANTIALIASLEFT", false);
        gui_.setVisible("BTN_ANISOTROPYLEFT", false);
    }
    if (screen == "optsoundadv") {
        gui_.setText("BTN_EAX", tlk(48573));
        gui_.setVisible("BTN_EAXLEFT", false); // value 0 hides the left arrow (006ddd70)
    }
    if (screen == "optkeymapping") {
        // RETAIL-PROVEN: 006ed930 builds one row per keymap event through 006ed610, which reads
        // ReMappable / ForceDisplay / Disabled / SortPos and makes no row when neither ReMappable nor
        // ForceDisplay is set. 006ecfd0 then lists the rows whose Page is the open filter tab,
        // insertion-sorted by SortPos (rows with an equal SortPos keep keymap order). A row shows
        // the ActionStrRef text ("Move Forward") and, in its second box, the bound key; the key
        // goes in the item's tag for the painter. The default binding is the Character column.
        // NOT TRACED: the display names retail gives to keys spelt as words (CAPTLOCK, LeftCTRL).
        struct KeymapRow {
            int sortPos;
            std::size_t index;
            GuiListItem item;
        };
        std::vector<KeymapRow> found;
        if (auto table = loadTable(game_.install(), "keymap")) {
            const auto number = [&](std::size_t row, const char* column) {
                const std::string cell = cellOf(*table, row, column);
                return cell.empty() ? 0 : std::atoi(cell.c_str());
            };
            for (std::size_t i = 0; i < table->rowCount(); ++i) {
                if (number(i, "disabled") != 0
                    || (number(i, "remappable") == 0 && number(i, "forcedisplay") == 0)
                    || number(i, "page") != keymapPage_) {
                    continue;
                }
                const std::string strref = cellOf(*table, i, "actionstrref");
                GuiListItem item;
                item.text = strref.empty() ? cellOf(*table, i, "name")
                                           : tlk(static_cast<std::uint32_t>(std::strtoul(strref.c_str(), nullptr, 10)));
                item.tag = cellOf(*table, i, "character");
                found.push_back({number(i, "sortpos"), i, std::move(item)});
            }
        }
        std::stable_sort(found.begin(), found.end(),
                         [](const KeymapRow& a, const KeymapRow& b) { return a.sortPos < b.sortPos; });
        std::vector<GuiListItem> rows;
        for (KeymapRow& row : found) {
            rows.push_back(std::move(row.item));
        }
        gui_.setItems("LST_EventList", std::move(rows));
        // RETAIL-PROVEN (CreateOption 006ed610): each row's height is (font height 16 + border dimension 14) * 2 = 60;
        // five fit the 304 px inner height and the spare 4 px go to the first four rows (OrganizeControls 0041b140),
        // so the rows sit 61 px apart. Paired: route_pairs 0010_gameplay_keymap.
        gui_.setRowHeight("LST_EventList", 60.0f);
    }
    if (screen == "optresolution") {
        // Retail lists the video modes its driver reports (800x600 and up, 4:3 era). Here: the
        // sizes this desktop holds, the current window size, and first a borderless fullscreen
        // row (ENGINE CHOICE: retail had no fullscreen toggle in this screen).
        resolutionRows_.clear();
        static const int kSizes[][2] = {{800, 600},   {1024, 768},  {1280, 720},  {1280, 1024}, {1600, 900},
                                        {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440},
                                        {3840, 2160}};
        for (const auto& size : kSizes) {
            if (desktopWidth_ <= 0 || (size[0] <= desktopWidth_ && size[1] <= desktopHeight_)) {
                resolutionRows_.push_back({size[0], size[1]});
            }
        }
        if (displayWidth_ > 0 && !displayFullscreen_) {
            const bool present = std::any_of(resolutionRows_.begin(), resolutionRows_.end(), [&](const DisplayMode& m) {
                return m.width == displayWidth_ && m.height == displayHeight_;
            });
            if (!present) {
                resolutionRows_.push_back({displayWidth_, displayHeight_});
                std::sort(resolutionRows_.begin(), resolutionRows_.end(), [](const DisplayMode& a, const DisplayMode& b) {
                    return a.width * a.height < b.width * b.height;
                });
            }
        }
        std::vector<GuiListItem> rows;
        GuiListItem fullscreen;
        fullscreen.text = "Fullscreen";
        rows.push_back(std::move(fullscreen));
        std::size_t selected = displayFullscreen_ ? 0 : 1;
        for (std::size_t i = 0; i < resolutionRows_.size(); ++i) {
            GuiListItem item;
            item.text = std::to_string(resolutionRows_[i].width) + "x" + std::to_string(resolutionRows_[i].height);
            rows.push_back(std::move(item));
            if (!displayFullscreen_ && resolutionRows_[i].width == displayWidth_
                && resolutionRows_[i].height == displayHeight_) {
                selected = i + 1;
            }
        }
        gui_.setItems("LB_RESOLUTIONS", std::move(rows));
        gui_.setListSelection("LB_RESOLUTIONS", selected);
    }
}

void FrontEnd::refreshSlots()
{
    slots_ = listSaves(saveRoot_);
}

void FrontEnd::bindSaveLoad()
{
    refreshSlots();
    gui_.setText("LBL_PANELNAME", tlk(saveMode_ ? 1588 : 1585));
    gui_.setText("BTN_SAVELOAD", tlk(saveMode_ ? 1587 : 1589));
    std::vector<GuiListItem> rows;
    if (saveMode_) {
        GuiListItem fresh;
        fresh.text = tlk(1590); // New Slot
        fresh.tag = "new";
        rows.push_back(std::move(fresh));
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        GuiListItem item;
        if (slots_[i].autosave) {
            // Retail's row reads "Auto Save - 18h 17m" (viewed in its 1280x720 capture).
            const std::int32_t played = slots_[i].playedSeconds;
            item.text = tlk(1593) + " - " + std::to_string(played / 3600) + "h " + std::to_string((played / 60) % 60) + "m";
        } else {
            // RETAIL-PROVEN (006c9780): "Game N" is the folder's slot id less one ("000007 - Game6"
            // reads Game 6), not the row's place.
            const std::size_t number = slots_[i].id > 0 ? static_cast<std::size_t>(slots_[i].id - 1) : slots_.size() - i;
            item.text = slots_[i].name.empty() ? timePlayedText(tlk(1594), number, slots_[i].playedSeconds)
                                               : timePlayedText(tlk(48519), number, slots_[i].playedSeconds, slots_[i].name);
        }
        item.value = static_cast<int>(i);
        rows.push_back(std::move(item));
    }
    gui_.setItems("LB_GAMES", std::move(rows));
    // RETAIL-PROVEN: the save entries are not the 50 tall prototype. 006cc160 builds each
    // CSWGuiSaveLoadEntry from an extent whose height is 0x2a, and the list spreads six 42 + 5 rows over its
    // 323 (rows 48 tall, 53 apart: paired walk out_ig5 0001_load_picked, boxes at 89, 142, 195, 248).
    gui_.setRowHeight("LB_GAMES", 42.0f);
    // Retail opens the list with its first row selected (framed yellow, its details and picture
    // shown: paired walk out_ig3 0000_load_list, the pointer away from the list).
    gui_.setListSelection("LB_GAMES", 0);
    shownSlot_ = -2;
    showSlot(0);
}

void FrontEnd::showSlot(int index)
{
    shownSlot_ = index;
    const std::vector<GuiListItem> items = gui_.state("LB_GAMES").items;
    const bool valid = index >= 0 && static_cast<std::size_t>(index) < items.size()
        && items[static_cast<std::size_t>(index)].tag != "new";
    const SaveSlot* slot = valid ? &slots_[static_cast<std::size_t>(items[static_cast<std::size_t>(index)].value)]
                                 : nullptr;
    gui_.setVisible("LBL_SCREENSHOT", slot != nullptr);
    gui_.setVisible("LBL_PM1", slot != nullptr);
    gui_.setVisible("LBL_PM2", slot != nullptr);
    gui_.setVisible("LBL_PM3", slot != nullptr);
    // savenfo AREANAME reads "Planet - Area" (retail: "Taris - Hideout"); the screen shows the
    // planet on the first line and the area on the second.
    std::string planet;
    std::string area;
    if (slot != nullptr) {
        const std::size_t split = slot->area.find(" - ");
        if (split != std::string::npos) {
            planet = slot->area.substr(0, split);
            area = slot->area.substr(split + 3);
        } else {
            planet = slot->area;
        }
    }
    gui_.setText("LBL_PLANETNAME", planet);
    gui_.setText("LBL_AREANAME", area);
    gui_.setEnabled("BTN_SAVELOAD", saveMode_ || slot != nullptr);
    gui_.setEnabled("BTN_DELETE", slot != nullptr);
    if (slot == nullptr) {
        return;
    }
    // The slot's own Screen.tga and its leader's portrait.
    const std::filesystem::path thumb = slot->folder / "Screen.tga";
    std::ifstream file(thumb, std::ios::binary);
    if (file) {
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
        try {
            const kotor::Image image = kotor::decodeTga(bytes, thumb.string());
            thumbKey_ = "save:" + slot->folder.string();
            if (painter_.uploadImage(thumbKey_, image) != nullptr) {
                gui_.setFill("LBL_SCREENSHOT", thumbKey_);
            }
        } catch (const kotor::FormatError&) {
            gui_.setVisible("LBL_SCREENSHOT", false);
        }
    } else if (!slot->screenshot.empty() && painter_.texture(slot->screenshot) != nullptr) {
        // A save with no Screen.tga (the retail autosave) names a texture in savenfo SCREENSHOT
        // ("load_end_m01ab"); retail shows that image (paired walk out_ig3 0000_load_list).
        gui_.setFill("LBL_SCREENSHOT", slot->screenshot);
    } else {
        gui_.setVisible("LBL_SCREENSHOT", false);
    }
    // Three boxes as in retail (viewed in its load list): the leader in the middle, the other
    // party members either side; a box with no member is left empty, not hidden.
    const std::string* portraits[3] = {&slot->portrait, &slot->portrait2, &slot->portrait3};
    const char* boxes[3] = {"LBL_PM1", "LBL_PM2", "LBL_PM3"};
    for (int i = 0; i < 3; ++i) {
        if (!portraits[i]->empty()) {
            gui_.setFill(boxes[i], *portraits[i]);
        } else {
            gui_.setFill(boxes[i], std::string{}); // the frame only, no portrait
        }
    }
}

void FrontEnd::bindMovies()
{
    // movies.2da: strrefname is the title; a movie shows once played, except the rows
    // flagged alwaysshow.
    movieNames_.clear();
    std::vector<GuiListItem> rows;
    if (auto table = loadTable(game_.install(), "movies")) {
        struct Row {
            int order;
            std::string label;
            std::string title;
        };
        std::vector<Row> found;
        for (std::size_t i = 0; i < table->rowCount(); ++i) {
            const std::string label = table->rowLabel(i);
            const bool showAlways = cellOf(*table, i, "alwaysshow") == "1";
            if (!showAlways && settings_.seenMovies.count(label) == 0) {
                continue;
            }
            const std::string ref = cellOf(*table, i, "strrefname");
            std::string title = ref.empty() || ref == "****" ? label
                : tlk(static_cast<std::uint32_t>(std::strtoul(ref.c_str(), nullptr, 10)));
            const std::string order = cellOf(*table, i, "order");
            found.push_back({std::atoi(order.c_str()), label, title});
        }
        std::sort(found.begin(), found.end(), [](const Row& a, const Row& b) { return a.order < b.order; });
        for (const Row& row : found) {
            GuiListItem item;
            item.text = row.title;
            item.tag = row.label;
            rows.push_back(std::move(item));
            movieNames_.push_back(row.label);
        }
    }
    gui_.setText("LBL_TITLE", tlk(1583));
    gui_.setItems("LB_MOVIES", std::move(rows));
}

void FrontEnd::bindCredits()
{
    const kotor::GuiControl* list = gui_.control("LB_CREDITS");
    creditsViewport_ = list != nullptr ? static_cast<float>(list->extent.height) : 400.0f;
    creditsScroll_ = -creditsViewport_; // the text rises from the bottom edge
    const std::string text = tlk(42190);
    float width = 350.0f;
    std::string font = "fnt_d16x16";
    if (list != nullptr && !list->proto.empty()) {
        width = static_cast<float>(list->proto.front().extent.width);
        if (!list->proto.front().font.empty()) {
            font = list->proto.front().font;
        }
    }
    creditsHeight_ = painter_.measureText(font, text, width);
    GuiListItem item;
    item.text = text;
    item.selectable = false;
    gui_.setItems("LB_CREDITS", {item});
    gui_.setPixelScroll("LB_CREDITS", creditsScroll_);
}

void FrontEnd::openConfirm(const std::string& text, const std::string& onOk)
{
    confirmText_ = text;
    confirmOk_ = onOk;
    gui_.pushModal("confirm");
    bound_.clear(); // re-bind
}

void FrontEnd::bindConfirm()
{
    GuiListItem item;
    item.text = confirmText_;
    item.selectable = false;
    gui_.setItems("LB_MESSAGE", {item});
    gui_.setText("BTN_OK", tlk(1580));
    gui_.setText("BTN_CANCEL", tlk(1581));
}

bool FrontEnd::action(const std::string& tag)
{
    const std::string& screen = gui_.screenName();
    if (screen == "confirm") {
        if (tag == "BTN_OK") {
            const std::string what = confirmOk_;
            gui_.popModal();
            if (what == "quit") {
                outcome_.quit = true;
            } else if (what == "delete") {
                const int row = static_cast<int>(gui_.listSelection("LB_GAMES"));
                const std::vector<GuiListItem> items = gui_.state("LB_GAMES").items;
                if (row >= 0 && static_cast<std::size_t>(row) < items.size() && items[static_cast<std::size_t>(row)].tag != "new") {
                    std::error_code ignored;
                    std::filesystem::remove_all(slots_[static_cast<std::size_t>(items[static_cast<std::size_t>(row)].value)].folder, ignored);
                    bound_.clear();
                }
            } else if (what == "overwrite") {
                // handled below through outcome_.saveFolder, already set
            }
            return true;
        }
        if (tag == "BTN_CANCEL") {
            gui_.popModal();
            outcome_.saveFolder.clear();
            return true;
        }
        return false;
    }
    if (tag == "BTN_BACK" && gui_.historyDepth() > 0) {
        gui_.pop();
        return true;
    }
    if (screen.rfind("mainmenu", 0) == 0) {
        if (tag == "BTN_NEWGAME") {
            outcome_.newGame = true;
            return true;
        }
        if (tag == "BTN_LOADGAME") {
            saveMode_ = false;
            gui_.push("saveload");
            return true;
        }
        if (tag == "BTN_MOVIES") {
            gui_.push("titlemovie");
            return true;
        }
        if (tag == "BTN_OPTIONS") {
            // 006e3e80 ends on the vtable+8 call that focuses BTN_GAMEPLAY: it opens lit (walk 0004_options).
            if (gui_.push("optionsmain")) {
                gui_.setFocused("BTN_GAMEPLAY");
            }
            return true;
        }
        if (tag == "BTN_EXIT") {
            outcome_.quit = true;
            return true;
        }
        return tag == "LBL_3DVIEW" || tag == "LBL_GAMELOGO" || tag == "LBL_MENUBG";
    }
    if (screen == "optionsmain" || screen == "optionsingame") {
        static const std::map<std::string, std::string> subs = {
            {"BTN_GAMEPLAY", "optgameplay"}, {"BTN_AUTOPAUSE", "optautopause"},
            {"BTN_GRAPHICS", "optgraphics"}, {"BTN_SOUND", "optsound"},
            {"BTN_FEEDBACK", "optfeedback"}};
        if (const auto found = subs.find(tag); found != subs.end()) {
            gui_.push(found->second);
            return true;
        }
        if (tag == "BTN_LOADGAME") {
            saveMode_ = false;
            gui_.push("saveload");
            return true;
        }
        if (tag == "BTN_SAVEGAME") {
            saveMode_ = true;
            gui_.push("saveload");
            return true;
        }
        if (tag == "BTN_EXIT") {
            outcome_.resumeGame = true; // optionsingame's Exit returns to the world
            return true;
        }
        if (tag == "BTN_QUIT") {
            openConfirm(tlk(42348), "quit");
            return true;
        }
        return false;
    }
    if (screen.rfind("opt", 0) == 0) {
        // Option sub-screens.
        for (const SettingBinding& binding : bindings_) {
            if (binding.tag == tag && !binding.slider) {
                settings_.set(binding.key, gui_.state(tag).selected ? 1.0f : 0.0f);
                return true;
            }
        }
        if (screen == "optfeedback" && tag == "LB_OPTIONS") {
            const int row = gui_.lastItem();
            if (row >= 0 && row < static_cast<int>(std::size(kFeedback))) {
                const char* key = kFeedback[static_cast<std::size_t>(row)].key;
                settings_.set(key, settings_.get(key) != 0.0f ? 0.0f : 1.0f); // 006de550 .. 006deaa0
                bindFeedbackRows();
            }
            return true;
        }
        if (tag == "BTN_DEFAULT") {
            const std::string group = screen == "optgameplay" ? "gameplay."
                : screen == "optfeedback"                     ? "feedback."
                : screen == "optautopause"                    ? "autopause."
                : screen == "optsound" || screen == "optsoundadv" ? "sound."
                : screen == "optmouse"                            ? "mouse."
                                                                  : "graphics.";
            settings_.resetGroup(group);
            bind(screen);
            applyAudio();
            return true;
        }
        if (screen == "optgameplay") {
            if (tag == "BTN_DIFFLEFT" || tag == "BTN_DIFFRIGHT") {
                int level = static_cast<int>(settings_.get("gameplay.difficulty"));
                level = std::clamp(level + (tag == "BTN_DIFFLEFT" ? -1 : 1), 0, 2);
                settings_.set("gameplay.difficulty", static_cast<float>(level));
                gui_.setText("BTN_DIFFICULTY", difficultyLabel());
                return true;
            }
            if (tag == "BTN_KEYMAP") {
                keymapPage_ = 0;
                gui_.push("optkeymapping");
                return true;
            }
            if (tag == "BTN_MOUSE") {
                gui_.push("optmouse");
                return true;
            }
        }
        if (screen == "optkeymapping"
            && (tag == "BTN_Filter_Move" || tag == "BTN_Filter_Game" || tag == "BTN_Filter_Mini")) {
            // The three filter tabs (click handlers registered in 006edc90) relist page 0/1/2 (006ecfd0).
            keymapPage_ = tag == "BTN_Filter_Move" ? 0 : tag == "BTN_Filter_Game" ? 1 : 2;
            bind(screen);
            return true;
        }
        if (screen == "optsound" && tag == "BTN_ADVANCED") {
            gui_.push("optsoundadv");
            return true;
        }
        if (screen == "optgraphics") {
            if (tag == "BTN_ADVANCED") {
                gui_.push("optgraphicsadv");
                return true;
            }
            if (tag == "BTN_RESOLUTION") {
                gui_.push("optresolution");
                return true;
            }
        }
        if (screen == "optresolution" && tag == "BTN_OK") {
            const std::size_t row = gui_.listSelection("LB_RESOLUTIONS");
            if (row == 0) {
                outcome_.fullscreen = 1;
                settings_.set("graphics.fullscreen", 1.0f);
            } else if (row - 1 < resolutionRows_.size()) {
                outcome_.displayWidth = resolutionRows_[row - 1].width;
                outcome_.displayHeight = resolutionRows_[row - 1].height;
                outcome_.fullscreen = 0;
                settings_.set("graphics.fullscreen", 0.0f);
                settings_.set("graphics.width", static_cast<float>(outcome_.displayWidth));
                settings_.set("graphics.height", static_cast<float>(outcome_.displayHeight));
            }
        }
        if (tag == "BTN_OK" || tag == "BTN_CANCEL" || tag == "BTN_Accept" || tag == "BTN_Cancel") {
            return gui_.pop();
        }
        return false;
    }
    if (screen == "saveload") {
        const int row = static_cast<int>(gui_.listSelection("LB_GAMES"));
        const std::vector<GuiListItem> items = gui_.state("LB_GAMES").items;
        const bool valid = row >= 0 && static_cast<std::size_t>(row) < items.size();
        if (tag == "BTN_SAVELOAD" && valid) {
            const GuiListItem& item = items[static_cast<std::size_t>(row)];
            if (saveMode_) {
                std::filesystem::path folder;
                if (item.tag == "new") {
                    // RETAIL-PROVEN (006cc160): the new slot's id is the highest id plus one, at least 2.
                    int id = 1;
                    for (const SaveSlot& slot : slots_) {
                        id = std::max(id, slot.id);
                    }
                    ++id;
                    char name[64];
                    std::snprintf(name, sizeof(name), "%06d - Game%d", id, id - 1);
                    folder = saveRoot_ / name;
                    outcome_.saveFolder = folder;
                    outcome_.saveName = "Game " + std::to_string(id - 1);
                } else {
                    outcome_.saveFolder = slots_[static_cast<std::size_t>(item.value)].folder;
                    outcome_.saveName = slots_[static_cast<std::size_t>(item.value)].name;
                    openConfirm(tlk(1591), "overwrite");
                }
            } else if (item.tag != "new") {
                outcome_.loadFolder = slots_[static_cast<std::size_t>(item.value)].folder;
            }
            return true;
        }
        if (tag == "BTN_DELETE" && valid && items[static_cast<std::size_t>(row)].tag != "new") {
            openConfirm(tlk(1592), "delete");
            return true;
        }
        return tag == "LB_GAMES";
    }
    if (screen == "titlemovie") {
        if (tag == "LB_MOVIES") {
            const int row = gui_.lastItem();
            if (row >= 0 && static_cast<std::size_t>(row) < movieNames_.size()) {
                outcome_.playMovie = movieNames_[static_cast<std::size_t>(row)];
            }
            return true;
        }
    }
    return false;
}

void FrontEnd::setDisplay(int width, int height, bool fullscreen, int desktopWidth, int desktopHeight)
{
    displayWidth_ = width;
    displayHeight_ = height;
    displayFullscreen_ = fullscreen;
    desktopWidth_ = desktopWidth;
    desktopHeight_ = desktopHeight;
}

FrontEndOutcome FrontEnd::takeOutcome()
{
    FrontEndOutcome out = std::move(outcome_);
    outcome_ = FrontEndOutcome{};
    return out;
}

} // namespace ke
