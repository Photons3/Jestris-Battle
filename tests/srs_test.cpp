#include <doctest/doctest.h>

#include <initializer_list>

#include "core/srs.hpp"

using namespace tetris;

namespace {

// A board filled everywhere except the given cells.
Board filledExcept(std::initializer_list<Point> holes) {
    Board board;
    for (int y = 0; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x) board.set(x, y, PieceType::Z);
    for (Point h : holes) board.set(h.x, h.y, std::nullopt);
    return board;
}

}  // namespace

TEST_CASE("srs: rotation on an open board needs no kick") {
    const Board board;
    const Piece t{PieceType::T, Rotation::Zero, {4, 10}};
    const auto rotated = rotateWithKicks(board, t, RotateDir::CW);
    REQUIRE(rotated.has_value());
    CHECK(rotated->rotation == Rotation::R);
    CHECK(rotated->pos == Point{4, 10});
}

TEST_CASE("srs: T kicks right off the left wall (R -> 0)") {
    const Board board;
    const Piece t{PieceType::T, Rotation::R, {-1, 10}};
    REQUIRE_FALSE(board.collides(t));
    const auto rotated = rotateWithKicks(board, t, RotateDir::CCW);
    REQUIRE(rotated.has_value());
    CHECK(rotated->rotation == Rotation::Zero);
    CHECK(rotated->pos == Point{0, 10});  // second kick, (+1, 0)
}

TEST_CASE("srs: I kicks two columns off the left wall (R -> 2)") {
    const Board board;
    const Piece i{PieceType::I, Rotation::R, {-2, 10}};  // vertical in column 0
    REQUIRE_FALSE(board.collides(i));
    const auto rotated = rotateWithKicks(board, i, RotateDir::CW);
    REQUIRE(rotated.has_value());
    CHECK(rotated->rotation == Rotation::Two);
    CHECK(rotated->pos == Point{0, 10});  // third kick, (+2, 0)
}

TEST_CASE("srs: fifth kick is used when the first four are blocked (T-spin triple kick)") {
    // Holes = the T's current cells plus the cells of the target placement (offset -1, -2).
    const Board board = filledExcept({{5, 10}, {4, 9}, {5, 9}, {6, 9}, {4, 8}, {4, 7}, {5, 7}, {4, 6}});
    const Piece t{PieceType::T, Rotation::Zero, {4, 10}};
    REQUIRE_FALSE(board.collides(t));
    const auto rotated = rotateWithKicks(board, t, RotateDir::CW);
    REQUIRE(rotated.has_value());
    CHECK(rotated->rotation == Rotation::R);
    CHECK(rotated->pos == Point{3, 8});
}

TEST_CASE("srs: rotation fails when every kick is blocked") {
    const Board board = filledExcept({{5, 10}, {4, 9}, {5, 9}, {6, 9}});
    const Piece t{PieceType::T, Rotation::Zero, {4, 10}};
    CHECK_FALSE(rotateWithKicks(board, t, RotateDir::CW).has_value());
}

TEST_CASE("srs: O changes rotation state without moving") {
    const Board board;
    const auto rotated = rotateWithKicks(board, spawnPiece(PieceType::O), RotateDir::CW);
    REQUIRE(rotated.has_value());
    CHECK(rotated->rotation == Rotation::R);
    CHECK(rotated->pos == kSpawnPos);
}
