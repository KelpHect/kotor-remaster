#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ke {

// Pazaak as retail plays it. Every rule below is from the decompiled 1.03 exe:
//   CSWGuiPazaakGame::DoGameSequence 00680030 (the turn state machine), Draw 00680710 (its timer),
//   CSWPazaak::DrawCard 006e4800 / ShuffleDeck 006e4730 (40 main cards, 1..10 four times, reshuffled only
//   when the deck runs out), CPazaakPlayer::AddGameCard 006e45f0 (a card that fills the ninth place or
//   makes the total exactly 20 stands the side; nine cards do not win by themselves),
//   CSWPazaak::DetermineWinner 006e48a0 (decided once both stand; a side over 20 makes both stand and
//   counts as -1000000; equal is a tie and nobody scores), CSWPazaak::PlayAITurn 006e4c60 and
//   FindPotentialSidecardTotal 006e4910 (the opponent), CPazaakPlayer::ChooseSidedeck 006e4580 (four of
//   the ten side cards at random), CSWPazaak::ClearGameBoard 006e4640 (a new set clears the tables and
//   the stand flags; hands carry over), CSWGuiPazaakGame::HandleEndDialog 0067e9a0 (three sets win).
// The player starts every set (state 0 -> 1 after StartNewGame 0067e770).

// Card kinds as retail numbers them: 0..5 +1..+6, 6..11 -1..-6, 12..17 the +/-1..6 cards, 18..27 the
// main deck's 1..10, -1 no card, -2 a face-down card (the opponent's hand, RefreshDisplay 0067d4b0).
constexpr int kPazaakNoCard = -1;
constexpr int kPazaakFaceDown = -2;
int pazaakKindValue(int kind); // the table at 007a26b8

struct PazaakCard {
    int kind = kPazaakNoCard;
    int flipped = 0; // a +/- card shows its - side (the flag beside the kind)
    bool present() const { return kind >= 0; }
    bool flip() const { return kind >= 12 && kind <= 17; }
    bool main() const { return kind >= 18 && kind <= 27; }
    int value() const; // as it plays (GetTotal 006e4360)
};

// CSWGuiTutorial (0067def0, on the shipped confirm.gui): the help box PlayPazaak's bShowTutorial turns on,
// for the side deck panel and for the table. A queue of strrefs, one page shown at a time.
//   ShowHelp 0067dfc0: a new queue of one page. OK -1 with Cancel 0x96e2 is the plain form: one button,
//     OK reads 0x96df ("Continue"). Otherwise both buttons show, OK reads the given text (-1 keeps the
//     one it had) and Cancel the other. The place it was put at is forgotten.
//   ShowMultipleHelp 0067e0b0: ShowHelp with the first page, the rest queued, Cancel reads 0x96e0 ("Back").
//   HandleInputEvent 0067d260: OK (0x27, 0x1f6) before the last page shows the next one with both
//     buttons; Cancel or Escape (0x28, 0x2e, 0x1f7) after the first goes back, the first page has one
//     button again. Otherwise the box closes (006250f0): accepted on OK, and on Cancel or Escape when
//     only one button shows; the owner's close handler gets the page shown.
//   PlaceRightOf 0067d110 / PlaceAbove 0067d1c0: next to a control; PlaceAbove is kept for the pages.
class PazaakHelp {
public:
    enum class Anchor { None, RightOfPlayerCard, AbovePlayerHand, AboveEndTurn };
    static constexpr std::int32_t kContinue = 0x96df;
    static constexpr std::int32_t kBack = 0x96e0;
    static constexpr std::int32_t kEndTutorial = 0x96e1;
    static constexpr std::int32_t kCancel = 0x96e2;

    void enable(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    void show(std::int32_t message, std::int32_t okText = -1, std::int32_t cancelText = kCancel);
    void showPages(const std::vector<std::int32_t>& pages);
    void place(Anchor anchor) { anchor_ = anchor; }
    void close() { open_ = false; }

    bool open() const { return open_; }
    std::int32_t message() const;
    int page() const { return page_; }
    int pageCount() const { return static_cast<int>(pages_.size()); }
    bool twoButtons() const { return twoButtons_; }
    std::int32_t okText() const { return okText_; }         // -1: the shipped label
    std::int32_t cancelText() const { return cancelText_; } // -1: the shipped label
    Anchor anchor() const { return anchor_; }

    // OK, and Cancel or Escape. True when the box closed; accepted() and closedMessage() then tell how.
    bool forward();
    bool back();
    bool accepted() const { return accepted_; }
    std::int32_t closedMessage() const { return closedMessage_; }

    // The box's top left for a control at (x, y, w, h) and a box of boxW x boxH (0067d110, 0067d1c0).
    static std::array<int, 2> placeNear(Anchor anchor, int x, int y, int w, int h, int boxW, int boxH);

private:
    bool enabled_ = false;
    bool open_ = false;
    std::vector<std::int32_t> pages_;
    int page_ = 0;
    bool twoButtons_ = false;
    std::int32_t okText_ = -1;
    std::int32_t cancelText_ = -1;
    Anchor anchor_ = Anchor::None;
    bool accepted_ = false;
    std::int32_t closedMessage_ = 0;
};

class PazaakGame {
public:
    enum class Phase { PlayerTurn, OpponentTurn, SetOver, MatchOver };
    enum class SetResult { None, Player, Opponent, Draw };

    struct Side {
        std::array<PazaakCard, 4> hand{};  // a played card leaves its place empty
        std::array<PazaakCard, 9> table{}; // filled from the left
        bool standing = false;
        int sets = 0;
        int total() const;
        int tableCount() const;
        int handCount() const;
    };

    // Ten side cards each; four of each ten are dealt at random (ChooseSidedeck). A list shorter than
    // ten deals only from what is there.
    // `tutorial` is PlayPazaak's bShowTutorial (the panel's 0x86b4, 006808a0). OnPanelAdded 0067ffa0 runs
    // StartNewGame, so the rules pages come up at once when it is on.
    void reset(std::uint32_t seed, const std::vector<PazaakCard>& playerSide = defaultSide(),
               const std::vector<PazaakCard>& opponentSide = defaultSide(), bool tutorial = false);
    static std::vector<PazaakCard> defaultSide();

    // The panel's per-frame call (Draw 00680710): while the timer runs it counts down; once it is out
    // the sequence runs until it waits again.
    void tick(float seconds);

    Phase phase() const;
    int sequenceState() const { return state_; }
    float timer() const { return timer_; }
    const Side& player() const { return player_; }
    const Side& opponent() const { return opponent_; }
    int playerTotal() const { return player_.total(); }
    int opponentTotal() const { return opponent_.total(); }
    int playerSets() const { return player_.sets; }
    int opponentSets() const { return opponent_.sets; }
    int deckRemaining() const { return deckIndex_ + 1; }
    SetResult lastSet() const { return lastSet_; }
    bool matchOver() const { return player_.sets >= 3 || opponent_.sets >= 3; }
    // EndPazaakGame's bWon (DoGameSequence state 12): more sets than the opponent.
    bool playerWonMatch() const { return opponent_.sets < player_.sets; }
    // The player may act: End Turn and Stand (0067ec20, 0067ed00) in states 3 and 4 while not standing.
    bool awaitingPlayer() const { return (state_ == 3 || state_ == 4) && !player_.standing; }
    // A hand card was played this turn (state 4): no second one (HandlePlayHandCard 0067ede0).
    bool handCardPlayed() const { return state_ == 4; }
    // LBL_PLRTURN / LBL_NPCTURN: shown from the start of that side's turn (states 1 and 6) until the
    // other side's turn starts. 0 none, 1 the player, 2 the opponent.
    int turnMark() const { return turnMark_; }
    // The one-shot sounds DoGameSequence played since the last call (mgs_startturn, mgs_drawmain,
    // mgs_playside, mgs_warnbust, mgs_winset, mgs_loseset, mgs_winmatch, mgs_losematch).
    std::vector<std::string> takeSounds();

    bool playerPlayHand(std::size_t index);
    bool playerFlipHand(std::size_t index); // HandleFlipHandCard 0067dcf0: no turn check in retail
    bool playerEndTurn();
    bool playerStand();
    // OK on the set or match message (HandleEndDialog 0067e9a0).
    bool acknowledgeSet();

    // Retail's opponent move, exposed for the checks: one PlayAITurn call with the given state.
    int playAITurn(int state);
    // FindPotentialSidecardTotal on the opponent's hand: the best total at 20 or under one hand card
    // reaches (lowest == false), or the lowest under the current one (lowest == true); `index` gets the
    // card. Like retail it leaves every +/- card on its + side except the one it picked.
    static int findPotentialSidecardTotal(Side& side, int& index, bool lowest);
    // Sets a side's cards for the checks.
    void setForTest(const Side& player, const Side& opponent, int state);

    // The table's help box. While it is up the sequence waits (Draw 00680710 steps only while the panel
    // is on top, 0040adf0); its timer still runs out.
    const PazaakHelp& help() const { return help_; }
    bool helpOpen() const { return help_.open(); }
    // OK and Cancel/Escape on the help box; when it closes, HandleTutorialClose 0067ea10 runs.
    void helpForward();
    void helpBack();
    // DoGameSequence state 12: EndPazaakGame is due (the match is over, or End Tutorial was chosen).
    bool endDue() const { return state_ == 12; }

private:
    void startNewGame(); // StartNewGame 0067e770's tutorial part
    void tutorialClosed(std::int32_t message, bool accepted);
    bool step(); // DoGameSequence: true when it should run again at once
    int determineWinner();
    PazaakCard drawCard();
    void shuffleDeck();
    static void addGameCard(Side& side, PazaakCard card);
    void chooseSidedeck(std::vector<PazaakCard> source, std::array<PazaakCard, 4>& hand);
    std::uint32_t nextRandom();

    std::uint32_t random_ = 0x12345678u;
    std::array<PazaakCard, 40> deck_{};
    int deckIndex_ = -1; // DrawCard: from 39 down; below 0 reshuffles
    Side player_;
    Side opponent_;
    int state_ = 0;
    float timer_ = -1.0f;
    int aiState_ = 0;
    int turnMark_ = 0;
    std::string endSound_;
    std::vector<std::string> sounds_;
    SetResult lastSet_ = SetResult::None;
    PazaakHelp help_;
    int tutorialFlags_ = 0; // 0x86dc: 2 first draw, 4 first opponent turn, 8 rules, 0x10 twenty this set
    int pendingHand_ = -1;  // 0x86e0: the hand card waiting on the 0x96f2 question
};

} // namespace ke
