#include "game/Minigames.h"

#include <algorithm>

namespace ke {

namespace {

constexpr int kBust = -1000000; // retail's value for a total over 20 (006e48a0, 006e4c60, 006e4910)
constexpr float kShortWait = 0.4f;  // 0x3ecccccd in DoGameSequence 00680030 and the button handlers
constexpr float kLongWait = 0.8f;   // 0x3f4ccccd after an opponent draw or hand card (00680030)

int capped(int total)
{
    return total > 20 ? kBust : total;
}

} // namespace

int pazaakKindValue(int kind)
{
    // 007a26b8: +1..+6, -1..-6, the +/- cards' 1..6, the main deck's 1..10.
    static constexpr int kValues[28] = {1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6, 1, 2,
                                        3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    return kind >= 0 && kind < 28 ? kValues[kind] : 0;
}

int PazaakCard::value() const
{
    if (kind < 0) {
        return 0;
    }
    return flip() && flipped != 0 ? -pazaakKindValue(kind) : pazaakKindValue(kind);
}

// GetTotal 006e4360: the table from the left until the first empty place.
int PazaakGame::Side::total() const
{
    int sum = 0;
    for (const PazaakCard& card : table) {
        if (card.kind == kPazaakNoCard) {
            break;
        }
        sum += card.value();
    }
    return sum;
}

int PazaakGame::Side::tableCount() const
{
    return static_cast<int>(std::count_if(table.begin(), table.end(), [](const PazaakCard& c) { return c.present(); }));
}

int PazaakGame::Side::handCount() const
{
    return static_cast<int>(std::count_if(hand.begin(), hand.end(), [](const PazaakCard& c) { return c.present(); }));
}

// pazaakdecks.2da row 0 when the table cannot be read.
std::vector<PazaakCard> PazaakGame::defaultSide()
{
    return {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {6, 0}, {7, 0}, {8, 0}, {12, 0}, {13, 0}};
}

std::uint32_t PazaakGame::nextRandom()
{
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
}

// ShuffleDeck 006e4730: forty cards, 1..10 four times, drawn out of a pool at random.
void PazaakGame::shuffleDeck()
{
    std::array<PazaakCard, 40> pool{};
    for (int i = 0; i < 40; ++i) {
        pool[static_cast<std::size_t>(i)] = {i % 10 + 18, 0};
    }
    for (int left = 40, out = 0; left > 0; --left, ++out) {
        const int pick = static_cast<int>(nextRandom() % static_cast<std::uint32_t>(left));
        deck_[static_cast<std::size_t>(out)] = pool[static_cast<std::size_t>(pick)];
        pool[static_cast<std::size_t>(pick)] = pool[static_cast<std::size_t>(left - 1)];
    }
    deckIndex_ = 39;
}

// DrawCard 006e4800: from the top index down; an empty deck is shuffled first.
PazaakCard PazaakGame::drawCard()
{
    if (deckIndex_ < 0) {
        shuffleDeck();
    }
    return deck_[static_cast<std::size_t>(deckIndex_--)];
}

// AddGameCard 006e45f0: the first empty place; the ninth place or a total of exactly 20 stands the side.
void PazaakGame::addGameCard(Side& side, PazaakCard card)
{
    int place = 0;
    for (; place < 9; ++place) {
        if (side.table[static_cast<std::size_t>(place)].kind == kPazaakNoCard) {
            side.table[static_cast<std::size_t>(place)] = card;
            break;
        }
    }
    if (place > 7 || side.total() == 20) {
        side.standing = true;
    }
}

// ChooseSidedeck 006e4580: four times, a card at random out of the ten, the last one moving into its place.
void PazaakGame::chooseSidedeck(std::vector<PazaakCard> source, std::array<PazaakCard, 4>& hand)
{
    hand.fill({});
    source.resize(10);
    int left = 10;
    for (PazaakCard& slot : hand) {
        const int pick = static_cast<int>(nextRandom() % static_cast<std::uint32_t>(left));
        slot = source[static_cast<std::size_t>(pick)];
        source[static_cast<std::size_t>(pick)] = source[static_cast<std::size_t>(left - 1)];
        source[static_cast<std::size_t>(left - 1)] = {};
        --left;
    }
}

void PazaakHelp::show(std::int32_t message, std::int32_t okText, std::int32_t cancelText)
{
    if (!enabled_) {
        return; // +0x994
    }
    pages_.assign(1, message);
    page_ = 0;
    anchor_ = Anchor::None;
    twoButtons_ = !(okText == -1 && cancelText == kCancel);
    if (!twoButtons_) {
        okText_ = kContinue;
    } else {
        if (okText != -1) {
            okText_ = okText; // 00415e50 leaves the label alone for -1
        }
        if (cancelText != -1) {
            cancelText_ = cancelText;
        }
    }
    open_ = true;
}

void PazaakHelp::showPages(const std::vector<std::int32_t>& pages)
{
    if (!enabled_ || pages.empty()) {
        return;
    }
    show(pages.front());
    pages_ = pages;
    cancelText_ = kBack;
}

std::int32_t PazaakHelp::message() const
{
    return page_ >= 0 && page_ < pageCount() ? pages_[static_cast<std::size_t>(page_)] : 0;
}

bool PazaakHelp::forward()
{
    if (!open_) {
        return false;
    }
    if (page_ < pageCount() - 1) {
        ++page_;
        twoButtons_ = true;
        return false;
    }
    accepted_ = true;
    closedMessage_ = message();
    open_ = false;
    return true;
}

bool PazaakHelp::back()
{
    if (!open_) {
        return false;
    }
    if (page_ > 0) {
        --page_;
        if (page_ == 0) {
            twoButtons_ = false;
        }
        return false;
    }
    accepted_ = !twoButtons_;
    closedMessage_ = message();
    open_ = false;
    return true;
}

std::array<int, 2> PazaakHelp::placeNear(Anchor anchor, int x, int y, int w, int h, int boxW, int boxH)
{
    (void)boxW;
    switch (anchor) {
    case Anchor::RightOfPlayerCard:
        return {x + w, y + h / 2 - boxH / 2}; // 0067d110
    case Anchor::AbovePlayerHand:
    case Anchor::AboveEndTurn:
        return {x, y - boxH}; // 0067d1c0
    case Anchor::None:
        break;
    }
    return {x, y};
}

void PazaakGame::reset(std::uint32_t seed, const std::vector<PazaakCard>& playerSide,
                       const std::vector<PazaakCard>& opponentSide, bool tutorial)
{
    random_ = seed != 0 ? seed : 0x12345678u;
    deckIndex_ = -1;
    player_ = {};
    opponent_ = {};
    chooseSidedeck(opponentSide, opponent_.hand); // the start panel deals the opponent first (00681a90)
    chooseSidedeck(playerSide, player_.hand);     // then the player's on accept (00681890)
    state_ = 0;
    timer_ = -1.0f;
    aiState_ = 0;
    turnMark_ = 0;
    endSound_.clear();
    sounds_.clear();
    lastSet_ = SetResult::None;
    help_ = {};
    help_.enable(tutorial); // the panel's tutorial, enabled by the constructor's argument (006808a0)
    tutorialFlags_ = 0;
    pendingHand_ = -1;
    startNewGame(); // OnPanelAdded 0067ffa0 -> StartNewGame
}

// StartNewGame 0067e770: 0x10 is cleared for every set; the rules pages (0x96e7, 0x96e8) once.
void PazaakGame::startNewGame()
{
    tutorialFlags_ &= ~0x10;
    if ((tutorialFlags_ & 8) == 0) {
        tutorialFlags_ |= 8;
        help_.showPages({0x96e7, 0x96e8});
    }
}

void PazaakGame::helpForward()
{
    if (help_.forward()) {
        tutorialClosed(help_.closedMessage(), help_.accepted());
    }
}

void PazaakGame::helpBack()
{
    if (help_.back()) {
        tutorialClosed(help_.closedMessage(), help_.accepted());
    }
}

// HandleTutorialClose 0067ea10: the jump table at 0067ebec / 0067ec08 on (message - 0x96e9).
void PazaakGame::tutorialClosed(std::int32_t message, bool accepted)
{
    if (!accepted) {
        if (message == 0x96f8 || message == 0x96f9) {
            state_ = 12; // End Tutorial ends the match
        }
        return;
    }
    switch (message) {
    case 0x96e9:
        help_.showPages({0x96ea, 0x96eb, 0x96ec, 0x96f3, 0x96f4});
        help_.place(PazaakHelp::Anchor::AbovePlayerHand);
        break;
    case 0x96ec:
    case 0x96f4:
        help_.showPages({0x96ed, 0x96ee, 0x96ef});
        help_.place(PazaakHelp::Anchor::AboveEndTurn);
        break;
    case 0x96f0: // Stand confirmed
        player_.standing = true;
        state_ = 5;
        timer_ = kShortWait;
        break;
    case 0x96f1: // End Turn confirmed
        state_ = 5;
        timer_ = kShortWait;
        break;
    case 0x96f2: // Play Hand Card confirmed: the card goes down, no wait (0067eb1d)
        if (pendingHand_ >= 0 && pendingHand_ < 4 && player_.hand[static_cast<std::size_t>(pendingHand_)].present()) {
            addGameCard(player_, player_.hand[static_cast<std::size_t>(pendingHand_)]);
            player_.hand[static_cast<std::size_t>(pendingHand_)] = {};
            sounds_.push_back("mgs_playside");
            state_ = 4;
        }
        pendingHand_ = -1;
        break;
    case 0x96f9:
        state_ = 12; // EndPazaakGame (0067eb83)
        break;
    default:
        break;
    }
}

std::vector<std::string> PazaakGame::takeSounds()
{
    std::vector<std::string> out;
    out.swap(sounds_);
    return out;
}

void PazaakGame::setForTest(const Side& player, const Side& opponent, int state)
{
    player_ = player;
    opponent_ = opponent;
    state_ = state;
    timer_ = -1.0f;
    aiState_ = 0;
}

PazaakGame::Phase PazaakGame::phase() const
{
    if (state_ >= 9) {
        return matchOver() ? Phase::MatchOver : Phase::SetOver;
    }
    return state_ >= 5 ? Phase::OpponentTurn : Phase::PlayerTurn;
}

// DetermineWinner 006e48a0: 0 undecided, 1 the player, 2 the opponent, 3 a tie.
int PazaakGame::determineWinner()
{
    int mine = player_.total();
    int theirs = opponent_.total();
    if (mine > 20 || theirs > 20) {
        player_.standing = true;
        opponent_.standing = true;
    }
    if (!player_.standing || !opponent_.standing) {
        return 0;
    }
    mine = capped(mine);
    theirs = capped(theirs);
    if (mine > theirs) {
        return 1;
    }
    return theirs <= mine ? 3 : 2;
}

int PazaakGame::findPotentialSidecardTotal(Side& side, int& index, bool lowest)
{
    const int current = side.total();
    int best = capped(current);
    int bestIndex = -1;
    int bestFlipped = 0;
    bool bestIsFlip = false;
    for (int i = 0; i < 4; ++i) {
        PazaakCard& card = side.hand[static_cast<std::size_t>(i)];
        const bool flip = card.flip();
        // A +/- card is tried on its - side first, then its + side; the flag is left on the last try.
        for (int pass = flip ? 2 : 1; pass > 0; --pass) {
            int flipped = 0;
            if (flip) {
                flipped = pass == 2 ? 1 : 0;
                card.flipped = flipped;
            }
            const int candidate = capped(current + card.value());
            bool take = false;
            if (!lowest) {
                take = candidate > best || (candidate == best && bestIsFlip);
            } else {
                take = (candidate < best && candidate > kBust) || (candidate == best && bestIsFlip);
            }
            if (take) {
                best = candidate;
                bestIndex = i;
                bestFlipped = flipped;
                bestIsFlip = flip;
            }
        }
    }
    if (bestIndex >= 0) {
        index = bestIndex;
        side.hand[static_cast<std::size_t>(bestIndex)].flipped = bestFlipped;
    }
    return best;
}

// PlayAITurn 006e4c60, one call per step: 1 drew, 2 played a hand card, 3 stood, 4 ended the turn, 0 done.
int PazaakGame::playAITurn(int state)
{
    Side& ai = opponent_;
    Side& rival = player_;
    int rivalTotal = rival.total();
    if (rivalTotal > 20) {
        rivalTotal = kBust;
        rival.standing = true;
    }
    const auto playCard = [&](int index) {
        addGameCard(ai, ai.hand[static_cast<std::size_t>(index)]);
        ai.hand[static_cast<std::size_t>(index)] = {};
        return 2;
    };
    const auto settle = [&]() {
        const int mine = capped(ai.total());
        if (rival.standing && mine < rivalTotal) {
            return 4;
        }
        int ignored = -1;
        if (mine < 18 && findPotentialSidecardTotal(ai, ignored, true) < 16) {
            return 4;
        }
        ai.standing = true;
        return 3;
    };
    switch (state) {
    case 0:
        if (ai.standing) {
            return 0;
        }
        addGameCard(ai, drawCard());
        return 1;
    case 1: {
        const int mine = ai.total();
        const bool mayWait = rival.sets + 1 < 3;
        if (rival.standing && rivalTotal < mine && mine <= 20) {
            ai.standing = true;
            return 3;
        }
        int index = -1;
        const int low = findPotentialSidecardTotal(ai, index, true);
        index = -1;
        const int high = findPotentialSidecardTotal(ai, index, false);
        if (high >= 18 && rivalTotal <= high && (high >= 20 || (high >= 19 && low > 11) || low > 13) && index >= 0) {
            return playCard(index);
        }
        if (mine > 20) {
            if (mayWait && rivalTotal >= 18 && high < rivalTotal) {
                return 4;
            }
            if (index >= 0) {
                return playCard(index);
            }
        }
        return settle();
    }
    case 2:
        return settle();
    case 3:
    case 4:
        return 0;
    default:
        return 4;
    }
}

// DoGameSequence 00680030. Returns true when the panel should call it again in the same frame.
bool PazaakGame::step()
{
    switch (state_) {
    case 0:
        state_ = 1;
        return true;
    case 1:
        if (!player_.standing) {
            turnMark_ = 1;
            sounds_.push_back("mgs_startturn");
            state_ = 2;
            timer_ = kShortWait;
            return false;
        }
        state_ = 5;
        return true;
    case 2:
        addGameCard(player_, drawCard()); // DrawPlayerCard 0067cae0
        sounds_.push_back("mgs_drawmain");
        if ((tutorialFlags_ & 2) == 0) {
            tutorialFlags_ |= 2;
            help_.show(0x96e9);
            help_.place(PazaakHelp::Anchor::RightOfPlayerCard); // beside BTN_PLR0 (+0x11e4)
        }
        if (player_.standing) {
            timer_ = kShortWait;
        }
        if (player_.total() > 20) {
            sounds_.push_back("mgs_warnbust");
        }
        state_ = 3;
        return false;
    case 3:
    case 4:
        if (player_.standing) {
            state_ = 5;
            return true;
        }
        return false;
    case 5: {
        const int total = player_.total();
        if (total > 20) {
            help_.show(0x96f7); // every bust
        } else if (total == 20 && (tutorialFlags_ & 0x10) == 0) {
            tutorialFlags_ |= 0x10;
            help_.show(0x96f6);
        }
        if (determineWinner() != 0) {
            state_ = 8;
            return true;
        }
        state_ = 6;
    }
        [[fallthrough]];
    case 6:
        if (opponent_.standing) {
            state_ = 8;
            return true;
        }
        turnMark_ = 2;
        sounds_.push_back("mgs_startturn");
        timer_ = kShortWait;
        state_ = 7;
        aiState_ = 0;
        if ((tutorialFlags_ & 4) == 0) {
            tutorialFlags_ |= 4;
            help_.show(0x96f5);
        }
        return false;
    case 7: {
        aiState_ = playAITurn(aiState_);
        if (aiState_ == 1) {
            sounds_.push_back("mgs_drawmain");
            if (opponent_.total() > 20) {
                sounds_.push_back("mgs_warnbust");
            }
        } else if (aiState_ == 2) {
            sounds_.push_back("mgs_playside");
        }
        if (aiState_ == 1 || aiState_ == 2) {
            timer_ = kLongWait;
            if (opponent_.total() == 20 && (tutorialFlags_ & 0x10) == 0) {
                tutorialFlags_ |= 0x10;
                help_.show(0x96f6);
            }
        }
        if (aiState_ != 0) {
            return false;
        }
        timer_ = kShortWait;
        state_ = 8;
        [[fallthrough]];
    }
    case 8: {
        const int result = determineWinner();
        if (result == 0) {
            state_ = 1;
            return false;
        }
        if (result == 1) {
            ++player_.sets;
            lastSet_ = SetResult::Player;
            endSound_ = player_.sets < 3 ? "mgs_winset" : "mgs_winmatch";
        } else if (result == 2) {
            ++opponent_.sets;
            lastSet_ = SetResult::Opponent;
            endSound_ = opponent_.sets < 3 ? "mgs_loseset" : "mgs_losematch";
        } else {
            lastSet_ = SetResult::Draw;
        }
        state_ = 9;
        return true;
    }
    case 9:
        if (!endSound_.empty()) {
            sounds_.push_back(endSound_);
            endSound_.clear();
        }
        state_ = 10; // the message box goes up (GameScreensPazaak)
        return false;
    case 11:
        // StartNewGame 0067e770 / ClearGameBoard 006e4640: tables and stand flags only.
        player_.table.fill({});
        opponent_.table.fill({});
        player_.standing = false;
        opponent_.standing = false;
        state_ = 0;
        startNewGame();
        return false;
    default:
        return false;
    }
}

void PazaakGame::tick(float seconds)
{
    if (timer_ >= 0.0f) {
        timer_ -= seconds;
        return;
    }
    // Draw 00680710: the sequence runs only while the table is the top panel (0040adf0).
    for (int guard = 0; guard < 64 && !help_.open() && step(); ++guard) {
    }
}

// HandlePlayHandCard 0067ede0: only in state 3, then state 4 and a 0.4 s wait.
bool PazaakGame::playerPlayHand(std::size_t index)
{
    if (help_.open() || state_ != 3 || index >= player_.hand.size() || !player_.hand[index].present()) {
        return false;
    }
    if (help_.enabled() && player_.total() <= 12) {
        pendingHand_ = static_cast<int>(index); // 0067ee47: asked first, "Play Hand Card" / Cancel
        help_.show(0x96f2, 0x7e4a, PazaakHelp::kCancel);
        return true;
    }
    addGameCard(player_, player_.hand[index]);
    player_.hand[index] = {};
    sounds_.push_back("mgs_playside");
    state_ = 4;
    timer_ = kShortWait;
    return true;
}

bool PazaakGame::playerFlipHand(std::size_t index)
{
    if (index >= player_.hand.size() || !player_.hand[index].flip()) {
        return false;
    }
    player_.hand[index].flipped = player_.hand[index].flipped == 0 ? 1 : 0;
    return true;
}

// HandleContinue 0067ec20 / HandleStand 0067ed00: states 3 and 4, not standing; state 5 after 0.4 s.
bool PazaakGame::playerEndTurn()
{
    if (help_.open() || !awaitingPlayer()) {
        return false;
    }
    if (help_.enabled() && player_.total() >= 16) {
        help_.show(0x96f1, 0x7e4c, PazaakHelp::kCancel); // 0067ec9c: "End Turn" / Cancel
        return true;
    }
    state_ = 5;
    timer_ = kShortWait;
    return true;
}

bool PazaakGame::playerStand()
{
    if (help_.open() || !awaitingPlayer()) {
        return false;
    }
    if (help_.enabled() && player_.total() <= 13) {
        help_.show(0x96f0, 0x7e4d, PazaakHelp::kCancel); // 0067ed6e: "Stand" / Cancel
        return true;
    }
    player_.standing = true;
    state_ = 5;
    timer_ = kShortWait;
    return true;
}

bool PazaakGame::acknowledgeSet()
{
    if (state_ != 10 && state_ != 9) {
        return false;
    }
    // HandleEndDialog 0067e9a0: between sets 0x96f8 with End Tutorial, at the end 0x96f9.
    if (!matchOver()) {
        help_.show(0x96f8, -1, PazaakHelp::kEndTutorial);
        state_ = 11;
    } else {
        help_.show(0x96f9);
        state_ = 12;
    }
    return true;
}

} // namespace ke
