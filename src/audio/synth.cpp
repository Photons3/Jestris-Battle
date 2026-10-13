#include "audio/synth.hpp"
#include "audio/osc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace tetris {
namespace {

struct Note {
    Wave wave;
    float startHz;
    float endHz;   // the pitch slides linearly from startHz; for Noise it is how often a new random value is drawn
    float at;      // seconds from the start of the sound
    float length;  // seconds
    float volume;
};

constexpr float kAttackS = 0.005f;

constexpr float kC3 = 130.81f, kA3 = 220.0f;
constexpr float kC4 = 261.63f, kD4 = 293.66f, kE4 = 329.63f, kG4 = 392.0f, kA4 = 440.0f;
constexpr float kC5 = 523.25f, kE5 = 659.26f, kG5 = 783.99f, kC6 = 1046.5f;
// Combo1-Combo7 climb a G major scale from G5, above the clear arpeggios.
constexpr std::array<float, kComboSteps> kComboHz{783.99f, 880.0f, 987.77f, 1046.5f, 1174.66f, 1318.51f, 1479.98f};

// Notes played one after another, `step` seconds apart; the last one rings for `last` seconds.
std::vector<Note> run(Wave wave, std::initializer_list<float> pitches, float step, float last, float volume) {
    std::vector<Note> notes;
    float at = 0.0f;
    for (float hz : pitches) {
        notes.push_back({wave, hz, hz, at, step, volume});
        at += step;
    }
    notes.back().length = last;
    return notes;
}

std::vector<Note> notesFor(Sound sound) {
    switch (sound) {
    case Sound::Move:
        return {{Wave::Square, kA4, kA4, 0.0f, 0.025f, 0.25f}};
    case Sound::Rotate:
        return {{Wave::Square, kE5, kE5, 0.0f, 0.03f, 0.25f}};
    case Sound::Hold:
        return run(Wave::Square, {kC5, kG5}, 0.04f, 0.05f, 0.35f);
    case Sound::HardDrop:
        return {{Wave::Noise, 3000.0f, 1000.0f, 0.0f, 0.08f, 0.6f}, {Wave::Square, 110.0f, 55.0f, 0.0f, 0.08f, 0.4f}};
    case Sound::Lock:
        return {{Wave::Noise, 4000.0f, 4000.0f, 0.0f, 0.03f, 0.25f}};
    case Sound::Clear1:
        return run(Wave::Triangle, {kC5, kE5}, 0.06f, 0.12f, 0.5f);
    case Sound::Clear2:
        return run(Wave::Triangle, {kC5, kE5, kG5}, 0.06f, 0.14f, 0.55f);
    case Sound::Clear3:
        return run(Wave::Triangle, {kC5, kE5, kG5, kC6}, 0.06f, 0.16f, 0.6f);
    case Sound::Tetris:
        return run(Wave::Square, {kC5, kE5, kG5, kC6}, 0.08f, 0.25f, 0.7f);
    case Sound::Bomb:
        return {{Wave::Noise, 2000.0f, 200.0f, 0.0f, 0.4f, 0.8f}, {Wave::Square, 150.0f, 40.0f, 0.0f, 0.3f, 0.5f}};
    case Sound::Garbage:
        return {{Wave::Noise, 300.0f, 100.0f, 0.0f, 0.25f, 0.5f}, {Wave::Triangle, 80.0f, 60.0f, 0.0f, 0.25f, 0.5f}};
    case Sound::LevelUp:
        return {{Wave::Triangle, kC5, kC5, 0.0f, 0.25f, 0.3f},
                {Wave::Triangle, kE5, kE5, 0.0f, 0.25f, 0.3f},
                {Wave::Triangle, kG5, kG5, 0.0f, 0.25f, 0.3f}};
    case Sound::Combo1:
    case Sound::Combo2:
    case Sound::Combo3:
    case Sound::Combo4:
    case Sound::Combo5:
    case Sound::Combo6:
    case Sound::Combo7: {
        const float hz = kComboHz[static_cast<std::size_t>(static_cast<int>(sound) - static_cast<int>(Sound::Combo1))];
        return {{Wave::Square, hz, hz, 0.0f, 0.09f, 0.5f}, {Wave::Triangle, 2 * hz, 2 * hz, 0.09f, 0.12f, 0.2f}};
    }
    case Sound::CountdownTick:
        return {{Wave::Square, kA4, kA4, 0.0f, 0.08f, 0.3f}};
    case Sound::Go:
        return {{Wave::Square, 2 * kA4, 2 * kA4, 0.0f, 0.2f, 0.4f}};
    case Sound::Attacked:
        return {{Wave::Square, 220.0f, 220.0f, 0.0f, 0.05f, 0.4f}, {Wave::Square, 220.0f, 220.0f, 0.08f, 0.05f, 0.4f}};
    case Sound::KoMine:
        return {{Wave::Noise, 6000.0f, 6000.0f, 0.0f, 0.06f, 0.6f},
                {Wave::Square, kC5, kC6, 0.06f, 0.15f, 0.6f},
                {Wave::Triangle, kC6, kC6, 0.21f, 0.5f, 0.4f},
                {Wave::Triangle, 2 * kG5, 2 * kG5, 0.21f, 0.5f, 0.4f}};
    case Sound::KoTheirs:
        return {{Wave::Noise, 2000.0f, 100.0f, 0.0f, 0.4f, 0.7f}, {Wave::Square, 330.0f, 80.0f, 0.0f, 0.7f, 0.5f}};
    case Sound::Won: {
        std::vector<Note> notes = run(Wave::Square, {kC5, kE5, kG5}, 0.1f, 0.1f, 0.5f);
        notes.push_back({Wave::Square, kC6, kC6, 0.3f, 1.6f, 0.3f});
        notes.push_back({Wave::Triangle, 2 * kE5, 2 * kE5, 0.3f, 1.6f, 0.2f});
        notes.push_back({Wave::Triangle, 2 * kG5, 2 * kG5, 0.3f, 1.6f, 0.2f});
        notes.push_back({Wave::Triangle, kC3, kC3, 0.0f, 1.9f, 0.3f});
        return notes;
    }
    case Sound::Lost:
        return run(Wave::Triangle, {kA4, kG4, kE4, kD4, kC4, kA3}, 0.3f, 0.8f, 0.5f);
    case Sound::Draw: {
        std::vector<Note> notes = run(Wave::Triangle, {kE4, kE4, kD4}, 0.15f, 0.3f, 0.5f);
        const std::size_t phrase = notes.size();
        for (std::size_t i = 0; i < phrase; ++i) {
            Note repeat = notes[i];
            repeat.at += 0.6f;
            notes.push_back(repeat);
        }
        return notes;
    }
    case Sound::Count:
        break;
    }
    return {};
}

}  // namespace

std::vector<float> render(Sound sound, int sampleRate) {
    const std::vector<Note> notes = notesFor(sound);
    float end = 0.0f;
    for (const Note& note : notes) end = std::max(end, note.at + note.length);
    std::vector<float> out(static_cast<std::size_t>(std::ceil(end * static_cast<float>(sampleRate))), 0.0f);
    std::uint32_t noise = 0x12345678u;  // fixed seed, so every render is identical
    for (const Note& note : notes) {
        const int first = static_cast<int>(note.at * static_cast<float>(sampleRate));
        const int count = static_cast<int>(note.length * static_cast<float>(sampleRate));
        double phase = 0.0;
        float held = 0.0f;
        for (int i = 0; i < count && first + i < static_cast<int>(out.size()); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
            const float hz = note.startHz + (note.endHz - note.startHz) * t / note.length;
            const bool wrapped = advancePhase(phase, hz, sampleRate);
            float sample = 0.0f;
            switch (note.wave) {
            case Wave::Square:
                sample = squareWave(phase);
                break;
            case Wave::Triangle:
                sample = triangleWave(phase);
                break;
            case Wave::Noise:
                if (i == 0 || wrapped) {  // draw a new value each period
                    noise = noise * 1664525u + 1013904223u;
                    held = static_cast<float>(noise >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
                }
                sample = held;
                break;
            }
            const float envelope = t < kAttackS ? t / kAttackS : 1.0f - (t - kAttackS) / (note.length - kAttackS);
            out[static_cast<std::size_t>(first + i)] += sample * note.volume * envelope;
        }
    }
    for (float& s : out) s = std::clamp(s, -1.0f, 1.0f);
    return out;
}

}  // namespace tetris
