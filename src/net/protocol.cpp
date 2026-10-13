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

bool isRoomCode(std::string_view text) {
    return static_cast<int>(text.size()) == kRoomCodeLength &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
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

std::optional<Message> decodeSnapshot(const std::vector<std::string_view>& parts) {
    const auto score = parseNumber(parts[1], 0, kMaxInt);
    const auto lines = parseNumber(parts[2], 0, kMaxInt);
    const auto pending = parseNumber(parts[3], 0, kMaxInt);
    const std::string_view cells = parts[4];
    if (!score || !lines || !pending || static_cast<int>(cells.size()) != kSnapshotCells) return std::nullopt;
    Snapshot snapshot{*score, *lines, *pending, {}};
    for (int i = 0; i < kSnapshotCells; ++i)
        if (!parseCell(cells[i], snapshot.cells[i])) return std::nullopt;
    return snapshot;
}

}  // namespace

Snapshot snapshotOf(const Game& game) {
    Snapshot snapshot{game.score(), game.lines(), game.pendingGarbage(), {}};
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x) snapshot.cells[y * kBoardWidth + x] = game.board().at(x, y);
    if (game.state() == GameState::Playing)
        for (Point c : game.active().cells())
            if (c.y < kVisibleHeight) snapshot.cells[c.y * kBoardWidth + c.x] = game.active().type;
    return snapshot;
}

std::string encode(const Message& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, CreateRoom>) {
                return "CREATE " + std::to_string(m.startLevel);
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
                for (Cell cell : m.cells) text += cellChar(cell);
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
    if (verb == "CREATE" && args == 1) {
        if (const auto level = parseNumber(parts[1], kMinStartLevel, kMaxStartLevel)) return CreateRoom{*level};
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
    } else if (verb == "SNAP" && args == 4) {
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
    } else if (args == 0) {
        if (verb == "DEAD") return Dead{};
        if (verb == "REMATCH") return Rematch{};
        if (verb == "LEFT") return OpponentLeft{};
    }
    return std::nullopt;
}

}  // namespace tetris
