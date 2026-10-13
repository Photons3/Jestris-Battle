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
