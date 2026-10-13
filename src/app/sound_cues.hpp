#pragma once

#include "app/duel.hpp"
#include "audio/sound.hpp"
#include "core/game.hpp"

namespace tetris {

// In a duel a top-out is a KO against us, not the end of the game.
Sound soundFor(const GameEvent& event, bool inDuel);
Sound soundFor(DuelEvent event);

enum class MusicMode { Stopped, Playing, Paused };

struct MusicCue {
    MusicMode mode = MusicMode::Stopped;
    float bpm = 0.0f;
    friend bool operator==(const MusicCue&, const MusicCue&) = default;
};

inline constexpr float kDuelBpm = 150.0f;

float soloBpm(int level);               // 120, plus 8 per level above 1, at most 200
MusicCue soloMusic(const Game& game);   // plays while the game runs or clears, pauses with it
MusicCue duelMusic(DuelPhase phase);    // plays only while the match runs

}  // namespace tetris
