#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/game.hpp"

namespace tetris {

inline constexpr int kSnapshotCells = kBoardWidth * kVisibleHeight;
inline constexpr int kSnapshotNext = 5;  // the opponent's next queue, as long as your own
inline constexpr int kRoomCodeLength = 4;
inline constexpr int kKosToWin = 3;  // the first player to this many KOs wins the match
inline constexpr int kMaxNameLength = 12;  // maxNameLength in server/hub.go
inline constexpr int kMaxListedRooms = 20;  // maxListedRooms in server/hub.go
inline constexpr int kMaxRoomPlayers = 16;  // maxRoomPlayers in server/hub.go

// Four letters A-Z.
bool isRoomCode(std::string_view text);

// A display name is 1 to kMaxNameLength of A-Z a-z 0-9 _ - . (no spaces or ':', which the protocol uses).
bool isNameChar(char c);
bool isValidName(std::string_view text);

// What the opponent sees of a board: the visible field with the falling piece merged in, its ghost, the hold
// and the next queue.
struct Snapshot {
    int score = 0;
    int lines = 0;
    int pending = 0;                           // garbage waiting to rise
    int ready = 0;                             // the part of pending that can rise now
    std::array<Cell, kSnapshotCells> cells{};  // index y * kBoardWidth + x; y = 0 is the bottom row
    std::array<Cell, kSnapshotCells> ghost{};  // same indexing; set only where `cells` is empty
    Cell hold{};                               // nullopt: nothing held
    bool holdLocked = false;                   // hold already used this turn; drawn dimmed
    std::array<Cell, kSnapshotNext> next{};    // soonest first; nullopt only before the first SNAP arrives
    GameStats stats{};                         // match totals, for the scoreboard and the result table
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshotOf(const Game& game);

// Messages between a browser and the duel server. Attack and Snapshot are relayed to the opponent; Dead goes
// to the server, which answers with KoCounts and, when the match ends, MatchOver.
struct CreateRoom {
    int startLevel = 1;
    bool isPublic = false;  // listed in the lobby; CREATE <lvl> PUB
    friend bool operator==(const CreateRoom&, const CreateRoom&) = default;
};
struct JoinRoom {
    std::string code;
    friend bool operator==(const JoinRoom&, const JoinRoom&) = default;
};
struct RoomEntered {  // ROOM <CODE> <id>: you are in the room, as this player
    std::string code;
    int playerId = 1;
    friend bool operator==(const RoomEntered&, const RoomEntered&) = default;
};
struct StartMatch {  // START seed lvl id...: the players of the match, 2 to kMaxRoomPlayers unique ids
    std::uint32_t seed = 0;
    int startLevel = 1;
    std::vector<int> players;
    friend bool operator==(const StartMatch&, const StartMatch&) = default;
};
struct PlayerSnapshot {  // from the server: SNAP <id> followed by that player's Snapshot arguments
    int fromId = 0;
    Snapshot snapshot;
    friend bool operator==(const PlayerSnapshot&, const PlayerSnapshot&) = default;
};
struct RosterEntry {
    int id = 0;
    std::string name;
    bool inMatch = false;
    friend bool operator==(const RosterEntry&, const RosterEntry&) = default;
};
struct Roster {  // PLAYERS <hostId> id:name:0|1 ...: everyone in the room, 1 to kMaxRoomPlayers
    int hostId = 0;
    std::vector<RosterEntry> players;
    friend bool operator==(const Roster&, const Roster&) = default;
};
struct Begin {  // the host starts the match
    friend bool operator==(const Begin&, const Begin&) = default;
};
struct PlayerOut {  // OUT <id> <place>: the player was eliminated, finishing in this place
    int id = 0;
    int place = 0;
    friend bool operator==(const PlayerOut&, const PlayerOut&) = default;
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
struct Hello {  // the sender's display name; the first message on every connection
    std::string name;
    friend bool operator==(const Hello&, const Hello&) = default;
};
struct OpponentName {  // from the server, just before a room's first START
    std::string name;
    friend bool operator==(const OpponentName&, const OpponentName&) = default;
};

struct ListRooms {  // start watching the lobby
    friend bool operator==(const ListRooms&, const ListRooms&) = default;
};
struct UnlistRooms {  // stop watching the lobby
    friend bool operator==(const UnlistRooms&, const UnlistRooms&) = default;
};
struct QuickMatch {  // join the oldest open public room, or open one at this level
    int startLevel = 1;
    friend bool operator==(const QuickMatch&, const QuickMatch&) = default;
};
struct RoomInfo {  // one open public room in the lobby: CODE:lvl:host:n
    std::string code;
    int startLevel = 1;
    std::string host;
    int players = 1;  // 1 to kMaxRoomPlayers - 1; a full room is not listed
    friend bool operator==(const RoomInfo&, const RoomInfo&) = default;
};
struct RoomList {  // the open public rooms, oldest first, at most kMaxListedRooms
    std::vector<RoomInfo> rooms;
    friend bool operator==(const RoomList&, const RoomList&) = default;
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

using Message = std::variant<CreateRoom, JoinRoom, RoomEntered, StartMatch, Attack, Snapshot, Dead, Rematch,
                             OpponentLeft, ServerError, KoCounts, MatchOver, Hello, OpponentName, ListRooms,
                             UnlistRooms, QuickMatch, RoomList, PlayerSnapshot, Roster, Begin, PlayerOut>;

std::string encode(const Message& message);
// nullopt for anything malformed or unknown.
std::optional<Message> decode(std::string_view text);

}  // namespace tetris
