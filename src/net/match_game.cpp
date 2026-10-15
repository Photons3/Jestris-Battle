#include "net/match_game.hpp"

#include <algorithm>
#include <climits>

namespace tetris {

Game makeDuelGame(const StartMatch& start) {
    const GarbageStyle garbage = start.players.size() >= 3 ? GarbageStyle::Holes : GarbageStyle::Bombs;
    return Game(start.seed, start.startLevel, Board{}, GameOptions{.allowPause = false, .garbage = garbage});
}

namespace {

bool logCodeOf(Action action, LogCode& code) {
    switch (action) {
    case Action::Left: code = LogCode::Left; return true;
    case Action::Right: code = LogCode::Right; return true;
    case Action::RotateCW: code = LogCode::RotateCW; return true;
    case Action::RotateCCW: code = LogCode::RotateCCW; return true;
    case Action::HardDrop: code = LogCode::HardDrop; return true;
    case Action::Hold: code = LogCode::Hold; return true;
    case Action::LeftWall: code = LogCode::LeftWall; return true;
    case Action::RightWall: code = LogCode::RightWall; return true;
    case Action::Pause: return false;
    }
    return false;
}

}  // namespace

MatchGame::MatchGame(const MatchSetup& setup, bool keepLog)
    : setup_(setup), game_(makeDuelGame(setup.start)), keepLog_(keepLog) {}

void MatchGame::record(const LogEntry& entry) {
    if (keepLog_) log_.push_back(entry);
}

void MatchGame::input(Action action) {
    LogCode code;
    if (!logCodeOf(action, code) || frozen()) return;
    game_.apply(action);
    record({t_, code});
}

void MatchGame::softDrop(bool on) {
    if (on == softDrop_) return;
    softDrop_ = on;
    record({t_, on ? LogCode::SoftOn : LogCode::SoftOff});
}

void MatchGame::acceptGarbage(int id, int lines) {
    game_.receiveGarbage(lines);
    record({t_, LogCode::Garbage, id, lines});
}

void MatchGame::apply(const LogEntry& entry) {
    switch (entry.code) {
    case LogCode::Left: input(Action::Left); break;
    case LogCode::Right: input(Action::Right); break;
    case LogCode::RotateCW: input(Action::RotateCW); break;
    case LogCode::RotateCCW: input(Action::RotateCCW); break;
    case LogCode::HardDrop: input(Action::HardDrop); break;
    case LogCode::Hold: input(Action::Hold); break;
    case LogCode::LeftWall: input(Action::LeftWall); break;
    case LogCode::RightWall: input(Action::RightWall); break;
    case LogCode::SoftOn: softDrop(true); break;
    case LogCode::SoftOff: softDrop(false); break;
    case LogCode::Garbage: acceptGarbage(entry.garbageId, entry.garbageLines); break;
    }
}

void MatchGame::advance(int ms) {
    for (int i = 0; i < ms; ++i) step();
}

// One millisecond of the match. The order is the spec's (server-authority design, section 4) and must not change:
// the server replays logs with this same code.
void MatchGame::step() {
    const bool royale = setup_.royale();
    if (royale) game_.setLevelFloor(setup_.start.startLevel + t_ / kRoyaleLevelStepMs);
    if (!frozen()) {
        game_.setSoftDrop(softDrop_);
        game_.setSoftDropFactor(setup_.softDropFactor);
        game_.tick(1);
    }
    if (!stopped_ && game_.state() == GameState::GameOver) {
        ++topOuts_;
        if (royale) {
            stopped_ = true;  // eliminated: the board stays on its game-over state
        } else {
            game_.resetBoard();
            koUntil_ = t_ + 1 + kKoFlashMs;  // the next kKoFlashMs steps don't tick
        }
    }
    if (!royale && t_ + 1 == kMatchMs) {
        while (game_.state() == GameState::Clearing) game_.tick(1);  // a clear still animating is still sent
        stopped_ = true;
    }
    ++t_;
    // The floor for the step about to run, so level() already shows it once t reaches a multiple of
    // kRoyaleLevelStepMs. The step itself sets the same floor again before it ticks.
    if (royale) game_.setLevelFloor(setup_.start.startLevel + t_ / kRoyaleLevelStepMs);
}

bool MatchGame::hasGarbage(int id) const {
    return std::any_of(log_.begin(), log_.end(),
                       [id](const LogEntry& e) { return e.code == LogCode::Garbage && e.garbageId == id; });
}

std::vector<LogEntry> MatchGame::takeUnsent() {
    std::vector<LogEntry> unsent(log_.begin() + static_cast<std::ptrdiff_t>(sent_), log_.end());
    sent_ = log_.size();
    return unsent;
}

// Continues game().stateHash()'s FNV-1a 64 with t, the frozen-until time (INT_MAX once frozen for good) and
// topOuts, each as 4 little-endian bytes.
std::uint64_t MatchGame::hash() const {
    std::uint64_t h = game_.stateHash();
    auto i32 = [&h](std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<std::uint8_t>(u >> (8 * i));
            h *= 1099511628211ull;
        }
    };
    i32(t_);
    i32(stopped_ ? INT_MAX : koUntil_);
    i32(topOuts_);
    return h;
}

MatchGame MatchGame::replay(const MatchSetup& setup, const std::vector<LogEntry>& log, int upTo) {
    MatchGame match(setup);
    std::size_t next = 0;
    while (match.t_ < upTo) {
        for (; next < log.size() && log[next].t <= match.t_; ++next) match.apply(log[next]);
        match.step();
    }
    for (; next < log.size() && log[next].t <= upTo; ++next) match.apply(log[next]);  // this instant's events
    return match;
}

}  // namespace tetris
