#pragma once

#include <cstdint>
#include <deque>
#include <random>
#include <string>

#include "core/game.hpp"

namespace tetris {

enum class BotLevel { Easy, Medium, Hard };

struct BotPreset {
    int thinkMs;         // pause before each piece is planned
    int inputMs;         // gap between two key presses
    double mistakeRate;  // chance of playing the 2nd or 3rd best placement
};

BotPreset presetFor(BotLevel level);
std::string botName(BotLevel level);

class BotPlayer {
public:
    BotPlayer(BotLevel level, std::uint32_t seed);

    // Advances the bot by `ms` in 1 ms steps; only calls game.apply(). Call before ticking the game each ms.
    void update(Game& game, int ms);
    // Drops the plan, e.g. after the board was reset by a KO.
    void reset();

private:
    void step(Game& game);

    BotPreset preset_;
    std::mt19937 rng_;
    std::deque<Action> queue_;
    int thinkLeft_;
    int inputLeft_ = 0;
    Piece expected_{};  // where the active piece should be after the last action; gravity moving it voids the plan
    Piece target_{};    // where the current plan locks the piece; a replan for the same piece aims there again
};

}  // namespace tetris
