#pragma once

#include <vector>

#include "audio/sound.hpp"

namespace tetris {

inline constexpr int kMaxSoundMs = 2500;

// Mono samples in [-1, 1]. Deterministic: the same sound always renders the same samples.
std::vector<float> render(Sound sound, int sampleRate);

}  // namespace tetris
