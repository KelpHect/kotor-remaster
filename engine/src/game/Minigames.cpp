#include "game/Minigames.h"

#include <algorithm>
#include <cmath>

namespace ke {

std::uint32_t PazaakGame::nextRandom(std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void PazaakGame::reset(std::uint32_t seed, const std::vector<int>& playerSide,
                       const std::vector<int>& opponentSide)
{
    random_ = seed != 0 ? seed : 0x12345678u;
    deck_.clear();
    for (int value = 1; value <= 10; ++value) {
        for (int copy = 0; copy < 4; ++copy) {
            deck_.push_back(value);
        }
    }
    shuffleDeck();
    playerSide_ = playerSide;
    opponentSide_ = opponentSide;
    initialPlayerSide_ = playerSide;
    initialOpponentSide_ = opponentSide;
    playerTotal_ = 0;
    opponentTotal_ = 0;
    playerSets_ = 0;
    opponentSets_ = 0;
    phase_ = Phase::PlayerTurn;
    lastSet_ = SetResult::None;
}

void PazaakGame::shuffleDeck()
{
    for (std::size_t i = deck_.size(); i > 1; --i) {
        const std::size_t j = nextRandom(random_) % i;
        std::swap(deck_[i - 1], deck_[j]);
    }
    deckCursor_ = 0;
}

int PazaakGame::nextCard()
{
    if (deckCursor_ >= deck_.size()) {
        shuffleDeck();
    }
    return deck_[deckCursor_++];
}

void PazaakGame::drawCard(int& total, bool& bust)
{
    total += nextCard();
    bust = total > 20;
}

void PazaakGame::finishSet()
{
    if (playerTotal_ > 20 || (opponentTotal_ <= 20 && opponentTotal_ > playerTotal_)) {
        ++opponentSets_;
        lastSet_ = SetResult::Opponent;
    } else if (opponentTotal_ > 20 || playerTotal_ > opponentTotal_) {
        ++playerSets_;
        lastSet_ = SetResult::Player;
    } else {
        lastSet_ = SetResult::Draw;
    }
    if (playerSets_ >= 2 || opponentSets_ >= 2) {
        phase_ = Phase::MatchOver;
        return;
    }
    phase_ = Phase::PlayerTurn;
    playerTotal_ = 0;
    opponentTotal_ = 0;
    playerSide_ = initialPlayerSide_;
    opponentSide_ = initialOpponentSide_;
}

bool PazaakGame::playerDraw()
{
    if (phase_ != Phase::PlayerTurn) {
        return false;
    }
    bool bust = false;
    drawCard(playerTotal_, bust);
    if (bust) {
        opponentTotal_ = 0;
        finishSet();
        return true;
    }
    phase_ = Phase::OpponentTurn;
    return true;
}

bool PazaakGame::playerPlaySide(std::size_t index)
{
    if (phase_ != Phase::PlayerTurn || index >= playerSide_.size()) {
        return false;
    }
    playerTotal_ += playerSide_[index];
    playerSide_.erase(playerSide_.begin() + static_cast<std::ptrdiff_t>(index));
    if (playerTotal_ > 20) {
        opponentTotal_ = 0;
        finishSet();
    }
    return true;
}

bool PazaakGame::playerStand()
{
    if (phase_ != Phase::PlayerTurn || playerTotal_ <= 0) {
        return false;
    }
    phase_ = Phase::OpponentTurn;
    return true;
}

bool PazaakGame::opponentTurn()
{
    if (phase_ != Phase::OpponentTurn) {
        return false;
    }
    while (opponentTotal_ < 17) {
        bool bust = false;
        drawCard(opponentTotal_, bust);
        if (bust) {
            finishSet();
            return true;
        }
    }
    // Spend any side card that moves the total closer to 20 without busting.
    bool played = true;
    while (played) {
        played = false;
        int bestIndex = -1;
        int bestTotal = opponentTotal_;
        for (std::size_t i = 0; i < opponentSide_.size(); ++i) {
            const int candidate = opponentTotal_ + opponentSide_[i];
            if (candidate <= 20 && candidate > bestTotal) {
                bestTotal = candidate;
                bestIndex = static_cast<int>(i);
            }
        }
        if (bestIndex >= 0) {
            opponentTotal_ = bestTotal;
            opponentSide_.erase(opponentSide_.begin() + bestIndex);
            played = true;
        }
    }
    finishSet();
    return true;
}

void SwoopRace::reset()
{
    state_ = {};
}

void SwoopRace::update(float dt, float throttle, bool boost, bool brake)
{
    if (state_.finished || dt <= 0.0f) {
        return;
    }
    throttle = std::clamp(throttle, 0.0f, 1.0f);
    const float target = 35.0f + throttle * 70.0f + (boost ? 35.0f : 0.0f)
        - (brake ? 25.0f : 0.0f);
    const float blend = std::min(1.0f, dt * 1.5f);
    state_.speed += (target - state_.speed) * blend;
    state_.speed = std::clamp(state_.speed, 0.0f, 140.0f);
    state_.distance += state_.speed * dt;
    state_.time += dt;
    const int lap = static_cast<int>(state_.distance / kTrackLength) + 1;
    state_.lap = std::min(lap, kLaps);
    if (state_.distance >= kTrackLength * static_cast<float>(kLaps)) {
        state_.finished = true;
        state_.lap = kLaps;
    }
}

void TurretGame::reset()
{
    state_ = {};
    targets_.clear();
    for (int i = 0; i < 5; ++i) {
        TurretTarget target;
        target.angle = -40.0f + static_cast<float>(i) * 20.0f;
        target.distance = 100.0f - static_cast<float>(i) * 8.0f;
        targets_.push_back(target);
    }
}

void TurretGame::update(float dt)
{
    if (dt <= 0.0f) {
        return;
    }
    state_.time += dt;
    for (TurretTarget& target : targets_) {
        if (!target.alive) {
            continue;
        }
        target.distance -= 12.0f * dt;
        if (target.distance <= 0.0f) {
            target.alive = false;
            state_.hitPoints -= 20;
        }
    }
}

bool TurretGame::fireAt(std::size_t index)
{
    if (index >= targets_.size() || !targets_[index].alive) {
        return false;
    }
    ++state_.shots;
    targets_[index].alive = false;
    ++state_.hits;
    state_.score += 100;
    return true;
}

} // namespace ke
