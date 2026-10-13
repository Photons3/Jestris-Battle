#pragma once

#include <cstddef>
#include <vector>

#include "audio/music.hpp"
#include "audio/sound.hpp"

namespace tetris {

inline constexpr int kMaxVoices = 8;
inline constexpr float kMasterGain = 0.5f;
inline constexpr float kMusicGain = 0.4f;  // music sits under the effects

// Plays cached sounds on up to kMaxVoices voices. Not thread-safe: AudioOut serialises calls.
class Mixer {
public:
    explicit Mixer(int sampleRate);  // renders every sound once

    void play(Sound sound);            // ignored while muted; replaces the oldest voice when all are busy
    void mix(float* out, int frames);  // overwrites `out` with the next `frames` samples
    void setMuted(bool muted);         // muting also stops the effects that are playing; the music keeps its place
    bool muted() const { return muted_; }
    int activeVoices() const { return static_cast<int>(voices_.size()); }

    void startMusic() { music_.start(); }
    void stopMusic() { music_.stop(); }
    void setMusicPaused(bool paused) { music_.setPaused(paused); }
    void setMusicTempo(float bpm) { music_.setTempo(bpm); }
    void setMusicMuted(bool muted) { musicMuted_ = muted; }  // the song keeps advancing while muted
    bool musicMuted() const { return musicMuted_; }
    bool musicPlaying() const { return music_.playing(); }

private:
    struct Voice {
        int sound;
        std::size_t pos;
    };

    std::vector<std::vector<float>> cache_;  // indexed by Sound
    std::vector<Voice> voices_;              // oldest first
    bool muted_ = false;
    Music music_;
    bool musicMuted_ = false;
};

}  // namespace tetris
