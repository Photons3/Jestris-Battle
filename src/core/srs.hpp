#pragma once

#include <optional>

#include "core/board.hpp"
#include "core/piece.hpp"

namespace tetris {

enum class RotateDir { CW, CCW };

// Rotates `piece`, trying the five SRS kick offsets in order. Returns nullopt if every one collides.
std::optional<Piece> rotateWithKicks(const Board& board, const Piece& piece, RotateDir dir);

}  // namespace tetris
