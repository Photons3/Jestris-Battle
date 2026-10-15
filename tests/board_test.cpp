#include <doctest/doctest.h>

#include <random>
#include <vector>

#include "core/board.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("board: a spawned piece fits on an empty board") {
    Board board;
    CHECK_FALSE(board.collides(spawnPiece(PieceType::T)));
}

TEST_CASE("board: walls and floor collide") {
    Board board;
    Piece t = spawnPiece(PieceType::T);
    t.pos.x = -1;  // leftmost cell at x = -1
    CHECK(board.collides(t));
    t.pos.x = 8;  // rightmost cell at x = 10
    CHECK(board.collides(t));
    t.pos = {3, 0};  // bottom row at y = -1
    CHECK(board.collides(t));
    t.pos = {3, 1};  // resting on the floor
    CHECK_FALSE(board.collides(t));
}

TEST_CASE("board: an occupied cell collides") {
    Board board;
    board.set(4, 20, PieceType::Z);
    CHECK(board.collides(spawnPiece(PieceType::T)));
}

TEST_CASE("board: lock writes the piece type into its cells") {
    Board board;
    Piece l = spawnPiece(PieceType::L);
    l.pos.y = 1;
    board.lock(l);
    for (Point c : l.cells()) CHECK(board.at(c.x, c.y) == PieceType::L);
    CHECK_FALSE(board.at(0, 0).has_value());
}

TEST_CASE("board: fullRows lists complete rows bottom-up") {
    Board board;
    for (int x = 0; x < kBoardWidth; ++x) {
        board.set(x, 0, PieceType::I);
        board.set(x, 2, PieceType::I);
    }
    for (int x = 0; x < kBoardWidth - 1; ++x) board.set(x, 1, PieceType::I);
    CHECK(board.fullRows() == std::vector<int>{0, 2});
}

TEST_CASE("board: removeRows shifts the rows above down") {
    Board board;
    for (int x = 0; x < kBoardWidth; ++x) {
        board.set(x, 0, PieceType::I);
        board.set(x, 2, PieceType::I);
    }
    board.set(1, 1, PieceType::T);
    board.set(2, 3, PieceType::S);
    board.removeRows({0, 2});
    CHECK(board.at(1, 0) == PieceType::T);
    CHECK(board.at(2, 1) == PieceType::S);
    CHECK_FALSE(board.at(0, 0).has_value());
    CHECK_FALSE(board.at(1, 1).has_value());
    CHECK(board.fullRows().empty());
}

TEST_CASE("board: garbage pushes the stack up with a bomb in the same column of every row") {
    Board board;
    board.set(2, 0, PieceType::T);
    REQUIRE(board.pushGarbage(2, 7));
    CHECK(board.at(2, 2) == PieceType::T);
    CHECK_FALSE(board.at(2, 3).has_value());
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            CHECK(board.at(x, y) == (x == 7 ? PieceType::Bomb : PieceType::Garbage));
}

TEST_CASE("board: garbage rows can each put their bomb in a different column") {
    Board board;
    board.set(2, 0, PieceType::T);
    REQUIRE(board.pushGarbage(std::vector<int>{3, 3, 8}));  // bottom row first
    CHECK(board.at(2, 3) == PieceType::T);
    CHECK(board.at(3, 0) == PieceType::Bomb);
    CHECK(board.at(3, 1) == PieceType::Bomb);
    CHECK(board.at(8, 2) == PieceType::Bomb);
    CHECK(board.at(3, 2) == PieceType::Garbage);
    CHECK(countFilled(board) == 1 + 3 * kBoardWidth);
}

TEST_CASE("board: garbage that would push a block off the top is refused") {
    Board board;
    board.set(0, kBoardHeight - 1, PieceType::I);
    CHECK_FALSE(board.pushGarbage(1, 0));
    CHECK(board.at(0, kBoardHeight - 1) == PieceType::I);  // unchanged
    CHECK_FALSE(board.at(1, 0).has_value());
}

TEST_CASE("board: zero garbage rows change nothing") {
    Board board;
    board.set(4, 0, PieceType::O);
    CHECK(board.pushGarbage(0, 3));
    CHECK(board.at(4, 0) == PieceType::O);
}

namespace {

// An O piece whose lower-left cell is (x, bottomY); it fills columns x..x+1, rows bottomY..bottomY+1.
Piece oAt(int x, int bottomY) { return Piece{PieceType::O, Rotation::Zero, {x - 1, bottomY + 1}}; }

}  // namespace

TEST_CASE("board: a bomb row is never full") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    for (int x = 0; x < kBoardWidth; ++x) board.set(x, 1, PieceType::I);
    CHECK(board.fullRows() == std::vector<int>{1});
}

TEST_CASE("board: a piece resting directly on a bomb hits its row") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    CHECK(board.bombRowsHitBy(oAt(4, 1)) == std::vector<int>{0});
}

TEST_CASE("board: a piece beside a bomb, or above it with a gap, hits nothing") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    CHECK(board.bombRowsHitBy(oAt(2, 1)).empty());  // columns 2-3, beside the bomb
    CHECK(board.bombRowsHitBy(oAt(5, 1)).empty());  // columns 5-6, beside the bomb
    CHECK(board.bombRowsHitBy(oAt(4, 2)).empty());  // row 1 is empty between the piece and the bomb
}

TEST_CASE("board: the chain takes the bombs below and stops at the first non-bomb cell") {
    Board board;
    REQUIRE(board.pushGarbage(3, 4));  // older batch: bombs in column 4
    REQUIRE(board.pushGarbage(1, 6));  // newer batch underneath: column 4 is plain garbage in row 0
    CHECK(board.bombRowsHitBy(oAt(4, 4)) == std::vector<int>{1, 2, 3});
}

TEST_CASE("board: touching batches that share a column chain together") {
    Board board;
    REQUIRE(board.pushGarbage(2, 4));
    REQUIRE(board.pushGarbage(2, 4));
    CHECK(board.bombRowsHitBy(oAt(4, 4)) == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("board: cheese rows have one hole each, never stacked, and no bombs") {
    std::mt19937 rng(42);
    Board board;
    board.pushCheese(10, rng);
    int previousHole = -1;
    for (int y = 0; y < 10; ++y) {
        int holes = 0, hole = -1;
        for (int x = 0; x < kBoardWidth; ++x) {
            if (!board.at(x, y)) { ++holes; hole = x; }
            else CHECK(board.at(x, y) == PieceType::Garbage);
        }
        CHECK(holes == 1);
        CHECK(hole != previousHole);
        previousHole = hole;
    }
    for (int x = 0; x < kBoardWidth; ++x) CHECK_FALSE(board.at(x, 10));
    CHECK(board.garbageRows() == 10);
    CHECK(board.fullRows().empty());
}

TEST_CASE("board: garbage rows count rows with garbage, not bombs or pieces") {
    Board board;
    board.set(0, 0, PieceType::Garbage);
    board.set(1, 1, PieceType::Bomb);
    board.set(2, 2, PieceType::T);
    board.set(3, 3, PieceType::Garbage);
    CHECK(board.garbageRows() == 2);
}

TEST_CASE("board: pushGarbageHoles adds gray rows with one hole each") {
    Board board;
    REQUIRE(board.pushGarbageHoles({3, 3, 7}));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            CHECK(board.at(x, y) == (x == (y < 2 ? 3 : 7) ? Cell{} : Cell{PieceType::Garbage}));
    CHECK(board.fullRows().empty());
}

TEST_CASE("board: an overflowing pushGarbageHoles is refused and changes nothing") {
    Board board;
    board.set(0, kBoardHeight - 1, PieceType::I);
    const Board before = board;
    CHECK_FALSE(board.pushGarbageHoles({0}));
    CHECK(board == before);
}
