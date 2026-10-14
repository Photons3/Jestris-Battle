#include <doctest/doctest.h>

#include <vector>

#include "app/solo.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("solo: each mode's rules") {
    CHECK(rulesFor(SoloMode::Sprint).goalLines == 40);
    CHECK(rulesFor(SoloMode::Sprint).countdown);
    CHECK(rulesFor(SoloMode::Blitz).timeLimitMs == 120000);
    CHECK(rulesFor(SoloMode::Cheese).countdown);
    CHECK_FALSE(rulesFor(SoloMode::Zen).countdown);
    CHECK_FALSE(rulesFor(SoloMode::Marathon).countdown);
    CHECK(makeSoloGame(SoloMode::Sprint, 1, 9).level() == 1);
    CHECK(makeSoloGame(SoloMode::Zen, 1, 9).level() == 9);
    CHECK(makeSoloGame(SoloMode::Cheese, 1, 9).board().garbageRows() == 10);
}

TEST_CASE("solo: the countdown blocks play, ticks each second, then starts") {
    SoloSession solo(rulesFor(SoloMode::Sprint), makeSoloGame(SoloMode::Sprint, 1, 1));
    CHECK(solo.phase() == SoloPhase::Countdown);
    CHECK(solo.frozen());
    solo.update(1000);
    solo.update(1000);
    solo.update(999);
    CHECK(solo.phase() == SoloPhase::Countdown);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::Playing);
    CHECK(solo.clockMs() == 0);
    CHECK(solo.takeEvents() == std::vector{SoloEvent::CountdownTick, SoloEvent::CountdownTick,
                                           SoloEvent::CountdownTick, SoloEvent::Start});
}

TEST_CASE("solo: pausing during the countdown does nothing") {
    SoloSession solo(rulesFor(SoloMode::Blitz), makeSoloGame(SoloMode::Blitz, 1, 1));
    solo.pause();
    CHECK(solo.game().state() == GameState::Playing);
}

TEST_CASE("solo: sprint finishes at the goal with the time") {
    SoloSession solo({SoloMode::Sprint, 4}, Game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5)));
    solo.update(250);
    dropUprightI(solo.game());
    solo.game().tick(kClearDelayMs);
    solo.update(kClearDelayMs);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == 250 + kClearDelayMs);
    CHECK(solo.frozen());
}

TEST_CASE("solo: blitz ends at 0:00 and still counts a clear in progress") {
    SoloSession solo({SoloMode::Blitz, 0, 1000}, Game(seedWithFirst(PieceType::I), 1, boardWithWell(1, 5)));
    dropUprightI(solo.game());
    REQUIRE(solo.game().state() == GameState::Clearing);
    solo.update(1000);
    CHECK(solo.game().lines() == 1);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == solo.game().score());
}

TEST_CASE("solo: cheese finishes when the last garbage row clears") {
    Board board = boardWithWell(1, 5);       // then turn row 0's blocks into garbage
    for (int x = 0; x < kBoardWidth; ++x) if (board.at(x, 0)) board.set(x, 0, PieceType::Garbage);
    SoloSession solo({SoloMode::Cheese}, Game(seedWithFirst(PieceType::I), 1, board));
    dropUprightI(solo.game());
    solo.game().tick(kClearDelayMs);
    solo.update(kClearDelayMs);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == kClearDelayMs);
}

TEST_CASE("solo: a top-out ends the run with no result") {
    SoloSession solo({SoloMode::Sprint, 40}, Game(1, 1));
    for (int i = 0; i < 100 && solo.game().state() != GameState::GameOver; ++i) solo.game().apply(Action::HardDrop);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::ToppedOut);
    CHECK_FALSE(solo.result());
}

TEST_CASE("solo: zen clears the board and plays on after a top-out") {
    SoloSession solo({SoloMode::Zen}, Game(1, 1));
    for (int i = 0; i < 100 && solo.game().state() != GameState::GameOver; ++i) solo.game().apply(Action::HardDrop);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::Playing);
    CHECK(solo.game().state() == GameState::Playing);
    CHECK(countFilled(solo.game().board()) == 0);
}

TEST_CASE("solo: the clock stands still while paused") {
    SoloSession solo({SoloMode::Sprint, 40}, Game(1, 1));
    solo.update(100);
    solo.pause();
    solo.update(500);
    CHECK(solo.clockMs() == 100);
}

TEST_CASE("solo: personal bests keep the lower time and the higher score") {
    PersonalBests bests;
    CHECK(bests.submit(SoloMode::Sprint, 60000));
    CHECK_FALSE(bests.submit(SoloMode::Sprint, 61000));
    CHECK(bests.submit(SoloMode::Sprint, 59000));
    CHECK(bests.best(SoloMode::Sprint) == 59000);
    CHECK(bests.submit(SoloMode::Blitz, 1000));
    CHECK_FALSE(bests.submit(SoloMode::Blitz, 900));
    CHECK(bests.submit(SoloMode::Blitz, 1100));
    CHECK_FALSE(bests.best(SoloMode::Zen));
    CHECK_FALSE(bests.submit(SoloMode::Marathon, 5));
}
