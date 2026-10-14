#include <vector>

#include <doctest/doctest.h>

#include "bot/bot_player.hpp"
#include "net/protocol.hpp"

using namespace tetris;

namespace {

int playFor(BotLevel level, std::uint32_t seed, int ms, Game& game) {
    BotPlayer bot(level, seed);
    int pieces = 0;
    for (int t = 0; t < ms && game.state() != GameState::GameOver; ++t) {
        bot.update(game, 1);
        game.tick(1);
        for (const GameEvent& e : game.takeEvents()) pieces += e.type == GameEventType::HardDropped;
    }
    return pieces;
}

std::vector<int> snapshotCells(const Game& game) {
    std::vector<int> cells;
    for (int y = 0; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x) {
            const Cell c = game.board().at(x, y);
            cells.push_back(c ? static_cast<int>(*c) : -1);
        }
    return cells;
}

}  // namespace

TEST_CASE("bot: hard survives 500 pieces on its own") {
    Game game(7, 1);
    BotPlayer bot(BotLevel::Hard, 7);
    int pieces = 0;
    for (int ms = 0; pieces < 500 && game.state() != GameState::GameOver && ms < 600000; ++ms) {
        bot.update(game, 1);
        game.tick(1);
        for (const GameEvent& e : game.takeEvents()) pieces += e.type == GameEventType::HardDropped;
    }
    CHECK(pieces >= 500);
    CHECK(game.state() != GameState::GameOver);
}

TEST_CASE("bot: each level plays at its pace") {
    auto pps = [](BotLevel level) { Game game(3, 1); return playFor(level, 3, 60000, game) / 60.0; };
    const double easy = pps(BotLevel::Easy), medium = pps(BotLevel::Medium), hard = pps(BotLevel::Hard);
    CHECK(easy == doctest::Approx(0.8).epsilon(0.25));
    CHECK(medium == doctest::Approx(1.5).epsilon(0.25));
    CHECK(hard == doctest::Approx(2.5).epsilon(0.25));
}

TEST_CASE("bot: the same seed plays the same game") {
    Game a(5, 1), b(5, 1);
    playFor(BotLevel::Medium, 9, 20000, a);
    playFor(BotLevel::Medium, 9, 20000, b);
    CHECK(snapshotCells(a) == snapshotCells(b));
}

TEST_CASE("bot: does nothing once the game is over") {
    Game game(1, 1);
    for (int i = 0; i < 100 && game.state() != GameState::GameOver; ++i) game.apply(Action::HardDrop);
    BotPlayer bot(BotLevel::Hard, 1);
    bot.update(game, 5000);
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("bot: names are valid player names") {
    for (BotLevel l : {BotLevel::Easy, BotLevel::Medium, BotLevel::Hard}) CHECK(isValidName(botName(l)));
}

TEST_CASE("bot: replans when gravity moves the piece mid-plan") {
    // At level 19 a piece falls to the stack while the bot is still shifting it; plans that go on as if
    // nothing happened drop pieces in the wrong column and the stack tops out within a couple of hundred pieces.
    Game game(11, 19);
    BotPlayer bot(BotLevel::Hard, 11);
    int pieces = 0;
    for (int ms = 0; pieces < 200 && game.state() != GameState::GameOver && ms < 600000; ++ms) {
        bot.update(game, 1);
        game.tick(1);
        for (const GameEvent& e : game.takeEvents()) pieces += e.type == GameEventType::HardDropped;
    }
    CHECK(pieces >= 200);
    CHECK(game.state() != GameState::GameOver);
}

TEST_CASE("bot: a mistake is chosen once per piece, not again on every replan") {
    // At level 8 gravity moves the piece between most of Easy's key presses, so the bot replans nearly every
    // input. A replan keeps the piece's target; rolling the mistake again would send it back and forth.
    for (BotLevel level : {BotLevel::Easy, BotLevel::Medium}) {
        Game game(3, 8);
        BotPlayer bot(level, 3);
        int pieces = 0, reversed = 0;
        bool movedLeft = false, movedRight = false;
        for (int ms = 0; ms < 60000 && game.state() != GameState::GameOver; ++ms) {
            const Piece before = game.active();
            bot.update(game, 1);
            bool pieceDone = false;
            for (const GameEvent& e : game.takeEvents()) pieceDone |= e.type == GameEventType::HardDropped;
            const Piece after = game.active();
            if (!pieceDone && after.type == before.type && after.rotation == before.rotation) {  // no kicks
                movedLeft |= after.pos.x < before.pos.x;
                movedRight |= after.pos.x > before.pos.x;
            }
            game.tick(1);
            for (const GameEvent& e : game.takeEvents()) pieceDone |= e.type == GameEventType::Locked;
            if (pieceDone) {
                ++pieces;
                reversed += movedLeft && movedRight;
                movedLeft = movedRight = false;
            }
        }
        CAPTURE(botName(level));
        CAPTURE(pieces);
        CHECK(pieces >= 20);
        CHECK(reversed == 0);
    }
}
