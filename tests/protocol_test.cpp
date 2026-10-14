#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("protocol: every message survives an encode/decode round trip") {
    Snapshot snapshot{1200, 7, 3, {}};
    snapshot.cells[0] = PieceType::Garbage;
    snapshot.cells[kSnapshotCells - 1] = PieceType::I;
    snapshot.cells[1] = PieceType::Bomb;
    const std::vector<Message> messages{CreateRoom{3},
                                        JoinRoom{"ABCD"},
                                        RoomCreated{"WXYZ"},
                                        StartMatch{4000000000u, 15},
                                        Attack{4},
                                        snapshot,
                                        Dead{},
                                        Rematch{},
                                        OpponentLeft{},
                                        ServerError{"room is full"},
                                        KoCounts{1, 0},
                                        KoCounts{3, 2},
                                        MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7},
                                        MatchOver{MatchResult::Draw, MatchReason::Time, 0, 0},
                                        Hello{"Sam"},
                                        OpponentName{"x_y-z.9"}};
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
    CHECK(encode(Snapshot{}) == "SNAP 0 0 0 " + std::string(kSnapshotCells, '.'));
    Snapshot bomb{};
    bomb.cells[0] = PieceType::Bomb;
    CHECK(encode(bomb) == "SNAP 0 0 0 B" + std::string(kSnapshotCells - 1, '.'));
}

TEST_CASE("protocol: malformed messages are rejected") {
    for (std::string_view text : {"", "HELLO", "DEAD ", "DEAD now", "ATK", "ATK 0", "ATK -1", "ATK x", "ATK 41",
                                  "JOIN abcd", "JOIN ABC", "JOIN ABCDE", "CREATE 0", "CREATE 16", "START 1",
                                  "START -1 1", "SNAP 0 0 0 ...", "ERR", "ERR "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string badCell = "SNAP 0 0 0 " + std::string(kSnapshotCells, '.');
    badCell.back() = 'Q';
    CHECK_FALSE(decode(badCell).has_value());
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
    CHECK(snapshotOf(game) == Snapshot{});  // the spawned piece is above the visible field
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
