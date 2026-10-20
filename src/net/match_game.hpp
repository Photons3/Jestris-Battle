#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/game.hpp"
#include "net/protocol.hpp"

namespace tetris {

inline constexpr int kMatchMs = 120000;  // a duel's clock; the server's -match default is the same 2 minutes
inline constexpr int kKoFlashMs = 1000;
inline constexpr int kRoyaleLevelStepMs = 30000;  // a royale's gravity floor rises one level per this much play

// The local game for a match: same seed (so the same pieces) as the opponents, and no pausing. A royale (3 or
// more players) gets hole garbage instead of bombs.
Game makeDuelGame(const StartMatch& start);

struct MatchSetup {
    StartMatch start;
    int softDropFactor = kDefaultSoftDropFactor;
    bool royale() const { return start.players.size() >= 3; }
};

// One player's game in a networked match, stepped 1 ms at a time under the match rules (KO flash, time-up,
// royale elimination and level floor), with a log of everything that changed it. The client plays it live and the
// server replays the log; both land on the same state however the client split its frames.
class MatchGame {
public:
    // Without `keepLog` (the server's simulation, which keeps its own record) nothing is logged: log() and
    // takeUnsent() stay empty and hasGarbage() is always false. Play is the same either way.
    explicit MatchGame(const MatchSetup& setup, bool keepLog = true);

    void input(Action action);  // applied now and logged at t(); Pause, and any input while frozen, does nothing
    void softDrop(bool on);     // logs SoftOn/SoftOff only on a change
    void acceptGarbage(int id, int lines);  // receiveGarbage(lines) now, logged as Garbage{id, lines} at t()
    void apply(const LogEntry& entry);      // the call `entry` stands for, at t() (its own t is not checked)
    void advance(int ms);

    int t() const { return t_; }
    // In the KO flash, at or after time-up (duel), or eliminated (royale): the game does not tick or take input.
    bool frozen() const { return stopped_ || t_ < koUntil_; }
    bool eliminated() const { return setup_.royale() && topOuts_ > 0; }
    // A duel top-out whose stack still blocked the spawn: the match ends on this KO.
    bool buried() const { return buried_; }
    int topOuts() const { return topOuts_; }  // KOs (duel) or 1 once eliminated (royale)
    int koFlashMs() const { return t_ < koUntil_ ? koUntil_ - t_ : 0; }  // ms of KO flash left, for display
    bool hasGarbage(int id) const;  // a Garbage entry with this id is already in the log
    // Entries logged since the last call, oldest first.
    std::vector<LogEntry> takeUnsent();
    const std::vector<LogEntry>& log() const { return log_; }
    const MatchSetup& setup() const { return setup_; }
    Game& game() { return game_; }
    const Game& game() const { return game_; }
    // game().stateHash() mixed with t, the frozen-until time and topOuts, for replay checks.
    std::uint64_t hash() const;

    // A fresh match advanced to t == upTo with `log` applied: each entry before the step at its t, in log order.
    // Entries stamped upTo are applied too (as a live game at t() == upTo that took them); later ones are left out.
    static MatchGame replay(const MatchSetup& setup, const std::vector<LogEntry>& log, int upTo);

private:
    void step();
    void record(const LogEntry& entry);

    MatchSetup setup_;
    Game game_;
    int t_ = 0;
    int koUntil_ = 0;       // the KO flash freezes steps while t < koUntil_
    bool stopped_ = false;  // frozen for good: time-up (duel), eliminated (royale), or buried
    bool buried_ = false;   // a duel KO left no room to spawn
    int topOuts_ = 0;
    bool softDrop_ = false;
    bool keepLog_ = true;
    std::vector<LogEntry> log_;
    std::size_t sent_ = 0;  // log entries already returned by takeUnsent()
};

}  // namespace tetris
