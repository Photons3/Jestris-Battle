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

void Board::pushCheese(int rows, std::mt19937& rng) {
    int previousHole = -1;
    for (int y = 0; y < rows; ++y) {
        int hole = static_cast<int>(rng() % kBoardWidth);
        while (hole == previousHole) hole = static_cast<int>(rng() % kBoardWidth);
        previousHole = hole;
        for (int x = 0; x < kBoardWidth; ++x) {
            if (x != hole) set(x, y, PieceType::Garbage);
        }
    }
}

int Board::garbageRows() const {
    int count = 0;
    for (int y = 0; y < kBoardHeight; ++y) {
        for (int x = 0; x < kBoardWidth; ++x) {
            if (at(x, y) == PieceType::Garbage) {
                ++count;
                break;
            }
        }
    }
    return count;
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
    assert(rows >= 0);
    return pushGarbage(std::vector<int>(static_cast<std::size_t>(rows), bombX));
}

bool Board::pushGarbage(const std::vector<int>& bombColumns) {
    return pushGarbageRows(bombColumns, Cell{PieceType::Bomb});
}

bool Board::pushGarbageHoles(const std::vector<int>& holeColumns) {
    return pushGarbageRows(holeColumns, Cell{});
}

bool Board::pushGarbageRows(const std::vector<int>& columns, Cell special) {
    const int rows = std::min(static_cast<int>(columns.size()), kBoardHeight);
    for (int y = kBoardHeight - rows; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (cells_[index(x, y)]) return false;
    for (int y = kBoardHeight - 1; y >= rows; --y)
        for (int x = 0; x < kBoardWidth; ++x) cells_[index(x, y)] = cells_[index(x, y - rows)];
    for (int y = 0; y < rows; ++y) {
        const int specialX = columns[static_cast<std::size_t>(y)];
        assert(specialX >= 0 && specialX < kBoardWidth);
        for (int x = 0; x < kBoardWidth; ++x)
            cells_[index(x, y)] = x == specialX ? special : Cell{PieceType::Garbage};
    }
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
