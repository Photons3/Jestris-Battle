#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "core/game.hpp"

namespace tetris {

inline constexpr int kSnapshotCells = kBoardWidth * kVisibleHeight;
inline constexpr int kRoomCodeLength = 4;
inline constexpr int kKosToWin = 3;  // the first player to this many KOs wins the match

// What the opponent sees of a board: the visible field with the falling piece merged in.
struct Snapshot {
    int score = 0;
    int lines = 0;
    int pending = 0;                           // garbage waiting to rise
    std::array<Cell, kSnapshotCells> cells{};  // index y * kBoardWidth + x; y = 0 is the bottom row
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshotOf(const Game& game);

// Messages between a browser and the duel server. Attack and Snapshot are relayed to the opponent; Dead goes
// to the server, which answers with KoCounts and, when the match ends, MatchOver.
struct CreateRoom {
    int startLevel = 1;
    friend bool operator==(const CreateRoom&, const CreateRoom&) = default;
};
struct JoinRoom {
    std::string code;
    friend bool operator==(const JoinRoom&, const JoinRoom&) = default;
};
struct RoomCreated {
    std::string code;
    friend bool operator==(const RoomCreated&, const RoomCreated&) = default;
};
struct StartMatch {
    std::uint32_t seed = 0;
    int startLevel = 1;
    friend bool operator==(const StartMatch&, const StartMatch&) = default;
};
struct Attack {
    int lines = 0;
    friend bool operator==(const Attack&, const Attack&) = default;
};
struct Dead {
    friend bool operator==(const Dead&, const Dead&) = default;
};
struct Rematch {
    friend bool operator==(const Rematch&, const Rematch&) = default;
};
struct OpponentLeft {
    friend bool operator==(const OpponentLeft&, const OpponentLeft&) = default;
};
struct ServerError {
    std::string reason;
    friend bool operator==(const ServerError&, const ServerError&) = default;
};

enum class MatchResult { Win, Lose, Draw };
enum class MatchReason { KOs, Time };
struct KoCounts {  // from the receiver's point of view
    int mine = 0;
    int theirs = 0;
    friend bool operator==(const KoCounts&, const KoCounts&) = default;
};
struct MatchOver {  // the server's verdict, from the receiver's point of view
    MatchResult result = MatchResult::Draw;
    MatchReason reason = MatchReason::Time;
    int mySent = 0;
    int theirSent = 0;
    friend bool operator==(const MatchOver&, const MatchOver&) = default;
};

using Message = std::variant<CreateRoom, JoinRoom, RoomCreated, StartMatch, Attack, Snapshot, Dead, Rematch,
                             OpponentLeft, ServerError, KoCounts, MatchOver>;

std::string encode(const Message& message);
// nullopt for anything malformed or unknown.
std::optional<Message> decode(std::string_view text);

}  // namespace tetris
