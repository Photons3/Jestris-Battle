#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/duel.hpp"
#include "test_helpers.hpp"

using namespace tetris;

namespace {

struct Wire {
    std::vector<std::string> sent;
    DuelSession::Send sender() {
        return [this](const std::string& text) { sent.push_back(text); };
    }
};

// Creates a room, receives the roster and START and runs out the countdown; clears what was sent so far.
void startMatch(DuelSession& duel, Wire& wire) {
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");
    duel.onMessage("START 7 1 1 2");
    duel.update(kCountdownMs);
    REQUIRE(duel.phase() == DuelPhase::Playing);
    wire.sent.clear();
}

// startMatch, then swaps in a game that deals an I first over a 4-row well in column 5.
void startWithWell(DuelSession& duel, Wire& wire) {
    startMatch(duel, wire);
    duel.match()->game() = Game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
}

// dropUprightI through the match, so the inputs are logged.
void dropUprightI(MatchGame& match) {
    match.input(Action::RotateCW);
    match.input(Action::HardDrop);
}

// topOutWithGarbage through the match: the game is in GameOver afterwards, and the match handles it at the next
// step.
void topOutWithGarbage(DuelSession& duel) {
    MatchGame& match = *duel.match();
    match.acceptGarbage(0, kBoardHeight - 1);
    duel.step(kGarbageTravelMs);
    for (int i = 0; i < 10 && match.game().state() != GameState::GameOver; ++i) {
        match.game().takeEvents();
        match.input(Action::HardDrop);
    }
}

bool isIn(const std::string& text) { return text.starts_with("IN "); }

// What was sent apart from the IN stream.
std::vector<std::string> withoutIn(const std::vector<std::string>& sent) {
    std::vector<std::string> rest;
    std::ranges::copy_if(sent, std::back_inserter(rest), [](const std::string& text) { return !isIn(text); });
    return rest;
}

std::vector<std::string> insOf(const std::vector<std::string>& sent) {
    std::vector<std::string> ins;
    std::ranges::copy_if(sent, std::back_inserter(ins), isIn);
    return ins;
}

}  // namespace

TEST_CASE("duel: creating a room sends CREATE and shows the code") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(3);
    CHECK(wire.sent == std::vector<std::string>{"CREATE 3"});
    CHECK(duel.phase() == DuelPhase::Waiting);
    CHECK(duel.roomCode().empty());
    duel.onMessage("ROOM ABCD 1");
    CHECK(duel.roomCode() == "ABCD");
}

TEST_CASE("duel: a refused join returns to idle with the reason") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    CHECK(wire.sent == std::vector<std::string>{"JOIN ABCD"});
    CHECK(duel.phase() == DuelPhase::Waiting);
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.error() == "room is full");
}

TEST_CASE("duel: START counts down, then the match runs without pausing") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 2 1 2");
    CHECK(duel.phase() == DuelPhase::Countdown);
    REQUIRE(duel.game() != nullptr);
    CHECK(duel.game()->level() == 2);
    duel.update(kCountdownMs - 1);
    CHECK(duel.phase() == DuelPhase::Countdown);
    duel.update(1);
    CHECK(duel.phase() == DuelPhase::Playing);
    duel.game()->apply(Action::Pause);
    CHECK(duel.game()->state() == GameState::Playing);
}

TEST_CASE("duel: snapshots go out on the interval and only when changed") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.update(kSnapshotIntervalMs - 1);
    CHECK(wire.sent.empty());
    duel.update(1);
    REQUIRE(wire.sent.size() == 1);
    CHECK(wire.sent[0].starts_with("SNAP "));
    duel.update(kSnapshotIntervalMs);  // nothing changed on the board
    CHECK(wire.sent.size() == 1);
}

TEST_CASE("duel: holding a piece sends a snapshot even when the board looks the same") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.update(kSnapshotIntervalMs);
    REQUIRE(wire.sent.size() == 1);
    duel.game()->apply(Action::Hold);
    duel.update(kSnapshotIntervalMs);
    REQUIRE(wire.sent.size() == 2);
    const auto snapshot = decode(wire.sent[1]);
    REQUIRE(snapshot.has_value());
    REQUIRE(std::holds_alternative<Snapshot>(*snapshot));
    CHECK(std::get<Snapshot>(*snapshot).holdLocked);
}

TEST_CASE("duel: incoming attacks queue garbage on the local game") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 2");
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: line clears are sent as attacks") {
    Wire wire;
    DuelSession duel(wire.sender());
    startWithWell(duel, wire);
    dropUprightI(*duel.match());
    duel.step(kClearDelayMs);
    duel.update(0);
    CHECK(withoutIn(wire.sent) == std::vector<std::string>{"ATK 4"});
    CHECK(duel.sentMine() == 4);
    const std::vector<DuelEvent> events = duel.takeEvents();
    CHECK(std::count(events.begin(), events.end(), DuelEvent::Sent) == 1);
}

TEST_CASE("duel: a clear cancels queued garbage first and sends only what is left") {
    Wire wire;
    DuelSession duel(wire.sender());
    startWithWell(duel, wire);
    duel.onMessage("ATK 3");
    REQUIRE(duel.game()->pendingGarbage() == 3);
    dropUprightI(*duel.match());  // a tetris: 4 lines against the 3 queued
    duel.step(kClearDelayMs);
    duel.update(0);
    CHECK(duel.game()->pendingGarbage() == 0);
    CHECK(withoutIn(wire.sent) == std::vector<std::string>{"ATK 1"});
    CHECK(duel.sentMine() == 1);
    CHECK(countFilled(duel.game()->board()) == 1);  // just the well's extra block: no garbage rose
}

TEST_CASE("duel: a clear still animating at 0:00 is settled and sent") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.step(kMatchMs - 1);
    REQUIRE_FALSE(duel.match()->frozen());
    duel.match()->game() = Game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));  // the well, at 0:00.001
    dropUprightI(*duel.match());
    REQUIRE(duel.game()->state() == GameState::Clearing);
    duel.step(1);  // the last step of the clock: the game is not ticked after it
    duel.update(kMatchMs);
    CHECK(duel.frozen());
    CHECK(withoutIn(wire.sent).size() >= 1);
    CHECK(withoutIn(wire.sent)[0] == "ATK 4");
    CHECK(duel.sentMine() == 4);
}

TEST_CASE("duel: topping out sends a final snapshot and DEAD, resets the board and keeps playing") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    topOutWithGarbage(duel);
    duel.step(1);
    duel.update(0);
    const std::vector<std::string> sent = withoutIn(wire.sent);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].starts_with("SNAP "));
    CHECK(sent[1] == "DEAD");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.game()->state() == GameState::Playing);
    CHECK(countFilled(duel.game()->board()) == 0);
    CHECK(duel.game()->pendingGarbage() == 0);
    CHECK(duel.koFlashMs() == kKoFlashMs);
}

TEST_CASE("duel: a relayed DEAD is ignored now that the server referees") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("DEAD");
    CHECK(duel.phase() == DuelPhase::Playing);
}

TEST_CASE("duel: opponent leaving mid-match wins") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
    CHECK(wire.sent.empty());
}

TEST_CASE("duel: Result goes back to Countdown on the next START") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 3");
    duel.onMessage("KO 3 0");
    duel.onMessage("OVER WIN KO 3 0");
    REQUIRE(duel.phase() == DuelPhase::Result);
    duel.onMessage("START 9 1 1 2");
    CHECK(duel.phase() == DuelPhase::Countdown);
    CHECK(duel.outcome() == DuelOutcome::None);
    CHECK(duel.game()->pendingGarbage() == 0);
    CHECK(duel.kosMine() == 0);
    CHECK(duel.sentMine() == 0);
    CHECK(duel.sentTheirs() == 0);
    CHECK(duel.matchClockMs() == kMatchMs);
}

TEST_CASE("duel: opponent snapshots are kept for display") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    Snapshot snapshot{.score = 500, .lines = 2, .pending = 1};
    snapshot.cells[0] = PieceType::Garbage;
    duel.onMessage(encode(PlayerSnapshot{2, snapshot}));
    CHECK(duel.opponent() == snapshot);
}

TEST_CASE("duel: losing the connection returns to idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onDisconnected();
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.error() == "Connection lost");
    CHECK(duel.game() == nullptr);
}

TEST_CASE("duel: messages are ignored once idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.leave();
    duel.onMessage("START 1 1 1 2");
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.error().empty());
}

TEST_CASE("duel: START during a match is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 2");
    const Game* before = duel.game();
    duel.onMessage("START 9 1 1 2");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.game() == before);
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: after the opponent left, the host's next START begins a new match") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("LEFT");
    REQUIRE(duel.outcome() == DuelOutcome::OpponentLeft);
    duel.onMessage("PLAYERS 1 1:Me:0 3:New:0");
    duel.onMessage("START 9 1 1 3");
    CHECK(duel.phase() == DuelPhase::Countdown);
    CHECK(duel.outcome() == DuelOutcome::None);
    CHECK(duel.opponentName() == "New");
}

TEST_CASE("duel: a stray error mid-match is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.error().empty());
}

TEST_CASE("duel: LEFT in the room is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.error().empty());
    CHECK(duel.game() == nullptr);
}

TEST_CASE("duel: an attack during the countdown is queued") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    REQUIRE(duel.phase() == DuelPhase::Countdown);
    duel.onMessage("ATK 2");
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: OVER during our countdown ends the match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    REQUIRE(duel.phase() == DuelPhase::Countdown);
    duel.onMessage("OVER WIN KO 0 0");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: START before ROOM is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("LEFT");
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.phase() == DuelPhase::Waiting);
    CHECK(duel.game() == nullptr);
}

TEST_CASE("duel: the KO flash freezes the game for 1000 ms while the clock keeps running") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    CHECK_FALSE(duel.frozen());
    topOutWithGarbage(duel);
    duel.step(1);
    duel.update(0);
    CHECK(duel.frozen());
    duel.step(kKoFlashMs - 1);
    duel.update(kKoFlashMs - 1);
    CHECK(duel.frozen());
    CHECK(duel.matchClockMs() == kMatchMs - (kKoFlashMs - 1));
    duel.step(1);
    duel.update(1);
    CHECK_FALSE(duel.frozen());
    CHECK(duel.koFlashMs() == 0);
}

TEST_CASE("duel: garbage arriving during the KO flash is queued on the fresh board") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    topOutWithGarbage(duel);
    duel.step(1);
    duel.update(0);
    REQUIRE(duel.frozen());
    duel.onMessage("ATK 3");
    CHECK(duel.game()->pendingGarbage() == 3);
    CHECK(duel.sentTheirs() == 3);
}

TEST_CASE("duel: KO updates both counts from the server") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("KO 1 2");
    CHECK(duel.kosMine() == 1);
    CHECK(duel.kosTheirs() == 2);
    CHECK(duel.phase() == DuelPhase::Playing);
}

TEST_CASE("duel: each OVER reaches the result screen with its outcome, reason and the server's totals") {
    struct Case {
        const char* message;
        DuelOutcome outcome;
        DuelReason reason;
        int mine, theirs;
    };
    for (const Case& c : {Case{"OVER WIN KO 3 1", DuelOutcome::Won, DuelReason::KOs, 3, 1},
                          Case{"OVER LOSE TIME 2 5", DuelOutcome::Lost, DuelReason::Time, 2, 5},
                          Case{"OVER DRAW TIME 4 4", DuelOutcome::Draw, DuelReason::Time, 4, 4}}) {
        CAPTURE(c.message);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.onMessage("ATK 9");  // the live tally is replaced by the server's totals
        duel.onMessage(c.message);
        CHECK(duel.phase() == DuelPhase::Result);
        CHECK(duel.outcome() == c.outcome);
        CHECK(duel.reason() == c.reason);
        CHECK(duel.sentMine() == c.mine);
        CHECK(duel.sentTheirs() == c.theirs);
    }
}

TEST_CASE("duel: KO and OVER are ignored outside the countdown and play") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("KO 1 0");
    CHECK(duel.phase() == DuelPhase::Waiting);
    CHECK(duel.kosMine() == 0);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("KO 1 0");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.kosMine() == 0);
    duel.onMessage("START 7 1 1 2");
    duel.update(kCountdownMs);
    duel.onMessage("OVER WIN KO 0 0");
    REQUIRE(duel.phase() == DuelPhase::Result);
    duel.onMessage("KO 3 3");
    duel.onMessage("OVER LOSE TIME 1 1");
    CHECK(duel.kosMine() == 0);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: the match clock runs only while playing, stops at 0 and freezes the game there") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    duel.update(kCountdownMs - 1);
    CHECK(duel.matchClockMs() == kMatchMs);
    duel.update(1);
    REQUIRE(duel.phase() == DuelPhase::Playing);
    duel.update(1000);
    CHECK(duel.matchClockMs() == kMatchMs - 1000);
    CHECK_FALSE(duel.frozen());
    duel.update(kMatchMs);
    CHECK(duel.matchClockMs() == 0);
    CHECK(duel.frozen());
    CHECK(duel.phase() == DuelPhase::Playing);  // waits for the server's OVER
    duel.update(5);
    CHECK(duel.matchClockMs() == 0);
}

TEST_CASE("duel: incoming attacks add to the opponent's sent tally") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 2");
    duel.onMessage("ATK 1");
    CHECK(duel.sentTheirs() == 3);
    CHECK(duel.sentMine() == 0);
}

TEST_CASE("duel: the result line explains how the match ended") {
    struct Case {
        std::vector<std::string> messages;
        std::string line;
    };
    const std::vector<Case> cases{
        {{"KO 3 0", "OVER WIN KO 0 0"}, "3 KOs"},
        {{"KO 0 3", "OVER LOSE KO 0 0"}, "3 KOs"},
        {{"KO 2 1", "OVER WIN TIME 3 9"}, "Time up \xC2\xB7 KO 2 : 1"},
        {{"KO 1 1", "OVER WIN TIME 12 7"}, "Time up \xC2\xB7 KO 1 : 1 \xC2\xB7 12 : 7 sent"},
        {{"OVER DRAW TIME 9 9"}, "Time up \xC2\xB7 KO 0 : 0 \xC2\xB7 9 : 9 sent"},
        {{"LEFT"}, "Foe left the match"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.line);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        CHECK(resultReasonText(duel).empty());
        for (const std::string& message : c.messages) duel.onMessage(message);
        CHECK(resultReasonText(duel) == c.line);
    }
}

TEST_CASE("duel events: the countdown ticks each second, then the match starts") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "3"
    duel.update(1000);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "2"
    duel.update(999);
    CHECK(duel.takeEvents().empty());
    duel.update(1);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "1"
    duel.update(1000);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::MatchStart});
}

TEST_CASE("duel events: incoming garbage and our KOs are reported, the opponent's KOs are not") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.takeEvents();
    duel.onMessage("ATK 3");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::Attacked});
    duel.onMessage("KO 1 0");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::KoMine});
    duel.onMessage("KO 1 1");
    CHECK(duel.takeEvents().empty());
}

TEST_CASE("duel events: each result is reported, and an opponent leaving is silent") {
    for (const auto& c : {std::pair{"OVER WIN KO 3 1", DuelEvent::Won},
                          std::pair{"OVER LOSE TIME 2 5", DuelEvent::Lost},
                          std::pair{"OVER DRAW TIME 4 4", DuelEvent::Draw}}) {
        CAPTURE(c.first);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.takeEvents();
        duel.onMessage(c.first);
        CHECK(duel.takeEvents() == std::vector<DuelEvent>{c.second});
    }
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.takeEvents();
    duel.onMessage("LEFT");
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
    CHECK(duel.takeEvents().empty());
}

TEST_CASE("duel events: our top-out survives the KO board reset") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    topOutWithGarbage(duel);
    duel.step(1);  // the KO: the match resets the board
    REQUIRE(duel.match()->topOuts() == 1);
    duel.update(0);
    const std::vector<GameEvent> events = duel.game()->takeEvents();  // the last lock may also raise garbage
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().type == GameEventType::HardDropped);
    CHECK(events.back().type == GameEventType::ToppedOut);
}

TEST_CASE("duel: frames replay a long stall only while a match runs") {
    CHECK(frameBudgetMs(16, false) == 16);
    CHECK(frameBudgetMs(5000, false) == kMaxFrameMs);
    CHECK(frameBudgetMs(5000, true) == 5000);
    CHECK(frameBudgetMs(10'000'000, true) == kMaxCatchUpMs);
}

TEST_CASE("duel: inMatch covers the countdown and play, not waiting or the result") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    CHECK_FALSE(duel.inMatch());
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.inMatch());
    duel.update(kCountdownMs);
    CHECK(duel.inMatch());
    duel.onMessage("OVER WIN KO 3 0");
    CHECK_FALSE(duel.inMatch());
}

TEST_CASE("duel: catching up after a hidden tab runs the clock and sends the new board") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->tick(5000);  // gravity while hidden, as step() replays it
    duel.update(5000);
    CHECK(duel.matchClockMs() == kMatchMs - 5000);
    REQUIRE(wire.sent.size() == 1);
    CHECK(wire.sent[0].starts_with("SNAP "));
}

TEST_CASE("duel: the opponent is named from the roster at START, and leaving forgets it") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 2");
    duel.onMessage("PLAYERS 1 1:Kai:0 2:Me:0");
    CHECK(duel.opponentName().empty());  // no match yet
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.opponentName() == "Kai");
    duel.onMessage("PLAYERS 1 1:Zed:1 2:Me:1");  // fixed at START
    CHECK(duel.opponentName() == "Kai");
    duel.leave();
    CHECK(duel.opponentName().empty());
}

TEST_CASE("duel: an opponent missing from the roster is named by id") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.opponentName() == "P2");
    duel.onMessage("LEFT");
    CHECK(resultReasonText(duel) == "P2 left the match");
}

TEST_CASE("duel: the lobby sends LIST and keeps the pushed list") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    CHECK(duel.phase() == DuelPhase::Lobby);
    CHECK(wire.sent == std::vector<std::string>{"LIST"});
    duel.onMessage("ROOMS ABCD:3:Sam:1");
    REQUIRE(duel.rooms().size() == 1);
    CHECK(duel.rooms()[0] == RoomInfo{"ABCD", 3, "Sam", 1});
    duel.onMessage("ROOMS");
    CHECK(duel.rooms().empty());
}

TEST_CASE("duel: ROOMS outside the lobby is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOMS ABCD:3:Sam:1");
    CHECK(duel.rooms().empty());
}

TEST_CASE("duel: a refused join from the lobby goes back to the lobby") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    duel.onMessage("ROOMS ABCD:3:Sam:1");
    duel.joinRoom("ABCD");
    CHECK(duel.phase() == DuelPhase::Waiting);
    wire.sent.clear();
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Lobby);
    CHECK(duel.error() == "room is full");
    CHECK(wire.sent == std::vector<std::string>{"LIST"});
}

TEST_CASE("duel: a refused join from the start screen still goes to idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ERR unknown room");
    CHECK(duel.phase() == DuelPhase::Idle);
}

TEST_CASE("duel: a public room from the lobby") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    wire.sent.clear();
    duel.createRoom(4, true);
    CHECK(wire.sent == std::vector<std::string>{"CREATE 4 PUB"});
    CHECK(duel.visibility() == RoomVisibility::Public);
    CHECK_FALSE(duel.quickMatching());
}

TEST_CASE("duel: quick match waits as a host or starts as a joiner") {
    Wire wire;
    DuelSession host(wire.sender());
    host.quickMatch(2);
    CHECK(wire.sent == std::vector<std::string>{"QUICK 2"});
    CHECK(host.quickMatching());
    host.onMessage("ROOM ABCD 1");
    CHECK(host.roomCode() == "ABCD");
    CHECK(host.phase() == DuelPhase::Room);

    DuelSession joiner(wire.sender());
    joiner.quickMatch(9);
    joiner.onMessage("ROOM ABCD 2");
    joiner.onMessage("PLAYERS 1 1:Sam:0 2:Me:0");
    joiner.onMessage("START 7 2 1 2");
    CHECK(joiner.phase() == DuelPhase::Countdown);
    CHECK(joiner.opponentName() == "Sam");
}

TEST_CASE("duel stats: rates are per second and per minute of play, zero before any") {
    CHECK(ratesOf(GameStats{.pieces = 5}, 0).pps == 0.0);
    const MatchRates r = ratesOf(GameStats{.pieces = 60, .attack = 30, .garbageCleared = 10}, 60000);
    CHECK(r.pps == doctest::Approx(1.0));
    CHECK(r.apm == doctest::Approx(30.0));
    CHECK(r.vs == doctest::Approx(66.6667).epsilon(0.0001));
}

TEST_CASE("duel stats: played time counts only play, and stops with the match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    duel.update(kCountdownMs - 1);
    CHECK(duel.playedMs() == 0);
    duel.update(1);  // ends the countdown; update() returns there, so play time starts with the next call
    CHECK(duel.playedMs() == 0);
    duel.update(1500);
    CHECK(duel.playedMs() == 1500);
    duel.onMessage("OVER WIN KO 0 0");
    duel.update(1000);
    CHECK(duel.playedMs() == 1500);
}

TEST_CASE("duel stats: our snapshots carry our stats, theirs land in opponent()") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->apply(Action::HardDrop);
    duel.update(kSnapshotIntervalMs);
    REQUIRE_FALSE(wire.sent.empty());
    const auto sent = decode(wire.sent.back());
    REQUIRE(sent.has_value());
    CHECK(std::get<Snapshot>(*sent).stats.pieces == 1);
    Snapshot theirs{};
    theirs.stats.tetrises = 2;
    duel.onMessage(encode(PlayerSnapshot{2, theirs}));
    CHECK(duel.opponent().stats.tetrises == 2);
}

TEST_CASE("duel stats: the result table, ours on the left") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    CHECK(resultStatRows(duel).empty());
    duel.update(60000);
    Snapshot theirs{};
    theirs.stats = {.pieces = 90, .attack = 45, .garbageCleared = 15, .cancelled = 6, .tetrises = 3, .tSpins = 2,
                    .maxCombo = 5};
    duel.onMessage(encode(PlayerSnapshot{2, theirs}));
    duel.onMessage("OVER LOSE TIME 4 39");
    const std::vector<StatRow> rows = resultStatRows(duel);
    std::vector<std::string> labels;
    for (const StatRow& row : rows) labels.push_back(row.label);
    CHECK(labels == std::vector<std::string>{"PPS", "APM", "VS", "Sent", "Cancelled", "Tetrises", "T-spins",
                                             "Max combo"});
    CHECK(rows[0] == StatRow{"PPS", "0.00", "1.50"});
    CHECK(rows[1] == StatRow{"APM", "0.0", "45.0"});
    CHECK(rows[2] == StatRow{"VS", "0.0", "100.0"});
    CHECK(rows[3] == StatRow{"Sent", "4", "39"});
    CHECK(rows[7] == StatRow{"Max combo", "-", "5"});
}

TEST_CASE("duel stats: no opponent snapshot shows zeros, and the next match starts from zero") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    Snapshot theirs{};
    theirs.stats.pieces = 30;
    duel.onMessage(encode(PlayerSnapshot{2, theirs}));
    duel.game()->apply(Action::HardDrop);
    duel.update(5000);
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("START 8 1 1 2");
    duel.update(kCountdownMs);
    CHECK(duel.playedMs() == 0);
    CHECK(duel.game()->stats() == GameStats{});
    CHECK(duel.opponent().stats == GameStats{});
    duel.onMessage("LEFT");  // leaves before sending a single SNAP
    const std::vector<StatRow> rows = resultStatRows(duel);
    REQUIRE(rows.size() == 8);
    CHECK(rows[0].theirs == "0.00");
    CHECK(rows[7].theirs == "-");
}

TEST_CASE("duel: ROOM moves Waiting to Room with my id") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    CHECK(duel.myId() == 0);
    duel.onMessage("ROOM ABCD 1");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.myId() == 1);
    CHECK(duel.roomCode() == "ABCD");
    CHECK_FALSE(duel.inMatch());
    duel.onMessage("ROOM WXYZ 5");  // only the answer to our request counts
    CHECK(duel.myId() == 1);
    CHECK(duel.roomCode() == "ABCD");
}

TEST_CASE("duel: PLAYERS updates roster and host") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 1 1:Me:0");
    CHECK(duel.hostId() == 1);
    CHECK(duel.isHost());
    CHECK(duel.roster().size() == 1);
    duel.onMessage("PLAYERS 2 1:Me:0 2:Foe:0");
    CHECK(duel.hostId() == 2);
    CHECK_FALSE(duel.isHost());
    REQUIRE(duel.roster().size() == 2);
    CHECK(duel.roster()[1] == RosterEntry{2, "Foe", false});
}

TEST_CASE("duel: PLAYERS is taken in the room, the match and the result, and nowhere else") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");  // Idle
    CHECK(duel.roster().empty());
    duel.openLobby();
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");
    CHECK(duel.roster().empty());
    duel.createRoom(1);
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");  // Waiting
    CHECK(duel.roster().empty());
    CHECK(duel.hostId() == 0);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");
    CHECK(duel.roster().size() == 2);
    duel.onMessage("START 7 1 1 2");
    duel.onMessage("PLAYERS 1 1:Me:1 2:Foe:1 3:Late:0");  // Countdown
    CHECK(duel.roster().size() == 3);
    duel.update(kCountdownMs);
    duel.onMessage("PLAYERS 2 2:Foe:1 3:Late:0");  // Playing
    CHECK(duel.hostId() == 2);
    duel.onMessage("OVER WIN KO 3 0");
    duel.onMessage("PLAYERS 3 3:Late:0 1:Me:0");  // Result
    CHECK(duel.hostId() == 3);
    CHECK(duel.roster().size() == 2);
}

TEST_CASE("duel: begin sends BEGIN only for the host with two or more players") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.begin();  // Idle
    duel.createRoom(1);
    wire.sent.clear();
    duel.begin();  // Waiting
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 1 1:Me:0");
    duel.begin();  // alone
    CHECK(wire.sent.empty());
    duel.onMessage("PLAYERS 2 1:Me:0 2:Foe:0");
    duel.begin();  // not the host
    CHECK(wire.sent.empty());
    duel.onMessage("PLAYERS 1 1:Me:0 2:Foe:0");
    duel.begin();
    CHECK(wire.sent == std::vector<std::string>{"BEGIN"});
    duel.onMessage("START 7 1 1 2");
    wire.sent.clear();
    duel.begin();  // Countdown
    duel.update(kCountdownMs);
    wire.sent.clear();
    duel.begin();  // Playing
    CHECK(wire.sent.empty());
    duel.onMessage("OVER WIN KO 3 0");
    duel.begin();
    CHECK(wire.sent == std::vector<std::string>{"BEGIN"});
}

TEST_CASE("duel: START without my id is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 2 1:Me:0 2:Foe:0 3:Bo:0");
    duel.onMessage("START 7 1 2 3");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.game() == nullptr);
    CHECK(duel.opponents().empty());
}

TEST_CASE("duel: START without my id on the result screen is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("OVER WIN KO 3 0");
    duel.onMessage("START 9 1 2 3");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: START with two ids is a duel with one named opponent") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 2");
    duel.onMessage("PLAYERS 2 1:Foe:0 2:Me:0 3:Late:0");
    duel.onMessage("START 7 1 1 2");
    CHECK(duel.phase() == DuelPhase::Countdown);
    CHECK(duel.mode() == DuelMode::Duel);
    REQUIRE(duel.opponents().size() == 1);
    CHECK(duel.opponents()[0].id == 1);
    CHECK(duel.opponents()[0].alive);
    CHECK(duel.opponents()[0].place == 0);
    CHECK(duel.opponentName() == "Foe");
}

TEST_CASE("duel: SNAP from a participant updates that opponent; unknown ids are ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    const Snapshot theirs{.score = 300};
    const Snapshot stranger{.score = 900};
    duel.onMessage(encode(PlayerSnapshot{3, stranger}));
    duel.onMessage(encode(PlayerSnapshot{1, stranger}));  // our own id
    CHECK(duel.opponent() == Snapshot{});
    duel.onMessage(encode(PlayerSnapshot{2, theirs}));
    CHECK(duel.opponent() == theirs);
    REQUIRE(duel.opponents().size() == 1);
    CHECK(duel.opponents()[0].snapshot == theirs);
}

TEST_CASE("duel: ATK, KO, OVER and LEFT do nothing in the room or the result") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("ATK 2");
    duel.onMessage("KO 1 0");
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.sentTheirs() == 0);
    CHECK(duel.kosMine() == 0);
    CHECK(duel.outcome() == DuelOutcome::None);
    duel.onMessage("START 7 1 1 2");
    duel.update(kCountdownMs);
    duel.onMessage("OVER LOSE KO 1 2");
    REQUIRE(duel.phase() == DuelPhase::Result);
    duel.onMessage("ATK 2");
    duel.onMessage("LEFT");
    CHECK(duel.outcome() == DuelOutcome::Lost);
    CHECK(duel.sentTheirs() == 2);
}

TEST_CASE("duel: ERR in the room is ignored, and a lost connection there goes to idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.error().empty());
    duel.onDisconnected();
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.error() == "Connection lost");
    CHECK(duel.myId() == 0);
    CHECK(duel.roster().empty());
}

TEST_CASE("duel: leaving after a match forgets the room") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("OVER WIN KO 3 0");
    duel.leave();
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.myId() == 0);
    CHECK(duel.hostId() == 0);
    CHECK(duel.roster().empty());
    CHECK(duel.opponents().empty());
    CHECK(duel.roomCode().empty());
}

TEST_CASE("duel stats: played time stops at the duel's 0:00") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.update(kMatchMs + 5000);
    CHECK(duel.playedMs() == kMatchMs);
}

namespace {

// Joins room ABCD as player 1 of 4 and runs out the countdown of a royale; clears what was sent so far.
void startRoyale(DuelSession& duel, Wire& wire) {
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("PLAYERS 1 1:Me:0 2:Bo:0 3:Cy:0 4:Di:0");
    duel.onMessage("START 7 1 1 2 3 4");
    duel.update(kCountdownMs);
    REQUIRE(duel.phase() == DuelPhase::Playing);
    wire.sent.clear();
}

bool hasBomb(const Board& board) {
    for (int y = 0; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (board.at(x, y) == Cell{PieceType::Bomb}) return true;
    return false;
}

}  // namespace

TEST_CASE("royale: START with three or more ids is a royale with hole garbage") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    CHECK(duel.mode() == DuelMode::Royale);
    CHECK(duel.opponents().size() == 3);
    CHECK(duel.participants() == 4);
    duel.onMessage("ATK 2");
    duel.game()->tick(kGarbageTravelMs);
    duel.game()->apply(Action::HardDrop);  // the first piece on an empty board clears nothing
    REQUIRE(duel.game()->pendingGarbage() == 0);
    CHECK(countFilled(duel.game()->board()) == 4 + 2 * (kBoardWidth - 1));  // the piece and two hole rows
    CHECK_FALSE(hasBomb(duel.game()->board()));
}

TEST_CASE("royale: a duel START still uses bomb garbage") {
    StartMatch start;
    start.seed = 7;
    start.startLevel = 1;
    start.players = {1, 2};
    Game game = makeDuelGame(start);
    game.receiveGarbage(2, 0);
    game.apply(Action::HardDrop);
    CHECK(hasBomb(game.board()));
}

TEST_CASE("royale: a top-out sends the final SNAP and DEAD once and keeps the board") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    topOutWithGarbage(duel);
    REQUIRE(duel.game()->state() == GameState::GameOver);
    const int filled = countFilled(duel.game()->board());
    duel.step(1);
    duel.update(0);
    duel.step(kSnapshotIntervalMs);
    duel.update(kSnapshotIntervalMs);
    const std::vector<std::string> sent = withoutIn(wire.sent);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].starts_with("SNAP "));
    CHECK(sent[1] == "DEAD");
    CHECK(duel.eliminated());
    CHECK(duel.game()->state() == GameState::GameOver);
    CHECK(countFilled(duel.game()->board()) == filled);
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.frozen());  // the match takes no more input
    CHECK(duel.koFlashMs() == 0);
    const int played = duel.playedMs();
    const std::size_t sentBefore = wire.sent.size();
    duel.onMessage("ATK 3");  // still reaches the game, but nothing more goes out
    duel.step(kSnapshotIntervalMs * 5);
    duel.update(kSnapshotIntervalMs * 5);
    CHECK(wire.sent.size() == sentBefore);
    CHECK(duel.playedMs() == played);  // our play time stops at elimination, for PPS and APM
}

TEST_CASE("royale: OUT marks an opponent out with its place") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("OUT 3 4");
    REQUIRE(duel.opponents().size() == 3);
    CHECK(duel.opponents()[1].id == 3);
    CHECK_FALSE(duel.opponents()[1].alive);
    CHECK(duel.opponents()[1].place == 4);
    CHECK(duel.opponents()[0].alive);
    CHECK(duel.opponents()[2].alive);
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.myPlace() == 0);
}

TEST_CASE("royale: OUT with place 1 for me is a win") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("OUT 2 4");
    duel.onMessage("OUT 3 3");
    duel.onMessage("OUT 4 2");
    CHECK(duel.phase() == DuelPhase::Playing);
    duel.onMessage("OUT 1 1");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
    CHECK(duel.myPlace() == 1);
    const std::vector<DuelEvent> events = duel.takeEvents();
    CHECK(std::count(events.begin(), events.end(), DuelEvent::Won) == 1);
}

TEST_CASE("royale: someone else's OUT 1 is a loss with my place") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    topOutWithGarbage(duel);
    duel.step(1);
    duel.update(0);
    REQUIRE(duel.eliminated());
    duel.onMessage("OUT 1 3");
    CHECK(duel.myPlace() == 3);
    CHECK(duel.phase() == DuelPhase::Playing);
    duel.onMessage("OUT 2 2");
    duel.onMessage("OUT 4 1");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Lost);
    CHECK(duel.myPlace() == 3);
    CHECK(duel.opponents()[2].place == 1);
}

TEST_CASE("royale: no clock freeze and the level floor rises every 30 s") {
    SUBCASE("past the duel's 2:00 the game is not frozen") {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        // Idle gravity would top the game out well before 2:00, so the board is cleared every 10 s.
        for (int played = 0; played < kMatchMs + 1000; played += 10000) {
            duel.game()->resetBoard();
            duel.step(std::min(10000, kMatchMs + 1000 - played));
        }
        duel.update(kMatchMs + 1000);
        REQUIRE(duel.game()->state() != GameState::GameOver);
        CHECK_FALSE(duel.eliminated());
        CHECK_FALSE(duel.frozen());
        CHECK(duel.playedMs() == kMatchMs + 1000);
        CHECK(duel.game()->level() == 1 + (kMatchMs + 1000) / kRoyaleLevelStepMs);
    }
    SUBCASE("the floor steps up at each 30 s of play") {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        duel.step(kRoyaleLevelStepMs - 1);
        duel.update(kRoyaleLevelStepMs - 1);
        CHECK(duel.game()->level() == 1);
        duel.step(1);
        duel.update(1);
        CHECK(duel.game()->level() == 2);
    }
    SUBCASE("a duel keeps its floor at 0") {
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.step(kRoyaleLevelStepMs * 2);
        duel.update(kRoyaleLevelStepMs * 2);
        CHECK(duel.game()->level() == 1);
    }
}

TEST_CASE("royale: opponent names are fixed at START even when an ID is reused") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("PLAYERS 1 1:Me:1 2:Bo:1 3:New:0 4:Di:1");
    REQUIRE(duel.opponents().size() == 3);
    CHECK(duel.opponents()[1].id == 3);
    CHECK(duel.opponents()[1].name == "Cy");
    duel.onMessage("OUT 3 4");
    CHECK(duel.opponents()[1].name == "Cy");
    CHECK_FALSE(duel.opponents()[1].alive);
    CHECK(duel.opponents()[1].place == 4);
}

TEST_CASE("royale: OUT outside a match is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("OUT 1 1");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.outcome() == DuelOutcome::None);
    CHECK(duel.myPlace() == 0);
    startRoyale(duel, wire);  // a fresh createRoom from the room
    duel.onMessage("OUT 2 4");
    duel.onMessage("OUT 3 3");
    duel.onMessage("OUT 1 2");
    duel.onMessage("OUT 4 1");
    REQUIRE(duel.phase() == DuelPhase::Result);
    REQUIRE(duel.outcome() == DuelOutcome::Lost);
    duel.onMessage("OUT 1 1");
    duel.onMessage("OUT 4 3");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Lost);
    CHECK(duel.myPlace() == 2);
    CHECK(duel.opponents()[2].place == 1);
}

TEST_CASE("royale: LEFT ends a royale as Match ended") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("LEFT");  // the server's simulation failed
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
    CHECK(resultReasonText(duel) == "Match ended");
}

TEST_CASE("royale: standings list places in order with attack") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    CHECK(royaleStandings(duel).empty());  // only on the result screen
    Snapshot bo{};
    bo.stats.attack = 5;
    Snapshot di{};
    di.stats.attack = 9;
    duel.onMessage(encode(PlayerSnapshot{2, bo}));
    duel.onMessage(encode(PlayerSnapshot{4, di}));
    duel.onMessage("OUT 2 4");
    duel.onMessage("OUT 1 3");
    duel.onMessage("OUT 4 2");
    duel.onMessage("OUT 3 1");
    REQUIRE(duel.phase() == DuelPhase::Result);
    const std::vector<Standing> standings = royaleStandings(duel);
    REQUIRE(standings.size() == 4);
    std::vector<int> places;
    std::vector<std::string> names;
    for (const Standing& s : standings) {
        places.push_back(s.place);
        names.push_back(s.name);
    }
    CHECK(places == std::vector<int>{1, 2, 3, 4});
    CHECK(names == std::vector<std::string>{"Cy", "Di", "Me", "Bo"});
    CHECK(standings[1].attack == 9);
    CHECK(standings[3].attack == 5);
    CHECK(standings[2].attack == duel.game()->stats().attack);
    CHECK(standings[2].me);
    CHECK_FALSE(standings[0].me);
    CHECK_FALSE(standings[1].me);
    CHECK_FALSE(standings[3].me);
}

TEST_CASE("royale: the result title is the place out of the field") {
    {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        CHECK(royaleResultTitle(duel).empty());  // only on the result screen
        duel.onMessage("OUT 1 3");
        duel.onMessage("OUT 2 2");
        duel.onMessage("OUT 4 1");
        REQUIRE(duel.phase() == DuelPhase::Result);
        CHECK(royaleResultTitle(duel) == "#3 of 4");
    }
    {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        duel.onMessage("OUT 2 4");
        duel.onMessage("OUT 3 3");
        duel.onMessage("OUT 4 2");
        duel.onMessage("OUT 1 1");
        REQUIRE(duel.phase() == DuelPhase::Result);
        CHECK(royaleResultTitle(duel) == "YOU WIN");
    }
}

TEST_CASE("royale: a duel result has no standings or royale title") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("OVER WIN KO 3 1");
    REQUIRE(duel.phase() == DuelPhase::Result);
    CHECK(royaleStandings(duel).empty());
    CHECK(royaleResultTitle(duel).empty());
}

TEST_CASE("royale: the winner's board stays alive and is placed #1") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("OUT 2 4");
    duel.onMessage("OUT 1 3");
    duel.onMessage("OUT 3 2");
    duel.onMessage("OUT 4 1");
    REQUIRE(duel.phase() == DuelPhase::Result);
    REQUIRE(duel.opponents()[2].id == 4);
    CHECK(duel.opponents()[2].alive);
    CHECK(duel.opponents()[2].place == 1);
    CHECK_FALSE(duel.opponents()[1].alive);
    CHECK(duel.opponents()[1].place == 2);
}

TEST_CASE("royale: surviving a simultaneous top-out wins and is no longer eliminated") {
    Wire wire;
    DuelSession duel(wire.sender());
    startRoyale(duel, wire);
    duel.onMessage("OUT 2 4");
    duel.onMessage("OUT 3 3");
    topOutWithGarbage(duel);
    duel.step(1);
    duel.update(0);
    REQUIRE(duel.eliminated());
    duel.onMessage("OUT 4 2");  // Di's DEAD reached the server first
    duel.onMessage("OUT 1 1");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
    CHECK(duel.myPlace() == 1);
    CHECK_FALSE(duel.eliminated());  // no "OUT · #1" over the winner's well
}

TEST_CASE("royale: a late OUT on the result screen records the place without finishing again") {
    SUBCASE("an opponent's") {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        duel.onMessage("OUT 2 4");
        duel.onMessage("OUT 1 3");
        duel.onMessage("OUT 3 1");
        REQUIRE(duel.phase() == DuelPhase::Result);
        duel.takeEvents();
        duel.onMessage("OUT 4 2");
        CHECK(duel.phase() == DuelPhase::Result);
        CHECK(duel.outcome() == DuelOutcome::Lost);
        CHECK(duel.takeEvents().empty());
        CHECK_FALSE(duel.opponents()[2].alive);
        CHECK(duel.opponents()[2].place == 2);
        std::vector<int> places;
        for (const Standing& s : royaleStandings(duel)) places.push_back(s.place);
        CHECK(places == std::vector<int>{1, 2, 3, 4});
    }
    SUBCASE("our own") {
        Wire wire;
        DuelSession duel(wire.sender());
        startRoyale(duel, wire);
        duel.onMessage("OUT 2 4");
        duel.onMessage("OUT 4 3");
        duel.onMessage("OUT 3 1");
        REQUIRE(duel.outcome() == DuelOutcome::Lost);
        duel.takeEvents();
        duel.onMessage("OUT 1 2");
        CHECK(duel.myPlace() == 2);
        CHECK(royaleResultTitle(duel) == "#2 of 4");
        CHECK(duel.takeEvents().empty());
    }
}

TEST_CASE("duel: the room's visibility comes from how it was entered") {
    Wire wire;
    DuelSession duel(wire.sender());
    CHECK(duel.visibility() == RoomVisibility::Unknown);
    duel.createRoom(1, false);
    CHECK(duel.visibility() == RoomVisibility::Private);
    duel.createRoom(1, true);
    CHECK(duel.visibility() == RoomVisibility::Public);
    duel.quickMatch(1);
    CHECK(duel.visibility() == RoomVisibility::Public);
    duel.openLobby();
    duel.joinRoom("ABCD", RoomVisibility::Public);  // picked from Browse rooms, so it is listed
    CHECK(duel.visibility() == RoomVisibility::Public);
    duel.onMessage("ROOM ABCD 2");
    CHECK(duel.phase() == DuelPhase::Room);
    CHECK(duel.visibility() == RoomVisibility::Public);
    duel.leave();
    duel.joinRoom("ABCD");  // a typed code or an invite link: it could be either
    CHECK(duel.visibility() == RoomVisibility::Unknown);
    CHECK(wire.sent.back() == "JOIN ABCD");
}

TEST_CASE("duel: myName is our room name from the roster") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ROOM ABCD 2");
    CHECK(duel.myName().empty());
    duel.onMessage("PLAYERS 1 1:Sam:0 2:Sam2:0");
    CHECK(duel.myName() == "Sam2");
}

TEST_CASE("duel: START sends SDF with the session's factor") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.setSoftDropFactor(0);
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD 1");
    duel.onMessage("START 7 1 1 2");
    CHECK(std::ranges::count(wire.sent, std::string("SDF 0")) == 1);
    REQUIRE(duel.match() != nullptr);
    CHECK(duel.match()->setup().softDropFactor == 0);
}

TEST_CASE("duel: inputs go out as IN before ATK") {
    Wire wire;
    DuelSession duel(wire.sender());
    startWithWell(duel, wire);
    dropUprightI(*duel.match());
    duel.step(kClearDelayMs);  // the attack goes out when the clear finishes
    duel.update(1);
    const auto in = std::ranges::find_if(wire.sent, isIn);
    const auto atk = std::ranges::find(wire.sent, std::string("ATK 4"));
    REQUIRE(in != wire.sent.end());
    REQUIRE(atk != wire.sent.end());
    CHECK(in < atk);
    CHECK(in->ends_with(":HD"));
}

TEST_CASE("duel: an idle player sends an IN heartbeat every 100 ms") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.step(kInputHeartbeatMs - 1);
    duel.update(kInputHeartbeatMs - 1);
    CHECK(insOf(wire.sent).empty());
    duel.step(1);
    duel.update(1);
    CHECK(insOf(wire.sent) == std::vector<std::string>{"IN 100"});
}

TEST_CASE("duel: an IN carries at most kMaxInputEvents entries, split by time") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    for (int i = 0; i < kMaxInputEvents + 6; ++i) {
        duel.match()->softDrop(i % 2 == 0);
        duel.step(1);
    }
    duel.update(kMaxInputEvents + 6);
    const std::vector<std::string> ins = insOf(wire.sent);
    REQUIRE(ins.size() == 2);
    int previousEnd = 0;
    std::size_t entries = 0;
    for (const std::string& text : ins) {
        const auto message = decode(text);
        REQUIRE(message.has_value());
        const InputBatch& batch = std::get<InputBatch>(*message);
        CHECK(batch.entries.size() <= kMaxInputEvents);
        for (const LogEntry& entry : batch.entries) CHECK(entry.t >= previousEnd);
        previousEnd = batch.tEnd;
        entries += batch.entries.size();
    }
    CHECK(entries == kMaxInputEvents + 6);
    CHECK(previousEnd == kMaxInputEvents + 6);
}

TEST_CASE("duel: GARB is accepted once, logged as G<id>x<lines>") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.takeEvents();
    duel.onMessage("GARB 1 3");
    duel.onMessage("GARB 1 3");
    CHECK(duel.game()->pendingGarbage() == 3);
    CHECK(duel.sentTheirs() == 3);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::Attacked});
    duel.step(1);
    duel.update(1);
    const std::vector<std::string> ins = insOf(wire.sent);
    REQUIRE(ins.size() == 1);
    CHECK(ins[0] == "IN 1 0:G1x3");
}

TEST_CASE("duel: RESYNC keeps local events after t and ignores a GARB it already has") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.step(50);
    duel.match()->input(Action::HardDrop);
    duel.step(100);
    duel.match()->input(Action::Left);
    duel.step(50);
    REQUIRE(duel.match()->t() == 200);
    duel.update(200);  // our events go out
    duel.onMessage("RESYNC 100 50:HD 80:G1x2");
    REQUIRE(duel.match() != nullptr);
    CHECK(duel.match()->t() == 200);
    CHECK(duel.match()->log() == std::vector<LogEntry>{{50, LogCode::HardDrop},
                                                       {80, LogCode::Garbage, 1, 2},
                                                       {150, LogCode::Left}});
    CHECK(duel.match()->takeUnsent().empty());  // the server has our own events, or they are on the way
    const int pending = duel.game()->pendingGarbage();
    duel.onMessage("GARB 1 2");
    CHECK(duel.game()->pendingGarbage() == pending);
}

TEST_CASE("duel: RESYNC to the same log leaves the board unchanged") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("GARB 1 2");
    duel.step(50);
    duel.match()->input(Action::HardDrop);
    duel.step(100);
    duel.match()->input(Action::Left);
    duel.match()->softDrop(true);
    duel.step(50);
    duel.update(200);
    const std::uint64_t before = duel.match()->hash();
    Resync resync{100, {}};
    for (const LogEntry& entry : duel.match()->log())
        if (entry.t <= resync.t) resync.entries.push_back(entry);
    duel.onMessage(encode(resync));
    CHECK(duel.match()->hash() == before);
    CHECK(duel.match()->t() == 200);
}

TEST_CASE("duel: a GARB just before a RESYNC is still acknowledged once") {
    for (const int resyncT : {100, 200}) {  // before the ack's t, and at it
        CAPTURE(resyncT);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.step(200);
        duel.update(200);
        wire.sent.clear();
        duel.onMessage("GARB 1 2");  // logged at t = 200, not sent yet
        duel.onMessage(encode(Resync{resyncT, {}}));
        CHECK(duel.game()->pendingGarbage() == 2);
        CHECK(duel.match()->hasGarbage(1));
        duel.step(1);
        duel.update(1);
        CHECK(insOf(wire.sent) == std::vector<std::string>{"IN 201 200:G1x2"});
        duel.onMessage("GARB 1 2");
        CHECK(duel.game()->pendingGarbage() == 2);
    }
}
