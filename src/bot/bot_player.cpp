#include "bot/bot_player.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "bot/planner.hpp"

namespace tetris {

namespace {

// A piece's cells in a fixed order, so two pieces covering the same cells compare equal.
std::array<std::pair<int, int>, 4> sortedCells(const Piece& piece) {
    std::array<std::pair<int, int>, 4> cells{};
    const std::array<Point, 4> points = piece.cells();
    for (std::size_t i = 0; i < points.size(); ++i) cells[i] = {points[i].x, points[i].y};
    std::sort(cells.begin(), cells.end());
    return cells;
}

}  // namespace

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
    // The plan was made for the piece where it stood. If gravity has carried it lower, the remaining shifts
    // would miss, so plan again from where it is, for the same target. If gravity locked it and a new piece
    // spawned while actions were still queued, plan for the new piece.
    bool replanned = false;
    bool samePiece = false;
    if (!queue_.empty()) {
        const Piece& now = game.active();
        if (now.type != expected_.type || now.rotation != expected_.rotation || now.pos.x != expected_.pos.x ||
            now.pos.y != expected_.pos.y) {
            samePiece = now.type == expected_.type && now.rotation == expected_.rotation &&
                        now.pos.x == expected_.pos.x && now.pos.y < expected_.pos.y;
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
        if (samePiece) {
            // The mistake was rolled when this piece was first planned; keep aiming where that plan went.
            const auto same = std::find_if(ranked.begin(), ranked.end(), [&](const Placement& p) {
                return sortedCells(p.landed) == sortedCells(target_);
            });
            if (same != ranked.end()) pick = static_cast<std::size_t>(same - ranked.begin());
        } else if (std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < preset_.mistakeRate) {
            pick = std::min<std::size_t>(ranked.size() - 1, 1 + std::uniform_int_distribution<int>(0, 1)(rng_));
        }
        queue_.assign(ranked[pick].actions.begin(), ranked[pick].actions.end());
        target_ = ranked[pick].landed;
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
