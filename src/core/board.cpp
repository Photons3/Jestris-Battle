#include "core/board.hpp"

#include <algorithm>
#include <cassert>

namespace tetris {

Cell Board::at(int x, int y) const {
    assert(inBounds(x, y));
    return cells_[index(x, y)];
}

void Board::set(int x, int y, Cell cell) {
    assert(inBounds(x, y));
    cells_[index(x, y)] = cell;
}

bool Board::collides(const Piece& piece) const {
    for (Point c : piece.cells())
        if (!inBounds(c.x, c.y) || cells_[index(c.x, c.y)]) return true;
    return false;
}

void Board::lock(const Piece& piece) {
    for (Point c : piece.cells()) set(c.x, c.y, piece.type);
}

std::vector<int> Board::fullRows() const {
    std::vector<int> rows;
    for (int y = 0; y < kBoardHeight; ++y) {
        bool full = true;
        for (int x = 0; x < kBoardWidth && full; ++x) {
            const Cell cell = cells_[index(x, y)];
            full = cell.has_value() && *cell != PieceType::Bomb;
        }
        if (full) rows.push_back(y);
    }
    return rows;
}

void Board::removeRows(const std::vector<int>& rows) {
    int dst = 0;
    for (int src = 0; src < kBoardHeight; ++src) {
        if (std::find(rows.begin(), rows.end(), src) != rows.end()) continue;
        if (dst != src)
            for (int x = 0; x < kBoardWidth; ++x) cells_[index(x, dst)] = cells_[index(x, src)];
        ++dst;
    }
    for (; dst < kBoardHeight; ++dst)
        for (int x = 0; x < kBoardWidth; ++x) cells_[index(x, dst)] = std::nullopt;
}

bool Board::pushGarbage(int rows, int bombX) {
    assert(rows >= 0 && bombX >= 0 && bombX < kBoardWidth);
    rows = std::min(rows, kBoardHeight);
    for (int y = kBoardHeight - rows; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (cells_[index(x, y)]) return false;
    for (int y = kBoardHeight - 1; y >= rows; --y)
        for (int x = 0; x < kBoardWidth; ++x) cells_[index(x, y)] = cells_[index(x, y - rows)];
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            cells_[index(x, y)] = x == bombX ? Cell{PieceType::Bomb} : Cell{PieceType::Garbage};
    return true;
}

std::vector<int> Board::bombRowsHitBy(const Piece& piece) const {
    std::vector<int> rows;
    for (Point c : piece.cells())
        for (int y = c.y - 1; y >= 0 && cells_[index(c.x, y)] == PieceType::Bomb; --y) rows.push_back(y);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

}  // namespace tetris
