#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ke {

// Deterministic KotOR-style minigame state machines. They own rules and
// timing only; presentation is supplied by the UI/renderer.

class PazaakGame {
public:
    enum class Phase { PlayerTurn, OpponentTurn, SetOver, MatchOver };
    enum class SetResult { None, Player, Opponent, Draw };

    void reset(std::uint32_t seed,
               const std::vector<int>& playerSide = {-1, -2, 3, 4},
               const std::vector<int>& opponentSide = {-1, -2, 3, 4});
    Phase phase() const { return phase_; }
    int playerTotal() const { return playerTotal_; }
    int opponentTotal() const { return opponentTotal_; }
    int playerSets() const { return playerSets_; }
    int opponentSets() const { return opponentSets_; }
    const std::vector<int>& playerSide() const { return playerSide_; }
    std::size_t deckRemaining() const { return deck_.size() - deckCursor_; }
    SetResult lastSet() const { return lastSet_; }

    bool playerDraw();
    bool playerPlaySide(std::size_t index);
    bool playerStand();
    bool opponentTurn();

private:
    void drawCard(int& total, bool& bust);
    void finishSet();
    void shuffleDeck();
    int nextCard();
    static std::uint32_t nextRandom(std::uint32_t& state);

    std::uint32_t random_ = 0x12345678u;
    std::vector<int> deck_;
    std::size_t deckCursor_ = 0;
    std::vector<int> playerSide_;
    std::vector<int> opponentSide_;
    std::vector<int> initialPlayerSide_;
    std::vector<int> initialOpponentSide_;
    int playerTotal_ = 0;
    int opponentTotal_ = 0;
    int playerSets_ = 0;
    int opponentSets_ = 0;
    Phase phase_ = Phase::PlayerTurn;
    SetResult lastSet_ = SetResult::None;
};

struct SwoopState {
    float speed = 0.0f;
    float distance = 0.0f;
    float time = 0.0f;
    int lap = 1;
    bool finished = false;
};

class SwoopRace {
public:
    static constexpr float kTrackLength = 3000.0f;
    static constexpr int kLaps = 3;

    void reset();
    void update(float dt, float throttle, bool boost, bool brake);
    const SwoopState& state() const { return state_; }

private:
    SwoopState state_;
};

struct TurretTarget {
    float angle = 0.0f;
    float distance = 100.0f;
    bool alive = true;
};

struct TurretState {
    float time = 0.0f;
    int hitPoints = 100;
    int score = 0;
    int shots = 0;
    int hits = 0;
};

class TurretGame {
public:
    void reset();
    void update(float dt);
    bool fireAt(std::size_t target);
    const TurretState& state() const { return state_; }
    const std::vector<TurretTarget>& targets() const { return targets_; }

private:
    TurretState state_;
    std::vector<TurretTarget> targets_;
};

} // namespace ke
