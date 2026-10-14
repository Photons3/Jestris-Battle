#include "net/protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <type_traits>
#include <vector>

namespace tetris {
namespace {

constexpr std::string_view kCellChars = "IOTSZJLGB";  // index matches PieceType
constexpr std::string_view kPieceChars = "IOTSZJL";      // the dealable prefix of kCellChars
constexpr std::string_view kLockedHoldChars = "iotszjl";  // a held piece while hold is locked
constexpr std::string_view kGhostChars = "iotszjl";       // a ghost cell in the field; garbage and bombs never fall
constexpr char kEmptyCell = '.';
constexpr int kMaxInt = std::numeric_limits<int>::max();
constexpr std::array<std::string_view, 3> kResultTokens{"WIN", "LOSE", "DRAW"};  // index matches MatchResult
constexpr std::array<std::string_view, 2> kReasonTokens{"KO", "TIME"};          // index matches MatchReason

template <typename E, std::size_t N>
std::optional<E> parseToken(std::string_view text, const std::array<std::string_view, N>& tokens) {
    for (std::size_t i = 0; i < N; ++i)
        if (tokens[i] == text) return static_cast<E>(i);
    return std::nullopt;
}

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(' ', start), text.size());
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

template <typename T>
std::optional<T> parseNumber(std::string_view text, T lo, T hi) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < lo || value > hi) return std::nullopt;
    return value;
}

char cellChar(Cell cell) { return cell ? kCellChars[static_cast<int>(*cell)] : kEmptyCell; }

bool parseCell(char c, Cell& out) {
    if (c == kEmptyCell) {
        out = std::nullopt;
        return true;
    }
    const std::size_t i = kCellChars.find(c);
    if (i == std::string_view::npos) return false;
    out = static_cast<PieceType>(i);
    return true;
}

char fieldChar(Cell cell, Cell ghost) {
    if (cell || !ghost) return cellChar(cell);
    return kGhostChars[static_cast<int>(*ghost)];
}

// A field cell is a block, a ghost cell or empty.
bool parseFieldCell(char c, Cell& cell, Cell& ghost) {
    ghost = std::nullopt;
    if (parseCell(c, cell)) return true;
    const std::size_t i = kGhostChars.find(c);
    if (i == std::string_view::npos) return false;
    ghost = static_cast<PieceType>(i);
    return true;
}

char holdChar(Cell hold, bool locked) {
    if (!hold) return kEmptyCell;
    return (locked ? kLockedHoldChars : kPieceChars)[static_cast<int>(*hold)];
}

// A dealable piece or '.', never garbage or a bomb.
bool parsePiece(char c, Cell& out) {
    if (c == kEmptyCell) {
        out = std::nullopt;
        return true;
    }
    const std::size_t i = kPieceChars.find(c);
    if (i == std::string_view::npos) return false;
    out = static_cast<PieceType>(i);
    return true;
}

bool parseHold(std::string_view text, Cell& hold, bool& locked) {
    if (text.size() != 1) return false;
    const std::size_t i = kLockedHoldChars.find(text[0]);
    locked = i != std::string_view::npos;
    if (locked) {
        hold = static_cast<PieceType>(i);
        return true;
    }
    return parsePiece(text[0], hold);
}

std::optional<Message> decodeSnapshot(const std::vector<std::string_view>& parts) {
    const auto score = parseNumber(parts[1], 0, kMaxInt);
    const auto lines = parseNumber(parts[2], 0, kMaxInt);
    const auto pending = parseNumber(parts[3], 0, kMaxInt);
    const std::string_view next = parts[12];
    const std::string_view cells = parts[13];
    if (!score || !lines || !pending || static_cast<int>(next.size()) != kSnapshotNext ||
        static_cast<int>(cells.size()) != kSnapshotCells)
        return std::nullopt;
    Snapshot snapshot{.score = *score, .lines = *lines, .pending = *pending};
    int* const stats[] = {&snapshot.stats.pieces,   &snapshot.stats.attack, &snapshot.stats.garbageCleared,
                          &snapshot.stats.cancelled, &snapshot.stats.tetrises, &snapshot.stats.tSpins,
                          &snapshot.stats.maxCombo};
    int part = 4;  // the stats follow pending, in GameStats order
    for (int* stat : stats) {
        const auto value = parseNumber(parts[part++], 0, kMaxInt);
        if (!value) return std::nullopt;
        *stat = *value;
    }
    if (!parseHold(parts[11], snapshot.hold, snapshot.holdLocked)) return std::nullopt;
    for (int i = 0; i < kSnapshotNext; ++i)
        if (!parsePiece(next[i], snapshot.next[i])) return std::nullopt;
    for (int i = 0; i < kSnapshotCells; ++i)
        if (!parseFieldCell(cells[i], snapshot.cells[i], snapshot.ghost[i])) return std::nullopt;
    return snapshot;
}

// "CODE:lvl:host"; nullopt unless all three parts are valid.
std::optional<RoomInfo> decodeRoomInfo(std::string_view entry) {
    const std::size_t first = entry.find(':');
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t second = entry.find(':', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    const std::string_view code = entry.substr(0, first);
    const auto level = parseNumber(entry.substr(first + 1, second - first - 1), kMinStartLevel, kMaxStartLevel);
    const std::string_view host = entry.substr(second + 1);
    if (!isRoomCode(code) || !level || !isValidName(host)) return std::nullopt;  // a third ':' fails isValidName
    return RoomInfo{std::string(code), *level, std::string(host)};
}

}  // namespace

bool isRoomCode(std::string_view text) {
    return static_cast<int>(text.size()) == kRoomCodeLength &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

bool isNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

bool isValidName(std::string_view text) {
    return !text.empty() && static_cast<int>(text.size()) <= kMaxNameLength &&
           std::all_of(text.begin(), text.end(), isNameChar);
}

Snapshot snapshotOf(const Game& game) {
    Snapshot snapshot{.score = game.score(), .lines = game.lines(), .pending = game.pendingGarbage()};
    snapshot.stats = game.stats();
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x) snapshot.cells[y * kBoardWidth + x] = game.board().at(x, y);
    if (game.state() == GameState::Playing) {
        for (Point c : game.active().cells())
            if (c.y < kVisibleHeight) snapshot.cells[c.y * kBoardWidth + c.x] = game.active().type;
        Piece ghost = game.active();
        ghost.pos.y -= game.ghostDistance();
        for (Point c : ghost.cells()) {
            const int i = c.y * kBoardWidth + c.x;
            if (c.y < kVisibleHeight && !snapshot.cells[i]) snapshot.ghost[i] = ghost.type;
        }
    }
    snapshot.hold = game.hold();
    snapshot.holdLocked = !game.canHold();
    const std::vector<PieceType> next = game.next(kSnapshotNext);
    std::copy_n(next.begin(), std::min<std::size_t>(next.size(), kSnapshotNext), snapshot.next.begin());
    return snapshot;
}

std::string encode(const Message& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, CreateRoom>) {
                return "CREATE " + std::to_string(m.startLevel) + (m.isPublic ? " PUB" : "");
            } else if constexpr (std::is_same_v<T, JoinRoom>) {
                return "JOIN " + m.code;
            } else if constexpr (std::is_same_v<T, RoomCreated>) {
                return "ROOM " + m.code;
            } else if constexpr (std::is_same_v<T, StartMatch>) {
                return "START " + std::to_string(m.seed) + " " + std::to_string(m.startLevel);
            } else if constexpr (std::is_same_v<T, Attack>) {
                return "ATK " + std::to_string(m.lines);
            } else if constexpr (std::is_same_v<T, Snapshot>) {
                std::string text = "SNAP " + std::to_string(m.score) + " " + std::to_string(m.lines) + " " +
                                   std::to_string(m.pending) + " ";
                for (int stat : {m.stats.pieces, m.stats.attack, m.stats.garbageCleared, m.stats.cancelled,
                                 m.stats.tetrises, m.stats.tSpins, m.stats.maxCombo})
                    text += std::to_string(stat) + " ";
                text += holdChar(m.hold, m.holdLocked);
                text += ' ';
                for (Cell piece : m.next) text += cellChar(piece);
                text += ' ';
                for (int i = 0; i < kSnapshotCells; ++i) text += fieldChar(m.cells[i], m.ghost[i]);
                return text;
            } else if constexpr (std::is_same_v<T, Dead>) {
                return "DEAD";
            } else if constexpr (std::is_same_v<T, Rematch>) {
                return "REMATCH";
            } else if constexpr (std::is_same_v<T, OpponentLeft>) {
                return "LEFT";
            } else if constexpr (std::is_same_v<T, ServerError>) {
                return "ERR " + m.reason;
            } else if constexpr (std::is_same_v<T, KoCounts>) {
                return "KO " + std::to_string(m.mine) + " " + std::to_string(m.theirs);
            } else if constexpr (std::is_same_v<T, Hello>) {
                return "HELLO " + m.name;
            } else if constexpr (std::is_same_v<T, OpponentName>) {
                return "OPP " + m.name;
            } else if constexpr (std::is_same_v<T, ListRooms>) {
                return "LIST";
            } else if constexpr (std::is_same_v<T, UnlistRooms>) {
                return "UNLIST";
            } else if constexpr (std::is_same_v<T, QuickMatch>) {
                return "QUICK " + std::to_string(m.startLevel);
            } else if constexpr (std::is_same_v<T, RoomList>) {
                std::string text = "ROOMS";
                for (const RoomInfo& room : m.rooms)
                    text += " " + room.code + ":" + std::to_string(room.startLevel) + ":" + room.host;
                return text;
            } else {
                static_assert(std::is_same_v<T, MatchOver>);
                return "OVER " + std::string(kResultTokens[static_cast<std::size_t>(m.result)]) + " " +
                       std::string(kReasonTokens[static_cast<std::size_t>(m.reason)]) + " " +
                       std::to_string(m.mySent) + " " + std::to_string(m.theirSent);
            }
        },
        message);
}

std::optional<Message> decode(std::string_view text) {
    if (text.starts_with("ERR ") && text.size() > 4) return ServerError{std::string(text.substr(4))};
    const std::vector<std::string_view> parts = split(text);
    const std::string_view verb = parts[0];
    const std::size_t args = parts.size() - 1;
    if (verb == "CREATE" && (args == 1 || args == 2)) {
        const auto level = parseNumber(parts[1], kMinStartLevel, kMaxStartLevel);
        if (!level) return std::nullopt;
        if (args == 1 || parts[2] == "PRIV") return CreateRoom{*level, false};
        if (parts[2] == "PUB") return CreateRoom{*level, true};
    } else if (verb == "QUICK" && args == 1) {
        if (const auto level = parseNumber(parts[1], kMinStartLevel, kMaxStartLevel)) return QuickMatch{*level};
    } else if (verb == "ROOMS" && args <= static_cast<std::size_t>(kMaxListedRooms)) {
        RoomList list;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const auto room = decodeRoomInfo(parts[i]);
            if (!room) return std::nullopt;
            list.rooms.push_back(*room);
        }
        return list;
    } else if (verb == "JOIN" && args == 1) {
        if (isRoomCode(parts[1])) return JoinRoom{std::string(parts[1])};
    } else if (verb == "ROOM" && args == 1) {
        if (isRoomCode(parts[1])) return RoomCreated{std::string(parts[1])};
    } else if (verb == "START" && args == 2) {
        const auto seed = parseNumber<std::uint32_t>(parts[1], 0, std::numeric_limits<std::uint32_t>::max());
        const auto level = parseNumber(parts[2], kMinStartLevel, kMaxStartLevel);
        if (seed && level) return StartMatch{*seed, *level};
    } else if (verb == "ATK" && args == 1) {
        if (const auto lines = parseNumber(parts[1], 1, kBoardHeight)) return Attack{*lines};
    } else if (verb == "SNAP" && args == 13) {
        return decodeSnapshot(parts);
    } else if (verb == "KO" && args == 2) {
        const auto mine = parseNumber(parts[1], 0, kKosToWin);
        const auto theirs = parseNumber(parts[2], 0, kKosToWin);
        if (mine && theirs) return KoCounts{*mine, *theirs};
    } else if (verb == "OVER" && args == 4) {
        const auto result = parseToken<MatchResult>(parts[1], kResultTokens);
        const auto reason = parseToken<MatchReason>(parts[2], kReasonTokens);
        const auto mySent = parseNumber(parts[3], 0, kMaxInt);
        const auto theirSent = parseNumber(parts[4], 0, kMaxInt);
        if (result && reason && mySent && theirSent) return MatchOver{*result, *reason, *mySent, *theirSent};
    } else if (verb == "HELLO" && args == 1) {
        if (isValidName(parts[1])) return Hello{std::string(parts[1])};
    } else if (verb == "OPP" && args == 1) {
        if (isValidName(parts[1])) return OpponentName{std::string(parts[1])};
    } else if (args == 0) {
        if (verb == "DEAD") return Dead{};
        if (verb == "REMATCH") return Rematch{};
        if (verb == "LEFT") return OpponentLeft{};
        if (verb == "LIST") return ListRooms{};
        if (verb == "UNLIST") return UnlistRooms{};
    }
    return std::nullopt;
}

}  // namespace tetris
