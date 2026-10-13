#pragma once

#include <array>
#include <cstdint>

namespace tetris {

// Garbage and Bomb only appear in board cells (sent by a duel opponent); the bag never deals them.
enum class PieceType : std::uint8_t { I, O, T, S, Z, J, L, Garbage, Bomb };
inline constexpr int kPieceTypeCount = 7;  // dealable types

enum class Rotation : std::uint8_t { Zero, R, Two, L };

struct Point {
    int x = 0;
    int y = 0;
    friend bool operator==(const Point&, const Point&) = default;
};

struct Piece {
    PieceType type = PieceType::I;
    Rotation rotation = Rotation::Zero;
    Point pos;  // board position of the shape box's top-left corner (board y grows upward)

    std::array<Point, 4> cells() const;  // board coordinates
};

inline constexpr Point kSpawnPos{3, 21};

// Cells inside the piece's SRS bounding box; box y grows downward.
std::array<Point, 4> shapeCells(PieceType type, Rotation rotation);
Piece spawnPiece(PieceType type);
Rotation rotatedCW(Rotation rotation);
Rotation rotatedCCW(Rotation rotation);

}  // namespace tetris
