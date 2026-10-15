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
    Snapshot snapshot{.score = 1200, .lines = 7, .pending = 3, .ready = 2};
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
                                        RoomEntered{"WXYZ", 16},
                                        StartMatch{4000000000u, 15, {3, 1}},
                                        Attack{4},
                                        snapshot,
                                        unlockedHold,
                                        Snapshot{},
                                        Dead{},
                                        OpponentLeft{},
                                        ServerError{"room is full"},
                                        KoCounts{1, 0},
                                        KoCounts{3, 2},
                                        MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7},
                                        MatchOver{MatchResult::Draw, MatchReason::Time, 0, 0},
                                        Hello{"Sam"},
                                        CreateRoom{3, true},
                                        ListRooms{},
                                        UnlistRooms{},
                                        QuickMatch{9},
                                        RoomList{},
                                        RoomList{{{"ABCD", 3, "Sam", 5}, {"WXYZ", 15, "Player0001", 15}}},
                                        RoomEntered{"ABCD", 3},
                                        StartMatch{7, 2, {1, 2}},
                                        StartMatch{7, 2, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},
                                        PlayerSnapshot{5, snapshot},
                                        Roster{2, {{1, "Sam", true}, {2, "Sam2", false}}},
                                        Begin{},
                                        PlayerOut{4, 9}};
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
    CHECK(encode(StartMatch{42, 1, {1, 2}}) == "START 42 1 1 2");
    const std::string empty(kSnapshotCells, '.');
    CHECK(encode(Snapshot{}) == "SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... " + empty);
    Snapshot bomb{};
    bomb.cells[0] = PieceType::Bomb;
    CHECK(encode(bomb) == "SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... B" + std::string(kSnapshotCells - 1, '.'));
    Snapshot ghost{};
    ghost.ghost[0] = PieceType::T;  // ghost cells are the lowercase piece letter
    CHECK(encode(ghost) == "SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... t" + std::string(kSnapshotCells - 1, '.'));
    Snapshot held{};
    held.hold = PieceType::T;
    held.holdLocked = true;
    held.next = {PieceType::I, PieceType::O, PieceType::S, PieceType::Z, PieceType::L};
    CHECK(encode(held) == "SNAP 0 0 0 0 0 0 0 0 0 0 0 t IOSZL " + empty);
    held.holdLocked = false;
    CHECK(encode(held) == "SNAP 0 0 0 0 0 0 0 0 0 0 0 T IOSZL " + empty);
    Snapshot counted{};
    counted.stats = {40, 12, 5, 3, 2, 1, 4};
    CHECK(encode(counted) == "SNAP 0 0 0 0 40 12 5 3 2 1 4 . ..... " + empty);
    Snapshot incoming{.pending = 5, .ready = 3};
    CHECK(encode(incoming) == "SNAP 0 0 5 3 0 0 0 0 0 0 0 . ..... " + empty);
}

TEST_CASE("protocol: malformed messages are rejected") {
    for (std::string_view text : {"", "HELLO", "DEAD ", "DEAD now", "ATK", "ATK 0", "ATK -1", "ATK x", "ATK 41",
                                  "JOIN abcd", "JOIN ABC", "JOIN ABCDE", "CREATE 0", "CREATE 16", "START 1",
                                  "START -1 1", "SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... ...", "ERR", "ERR "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string badCell = "SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... " + std::string(kSnapshotCells, '.');
    badCell.back() = 'Q';
    CHECK_FALSE(decode(badCell).has_value());
    for (char ghostOfBlock : {'g', 'b'}) {  // garbage and bombs never fall, so they have no ghost
        badCell.back() = ghostOfBlock;
        CHECK_FALSE(decode(badCell).has_value());
    }
}

TEST_CASE("protocol: malformed snapshot hold and next fields are rejected") {
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 0 0 0 0 0 0 0 0 0 t IOSZL " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 0 " + cells).has_value());  // the old form, without hold and next
    for (std::string_view hold : {"G", "B", "g", "b", "x", "TT", ""}) {
        CAPTURE(hold);
        CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 0 " + std::string(hold) + " IOSZL " + cells).has_value());
    }
    for (std::string_view next : {"IOSZ", "IOSZLJ", "IOSZG", "IOSZB", "iosz l", "ioszl"}) {
        CAPTURE(next);
        CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 0 . " + std::string(next) + " " + cells).has_value());
    }
}

TEST_CASE("protocol: malformed snapshot stats are rejected") {
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 . ..... " + cells).has_value());          // the 6-argument form
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());  // one stat short
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 -1 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 x 0 0 0 . ..... " + cells).has_value());
}

TEST_CASE("protocol: malformed snapshot ready counts are rejected") {
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 3 3 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 2 3 0 0 0 0 0 0 0 . ..... " + cells).has_value());   // more ready than pending
    CHECK_FALSE(decode("SNAP 0 0 2 -1 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 2 x 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 2 0 0 0 0 0 0 0 . ..... " + cells).has_value());     // the old form, without ready
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
    CHECK(snapshotOf(game).ready == 0);
    game.tick(kGarbageTravelMs);
    CHECK(snapshotOf(game).ready == 3);
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

TEST_CASE("protocol: HELLO on the wire, and the removed OPP and REMATCH decode to nothing") {
    CHECK(encode(Hello{"Sam"}) == "HELLO Sam");
    CHECK(decode("HELLO x_y-z.9") == Message{Hello{"x_y-z.9"}});
    for (std::string_view text : {"HELLO", "HELLO ", "HELLO a b", "HELLO a:b", "HELLO abcdefghijklm", "OPP Kai",
                                  "OPP", "REMATCH"}) {
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
    CHECK(encode(RoomList{{{"ABCD", 3, "Sam", 1}, {"WXYZ", 12, "Kai", 2}}}) == "ROOMS ABCD:3:Sam:1 WXYZ:12:Kai:2");
}

TEST_CASE("protocol: malformed lobby messages are rejected") {
    for (std::string_view text : {"CREATE 3 OPEN", "CREATE 3 pub", "CREATE 3 PUB x", "LIST x", "UNLIST x", "QUICK",
                                  "QUICK 0", "QUICK 16", "QUICK x", "ROOMS ", "ROOMS ABCD", "ROOMS ABCD:3", "ROOMS ABCD:3:Sam",
                                  "ROOMS ABCD:3:", "ROOMS abcd:3:Sam:1", "ROOMS ABCD:0:Sam:1", "ROOMS ABCD:3:Sam:0", "ROOMS ABCD:3:Sam:16", "ROOMS ABCD:3:Sam:x", "ROOMS ABCD:3:Sam:1:x",
                                  "ROOMS ABCD:3:a b:1", "ROOMS ABCD:x:Sam:1", "ROOMS ABCD:3:Sam:1 "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string tooMany = "ROOMS";
    for (int i = 0; i <= kMaxListedRooms; ++i) tooMany += " ABCD:1:Sam:1";
    CHECK_FALSE(decode(tooMany).has_value());
}

TEST_CASE("protocol: room messages carry player ids") {
    CHECK(encode(RoomEntered{"ABCD", 3}) == "ROOM ABCD 3");
    CHECK(encode(StartMatch{7, 2, {1, 4, 9}}) == "START 7 2 1 4 9");
    CHECK(encode(Roster{2, {{1, "Sam", true}, {2, "Kim", false}}}) == "PLAYERS 2 1:Sam:1 2:Kim:0");
    CHECK(encode(Begin{}) == "BEGIN");
    CHECK(encode(PlayerOut{4, 9}) == "OUT 4 9");
    CHECK(encode(RoomList{{{"ABCD", 3, "Sam", 5}}}) == "ROOMS ABCD:3:Sam:5");
    CHECK(encode(PlayerSnapshot{5, Snapshot{}}) == "SNAP 5 " + encode(Snapshot{}).substr(5));
    CHECK(decode(encode(Snapshot{})) == Message{Snapshot{}});  // 14 arguments: a client's own snapshot
}

TEST_CASE("protocol: malformed player-id messages are rejected") {
    const std::string args = encode(Snapshot{}).substr(5);
    std::vector<std::string> bad{"ROOM ABCD", "ROOM ABCD 0", "ROOM ABCD 17", "ROOM ABCD x", "ROOM ABCD 1 2",
                                 "START 7 2", "START 7 2 1", "START 7 2 1 1", "START 7 2 1 17", "START 7 2 0 1",
                                 "START 7 2 1 x", "PLAYERS 1", "PLAYERS 3 1:Sam:1", "PLAYERS 1 1:Sam:2",
                                 "PLAYERS 1 1:Sam:1 1:Kim:0", "PLAYERS 1 1:bad name:1", "PLAYERS 1 1:Sam",
                                 "PLAYERS 1 0:Sam:1", "PLAYERS 1 17:Sam:1", "PLAYERS 1 1::1", "PLAYERS 1 1:Sam:1:x",
                                 "OUT 0 2", "OUT 3 0", "OUT 3 17", "OUT 17 2", "OUT 3", "BEGIN now",
                                 "SNAP 0 " + args, "SNAP 17 " + args, "SNAP x " + args};
    std::string manyStarts = "START 7 2";
    for (int i = 1; i <= kMaxRoomPlayers + 1; ++i) manyStarts += " " + std::to_string(i);
    bad.push_back(manyStarts);
    std::string manyPlayers = "PLAYERS 1";
    for (int i = 1; i <= kMaxRoomPlayers + 1; ++i) manyPlayers += " " + std::to_string(i) + ":P:0";
    bad.push_back(manyPlayers);
    for (const std::string& text : bad) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}

TEST_CASE("protocol: IN round-trips every token") {
    InputBatch batch{900, {}};
    const LogCode codes[] = {LogCode::Left,     LogCode::Right,    LogCode::RotateCW, LogCode::RotateCCW,
                             LogCode::HardDrop, LogCode::Hold,     LogCode::LeftWall, LogCode::RightWall,
                             LogCode::SoftOn,   LogCode::SoftOff,  LogCode::Garbage};
    int t = 10;
    for (LogCode code : codes) {
        LogEntry entry{t++, code};
        if (code == LogCode::Garbage) entry.garbageId = 3, entry.garbageLines = 2;
        batch.entries.push_back(entry);
    }
    const std::string text = "IN 900 10:L 11:R 12:CW 13:CCW 14:HD 15:H 16:LW 17:RW 18:S+ 19:S- 20:G3x2";
    CHECK(encode(batch) == text);
    CHECK(decode(text) == Message{batch});
}

TEST_CASE("protocol: SDF, GARB and RESYNC round-trip") {
    for (const char* text : {"SDF 0", "SDF 40", "GARB 5 4", "RESYNC 120 3:HD 100:G1x2"}) {
        CAPTURE(text);
        const auto message = decode(text);
        REQUIRE(message.has_value());
        CHECK(encode(*message) == text);
    }
    CHECK(decode("SDF 40") == Message{SoftDropSetting{40}});
    CHECK(decode("GARB 5 4") == Message{GarbageIn{5, 4}});
    CHECK(decode("RESYNC 120 3:HD 100:G1x2") ==
          Message{Resync{120, {{3, LogCode::HardDrop}, {100, LogCode::Garbage, 1, 2}}}});
}

TEST_CASE("protocol: malformed input batches are rejected") {
    std::string tooMany = "IN 100";
    for (int i = 0; i <= kMaxInputEvents; ++i) tooMany += " " + std::to_string(i) + ":L";
    std::vector<std::string> bad{tooMany, "IN 10 11:L", "IN 10 5:L 4:R", "IN 10 5:X", "IN 10 5:G0x2", "SDF 3",
                                 "SDF 41", "GARB 0 1", "GARB 1 41", "IN -1", "RESYNC 5 3:L 2:R"};
    for (const std::string& text : bad) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}
