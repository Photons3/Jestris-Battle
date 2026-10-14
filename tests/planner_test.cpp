#include <algorithm>

#include <doctest/doctest.h>

#include "bot/planner.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("planner: an O piece on an empty board has 9 places to land") {
    const Game game(seedWithFirst(PieceType::O), 1);
    const auto ranked = rankPlacements(game);
    const auto own = std::count_if(ranked.begin(), ranked.end(),
                                   [](const Placement& p) { return p.actions.front() != Action::Hold; });
    CHECK(own == 9);
    for (const Placement& p : ranked) CHECK(p.actions.back() == Action::HardDrop);
}

TEST_CASE("planner: fills the gap in an almost full row") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    const Placement best = rankPlacements(game).front();
    for (Action a : best.actions) game.apply(a);
    CHECK(game.state() == GameState::Clearing);
}

TEST_CASE("planner: prefers setting off a bomb") {
    Board board;
    for (int x = 0; x < kBoardWidth; ++x) board.set(x, 0, x == 8 ? PieceType::Bomb : PieceType::Garbage);
    Game game(seedWithFirst(PieceType::O), 1, board);
    const Placement best = rankPlacements(game).front();
    const auto cells = best.landed.cells();
    CHECK(std::any_of(cells.begin(), cells.end(), [](Point c) { return c.x == 8 && c.y == 1; }));
}

TEST_CASE("planner: hold candidates only while holding is allowed") {
    Game game(seedWithFirst(PieceType::T), 1);
    auto holds = [&] { auto r = rankPlacements(game);
        return std::any_of(r.begin(), r.end(), [](const Placement& p) { return p.actions.front() == Action::Hold; }); };
    CHECK(holds());
    game.apply(Action::Hold);
    CHECK_FALSE(holds());
}

TEST_CASE("planner: planned moves land exactly where planned") {
    const Game start(seedWithFirst(PieceType::T), 1, boardWithWell(2, 0));
    for (const Placement& p : rankPlacements(start)) {
        Game game = start;
        for (Action a : p.actions) {
            if (a == Action::HardDrop) {  // check the landing spot just before the drop locks it
                Piece dropped = game.active();
                dropped.pos.y -= game.ghostDistance();
                CHECK(sortedCells(dropped.cells()) == sortedCells(p.landed.cells()));
            }
            game.apply(a);
        }
    }
}
