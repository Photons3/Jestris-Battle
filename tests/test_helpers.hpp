#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

#include "core/bag.hpp"
#include "core/game.hpp"
#include "core/piece.hpp"

// Cells in a canonical order so shapes can be compared regardless of listing order.
inline std::array<tetris::Point, 4> sortedCells(std::array<tetris::Point, 4> cells) {
    std::sort(cells.begin(), cells.end(),
              [](tetris::Point a, tetris::Point b) { return std::tie(a.x, a.y) < std::tie(b.x, b.y); });
    return cells;
}

// First seed whose bag deals `type` first, so a Game built with it starts with that piece.
inline std::uint32_t seedWithFirst(tetris::PieceType type) {
    for (std::uint32_t seed = 0;; ++seed)
        if (tetris::Bag(seed).peek(1)[0] == type) return seed;
}

// First seed whose bag deals `first` and then `second`.
inline std::uint32_t seedWithFirstTwo(tetris::PieceType first, tetris::PieceType second) {
    for (std::uint32_t seed = 0;; ++seed) {
        const std::vector<tetris::PieceType> dealt = tetris::Bag(seed).peek(2);
        if (dealt[0] == first && dealt[1] == second) return seed;
    }
}

inline int countFilled(const tetris::Board& board) {
    int count = 0;
    for (int y = 0; y < tetris::kBoardHeight; ++y)
        for (int x = 0; x < tetris::kBoardWidth; ++x) count += board.at(x, y).has_value() ? 1 : 0;
    return count;
}

// Soft-drops the active piece until it rests on something. The lock timer is still 0 afterwards.
inline void dropToFloor(tetris::Game& game) {
    game.setSoftDrop(true);
    for (int i = 0; i < 100000 && game.ghostDistance() > 0; ++i) game.tick(1);
    game.setSoftDrop(false);
}

// Board whose bottom `rows` rows are full except column `gapX`, with one more block on top at the far side, so
// clearing the well is not a perfect clear.
inline tetris::Board boardWithWell(int rows, int gapX) {
    tetris::Board board;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < tetris::kBoardWidth; ++x)
            if (x != gapX) board.set(x, y, tetris::PieceType::Z);
    board.set(gapX == 0 ? tetris::kBoardWidth - 1 : 0, rows, tetris::PieceType::Z);
    return board;
}

// Turns the freshly spawned I piece upright (it then fills column 5) and hard-drops it.
inline void dropUprightI(tetris::Game& game) {
    game.apply(tetris::Action::RotateCW);
    game.apply(tetris::Action::HardDrop);
}
