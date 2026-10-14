#include <doctest/doctest.h>

#include <string>
#include <utility>
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

// Creates a room, receives START and runs out the countdown; clears what was sent so far.
void startMatch(DuelSession& duel, Wire& wire) {
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD");
    duel.onMessage("START 7 1");
    duel.update(kCountdownMs);
    REQUIRE(duel.phase() == DuelPhase::Playing);
    wire.sent.clear();
}

}  // namespace

TEST_CASE("duel: creating a room sends CREATE and shows the code") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(3);
    CHECK(wire.sent == std::vector<std::string>{"CREATE 3"});
    CHECK(duel.phase() == DuelPhase::Waiting);
    CHECK(duel.roomCode().empty());
    duel.onMessage("ROOM ABCD");
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
    duel.onMessage("START 7 2");
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

TEST_CASE("duel: incoming attacks queue garbage on the local game") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 2");
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: line clears are sent as attacks") {
    Wire wire;
    DuelSession duel(wire.sender(), [](const StartMatch& start) {
        return Game(seedWithFirst(PieceType::I), start.startLevel, boardWithWell(4, 5));
    });
    startMatch(duel, wire);
    dropUprightI(*duel.game());
    duel.game()->tick(kClearDelayMs);
    duel.update(0);
    CHECK(wire.sent == std::vector<std::string>{"ATK 4"});
    CHECK(duel.sentMine() == 4);
}

TEST_CASE("duel: a clear still animating at 0:00 is settled and sent") {
    Wire wire;
    DuelSession duel(wire.sender(), [](const StartMatch& start) {
        return Game(seedWithFirst(PieceType::I), start.startLevel, boardWithWell(4, 5));
    });
    startMatch(duel, wire);
    dropUprightI(*duel.game());
    REQUIRE(duel.game()->state() == GameState::Clearing);
    duel.update(kMatchMs);  // the app does not tick the game once the clock is at 0:00
    CHECK(duel.frozen());
    CHECK(wire.sent.size() >= 1);
    CHECK(wire.sent[0] == "ATK 4");
    CHECK(duel.sentMine() == 4);
}

TEST_CASE("duel: topping out sends a final snapshot and DEAD, resets the board and keeps playing") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    REQUIRE(wire.sent.size() == 2);
    CHECK(wire.sent[0].starts_with("SNAP "));
    CHECK(wire.sent[1] == "DEAD");
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

TEST_CASE("duel: opponent leaving mid-match wins and blocks rematch") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
    CHECK(duel.opponentGone());
    duel.requestRematch();
    CHECK(wire.sent.empty());
}

TEST_CASE("duel: rematch is sent once and START begins a fresh match") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 3");
    duel.onMessage("KO 3 0");
    duel.onMessage("OVER WIN KO 3 0");
    duel.requestRematch();
    duel.requestRematch();
    CHECK(wire.sent == std::vector<std::string>{"REMATCH"});
    CHECK(duel.rematchRequested());
    duel.onMessage("START 9 1");
    CHECK(duel.phase() == DuelPhase::Countdown);
    CHECK(duel.outcome() == DuelOutcome::None);
    CHECK_FALSE(duel.rematchRequested());
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
    Snapshot snapshot{500, 2, 1, {}};
    snapshot.cells[0] = PieceType::Garbage;
    duel.onMessage(encode(snapshot));
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
    duel.onMessage("START 1 1");
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
    duel.onMessage("START 9 1");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.game() == before);
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: START after the opponent left does not start a match") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("LEFT");
    duel.onMessage("START 9 1");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
}

TEST_CASE("duel: START on the result screen is ignored without a rematch request") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("START 9 1");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: a stray error mid-match is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.error().empty());
}

TEST_CASE("duel: LEFT while waiting for an opponent returns to idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOM ABCD");
    duel.onMessage("LEFT");
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.error() == "Opponent left");
    CHECK(duel.game() == nullptr);
}

TEST_CASE("duel: an attack during the countdown is queued") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("START 7 1");
    REQUIRE(duel.phase() == DuelPhase::Countdown);
    duel.onMessage("ATK 2");
    CHECK(duel.game()->pendingGarbage() == 2);
}

TEST_CASE("duel: OVER during our countdown ends the match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("START 7 1");
    REQUIRE(duel.phase() == DuelPhase::Countdown);
    duel.onMessage("OVER WIN KO 0 0");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: a room that closes before START does not start a match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("LEFT");
    duel.onMessage("START 7 1");
    CHECK(duel.phase() == DuelPhase::Idle);
    CHECK(duel.game() == nullptr);
}

TEST_CASE("duel: the KO flash freezes the game for 1000 ms while the clock keeps running") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    CHECK_FALSE(duel.frozen());
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    CHECK(duel.frozen());
    duel.update(kKoFlashMs - 1);
    CHECK(duel.frozen());
    CHECK(duel.matchClockMs() == kMatchMs - (kKoFlashMs - 1));
    duel.update(1);
    CHECK_FALSE(duel.frozen());
    CHECK(duel.koFlashMs() == 0);
}

TEST_CASE("duel: garbage arriving during the KO flash is queued on the fresh board") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
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
    duel.onMessage("START 7 1");
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
    duel.onMessage("START 7 1");
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
        {{"LEFT"}, "Opponent left the match"},
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
    duel.onMessage("START 7 1");
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
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    CHECK(duel.game()->takeEvents() ==
          std::vector<GameEvent>{{GameEventType::HardDropped}, {GameEventType::ToppedOut}});
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
    duel.onMessage("START 7 1");
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

TEST_CASE("duel: OPP while waiting names the opponent, and leaving forgets it") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("OPP Kai");
    CHECK(duel.opponentName() == "Kai");
    duel.onMessage("START 7 1");
    CHECK(duel.opponentName() == "Kai");
    duel.leave();
    CHECK(duel.opponentName().empty());
}

TEST_CASE("duel: OPP outside Waiting is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.onMessage("OPP Kai");  // Idle
    CHECK(duel.opponentName().empty());
    startMatch(duel, wire);
    duel.onMessage("OPP Zed");  // Playing
    CHECK(duel.opponentName().empty());
}

TEST_CASE("duel: the opponent-left line uses the name when known") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("OPP Kai");
    duel.onMessage("START 7 1");
    duel.onMessage("LEFT");
    CHECK(resultReasonText(duel) == "Kai left the match");
}
