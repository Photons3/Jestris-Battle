#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "bot/bot_player.hpp"
#include "core/game.hpp"
#include "net/connection.hpp"
#include "net/protocol.hpp"

namespace tetris {

inline constexpr int kLocalMatchGraceMs = 500;  // like defaultMatchGrace in server/hub.go

// The verdict for the human, as server/hub.go's verdict(): KOs, then lines sent, else a draw.
MatchOver verdictFor(int kosMine, int kosTheirs, int sentMine, int sentTheirs, MatchReason reason);

// An offline duel against the bot: plays the server's referee and the remote player, so a DuelSession can talk
// to it like any other connection. Messages it sends are answered from receive(); the bot plays in update().
class LocalMatch : public Connection {
public:
    LocalMatch(BotLevel level, std::uint32_t seed);

    void send(const std::string& text) override;
    std::vector<std::string> receive() override;
    bool closed() const override { return false; }
    void update(int ms) override;

    const Game* botGame() const { return bot_ ? &*bot_ : nullptr; }  // null before the first START

private:
    bool playing() const { return bot_.has_value() && !over_; }
    void start();
    void step();
    void knockOut(bool humanScores);
    void finish(MatchReason reason);
    void queue(const Message& message) { outbox_.push_back(encode(message)); }

    BotLevel level_;
    std::mt19937 rng_;
    BotPlayer player_;
    std::optional<Game> bot_;
    int startLevel_ = 1;
    bool named_ = false;  // OpponentName goes out before the first START only
    bool over_ = false;
    int clockMs_ = 0;
    int koFlashMs_ = 0;
    int sinceSnapshotMs_ = 0;
    std::optional<Snapshot> lastSent_;
    int kosHuman_ = 0;  // KOs the human scored: the bot topped out
    int kosBot_ = 0;
    int sentHuman_ = 0;
    int sentBot_ = 0;
    std::vector<std::string> outbox_;
};

}  // namespace tetris
