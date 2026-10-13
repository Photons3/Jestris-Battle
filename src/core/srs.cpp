#include "core/srs.hpp"

#include <array>

namespace tetris {
namespace {

using Kicks = std::array<Point, 5>;
using KickTable = std::array<std::array<Kicks, 2>, 4>;  // [from rotation][0 = CW, 1 = CCW]

// Offsets use +y = up, as on the SRS reference tables.
constexpr KickTable kJlstzKicks{{
    {{{{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}}, {{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}}}},    // from 0
    {{{{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}}, {{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}}}},          // from R
    {{{{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}}, {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}}}},    // from 2
    {{{{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}}, {{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}}}},  // from L
}};

constexpr KickTable kIKicks{{
    {{{{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}}, {{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}}}},  // from 0
    {{{{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}}, {{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}}}},  // from R
    {{{{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}}, {{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}}}},  // from 2
    {{{{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}}, {{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}}}},  // from L
}};

}  // namespace

std::optional<Piece> rotateWithKicks(const Board& board, const Piece& piece, RotateDir dir) {
    Piece rotated = piece;
    rotated.rotation = dir == RotateDir::CW ? rotatedCW(piece.rotation) : rotatedCCW(piece.rotation);
    if (piece.type == PieceType::O) {
        if (board.collides(rotated)) return std::nullopt;
        return rotated;
    }
    const KickTable& table = piece.type == PieceType::I ? kIKicks : kJlstzKicks;
    for (Point kick : table[static_cast<int>(piece.rotation)][dir == RotateDir::CW ? 0 : 1]) {
        Piece candidate = rotated;
        candidate.pos.x += kick.x;
        candidate.pos.y += kick.y;
        if (!board.collides(candidate)) return candidate;
    }
    return std::nullopt;
}

}  // namespace tetris
