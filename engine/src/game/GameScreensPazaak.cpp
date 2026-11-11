#include "game/GameScreens.h"

#include "core/Log.h"
#include "game/AudioSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

// The Pazaak screens PlayPazaak (action 364) opens: pazaakwager (how much), pazaaksetup (which of the
// cards owned make the side deck) and pazaakgame (the table). The session keeps the model
// (GameSession::pazaak*, Minigames.h); this file binds the shipped .gui files to it, as the other
// GameScreens*.cpp do. Retail: CSWGuiWagerPopup 0067f000, CSWGuiPazaakStart 00681a90 and
// CSWGuiPazaakGame 006808a0 with RefreshDisplay 0067d4b0 and CSWGuiPazaakCard::SetCard 0067cd30.

namespace ke {

namespace {

constexpr int kHandCards = 4;
constexpr int kTableCells = 9;
constexpr kotor::Vec3 kWhite{1.0f, 1.0f, 1.0f};
constexpr kotor::Vec3 kGrey{0.667f, 0.667f, 0.667f}; // 0x3f2ac083 (RefreshDisplay, OnPanelAdded 0067f750)

// SetCard 0067cd30: the art per kind.
std::string cardFill(const PazaakCard& card)
{
    const int kind = card.kind;
    if (kind >= 0 && kind <= 5) {
        return "lbl_cardmpos";
    }
    if (kind >= 6 && kind <= 11) {
        return "lbl_cardmneg";
    }
    if (kind >= 12 && kind <= 17) {
        return card.flipped == 0 ? "lbl_cardrarem" : "lbl_cardraref";
    }
    if (kind >= 18 && kind <= 27) {
        return "lbl_cardstand"; // the main deck's cards
    }
    if (kind == kPazaakNoCard) {
        return {};
    }
    return "lbl_cardback"; // face down
}

// SetCard's text: 007a2600 "+1".."+6", "-1".."-6", the main deck "1".."10"; a +/- card reads its side.
std::string cardText(const PazaakCard& card)
{
    if (!card.present()) {
        return {};
    }
    const int value = card.value();
    if (card.main()) {
        return std::to_string(value);
    }
    return (value >= 0 ? "+" : "") + std::to_string(value);
}

std::string cell(const char* prefix, int index)
{
    return std::string(prefix) + std::to_string(index);
}

} // namespace

std::string GameScreens::pazaakScreenName(const GameSession& game)
{
    switch (game.pazaakStage()) {
    // The wager popup is not a screen of its own: the start panel (00681a90) is up under it and
    // OnPanelAdded 0067f750 adds the popup (+0x75f4) over it (0067f795).
    case GameSession::PazaakStage::Wager: return "pazaaksetup";
    case GameSession::PazaakStage::Setup: return "pazaaksetup";
    case GameSession::PazaakStage::Game: return "pazaakgame";
    case GameSession::PazaakStage::None: break;
    }
    return {};
}

// The checks run on fixed frames: one frame stands for 0.4 s / frames of play.
void GameScreens::setPazaakOpponentDelay(int frames)
{
    pazaakDelay_ = std::max(1, frames);
    pazaakFixedStep_ = 0.4f / static_cast<float>(pazaakDelay_);
}

// A message over the screen: the set and match results (no Cancel), the side deck question, the quit
// questions and the tutorial help, in the shipped confirm.gui (OK over Cancel), fitted to the text as
// retail's FixMessageLabel 006253a0 does (below).
void GameScreens::openPazaakPopup(int kind)
{
    if (kind == 10) {
        // CSWGuiWagerPopup 0067f000 over the side deck panel.
        if (!gui_.pushModal("pazaakwager")) {
            return;
        }
        pazaakPopup_ = 10;
        refreshPazaakWager();
        return;
    }
    PazaakHelp* help = kind == 9 ? pazaakHelp() : nullptr;
    if (kind == 9 && help == nullptr) {
        return;
    }
    const bool ask = kind == 2 || kind == 4 || kind == 6 || kind == 7 || (help != nullptr && help->twoButtons());
    const std::string alias = help != nullptr ? "pazaak_help" : (ask ? "pazaak_ask" : "pazaak_notice");
    // The help box goes next to its control (PlaceRightOf 0067d110, PlaceAbove 0067d1c0): the control's
    // place on the table panel, the panel's own offset added.
    std::array<int, 4> anchorRect{};
    bool anchored = false;
    if (help != nullptr && help->anchor() != PazaakHelp::Anchor::None && gui_.screen() != nullptr) {
        const char* tag = help->anchor() == PazaakHelp::Anchor::RightOfPlayerCard ? "BTN_PLR0"
            : (help->anchor() == PazaakHelp::Anchor::AbovePlayerHand ? "BTN_PLRSIDE0" : "BTN_XTEXT");
        if (const kotor::GuiControl* control = gui_.control(tag)) {
            const auto& panel = gui_.screen()->extent;
            anchorRect = {panel.left + control->extent.left, panel.top + control->extent.top,
                          control->extent.width, control->extent.height};
            anchored = true;
        }
    }
    kotor::GuiScreen* box = gui_.variant("confirm", alias);
    if (box == nullptr) {
        return;
    }
    std::string text = pazaakPopupText(kind);
    // ShowHelp 0067dfc0 / ShowMultipleHelp 0067e0b0 set the labels; -1 keeps the shipped one.
    std::string okLabel = tlk(1580);
    std::string cancelLabel = tlk(1581);
    if (help != nullptr && help->okText() >= 0) {
        okLabel = tlk(static_cast<std::uint32_t>(help->okText()));
    }
    if (help != nullptr && help->cancelText() >= 0) {
        cancelLabel = tlk(static_cast<std::uint32_t>(help->cancelText()));
    }
    std::string font;
    for (const kotor::GuiControl& control : box->controls) {
        if (control.key == "LB_MESSAGE/PROTOITEM" && !control.font.empty()) {
            font = control.font;
        } else if (control.tag == "LB_MESSAGE" && font.empty()) {
            // The PROTOITEM (dialogfont16x16) is the list's prototype row, not one of its children.
            for (const kotor::GuiControl& proto : control.proto) {
                if (!proto.font.empty()) {
                    font = proto.font;
                }
            }
            for (const kotor::GuiControl& child : control.children) {
                if (child.tag == "PROTOITEM" && !child.font.empty()) {
                    font = child.font;
                }
            }
            if (font.empty()) {
                font = control.font;
            }
        }
    }
    // FixMessageLabel 006253a0 from confirm.gui as shipped (290x87, LB_MESSAGE 278x20, PROTOITEM 258x16):
    // with Cancel hidden the box loses Cancel's 22 + 2; the buttons widen by 10 until their label fits
    // one line; the text is laid out at the PROTOITEM width, 16 a line, and while the lines do not fit
    // the list the box grows a line (or 40 wider once over 160 tall). AddPanel (0040bc70) then centres
    // it on the 640x480 panel. Paired frames req_pazaak_set_result (290x79 at y 200: "The opponent wins
    // the set." is 261 wide, two lines at 258), req_pazaak_match_result (290x63), req_pazaak_match_esc
    // (290x103) and req_pazaak_wager_esc (290x119).
    constexpr int kItemLine = 16;
    int width = 290;
    int height = ask ? 87 : 87 - 24;
    int listWidth = 278;
    int itemWidth = 258;
    int listHeight = 20;
    const float line = std::max(1.0f, painter_.measureText(font, "Ay", 1000.0f));
    const auto lines = [&](const std::string& s, int w) {
        return std::max(1, static_cast<int>(std::lround(painter_.measureText(font, s, static_cast<float>(w)) / line)));
    };
    int buttonWidth = 100;
    for (int b = 0; b < (ask ? 2 : 1); ++b) {
        const std::string& label = b == 0 ? okLabel : cancelLabel;
        if (label.empty()) {
            continue;
        }
        while (buttonWidth < 600 && lines(label, buttonWidth) > 1) {
            buttonWidth += 10;
        }
    }
    width = std::max(width, buttonWidth - 2);
    for (int guard = 0; guard < 64; ++guard) {
        if (lines(text, itemWidth) * kItemLine <= listHeight || (width > 0x1b7 && height > 0x117)) {
            break;
        }
        if (height > 0xa0 && width < 0x1b8) {
            width += 0x28;
            listWidth += 0x28;
            itemWidth += 0x28;
        } else {
            height += kItemLine;
            listHeight += kItemLine;
        }
    }
    int left = (640 - width) / 2;
    int top = (480 - height) / 2;
    const int grown = listHeight - 20;
    if (anchored) {
        const std::array<int, 2> at = PazaakHelp::placeNear(help->anchor(), anchorRect[0], anchorRect[1],
                                                            anchorRect[2], anchorRect[3], width, height);
        left = at[0];
        top = at[1];
    }
    box->extent = {left, top, width, height};
    for (kotor::GuiControl& control : box->controls) {
        if (control.tag == "LB_MESSAGE") {
            control.extent = {6, 10, listWidth, listHeight};
            // Retail measures at the PROTOITEM width but draws the line across the list: 261 px of "The
            // opponent wins the set." on one line in a box grown for two (req_pazaak_set_result).
            if (!control.proto.empty()) {
                control.proto.front().extent.width = listWidth - 4;
            }
            // 006253a0 hands the list its scroll state back (FUN_004181f0 with the saved +0x1e3) only
            // when the rows still overflow after the box has grown; every retail box shows no
            // scrollbar (req_pazaak_set_result, _match_result, _wager_esc, _match_esc).
            if (lines(text, itemWidth) * kItemLine <= listHeight) {
                std::erase_if(control.children, [](const kotor::GuiControl& child) { return child.type == 9; });
            }
        } else if (control.tag == "BTN_OK") {
            control.extent = {(width - buttonWidth) / 2, 34 + grown, buttonWidth, 22};
        } else if (control.tag == "BTN_CANCEL") {
            control.extent = {(width - buttonWidth) / 2, 58 + grown, buttonWidth, 22};
        }
    }
    if (!gui_.pushModal(alias)) {
        return;
    }
    GuiListItem item;
    item.text = text;
    item.selectable = false;
    gui_.setItems("LB_MESSAGE", {item});
    gui_.setText("BTN_OK", okLabel);
    gui_.setText("BTN_CANCEL", cancelLabel);
    gui_.setVisible("BTN_CANCEL", ask);
    pazaakPopup_ = kind;
}

PazaakHelp* GameScreens::pazaakHelp()
{
    if (game_.pazaakStage() == GameSession::PazaakStage::Game) {
        PazaakGame* game = game_.pazaak();
        return game != nullptr ? const_cast<PazaakHelp*>(&game->help()) : nullptr;
    }
    if (game_.pazaakStage() != GameSession::PazaakStage::None) {
        return &game_.pazaakSetupHelp();
    }
    return nullptr;
}

// After OK or Cancel on the help box: the next page, or the box closed and the panel's close handler
// ran (HandleTutorialClose 0067ea10 at the table, nothing on the side deck panel).
void GameScreens::pazaakHelpClosed()
{
    PazaakHelp* help = pazaakHelp();
    if (help != nullptr && help->open()) {
        openPazaakPopup(9);
        return;
    }
    game_.resolvePazaakIfOver();
}

std::string GameScreens::pazaakPopupText(int kind)
{
    std::string text;
    if (kind == 1) {
        const PazaakGame* game = game_.pazaak();
        const PazaakGame::SetResult result = game != nullptr ? game->lastSet() : PazaakGame::SetResult::None;
        // DoGameSequence state 8: 0x7e4e set won, 0x7e4f set lost, 0x7e52 tie.
        text = tlk(result == PazaakGame::SetResult::Player ? 32334
                   : (result == PazaakGame::SetResult::Opponent ? 32335 : 32338));
    } else if (kind == 2) {
        text = tlk(32322); // 0x7e42 (006819e0)
    } else if (kind == 5) {
        text = tlk(mapNote_); // the map's refusal, retail 00692bc0
    } else if (kind == 4) {
        // The map's Return button asks the question SetReturnStrref named (retail 00692bc0).
        const std::int32_t query = game_.presentation().returnQueryStrref;
        text = tlk(static_cast<std::uint32_t>(query > 0 ? query : 38451));
    } else if (kind == 6) {
        text = tlk(42424); // 0xa5b8: leave before the table (0067e150, 006816f0)
    } else if (kind == 7) {
        text = tlk(42425); // 0xa5b9: forfeit at the table (0067e8f0)
    } else if (kind == 8) {
        text = tlk(38630); // 0x96e6: Play with fewer than ten cards (006819e0)
    } else if (kind == 9) {
        const PazaakHelp* help = pazaakHelp();
        text = help != nullptr ? tlk(static_cast<std::uint32_t>(help->message())) : std::string{};
    } else {
        const PazaakGame* game = game_.pazaak();
        text = tlk(game != nullptr && game->playerWonMatch() ? 32336 : 32337); // 0x7e50 / 0x7e51
    }
    return text;
}

void GameScreens::refreshPazaakWager()
{
    // 0067f000: the wager opens at the maximum; LBL_MAXIMUM reads 0x7e41 max, a new line, 0x96c8 gold.
    const int maximum = game_.pazaakMaxWager();
    gui_.setText("LBL_WAGERVAL", std::to_string(game_.pazaakWager()));
    gui_.setText("LBL_MAXIMUM", tlk(32321) + " " + std::to_string(maximum) + "\n" + tlk(38600) + " "
                                    + std::to_string(game_.gold()));
}

bool GameScreens::actionPazaakPopup(const std::string& tag)
{
    const int kind = pazaakPopup_;
    if (kind == 10) {
        if (tag == "BTN_LESS") {
            game_.setPazaakWager(game_.pazaakWager() - game_.pazaakWagerStep());
            refreshPazaakWager();
        } else if (tag == "BTN_MORE") {
            game_.setPazaakWager(game_.pazaakWager() + game_.pazaakWagerStep());
            refreshPazaakWager();
        } else if (tag == "BTN_WAGER") {
            gui_.popModal();
            pazaakPopup_ = 0;
            key_.clear();
            game_.confirmPazaakWager(); // HandleWagerExit 0067d3b0
        } else if (tag == "BTN_QUIT") {
            // HandleQuitDialog 0067d3d0 adds the 0xa5b8 box (+0x7604) over what is up: the wager
            // popup stays under it (paired frame req_pazaak_wager_esc).
            key_.clear();
            openPazaakPopup(6);
        }
        return true;
    }
    if (tag == "LB_MESSAGE") {
        return true;
    }
    if (tag != "BTN_OK" && tag != "BTN_CANCEL") {
        return false;
    }
    gui_.popModal();
    pazaakPopup_ = 0;
    pazaakWait_ = 0;
    key_.clear();
    const bool ok = tag == "BTN_OK";
    if (kind == 9) {
        // HandleInputEvent 0067d260: OK pages on, Cancel pages back; past either end the box closes.
        if (game_.pazaakStage() == GameSession::PazaakStage::Game && game_.pazaak() != nullptr) {
            ok ? game_.pazaak()->helpForward() : game_.pazaak()->helpBack();
        } else {
            ok ? game_.pazaakSetupHelp().forward() : game_.pazaakSetupHelp().back();
        }
        pazaakHelpClosed();
        return true;
    }
    if (kind == 5) {
        mapNote_ = 0;
    }
    if (kind == 1) {
        if (PazaakGame* game = game_.pazaak()) {
            game->acknowledgeSet(); // HandleEndDialog 0067e9a0 (with the tutorial, 0x96f8 next)
        }
    } else if (kind == 2) {
        if (ok) {
            game_.pazaakPlay();
        }
    } else if (kind == 3) {
        if (PazaakGame* game = game_.pazaak()) {
            game->acknowledgeSet();
        }
        game_.resolvePazaakIfOver();
    } else if (kind == 4 && ok) {
        // Retail's OK callback (00692b00) runs the shipped script k_sup_gohawk on the party leader
        // and closes the map.
        game_.setUiPanel(UiPanel::None);
        game_.runScript("k_sup_gohawk", game_.scriptWorld().pc, game_.scriptWorld().pc, game_.scriptWorld().pc,
                        game_.scriptWorld().pc);
    } else if (kind == 6 && gui_.screenName() == "pazaakwager") {
        // The box was over the wager popup: Cancel goes back to it, OK leaves with it.
        if (ok) {
            gui_.popModal();
            game_.quitPazaak();
        } else {
            pazaakPopup_ = 10;
        }
    } else if (kind == 6 && ok) {
        game_.quitPazaak();
    } else if (kind == 7 && ok) {
        game_.forfeitPazaak();
    }
    return true;
}

// Once per frame while a Pazaak screen is bound: the table's sequence runs on its own timer
// (CSWGuiPazaakGame::Draw 00680710) and the set and match results pop up. True while a popup holds
// the screen (the rest of update() leaves it alone).
bool GameScreens::updatePazaak()
{
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    float elapsed = std::chrono::duration<float>(now - pazaakClock_).count();
    pazaakClock_ = now;
    if (pazaakFixedStep_ > 0.0f) {
        elapsed = pazaakFixedStep_;
    }
    elapsed = std::clamp(elapsed, 0.0f, 0.25f);
    if (pazaakPopup_ != 0) {
        if (!gui_.modalActive()) {
            pazaakPopup_ = 0;
        }
        return pazaakPopup_ != 0;
    }
    if (bound_.rfind("pazaak", 0) != 0) {
        pazaakPick_ = -1;
        return false;
    }
    if (PazaakHelp* help = pazaakHelp(); help != nullptr && help->open()) {
        // The help box is on top: the table waits (its timer still runs, Draw 00680710).
        if (PazaakGame* game = game_.pazaak(); game != nullptr && bound_ == "pazaakgame") {
            game->tick(elapsed);
        }
        openPazaakPopup(9);
        return pazaakPopup_ != 0;
    }
    PazaakGame* game = game_.pazaak();
    if (bound_ != "pazaakgame" || game == nullptr) {
        return false;
    }
    game->tick(elapsed);
    if (AudioSystem* audio = game_.audioOrNull()) {
        for (const std::string& sound : game->takeSounds()) {
            audio->playSfx(sound, nullptr, 1.0f, 0.0f, 0.0f);
        }
    } else {
        game->takeSounds();
    }
    // State 9 puts the message box up; OK on it is HandleEndDialog.
    if (game->sequenceState() == 10) {
        openPazaakPopup(game->matchOver() ? 3 : 1);
        return pazaakPopup_ != 0;
    }
    return false;
}

std::string GameScreens::pazaakStateKey() const
{
    std::string key = "|pz" + std::to_string(static_cast<int>(game_.pazaakStage())) + "," + std::to_string(pazaakPick_)
        + "," + std::to_string(game_.pazaakWager()) + "," + std::to_string(game_.gold());
    for (const int kind : game_.pazaakChosen()) {
        key += "c" + std::to_string(kind);
    }
    if (const PazaakGame* game = game_.pazaak()) {
        key += "|p" + std::to_string(game->sequenceState()) + "," + std::to_string(game->turnMark()) + ","
            + std::to_string(game->player().sets) + "," + std::to_string(game->opponent().sets) + ","
            + std::to_string(game->player().standing) + std::to_string(game->opponent().standing);
        for (const PazaakGame::Side* side : {&game->player(), &game->opponent()}) {
            for (const PazaakCard& card : side->table) {
                key += "t" + std::to_string(card.kind) + "." + std::to_string(card.flipped);
            }
            for (const PazaakCard& card : side->hand) {
                key += "h" + std::to_string(card.kind) + "." + std::to_string(card.flipped);
            }
        }
    }
    return key;
}

void GameScreens::refreshPazaak()
{
    if (bound_ == "pazaaksetup") {
        // Columns are the + cards, the - cards and the +/- cards; rows are 1 to 6 (BTN_AVAIL<column><row>).
        // OnPanelAdded 0067f750: a kind not owned shows no card; one owned shows how many are left and
        // greys out at none.
        std::array<int, 18> left{};
        for (int kind = 0; kind < 18; ++kind) {
            left[static_cast<std::size_t>(kind)] = game_.pazaakCardsOwned(kind);
        }
        for (const int kind : game_.pazaakChosen()) {
            --left[static_cast<std::size_t>(kind)];
        }
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 6; ++row) {
                const int kind = column * 6 + row;
                const std::string suffix = std::to_string(column) + std::to_string(row);
                const int count = left[static_cast<std::size_t>(kind)];
                const bool owned = game_.pazaakCardsOwned(kind) > 0;
                for (const char* prefix : {"BTN_AVAIL", "LBL_AVAIL", "LBL_AVAILNUM"}) {
                    gui_.setVisible(prefix + suffix, owned);
                }
                if (!owned) {
                    continue;
                }
                const PazaakCard card = GameSession::pazaakCardOfKind(kind);
                gui_.setFill("BTN_AVAIL" + suffix, cardFill(card));
                gui_.setFillColor("BTN_AVAIL" + suffix, count > 0 ? kWhite : kGrey);
                gui_.setHilightOverFill("BTN_AVAIL" + suffix, true);
                gui_.setEnabled("BTN_AVAIL" + suffix, count > 0);
                gui_.setText("LBL_AVAIL" + suffix, cardText(card));
                gui_.setText("LBL_AVAILNUM" + suffix, std::to_string(std::max(0, count)));
                gui_.setMarked("BTN_AVAIL" + suffix, false);
            }
        }
        const std::vector<int>& chosen = game_.pazaakChosen();
        for (int slot = 0; slot < static_cast<int>(GameSession::kPazaakDeckSize); ++slot) {
            const bool filled = static_cast<std::size_t>(slot) < chosen.size();
            gui_.setVisible(cell("BTN_CHOSEN", slot), filled);
            gui_.setVisible(cell("LBL_CHOSEN", slot), filled);
            if (filled) {
                const PazaakCard card = GameSession::pazaakCardOfKind(chosen[static_cast<std::size_t>(slot)]);
                gui_.setFill(cell("BTN_CHOSEN", slot), cardFill(card));
                gui_.setHilightOverFill(cell("BTN_CHOSEN", slot), true);
                gui_.setText(cell("LBL_CHOSEN", slot), cardText(card));
            }
        }
        // The constructor never binds BTN_YTEXT, so retail does not show it; Play is on with ten cards
        // (ShowDisabledButtons 0067e400).
        gui_.setVisible("BTN_YTEXT", false);
        gui_.setEnabled("BTN_ATEXT", chosen.size() == GameSession::kPazaakDeckSize);
        // The wager popup goes up over the panel once it is drawn. The side deck help (on top of it in
        // retail) is dismissed first here.
        if (game_.pazaakStage() == GameSession::PazaakStage::Wager && pazaakPopup_ == 0
            && !game_.pazaakSetupHelp().open()) {
            openPazaakPopup(10);
        }
        return;
    }
    const PazaakGame* game = game_.pazaak();
    if (game == nullptr) {
        return;
    }
    // 006808a0 / OnPanelAdded 0067ffa0: the player's name and the opponent's.
    gui_.setText("LBL_PLRNAME", game_.character().name);
    if (const std::string opponent = game_.pazaakOpponentName(); !opponent.empty()) {
        gui_.setText("LBL_NPCNAME", opponent);
    }
    const int state = game->sequenceState();
    for (int side = 0; side < 2; ++side) {
        const PazaakGame::Side& who = side == 0 ? game->player() : game->opponent();
        const char* table = side == 0 ? "PLR" : "NPC";
        // A standing side's table is greyed until the set ends (RefreshDisplay, states up to 9).
        const bool greyTable = who.standing && state <= 9;
        for (int index = 0; index < kTableCells; ++index) {
            const std::string button = std::string("BTN_") + table + std::to_string(index);
            const std::string label = std::string("LBL_") + table + std::to_string(index);
            const PazaakCard& card = who.table[static_cast<std::size_t>(index)];
            gui_.setVisible(button, card.present());
            gui_.setVisible(label, card.present());
            if (card.present()) {
                gui_.setFill(button, cardFill(card));
                gui_.setFillColor(button, greyTable ? kGrey : kWhite);
                gui_.setText(label, cardText(card));
            }
        }
        for (int index = 0; index < kHandCards; ++index) {
            const std::string button = std::string("BTN_") + table + "SIDE" + std::to_string(index);
            const std::string label = std::string("LBL_") + table + "SIDE" + std::to_string(index);
            PazaakCard card = who.hand[static_cast<std::size_t>(index)];
            if (side == 1 && card.present()) {
                card = {kPazaakFaceDown, 0}; // the opponent's hand is face down
            }
            gui_.setVisible(button, card.kind != kPazaakNoCard);
            gui_.setVisible(label, card.present());
            if (card.kind == kPazaakNoCard) {
                continue;
            }
            gui_.setFill(button, cardFill(card));
            gui_.setText(label, cardText(card));
            if (side == 0) {
                gui_.setHilightOverFill(button, true);
                gui_.setFillColor(button, state == 3 ? kWhite : kGrey);
                gui_.setEnabled(button, state == 3);
            }
        }
        for (int mark = 0; mark < 3; ++mark) {
            gui_.setFill(std::string("LBL_") + table + "SCORE" + std::to_string(mark),
                         mark < who.sets ? "lbl_winmark02" : "lbl_winmark01");
        }
        gui_.setText(std::string("LBL_") + table + "TOTAL", std::to_string(who.total()));
        gui_.setVisible(std::string("LBL_") + table + "TURN", game->turnMark() == side + 1);
    }
    // A +/- card in the hand has its flip button; the icon and legend show while any is held.
    bool anyFlip = false;
    for (int index = 0; index < kHandCards; ++index) {
        const bool flip = game->player().hand[static_cast<std::size_t>(index)].flip();
        anyFlip = anyFlip || flip;
        gui_.setVisible(cell("BTN_FLIP", index), flip);
        gui_.setEnabled(cell("BTN_FLIP", index), flip);
    }
    gui_.setVisible("LBL_FLIPICON", anyFlip);
    gui_.setVisible("LBL_FLIPLEGEND", anyFlip);
    const bool acting = game->awaitingPlayer();
    gui_.setEnabled("BTN_XTEXT", acting);
    gui_.setEnabled("BTN_YTEXT", acting);
}

bool GameScreens::actionPazaak(const std::string& tag)
{
    if (bound_ == "pazaaksetup") {
        // HandleMoveCard 006807e0: a click on an available card adds it to the deck, on a chosen card
        // takes it back.
        if (tag.rfind("BTN_AVAIL", 0) == 0 && tag.size() == 11) {
            const int kind = (tag[9] - '0') * 6 + (tag[10] - '0');
            if (kind >= 0 && kind < 18) {
                game_.pazaakChoose(kind);
            }
        } else if (tag.rfind("BTN_CHOSEN", 0) == 0 && tag.size() == 11) {
            game_.pazaakUnchoose(static_cast<std::size_t>(tag[10] - '0'));
        } else if (tag == "BTN_ATEXT") {
            openPazaakPopup(game_.pazaakChosen().size() == GameSession::kPazaakDeckSize ? 2 : 8);
        }
        key_.clear();
        return true;
    }
    PazaakGame* game = game_.pazaak();
    if (game == nullptr) {
        return true;
    }
    if (tag.rfind("BTN_PLRSIDE", 0) == 0 && tag.size() == 12) {
        game->playerPlayHand(static_cast<std::size_t>(tag[11] - '0'));
    } else if (tag.rfind("BTN_FLIP", 0) == 0 && tag.size() == 9) {
        game->playerFlipHand(static_cast<std::size_t>(tag[8] - '0'));
    } else if (tag == "BTN_XTEXT") {
        game->playerEndTurn();
    } else if (tag == "BTN_YTEXT") {
        game->playerStand();
    }
    key_.clear();
    return true;
}

// Escape: a question first (0xa5b8 before the table, 0xa5b9 at it); OK quits or forfeits.
bool GameScreens::pazaakBack()
{
    if (pazaakPopup_ == 9) {
        return actionPazaakPopup("BTN_CANCEL"); // Escape (0x2e) is Back / close, as Cancel
    }
    if (pazaakPopup_ == 10) {
        return actionPazaakPopup("BTN_QUIT"); // Escape on the wager popup asks 0xa5b8
    }
    if (pazaakPopup_ != 0) {
        const bool cancellable = pazaakPopup_ == 2 || pazaakPopup_ == 4 || pazaakPopup_ == 6 || pazaakPopup_ == 7;
        return actionPazaakPopup(cancellable ? "BTN_CANCEL" : "BTN_OK");
    }
    if (bound_ == "pazaaksetup") {
        openPazaakPopup(6);
    } else if (bound_ == "pazaakgame") {
        openPazaakPopup(7);
    }
    return true;
}

} // namespace ke
