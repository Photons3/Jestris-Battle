#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <vector>

#include "core/game.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("game: gravity follows the guideline curve and caps at level 20") {
    CHECK(gravityMsPerRow(1) == doctest::Approx(1000.0));
    CHECK(gravityMsPerRow(2) == doctest::Approx(793.0));
    CHECK(gravityMsPerRow(20) == doctest::Approx(0.4554).epsilon(0.001));
    CHECK(gravityMsPerRow(25) == gravityMsPerRow(20));
}

TEST_CASE("game: line-clear score and level formulas") {
    CHECK(lineClearScore(1, 1) == 100);
    CHECK(lineClearScore(2, 1) == 300);
    CHECK(lineClearScore(3, 1) == 500);
    CHECK(lineClearScore(4, 1) == 800);
    CHECK(lineClearScore(4, 3) == 2400);
    CHECK(levelFor(1, 9) == 1);
    CHECK(levelFor(1, 10) == 2);
    CHECK(levelFor(5, 25) == 7);
}

TEST_CASE("game: the first piece spawns at the spawn position") {
    Game game(seedWithFirst(PieceType::T), 1);
    CHECK(game.state() == GameState::Playing);
    CHECK(game.active().type == PieceType::T);
    CHECK(game.active().pos == kSpawnPos);
    CHECK(game.ghostDistance() == 20);
}

TEST_CASE("game: gravity drops one row per second at level 1") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.tick(999);
    CHECK(game.active().pos.y == 21);
    game.tick(1);
    CHECK(game.active().pos.y == 20);
}

TEST_CASE("game: level 20 drops two rows in one millisecond, and higher levels are no faster") {
    for (int level : {20, 25}) {
        Game game(seedWithFirst(PieceType::T), level);
        game.tick(1);
        CHECK(game.active().pos.y == 19);
    }
}

TEST_CASE("game: soft drop is 20x gravity and scores 1 per row") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.setSoftDrop(true);
    game.tick(50);
    CHECK(game.active().pos.y == 20);
    CHECK(game.score() == 1);
}

TEST_CASE("game: the soft-drop factor sets how much faster soft drop falls") {
    for (int factor : {5, 40}) {
        Game game(seedWithFirst(PieceType::T), 1);
        game.setSoftDropFactor(factor);
        game.setSoftDrop(true);
        game.tick(1000 / factor - 1);
        CHECK(game.active().pos.y == 21);
        game.tick(1);
        CHECK(game.active().pos.y == 20);
    }
}

TEST_CASE("game: instant soft drop reaches the floor in one tick, scores 1 per row and leaves the lock delay") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.setSoftDropFactor(kSoftDropInstant);
    game.setSoftDrop(true);
    game.tick(1);
    CHECK(game.ghostDistance() == 0);
    CHECK(game.score() == 20);
    CHECK(countFilled(game.board()) == 0);
    game.tick(kLockDelayMs - 1);
    CHECK(countFilled(game.board()) == 0);
    game.tick(1);
    CHECK(countFilled(game.board()) == 4);
}

TEST_CASE("game: instant soft drop also drops a piece swapped in from hold") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.setSoftDropFactor(kSoftDropInstant);
    game.setSoftDrop(true);
    game.tick(1);
    game.apply(Action::Hold);
    REQUIRE(game.ghostDistance() > 0);
    game.tick(1);
    CHECK(game.ghostDistance() == 0);
}

TEST_CASE("game: moving stops at the wall") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::Left);
    CHECK(game.active().pos.x == 2);
    for (int i = 0; i < 5; ++i) game.apply(Action::Left);
    CHECK(game.active().pos.x == 0);
}

TEST_CASE("game: a wall shift moves all the way to the wall") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::RightWall);
    CHECK(game.active().pos.x == 7);
    game.apply(Action::LeftWall);
    CHECK(game.active().pos.x == 0);
}

TEST_CASE("game: a wall shift stops at a block in the way") {
    Board board;
    board.set(8, 20, PieceType::Z);  // in the spawned T's row, two columns right of it
    Game game(seedWithFirst(PieceType::T), 1, board);
    REQUIRE(game.active().pos.y == 21);  // the flat of the T is row 20
    game.apply(Action::RightWall);
    CHECK(game.active().pos.x == 5);
}

TEST_CASE("game: a wall shift restarts the lock delay once, like a single move") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    for (int i = 0; i < kMaxLockResets; ++i) {
        game.tick(100);
        game.apply(i % 2 == 0 ? Action::LeftWall : Action::RightWall);
    }
    game.tick(100);
    game.apply(Action::LeftWall);  // 16th move: timer keeps running
    game.tick(kLockDelayMs - 101);
    CHECK(countFilled(game.board()) == 0);
    game.tick(1);
    CHECK(countFilled(game.board()) == 4);
}

TEST_CASE("game: rotating uses SRS") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::RotateCW);
    CHECK(game.active().rotation == Rotation::R);
    game.apply(Action::RotateCCW);
    CHECK(game.active().rotation == Rotation::Zero);
}

TEST_CASE("game: hard drop locks at once, scores 2 per row, and spawns the next piece") {
    Game game(seedWithFirst(PieceType::T), 1);
    const PieceType nextType = game.next(1)[0];
    game.apply(Action::HardDrop);
    CHECK(game.score() == 40);
    CHECK(countFilled(game.board()) == 4);
    CHECK(game.active().type == nextType);
    CHECK(game.active().pos == kSpawnPos);
}

TEST_CASE("game: a grounded piece locks after 500 ms") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    game.tick(kLockDelayMs - 1);
    CHECK(countFilled(game.board()) == 0);
    game.tick(1);
    CHECK(countFilled(game.board()) == 4);
}

TEST_CASE("game: moving a grounded piece restarts the lock delay") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    game.tick(400);
    game.apply(Action::Left);
    game.tick(400);
    CHECK(countFilled(game.board()) == 0);
    game.tick(100);
    CHECK(countFilled(game.board()) == 4);
}

TEST_CASE("game: the lock delay restarts at most 15 times per piece") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    for (int i = 0; i < kMaxLockResets; ++i) {
        game.tick(100);
        game.apply(i % 2 == 0 ? Action::Left : Action::Right);
    }
    game.tick(100);
    game.apply(Action::Right);  // 16th move: timer keeps running
    game.tick(kLockDelayMs - 101);
    CHECK(countFilled(game.board()) == 0);
    game.tick(1);
    CHECK(countFilled(game.board()) == 4);
}

TEST_CASE("game: reaching a new lowest row gives the piece fresh lock resets") {
    Board ledge;  // 6 wide, 5 tall platform on the left
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 6; ++x) ledge.set(x, y, PieceType::Z);
    Game game(seedWithFirst(PieceType::T), 1, ledge);
    dropToFloor(game);
    REQUIRE(game.active().pos.y == 6);  // resting on the platform
    for (int i = 0; i < kMaxLockResets; ++i) {
        game.tick(100);
        game.apply(i % 2 == 0 ? Action::Left : Action::Right);
    }
    for (int i = 0; i < 4; ++i) {  // walk right off the platform without resets
        game.tick(10);
        game.apply(Action::Right);
    }
    REQUIRE(game.ghostDistance() > 0);
    dropToFloor(game);
    game.tick(400);
    game.apply(Action::Right);  // counter was reset by the fall, so this restarts the timer
    game.tick(kLockDelayMs - 1);
    CHECK(countFilled(game.board()) == 30);
    game.tick(1);
    CHECK(countFilled(game.board()) == 34);
}

TEST_CASE("game: hold works once per piece and swaps back in spawn orientation") {
    Game game(11, 1);
    const PieceType first = game.active().type;
    const PieceType second = game.next(1)[0];
    game.apply(Action::RotateCW);
    game.apply(Action::Hold);
    CHECK(game.hold() == first);
    CHECK(game.active().type == second);
    CHECK_FALSE(game.canHold());
    game.apply(Action::Hold);  // ignored until the next lock
    CHECK(game.active().type == second);
    game.apply(Action::HardDrop);
    CHECK(game.canHold());
    const PieceType third = game.active().type;
    game.apply(Action::Hold);
    CHECK(game.active().type == first);
    CHECK(game.active().rotation == Rotation::Zero);
    CHECK(game.active().pos == kSpawnPos);
    CHECK(game.hold() == third);
}

TEST_CASE("game: a four-line clear flashes for 300 ms, ignores input, then scores 800 at level 1") {
    Board well;  // rows 0-3 full except column 0, and one block above them so it is not a perfect clear
    for (int y = 0; y < 4; ++y)
        for (int x = 1; x < kBoardWidth; ++x) well.set(x, y, PieceType::Z);
    well.set(9, 4, PieceType::Z);
    Game game(seedWithFirst(PieceType::I), 1, well);
    game.apply(Action::RotateCW);                       // vertical I in column 5
    for (int i = 0; i < 5; ++i) game.apply(Action::Left);  // column 0
    REQUIRE(game.active().pos.x == -2);
    game.apply(Action::HardDrop);  // 18 rows: +36
    CHECK(game.state() == GameState::Clearing);
    CHECK(game.clearingRows() == std::vector<int>{0, 1, 2, 3});
    CHECK(game.clearElapsedMs() == 0);
    game.apply(Action::Hold);
    CHECK_FALSE(game.hold().has_value());
    game.tick(kClearDelayMs - 1);
    CHECK(game.state() == GameState::Clearing);
    CHECK(game.clearElapsedMs() == kClearDelayMs - 1);
    CHECK(game.score() == 36);
    game.tick(1);
    CHECK(game.state() == GameState::Playing);
    CHECK(game.score() == 836);
    CHECK(game.lines() == 4);
    CHECK(game.level() == 1);
    CHECK(countFilled(game.board()) == 1);
}

TEST_CASE("game: block-out ends the game when the spawn area is occupied") {
    Board board;
    board.set(4, 20, PieceType::Z);
    Game game(seedWithFirst(PieceType::T), 1, board);
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("game: lock-out ends the game when a piece locks above the visible field") {
    Board board;  // rows 0-19 full except column 9, so nothing clears
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth - 1; ++x) board.set(x, y, PieceType::Z);
    Game game(seedWithFirst(PieceType::T), 1, board);
    REQUIRE(game.state() == GameState::Playing);
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("game: the Pause action freezes the game and toggles back") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::Pause);
    CHECK(game.state() == GameState::Paused);
    game.apply(Action::Left);
    game.tick(5000);
    CHECK(game.active().pos == kSpawnPos);
    game.apply(Action::Pause);
    CHECK(game.state() == GameState::Playing);
}

TEST_CASE("game: pause and resume are idempotent and never leave game over") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.resume();
    CHECK(game.state() == GameState::Playing);
    game.pause();
    game.pause();
    CHECK(game.state() == GameState::Paused);
    game.resume();
    CHECK(game.state() == GameState::Playing);

    Board blocked;
    blocked.set(4, 20, PieceType::Z);
    Game over(seedWithFirst(PieceType::T), 1, blocked);
    over.pause();
    CHECK(over.state() == GameState::GameOver);
}

TEST_CASE("game: start level is clamped to the selectable range 1-15") {
    CHECK(clampStartLevel(0) == kMinStartLevel);
    CHECK(clampStartLevel(-3) == kMinStartLevel);
    CHECK(clampStartLevel(7) == 7);
    CHECK(clampStartLevel(50000000) == kMaxStartLevel);
    CHECK(kMinStartLevel == 1);
    CHECK(kMaxStartLevel == 15);
}

TEST_CASE("game: attack table sends 0/1/2/4 lines for 1/2/3/4 clears") {
    CHECK(attackFor(1) == 0);
    CHECK(attackFor(2) == 1);
    CHECK(attackFor(3) == 2);
    CHECK(attackFor(4) == 4);
}

TEST_CASE("game: a tetris sends four lines, once") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 4);
    CHECK(game.takeOutgoingAttack() == 4);
    CHECK(game.takeOutgoingAttack() == 0);
}

TEST_CASE("game: a double sends one line") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(2, 5));
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 2);
    CHECK(game.takeOutgoingAttack() == 1);
}

TEST_CASE("game: clearing lines cancels pending garbage before sending any") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    SUBCASE("attack larger than pending") {
        game.receiveGarbage(3);
        dropUprightI(game);
        game.tick(kClearDelayMs);
        CHECK(game.pendingGarbage() == 0);
        CHECK(game.takeOutgoingAttack() == 1);
    }
    SUBCASE("attack smaller than pending") {
        game.receiveGarbage(5);
        dropUprightI(game);
        game.tick(kClearDelayMs);
        CHECK(game.pendingGarbage() == 1);
        CHECK(game.takeOutgoingAttack() == 0);
    }
}

TEST_CASE("game: pending garbage rises when a piece locks without clearing") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(2);
    CHECK(game.pendingGarbage() == 2);
    game.apply(Action::HardDrop);
    CHECK(game.pendingGarbage() == 0);
    CHECK(game.state() == GameState::Playing);
    CHECK(countFilled(game.board()) == 4 + 2 * kBoardWidth);
    for (int y = 0; y < 2; ++y) {  // one bomb per row, the rest plain garbage
        int bombs = 0;
        for (int x = 0; x < kBoardWidth; ++x) {
            const Cell cell = game.board().at(x, y);
            CHECK((cell == PieceType::Bomb || cell == PieceType::Garbage));
            bombs += cell == PieceType::Bomb ? 1 : 0;
        }
        CHECK(bombs == 1);
    }
}

namespace {

// Bombs stacked in `columns` (bottom row first): the longest run in one column.
int longestBombRun(const std::vector<int>& columns, int aboveX = -1, int aboveRun = 0) {
    int longest = aboveRun;
    int run = aboveRun;
    int previous = aboveX;
    for (auto it = columns.rbegin(); it != columns.rend(); ++it) {
        run = *it == previous ? run + 1 : 1;
        previous = *it;
        longest = std::max(longest, run);
    }
    return longest;
}

}  // namespace

TEST_CASE("game: bombs stack at most two high, in every batch and against the row above") {
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        const int rows = 1 + i % 12;
        const int aboveX = i % 3 == 0 ? -1 : i % kBoardWidth;
        const int aboveRun = aboveX < 0 ? 0 : 1 + i % 3;
        const std::vector<int> columns = bombColumnsFor(rows, aboveX, aboveRun, rng);
        REQUIRE(static_cast<int>(columns.size()) == rows);
        for (int x : columns) CHECK((x >= 0 && x < kBoardWidth));
        CHECK(longestBombRun(columns, aboveX, std::min(aboveRun, 2)) <= 2);
        if (aboveRun >= 2) CHECK(columns.back() != aboveX);  // the top new row never makes a third
    }
}

TEST_CASE("game: a bomb pairs with a lone bomb above it about 90% of the time") {
    std::mt19937 rng(11);
    int lone = 0;
    int paired = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::vector<int> columns = bombColumnsFor(1, i % kBoardWidth, 1, rng);
        ++lone;
        paired += columns[0] == i % kBoardWidth ? 1 : 0;
    }
    CHECK(static_cast<double>(paired) / lone == doctest::Approx(kBombPairPercent / 100.0).epsilon(0.03));
}

TEST_CASE("game: rising garbage never stacks a third bomb on a pair") {
    Board board;
    board.pushGarbage(2, 9);  // a pair in column 9 at the bottom, away from where the T lands
    Game game(seedWithFirst(PieceType::T), 1, board);
    game.receiveGarbage(1);
    game.apply(Action::HardDrop);  // the T lands on the garbage and the new row goes under the pair
    REQUIRE(game.board().at(9, 1) == PieceType::Bomb);
    REQUIRE(game.board().at(9, 2) == PieceType::Bomb);
    CHECK(game.board().at(9, 0) == PieceType::Garbage);
}

TEST_CASE("game: a clearing lock holds garbage back until the next lock") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(1, 5));
    game.receiveGarbage(1);
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 1);
    CHECK(game.pendingGarbage() == 1);
    CHECK(countFilled(game.board()) == 3 + 1);  // the rest of the I and the well's extra block; no garbage yet
    game.apply(Action::HardDrop);
    CHECK(game.pendingGarbage() == 0);
    CHECK(countFilled(game.board()) == 3 + 1 + 4 + kBoardWidth);
}

TEST_CASE("game: garbage that pushes blocks off the top ends the game") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("game: garbage that leaves no room to spawn ends the game") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(25);
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("game: pausing can be turned off for duels") {
    Game game(seedWithFirst(PieceType::T), 1, Board{}, GameOptions{.allowPause = false});
    game.apply(Action::Pause);
    CHECK(game.state() == GameState::Playing);
    game.pause();
    CHECK(game.state() == GameState::Playing);
}

namespace {

// `bombRows` garbage rows with a bomb in column `bombX`, and above them `openRows` rows that are full except
// columns 4-5, which is where a freshly spawned O lands. One block on top in column 0 keeps any clear from
// being a perfect clear.
Board bombBoard(int bombRows, int bombX, int openRows) {
    Board board;
    board.pushGarbage(bombRows, bombX);
    for (int y = bombRows; y < bombRows + openRows; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 4 && x != 5) board.set(x, y, PieceType::Z);
    board.set(0, bombRows + openRows, PieceType::Z);
    return board;
}

// Rows 0-3 are full except column 5, where an upright I clears all four. The two rows above are open only at
// columns 4-5, so after that clear they are the bottom rows, and an O dropped from spawn clears them. A block
// on top in column 0 is left over, so neither clear is a perfect clear.
Board comboBoard() {
    Board board;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) board.set(x, y, PieceType::Z);
    for (int y = 4; y < 6; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 4 && x != 5) board.set(x, y, PieceType::Z);
    board.set(0, 6, PieceType::Z);
    return board;
}

// Two bomb rows (bomb in column 4) under four rows open only at column 5. An upright I clears the four, then
// an O dropped from spawn lands on the bomb.
Board comboBombBoard() {
    Board board;
    board.pushGarbage(2, 4);
    for (int y = 2; y < 6; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) board.set(x, y, PieceType::Z);
    return board;
}

}  // namespace

TEST_CASE("game: landing on a bomb clears its whole stack and counts as cleared lines") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::Clearing);
    CHECK(game.clearingRows() == std::vector<int>{0, 1});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 2);
    CHECK(game.takeOutgoingAttack() == 1);
    CHECK(countFilled(game.board()) == 4 + 1);  // the O and the extra block, dropped to the floor
}

TEST_CASE("game: landing beside a bomb sets nothing off") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 7, 0));
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::Playing);
    CHECK(game.lines() == 0);
    CHECK(countFilled(game.board()) == 2 * kBoardWidth + 4 + 1);
}

TEST_CASE("game: a bomb hit and a normal clear in one lock add up to one attack") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(1, 4, 2));
    game.apply(Action::HardDrop);
    CHECK(game.clearingRows() == std::vector<int>{0, 1, 2});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 3);
    CHECK(game.takeOutgoingAttack() == 2);
}

TEST_CASE("game: more than four cleared rows are capped at the four-line attack and score") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(4, 4, 2));
    const int distance = game.ghostDistance();
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 6);
    CHECK(game.takeOutgoingAttack() == 4);
    CHECK(game.score() == 2 * distance + lineClearScore(4, 1));
    CHECK(game.backToBack() == -1);  // scored like a tetris, but bomb rows don't start a back-to-back chain
}

TEST_CASE("game: a detonating lock cancels pending garbage and holds the rest back") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
    game.receiveGarbage(3);
    game.apply(Action::HardDrop);
    CHECK(game.pendingGarbage() == 3);
    game.tick(kClearDelayMs);
    CHECK(game.pendingGarbage() == 2);  // the 1-line attack cancelled one
    CHECK(game.takeOutgoingAttack() == 0);
    CHECK(countFilled(game.board()) == 4 + 1);  // nothing rose
}

TEST_CASE("game: a clear that drops a cell onto a bomb does not detonate it") {
    Board board;
    board.pushGarbage(1, 4);                                           // row 0: bomb in column 4
    for (int x = 0; x < kBoardWidth; ++x)
        if (x != 5) board.set(x, 1, PieceType::Z);                     // row 1: open only in column 5
    board.set(4, 2, PieceType::Z);                                     // falls onto the bomb after the clear
    Game game(seedWithFirst(PieceType::I), 1, board);
    dropUprightI(game);                                                // fills column 5, resting on garbage
    CHECK(game.clearingRows() == std::vector<int>{1});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 1);
    CHECK(game.board().at(4, 0) == PieceType::Bomb);
    CHECK(game.board().at(4, 1) == PieceType::Z);
}

TEST_CASE("game: resetBoard after a top-out empties everything but keeps score, lines and level") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    game.tick(kClearDelayMs);  // a tetris: 4 lines
    game.apply(Action::Hold);
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    REQUIRE(game.state() == GameState::GameOver);
    const int score = game.score();
    const PieceType next = game.next(1)[0];

    game.resetBoard();
    CHECK(game.state() == GameState::Playing);
    CHECK(countFilled(game.board()) == 0);
    CHECK(game.pendingGarbage() == 0);
    CHECK_FALSE(game.hold().has_value());
    CHECK(game.canHold());
    CHECK(game.score() == score);
    CHECK(game.lines() == 4);
    CHECK(game.level() == 1);
    CHECK(game.active().type == next);
    CHECK(game.active().pos == kSpawnPos);
}

TEST_CASE("game: resetBoard during a clear drops the clear") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.state() == GameState::Clearing);
    game.resetBoard();
    CHECK(game.state() == GameState::Playing);
    CHECK(game.clearingRows().empty());
    CHECK(game.lines() == 0);
    CHECK(countFilled(game.board()) == 0);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 0);  // the dropped clear never finishes
}

TEST_CASE("game: combo counts consecutive clearing locks and resets on a lock that clears nothing") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    CHECK(game.combo() == -1);
    dropUprightI(game);
    CHECK(game.combo() == 0);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);  // the O completes the two rows left behind
    CHECK(game.combo() == 1);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);  // lands on an empty floor
    CHECK(game.combo() == -1);
}

TEST_CASE("game: a lock that only sets off a bomb keeps the combo going") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBombBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);
    CHECK(game.clearingRows() == std::vector<int>{0, 1});
    CHECK(game.combo() == 1);
}

TEST_CASE("game: resetBoard ends the combo") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.combo() == 0);
    game.resetBoard();
    CHECK(game.combo() == -1);
}

TEST_CASE("game: combo bonus follows the guideline table: 1,1,2,2,3,3,4,4,4 and then 5") {
    CHECK(comboBonus(-1) == 0);
    CHECK(comboBonus(0) == 0);
    const std::vector<int> expected{1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5};
    for (int combo = 1; combo <= static_cast<int>(expected.size()); ++combo) {
        CAPTURE(combo);
        CHECK(comboBonus(combo) == expected[combo - 1]);
    }
    CHECK(comboBonus(40) == 5);
}

TEST_CASE("game: a combo clear sends its bonus on top of the line attack") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.takeOutgoingAttack() == 4);  // combo 0: no bonus
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.takeOutgoingAttack() == attackFor(2) + comboBonus(1));
}

TEST_CASE("game: pending garbage cancels a combo bonus like any other attack") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    game.takeOutgoingAttack();
    game.receiveGarbage(1);
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.pendingGarbage() == 0);
    CHECK(game.takeOutgoingAttack() == 1);  // 1 line + 1 bonus, minus the 1 cancelled
}

TEST_CASE("game: guideline points for T-spins, back-to-back, combos and perfect clears") {
    CHECK(awardScore({0}, -1, 1) == 0);
    CHECK(awardScore({4}, 0, 1) == 800);
    CHECK(awardScore({0, Spin::Mini}, -1, 1) == 100);
    CHECK(awardScore({1, Spin::Mini}, 0, 1) == 200);
    CHECK(awardScore({2, Spin::Mini}, 0, 1) == 400);
    CHECK(awardScore({0, Spin::Full}, -1, 1) == 400);
    CHECK(awardScore({1, Spin::Full}, 0, 1) == 800);
    CHECK(awardScore({2, Spin::Full}, 0, 1) == 1200);
    CHECK(awardScore({3, Spin::Full}, 0, 3) == 1600 * 3);
    CHECK(awardScore({4, Spin::None, 1}, 0, 1) == 1200);         // back-to-back is worth 1.5x
    CHECK(awardScore({2, Spin::Full, 2}, 0, 2) == 1800 * 2);
    CHECK(awardScore({1}, 3, 2) == (100 + 150) * 2);             // 50 per combo step
    CHECK(awardScore({0, Spin::Full}, 3, 1) == 400);             // no lines, no combo points
    CHECK(awardScore({1, Spin::None, 0, true}, 0, 1) == 100 + 800);
    CHECK(awardScore({2, Spin::None, 0, true}, 0, 1) == 300 + 1200);
    CHECK(awardScore({3, Spin::None, 0, true}, 0, 1) == 500 + 1800);
    CHECK(awardScore({4, Spin::None, 0, true}, 0, 1) == 800 + 2000);
    CHECK(awardScore({4, Spin::None, 1, true}, 0, 1) == 1200 + 3200);
}

TEST_CASE("game: guideline garbage for T-spins, back-to-back, combos and perfect clears") {
    CHECK(awardAttack({0, Spin::Full}, -1) == 0);
    CHECK(awardAttack({1, Spin::Mini}, 0) == 0);
    CHECK(awardAttack({2, Spin::Mini}, 0) == 1);
    CHECK(awardAttack({1, Spin::Full}, 0) == 2);
    CHECK(awardAttack({2, Spin::Full}, 0) == 4);
    CHECK(awardAttack({3, Spin::Full}, 0) == 6);
    CHECK(awardAttack({4}, 0) == 4);
    CHECK(awardAttack({4, Spin::None, 1}, 0) == 5);  // back-to-back adds one line
    CHECK(awardAttack({2, Spin::Full, 3}, 0) == 5);
    CHECK(awardAttack({2}, 2) == 1 + comboBonus(2));
    CHECK(awardAttack({1, Spin::None, 0, true}, 0) == 10);
    CHECK(awardAttack({4, Spin::None, 0, true}, 0) == 14);
    CHECK(awardAttack({0, Spin::None, 0, false, 6}, 0) == 4);  // bomb rows send like lines, up to a tetris
    CHECK(awardAttack({1, Spin::None, 0, false, 2}, 0) == 2);
}

TEST_CASE("game: only tetrises and T-spins that clear lines are difficult") {
    CHECK(isDifficult({4}));
    CHECK(isDifficult({1, Spin::Mini}));
    CHECK(isDifficult({2, Spin::Full}));
    CHECK_FALSE(isDifficult({3}));
    CHECK_FALSE(isDifficult({0, Spin::Full}));
    CHECK_FALSE(isDifficult({0, Spin::None, 0, false, 4}));  // four bomb rows are not a tetris
}

TEST_CASE("game: the three-corner rule tells full T-spins from minis") {
    const Piece upOnFloor{PieceType::T, Rotation::Zero, {3, 1}};  // center (4, 0): the floor fills both low corners
    Board board;
    CHECK(tSpinFor(board, upOnFloor, 0) == Spin::None);  // two corners
    board.set(3, 1, PieceType::Z);                        // a third corner, behind the T's point
    CHECK(tSpinFor(board, upOnFloor, 0) == Spin::Mini);
    CHECK(tSpinFor(board, upOnFloor, 4) == Spin::Full);   // the last SRS kick always makes it full
    board.set(5, 1, PieceType::Z);                        // both corners the T points to
    CHECK(tSpinFor(board, upOnFloor, 0) == Spin::Full);
    CHECK(tSpinFor(board, Piece{PieceType::J, Rotation::Zero, {3, 1}}, 0) == Spin::None);
}

namespace {

// A T-spin double slot: row 0 is full except column 4, row 1 except columns 3-5, and row 2 has a roof over
// column 3 (columns 0-3 filled). A T rotated at spawn, soft-dropped and rotated again spins into the slot.
Board tSpinDoubleBoard() {
    Board board;
    for (int x = 0; x < kBoardWidth; ++x) {
        if (x != 4) board.set(x, 0, PieceType::Z);
        if (x < 3 || x > 5) board.set(x, 1, PieceType::Z);
    }
    for (int x = 0; x <= 3; ++x) board.set(x, 2, PieceType::Z);
    return board;
}

// Four rows open only at column 4 under the T-spin double slot, whose open cells sit in column 4 too. An upright
// I moved one column left clears the four, which drops the slot to the floor for the T that comes next.
Board backToBackBoard() {
    Board board;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 4) board.set(x, y, PieceType::Z);
    const Board slot = tSpinDoubleBoard();
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < kBoardWidth; ++x) board.set(x, y + 4, slot.at(x, y));
    return board;
}

// Four bomb rows (bomb in column 4) under four rows open only at column 5, and one leftover block on top. An
// upright I clears the four rows, then an O dropped from spawn sets off all four bombs.
Board bombTetrisBoard() {
    Board board;
    board.pushGarbage(4, 4);
    for (int y = 4; y < 8; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) board.set(x, y, PieceType::Z);
    board.set(0, 8, PieceType::Z);
    return board;
}

}  // namespace

TEST_CASE("game: a T-spin double scores 1200 and sends four lines") {
    Game game(seedWithFirst(PieceType::T), 1, tSpinDoubleBoard());
    game.apply(Action::RotateCW);
    dropToFloor(game);
    game.apply(Action::RotateCW);
    const int before = game.score();
    game.takeEvents();
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == std::vector<GameEvent>{{GameEventType::HardDropped},
                                                      {GameEventType::LinesCleared, 2},
                                                      {GameEventType::SpecialClear, 0, {2, Spin::Full}}});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 2);
    CHECK(game.score() == before + 1200);
    CHECK(game.takeOutgoingAttack() == 4);
    CHECK(game.backToBack() == 0);
}

TEST_CASE("game: a T-spin that clears nothing still scores, sends nothing and keeps the chain") {
    Board board = tSpinDoubleBoard();
    board.set(9, 0, std::nullopt);  // neither row can complete now
    board.set(9, 1, std::nullopt);
    Game game(seedWithFirst(PieceType::T), 1, board);
    game.apply(Action::RotateCW);
    dropToFloor(game);
    game.apply(Action::RotateCW);
    const int before = game.score();
    game.takeEvents();
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == std::vector<GameEvent>{{GameEventType::HardDropped},
                                                      {GameEventType::SpecialClear, 0, {0, Spin::Full}}});
    CHECK(game.state() == GameState::Playing);
    CHECK(game.score() == before + 400);
    CHECK(game.takeOutgoingAttack() == 0);
    CHECK(game.backToBack() == -1);
}

TEST_CASE("game: a T that drops into the slot without spinning is no T-spin") {
    Game game(seedWithFirst(PieceType::T), 1, tSpinDoubleBoard());
    game.apply(Action::RotateCW);
    dropToFloor(game);  // the rotation is followed by falling, so it no longer counts
    game.takeEvents();
    game.apply(Action::HardDrop);
    for (const GameEvent& event : game.takeEvents()) CHECK(event.type != GameEventType::SpecialClear);
}

TEST_CASE("game: a tetris then a T-spin double is back-to-back") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::T), 1, backToBackBoard());
    game.apply(Action::RotateCW);
    game.apply(Action::Left);  // the I drops down column 4
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    REQUIRE(game.lines() == 4);
    CHECK(game.takeOutgoingAttack() == 4);
    CHECK(game.backToBack() == 0);
    game.apply(Action::RotateCW);
    dropToFloor(game);
    game.apply(Action::RotateCW);
    const int before = game.score();
    game.takeEvents();
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == std::vector<GameEvent>{{GameEventType::HardDropped},
                                                      {GameEventType::LinesCleared, 2},
                                                      {GameEventType::Combo, 1},
                                                      {GameEventType::SpecialClear, 0, {2, Spin::Full, 1}}});
    game.tick(kClearDelayMs);
    CHECK(game.backToBack() == 1);
    CHECK(game.score() == before + 1200 * 3 / 2 + 50);
    CHECK(game.takeOutgoingAttack() == 4 + 1 + comboBonus(1));
}

TEST_CASE("game: a bomb-only clear neither continues nor breaks the back-to-back chain") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, bombTetrisBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    REQUIRE(game.backToBack() == 0);
    REQUIRE(game.takeOutgoingAttack() == 4);
    game.takeEvents();
    game.apply(Action::HardDrop);  // the O sets off four bombs: scored like a tetris, but no full rows
    CHECK(game.takeEvents() == std::vector<GameEvent>{{GameEventType::HardDropped},
                                                      {GameEventType::BombExploded},
                                                      {GameEventType::Combo, 1}});
    game.tick(kClearDelayMs);
    CHECK(game.backToBack() == 0);
    CHECK(game.takeOutgoingAttack() == 4 + comboBonus(1));  // no back-to-back line
}

TEST_CASE("game: a plain line clear ends the back-to-back chain, a lock that clears nothing does not") {
    SUBCASE("a double after a tetris") {
        Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
        dropUprightI(game);
        game.tick(kClearDelayMs);
        REQUIRE(game.backToBack() == 0);
        game.apply(Action::HardDrop);
        CHECK(game.backToBack() == -1);
    }
    SUBCASE("an empty lock after a tetris") {
        Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
        dropUprightI(game);
        game.tick(kClearDelayMs);
        game.apply(Action::HardDrop);
        CHECK(game.lines() == 4);
        CHECK(game.backToBack() == 0);
    }
}

TEST_CASE("game: resetBoard ends the back-to-back chain") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.backToBack() == 0);
    game.resetBoard();
    CHECK(game.backToBack() == -1);
}

TEST_CASE("game: a clear that empties the board is a perfect clear: 2000 more points and ten more lines") {
    Board well;  // rows 0-3 full except column 5, and nothing else
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) well.set(x, y, PieceType::Z);
    Game game(seedWithFirst(PieceType::I), 1, well);
    game.apply(Action::RotateCW);
    const int distance = game.ghostDistance();
    game.takeEvents();
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == std::vector<GameEvent>{{GameEventType::HardDropped},
                                                      {GameEventType::LinesCleared, 4},
                                                      {GameEventType::SpecialClear, 0, {4, Spin::None, 0, true}}});
    game.tick(kClearDelayMs);
    CHECK(countFilled(game.board()) == 0);
    CHECK(game.score() == 2 * distance + 800 + 2000);
    CHECK(game.takeOutgoingAttack() == 4 + 10);
}

namespace {

using Events = std::vector<GameEvent>;
constexpr GameEvent kMoved{GameEventType::Moved};
constexpr GameEvent kRotated{GameEventType::Rotated};
constexpr GameEvent kHeld{GameEventType::Held};
constexpr GameEvent kHardDropped{GameEventType::HardDropped};
constexpr GameEvent kLocked{GameEventType::Locked};
constexpr GameEvent kBombExploded{GameEventType::BombExploded};
constexpr GameEvent kGarbageRose{GameEventType::GarbageRose};
constexpr GameEvent kLevelUp{GameEventType::LevelUp};
constexpr GameEvent kToppedOut{GameEventType::ToppedOut};
constexpr GameEvent cleared(int lines) { return {GameEventType::LinesCleared, lines}; }
constexpr GameEvent comboOf(int n) { return {GameEventType::Combo, n}; }
constexpr GameEvent special(ClearAward award) { return {GameEventType::SpecialClear, 0, award}; }
constexpr GameEvent kTetris = special({4});

}  // namespace

TEST_CASE("game events: moves, rotations and holds are reported only when they happen") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::RotateCW);
    game.apply(Action::Hold);
    game.apply(Action::Hold);  // refused until the next lock
    CHECK(game.takeEvents() == Events{kRotated, kHeld});
    CHECK(game.takeEvents().empty());
}

TEST_CASE("game events: a blocked move is silent") {
    Game game(seedWithFirst(PieceType::T), 1);
    for (int i = 0; i < 5; ++i) game.apply(Action::Left);  // only 3 columns to the wall
    CHECK(game.takeEvents() == Events{kMoved, kMoved, kMoved});
}

TEST_CASE("game events: a wall shift reports one move, and nothing once at the wall") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::LeftWall);
    CHECK(game.takeEvents() == Events{kMoved});
    game.apply(Action::LeftWall);
    CHECK(game.takeEvents().empty());
}

TEST_CASE("game events: a hard drop reports the drop, not a lock") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped});
}

TEST_CASE("game events: a hard-dropped tetris reports four full rows at once") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    CHECK(game.takeEvents() == Events{kRotated, kHardDropped, cleared(4), kTetris});
    game.tick(kClearDelayMs);
    CHECK(game.takeEvents().empty());
}

TEST_CASE("game events: a piece that locks on its own reports Locked") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    CHECK(game.takeEvents().empty());
    game.tick(kLockDelayMs);
    CHECK(game.takeEvents() == Events{kLocked});
}

TEST_CASE("game events: bomb rows are reported apart from full rows") {
    SUBCASE("bombs only") {
        Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, kBombExploded});
    }
    SUBCASE("bombs and full rows") {
        Game game(seedWithFirst(PieceType::O), 1, bombBoard(1, 4, 2));
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, cleared(2), kBombExploded});
    }
}

TEST_CASE("game events: garbage rising after a lock is reported") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(2);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, kGarbageRose});
}

TEST_CASE("game events: reaching the next level is reported when the clear finishes") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(10, 4, 0));  // ten bomb rows clear as ten lines
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, kBombExploded});  // bomb rows are never called a tetris
    game.tick(kClearDelayMs);
    CHECK(game.level() == 2);
    CHECK(game.takeEvents() == Events{kLevelUp});
}

TEST_CASE("game events: every kind of top-out is reported once") {
    SUBCASE("block-out") {
        Board board;
        board.set(4, 20, PieceType::Z);
        Game game(seedWithFirst(PieceType::T), 1, board);
        CHECK(game.takeEvents() == Events{kToppedOut});
    }
    SUBCASE("lock-out") {
        Board board;
        for (int y = 0; y < kVisibleHeight; ++y)
            for (int x = 0; x < kBoardWidth - 1; ++x) board.set(x, y, PieceType::Z);
        Game game(seedWithFirst(PieceType::T), 1, board);
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
    }
    SUBCASE("garbage overflow") {
        Game game(seedWithFirst(PieceType::T), 1);
        game.receiveGarbage(kBoardHeight - 1);
        game.apply(Action::HardDrop);
        game.tick(1000);
        CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
    }
}

TEST_CASE("game events: resetting the board keeps events not yet taken") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    game.resetBoard();
    CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
}

TEST_CASE("game events: a combo is reported from the second clear in a row") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    CHECK(game.takeEvents() == Events{kRotated, kHardDropped, cleared(4), kTetris});
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, cleared(2), comboOf(1)});
}

TEST_CASE("game: lines per level is settable") {
    CHECK(levelFor(1, 9) == 1);
    CHECK(levelFor(1, 10) == 2);
    CHECK(levelFor(1, 5, 5) == 2);
    CHECK(levelFor(3, 12, 5) == 5);
}

TEST_CASE("game: a faster level curve levels up after fewer lines") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5), GameOptions{.linesPerLevel = 2});
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 4);
    CHECK(game.level() == 3);
}
