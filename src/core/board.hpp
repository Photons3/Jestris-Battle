#pragma once

#include <array>
#include <optional>
#include <random>
#include <vector>

#include "core/piece.hpp"

namespace tetris {

inline constexpr int kBoardWidth = 10;
inline constexpr int kBoardHeight = 40;
inline constexpr int kVisibleHeight = 20;

using Cell = std::optional<PieceType>;

// 10×40 grid. y = 0 is the bottom row; rows 20–39 are the hidden buffer above the visible field.
class Board {
public:
    static bool inBounds(int x, int y) { return x >= 0 && x < kBoardWidth && y >= 0 && y < kBoardHeight; }

    Cell at(int x, int y) const;
    void set(int x, int y, Cell cell);
    bool collides(const Piece& piece) const;
    void lock(const Piece& piece);
    // Complete rows, bottom-up. A row holding a bomb is never complete; it clears only by detonation.
    std::vector<int> fullRows() const;
    void removeRows(const std::vector<int>& rows);
    // Pushes the stack up by `rows` and fills the bottom rows with garbage, with a bomb in column `bombX`.
    // Returns false and changes nothing if a filled cell would be pushed off the top.
    bool pushGarbage(int rows, int bombX);
    // The same, one garbage row per entry, bottom row first, each with its bomb in the given column.
    bool pushGarbage(const std::vector<int>& bombColumns);
    // Fills rows 0..rows-1 with garbage, one hole each; a row's hole differs from the one below it.
    // Expects an empty board above those rows. No bombs.
    void pushCheese(int rows, std::mt19937& rng);
    // Number of rows holding at least one garbage cell (bombs and pieces don't count).
    int garbageRows() const;
    // Rows a just-locked `piece` sets off: each bomb directly under one of its cells, plus every bomb
    // touching that one below it in the same column. Sorted bottom-up, without duplicates.
    std::vector<int> bombRowsHitBy(const Piece& piece) const;

private:
    static int index(int x, int y) { return y * kBoardWidth + x; }

    std::array<Cell, kBoardWidth * kBoardHeight> cells_{};
};

}  // namespace tetris
