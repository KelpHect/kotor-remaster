#pragma once

// The shipped character-generation screens (classsel, portcust, abchrgen, skchrgen,
// ftchrgen, name) driven by GuiSystem actions. The window loop and the headless
// gate share this state machine; both feed it the same BTN_* / *_PLUS_BTN actions
// that a pointer click on the control produces.

#include "game/Chargen.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"
#include "kotor/Formats.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace ke {

// The MAINCG hub (retail 006eb420): the main panel with one of its three sub-panels laid in at the
// sub-panel's own extent, painted and hit-tested as one screen.
inline constexpr int kChargenHub = 6;
// Retail 006ea760's panel numbers: 1 QORCPNL (Quick or Custom), 2 QUICKPNL, 3 CUSTPNL.
inline constexpr int kHubChoice = 1;
inline constexpr int kHubQuick = 2;
inline constexpr int kHubCustom = 3;

struct ChargenGuiState {
    explicit ChargenGuiState(const Rules& rules) : chargen(rules) {}
    Chargen chargen;
    int step = 0; // classsel, portcust, abchrgen, skchrgen, ftchrgen, name, or kChargenHub
    int hubPanel = kHubChoice;
    int quickStep = 0;  // QUICKPNL +0x135c (006efc10): the step that is lit, 0..2
    int customStep = 0; // CUSTPNL +0x2184 (006eefd0): 0..5
    std::string hubFocus = "QUICK_CHAR_BTN"; // QORCPNL's focused button (006f09f0 focuses Quick)
    bool statsShown = false;  // 006eac30 has filled the six scores, vitality, defence and saves
    bool nameShown = false;   // 006ea9b0 has put the name on LBL_NAME
    bool cancelConfirm = false; // 006ef6a0's "Are you sure you want to cancel?" is up
    std::string nameBuffer = "Player";
    bool nameRefused = false; // END_BTN with a blank name
    int abilityFocus = 0;     // the ability row whose help the screen shows
    int skillFocus = 0;       // the skill row likewise (006f51d0 focuses the first)
    int classFocus = 0;       // the class slot whose gender, class and line classsel shows
    bool active = true;
    // The moment the flow finished and handed the character to the session.
    bool finished = false;
};

const char* chargenScreenName(int step);
// The hub's screen name for a sub-panel ("maincg_qorcpnl" ...), built from the shipped files on first use.
std::string chargenHubScreen(GuiSystem& gui, int panel);
// MAINCG with any of its sub-panels (also LEVELUPPNL for the level-up hub, 006e8ef0): "maincg_<sub>".
std::string maincgComposite(GuiSystem& gui, const std::string& sub);
// Hides the MAINCG labels the chargen and level-up panel classes never bind (old/new columns, arrows).
void hideMaincgUnbound(GuiSystem& gui);
// Shows `count` numbered step rows of a QUICKPNL, CUSTPNL or LEVELUPPNL laid into MAINCG, row `lit` lit
// as retail's 006efc10 / 006eefd0 do, and Back greyed on the first step.
void bindHubSteps(GuiSystem& gui, int count, int lit);
// LEVELUPPNL's five rows (006ee7d0: attributes, skills, feats, powers, accept): hidden rows lose LBL_n, LBL_NUMn
// and BTN_STEPNAMEn, the shown ones are numbered 1.. in order, row `lit` lit as 006ee0e0 does. Back is greyed and
// off on the first shown row (006ee6c0), and Cancel stays hidden outside chargen's own level-up (+0x1ce8 == 0).
void bindLevelUpSteps(GuiSystem& gui, const std::array<bool, 5>& rows, int lit, bool backGreyed);
// MAINCG's labels for a finished sheet: name, class, level, portrait, scores, vitality, defence and saves
// (006e7ec0 and 006e7f70 in level-up mode, 006e8ef0 binding LBL_LEVEL and LBL_LEVEL_VAL).
void bindLevelUpMaincg(GuiSystem& gui, GameSession& game, const CharacterSheet& sheet);
// The ability screen's focused row (retail 006f70e0): its label and score in 007a23c0, the others in
// 007a23b4, the point cost (1 in level-up, else 1/2/3 by the score) and the modifier ("-" for 0), and its
// help in LB_DESC under the file's "Description". `scores` run STR DEX CON INT WIS CHA.
void bindAbilityFocus(GuiSystem& gui, GameSession& game, const std::array<std::int32_t, 6>& scores, int focus,
                      bool levelUp);
// The skill screen's focused row (retail 006f4bf0): colours as above, Class Skill or Cross-class Skill on
// CLASSSKL_LBL (38152 / 38153), the rank cost on COST_POINTS_LBL and the skill's description in LB_DESC.
void bindSkillFocus(GuiSystem& gui, GameSession& game, std::int32_t classId, int focus);
// The row under the pointer on abchrgen (0..5) or skchrgen (0..7), -1 when none: retail's focus event 0.
int abilityRowHovered(const GuiSystem& gui);
int skillRowHovered(const GuiSystem& gui);
// A first and a last name from the shipped letter tables, empty if they cannot be read (retail 006f9bf0).
std::string chargenRandomName(GameSession& game, int gender);

// The name screen is the shipped `name` screen with a text box typed into by the keyboard (the
// application sends SDL text and key events here while step 5 is up). The box holds at most
// kChargenNameMax characters (the edit box object of retail 006f9e70 keeps 0x12 at the end of its
// layout; read from the layout, not from a use). The characters accepted are retail 004184f0's.
constexpr std::size_t kChargenNameMax = 18;
bool chargenNameEditing(const ChargenGuiState& state);
void chargenNameInput(ChargenGuiState& state, const char* utf8);
void chargenNameBackspace(ChargenGuiState& state);

// Fills the ability, skill and feat screens (abchrgen, skchrgen, ftchrgen) from the flow's
// state: the points left, each score or rank, the feat list with its description. Called
// once per frame before the screen is painted; other steps are untouched.
void bindChargenScreen(GuiSystem& gui, ChargenGuiState& state, GameSession& game);
// The feat screen's rows (retail 006ce570): one row per base feat the class knows, with the feats that build on it
// in the second and third cells; `status` gives each feat's cell status (GuiListCell). Sets the row height, and
// the first takeable cell as the pick when none is picked. Chargen and level-up share it (006f3d60).
void bindFeatGrid(GuiSystem& gui, GameSession& game, std::int32_t classId, const std::set<std::int32_t>& owned,
                  const std::function<int(std::int32_t)>& status, const char* list = "LB_FEATS", bool ownedOnly = false);

// Returns true when the action belonged to the chargen flow.
bool handleChargenGuiAction(const std::string& action, ChargenGuiState& state, GuiSystem& gui,
                            GameSession& game);

// Decoded portrait art (portraits.2da BaseResRef*, TPC or TGA via the VFS), cached by
// resref. Used by the window overlay (uploaded as a UI image) and the headless gate.
class PortraitImages {
public:
    explicit PortraitImages(kotor::Install& install) : install_(install) {}
    // Null when the resref names no texture in the install.
    const kotor::Image* image(const std::string& resref);
    // A stable digest of the decoded pixels, 0 when missing; lets a gate compare art.
    std::uint64_t digest(const std::string& resref);

private:
    kotor::Install& install_;
    std::map<std::string, std::optional<kotor::Image>> cache_;
};

} // namespace ke
