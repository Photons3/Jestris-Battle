#pragma once

#include <optional>

#include "core/board.hpp"
#include "core/piece.hpp"

namespace tetris {

enum class RotateDir { CW, CCW };

// Rotates `piece`, trying the five SRS kick offsets in order. Returns nullopt if every one collides. On success
// `kick`, if given, gets the index of the offset that fit (0-4; always 0 for O).
std::optional<Piece> rotateWithKicks(const Board& board, const Piece& piece, RotateDir dir, int* kick = nullptr);

}  // namespace tetris
