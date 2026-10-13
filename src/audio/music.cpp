#include "audio/music.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace tetris {
namespace {

constexpr float kMelodyVolume = 0.35f;
constexpr float kBassVolume = 0.4f;
constexpr float kAttackS = 0.005f;
constexpr float kReleaseS = 0.02f;
constexpr float kGate = 0.85f;  // a note sounds for this share of its length, then releases

constexpr int kGs4 = 68, kA4 = 69, kB4 = 71, kC5 = 72, kD5 = 74, kE5 = 76, kF5 = 77, kG5 = 79, kGs5 = 80, kA5 = 81;
constexpr int kC2 = 36, kD2 = 38, kE2 = 40, kA2 = 45;

constexpr std::array<ScoreNote, 37> kPartA{{
    {kE5, 1}, {kB4, 0.5f}, {kC5, 0.5f}, {kD5, 1}, {kC5, 0.5f}, {kB4, 0.5f},
    {kA4, 1}, {kA4, 0.5f}, {kC5, 0.5f}, {kE5, 1}, {kD5, 0.5f}, {kC5, 0.5f},
    {kB4, 1.5f}, {kC5, 0.5f}, {kD5, 1}, {kE5, 1},
    {kC5, 1}, {kA4, 1}, {kA4, 2},
    {kD5, 1.5f}, {kF5, 0.5f}, {kA5, 1}, {kG5, 0.5f}, {kF5, 0.5f},
    {kE5, 1.5f}, {kC5, 0.5f}, {kE5, 1}, {kD5, 0.5f}, {kC5, 0.5f},
    {kB4, 1}, {kB4, 0.5f}, {kC5, 0.5f}, {kD5, 1}, {kE5, 1},
    {kC5, 1}, {kA4, 1}, {kA4, 2},
}};

constexpr std::array<ScoreNote, 16> kPartB{{
    {kE5, 2}, {kC5, 2}, {kD5, 2}, {kB4, 2}, {kC5, 2}, {kA4, 2}, {kGs4, 2}, {kB4, 2},
    {kE5, 2}, {kC5, 2}, {kD5, 2}, {kB4, 2}, {kC5, 1}, {kE5, 1}, {kA5, 2}, {kGs5, 4},
}};

// One root per 4-beat bar; the bass alternates it with its octave in eighth notes.
constexpr std::array<int, 8> kBassA{kE2, kA2, kE2, kA2, kD2, kC2, kE2, kA2};
constexpr std::array<int, 8> kBassB{kA2, kE2, kA2, kE2, kA2, kE2, kA2, kE2};

std::vector<ScoreNote> buildMelody() {
    std::vector<ScoreNote> notes;
    for (const auto* part : {&kPartA, &kPartA}) notes.insert(notes.end(), part->begin(), part->end());
    notes.insert(notes.end(), kPartB.begin(), kPartB.end());
    return notes;
}

std::vector<ScoreNote> buildBass() {
    std::vector<ScoreNote> notes;
    for (const auto* roots : {&kBassA, &kBassA, &kBassB})
        for (int root : *roots)
            for (int i = 0; i < 4; ++i) {
                notes.push_back({root, 0.5f});
                notes.push_back({root + 12, 0.5f});
            }
    return notes;
}

float envelope(int pos, int length, int sampleRate) {
    const float t = static_cast<float>(pos) / static_cast<float>(sampleRate);
    const float gateEnd = kGate * static_cast<float>(length) / static_cast<float>(sampleRate);
    if (t < kAttackS) return t / kAttackS;
    if (t < gateEnd) return 1.0f;
    return std::max(0.0f, 1.0f - (t - gateEnd) / kReleaseS);
}

}  // namespace

const std::vector<ScoreNote>& melodyScore() {
    static const std::vector<ScoreNote> kMelody = buildMelody();
    return kMelody;
}

const std::vector<ScoreNote>& bassScore() {
    static const std::vector<ScoreNote> kBass = buildBass();
    return kBass;
}

float beatsOf(const std::vector<ScoreNote>& score) {
    float beats = 0.0f;
    for (const ScoreNote& note : score) beats += note.beats;
    return beats;
}

Music::Music(int sampleRate)
    : sampleRate_(sampleRate),
      voices_{{{&melodyScore(), Wave::Square, kMelodyVolume}, {&bassScore(), Wave::Triangle, kBassVolume}}} {
    rewind();
}

void Music::start() {
    started_ = true;
    paused_ = false;
    rewind();
}

void Music::stop() {
    started_ = false;
    paused_ = false;
    rewind();
}

void Music::setPaused(bool paused) {
    if (started_) paused_ = paused;
}

void Music::setTempo(float bpm) { nextBpm_ = std::max(bpm, 1.0f); }

void Music::mix(float* out, int frames, float gain) {
    if (!started_ || paused_) return;
    for (int i = 0; i < frames; ++i) {
        float sample = 0.0f;
        for (Voice& voice : voices_) sample += nextSample(voice);
        out[i] += sample * gain;
        ++sample_;
    }
}

void Music::rewind() {
    sample_ = 0;
    anchorSample_ = 0;
    anchorBeat_ = 0.0;
    bpm_ = nextBpm_;
    for (Voice& voice : voices_) {
        voice.index = 0;
        voice.beat = 0.0;
        beginNote(voice);
    }
}

// The phase restarts with each note, so every pass through the loop is sample-for-sample the same.
void Music::beginNote(Voice& voice) {
    const ScoreNote& note = (*voice.score)[voice.index];
    const double samplesPerBeat = 60.0 * sampleRate_ / bpm_;
    voice.start = sample_;
    voice.end = anchorSample_ + std::llround((voice.beat + note.beats - anchorBeat_) * samplesPerBeat);
    voice.end = std::max(voice.end, voice.start + 1);
    voice.hz = 440.0f * std::exp2(static_cast<float>(note.midi - 69) / 12.0f);
    voice.phase = 0.0;
}

// Melody notes all start on the eighth-note grid, where a bass note starts too (it's processed second),
// so switching the tempo on a melody note switches both voices on the same sample.
void Music::nextNote(Voice& voice) {
    voice.beat += (*voice.score)[voice.index].beats;
    voice.index = (voice.index + 1) % voice.score->size();
    if (voice.index == 0) voice.beat = 0.0;
    if (&voice == &voices_[0] && (voice.index == 0 || bpm_ != nextBpm_)) {
        anchorSample_ = sample_;
        anchorBeat_ = voice.beat;
        bpm_ = nextBpm_;
    }
    beginNote(voice);
}

float Music::nextSample(Voice& voice) {
    if (sample_ >= voice.end) nextNote(voice);
    advancePhase(voice.phase, voice.hz, sampleRate_);
    const float wave = voice.wave == Wave::Square ? squareWave(voice.phase) : triangleWave(voice.phase);
    const auto pos = static_cast<int>(sample_ - voice.start);
    const auto length = static_cast<int>(voice.end - voice.start);
    return wave * voice.volume * envelope(pos, length, sampleRate_);
}

}  // namespace tetris
