#include "app/duel.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string_view>
#include <utility>
#include <variant>

namespace tetris {

namespace {

DuelOutcome outcomeOf(MatchResult result) {
    switch (result) {
    case MatchResult::Win:
        return DuelOutcome::Won;
    case MatchResult::Lose:
        return DuelOutcome::Lost;
    case MatchResult::Draw:
        return DuelOutcome::Draw;
    }
    return DuelOutcome::None;
}

constexpr std::string_view kSeparator = " \xC2\xB7 ";  // a spaced middle dot, UTF-8

std::string versus(int mine, int theirs) { return std::to_string(mine) + " : " + std::to_string(theirs); }

std::string formatted(const char* format, double value) {
    char text[32];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

std::string comboText(int maxCombo) { return maxCombo > 0 ? std::to_string(maxCombo) : "-"; }

bool royaleResult(const DuelSession& duel) {
    return duel.phase() == DuelPhase::Result && duel.mode() == DuelMode::Royale;
}

const Snapshot kNoSnapshot{};
const std::string kNoName;

}  // namespace

int frameBudgetMs(std::uint64_t elapsedMs, bool inMatch) {
    return static_cast<int>(std::min<std::uint64_t>(elapsedMs, inMatch ? kMaxCatchUpMs : kMaxFrameMs));
}

const Snapshot& DuelSession::opponent() const {
    return opponents_.empty() ? kNoSnapshot : opponents_.front().snapshot;
}

const std::string& DuelSession::myName() const {
    const auto me = std::ranges::find(roster_, myId_, &RosterEntry::id);
    return me != roster_.end() ? me->name : kNoName;
}

const std::string& DuelSession::opponentName() const {
    return opponents_.empty() ? kNoName : opponents_.front().name;
}

bool DuelSession::frozen() const {
    if (phase_ != DuelPhase::Playing) return false;
    return mode_ == DuelMode::Royale ? match_->frozen() : match_->frozen() || matchClockMs_ == 0;
}

std::string resultReasonText(const DuelSession& duel) {
    if (duel.phase() != DuelPhase::Result) return {};
    if (duel.outcome() == DuelOutcome::OpponentLeft && duel.mode() == DuelMode::Royale) return "Match ended";
    if (duel.outcome() == DuelOutcome::OpponentLeft)
        return (duel.opponentName().empty() ? std::string("Opponent") : duel.opponentName()) + " left the match";
    if (duel.reason() == DuelReason::KOs) return std::to_string(kKosToWin) + " KOs";
    std::string text = "Time up" + std::string(kSeparator) + "KO " + versus(duel.kosMine(), duel.kosTheirs());
    if (duel.kosMine() == duel.kosTheirs())
        text += std::string(kSeparator) + versus(duel.sentMine(), duel.sentTheirs()) + " sent";
    return text;
}

std::vector<Standing> royaleStandings(const DuelSession& duel) {
    if (!royaleResult(duel)) return {};
    std::vector<Standing> standings;
    for (const OpponentView& opponent : duel.opponents())
        standings.push_back({opponent.place, opponent.name, opponent.snapshot.stats.attack, false});
    // Our name stays in the roster while we're in the room.
    standings.push_back({duel.myPlace(), duel.myName().empty() ? std::string("You") : duel.myName(),
                         duel.game() ? duel.game()->stats().attack : 0, true});
    // An unplaced player (place 0) can't happen once OUT ... 1 arrives, but would sort last rather than first.
    std::ranges::stable_sort(standings, {}, [](const Standing& s) { return s.place > 0 ? s.place : INT_MAX; });
    return standings;
}

std::string royaleResultTitle(const DuelSession& duel) {
    if (!royaleResult(duel)) return {};
    if (duel.outcome() == DuelOutcome::Won) return "YOU WIN";
    return "#" + std::to_string(duel.myPlace()) + " of " + std::to_string(duel.participants());
}

MatchRates ratesOf(const GameStats& stats, int playedMs) {
    if (playedMs <= 0) return {};
    const double seconds = playedMs / 1000.0;
    return {.pps = stats.pieces / seconds,
            .apm = stats.attack / (seconds / 60.0),
            .vs = (stats.attack + stats.garbageCleared) / seconds * 100.0};
}

std::vector<StatRow> resultStatRows(const DuelSession& duel) {
    if (duel.phase() != DuelPhase::Result) return {};
    const GameStats mine = duel.game() ? duel.game()->stats() : GameStats{};
    const GameStats& theirs = duel.opponent().stats;
    const MatchRates myRates = ratesOf(mine, duel.playedMs());
    const MatchRates theirRates = ratesOf(theirs, duel.playedMs());
    return {
        {"PPS", formatted("%.2f", myRates.pps), formatted("%.2f", theirRates.pps)},
        {"APM", formatted("%.1f", myRates.apm), formatted("%.1f", theirRates.apm)},
        {"VS", formatted("%.1f", myRates.vs), formatted("%.1f", theirRates.vs)},
        {"Sent", std::to_string(duel.sentMine()), std::to_string(duel.sentTheirs())},
        {"Cancelled", std::to_string(mine.cancelled), std::to_string(theirs.cancelled)},
        {"Tetrises", std::to_string(mine.tetrises), std::to_string(theirs.tetrises)},
        {"T-spins", std::to_string(mine.tSpins), std::to_string(theirs.tSpins)},
        {"Max combo", comboText(mine.maxCombo), comboText(theirs.maxCombo)},
    };
}

void DuelSession::openLobby() {
    reset();
    phase_ = DuelPhase::Lobby;
    send(ListRooms{});
}

// Resets for a new room request, remembering whether it came from the lobby.
void DuelSession::beginRequest(RoomVisibility visibility, bool quick) {
    const bool fromLobby = phase_ == DuelPhase::Lobby;
    reset();
    phase_ = DuelPhase::Waiting;
    fromLobby_ = fromLobby;
    visibility_ = visibility;
    quick_ = quick;
}

void DuelSession::createRoom(int startLevel, bool isPublic) {
    beginRequest(isPublic ? RoomVisibility::Public : RoomVisibility::Private, false);
    send(CreateRoom{clampStartLevel(startLevel), isPublic});
}

void DuelSession::joinRoom(const std::string& code, RoomVisibility visibility) {
    beginRequest(visibility, false);
    send(JoinRoom{code});
}

void DuelSession::quickMatch(int startLevel) {
    beginRequest(RoomVisibility::Public, true);
    send(QuickMatch{clampStartLevel(startLevel)});
}

void DuelSession::begin() {
    if (phase_ != DuelPhase::Room && phase_ != DuelPhase::Result) return;
    if (!isHost() || roster_.size() < 2) return;
    send(Begin{});
}

void DuelSession::leave() { reset(); }

void DuelSession::onDisconnected() {
    if (phase_ == DuelPhase::Idle) return;
    reset();
    error_ = "Connection lost";
}

void DuelSession::onMessage(std::string_view text) {
    const std::optional<Message> message = decode(text);
    if (!message || phase_ == DuelPhase::Idle) return;
    if (const auto* room = std::get_if<RoomEntered>(&*message)) {
        if (phase_ == DuelPhase::Waiting) {
            roomCode_ = room->code;
            myId_ = room->playerId;
            phase_ = DuelPhase::Room;
        }
    } else if (const auto* roster = std::get_if<Roster>(&*message)) {
        if (phase_ == DuelPhase::Room || inMatch() || phase_ == DuelPhase::Result) {
            hostId_ = roster->hostId;
            roster_ = roster->players;
        }
    } else if (const auto* start = std::get_if<StartMatch>(&*message)) {
        const bool betweenMatches = phase_ == DuelPhase::Room || phase_ == DuelPhase::Result;
        const bool listsMe = std::ranges::find(start->players, myId_) != start->players.end();
        if (betweenMatches && listsMe) startMatch(*start);
    } else if (const auto* error = std::get_if<ServerError>(&*message)) {
        if (phase_ == DuelPhase::Waiting) {
            const bool backToLobby = fromLobby_;
            reset();
            error_ = error->reason;
            if (backToLobby) {  // the server stopped sending the list when we asked for a room
                phase_ = DuelPhase::Lobby;
                send(ListRooms{});
            }
        }
    } else if (const auto* list = std::get_if<RoomList>(&*message)) {
        if (phase_ == DuelPhase::Lobby) rooms_ = list->rooms;
    } else if (const auto* attack = std::get_if<Attack>(&*message)) {
        // The two countdowns drift, so the opponent may already be playing while ours runs.
        if (inMatch()) acceptGarbage(0, attack->lines);  // vs Bot; the server sends GARB
    } else if (const auto* garbage = std::get_if<GarbageIn>(&*message)) {
        // A batch that already came in a RESYNC is in the log.
        if (inMatch() && !match_->hasGarbage(garbage->id)) acceptGarbage(garbage->id, garbage->lines);
    } else if (const auto* correction = std::get_if<Resync>(&*message)) {
        if (inMatch()) resync(*correction);
    } else if (const auto* snapshot = std::get_if<PlayerSnapshot>(&*message)) {
        for (OpponentView& opponent : opponents_)
            if (opponent.id == snapshot->fromId) opponent.snapshot = snapshot->snapshot;
    } else if (const auto* kos = std::get_if<KoCounts>(&*message)) {
        if (inMatch()) {
            if (kos->mine > kosMine_) events_.push_back(DuelEvent::KoMine);
            kosMine_ = kos->mine;
            kosTheirs_ = kos->theirs;
        }
    } else if (const auto* over = std::get_if<MatchOver>(&*message)) {
        if (inMatch()) {
            reason_ = over->reason;
            sentMine_ = over->mySent;
            sentTheirs_ = over->theirSent;
            finish(outcomeOf(over->result));
        }
    } else if (const auto* out = std::get_if<PlayerOut>(&*message)) {
        // A late OUT on the result screen still places a player nobody has placed yet, so the standings never
        // show #0, but it doesn't end the match again.
        const bool late = phase_ == DuelPhase::Result;
        if ((inMatch() || late) && mode_ == DuelMode::Royale) {
            if (out->id == myId_ && (!late || myPlace_ == 0)) {
                myPlace_ = out->place;
                if (out->place == 1) eliminated_ = false;  // survived a simultaneous top-out: the winner isn't out
            }
            for (OpponentView& opponent : opponents_) {
                if (opponent.id != out->id || (late && opponent.place != 0)) continue;
                if (out->place > 1) opponent.alive = false;  // the winner's board stays lit
                opponent.place = out->place;
            }
            if (out->place == 1 && !late) finish(out->id == myId_ ? DuelOutcome::Won : DuelOutcome::Lost);
        }
    } else if (std::holds_alternative<OpponentLeft>(*message)) {
        if (inMatch()) finish(DuelOutcome::OpponentLeft);  // a royale gets LEFT when the server's simulation fails
    }
}

void DuelSession::acceptGarbage(int id, int lines) {
    match_->acceptGarbage(id, lines);
    sentTheirs_ += lines;
    events_.push_back(DuelEvent::Attacked);
}

// Replaces the match with the server's log up to its t, then replays our own later events at their times up to where
// we were. Those are already sent or on their way, so nothing is sent again.
void DuelSession::resync(const Resync& correction) {
    const int now = match_->t();
    const std::uint64_t before = match_->hash();
    std::vector<LogEntry> own;
    std::ranges::copy_if(match_->log(), std::back_inserter(own),
                         [&](const LogEntry& e) { return e.t > correction.t; });
    MatchGame corrected = MatchGame::replay(match_->setup(), correction.entries, correction.t);
    for (const LogEntry& entry : own) {
        corrected.advance(entry.t - corrected.t());
        // A batch the server applied itself, before we acknowledged it, is not taken twice.
        if (entry.code == LogCode::Garbage && corrected.hasGarbage(entry.garbageId)) continue;
        corrected.apply(entry);
    }
    if (corrected.t() < now) corrected.advance(now - corrected.t());
    corrected.takeUnsent();
    // The replay re-earns attacks and events the live game already reported.
    corrected.game().takeOutgoingAttack();
    corrected.game().takeEvents();
    match_ = std::move(corrected);
    topOutsSeen_ = std::min(topOutsSeen_, match_->topOuts());  // a KO only the corrected match has is still reported
    std::fprintf(stderr, "resync t=%d hash %016llx -> %016llx\n", correction.t,
                 static_cast<unsigned long long>(before), static_cast<unsigned long long>(match_->hash()));
}

void DuelSession::step(int ms) {
    if (phase_ == DuelPhase::Playing) match_->advance(ms);
}

void DuelSession::update(int ms) {
    if (phase_ == DuelPhase::Countdown) {
        const int before = countdownMs_;
        countdownMs_ -= ms;
        if (countdownMs_ <= 0) {
            countdownMs_ = 0;
            phase_ = DuelPhase::Playing;
            events_.push_back(DuelEvent::MatchStart);
        } else if ((before - 1) / 1000 != (countdownMs_ - 1) / 1000) {  // the shown second changed
            events_.push_back(DuelEvent::CountdownTick);
        }
        return;
    }
    if (phase_ != DuelPhase::Playing) return;
    const bool royale = mode_ == DuelMode::Royale;
    if (royale) {
        if (eliminated_) return;  // out: the board stays as it died and nothing more goes out
        playedMs_ += ms;
    } else {
        const int clockBefore = matchClockMs_;
        matchClockMs_ = std::max(0, matchClockMs_ - ms);
        playedMs_ += clockBefore - matchClockMs_;  // a duel's play stops at 0:00
    }
    sendInputs();
    // The old messages, which the server relays until it simulates every match itself.
    Game& game = match_->game();
    if (const int lines = game.takeOutgoingAttack(); lines > 0) {
        send(Attack{lines});
        sentMine_ += lines;
        events_.push_back(DuelEvent::Sent);
    }
    // The match handles a top-out at the step it happens: a KO resets the board and starts the flash, an
    // elimination keeps the game-over board.
    const int newTopOuts = match_->topOuts() - topOutsSeen_;
    sinceSnapshotMs_ += ms;
    if (sinceSnapshotMs_ >= kSnapshotIntervalMs || newTopOuts > 0) {
        sinceSnapshotMs_ = 0;
        const Snapshot snapshot = snapshotOf(game);
        if (snapshot != lastSent_) {
            send(snapshot);
            lastSent_ = snapshot;
        }
    }
    for (int i = 0; i < newTopOuts; ++i) send(Dead{});  // the server counts each KO, or places us
    topOutsSeen_ = match_->topOuts();
    if (royale) eliminated_ = match_->eliminated();
}

// IN with the entries the match logged since the last one, or an empty one each kInputHeartbeatMs of match time.
// More than kMaxInputEvents entries (a long catch-up) go out as several INs, each one valid up to its last entry's
// t except the last, which is valid up to now.
void DuelSession::sendInputs() {
    const std::vector<LogEntry> unsent = match_->takeUnsent();
    if (unsent.empty() && match_->t() - lastInT_ < kInputHeartbeatMs) return;
    std::size_t next = 0;
    do {
        const std::size_t end = std::min(unsent.size(), next + kMaxInputEvents);
        InputBatch batch;
        batch.entries.assign(unsent.begin() + static_cast<std::ptrdiff_t>(next),
                             unsent.begin() + static_cast<std::ptrdiff_t>(end));
        batch.tEnd = end < unsent.size() ? batch.entries.back().t : match_->t();
        send(batch);
        next = end;
    } while (next < unsent.size());
    lastInT_ = match_->t();
}

// The other players in START are named from the roster as it is now, so an id reused later keeps the old name.
void DuelSession::startMatch(const StartMatch& start) {
    match_.emplace(MatchSetup{start, softDropFactor_});
    send(SoftDropSetting{softDropFactor_});
    phase_ = DuelPhase::Countdown;
    outcome_ = DuelOutcome::None;
    mode_ = start.players.size() == 2 ? DuelMode::Duel : DuelMode::Royale;
    participants_ = static_cast<int>(start.players.size());
    countdownMs_ = kCountdownMs;
    sinceSnapshotMs_ = 0;
    lastInT_ = 0;
    topOutsSeen_ = 0;
    lastSent_.reset();
    std::vector<int> ids = start.players;
    std::ranges::sort(ids);
    opponents_.clear();
    for (int id : ids) {
        if (id == myId_) continue;
        const auto entry = std::ranges::find(roster_, id, &RosterEntry::id);
        OpponentView opponent;
        opponent.id = id;
        opponent.name = entry != roster_.end() ? entry->name : "P" + std::to_string(id);
        opponents_.push_back(std::move(opponent));
    }
    resetMatchStats();
    events_.push_back(DuelEvent::CountdownTick);
}

void DuelSession::finish(DuelOutcome outcome) {
    phase_ = DuelPhase::Result;
    outcome_ = outcome;
    if (outcome == DuelOutcome::Won) events_.push_back(DuelEvent::Won);
    if (outcome == DuelOutcome::Lost) events_.push_back(DuelEvent::Lost);
    if (outcome == DuelOutcome::Draw) events_.push_back(DuelEvent::Draw);
}

void DuelSession::reset() {
    phase_ = DuelPhase::Idle;
    outcome_ = DuelOutcome::None;
    match_.reset();
    lastSent_.reset();
    roomCode_.clear();
    myId_ = 0;
    hostId_ = 0;
    roster_.clear();
    mode_ = DuelMode::Duel;
    opponents_.clear();
    participants_ = 0;
    rooms_.clear();
    fromLobby_ = false;
    visibility_ = RoomVisibility::Unknown;
    quick_ = false;
    error_.clear();
    countdownMs_ = 0;
    sinceSnapshotMs_ = 0;
    lastInT_ = 0;
    topOutsSeen_ = 0;
    resetMatchStats();
}

void DuelSession::resetMatchStats() {
    reason_ = DuelReason::KOs;
    kosMine_ = 0;
    kosTheirs_ = 0;
    sentMine_ = 0;
    sentTheirs_ = 0;
    matchClockMs_ = kMatchMs;
    playedMs_ = 0;
    eliminated_ = false;
    myPlace_ = 0;
}

}  // namespace tetris
