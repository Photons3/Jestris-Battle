#include "app/duel.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <string_view>
#include <utility>
#include <variant>

namespace tetris {

Game makeDuelGame(const StartMatch& start) {
    const GarbageStyle garbage = start.players.size() >= 3 ? GarbageStyle::Holes : GarbageStyle::Bombs;
    return Game(start.seed, start.startLevel, Board{}, GameOptions{.allowPause = false, .garbage = garbage});
}

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

const std::string& DuelSession::opponentName() const {
    return opponents_.empty() ? kNoName : opponents_.front().name;
}

bool DuelSession::frozen() const {
    return phase_ == DuelPhase::Playing && mode_ == DuelMode::Duel && (koFlashMs_ > 0 || matchClockMs_ == 0);
}

std::string resultReasonText(const DuelSession& duel) {
    if (duel.phase() != DuelPhase::Result) return {};
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
    const auto me = std::ranges::find(duel.roster(), duel.myId(), &RosterEntry::id);  // our name stays while we're in
    standings.push_back({duel.myPlace(), me != duel.roster().end() ? me->name : std::string("You"),
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
void DuelSession::beginRequest(bool isPublic, bool quick) {
    const bool fromLobby = phase_ == DuelPhase::Lobby;
    reset();
    phase_ = DuelPhase::Waiting;
    fromLobby_ = fromLobby;
    publicRoom_ = isPublic;
    quick_ = quick;
}

void DuelSession::createRoom(int startLevel, bool isPublic) {
    beginRequest(isPublic, false);
    send(CreateRoom{clampStartLevel(startLevel), isPublic});
}

void DuelSession::joinRoom(const std::string& code) {
    beginRequest(false, false);
    send(JoinRoom{code});
}

void DuelSession::quickMatch(int startLevel) {
    beginRequest(true, true);
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
        if (inMatch()) {
            game_->receiveGarbage(attack->lines);
            sentTheirs_ += attack->lines;
            events_.push_back(DuelEvent::Attacked);
        }
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
        if (inMatch() && mode_ == DuelMode::Royale) {
            if (out->id == myId_) myPlace_ = out->place;
            for (OpponentView& opponent : opponents_) {
                if (opponent.id != out->id) continue;
                opponent.alive = false;
                opponent.place = out->place;
            }
            if (out->place == 1) finish(out->id == myId_ ? DuelOutcome::Won : DuelOutcome::Lost);
        }
    } else if (std::holds_alternative<OpponentLeft>(*message)) {
        if (inMatch() && mode_ == DuelMode::Duel) finish(DuelOutcome::OpponentLeft);  // only duels send LEFT
    }
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
        game_->setLevelFloor(startLevel_ + playedMs_ / kRoyaleLevelStepMs);
    } else {
        const int clockBefore = matchClockMs_;
        matchClockMs_ = std::max(0, matchClockMs_ - ms);
        playedMs_ += clockBefore - matchClockMs_;  // a duel's play stops at 0:00
        koFlashMs_ = std::max(0, koFlashMs_ - ms);
        // The app stops ticking the game at 0:00, so a clear still animating would never be sent.
        if (matchClockMs_ == 0)
            while (game_->state() == GameState::Clearing) game_->tick(1);
    }
    if (const int lines = game_->takeOutgoingAttack(); lines > 0) {
        send(Attack{lines});
        sentMine_ += lines;
        events_.push_back(DuelEvent::Sent);
    }
    const bool toppedOut = game_->state() == GameState::GameOver;
    sinceSnapshotMs_ += ms;
    if (sinceSnapshotMs_ >= kSnapshotIntervalMs || toppedOut) {
        sinceSnapshotMs_ = 0;
        const Snapshot snapshot = snapshotOf(*game_);
        if (snapshot != lastSent_) {
            send(snapshot);
            lastSent_ = snapshot;
        }
    }
    if (toppedOut && royale) {  // eliminated: the server places us, the board stays on its game-over state
        send(Dead{});
        eliminated_ = true;
    } else if (toppedOut) {  // a KO: the server counts it, we start over on an empty board
        send(Dead{});
        game_->resetBoard();
        koFlashMs_ = kKoFlashMs;
    }
}

// The other players in START are named from the roster as it is now, so an id reused later keeps the old name.
void DuelSession::startMatch(const StartMatch& start) {
    game_.emplace(makeGame_(start));
    phase_ = DuelPhase::Countdown;
    outcome_ = DuelOutcome::None;
    mode_ = start.players.size() == 2 ? DuelMode::Duel : DuelMode::Royale;
    participants_ = static_cast<int>(start.players.size());
    startLevel_ = start.startLevel;
    countdownMs_ = kCountdownMs;
    sinceSnapshotMs_ = 0;
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
    game_.reset();
    lastSent_.reset();
    roomCode_.clear();
    myId_ = 0;
    hostId_ = 0;
    roster_.clear();
    mode_ = DuelMode::Duel;
    opponents_.clear();
    participants_ = 0;
    startLevel_ = 1;
    rooms_.clear();
    fromLobby_ = false;
    publicRoom_ = false;
    quick_ = false;
    error_.clear();
    countdownMs_ = 0;
    sinceSnapshotMs_ = 0;
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
    koFlashMs_ = 0;
    eliminated_ = false;
    myPlace_ = 0;
}

}  // namespace tetris
