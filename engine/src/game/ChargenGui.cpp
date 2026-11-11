#include "game/ChargenGui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

namespace ke {

namespace {

// A shipped LTR letter table: "LTR V1.0", the letter count, then cumulative float tables in file order:
// start, middle and end for single letters, for each first letter, and for each pair of letters.
class LetterTable {
public:
    static std::optional<LetterTable> load(kotor::Install& install, const std::string& name)
    {
        auto data = install.find(name, 2036); // LTR
        if (!data.has_value() || data->size() < 9 || std::memcmp(data->data(), "LTR V1.0", 8) != 0) {
            return std::nullopt;
        }
        LetterTable table;
        table.count_ = (*data)[8];
        const std::size_t n = table.count_;
        if (n == 0 || data->size() != 9 + (1 + n + n * n) * 3 * n * sizeof(float)) {
            return std::nullopt;
        }
        table.cells_.resize((data->size() - 9) / sizeof(float));
        std::memcpy(table.cells_.data(), data->data() + 9, table.cells_.size() * sizeof(float));
        return table;
    }
    int count() const { return static_cast<int>(count_); }
    // part: 0 start, 1 middle, 2 end. `a` and `b` are -1 when the context is shorter. The letter whose
    // cumulative value first exceeds `roll`, or count() when none does.
    int pick(int part, int a, int b, float roll) const
    {
        const std::size_t n = count_;
        std::size_t base = 0;
        if (b >= 0) {
            base = (1 + n) * 3 * n + (static_cast<std::size_t>(a) * n + static_cast<std::size_t>(b)) * 3 * n;
        } else if (a >= 0) {
            base = 3 * n + static_cast<std::size_t>(a) * 3 * n;
        }
        base += static_cast<std::size_t>(part) * n;
        for (int i = 0; i < count(); ++i) {
            if (roll < cells_[base + static_cast<std::size_t>(i)]) {
                return i;
            }
        }
        return count();
    }

private:
    std::size_t count_ = 0;
    std::vector<float> cells_;
};

char letterOf(int index)
{
    return index == 26 ? '\'' : index == 27 ? '-' : static_cast<char>('a' + index); // retail 00711390
}

int indexOf(char letter)
{
    return letter == '\'' ? 26 : letter == '-' ? 27 : letter - 'a';
}

// One word, per retail 00711390: three letters from the start tables, then the middle table while a roll
// of 0..11 beats the length and the word is short of the limit, then the end table. A dead end in the
// middle or at the end (0071169e..007116ca) gives a letter back past three letters; otherwise it starts
// over, five times at most; a dead end at the start starts over.
std::string letterWord(const LetterTable& table, int limit, std::mt19937& random)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int n = table.count();
    for (int attempt = 0; attempt < 5;) {
        const int a = table.pick(0, -1, -1, unit(random));
        if (a >= n) {
            continue; // retail rolls again here without counting it
        }
        const int b = table.pick(0, a, -1, unit(random));
        if (b >= n) {
            continue;
        }
        const int c = table.pick(0, a, b, unit(random));
        if (c >= n) {
            continue;
        }
        std::string word{letterOf(a), letterOf(b), letterOf(c)};
        for (int steps = 0; steps < 256; ++steps) {
            const int size = static_cast<int>(word.size());
            const int first = indexOf(word[word.size() - 2]);
            const int second = indexOf(word[word.size() - 1]);
            const float roll = unit(random);
            if (size < static_cast<int>(random() % 12u) && (limit == 0 || size != limit - 1)) {
                const int next = table.pick(1, first, second, roll);
                if (next < n) {
                    word.push_back(letterOf(next));
                } else if (size > 3) {
                    word.pop_back();
                } else {
                    break;
                }
                continue;
            }
            const int last = table.pick(2, first, second, roll);
            if (last < n) {
                word.push_back(letterOf(last));
                word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
                return word;
            }
            if (size > 3) {
                word.pop_back();
                continue;
            }
            break;
        }
        ++attempt;
    }
    return {};
}

std::optional<kotor::TwoDa> chargenTable(kotor::Install& install, const char* name)
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

std::uint32_t chargenStrref(const std::optional<kotor::TwoDa>& data, std::size_t row, const char* column)
{
    if (!data || row >= data->rowCount()) {
        return UINT32_MAX;
    }
    const int index = data->columnIndex(column);
    if (index < 0) {
        return UINT32_MAX;
    }
    const std::string value = data->cell(row, static_cast<std::size_t>(index));
    return value.empty() || value == "****" ? UINT32_MAX
                                            : static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
}

constexpr const char* kAbilities[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
constexpr std::uint32_t kAbilityNames[6] = {719, 720, 721, 722, 723, 724};
constexpr const char* kSkillBase[8] = {"COMPUTER_USE", "DEMOLITIONS", "STEALTH", "AWARENESS",
                                       "PERSUADE",     "REPAIR",      "SECURITY", "TREAT_INJURY"};
constexpr std::uint32_t kChargenHeading = 208; // classsel.gui LBL_CHAR_GEN's own strref
constexpr const char* kSkillShort[8] = {"COM", "DEM", "STE", "AWA", "PER", "REP", "SEC", "TRE"};
// Retail 006f7600 sets control+0x58 to 0xde..0xe3 down its slots (STR DEX CON WIS INT CHA) and 006f70e0
// shows the focused slot's text; here by sheet order. The first slot is focused when the screen opens.
constexpr std::uint32_t kAbilityHelp[6] = {222, 223, 224, 226, 225, 227};

} // namespace

std::string chargenRandomName(GameSession& game, int gender)
{
    // Retail 006f9bf0 -> 007118c0: race 6 "human", the gender's first-name table and humanl, limit 15.
    static std::mt19937 random{std::random_device{}()};
    const auto first = LetterTable::load(game.install(), gender == 1 ? "humanf" : "humanm");
    const auto last = LetterTable::load(game.install(), "humanl");
    if (!first.has_value() || !last.has_value()) {
        return {};
    }
    const auto filter = chargenTable(game.install(), "namefilter");
    const auto refused = [&](const std::string& word) {
        // Retail 00711390 draws again when the shipped namefilter.2da has a part of the word.
        if (!filter.has_value()) {
            return false;
        }
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::size_t row = 0; row < filter->rowCount(); ++row) {
            const std::string bad = filter->cell(row, 0);
            if (!bad.empty() && lower.find(bad) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    constexpr int kLimit = 15;
    for (int split = static_cast<int>(random() % (kLimit / 3)) + kLimit / 3; split > 0; --split) {
        for (int part = split; part < kLimit * 2 / 3; ++part) {
            const std::string a = letterWord(*first, part, random);
            const std::string b = letterWord(*last, kLimit - part, random);
            if (!a.empty() && !b.empty() && !refused(a) && !refused(b)) {
                return a + " " + b;
            }
        }
    }
    return {};
}

const char* chargenScreenName(int step)
{
    switch (step) {
    case 0: return "classsel";
    case 1: return "portcust";
    case 2: return "abchrgen";
    case 3: return "skchrgen";
    case 4: return "ftchrgen";
    case 5: return "name";
    default: return "classsel";
    }
}

namespace {

void shiftControl(kotor::GuiControl& control, int dx, int dy, int idOffset)
{
    control.extent.left += dx;
    control.extent.top += dy;
    if (control.id >= 0) {
        control.id += idOffset;
    }
    for (int* link : {&control.moveUp, &control.moveDown, &control.moveLeft, &control.moveRight}) {
        if (*link >= 0) {
            *link += idOffset;
        }
    }
    for (kotor::GuiControl& child : control.children) {
        shiftControl(child, dx, dy, idOffset);
    }
    for (kotor::GuiControl& item : control.proto) {
        shiftControl(item, dx, dy, idOffset);
    }
}

bool hasKeyPrefix(const std::vector<kotor::GuiControl>& controls, const std::string& prefix)
{
    for (const kotor::GuiControl& control : controls) {
        if (control.key.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// Retail binds only the controls its panel class names (FUN_0040b930 in 006eb420 and 006e8ef0); the rest
// of maincg.gui is never drawn. These are the ones neither the chargen nor the level-up class binds.
constexpr const char* kMaincgUnbound[] = {"OLD_FORT_LBL", "OLD_REFL_LBL", "OLD_WILL_LBL", "FORT_ARROW_LBL",
                                          "REFL_ARROW_LBL", "WILL_ARROW_LBL", "OLD_VIT_LBL", "OLD_DEF_LBL",
                                          "NEW_VIT_LBL", "NEW_DEF_LBL", "VIT_ARROW_LBL", "DEF_ARROW_LBL",
                                          "OLD_LBL", "NEW_LBL"};

// Retail's step colours: 007a23c0 lit, 007a23b4 normal, 007a2408 greyed Back (006efc10, 006eefd0).
constexpr kotor::Vec3 kStepLit{0.98f, 1.0f, 0.0f};
constexpr kotor::Vec3 kStepNormal{0.0f, 0.66f, 0.98f};
constexpr kotor::Vec3 kStepGrey{0.0f, 0.33f, 0.49f};

int signedMod(int score)
{
    const int delta = score - 10;
    return delta >= 0 ? delta / 2 : -((1 - delta) / 2);
}

} // namespace

std::string chargenHubScreen(GuiSystem& gui, int panel)
{
    return maincgComposite(gui, panel == kHubQuick ? "quickpnl" : panel == kHubCustom ? "custpnl" : "qorcpnl");
}

std::string maincgComposite(GuiSystem& gui, const std::string& sub)
{
    const std::string alias = "maincg_" + sub;
    kotor::GuiScreen* hub = gui.variant("maincg", alias);
    kotor::GuiScreen* part = gui.variant(sub, alias + "_part");
    if (hub == nullptr || part == nullptr) {
        return {};
    }
    const std::string prefix = (part->tag.empty() ? sub : part->tag) + "/";
    if (!hasKeyPrefix(hub->controls, prefix)) {
        // The sub-panel's controls are placed against its own extent (QORCPNL, QUICKPNL, CUSTPNL at 322,87,
        // LEVELUPPNL at 322,77 inside the 640x480 MAINCG), as retail adds the panel as a child of MAINCG.
        for (kotor::GuiControl control : part->controls) {
            shiftControl(control, part->extent.left, part->extent.top, 1000);
            control.clipped = true;
            control.clip = part->extent;
            hub->controls.push_back(std::move(control));
            hub->controlCount += 1;
        }
    }
    return alias;
}

void hideMaincgUnbound(GuiSystem& gui)
{
    for (const char* tag : kMaincgUnbound) {
        gui.setVisible(tag, false);
    }
}

void bindHubSteps(GuiSystem& gui, int count, int lit)
{
    for (int i = 0; i < count; ++i) {
        const std::string n = std::to_string(i + 1);
        const bool on = i == lit;
        // LBL_n's hilight art (lbl_cg_circ2) is the lit ring: vtable+0x40 with 1 on the lit row only.
        gui.setMarked("LBL_" + n, on);
        gui.setBorderPulse("LBL_" + n, on); // the lit ring pulses with the lit step text (walk 0041..0081)
        gui.setTextColor("LBL_NUM" + n, on ? kStepLit : kStepNormal);
        gui.setTextColor("BTN_STEPNAME" + n, on ? kStepLit : kStepNormal);
        // The lit row pulses with the shared GUI pulse (retail walk 0041..0081: its yellow runs 155..253 over time).
        gui.setTextPulse("BTN_STEPNAME" + n, on);
        gui.setTextPulse("LBL_NUM" + n, on);
        // 006eefd0 (rva 0x2ef0c8): vtable+0x88 (0) on every other row's number label, which then draws at half
        // colour (0, 0.33, 0.49); the step-name buttons keep their full colour. Paired walk cg02..cg23 (retail
        // digits (0, 89, 140), the brackets' shade).
        gui.setEnabled("LBL_NUM" + n, on);
    }
    gui.setTextColor("BTN_BACK", lit == 0 ? kStepGrey : kStepNormal);
}

void bindLevelUpSteps(GuiSystem& gui, const std::array<bool, 5>& rows, int lit, bool backGreyed)
{
    int number = 0;
    for (int i = 0; i < 5; ++i) {
        const std::string n = std::to_string(i + 1);
        const bool shown = rows[static_cast<std::size_t>(i)];
        for (const std::string& tag : {"LBL_" + n, "LBL_NUM" + n, "BTN_STEPNAME" + n}) {
            gui.setVisible(tag, shown);
        }
        if (!shown) {
            continue;
        }
        gui.setText("LBL_NUM" + n, std::to_string(++number)); // 005e5bc0 with the running count
        const bool on = i == lit;
        gui.setMarked("LBL_" + n, on);
        gui.setBorderPulse("LBL_" + n, on); // the lit ring pulses with the lit step text (walk 0041..0081)
        gui.setTextColor("LBL_NUM" + n, on ? kStepLit : kStepNormal);
        gui.setTextColor("BTN_STEPNAME" + n, on ? kStepLit : kStepNormal);
        // The lit row pulses with the shared GUI pulse (retail walk 0041..0081: its yellow runs 155..253 over time).
        gui.setTextPulse("BTN_STEPNAME" + n, on);
        gui.setTextPulse("LBL_NUM" + n, on);
        gui.setEnabled("LBL_NUM" + n, on); // as on the hub (006eefd0): the other rows' numbers draw at half colour
        // 006ee0e0: vtable+0x88 (1) on the lit row's controls and (0) on every other, so only the lit row opens.
        // That call does not recolour (the rows keep 007a23b4), so the gating is GameScreens::actionLevelUp's and the
        // controls stay enabled here: a disabled control is drawn at half colour by the painter.
    }
    gui.setTextColor("BTN_BACK", backGreyed ? kStepGrey : kStepNormal); // 006ee0e0: 007a2408 is the whole grey
    gui.setVisible("BTN_CANCEL", false);
}

void bindLevelUpMaincg(GuiSystem& gui, GameSession& game, const CharacterSheet& sheet)
{
    const Rules& rules = game.rules();
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    hideMaincgUnbound(gui);
    gui.setText("MAIN_TITLE_LBL", tlk(1071)); // 006e8ef0: FUN_00415e50(0x42f)
    gui.setText("LBL_NAME", sheet.name);
    gui.setText("LBL_CLASS", rules.className(sheet.classId));
    gui.setVisible("LBL_LEVEL", true);
    gui.setVisible("LBL_LEVEL_VAL", true);
    gui.setText("LBL_LEVEL", tlk(32154));
    gui.setText("LBL_LEVEL_VAL", std::to_string(sheet.level));
    if (sheet.portraitRow >= 0) {
        if (const std::string portrait = rules.portraitResrefFor(rules.portraitRow(sheet.portraitRow), sheet.alignment);
            !portrait.empty()) {
            gui.setFill("PORTRAIT_LBL", portrait);
        }
    }
    static constexpr const char* labels[6] = {"STR_AB_LBL", "DEX_AB_LBL", "CON_AB_LBL",
                                              "INT_AB_LBL", "WIS_AB_LBL", "CHA_AB_LBL"};
    const std::int32_t scores[6] = {sheet.str, sheet.dex, sheet.con, sheet.intel, sheet.wis, sheet.cha};
    for (int i = 0; i < 6; ++i) {
        gui.setText(labels[i], std::to_string(scores[i]));
    }
    gui.setText("LBL_VIT", std::to_string(sheet.maxHp));
    gui.setText("LBL_DEF", std::to_string(rules.armorClass(sheet)));
    gui.setText("NEW_FORT_LBL", std::to_string(rules.savingThrow(sheet, 0) + signedMod(sheet.con)));
    gui.setText("NEW_REFL_LBL", std::to_string(rules.savingThrow(sheet, 1) + signedMod(sheet.dex)));
    gui.setText("NEW_WILL_LBL", std::to_string(rules.savingThrow(sheet, 2) + signedMod(sheet.wis)));
}

void bindAbilityFocus(GuiSystem& gui, GameSession& game, const std::array<std::int32_t, 6>& scores, int focus,
                      bool levelUp)
{
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    focus = std::clamp(focus, 0, 5);
    for (int i = 0; i < 6; ++i) {
        const std::string base = kAbilities[i];
        const kotor::Vec3 color = i == focus ? kStepLit : kStepNormal;
        gui.setTextColor(base + "_LBL", color);
        gui.setTextColor(base + "_POINTS_BTN", color);
    }
    const std::int32_t score = scores[static_cast<std::size_t>(focus)];
    // Level-up spends one point a step; chargen's cost climbs at 14 and 16 (006f70e0).
    const int cost = levelUp ? 1 : (score < 16 ? (score > 13 ? 2 : 1) : 3);
    for (const char* tag : {"COST_LBL", "COST_POINTS_LBL", "LBL_MODIFIER", "LBL_ABILITY_MOD"}) {
        gui.setVisible(tag, true);
    }
    gui.setText("COST_POINTS_LBL", std::to_string(cost));
    gui.setText("LBL_MODIFIER", tlk(32112));
    // 00647630: score / 2 - 5; a zero shows as "-" (0073ed40), a gain with "+" (00748ed4).
    const int mod = score / 2 - 5;
    gui.setText("LBL_ABILITY_MOD", mod == 0 ? std::string("-") : (mod > 0 ? "+" : "") + std::to_string(mod));
    gui.setVisible("DESC_LBL", true);
    gui.setText("DESC_LBL", tlk(217)); // 006f6fa0 only fills LB_DESC; the label keeps the file's "Description"
    GuiListItem help;
    help.text = tlk(kAbilityHelp[focus]);
    help.selectable = false;
    const GuiControlState before = gui.state("LB_DESC");
    if (before.items.size() != 1 || before.items.front().text != help.text) {
        gui.setItems("LB_DESC", {help});
    }
}

void bindSkillFocus(GuiSystem& gui, GameSession& game, std::int32_t classId, int focus)
{
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    focus = std::clamp(focus, 0, 7);
    for (int i = 0; i < 8; ++i) {
        const kotor::Vec3 color = i == focus ? kStepLit : kStepNormal;
        gui.setTextColor(std::string(kSkillBase[i]) + "_LBL", color);
        gui.setTextColor(std::string(kSkillBase[i]) + "_POINTS_BTN", color);
        // 006f4bf0 sets flag 0x40 (pulse) on the focused row's label and points button, and clears the others'.
        gui.setTextPulse(std::string(kSkillBase[i]) + "_LBL", i == focus);
        gui.setTextPulse(std::string(kSkillBase[i]) + "_POINTS_BTN", i == focus);
    }
    const bool classSkill = game.rules().skillIsClassSkill(classId, focus);
    for (const char* tag : {"COST_LBL", "COST_POINTS_LBL", "CLASSSKL_LBL"}) {
        gui.setVisible(tag, true);
    }
    gui.setText("CLASSSKL_LBL", tlk(classSkill ? 38152 : 38153)); // 006f4b60 picks 0x9508 / 0x9509
    gui.setText("COST_POINTS_LBL", std::to_string(game.rules().skillRankCost(classId, focus))); // 00647900
    gui.setVisible("DESC_LBL", true);
    gui.setText("DESC_LBL", tlk(217));
    const auto skills = chargenTable(game.install(), "skills");
    const std::uint32_t descRef = chargenStrref(skills, static_cast<std::size_t>(focus), "description");
    GuiListItem item;
    item.text = descRef != UINT32_MAX ? tlk(descRef) : std::string{};
    item.selectable = false;
    const GuiControlState before = gui.state("LB_DESC");
    if (before.items.size() != 1 || before.items.front().text != item.text) {
        gui.setItems("LB_DESC", {item});
    }
}

int abilityRowHovered(const GuiSystem& gui)
{
    for (int i = 0; i < 6; ++i) {
        for (const char* part : {"_LBL", "_POINTS_BTN", "_PLUS_BTN", "_MINUS_BTN"}) {
            if (gui.state(std::string(kAbilities[i]) + part).hovered) {
                return i;
            }
        }
    }
    return -1;
}

int skillRowHovered(const GuiSystem& gui)
{
    for (int i = 0; i < 8; ++i) {
        for (const char* part : {"_LBL", "_POINTS_BTN"}) {
            if (gui.state(std::string(kSkillBase[i]) + part).hovered) {
                return i;
            }
        }
        for (const char* part : {"_PLUS_BTN", "_MINUS_BTN"}) {
            if (gui.state(std::string(kSkillShort[i]) + part).hovered) {
                return i;
            }
        }
    }
    return -1;
}

namespace {

// MAINCG's own labels (006ea9f0 portrait and model, 006eac30 scores, 006ea9b0 name, 006ea910 class).
void bindMaincg(GuiSystem& gui, ChargenGuiState& state, GameSession& game)
{
    const Chargen& chargen = state.chargen;
    hideMaincgUnbound(gui);
    gui.setVisible("LBL_LEVEL", false);
    gui.setVisible("LBL_LEVEL_VAL", false);
    gui.setText("LBL_CLASS", game.rules().className(chargen.classId()));
    gui.setText("LBL_NAME", state.nameShown ? chargen.name() : std::string{});
    const CharacterSheet sheet = chargen.build();
    if (const std::string portrait = game.rules().portraitResrefFor(chargen.portrait(), sheet.alignment);
        !portrait.empty()) {
        gui.setFill("PORTRAIT_LBL", portrait);
    }
    static constexpr const char* scores[6] = {"STR_AB_LBL", "DEX_AB_LBL", "CON_AB_LBL",
                                              "INT_AB_LBL", "WIS_AB_LBL", "CHA_AB_LBL"};
    const bool shown = state.statsShown;
    for (int i = 0; i < 6; ++i) {
        gui.setText(scores[i], shown ? std::to_string(chargen.attribute(i)) : std::string{});
    }
    const Rules& rules = game.rules();
    gui.setText("LBL_VIT", shown ? std::to_string(sheet.maxHp) : std::string{});
    gui.setText("LBL_DEF", shown ? std::to_string(rules.armorClass(sheet)) : std::string{});
    // The creature's saves (+0x5c, +0x5d, +0x5e): the class's table plus the ability's modifier.
    gui.setText("NEW_FORT_LBL", shown ? std::to_string(rules.savingThrow(sheet, 0) + signedMod(sheet.con)) : std::string{});
    gui.setText("NEW_REFL_LBL", shown ? std::to_string(rules.savingThrow(sheet, 1) + signedMod(sheet.dex)) : std::string{});
    gui.setText("NEW_WILL_LBL", shown ? std::to_string(rules.savingThrow(sheet, 2) + signedMod(sheet.wis)) : std::string{});
}

void bindHub(GuiSystem& gui, ChargenGuiState& state, GameSession& game)
{
    bindMaincg(gui, state, game);
    if (state.hubPanel == kHubChoice) {
        // QORCPNL: focus follows the pointer (event 0 on each button, 006f09d0 / 006f09e0), and the focused
        // button's help (241 Quick, 242 Custom) fills LB_DESC (006f0860). Quick is focused when it opens.
        for (const char* tag : {"QUICK_CHAR_BTN", "CUST_CHAR_BTN"}) {
            if (gui.state(tag).hovered) {
                state.hubFocus = tag;
            }
        }
        // The focused button shows its hilight border and yellow text (paired frame cg_main, Quick).
        for (const char* tag : {"QUICK_CHAR_BTN", "CUST_CHAR_BTN"}) {
            const bool focused = state.hubFocus == tag;
            gui.setMarked(tag, focused);
            if (const kotor::GuiControl* button = gui.control(tag)) {
                if (focused) {
                    gui.setTextColor(tag, kStepLit); // yellow as retail's focused button (paired frame cg_main)
                } else if (button->hasTextColor) {
                    gui.setTextColor(tag, button->textColor);
                }
            }
        }
        GuiListItem help;
        help.text = game.localization().tlk().text(state.hubFocus == "CUST_CHAR_BTN" ? 242 : 241);
        help.selectable = false;
        const GuiControlState before = gui.state("LB_DESC");
        if (before.items.size() != 1 || before.items.front().text != help.text) {
            gui.setItems("LB_DESC", {help});
        }
        return;
    }
    if (state.hubPanel == kHubQuick) {
        bindHubSteps(gui, 3, state.quickStep);
    } else {
        bindHubSteps(gui, 6, state.customStep);
    }
}

} // namespace

bool chargenNameEditing(const ChargenGuiState& state)
{
    return state.active && state.step == 5;
}

void chargenNameInput(ChargenGuiState& state, const char* utf8)
{
    if (!chargenNameEditing(state) || utf8 == nullptr) {
        return;
    }
    for (const unsigned char c : std::string(utf8)) {
        // Retail 004184f0: only control codes, '_', '/' and '\\' are refused; digits and all other
        // punctuation go in. ENGINE CHOICE: bytes past ASCII stay out, since the font has no glyphs.
        const bool allowed = c >= 0x20 && c < 0x7f && c != '_' && c != '/' && c != '\\';
        if (allowed && state.nameBuffer.size() < kChargenNameMax) {
            state.nameBuffer.push_back(static_cast<char>(c));
            state.nameRefused = false;
        }
    }
}

void chargenNameBackspace(ChargenGuiState& state)
{
    if (chargenNameEditing(state) && !state.nameBuffer.empty()) {
        state.nameBuffer.pop_back();
        state.nameRefused = false;
    }
}

namespace {

// skillinfo as a feat list (FUN_006cdfc0 -> 006acb40 per feat): one unselectable entry per feat, icon and name, the
// message above and OK.
void showFeatPopup(GuiSystem& gui, GameSession& game, const std::vector<std::int32_t>& ids, std::uint32_t message)
{
    if (!gui.pushModal("skillinfo")) {
        return;
    }
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    const auto feats = chargenTable(game.install(), "feat");
    std::vector<GuiListItem> rows;
    for (const std::int32_t id : ids) {
        GuiListItem row;
        const std::size_t index = static_cast<std::size_t>(id);
        row.text = tlk(chargenStrref(feats, index, "name"));
        if (feats && feats->columnIndex("icon") >= 0) {
            row.icon = feats->cell(index, static_cast<std::size_t>(feats->columnIndex("icon")));
        }
        row.selectable = false;
        row.itemEntry = true; // 006acb40 on the skillinfo entries (vtable 0x755ed0): the icon in a lbl_hex_3 frame
        row.count = 1;
        rows.push_back(std::move(row));
    }
    gui.setText("LBL_MESSAGE", tlk(message));
    gui.setRowHeight("LB_SKILLS", 42.0f); // 006ac930 sizes each entry 42 tall
    gui.setItems("LB_SKILLS", std::move(rows));
    gui.setText("BTN_OK", tlk(1580));
}

} // namespace

void bindChargenScreen(GuiSystem& gui, ChargenGuiState& state, GameSession& game)
{
    if (gui.modalActive()) {
        return; // the Recommended popup: the screen below keeps what it shows
    }
    if (state.active && state.step == kChargenHub) {
        bindHub(gui, state, game);
        return;
    }
    if (state.active && state.step == 5) {
        const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
        // Paired frame cg_name: the two titles are "Character Generation" and the step's name.
        if (const kotor::GuiControl* heading = gui.control("MAIN_TITLE_LBL"); heading != nullptr) {
            gui.setText("MAIN_TITLE_LBL", tlk(kChargenHeading));
        }
        // That second line also says why a blank name was refused: retail raises a message box with
        // the same text (42360 "You must specify a name.", 006f9cd0); ENGINE CHOICE: a line.
        gui.setText("SUB_TITLE_LBL", state.nameRefused ? tlk(42360) : tlk(234));
        gui.setText("BTN_RANDOM", tlk(48579)); // the file's own strref (paired frame cg_name)
        // RETAIL-PROVEN (editbox SetText 00416490): while the box takes typing (+0x74 bit 1, set by 006f9bf0) it shows
        // its text through "%s_" (0x73e3ec), a steady underscore, not a blinking caret (paired walk 0079_cg21).
        gui.setText("NAME_BOX_EDIT", state.nameBuffer + "_");
        return;
    }
    if (state.active && state.step == 0 && gui.screenName() == "classsel") {
        // Paired frame cg_class: the slot under the pointer (the first at first) names gender and class,
        // and LBL_DESC gives the class's line. dialog.tlk 32109..32111 by what they say (inferred, the
        // table that picks them was not found); the two genders are 32097 and 32099.
        for (int slot = 0; slot < Chargen::kSlotCount; ++slot) {
            if (gui.state("BTN_SEL" + std::to_string(slot + 1)).hovered) {
                state.classFocus = slot;
            }
        }
        // 006dc3c0 (the loop at 006dc825): each BTN_SELn's border takes 007a23b4 (blue) and its hilight 007a23c0
        // (yellow) over the file's white; the focused slot (the first at open) wears the hilight.
        for (int slot = 0; slot < Chargen::kSlotCount; ++slot) {
            const std::string tag = "BTN_SEL" + std::to_string(slot + 1);
            const bool focused = slot == state.classFocus;
            gui.setMarked(tag, focused);
            gui.setFillColor(tag, focused ? kStepLit : kStepNormal);
        }
        const auto say = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
        const std::int32_t classId = Chargen::slotClass(state.classFocus);
        gui.setText("LBL_CLASS", say(Chargen::slotGender(state.classFocus) == 1 ? 32099 : 32097) + " "
                                     + game.rules().className(classId));
        gui.setText("LBL_DESC", say(classId == 2 ? 32109 : classId == 1 ? 32110 : 32111));
        return;
    }
    if (!state.active || state.step < 2 || state.step > 4) {
        return;
    }
    const Chargen& chargen = state.chargen;
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    // Paired frames cg_attr and cg_feats: "Character Generation" over the step's name, which is what
    // the files themselves carry on MAIN_TITLE_LBL and SUB_TITLE_LBL.
    static const std::uint32_t stepNames[3] = {209, 233, 232};
    gui.setText("MAIN_TITLE_LBL", tlk(kChargenHeading));
    gui.setText("SUB_TITLE_LBL", tlk(stepNames[state.step - 2]));
    // Recommended: the class's row of the shipped tables (Chargen::applyRecommended).
    gui.setVisible("BTN_RECOMMENDED", true);

    if (state.step == 2) {
        gui.setText("REMAINING_SELECTIONS_LBL", std::to_string(chargen.pointsRemaining()));
        gui.setText("SELECTIONS_REMAINING_LBL", tlk(210));
        std::array<std::int32_t, 6> scores{};
        for (int i = 0; i < 6; ++i) {
            const std::string base = kAbilities[i];
            scores[static_cast<std::size_t>(i)] = chargen.attribute(i);
            gui.setText(base + "_LBL", tlk(kAbilityNames[i]));
            gui.setText(base + "_POINTS_BTN", std::to_string(chargen.attribute(i)));
            // 006f7600 (chargen): the minus goes at 8, the plus at 18.
            gui.setVisible(base + "_MINUS_BTN", chargen.attribute(i) != 8);
            gui.setVisible(base + "_PLUS_BTN", chargen.attribute(i) != 18);
        }
        // Focus follows the pointer and stays (event 0, 006f70e0); the first row has it at open.
        if (const int hover = abilityRowHovered(gui); hover >= 0) {
            state.abilityFocus = hover;
        }
        bindAbilityFocus(gui, game, scores, state.abilityFocus, false);
        return;
    }

    if (state.step == 3) {
        const auto skills = chargenTable(game.install(), "skills");
        gui.setText("REMAINING_SELECTIONS_LBL", std::to_string(chargen.skillPointsRemaining()));
        gui.setText("SELECTIONS_REMAINING_LBL", tlk(210));
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t nameRef = chargenStrref(skills, static_cast<std::size_t>(i), "name");
            gui.setText(std::string(kSkillBase[i]) + "_LBL",
                        nameRef != UINT32_MAX ? tlk(nameRef) : game.rules().skillName(i));
            gui.setText(std::string(kSkillBase[i]) + "_POINTS_BTN", std::to_string(chargen.skillRank(i)));
            // 006f51d0 (chargen): the minus shows above rank 0, the plus below the rank cap.
            gui.setVisible(std::string(kSkillShort[i]) + "_MINUS_BTN", chargen.skillRank(i) != 0);
            gui.setVisible(std::string(kSkillShort[i]) + "_PLUS_BTN", chargen.skillRank(i) != chargen.skillRankCap(i));
        }
        // 006f51d0 focuses the first row at open; 006f4bf0 follows the pointer.
        if (const int hover = skillRowHovered(gui); hover >= 0) {
            state.skillFocus = hover;
        }
        bindSkillFocus(gui, game, chargen.classId(), state.skillFocus);
        return;
    }

    const auto feats = chargenTable(game.install(), "feat");
    const std::vector<std::int32_t> available = chargen.availableFeats();
    const std::set<std::int32_t> owned = chargen.ownedFeats();
    // Retail 006f30f0: out of reach unless a list says otherwise.
    const auto status = [&](std::int32_t id) {
        if (owned.contains(id)) {
            return 1;
        }
        if (chargen.featSelected(id)) {
            return 4;
        }
        return std::find(available.begin(), available.end(), id) != available.end() ? 0 : 3;
    };
    bindFeatGrid(gui, game, chargen.classId(), owned, status);
    gui.setText("STD_REMAINING_SELECTIONS_LBL", std::to_string(chargen.featPointsRemaining()));
    gui.setText("STD_SELECTIONS_REMAINING_LBL", tlk(32062));
    const std::int32_t pickedId = gui.listCellValue("LB_FEATS");
    const bool picked = pickedId >= 0;
    gui.setVisible("BTN_SELECT", picked);
    gui.setVisible("LBL_NAME", picked);
    gui.setText("DESC_LBL", tlk(217)); // the file's own "Description"; the name goes on LBL_NAME (paired frame)
    if (picked) {
        const std::int32_t id = pickedId;
        const std::uint32_t nameRef = chargenStrref(feats, static_cast<std::size_t>(id), "name");
        gui.setText("LBL_NAME", nameRef != UINT32_MAX ? tlk(nameRef) : game.rules().featName(id));
        // Retail 006f3d60 builds "Add Feat" and "Remove Feat" (38455, 38456, 42487); 006f2fb0 picks by status:
        // 0 adds, a feat picked here removes, the others show their word dimmed.
        const int cellStatus = status(id);
        gui.setText("BTN_SELECT", tlk(cellStatus == 4 || cellStatus == 1 ? 38456 : 38455) + " " + tlk(42487));
        gui.setEnabled("BTN_SELECT", cellStatus == 0 || cellStatus == 4);
        const std::uint32_t descRef = chargenStrref(feats, static_cast<std::size_t>(id), "description");
        GuiListItem item;
        item.text = descRef != UINT32_MAX ? tlk(descRef) : std::string{};
        item.selectable = false;
        gui.setItems("LB_DESC", {item});
    } else {
        gui.setItems("LB_DESC", {});
    }
}

void bindFeatGrid(GuiSystem& gui, GameSession& game, std::int32_t classId, const std::set<std::int32_t>& owned,
                  const std::function<int(std::int32_t)>& status, const char* list, bool ownedOnly)
{
    const auto feats = chargenTable(game.install(), "feat");
    const Rules& rules = game.rules();
    const auto number = [&](std::int32_t id, const char* column) {
        if (!feats) {
            return -1;
        }
        const std::string cell = feats->cell(feats->rowLabel(static_cast<std::size_t>(id)), column);
        return !cell.empty() && cell != "****" ? std::atoi(cell.c_str()) : -1;
    };
    const auto known = [&](std::int32_t id) { return owned.contains(id) || (!ownedOnly && rules.featIsAvailable(classId, id)); };
    const auto makeCell = [&](std::int32_t id) {
        GuiListCell cell;
        cell.value = id;
        if (feats) {
            cell.icon = feats->cell(feats->rowLabel(static_cast<std::size_t>(id)), "icon");
        }
        cell.status = status(id);
        return cell;
    };
    std::vector<GuiListItem> rows;
    for (std::int32_t id = 0; id < rules.featCount(); ++id) {
        if (number(id, "prereqfeat1") != -1 || !known(id)) {
            continue;
        }
        GuiListItem row;
        row.value = id;
        row.cells.resize(3);
        row.cells[0] = makeCell(id);
        for (std::int32_t child = 0; child < rules.featCount(); ++child) {
            if (number(child, "prereqfeat1") == id && known(child)) {
                row.cells[number(child, "prereqfeat2") == -1 ? 1 : 2] = makeCell(child);
            }
        }
        rows.push_back(std::move(row));
    }
    const GuiControlState before = gui.state(list);
    const std::size_t count = rows.size();
    gui.setItems(list, std::move(rows));
    gui.setRowHeight(list, kFeatRowHeight);
    if (before.listCell >= 0 && before.listSelection < count) {
        gui.setListCell(list, before.listSelection, before.listCell);
    } else if (count > 0) {
        // The first cell of the first row, owned or not: the paired retail frames select Two-Weapon Fighting
        // both in chargen (takeable) and at level up (owned, "Remove Feat" greyed).
        gui.setListCell(list, 0, 0);
    }
}

bool handleChargenGuiAction(const std::string& action, ChargenGuiState& state, GuiSystem& gui,
                            GameSession& game)
{
    if (!state.active) {
        return false;
    }
    const auto tlk = [&](std::uint32_t strref) { return game.localization().tlk().text(strref); };
    const auto openHub = [&](int panel) {
        state.step = kChargenHub;
        state.hubPanel = panel;
        gui.open(chargenHubScreen(gui, panel));
    };
    // 006ea760(1) then 006eb010: back to Quick or Custom, the build cleared and its labels blanked.
    const auto backToChoice = [&] {
        state.chargen.resetBuild();
        state.statsShown = false;
        state.quickStep = 0;
        state.customStep = 0;
        state.hubFocus = "QUICK_CHAR_BTN";
        openHub(kHubChoice);
    };
    const auto finish = [&] {
        // 006eb320 -> 006dbdf0: Play starts the game with the character as built.
        state.chargen.setName(state.nameBuffer);
        const CharacterSheet sheet = state.chargen.build();
        // The sheet carries gender and portrait row; the session derives the body
        // (appearance.2da by class size) from them.
        game.applyCharacter(sheet);
        game.setPlayerAppearance(static_cast<std::uint32_t>(std::max(0, state.chargen.appearanceId())),
                                 state.chargen.name());
        game.setThirdPerson(true);
        state.active = false;
        state.finished = true;
        gui.close();
    };
    if (gui.modalActive()) {
        if (state.cancelConfirm) {
            if (action == "BTN_OK" || action == "BTN_CANCEL") {
                gui.popModal();
                state.cancelConfirm = false;
                if (action == "BTN_OK") {
                    backToChoice(); // 006ef5b0 on yes
                }
                return true;
            }
            return action == "LB_MESSAGE";
        }
        if (action == "BTN_OK") {
            gui.popModal();
            return true;
        }
        return action == "LB_SKILLS";
    }
    const auto open = [&](int step) {
        state.step = step;
        state.abilityFocus = 0; // 006f7600 and 006f51d0 focus the first row when the screen opens
        state.skillFocus = 0;
        gui.open(chargenScreenName(step));
        if (step == 4) {
            // 006f3460, the feats panel's show: the creature's feats are cleared (00648320) and FUN_00649950 grants the
            // class's feats for its level again into a list; when that list is not empty it opens skillinfo
            // (FUN_006cdfc0) with strref 0xa512 above it. At level 1 that is every feat whose <class>_list is 3 and
            // <class>_granted is 1 (005bd0f0 keeps list 3 as the granted kind, 005be310 reads the level), in row
            // order; the box shows three rows and scrolls (paired walk 0057_cg11: Armour Proficiency Light,
            // Critical Strike, Sniper Shot on top for a Scoundrel).
            std::vector<std::int32_t> granted;
            if (const auto feats = chargenTable(game.install(), "feat")) {
                const std::string prefix = game.rules().classColumnPrefix(state.chargen.classId());
                const int listColumn = feats->columnIndex(prefix + "_list");
                const int levelColumn = feats->columnIndex(prefix + "_granted");
                for (std::size_t row = 0; !prefix.empty() && listColumn >= 0 && levelColumn >= 0 && row < feats->rowCount(); ++row) {
                    if (feats->cell(row, static_cast<std::size_t>(listColumn)) == "3"
                        && feats->cell(row, static_cast<std::size_t>(levelColumn)) == "1") {
                        granted.push_back(static_cast<std::int32_t>(row));
                    }
                }
            }
            if (!granted.empty()) {
                bindChargenScreen(gui, state, game); // the screen below is bound before the box covers it
                showFeatPopup(gui, game, granted, 42258);
            }
        }
        // Paired frame cg_name: the screen opens on a generated name (retail 006f9bf0).
        if (step == 5 && state.nameBuffer == "Player") {
            if (std::string name = chargenRandomName(game, state.chargen.gender()); !name.empty()) {
                state.nameBuffer = std::move(name);
            }
        }
    };
    const bool quick = state.hubPanel == kHubQuick;
    // A step screen's Accept: back to the hub with the step after it lit (quick 006eff60 / 006eff80,
    // custom 006ef4c0, 006ef500, 006ef520, 006ef5c0, 006ef4e0). Each sets the step outright, so a step
    // taken again from later on lights the one after it again.
    const auto accepted = [&](int customNext, int quickNext) {
        if (quick) {
            state.quickStep = quickNext;
        } else {
            state.customStep = customNext;
        }
        openHub(quick ? kHubQuick : kHubCustom);
    };
    if (state.step == 0) {
        // BTN_SEL1..6: the six class/gender slots (retail 006dc3c0), then MAINCG with Quick or Custom.
        if (action.rfind("BTN_SEL", 0) == 0 && action.size() == 8) {
            const int index = action[7] - '1';
            if (index >= 0 && index < Chargen::kSlotCount) {
                state.chargen.selectSlot(index);
                backToChoice();
            }
            return true;
        }
        if (action == "BTN_BACK") {
            state.active = false;
            gui.close();
            return true;
        }
        return false;
    }
    if (state.step == kChargenHub) {
        if (state.hubPanel == kHubChoice) {
            if (action == "QUICK_CHAR_BTN") {
                // QUICKPNL's show (006f0260 -> 006effd0): the class's recommended scores, feats and skills,
                // then 006eac30 fills MAINCG.
                state.chargen.applyRecommended(Chargen::Step::Abilities);
                state.chargen.applyRecommended(Chargen::Step::Feats);
                state.chargen.applyRecommended(Chargen::Step::Skills);
                state.statsShown = true;
                openHub(kHubQuick);
                return true;
            }
            if (action == "CUST_CHAR_BTN") {
                openHub(kHubCustom); // CUSTPNL's show is the default one: nothing is applied
                return true;
            }
            if (action == "BTN_BACK") {
                state.chargen.resetBuild();
                state.statsShown = false;
                state.step = 0;
                gui.open("classsel");
                return true;
            }
            return action == "LB_DESC";
        }
        const int at = quick ? state.quickStep : state.customStep;
        if (action.rfind("BTN_STEPNAME", 0) == 0 && action.size() == 13) {
            const int row = action[12] - '1';
            // Each step's handler acts only once the steps before it are done (step >= row).
            if (row < 0 || at < row) {
                return true;
            }
            static constexpr int quickScreens[3] = {1, 5, -1};
            static constexpr int customScreens[6] = {1, 2, 3, 4, 5, -1};
            const int screen = quick ? (row < 3 ? quickScreens[row] : -2) : (row < 6 ? customScreens[row] : -2);
            if (screen == -1) {
                finish();
            } else if (screen > 0) {
                open(screen);
            }
            return true;
        }
        if (action == "BTN_BACK") {
            // 0040b650 sends the panel key 0x28: 006f0280 / 006ef610 step back, from the first to Quick or Custom.
            if (at == 0) {
                backToChoice();
                return true;
            }
            if (quick) {
                --state.quickStep;
            } else if (--state.customStep == 1) {
                state.chargen.resetBuild(); // 006ef540
                state.statsShown = false;
            }
            return true;
        }
        if (action == "BTN_CANCEL") {
            // 006ef6a0 / 006f0310: on the first step straight back, later after a yes to strref 48541.
            if (at == 0) {
                backToChoice();
            } else if (gui.pushModal("confirm")) {
                state.cancelConfirm = true;
                GuiListItem line;
                line.text = tlk(48541);
                line.selectable = false;
                gui.setItems("LB_MESSAGE", {line});
                gui.setText("BTN_OK", tlk(1580));
                gui.setText("BTN_CANCEL", tlk(1581));
            }
            return true;
        }
        return false;
    }
    if (action == "BTN_BACK") {
        // A step screen's own Back leaves it unchanged for the hub.
        openHub(quick ? kHubQuick : kHubCustom);
        return true;
    }
    if (state.step == 1) {
        if (action == "BTN_ARRL") {
            state.chargen.cyclePortrait(-1);
            return true;
        }
        if (action == "BTN_ARRR") {
            state.chargen.cyclePortrait(1);
            return true;
        }
        if (action == "BTN_ACCEPT") {
            accepted(1, 1);
            return true;
        }
        return false;
    }
    if (state.step >= 2 && state.step <= 4 && action == "BTN_RECOMMENDED") {
        state.chargen.applyRecommended(state.step == 2 ? Chargen::Step::Abilities
                                                       : (state.step == 3 ? Chargen::Step::Skills : Chargen::Step::Feats));
        if (state.step == 4) {
            // Retail 006f3a80: FUN_006cdfc0 fills skillinfo with the picks, strref 0xa510 above them.
            const std::set<std::int32_t>& picks = state.chargen.selectedFeats();
            showFeatPopup(gui, game, std::vector<std::int32_t>(picks.begin(), picks.end()), 42256);
        }
        return true;
    }
    if (state.step == 2) {
        static const char* names[] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
        for (int i = 0; i < 6; ++i) {
            if (action == std::string(names[i]) + "_PLUS_BTN") {
                state.chargen.adjustAttribute(i, 1);
                return true;
            }
            if (action == std::string(names[i]) + "_MINUS_BTN") {
                state.chargen.adjustAttribute(i, -1);
                return true;
            }
        }
        if (action == "BTN_ACCEPT") {
            state.statsShown = true; // 006ef500 -> 006eac30
            accepted(2, state.quickStep);
            return true;
        }
        return false;
    }
    if (state.step == 3) {
        static const char* names[] = {"COM", "DEM", "STE", "AWA", "PER", "REP", "SEC", "TRE"};
        for (int i = 0; i < 8; ++i) {
            if (action == std::string(names[i]) + "_PLUS_BTN") {
                state.chargen.adjustSkill(i, 1);
                return true;
            }
            if (action == std::string(names[i]) + "_MINUS_BTN") {
                state.chargen.adjustSkill(i, -1);
                return true;
            }
        }
        if (action == "BTN_ACCEPT") {
            accepted(3, state.quickStep);
            return true;
        }
        return false;
    }
    if (state.step == 4) {
        if (action == "BTN_SELECT") {
            // The picked icon is the feat.
            if (gui.state("LB_FEATS").items.empty()) {
                bindChargenScreen(gui, state, game); // a flow driven without a window has not bound it yet
            }
            if (const int feat = gui.listCellValue("LB_FEATS"); feat >= 0) {
                state.chargen.toggleFeat(feat);
            }
            return true;
        }
        if (action == "LB_FEATS") {
            return true;
        }
        if (action == "BTN_ACCEPT") {
            accepted(4, state.quickStep);
            return true;
        }
        return false;
    }
    if (state.step == 5) {
        if (action == "BTN_RANDOM") {
            if (std::string name = chargenRandomName(game, state.chargen.gender()); !name.empty()) {
                state.nameBuffer = std::move(name);
                state.nameRefused = false;
            }
            return true;
        }
        if (action == "END_BTN") {
            // Trailing spaces go (006f9cd0 strips them); nothing left is refused.
            while (!state.nameBuffer.empty() && state.nameBuffer.back() == ' ') {
                state.nameBuffer.pop_back();
            }
            if (state.nameBuffer.empty()) {
                state.nameRefused = true;
                return true;
            }
            // 006eef70 -> 006ea9b0 puts the name on MAINCG; the hub lights Play (quick 2, custom 5).
            state.chargen.setName(state.nameBuffer);
            state.nameShown = true;
            accepted(5, 2);
            return true;
        }
        return false;
    }
    return false;
}

const kotor::Image* PortraitImages::image(const std::string& resref)
{
    if (resref.empty()) {
        return nullptr;
    }
    auto found = cache_.find(resref);
    if (found == cache_.end()) {
        std::optional<kotor::Image> decoded;
        std::uint32_t type = 0;
        if (auto data = install_.findTexture(resref, &type); data.has_value()) {
            try {
                decoded = kotor::decodeTexture(*data, type, resref);
                if (type == static_cast<std::uint32_t>(kotor::ResType::Tpc)) {
                    // TPC rows are stored bottom-up (the scene shaders flip V); a UI image
                    // is sampled top-down, so turn the rows over once here.
                    const std::size_t row = static_cast<std::size_t>(decoded->width) * 4u;
                    for (int y = 0; y < decoded->height / 2; ++y) {
                        std::swap_ranges(
                            decoded->rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
                            decoded->rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                            decoded->rgba.begin()
                                + static_cast<std::ptrdiff_t>((decoded->height - 1 - y) * row));
                    }
                }
            } catch (const kotor::FormatError&) {
                decoded.reset();
            }
        }
        found = cache_.emplace(resref, std::move(decoded)).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

std::uint64_t PortraitImages::digest(const std::string& resref)
{
    const kotor::Image* picture = image(resref);
    if (picture == nullptr) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : picture->rgba) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(picture->width) << 32 | static_cast<std::uint32_t>(picture->height);
    return hash == 0 ? 1 : hash;
}

} // namespace ke
