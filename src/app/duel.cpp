#include "app/duel.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <variant>

namespace tetris {

Game makeDuelGame(const StartMatch& start) {
    return Game(start.seed, start.startLevel, Board{}, GameOptions{.allowPause = false});
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

}  // namespace

int frameBudgetMs(std::uint64_t elapsedMs, bool inMatch) {
    return static_cast<int>(std::min<std::uint64_t>(elapsedMs, inMatch ? kMaxCatchUpMs : kMaxFrameMs));
}

bool DuelSession::frozen() const { return phase_ == DuelPhase::Playing && (koFlashMs_ > 0 || matchClockMs_ == 0); }

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

void DuelSession::requestRematch() {
    if (phase_ != DuelPhase::Result || opponentGone_ || rematchRequested_) return;
    rematchRequested_ = true;
    send(Rematch{});
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
        roomCode_ = room->code;
    } else if (const auto* opp = std::get_if<OpponentName>(&*message)) {
        if (phase_ == DuelPhase::Waiting) opponentName_ = opp->name;
    } else if (const auto* start = std::get_if<StartMatch>(&*message)) {
        const bool rematch = phase_ == DuelPhase::Result && rematchRequested_ && !opponentGone_;
        if (phase_ == DuelPhase::Waiting || rematch) startMatch(*start);
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
        opponent_ = snapshot->snapshot;
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
    } else if (std::holds_alternative<OpponentLeft>(*message)) {
        if (phase_ == DuelPhase::Waiting) {
            reset();
            error_ = "Opponent left";
            return;
        }
        opponentGone_ = true;
        if (phase_ != DuelPhase::Result) finish(DuelOutcome::OpponentLeft);
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
    matchClockMs_ = std::max(0, matchClockMs_ - ms);
    koFlashMs_ = std::max(0, koFlashMs_ - ms);
    // The app stops ticking the game at 0:00, so a clear still animating would never be sent.
    if (matchClockMs_ == 0)
        while (game_->state() == GameState::Clearing) game_->tick(1);
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
    if (toppedOut) {  // a KO: the server counts it, we start over on an empty board
        send(Dead{});
        game_->resetBoard();
        koFlashMs_ = kKoFlashMs;
    }
}

void DuelSession::startMatch(const StartMatch& start) {
    game_.emplace(makeGame_(start));
    phase_ = DuelPhase::Countdown;
    outcome_ = DuelOutcome::None;
    countdownMs_ = kCountdownMs;
    sinceSnapshotMs_ = 0;
    opponent_ = Snapshot{};
    lastSent_.reset();
    rematchRequested_ = false;
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
    opponent_ = Snapshot{};
    lastSent_.reset();
    roomCode_.clear();
    opponentName_.clear();
    rooms_.clear();
    fromLobby_ = false;
    publicRoom_ = false;
    quick_ = false;
    error_.clear();
    countdownMs_ = 0;
    sinceSnapshotMs_ = 0;
    rematchRequested_ = false;
    opponentGone_ = false;
    resetMatchStats();
}

void DuelSession::resetMatchStats() {
    reason_ = DuelReason::KOs;
    kosMine_ = 0;
    kosTheirs_ = 0;
    sentMine_ = 0;
    sentTheirs_ = 0;
    matchClockMs_ = kMatchMs;
    koFlashMs_ = 0;
}

}  // namespace tetris
