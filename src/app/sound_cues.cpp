#include "app/sound_cues.hpp"

#include <algorithm>

namespace tetris {

std::optional<Sound> soundFor(const GameEvent& event, bool inDuel) {
    switch (event.type) {
    case GameEventType::Moved:
        return Sound::Move;
    case GameEventType::Rotated:
        return Sound::Rotate;
    case GameEventType::Held:
        return Sound::Hold;
    case GameEventType::HardDropped:
        return Sound::HardDrop;
    case GameEventType::Locked:
        return Sound::Lock;
    case GameEventType::LinesCleared:
        return event.value >= 4 ? Sound::Tetris : event.value == 3 ? Sound::Clear3
                                              : event.value == 2   ? Sound::Clear2
                                                                   : Sound::Clear1;
    case GameEventType::BombExploded:
        return Sound::Bomb;
    case GameEventType::GarbageRose:
        return Sound::Garbage;
    case GameEventType::LevelUp:
        return Sound::LevelUp;
    case GameEventType::ToppedOut:
        return inDuel ? Sound::KoTheirs : Sound::Lost;
    case GameEventType::Combo:
        return static_cast<Sound>(static_cast<int>(Sound::Combo1) + std::clamp(event.value, 1, kComboSteps) - 1);
    case GameEventType::SpecialClear:  // one sound for the biggest part of the clear
        if (event.award.perfectClear) return Sound::PerfectClear;
        if (event.award.backToBack > 0) return Sound::BackToBack;
        if (event.award.spin != Spin::None) return Sound::TSpin;
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Sound> soundFor(SoloEvent event) {
    switch (event) {
    case SoloEvent::CountdownTick:
        return Sound::CountdownTick;
    case SoloEvent::Start:
        return Sound::Go;
    case SoloEvent::Finished:
        break;
    }
    return std::nullopt;
}

std::optional<Sound> soundFor(DuelEvent event) {
    switch (event) {
    case DuelEvent::CountdownTick:
        return Sound::CountdownTick;
    case DuelEvent::MatchStart:
        return Sound::Go;
    case DuelEvent::Attacked:
        return Sound::Attacked;
    case DuelEvent::Sent:
        return std::nullopt;
    case DuelEvent::KoMine:
        return Sound::KoMine;
    case DuelEvent::Won:
        return Sound::Won;
    case DuelEvent::Lost:
        return Sound::Lost;
    case DuelEvent::Draw:
        return Sound::Draw;
    }
    return Sound::CountdownTick;
}

float soloBpm(int level) { return std::min(120.0f + 8.0f * static_cast<float>(level - 1), 200.0f); }

MusicCue soloMusic(const Game& game) {
    switch (game.state()) {
    case GameState::Playing:
    case GameState::Clearing:
        return {MusicMode::Playing, soloBpm(game.level())};
    case GameState::Paused:
        return {MusicMode::Paused, soloBpm(game.level())};
    case GameState::GameOver:
        break;
    }
    return {};
}

MusicCue soloMusic(const SoloSession& solo) {
    if (solo.phase() != SoloPhase::Playing) return {};
    return soloMusic(solo.game());
}

MusicCue duelMusic(DuelPhase phase) {
    if (phase == DuelPhase::Playing) return {MusicMode::Playing, kDuelBpm};
    return {};
}

}  // namespace tetris
