#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/game.hpp"
#include "net/protocol.hpp"

namespace tetris {

inline constexpr int kCountdownMs = 3000;  // must match countdown in server/hub.go
inline constexpr int kSnapshotIntervalMs = 100;
inline constexpr int kMatchMs = 120000;  // display clock; the server's -match default is the same 2 minutes
inline constexpr int kKoFlashMs = 1000;
inline constexpr int kMaxFrameMs = 100;  // longer stalls are dropped, not replayed, outside a match
inline constexpr int kMaxCatchUpMs = 130000;  // a match replays stalls up to this long: the clock plus slack

// How much of `elapsedMs` of wall time to simulate this frame. A browser stops or throttles a hidden tab's
// loop, so a running match replays the gap: gravity keeps going and the opponent keeps getting snapshots.
int frameBudgetMs(std::uint64_t elapsedMs, bool inMatch);

// Lobby: connected, not in a room, watching the list of open public rooms.
enum class DuelPhase { Idle, Lobby, Waiting, Countdown, Playing, Result };
enum class DuelOutcome { None, Won, Lost, Draw, OpponentLeft };
using DuelReason = MatchReason;  // why a Won, Lost or Draw match ended, from the server's OVER

// Something that just happened in the duel, for sound. Our own top-out is reported by the Game.
enum class DuelEvent { CountdownTick, MatchStart, Attacked, KoMine, Won, Lost, Draw };

// The local game for a match: same seed (so the same pieces) as the opponent, and no pausing.
Game makeDuelGame(const StartMatch& start);

// One player's side of a duel. Reacts to server messages, owns the local Game while a match runs, and
// reports attacks, snapshots and top-outs through `send`. The server referees KOs and the result. Knows nothing about sockets or SDL.
class DuelSession {
public:
    using Send = std::function<void(const std::string&)>;
    using MakeGame = std::function<Game(const StartMatch&)>;

    explicit DuelSession(Send send, MakeGame makeGame = makeDuelGame)
        : send_(std::move(send)), makeGame_(std::move(makeGame)) {}

    void openLobby();  // Idle -> Lobby; the server then pushes the room list
    void createRoom(int startLevel, bool isPublic = false);
    void joinRoom(const std::string& code);
    void quickMatch(int startLevel);  // joins the oldest open public room, or waits in a new one
    void requestRematch();
    void leave();  // back to Idle; the caller closes the connection
    void onMessage(std::string_view text);
    void onDisconnected();
    // Call once per frame, after the local game has been advanced by `ms`.
    void update(int ms);

    DuelPhase phase() const { return phase_; }
    bool inMatch() const { return phase_ == DuelPhase::Countdown || phase_ == DuelPhase::Playing; }
    DuelOutcome outcome() const { return outcome_; }
    const std::string& roomCode() const { return roomCode_; }  // set once the server confirms a created room
    const std::string& error() const { return error_; }        // why the session last went back to Idle or Lobby
    const Snapshot& opponent() const { return opponent_; }
    int countdownMs() const { return countdownMs_; }
    bool rematchRequested() const { return rematchRequested_; }
    bool opponentGone() const { return opponentGone_; }
    const std::string& opponentName() const { return opponentName_; }  // from OPP; empty if not sent
    const std::vector<RoomInfo>& rooms() const { return rooms_; }      // the lobby list, while in Lobby
    bool publicRoom() const { return publicRoom_; }                    // the room we wait in is listed
    bool quickMatching() const { return quick_; }                      // waiting came from Quick match
    DuelReason reason() const { return reason_; }
    int kosMine() const { return kosMine_; }  // KOs this player has scored, from the server's KO messages
    int kosTheirs() const { return kosTheirs_; }
    // Lines sent this match: a live tally while playing, then the server's totals once OVER arrives.
    int sentMine() const { return sentMine_; }
    int sentTheirs() const { return sentTheirs_; }
    int matchClockMs() const { return matchClockMs_; }  // display only; the server decides time-up
    int koFlashMs() const { return koFlashMs_; }        // > 0 while "KO!" shows after our top-out
    // While true the local game must not tick or take input: during the KO flash, or once the clock hits 0.
    bool frozen() const;
    // Events since the last call, oldest first; empties the list.
    std::vector<DuelEvent> takeEvents() { return std::exchange(events_, {}); }
    Game* game() { return game_ ? &*game_ : nullptr; }
    const Game* game() const { return game_ ? &*game_ : nullptr; }

private:
    void beginRequest(bool isPublic, bool quick);
    void send(const Message& message) { send_(encode(message)); }
    void startMatch(const StartMatch& start);
    void finish(DuelOutcome outcome);
    void reset();
    void resetMatchStats();

    Send send_;
    MakeGame makeGame_;
    DuelPhase phase_ = DuelPhase::Idle;
    DuelOutcome outcome_ = DuelOutcome::None;
    std::optional<Game> game_;
    Snapshot opponent_;
    std::optional<Snapshot> lastSent_;
    std::string roomCode_;
    std::string opponentName_;
    std::vector<RoomInfo> rooms_;
    bool fromLobby_ = false;  // Waiting was entered from Lobby, so a refusal goes back there
    bool publicRoom_ = false;
    bool quick_ = false;
    std::string error_;
    int countdownMs_ = 0;
    int sinceSnapshotMs_ = 0;
    bool rematchRequested_ = false;
    bool opponentGone_ = false;
    DuelReason reason_ = DuelReason::KOs;
    int kosMine_ = 0;
    int kosTheirs_ = 0;
    int sentMine_ = 0;
    int sentTheirs_ = 0;
    int matchClockMs_ = 0;
    int koFlashMs_ = 0;
    std::vector<DuelEvent> events_;
};

// The line under the result title, e.g. "Time up · KO 1 : 1 · 12 : 7 sent". Empty outside Result.
std::string resultReasonText(const DuelSession& duel);

}  // namespace tetris
