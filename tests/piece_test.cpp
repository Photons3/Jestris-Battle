#include <doctest/doctest.h>

#include "core/piece.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("piece: T spawns centered in rows 20-21") {
    CHECK(sortedCells(spawnPiece(PieceType::T).cells()) == sortedCells({{{3, 20}, {4, 20}, {5, 20}, {4, 21}}}));
}

TEST_CASE("piece: I spawns flat in columns 3-6 of row 20") {
    CHECK(sortedCells(spawnPiece(PieceType::I).cells()) == sortedCells({{{3, 20}, {4, 20}, {5, 20}, {6, 20}}}));
}

TEST_CASE("piece: J and L spawn with their corner up") {
    CHECK(sortedCells(spawnPiece(PieceType::J).cells()) == sortedCells({{{3, 21}, {3, 20}, {4, 20}, {5, 20}}}));
    CHECK(sortedCells(spawnPiece(PieceType::L).cells()) == sortedCells({{{5, 21}, {3, 20}, {4, 20}, {5, 20}}}));
}

TEST_CASE("piece: O occupies columns 4-5 and looks the same in every rotation") {
    Piece o = spawnPiece(PieceType::O);
    const auto expected = sortedCells({{{4, 20}, {5, 20}, {4, 21}, {5, 21}}});
    for (Rotation r : {Rotation::Zero, Rotation::R, Rotation::Two, Rotation::L}) {
        o.rotation = r;
        CHECK(sortedCells(o.cells()) == expected);
    }
}

TEST_CASE("piece: SRS rotation states in box coordinates") {
    CHECK(sortedCells(shapeCells(PieceType::T, Rotation::R)) == sortedCells({{{1, 0}, {1, 1}, {2, 1}, {1, 2}}}));
    CHECK(sortedCells(shapeCells(PieceType::T, Rotation::Two)) == sortedCells({{{0, 1}, {1, 1}, {2, 1}, {1, 2}}}));
    CHECK(sortedCells(shapeCells(PieceType::T, Rotation::L)) == sortedCells({{{1, 0}, {0, 1}, {1, 1}, {1, 2}}}));
    CHECK(sortedCells(shapeCells(PieceType::I, Rotation::R)) == sortedCells({{{2, 0}, {2, 1}, {2, 2}, {2, 3}}}));
    CHECK(sortedCells(shapeCells(PieceType::I, Rotation::L)) == sortedCells({{{1, 0}, {1, 1}, {1, 2}, {1, 3}}}));
}

TEST_CASE("piece: rotation helpers wrap around") {
    CHECK(rotatedCW(Rotation::L) == Rotation::Zero);
    CHECK(rotatedCCW(Rotation::Zero) == Rotation::L);
    for (Rotation r : {Rotation::Zero, Rotation::R, Rotation::Two, Rotation::L})
        CHECK(rotatedCCW(rotatedCW(r)) == r);
}
