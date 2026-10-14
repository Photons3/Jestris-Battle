#pragma once

#include <vector>

#include "core/board.hpp"
#include "core/game.hpp"
#include "core/piece.hpp"

namespace tetris {

inline constexpr double kHeightWeight = -0.51, kHolesWeight = -0.36, kBumpinessWeight = -0.18,
                        kLinesWeight = 0.76, kBombBonus = 0.9, kDangerPenalty = 1.0;
inline constexpr int kDangerHeight = 14;

struct Placement {
    std::vector<Action> actions;  // ends with HardDrop; starts with Hold for hold candidates
    Piece landed;                 // where the piece locks
    double score = 0.0;
};

// Scores the board after locking `piece` on it, clearing its full rows and the bomb rows it sets off.
double evaluateBoard(const Board& board, const Piece& piece);

// Every distinct hard-drop placement for the active piece (and the hold option when canHold()), best first.
std::vector<Placement> rankPlacements(const Game& game);

}  // namespace tetris
