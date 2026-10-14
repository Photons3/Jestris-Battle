#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "app/duel.hpp"
#include "bot/local_match.hpp"
#include "net/protocol.hpp"

using namespace tetris;

namespace {

std::vector<Message> drain(LocalMatch& match) {
    std::vector<Message> messages;
    for (const std::string& text : match.receive()) {
        const std::optional<Message> message = decode(text);
        REQUIRE(message.has_value());
        messages.push_back(*message);
    }
    return messages;
}

template <typename T>
int countOf(const std::vector<Message>& messages) {
    int n = 0;
    for (const Message& m : messages) n += std::holds_alternative<T>(m);
    return n;
}

template <typename T>
std::vector<T> allOf(const std::vector<Message>& messages) {
    std::vector<T> found;
    for (const Message& m : messages)
        if (const T* t = std::get_if<T>(&m)) found.push_back(*t);
    return found;
}

std::vector<Snapshot> snapshotsOf(const std::vector<Message>& messages) {
    std::vector<Snapshot> found;
    for (const PlayerSnapshot& p : allOf<PlayerSnapshot>(messages)) found.push_back(p.snapshot);
    return found;
}

int attackLines(const std::vector<Message>& messages) {
    int lines = 0;
    for (const Attack& a : allOf<Attack>(messages)) lines += a.lines;
    return lines;
}

// Creates a room and runs out the countdown; drops what was received so far.
void startPlaying(LocalMatch& match) {
    match.send(encode(CreateRoom{1, false}));
    match.update(kCountdownMs);
    drain(match);
}

}  // namespace

TEST_CASE("local match: verdicts follow the server's rules") {
    CHECK(verdictFor(2, 1, 0, 9, MatchReason::Time).result == MatchResult::Win);
    CHECK(verdictFor(1, 1, 5, 3, MatchReason::Time).result == MatchResult::Win);
    CHECK(verdictFor(1, 1, 3, 5, MatchReason::Time).result == MatchResult::Lose);
    CHECK(verdictFor(1, 1, 4, 4, MatchReason::Time).result == MatchResult::Draw);
    CHECK(verdictFor(3, 0, 1, 2, MatchReason::KOs) == MatchOver{MatchResult::Win, MatchReason::KOs, 1, 2});
}

TEST_CASE("local match: CREATE answers ROOM, PLAYERS and START with two ids") {
    LocalMatch match(BotLevel::Hard, 1);
    match.send(encode(CreateRoom{4, false}));
    const auto got = drain(match);
    REQUIRE(got.size() == 3);
    CHECK(std::get<RoomEntered>(got[0]) == RoomEntered{"BOTS", 1});
    CHECK(std::get<Roster>(got[1]) == Roster{1, {{1, "You", true}, {2, "Bot-Hard", true}}});
    const StartMatch& start = std::get<StartMatch>(got[2]);
    CHECK(start.startLevel == 4);
    CHECK(start.players == std::vector<int>{1, 2});
}

TEST_CASE("local match: the roster uses the name from HELLO") {
    LocalMatch match(BotLevel::Easy, 1);
    match.send(encode(Hello{"Sam"}));
    match.send(encode(CreateRoom{1, false}));
    const auto got = drain(match);
    REQUIRE(got.size() == 3);
    CHECK(std::get<Roster>(got[1]) == Roster{1, {{1, "Sam", true}, {2, "Bot-Easy", true}}});
}

TEST_CASE("local match: the bot waits out the countdown, then plays and sends snapshots") {
    LocalMatch match(BotLevel::Hard, 3);
    match.send(encode(CreateRoom{1, false}));
    drain(match);
    match.update(kCountdownMs - 1);
    CHECK(countOf<PlayerSnapshot>(drain(match)) == 0);
    match.update(3000);
    const std::vector<PlayerSnapshot> snapshots = allOf<PlayerSnapshot>(drain(match));
    REQUIRE_FALSE(snapshots.empty());
    for (const PlayerSnapshot& s : snapshots) CHECK(s.fromId == 2);  // SNAP 2 ...
}

TEST_CASE("local match: three deaths lose by KOs") {
    LocalMatch match(BotLevel::Easy, 4);
    startPlaying(match);
    for (int i = 0; i < 3; ++i) match.send(encode(Dead{}));
    const auto got = drain(match);
    REQUIRE(got.size() == 4);
    CHECK(std::get<KoCounts>(got[0]) == KoCounts{0, 1});
    CHECK(std::get<KoCounts>(got[1]) == KoCounts{0, 2});
    CHECK(std::get<KoCounts>(got[2]) == KoCounts{0, 3});
    const MatchOver over = std::get<MatchOver>(got[3]);
    CHECK(over.result == MatchResult::Lose);
    CHECK(over.reason == MatchReason::KOs);
}

TEST_CASE("local match: burying the bot wins by KOs") {
    LocalMatch match(BotLevel::Hard, 5);
    startPlaying(match);
    std::vector<Message> got;
    int sent = 0;
    for (int i = 0; i < 200 && countOf<MatchOver>(got) == 0; ++i) {
        match.send(encode(Attack{40}));
        sent += 40;
        match.update(2000);
        for (const Message& m : drain(match)) got.push_back(m);
    }
    const std::vector<KoCounts> kos = allOf<KoCounts>(got);
    REQUIRE(!kos.empty());
    CHECK(kos.back().mine == 3);
    CHECK(kos.back().theirs == 0);
    REQUIRE(std::holds_alternative<MatchOver>(got.back()));
    const MatchOver over = std::get<MatchOver>(got.back());
    CHECK(over.result == MatchResult::Win);
    CHECK(over.reason == MatchReason::KOs);
    CHECK(over.mySent == sent);
    CHECK(over.theirSent == attackLines(got));
}

TEST_CASE("local match: time up gives the verdict on the totals") {
    LocalMatch match(BotLevel::Medium, 6);
    startPlaying(match);
    match.send(encode(Attack{3}));
    std::vector<Message> got;
    match.update(kMatchMs + kLocalMatchGraceMs - 1);
    for (const Message& m : drain(match)) got.push_back(m);
    CHECK(countOf<MatchOver>(got) == 0);
    match.update(1);
    for (const Message& m : drain(match)) got.push_back(m);
    REQUIRE(!got.empty());
    REQUIRE(std::holds_alternative<MatchOver>(got.back()));
    CHECK(countOf<MatchOver>(got) == 1);
    const std::vector<KoCounts> kos = allOf<KoCounts>(got);
    const KoCounts last = kos.empty() ? KoCounts{} : kos.back();
    const MatchOver over = std::get<MatchOver>(got.back());
    CHECK(over.reason == MatchReason::Time);
    CHECK(over == verdictFor(last.mine, last.theirs, 3, attackLines(got), MatchReason::Time));
}

TEST_CASE("local match: the bot stops at 0:00 and waits out the grace like the human") {
    LocalMatch match(BotLevel::Hard, 10);
    startPlaying(match);
    match.update(kMatchMs);  // the clock now reads 0:00
    drain(match);
    REQUIRE(match.botGame() != nullptr);
    const Snapshot atZero = snapshotOf(*match.botGame());
    const Piece pieceAtZero = match.botGame()->active();
    match.update(kLocalMatchGraceMs - 1);
    const auto during = drain(match);
    CHECK(countOf<Attack>(during) == 0);
    CHECK(countOf<KoCounts>(during) == 0);
    CHECK(countOf<PlayerSnapshot>(during) == 0);
    CHECK(snapshotOf(*match.botGame()) == atZero);
    CHECK(match.botGame()->active().pos.x == pieceAtZero.pos.x);
    CHECK(match.botGame()->active().pos.y == pieceAtZero.pos.y);
    CHECK(match.botGame()->active().rotation == pieceAtZero.rotation);
    match.update(1);
    const auto end = drain(match);
    REQUIRE(end.size() == 1);
    CHECK(std::get<MatchOver>(end[0]).reason == MatchReason::Time);
}

TEST_CASE("local match: a long update still plays and ends on time") {
    LocalMatch match(BotLevel::Hard, 7);
    match.send(encode(CreateRoom{1, false}));
    drain(match);
    match.update(kCountdownMs + kMatchMs + kLocalMatchGraceMs);
    const auto got = drain(match);
    const std::vector<Snapshot> snapshots = snapshotsOf(got);
    REQUIRE(!snapshots.empty());
    CHECK(snapshots.back().score > 0);  // pieces were hard-dropped
    REQUIRE(match.botGame() != nullptr);
    CHECK(match.botGame()->lines() > 0);
    REQUIRE(!got.empty());
    REQUIRE(std::holds_alternative<MatchOver>(got.back()));
    CHECK(std::get<MatchOver>(got.back()).reason == MatchReason::Time);
}

TEST_CASE("local match: late and early messages are ignored") {
    LocalMatch match(BotLevel::Hard, 8);
    // Before any match: nothing to score or attack.
    match.send(encode(Dead{}));
    match.send(encode(Attack{5}));
    match.send(encode(Begin{}));
    match.update(100);
    CHECK(drain(match).empty());
    CHECK(match.botGame() == nullptr);

    startPlaying(match);
    match.update(500);
    drain(match);
    match.send(encode(Begin{}));  // mid-match
    match.update(100);
    CHECK(countOf<StartMatch>(drain(match)) == 0);

    for (int i = 0; i < 3; ++i) match.send(encode(Dead{}));
    REQUIRE(countOf<MatchOver>(drain(match)) == 1);
    REQUIRE(match.botGame() != nullptr);
    const Snapshot before = snapshotOf(*match.botGame());
    match.send(encode(Dead{}));
    match.send(encode(Attack{5}));
    match.update(2000);
    CHECK(drain(match).empty());
    CHECK(snapshotOf(*match.botGame()) == before);
    CHECK(match.botGame()->pendingGarbage() == 0);
}

TEST_CASE("local match: BEGIN after OVER starts the next match") {
    LocalMatch match(BotLevel::Medium, 9);
    match.send(encode(CreateRoom{2, false}));
    const auto first = drain(match);
    REQUIRE(first.size() == 3);
    const StartMatch start = std::get<StartMatch>(first[2]);
    match.update(kCountdownMs + 1000);
    for (int i = 0; i < 3; ++i) match.send(encode(Dead{}));
    REQUIRE(countOf<MatchOver>(drain(match)) == 1);

    match.send(encode(Begin{}));
    const auto again = drain(match);
    REQUIRE(again.size() == 1);  // no new ROOM or PLAYERS
    const StartMatch restart = std::get<StartMatch>(again[0]);
    CHECK(restart.seed != start.seed);
    CHECK(restart.startLevel == 2);
    CHECK(restart.players == std::vector<int>{1, 2});
    REQUIRE(match.botGame() != nullptr);
    CHECK(match.botGame()->score() == 0);
}

TEST_CASE("local match: a real duel session plays the bot") {
    std::unique_ptr<LocalMatch> match = std::make_unique<LocalMatch>(BotLevel::Hard, 2);
    DuelSession duel([&](const std::string& text) { match->send(text); });
    duel.createRoom(1);
    auto frame = [&](int ms) {
        for (const std::string& text : match->receive()) duel.onMessage(text);
        if (duel.game() && !duel.frozen() && duel.phase() == DuelPhase::Playing) duel.game()->tick(ms);
        duel.update(ms);
        match->update(ms);
    };
    frame(16);
    CHECK(duel.opponentName() == "Bot-Hard");
    CHECK(duel.phase() == DuelPhase::Countdown);  // no Start click
    CHECK(duel.isHost());
    for (int i = 0; i < 2000 && duel.sentTheirs() == 0; ++i) frame(16);
    CHECK(duel.sentTheirs() > 0);  // the bot's garbage reached us
    for (int i = 0; i < 2000 && duel.opponent().score == 0; ++i) frame(16);
    CHECK(duel.opponent().score > 0);  // the bot's SNAP 2 reached its opponent view

    for (int i = 0; i < kKosToWin; ++i) match->send(encode(Dead{}));
    frame(16);
    REQUIRE(duel.phase() == DuelPhase::Result);
    duel.begin();  // the host's "Start next match"
    frame(16);
    CHECK(duel.phase() == DuelPhase::Countdown);
    CHECK(duel.opponentName() == "Bot-Hard");
}

TEST_CASE("local match: the bot's snapshots carry its stats") {
    LocalMatch match(BotLevel::Hard, 1);
    startPlaying(match);
    std::vector<Snapshot> snapshots;
    for (int i = 0; i < 5; ++i) {
        match.update(1000);
        for (const Snapshot& s : snapshotsOf(drain(match))) snapshots.push_back(s);
    }
    REQUIRE_FALSE(snapshots.empty());
    REQUIRE(match.botGame() != nullptr);
    const Snapshot& last = snapshots.back();
    CHECK(last.stats == match.botGame()->stats());
    CHECK(last.stats.pieces > 0);
}
