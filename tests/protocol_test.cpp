#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("protocol: every message survives an encode/decode round trip") {
    Snapshot snapshot{.score = 1200, .lines = 7, .pending = 3};
    snapshot.cells[0] = PieceType::Garbage;
    snapshot.cells[kSnapshotCells - 1] = PieceType::I;
    snapshot.cells[1] = PieceType::Bomb;
    snapshot.ghost[2] = PieceType::T;
    snapshot.hold = PieceType::T;
    snapshot.holdLocked = true;
    snapshot.next = {PieceType::I, PieceType::O, PieceType::S, PieceType::Z, PieceType::L};
    snapshot.stats = {.pieces = 40, .attack = 12, .garbageCleared = 5, .cancelled = 3, .tetrises = 2, .tSpins = 1,
                      .maxCombo = 4};
    Snapshot unlockedHold{};
    unlockedHold.hold = PieceType::J;
    unlockedHold.next = snapshot.next;
    const std::vector<Message> messages{CreateRoom{3},
                                        JoinRoom{"ABCD"},
                                        RoomCreated{"WXYZ"},
                                        StartMatch{4000000000u, 15},
                                        Attack{4},
                                        snapshot,
                                        unlockedHold,
                                        Snapshot{},
                                        Dead{},
                                        Rematch{},
                                        OpponentLeft{},
                                        ServerError{"room is full"},
                                        KoCounts{1, 0},
                                        KoCounts{3, 2},
                                        MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7},
                                        MatchOver{MatchResult::Draw, MatchReason::Time, 0, 0},
                                        Hello{"Sam"},
                                        OpponentName{"x_y-z.9"},
                                        CreateRoom{3, true},
                                        ListRooms{},
                                        UnlistRooms{},
                                        QuickMatch{9},
                                        RoomList{},
                                        RoomList{{{"ABCD", 3, "Sam"}, {"WXYZ", 15, "Player0001"}}}};
    for (const Message& message : messages) {
        CAPTURE(encode(message));
        const auto decoded = decode(encode(message));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == message);
    }
}

TEST_CASE("protocol: the wire format is plain text") {
    CHECK(encode(Attack{2}) == "ATK 2");
    CHECK(encode(KoCounts{1, 0}) == "KO 1 0");
    CHECK(encode(MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7}) == "OVER WIN KO 12 7");
    CHECK(encode(MatchOver{MatchResult::Lose, MatchReason::Time, 3, 9}) == "OVER LOSE TIME 3 9");
    CHECK(encode(MatchOver{MatchResult::Draw, MatchReason::Time, 4, 4}) == "OVER DRAW TIME 4 4");
    CHECK(encode(StartMatch{42, 1}) == "START 42 1");
    const std::string empty(kSnapshotCells, '.');
    CHECK(encode(Snapshot{}) == "SNAP 0 0 0 0 0 0 0 0 0 0 . ..... " + empty);
    Snapshot bomb{};
    bomb.cells[0] = PieceType::Bomb;
    CHECK(encode(bomb) == "SNAP 0 0 0 0 0 0 0 0 0 0 . ..... B" + std::string(kSnapshotCells - 1, '.'));
    Snapshot ghost{};
    ghost.ghost[0] = PieceType::T;  // ghost cells are the lowercase piece letter
    CHECK(encode(ghost) == "SNAP 0 0 0 0 0 0 0 0 0 0 . ..... t" + std::string(kSnapshotCells - 1, '.'));
    Snapshot held{};
    held.hold = PieceType::T;
    held.holdLocked = true;
    held.next = {PieceType::I, PieceType::O, PieceType::S, PieceType::Z, PieceType::L};
    CHECK(encode(held) == "SNAP 0 0 0 0 0 0 0 0 0 0 t IOSZL " + empty);
    held.holdLocked = false;
    CHECK(encode(held) == "SNAP 0 0 0 0 0 0 0 0 0 0 T IOSZL " + empty);
    Snapshot counted{};
    counted.stats = {40, 12, 5, 3, 2, 1, 4};
    CHECK(encode(counted) == "SNAP 0 0 0 40 12 5 3 2 1 4 . ..... " + empty);
}

TEST_CASE("protocol: malformed messages are rejected") {
    for (std::string_view text : {"", "HELLO", "DEAD ", "DEAD now", "ATK", "ATK 0", "ATK -1", "ATK x", "ATK 41",
                                  "JOIN abcd", "JOIN ABC", "JOIN ABCDE", "CREATE 0", "CREATE 16", "START 1",
                                  "START -1 1", "SNAP 0 0 0 0 0 0 0 0 0 0 . ..... ...", "ERR", "ERR "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string badCell = "SNAP 0 0 0 0 0 0 0 0 0 0 . ..... " + std::string(kSnapshotCells, '.');
    badCell.back() = 'Q';
    CHECK_FALSE(decode(badCell).has_value());
    for (char ghostOfBlock : {'g', 'b'}) {  // garbage and bombs never fall, so they have no ghost
        badCell.back() = ghostOfBlock;
        CHECK_FALSE(decode(badCell).has_value());
    }
}

TEST_CASE("protocol: malformed snapshot hold and next fields are rejected") {
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 0 0 0 0 0 0 0 0 t IOSZL " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 " + cells).has_value());  // the old form, without hold and next
    for (std::string_view hold : {"G", "B", "g", "b", "x", "TT", ""}) {
        CAPTURE(hold);
        CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 " + std::string(hold) + " IOSZL " + cells).has_value());
    }
    for (std::string_view next : {"IOSZ", "IOSZLJ", "IOSZG", "IOSZB", "iosz l", "ioszl"}) {
        CAPTURE(next);
        CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 . " + std::string(next) + " " + cells).has_value());
    }
}

TEST_CASE("protocol: malformed snapshot stats are rejected") {
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 . ..... " + cells).has_value());          // the 6-argument form
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());  // one stat short
    CHECK_FALSE(decode("SNAP 0 0 0 0 -1 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 x 0 0 0 . ..... " + cells).has_value());
}

TEST_CASE("protocol: malformed KO and OVER messages are rejected") {
    for (std::string_view text : {"KO", "KO 1", "KO 1 0 0", "KO 4 0", "KO 0 4", "KO -1 0", "KO x 0", "KO 1 0 ",
                                  "OVER", "OVER WIN KO 1", "OVER WIN KO 1 2 3", "OVER WON KO 1 2",
                                  "OVER WIN KOS 1 2", "OVER win KO 1 2", "OVER WIN KO -1 2", "OVER WIN KO 1 x",
                                  "OVER WIN TIME 1 2 "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}

TEST_CASE("protocol: a snapshot shows the visible field with the falling piece merged in") {
    Game game(seedWithFirst(PieceType::T), 1);
    for (Cell cell : snapshotOf(game).cells) CHECK_FALSE(cell);  // the spawned piece is above the visible field
    game.receiveGarbage(3);
    CHECK(snapshotOf(game).pending == 3);
    dropToFloor(game);
    int filled = 0;
    for (Cell cell : snapshotOf(game).cells) {
        if (!cell) continue;
        ++filled;
        CHECK(*cell == PieceType::T);
    }
    CHECK(filled == 4);
    for (Cell cell : snapshotOf(game).ghost) CHECK_FALSE(cell);  // a resting piece hides its own ghost
}

TEST_CASE("protocol: a snapshot carries the falling piece's ghost in the empty cells below it") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.setSoftDrop(true);
    while (game.ghostDistance() > kVisibleHeight / 2) game.tick(1);  // the piece is now in the visible field
    game.setSoftDrop(false);
    const Snapshot snapshot = snapshotOf(game);
    Piece ghost = game.active();
    ghost.pos.y -= game.ghostDistance();
    int ghostCells = 0;
    for (int i = 0; i < kSnapshotCells; ++i) {
        if (!snapshot.ghost[i]) continue;
        ++ghostCells;
        CHECK(*snapshot.ghost[i] == PieceType::T);
        CHECK_FALSE(snapshot.cells[i]);
    }
    CHECK(ghostCells == 4);
    for (Point c : ghost.cells()) CHECK(snapshot.ghost[c.y * kBoardWidth + c.x] == PieceType::T);
}

TEST_CASE("protocol: a snapshot carries the hold and the next queue") {
    Game game(seedWithFirst(PieceType::T), 1);
    const auto nextOf = [](const Game& g) {
        std::array<Cell, kSnapshotNext> next{};
        const std::vector<PieceType> pieces = g.next(kSnapshotNext);
        std::copy(pieces.begin(), pieces.end(), next.begin());
        return next;
    };
    Snapshot snapshot = snapshotOf(game);
    CHECK_FALSE(snapshot.hold);
    CHECK_FALSE(snapshot.holdLocked);
    CHECK(snapshot.next == nextOf(game));
    game.apply(Action::Hold);
    snapshot = snapshotOf(game);
    CHECK(snapshot.hold == PieceType::T);
    CHECK(snapshot.holdLocked);
    CHECK(snapshot.next == nextOf(game));
    game.resetBoard();
    snapshot = snapshotOf(game);
    CHECK_FALSE(snapshot.hold);
    CHECK_FALSE(snapshot.holdLocked);
}

TEST_CASE("protocol: name rules") {
    CHECK(isValidName("A"));
    CHECK(isValidName("Player4821"));
    CHECK(isValidName("abcdefghijkl"));  // 12
    CHECK(isValidName("a_b-c.9"));
    CHECK_FALSE(isValidName(""));
    CHECK_FALSE(isValidName("abcdefghijklm"));  // 13
    for (std::string_view bad : {"a b", "a:b", "a=b", "a\nb", "\xC3\xA9", "a/b", "a+b"}) {
        CAPTURE(bad);
        CHECK_FALSE(isValidName(bad));
    }
}

TEST_CASE("protocol: HELLO and OPP on the wire") {
    CHECK(encode(Hello{"Sam"}) == "HELLO Sam");
    CHECK(encode(OpponentName{"Kai"}) == "OPP Kai");
    for (std::string_view text : {"HELLO", "HELLO ", "HELLO a b", "HELLO a:b", "HELLO abcdefghijklm", "OPP",
                                  "OPP ", "OPP a b", "OPP a=b"}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}

TEST_CASE("protocol: lobby messages on the wire") {
    CHECK(encode(CreateRoom{3}) == "CREATE 3");  // private stays the old format
    CHECK(encode(CreateRoom{3, true}) == "CREATE 3 PUB");
    CHECK(decode("CREATE 3 PRIV") == Message{CreateRoom{3, false}});
    CHECK(encode(ListRooms{}) == "LIST");
    CHECK(encode(UnlistRooms{}) == "UNLIST");
    CHECK(encode(QuickMatch{4}) == "QUICK 4");
    CHECK(encode(RoomList{}) == "ROOMS");
    CHECK(encode(RoomList{{{"ABCD", 3, "Sam"}, {"WXYZ", 12, "Kai"}}}) == "ROOMS ABCD:3:Sam WXYZ:12:Kai");
}

TEST_CASE("protocol: malformed lobby messages are rejected") {
    for (std::string_view text : {"CREATE 3 OPEN", "CREATE 3 pub", "CREATE 3 PUB x", "LIST x", "UNLIST x", "QUICK",
                                  "QUICK 0", "QUICK 16", "QUICK x", "ROOMS ", "ROOMS ABCD", "ROOMS ABCD:3",
                                  "ROOMS ABCD:3:", "ROOMS abcd:3:Sam", "ROOMS ABCD:0:Sam", "ROOMS ABCD:3:Sam:x",
                                  "ROOMS ABCD:3:a b", "ROOMS ABCD:x:Sam", "ROOMS ABCD:3:Sam "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string tooMany = "ROOMS";
    for (int i = 0; i <= kMaxListedRooms; ++i) tooMany += " ABCD:1:Sam";
    CHECK_FALSE(decode(tooMany).has_value());
}
