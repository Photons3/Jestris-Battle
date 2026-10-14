#include "app/solo.hpp"

#include <algorithm>
#include <random>
#include <utility>

#include "app/duel.hpp"

namespace tetris {

SoloRules rulesFor(SoloMode mode) {
    switch (mode) {
    case SoloMode::Sprint:
        return {mode, kSprintLines, 0, true};
    case SoloMode::Blitz:
        return {mode, 0, kBlitzMs, true};
    case SoloMode::Cheese:
        return {mode, 0, 0, true};
    case SoloMode::Marathon:
    case SoloMode::Zen:
        break;
    }
    return {mode};
}

Game makeSoloGame(SoloMode mode, std::uint32_t seed, int startLevel) {
    Board board;
    GameOptions options;
    if (mode == SoloMode::Sprint || mode == SoloMode::Cheese) startLevel = 1;
    if (mode == SoloMode::Blitz) options.linesPerLevel = kBlitzLinesPerLevel;
    if (mode == SoloMode::Cheese) {
        std::mt19937 rng(seed);
        board.pushCheese(kCheeseRows, rng);
    }
    return Game(seed, startLevel, board, options);
}

bool isTimed(SoloMode mode) { return mode == SoloMode::Sprint || mode == SoloMode::Blitz || mode == SoloMode::Cheese; }

SoloSession::SoloSession(SoloRules rules, Game game) : rules_(rules), game_(std::move(game)) {
    if (rules_.countdown) {
        phase_ = SoloPhase::Countdown;
        countdownMs_ = kCountdownMs;
        events_.push_back(SoloEvent::CountdownTick);
    }
}

void SoloSession::pause() {
    if (phase_ == SoloPhase::Playing) game_.pause();
}

int SoloSession::clockMs() const {
    if (rules_.timeLimitMs > 0) return std::max(0, rules_.timeLimitMs - elapsedMs_);
    return elapsedMs_;
}

int SoloSession::progress() const {
    if (rules_.mode == SoloMode::Sprint) return game_.lines();
    if (rules_.mode == SoloMode::Cheese) return game_.board().garbageRows();
    return 0;
}

std::optional<int> SoloSession::result() const {
    if (phase_ != SoloPhase::Finished) return std::nullopt;
    return rules_.mode == SoloMode::Blitz ? game_.score() : elapsedMs_;
}

void SoloSession::finish() {
    phase_ = SoloPhase::Finished;
    events_.push_back(SoloEvent::Finished);
}

void SoloSession::update(int ms) {
    if (phase_ == SoloPhase::Countdown) {
        const int before = countdownMs_;
        countdownMs_ -= ms;
        if (countdownMs_ <= 0) {
            countdownMs_ = 0;
            phase_ = SoloPhase::Playing;
            events_.push_back(SoloEvent::Start);
        } else if ((before - 1) / 1000 != (countdownMs_ - 1) / 1000) {  // the shown second changed
            events_.push_back(SoloEvent::CountdownTick);
        }
        return;
    }
    if (phase_ != SoloPhase::Playing) return;

    const GameState state = game_.state();
    if (isTimed(rules_.mode) && (state == GameState::Playing || state == GameState::Clearing)) {
        elapsedMs_ += ms;
        if (rules_.timeLimitMs > 0 && elapsedMs_ >= rules_.timeLimitMs) {
            elapsedMs_ = rules_.timeLimitMs;
            // The app stops ticking the game at 0:00, so a clear still animating would never be counted.
            while (game_.state() == GameState::Clearing) game_.tick(1);
            finish();
            return;
        }
    }
    if (game_.state() == GameState::GameOver) {
        if (rules_.mode == SoloMode::Zen)
            game_.resetBoard();
        else
            phase_ = SoloPhase::ToppedOut;
        return;
    }
    if ((rules_.goalLines > 0 && game_.lines() >= rules_.goalLines) ||
        (rules_.mode == SoloMode::Cheese && game_.board().garbageRows() == 0))
        finish();
}

std::optional<int> PersonalBests::best(SoloMode mode) const {
    switch (mode) {
    case SoloMode::Sprint:
        return sprintMs;
    case SoloMode::Cheese:
        return cheeseMs;
    case SoloMode::Blitz:
        return blitzScore;
    case SoloMode::Marathon:
    case SoloMode::Zen:
        break;
    }
    return std::nullopt;
}

bool PersonalBests::submit(SoloMode mode, int result) {
    std::optional<int>* slot = nullptr;
    if (mode == SoloMode::Sprint) slot = &sprintMs;
    if (mode == SoloMode::Cheese) slot = &cheeseMs;
    if (mode == SoloMode::Blitz) slot = &blitzScore;
    if (!slot) return false;
    const bool better = !*slot || (mode == SoloMode::Blitz ? result > **slot : result < **slot);
    if (better) *slot = result;
    return better;
}

}  // namespace tetris
