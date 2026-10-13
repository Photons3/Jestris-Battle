#pragma once

namespace tetris {

enum class Sound {
    Move, Rotate, Hold, HardDrop, Lock, Clear1, Clear2, Clear3, Tetris, Bomb, Garbage, LevelUp,
    Combo1, Combo2, Combo3, Combo4, Combo5, Combo6, Combo7,  // keep consecutive: indexed from Combo1
    CountdownTick, Go, Attacked, KoMine, KoTheirs, Won, Lost, Draw,
    Count  // not a sound: the number of sounds
};

inline constexpr int kSoundCount = static_cast<int>(Sound::Count);
inline constexpr int kComboSteps = 7;  // Combo1-Combo7; longer combos repeat Combo7

}  // namespace tetris
