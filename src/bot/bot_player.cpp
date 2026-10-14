#include "bot/bot_player.hpp"

#include <algorithm>
#include <vector>

#include "bot/planner.hpp"

namespace tetris {

BotPreset presetFor(BotLevel level) {
    switch (level) {
        case BotLevel::Easy: return {900, 120, 0.25};
        case BotLevel::Medium: return {450, 60, 0.08};
        case BotLevel::Hard: return {200, 25, 0.0};
    }
    return {200, 25, 0.0};
}

std::string botName(BotLevel level) {
    switch (level) {
        case BotLevel::Easy: return "Bot-Easy";
        case BotLevel::Medium: return "Bot-Medium";
        case BotLevel::Hard: return "Bot-Hard";
    }
    return "Bot-Hard";
}

BotPlayer::BotPlayer(BotLevel level, std::uint32_t seed)
    : preset_(presetFor(level)), rng_(seed), thinkLeft_(preset_.thinkMs) {}

void BotPlayer::reset() {
    queue_.clear();
    thinkLeft_ = preset_.thinkMs;
    inputLeft_ = 0;
}

void BotPlayer::update(Game& game, int ms) {
    for (int i = 0; i < ms; ++i) step(game);
}

void BotPlayer::step(Game& game) {
    if (game.state() != GameState::Playing) return;
    // The plan was made for the piece where it stood; if gravity has carried it elsewhere (or the plan's
    // Hold swapped the piece), the remaining shifts would miss, so plan again from where it is.
    bool replanned = false;
    if (!queue_.empty()) {
        const Piece& now = game.active();
        if (now.type != expected_.type || now.rotation != expected_.rotation || now.pos.x != expected_.pos.x ||
            now.pos.y != expected_.pos.y) {
            queue_.clear();
            thinkLeft_ = 0;
            replanned = true;
        }
    }
    if (queue_.empty()) {
        if (thinkLeft_ > 0) --thinkLeft_;
        if (thinkLeft_ > 0) return;
        const std::vector<Placement> ranked = rankPlacements(game);
        if (ranked.empty()) return;
        std::size_t pick = 0;
        if (std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < preset_.mistakeRate)
            pick = std::min<std::size_t>(ranked.size() - 1, 1 + std::uniform_int_distribution<int>(0, 1)(rng_));
        queue_.assign(ranked[pick].actions.begin(), ranked[pick].actions.end());
        if (!replanned) inputLeft_ = 0;
        thinkLeft_ = preset_.thinkMs;
    }
    if (inputLeft_ > 0) --inputLeft_;
    if (inputLeft_ > 0) return;
    const Action action = queue_.front();
    queue_.pop_front();
    inputLeft_ = preset_.inputMs;
    game.apply(action);
    expected_ = game.active();
}

}  // namespace tetris
