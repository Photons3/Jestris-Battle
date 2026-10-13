#include "core/piece.hpp"

namespace tetris {
namespace {

struct Shape {
    int boxSize;
    std::array<Point, 4> spawn;  // rotation Zero, box coordinates
};

// Guideline spawn orientations. Index matches PieceType.
constexpr std::array<Shape, kPieceTypeCount> kShapes{{
    {4, {{{0, 1}, {1, 1}, {2, 1}, {3, 1}}}},  // I
    {4, {{{1, 0}, {2, 0}, {1, 1}, {2, 1}}}},  // O
    {3, {{{1, 0}, {0, 1}, {1, 1}, {2, 1}}}},  // T
    {3, {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}}},  // S
    {3, {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}}},  // Z
    {3, {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}}},  // J
    {3, {{{2, 0}, {0, 1}, {1, 1}, {2, 1}}}},  // L
}};

}  // namespace

std::array<Point, 4> shapeCells(PieceType type, Rotation rotation) {
    const Shape& shape = kShapes[static_cast<int>(type)];
    std::array<Point, 4> cells = shape.spawn;
    if (type == PieceType::O) return cells;
    // Each clockwise quarter turn inside an n×n box maps (x, y) to (n-1-y, x).
    for (int turn = 0; turn < static_cast<int>(rotation); ++turn)
        for (Point& c : cells) c = Point{shape.boxSize - 1 - c.y, c.x};
    return cells;
}

std::array<Point, 4> Piece::cells() const {
    std::array<Point, 4> result = shapeCells(type, rotation);
    for (Point& c : result) c = Point{pos.x + c.x, pos.y - c.y};
    return result;
}

Piece spawnPiece(PieceType type) { return Piece{type, Rotation::Zero, kSpawnPos}; }

Rotation rotatedCW(Rotation rotation) { return static_cast<Rotation>((static_cast<int>(rotation) + 1) % 4); }

Rotation rotatedCCW(Rotation rotation) { return static_cast<Rotation>((static_cast<int>(rotation) + 3) % 4); }

}  // namespace tetris
