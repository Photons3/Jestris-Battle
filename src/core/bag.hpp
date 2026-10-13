#pragma once

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "core/piece.hpp"

namespace tetris {

// Guideline 7-bag randomizer: each run of seven pieces is a shuffled set of all seven types.
class Bag {
public:
    explicit Bag(std::uint32_t seed);

    PieceType next();
    std::vector<PieceType> peek(int n) const;  // n <= kPieceTypeCount

private:
    void refill();

    std::mt19937 rng_;
    std::deque<PieceType> queue_;  // always holds at least kPieceTypeCount pieces
};

}  // namespace tetris
