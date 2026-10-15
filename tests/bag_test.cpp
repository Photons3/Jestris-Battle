#include <doctest/doctest.h>

#include <array>

#include "core/bag.hpp"

using namespace tetris;

TEST_CASE("bag: every group of seven holds each piece exactly once") {
    Bag bag(42);
    for (int group = 0; group < 10; ++group) {
        std::array<int, kPieceTypeCount> counts{};
        for (int i = 0; i < kPieceTypeCount; ++i) ++counts[static_cast<int>(bag.next())];
        for (int count : counts) CHECK(count == 1);
    }
}

TEST_CASE("bag: the same seed gives the same sequence") {
    Bag a(7);
    Bag b(7);
    for (int i = 0; i < 70; ++i) CHECK(a.next() == b.next());
}

TEST_CASE("bag: peek shows upcoming pieces without consuming them") {
    Bag bag(3);
    for (int round = 0; round < 3; ++round) {  // also crosses bag boundaries
        const auto upcoming = bag.peek(5);
        REQUIRE(upcoming.size() == 5);
        for (PieceType type : upcoming) CHECK(bag.next() == type);
    }
}

TEST_CASE("bag: a seed gives the same sequence on every standard library") {
    // Golden list from an explicit Fisher-Yates over std::mt19937 (both fully specified).
    const PieceType golden[14] = {PieceType::I, PieceType::S, PieceType::L, PieceType::T, PieceType::O,
                                  PieceType::Z, PieceType::J, PieceType::I, PieceType::T, PieceType::Z,
                                  PieceType::O, PieceType::S, PieceType::J, PieceType::L};
    Bag bag(7);
    for (PieceType expected : golden) CHECK(bag.next() == expected);
}
