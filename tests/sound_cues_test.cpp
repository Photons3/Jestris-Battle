#include <doctest/doctest.h>

#include "app/sound_cues.hpp"
#include "test_helpers.hpp"

using namespace tetris;

TEST_CASE("cues: game events map to their sounds") {
    CHECK(soundFor(GameEvent{GameEventType::Moved}, false) == Sound::Move);
    CHECK(soundFor(GameEvent{GameEventType::Rotated}, false) == Sound::Rotate);
    CHECK(soundFor(GameEvent{GameEventType::Held}, false) == Sound::Hold);
    CHECK(soundFor(GameEvent{GameEventType::HardDropped}, false) == Sound::HardDrop);
    CHECK(soundFor(GameEvent{GameEventType::Locked}, false) == Sound::Lock);
    CHECK(soundFor(GameEvent{GameEventType::LinesCleared, 1}, false) == Sound::Clear1);
    CHECK(soundFor(GameEvent{GameEventType::LinesCleared, 2}, false) == Sound::Clear2);
    CHECK(soundFor(GameEvent{GameEventType::LinesCleared, 3}, false) == Sound::Clear3);
    CHECK(soundFor(GameEvent{GameEventType::LinesCleared, 4}, false) == Sound::Tetris);
    CHECK(soundFor(GameEvent{GameEventType::BombExploded}, false) == Sound::Bomb);
    CHECK(soundFor(GameEvent{GameEventType::GarbageRose}, false) == Sound::Garbage);
    CHECK(soundFor(GameEvent{GameEventType::LevelUp}, false) == Sound::LevelUp);
}

TEST_CASE("cues: a top-out ends a solo game but is a KO in a duel") {
    CHECK(soundFor(GameEvent{GameEventType::ToppedOut}, false) == Sound::Lost);
    CHECK(soundFor(GameEvent{GameEventType::ToppedOut}, true) == Sound::KoTheirs);
}

TEST_CASE("cues: duel events map to their sounds") {
    CHECK(soundFor(DuelEvent::CountdownTick) == Sound::CountdownTick);
    CHECK(soundFor(DuelEvent::MatchStart) == Sound::Go);
    CHECK(soundFor(DuelEvent::Attacked) == Sound::Attacked);
    CHECK(soundFor(DuelEvent::KoMine) == Sound::KoMine);
    CHECK(soundFor(DuelEvent::Won) == Sound::Won);
    CHECK(soundFor(DuelEvent::Lost) == Sound::Lost);
    CHECK(soundFor(DuelEvent::Draw) == Sound::Draw);
}

TEST_CASE("cues: combos climb seven steps and then repeat the top one") {
    for (int n = 1; n <= kComboSteps; ++n) {
        CAPTURE(n);
        CHECK(soundFor(GameEvent{GameEventType::Combo, n}, false) ==
              static_cast<Sound>(static_cast<int>(Sound::Combo1) + n - 1));
    }
    CHECK(soundFor(GameEvent{GameEventType::Combo, 8}, false) == Sound::Combo7);
    CHECK(soundFor(GameEvent{GameEventType::Combo, 20}, true) == Sound::Combo7);
}

TEST_CASE("cues: solo music speeds up 8 BPM a level from 120, up to 200") {
    CHECK(soloBpm(1) == 120.0f);
    CHECK(soloBpm(2) == 128.0f);
    CHECK(soloBpm(10) == 192.0f);
    CHECK(soloBpm(11) == 200.0f);
    CHECK(soloBpm(15) == 200.0f);
}

TEST_CASE("cues: solo music plays, pauses with the game and stops at game over") {
    Game game(seedWithFirst(PieceType::T), 5);
    CHECK(soloMusic(game) == MusicCue{MusicMode::Playing, soloBpm(5)});
    game.pause();
    CHECK(soloMusic(game) == MusicCue{MusicMode::Paused, soloBpm(5)});
    game.resume();
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    REQUIRE(game.state() == GameState::GameOver);
    CHECK(soloMusic(game).mode == MusicMode::Stopped);
}

TEST_CASE("cues: solo music keeps playing through a line clear") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.state() == GameState::Clearing);
    CHECK(soloMusic(game).mode == MusicMode::Playing);
}

TEST_CASE("cues: duel music plays only while the match runs") {
    CHECK(duelMusic(DuelPhase::Playing) == MusicCue{MusicMode::Playing, kDuelBpm});
    for (DuelPhase phase : {DuelPhase::Idle, DuelPhase::Lobby, DuelPhase::Waiting, DuelPhase::Countdown, DuelPhase::Result}) {
        CAPTURE(static_cast<int>(phase));
        CHECK(duelMusic(phase).mode == MusicMode::Stopped);
    }
}
