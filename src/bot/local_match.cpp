#include "bot/local_match.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include "app/duel.hpp"

namespace tetris {

MatchOver verdictFor(int kosMine, int kosTheirs, int sentMine, int sentTheirs, MatchReason reason) {
    int mine = kosMine;
    int theirs = kosTheirs;
    if (mine == theirs) {
        mine = sentMine;
        theirs = sentTheirs;
    }
    const MatchResult result = mine > theirs ? MatchResult::Win : mine < theirs ? MatchResult::Lose : MatchResult::Draw;
    return MatchOver{result, reason, sentMine, sentTheirs};
}

LocalMatch::LocalMatch(BotLevel level, std::uint32_t seed) : level_(level), rng_(seed), player_(level, rng_()) {}

void LocalMatch::send(const std::string& text) {
    const std::optional<Message> message = decode(text);
    if (!message) return;
    std::visit(
        [this](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Hello>) {
                humanName_ = m.name;
            } else if constexpr (std::is_same_v<T, CreateRoom>) {
                startLevel_ = m.startLevel;
                if (!entered_) {
                    queue(RoomEntered{"BOTS", 1});
                    queueRoster(true);
                    entered_ = true;
                }
                start();
            } else if constexpr (std::is_same_v<T, Attack>) {
                if (!playing()) return;
                bot_->receiveGarbage(m.lines);
                sentHuman_ += m.lines;
            } else if constexpr (std::is_same_v<T, Dead>) {
                if (playing()) {
                    knockOut(false);
                    if (m.buried && playing()) finish(MatchReason::KOs);
                }
            } else if constexpr (std::is_same_v<T, Begin>) {
                if (!over_) return;
                start();
                queueRoster(true);  // the server's order: START, then PLAYERS with the in-match flags
            }
        },
        *message);
}

std::vector<std::string> LocalMatch::receive() { return std::exchange(outbox_, {}); }

void LocalMatch::start() {
    const StartMatch startMatch{static_cast<std::uint32_t>(rng_()), startLevel_, {1, 2}};
    bot_ = makeDuelGame(startMatch);
    player_ = BotPlayer(level_, static_cast<std::uint32_t>(rng_()));
    over_ = false;
    clockMs_ = 0;
    koFlashMs_ = 0;
    sinceSnapshotMs_ = 0;
    lastSent_.reset();
    kosHuman_ = kosBot_ = 0;
    sentHuman_ = sentBot_ = 0;
    queue(startMatch);
}

void LocalMatch::update(int ms) {
    for (int i = 0; i < ms && playing(); ++i) step();
}

void LocalMatch::step() {
    ++clockMs_;
    // The bot plays from the end of the countdown to 0:00; through the grace after it, it is frozen like the human.
    if (clockMs_ > kCountdownMs && clockMs_ <= kCountdownMs + kMatchMs) {
        if (koFlashMs_ > 0) {
            --koFlashMs_;
        } else {
            player_.update(*bot_, 1);
            bot_->tick(1);
            bot_->takeEvents();  // nobody listens; keep the list from growing
        }
        if (const int lines = bot_->takeOutgoingAttack(); lines > 0) {
            queue(Attack{lines});
            sentBot_ += lines;
        }
        const bool toppedOut = bot_->state() == GameState::GameOver;
        if (++sinceSnapshotMs_ >= kSnapshotIntervalMs || toppedOut) {
            sinceSnapshotMs_ = 0;
            const Snapshot snapshot = snapshotOf(*bot_);
            if (snapshot != lastSent_) {
                queue(PlayerSnapshot{2, snapshot});
                lastSent_ = snapshot;
            }
        }
        if (toppedOut) {
            const bool resumed = bot_->resumeAfterKo();
            player_.reset();
            if (resumed) koFlashMs_ = kKoFlashMs;
            knockOut(true);
            if (!resumed && playing()) finish(MatchReason::KOs);
            if (over_) return;
        }
    }
    if (clockMs_ >= kCountdownMs + kMatchMs + kLocalMatchGraceMs) finish(MatchReason::Time);
}

void LocalMatch::knockOut(bool humanScores) {
    (humanScores ? kosHuman_ : kosBot_) += 1;
    queue(KoCounts{kosHuman_, kosBot_});
    if (kosHuman_ >= kKosToWin || kosBot_ >= kKosToWin) finish(MatchReason::KOs);
}

void LocalMatch::finish(MatchReason reason) {
    queue(verdictFor(kosHuman_, kosBot_, sentHuman_, sentBot_, reason));
    queueRoster(false);  // like the server's PLAYERS after OVER: back between matches
    over_ = true;
}

void LocalMatch::queueRoster(bool inMatch) {
    queue(Roster{1, {{1, humanName_, inMatch}, {2, botName(level_), inMatch}}});
}

}  // namespace tetris
