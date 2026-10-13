#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/osc.hpp"

namespace tetris {

struct ScoreNote {
    int midi;     // MIDI note number; 69 is A4 (440 Hz)
    float beats;  // quarter notes
};

inline constexpr float kScoreBeats = 96.0f;  // Korobeiniki played A A B
inline constexpr float kDefaultBpm = 120.0f;

const std::vector<ScoreNote>& melodyScore();
const std::vector<ScoreNote>& bassScore();
float beatsOf(const std::vector<ScoreNote>& score);

// Plays the score on a loop, synthesizing each sample as it goes. Not thread-safe: Mixer callers serialise.
class Music {
public:
    explicit Music(int sampleRate);

    void start();                 // from the top, unpaused
    void stop();                  // silent and rewound
    void setPaused(bool paused);  // keeps the position; no effect while stopped
    void setTempo(float bpm);     // both voices pick it up together when the melody starts its next note
    bool playing() const { return started_; }  // paused still counts as playing
    // Adds the next `frames` samples, times `gain`, into `out`. Adds nothing while stopped or paused.
    void mix(float* out, int frames, float gain = 1.0f);
    // Samples played of voice 0 (melody) or 1 (bass)'s current note; 1 just after a note begins. For tests.
    int notePos(std::size_t voice) const { return static_cast<int>(sample_ - voices_[voice].start); }

private:
    struct Voice {
        const std::vector<ScoreNote>* score;
        Wave wave;
        float volume;
        std::size_t index = 0;
        double beat = 0.0;       // where the current note starts in the score
        std::int64_t start = 0;  // sample the current note starts on
        std::int64_t end = 0;    // sample the next note starts on
        float hz = 0.0f;
        double phase = 0.0;
    };

    void rewind();
    void beginNote(Voice& voice);
    void nextNote(Voice& voice);
    float nextSample(Voice& voice);

    int sampleRate_;
    float bpm_ = kDefaultBpm;      // the tempo being played
    float nextBpm_ = kDefaultBpm;  // set by setTempo, played from the melody's next note
    // Every note ends on the shared beat clock: anchorSample_ is where beat anchorBeat_ fell at the last
    // tempo switch or loop wrap, so the voices stay together and rounding never adds up.
    std::int64_t sample_ = 0;
    std::int64_t anchorSample_ = 0;
    double anchorBeat_ = 0.0;
    bool started_ = false;
    bool paused_ = false;
    std::array<Voice, 2> voices_;
};

}  // namespace tetris
