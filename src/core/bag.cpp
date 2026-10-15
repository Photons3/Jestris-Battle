#include "core/bag.hpp"

#include <algorithm>
#include <array>
#include <cassert>

namespace tetris {

Bag::Bag(std::uint32_t seed) : rng_(seed) { refill(); }

PieceType Bag::next() {
    const PieceType type = queue_.front();
    queue_.pop_front();
    if (static_cast<int>(queue_.size()) < kPieceTypeCount) refill();
    return type;
}

std::vector<PieceType> Bag::peek(int n) const {
    assert(n >= 0 && n <= kPieceTypeCount);
    return {queue_.begin(), queue_.begin() + n};
}

void Bag::refill() {
    std::array<PieceType, kPieceTypeCount> bag{PieceType::I, PieceType::O, PieceType::T, PieceType::S,
                                               PieceType::Z, PieceType::J, PieceType::L};
    // Explicit Fisher-Yates: std::shuffle is implementation-defined and would break native vs WASM replay.
    for (int i = kPieceTypeCount - 1; i > 0; --i) std::swap(bag[i], bag[rng_() % (i + 1)]);
    queue_.insert(queue_.end(), bag.begin(), bag.end());
}

}  // namespace tetris
