#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "match_script.hpp"
#include "net/match_game.hpp"
#include "test_helpers.hpp"

using namespace tetris;

namespace {

MatchSetup duelSetup(std::uint32_t seed = 1) { return MatchSetup{StartMatch{seed, 1, {1, 2}}}; }
MatchSetup royaleSetup(std::uint32_t seed = 1) { return MatchSetup{StartMatch{seed, 1, {1, 2, 3}}}; }

// Hard-drops until topOuts() rises, advancing 1 ms after each drop.
void topOut(MatchGame& match) {
    const int before = match.topOuts();
    for (int i = 0; i < 100 && match.topOuts() == before; ++i) {
        match.input(Action::HardDrop);
        match.advance(1);
    }
    REQUIRE(match.topOuts() == before + 1);
}

}  // namespace

TEST_CASE("match_game: a duel KO drops the colored stack and flashes when a piece still fits") {
    const std::uint32_t seed = seedWithFirst(PieceType::O);
    MatchGame match(duelSetup(seed));
    Board board;
    board.pushGarbage(4, 2);
    board.set(0, 32, PieceType::S);  // one more row of garbage would push this off the top
    match.game() = Game(seed, 1, board, GameOptions{.allowPause = false});
    match.game().receiveGarbage(kMaxGarbagePerLock, 0);
    match.input(Action::HardDrop);
    REQUIRE(match.game().state() == GameState::GameOver);

    match.advance(1);

    CHECK(match.topOuts() == 1);
    CHECK_FALSE(match.buried());
    CHECK(match.game().state() == GameState::Playing);
    CHECK(match.game().board().at(4, 0) == PieceType::O);  // the locked O, dropped with the garbage rows
    CHECK(match.game().board().at(0, 28) == PieceType::S);
    CHECK(match.game().pendingGarbage() == 0);
    CHECK(match.koFlashMs() == kKoFlashMs);
    const int t0 = match.t();
    while (match.t() < t0 + kKoFlashMs) {
        REQUIRE(match.frozen());
        match.advance(1);
    }
    CHECK_FALSE(match.frozen());
    CHECK(match.topOuts() == 1);
}

TEST_CASE("match_game: a duel KO that still blocks the spawn stops after that one KO") {
    MatchGame match(duelSetup());
    topOut(match);
    const int t0 = match.t();
    CHECK(match.topOuts() == 1);
    CHECK(match.buried());
    CHECK(match.frozen());
    CHECK(match.game().state() == GameState::GameOver);
    CHECK(countFilled(match.game().board()) > 0);
    match.advance(kKoFlashMs + 1000);
    CHECK(match.t() == t0 + kKoFlashMs + 1000);
    CHECK(match.topOuts() == 1);
    CHECK(match.buried());
    CHECK(match.game().state() == GameState::GameOver);
}

TEST_CASE("match_game: a royale top-out eliminates for good") {
    MatchGame match(royaleSetup());
    CHECK(match.setup().royale());
    topOut(match);
    CHECK(match.eliminated());
    CHECK(match.topOuts() == 1);
    match.advance(60000);
    CHECK(match.frozen());
    CHECK(match.eliminated());
    CHECK(match.topOuts() == 1);
    CHECK(match.game().state() == GameState::GameOver);
}

TEST_CASE("match_game: the royale floor rises at exactly 30 s") {
    SUBCASE("1 ms steps") {
        MatchGame match(royaleSetup());
        while (match.t() < kRoyaleLevelStepMs - 1) match.advance(1);
        CHECK(match.game().level() == 1);
        match.advance(1);
        CHECK(match.game().level() == 2);
    }
    SUBCASE("one long advance") {
        MatchGame match(royaleSetup());
        match.advance(kRoyaleLevelStepMs - 1);
        CHECK(match.game().level() == 1);
        match.advance(1);
        CHECK(match.game().level() == 2);
    }
    SUBCASE("16 ms chunks") {
        MatchGame match(royaleSetup());
        while (match.t() < kRoyaleLevelStepMs - 1) match.advance(std::min(16, kRoyaleLevelStepMs - 1 - match.t()));
        CHECK(match.game().level() == 1);
        match.advance(1);
        CHECK(match.game().level() == 2);
    }
    SUBCASE("a duel has no floor") {
        MatchGame match(duelSetup());
        match.advance(kRoyaleLevelStepMs * 2);
        CHECK(match.game().level() == 1);
    }
}

TEST_CASE("match_game: time-up settles a clear and freezes") {
    const std::uint32_t seed = seedWithFirst(PieceType::I);
    MatchGame match(duelSetup(seed));
    match.advance(kMatchMs - 10);
    match.game() = Game(seed, 1, boardWithWell(4, 5), GameOptions{.allowPause = false});
    match.input(Action::RotateCW);
    match.input(Action::HardDrop);
    REQUIRE(match.game().state() == GameState::Clearing);
    CHECK_FALSE(match.frozen());
    match.advance(20);
    CHECK(match.game().state() != GameState::Clearing);
    CHECK(match.frozen());
    CHECK(match.game().takeOutgoingAttack() > 0);
    CHECK(match.koFlashMs() == 0);
    CHECK(match.topOuts() == 0);
}

TEST_CASE("match_game: input and garbage are logged at the current t, soft drop only on change") {
    MatchGame match(duelSetup());
    match.advance(5);
    match.input(Action::Left);
    match.input(Action::Pause);
    match.softDrop(true);
    match.softDrop(true);
    match.advance(3);
    match.acceptGarbage(7, 2);
    match.softDrop(false);
    match.softDrop(false);
    match.input(Action::RightWall);
    match.input(Action::HardDrop);
    CHECK(match.hasGarbage(7));
    CHECK_FALSE(match.hasGarbage(8));
    CHECK(match.game().pendingGarbage() == 2);
    const std::vector<LogEntry> expected{
        {5, LogCode::Left},           {5, LogCode::SoftOn},    {8, LogCode::Garbage, 7, 2},
        {8, LogCode::SoftOff},        {8, LogCode::RightWall}, {8, LogCode::HardDrop},
    };
    CHECK(match.log() == expected);
    CHECK(match.takeUnsent() == expected);
    CHECK(match.takeUnsent().empty());
    match.input(Action::RotateCCW);
    CHECK(match.takeUnsent() == std::vector<LogEntry>{{8, LogCode::RotateCCW}});
    CHECK(match.log().size() == expected.size() + 1);
}

TEST_CASE("match_game: replay matches live play however frames are split") {
    for (const bool royale : {false, true}) {
        CAPTURE(royale);
        const MatchSetup setup = scriptSetup(7, royale);
        const std::vector<LogEntry> events = scriptEvents(7);
        REQUIRE(events.size() > 3000);
        for (const int frameMs : {1, 16, 130000}) {
            CAPTURE(frameMs);
            MatchGame live(setup);
            runScript(live, events, frameMs, 130000);
            CHECK(live.t() == 130000);
            if (!royale) CHECK(live.frozen());  // time-up
            if (!royale) CHECK(live.topOuts() > 0);
            CHECK(MatchGame::replay(setup, live.log(), live.t()).hash() == live.hash());
        }
    }
}

TEST_CASE("match_game: replay stops at upTo, with the events stamped upTo applied") {
    const MatchSetup setup = scriptSetup(7, false);
    std::vector<LogEntry> events = scriptEvents(7);
    // An instant at which the script has events, plus a garbage batch stamped exactly then (as in a RESYNC <T>).
    const int upTo = std::find_if(events.begin(), events.end(), [](const LogEntry& e) { return e.t >= 20000; })->t;
    const auto firstLater = std::find_if(events.begin(), events.end(), [upTo](const LogEntry& e) { return e.t > upTo; });
    events.insert(firstLater, LogEntry{upTo, LogCode::Garbage, 99, 3});
    std::vector<LogEntry> upToEvents, later;
    for (const LogEntry& e : events) (e.t > upTo ? later : upToEvents).push_back(e);

    MatchGame live(setup);
    runScript(live, upToEvents, 1, upTo);
    REQUIRE(live.t() == upTo);
    for (const LogEntry& e : upToEvents)
        if (e.t == upTo) live.apply(e);
    const std::uint64_t atUpTo = live.hash();
    runScript(live, later, 1, upTo + 20000);
    REQUIRE(live.log().back().t > upTo);  // entries after upTo exist and must be left out

    const MatchGame replayed = MatchGame::replay(setup, live.log(), upTo);
    CHECK(replayed.t() == upTo);
    CHECK(replayed.hasGarbage(99));
    CHECK(replayed.hash() == atUpTo);
}

TEST_CASE("match_game: garbage accepted while frozen replays identically") {
    SUBCASE("during a duel's KO flash") {
        MatchGame live(duelSetup());
        live.advance(100);
        topOut(live);
        REQUIRE(live.frozen());
        live.acceptGarbage(1, 3);
        live.advance(500);
        live.acceptGarbage(2, 2);
        live.advance(2000);
        CHECK(live.hasGarbage(1));
        CHECK(live.hasGarbage(2));
        CHECK(MatchGame::replay(duelSetup(), live.log(), live.t()).hash() == live.hash());
    }
    SUBCASE("after a royale elimination") {
        MatchGame live(royaleSetup());
        topOut(live);
        REQUIRE(live.eliminated());
        live.acceptGarbage(1, 3);
        live.advance(100);
        live.acceptGarbage(2, 1);
        live.advance(100);
        CHECK(live.hasGarbage(2));
        CHECK(MatchGame::replay(royaleSetup(), live.log(), live.t()).hash() == live.hash());
    }
}

TEST_CASE("match_game: without a log the game plays the same and keeps nothing") {
    MatchGame logged(duelSetup());
    MatchGame bare(duelSetup(), false);
    for (MatchGame* match : {&logged, &bare}) {
        match->advance(5);
        match->input(Action::Left);
        match->softDrop(true);
        match->advance(3);
        match->acceptGarbage(7, 2);
        match->softDrop(false);
        match->input(Action::HardDrop);
        match->advance(500);
    }
    CHECK(bare.hash() == logged.hash());
    CHECK(bare.log().empty());
    CHECK(bare.takeUnsent().empty());
    CHECK(logged.log().size() == 5);
}
