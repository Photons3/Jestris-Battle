#pragma once

#include <cmath>

namespace tetris {

enum class Wave { Square, Triangle, Noise };

// Moves `phase` on by one sample of a `hz` tone and wraps it into [0, 1). Returns true when it wrapped.
inline bool advancePhase(double& phase, float hz, int sampleRate) {
    phase += static_cast<double>(hz) / sampleRate;
    const bool wrapped = phase >= 1.0;
    phase -= std::floor(phase);
    return wrapped;
}

inline float squareWave(double phase) { return phase < 0.5 ? 1.0f : -1.0f; }
inline float triangleWave(double phase) { return 4.0f * std::abs(static_cast<float>(phase) - 0.5f) - 1.0f; }

}  // namespace tetris
