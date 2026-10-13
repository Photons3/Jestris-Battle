# Sound Effects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synthesized retro sound effects for solo play and duels, with a mute key, a menu toggle and a `MUTED` badge.

**Architecture:** `Game` and `DuelSession` queue events (`takeEvents()`, like `takeOutgoingAttack()`). A pure `tetris_audio` library renders every `Sound` once and mixes up to 8 voices. `sound_cues` maps events to sounds, and a thin `AudioOut` feeds the mixer to an SDL3 audio stream. Spec: `docs/superpowers/specs/2026-10-13-sound-effects-design.md`.

**Tech Stack:** C++20, SDL3 audio streams (`SDL_OpenAudioDeviceStream`), Dear ImGui, doctest, CMake, Emscripten for the web build.

## Global Constraints

- No audio files and no new dependencies. Every sound is generated in code.
- `tetris_core`, `tetris_audio` and `tetris_duel` must not include SDL. Only `src/app/audio_out.cpp` touches SDL audio.
- Mono float32 at 48000 Hz. Every sound is shorter than 1.5 s. Up to 8 voices, the oldest replaced first. Master gain 0.5. Output clamped to [-1, 1].
- If the audio device can't be opened, log one line to stderr and run silently. Never fail startup over audio.
- Mute: the `M` key (ignored while `ImGui::GetIO().WantTextInput`), a `Sound: On/Off` item on the start and pause menus, and a `MUTED` badge while muted. It lasts for the session only and starts unmuted.
- Match the surrounding code: `tetris` namespace, `kConstant` names, short `//` comments only where they say something non-obvious, `-Wall -Wextra -Wpedantic` clean.
- Build and test: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`. A single test: `./build/tetris_tests -tc="audio:*"`.
- Web build check: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web` (prefix with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14` if the system Python is older than 3.10).

## File map

| File | Responsibility |
|---|---|
| `src/audio/sound.hpp` (new) | `enum class Sound` |
| `src/audio/synth.{hpp,cpp}` (new) | Note tables and `render(Sound, sampleRate)` |
| `src/audio/mixer.{hpp,cpp}` (new) | The voice cache and the mix |
| `src/core/game.{hpp,cpp}` | `GameEvent` queue |
| `src/app/duel.{hpp,cpp}` | `DuelEvent` queue |
| `src/app/sound_cues.{hpp,cpp}` (new, in `tetris_duel`) | Event → `Sound` |
| `src/app/audio_out.{hpp,cpp}` (new) | SDL stream wrapper |
| `src/app/app.{hpp,cpp}` | Owns `AudioOut`, plays events each frame, mute toggle |
| `src/app/render.{hpp,cpp}` | Menu items and the badge |
| `tests/audio_test.cpp`, `tests/sound_cues_test.cpp` (new); `tests/game_test.cpp`, `tests/duel_test.cpp` | Tests |
| `CMakeLists.txt`, `README.md` | Build and docs |

---

### Task 1: `tetris_audio` synth

**Files:**
- Create: `src/audio/sound.hpp`, `src/audio/synth.hpp`, `src/audio/synth.cpp`, `tests/audio_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `enum class Sound { Move, Rotate, Hold, HardDrop, Lock, Clear1, Clear2, Clear3, Tetris, Bomb, Garbage, LevelUp, GameOver, CountdownTick, Go, Attacked, KoMine, KoTheirs, Won, Lost, Draw, Count }`; `inline constexpr int kSoundCount`; `inline constexpr int kMaxSoundMs = 1500`; `std::vector<float> render(Sound sound, int sampleRate)`; a CMake target `tetris_audio`.

- [ ] **Step 1: Add the build wiring**

In `CMakeLists.txt`, right after the `tetris_core` block, add:

```cmake
add_library(tetris_audio STATIC
  src/audio/synth.cpp)
target_include_directories(tetris_audio PUBLIC src)
target_compile_options(tetris_audio PRIVATE ${TETRIS_WARNINGS})
```

Add `tests/audio_test.cpp` to the `tetris_tests` source list, and change its link line to:

```cmake
target_link_libraries(tetris_tests PRIVATE tetris_input tetris_duel tetris_audio doctest::doctest)
```

- [ ] **Step 2: Write `src/audio/sound.hpp`**

```cpp
#pragma once

namespace tetris {

enum class Sound {
    Move, Rotate, Hold, HardDrop, Lock, Clear1, Clear2, Clear3, Tetris, Bomb, Garbage, LevelUp, GameOver,
    CountdownTick, Go, Attacked, KoMine, KoTheirs, Won, Lost, Draw,
    Count  // not a sound: the number of sounds
};

inline constexpr int kSoundCount = static_cast<int>(Sound::Count);

}  // namespace tetris
```

- [ ] **Step 3: Write `src/audio/synth.hpp`**

```cpp
#pragma once

#include <vector>

#include "audio/sound.hpp"

namespace tetris {

inline constexpr int kMaxSoundMs = 1500;

// Mono samples in [-1, 1]. Deterministic: the same sound always renders the same samples.
std::vector<float> render(Sound sound, int sampleRate);

}  // namespace tetris
```

- [ ] **Step 4: Write the failing test `tests/audio_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/synth.hpp"

using namespace tetris;

namespace {

constexpr int kRate = 48000;

float peakOf(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (float s : samples) peak = std::max(peak, std::abs(s));
    return peak;
}

}  // namespace

TEST_CASE("audio: every sound renders a short, audible, in-range buffer") {
    for (int i = 0; i < kSoundCount; ++i) {
        CAPTURE(i);
        const std::vector<float> samples = render(static_cast<Sound>(i), kRate);
        REQUIRE_FALSE(samples.empty());
        CHECK(samples.size() < static_cast<std::size_t>(kRate * kMaxSoundMs / 1000));
        CHECK(peakOf(samples) <= 1.0f);
        CHECK(peakOf(samples) > 0.05f);
    }
}

TEST_CASE("audio: rendering is deterministic") {
    for (int i = 0; i < kSoundCount; ++i) {
        CAPTURE(i);
        CHECK(render(static_cast<Sound>(i), kRate) == render(static_cast<Sound>(i), kRate));
    }
}
```

- [ ] **Step 5: Run it and check that it fails**

Run: `cmake -B build && cmake --build build`
Expected: a link error, `undefined reference/symbol ... tetris::render` (there's no `synth.cpp` yet; create an empty file first if CMake complains it's missing).

- [ ] **Step 6: Write `src/audio/synth.cpp`**

```cpp
#include "audio/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace tetris {
namespace {

enum class Wave { Square, Triangle, Noise };

struct Note {
    Wave wave;
    float startHz;
    float endHz;   // the pitch slides linearly from startHz; for Noise it is how often a new random value is drawn
    float at;      // seconds from the start of the sound
    float length;  // seconds
    float volume;
};

constexpr float kAttackS = 0.005f;

constexpr float kC4 = 261.63f, kE4 = 329.63f, kG4 = 392.0f, kA4 = 440.0f;
constexpr float kC5 = 523.25f, kE5 = 659.26f, kG5 = 783.99f, kC6 = 1046.5f;

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
    case Sound::GameOver:
        return {{Wave::Square, kA4, 110.0f, 0.0f, 0.8f, 0.5f}};
    case Sound::CountdownTick:
        return {{Wave::Square, kA4, kA4, 0.0f, 0.08f, 0.3f}};
    case Sound::Go:
        return {{Wave::Square, 2 * kA4, 2 * kA4, 0.0f, 0.2f, 0.4f}};
    case Sound::Attacked:
        return {{Wave::Square, 220.0f, 220.0f, 0.0f, 0.05f, 0.4f}, {Wave::Square, 220.0f, 220.0f, 0.08f, 0.05f, 0.4f}};
    case Sound::KoMine:
        return {{Wave::Square, kC5, kC6, 0.0f, 0.15f, 0.7f}, {Wave::Triangle, kC6, kC6, 0.15f, 0.2f, 0.7f}};
    case Sound::KoTheirs:
        return {{Wave::Noise, 1000.0f, 100.0f, 0.0f, 0.2f, 0.6f}, {Wave::Square, 330.0f, 110.0f, 0.0f, 0.3f, 0.6f}};
    case Sound::Won:
        return run(Wave::Triangle, {kC5, kE5, kG5, kC6}, 0.1f, 0.3f, 0.7f);
    case Sound::Lost:
        return run(Wave::Triangle, {kG4, kE4, kC4}, 0.15f, 0.4f, 0.5f);
    case Sound::Draw:
        return run(Wave::Triangle, {kE4, kE4}, 0.15f, 0.15f, 0.5f);
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
            const double before = phase;
            phase += hz / sampleRate;
            phase -= std::floor(phase);
            float sample = 0.0f;
            switch (note.wave) {
            case Wave::Square:
                sample = phase < 0.5 ? 1.0f : -1.0f;
                break;
            case Wave::Triangle:
                sample = 4.0f * std::abs(static_cast<float>(phase) - 0.5f) - 1.0f;
                break;
            case Wave::Noise:
                if (i == 0 || phase < before) {  // the phase wrapped: draw a new value
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
```

- [ ] **Step 7: Run the tests and check that they pass**

Run: `cmake --build build && ./build/tetris_tests -tc="audio:*"`
Expected: `2 passed`. Then run the full `ctest --test-dir build --output-on-failure` and confirm everything passes.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/audio tests/audio_test.cpp
git commit -m "feat(audio): synthesize retro sound effects"
```

---

### Task 2: `Mixer`

**Files:**
- Create: `src/audio/mixer.hpp`, `src/audio/mixer.cpp`
- Modify: `CMakeLists.txt` (add `src/audio/mixer.cpp` to `tetris_audio`), `tests/audio_test.cpp`

**Interfaces:**
- Consumes: `render(Sound, int)`, `kSoundCount`.
- Produces: `class Mixer { explicit Mixer(int sampleRate); void play(Sound); void mix(float* out, int frames); void setMuted(bool); bool muted() const; int activeVoices() const; }`, `kMaxVoices = 8`, `kMasterGain = 0.5f`.

- [ ] **Step 1: Write the failing tests** (append to `tests/audio_test.cpp` and add `#include "audio/mixer.hpp"` at the top)

```cpp
namespace {

std::vector<float> mixFrames(Mixer& mixer, int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames), 7.0f);  // junk, to prove mix overwrites
    mixer.mix(out.data(), frames);
    return out;
}

}  // namespace

TEST_CASE("audio: a mixer with no voices writes silence") {
    Mixer mixer(kRate);
    const std::vector<float> out = mixFrames(mixer, 256);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("audio: voices add up at the master gain") {
    Mixer one(kRate);
    one.play(Sound::Move);
    Mixer two(kRate);
    two.play(Sound::Move);
    two.play(Sound::Move);
    const std::vector<float> a = mixFrames(one, 512);
    const std::vector<float> b = mixFrames(two, 512);
    const std::vector<float> raw = render(Sound::Move, kRate);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i] == doctest::Approx(raw[i] * kMasterGain));
        CHECK(b[i] == doctest::Approx(2.0f * a[i]));
    }
}

TEST_CASE("audio: loud overlapping voices are clamped to [-1, 1]") {
    Mixer mixer(kRate);
    for (int i = 0; i < 4; ++i) mixer.play(Sound::Bomb);
    const std::vector<float> out = mixFrames(mixer, kRate / 10);
    CHECK(peakOf(out) == doctest::Approx(1.0f));
}

TEST_CASE("audio: finished voices are removed") {
    Mixer mixer(kRate);
    mixer.play(Sound::Move);
    CHECK(mixer.activeVoices() == 1);
    mixFrames(mixer, kRate / 10);  // Move lasts 25 ms
    CHECK(mixer.activeVoices() == 0);
}

TEST_CASE("audio: a ninth voice replaces the oldest") {
    Mixer mixer(kRate);
    mixer.play(Sound::Tetris);  // long; it would still be playing after 100 ms
    for (int i = 0; i < kMaxVoices; ++i) mixer.play(Sound::Move);
    CHECK(mixer.activeVoices() == kMaxVoices);
    mixFrames(mixer, kRate / 10);
    CHECK(mixer.activeVoices() == 0);
}

TEST_CASE("audio: muting silences, drops playing voices and ignores play") {
    Mixer mixer(kRate);
    mixer.play(Sound::Tetris);
    mixer.setMuted(true);
    CHECK(mixer.muted());
    CHECK(mixer.activeVoices() == 0);
    mixer.play(Sound::Tetris);
    CHECK(mixer.activeVoices() == 0);
    CHECK(peakOf(mixFrames(mixer, 256)) == 0.0f);
    mixer.setMuted(false);
    mixer.play(Sound::Tetris);
    CHECK(mixer.activeVoices() == 1);
}
```

- [ ] **Step 2: Run them and check that they fail**

Run: `cmake --build build`
Expected: a compile error, `audio/mixer.hpp: No such file`.

- [ ] **Step 3: Write `src/audio/mixer.hpp`**

```cpp
#pragma once

#include <cstddef>
#include <vector>

#include "audio/sound.hpp"

namespace tetris {

inline constexpr int kMaxVoices = 8;
inline constexpr float kMasterGain = 0.5f;

// Plays cached sounds on up to kMaxVoices voices. Not thread-safe: AudioOut serialises calls.
class Mixer {
public:
    explicit Mixer(int sampleRate);  // renders every sound once

    void play(Sound sound);            // ignored while muted; replaces the oldest voice when all are busy
    void mix(float* out, int frames);  // overwrites `out` with the next `frames` samples
    void setMuted(bool muted);         // muting also stops what is playing
    bool muted() const { return muted_; }
    int activeVoices() const { return static_cast<int>(voices_.size()); }

private:
    struct Voice {
        int sound;
        std::size_t pos;
    };

    std::vector<std::vector<float>> cache_;  // indexed by Sound
    std::vector<Voice> voices_;              // oldest first
    bool muted_ = false;
};

}  // namespace tetris
```

- [ ] **Step 4: Write `src/audio/mixer.cpp`** and add it to `tetris_audio` in `CMakeLists.txt`

```cpp
#include "audio/mixer.hpp"

#include <algorithm>

#include "audio/synth.hpp"

namespace tetris {

Mixer::Mixer(int sampleRate) {
    cache_.reserve(kSoundCount);
    for (int i = 0; i < kSoundCount; ++i) cache_.push_back(render(static_cast<Sound>(i), sampleRate));
    voices_.reserve(kMaxVoices);
}

void Mixer::play(Sound sound) {
    if (muted_) return;
    if (static_cast<int>(voices_.size()) == kMaxVoices) voices_.erase(voices_.begin());
    voices_.push_back({static_cast<int>(sound), 0});
}

void Mixer::mix(float* out, int frames) {
    std::fill(out, out + frames, 0.0f);
    for (Voice& voice : voices_) {
        const std::vector<float>& samples = cache_[static_cast<std::size_t>(voice.sound)];
        const std::size_t n = std::min(static_cast<std::size_t>(frames), samples.size() - voice.pos);
        for (std::size_t i = 0; i < n; ++i) out[i] += samples[voice.pos + i] * kMasterGain;
        voice.pos += n;
    }
    std::erase_if(voices_, [this](const Voice& v) { return v.pos >= cache_[static_cast<std::size_t>(v.sound)].size(); });
    for (int i = 0; i < frames; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void Mixer::setMuted(bool muted) {
    muted_ = muted;
    if (muted_) voices_.clear();
}

}  // namespace tetris
```

`std::erase_if` on a vector needs `<vector>` (C++20), which `mixer.hpp` already includes.

- [ ] **Step 5: Run the tests and check that they pass**

Run: `cmake --build build && ./build/tetris_tests -tc="audio:*"`
Expected: `8 passed`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/audio tests/audio_test.cpp
git commit -m "feat(audio): mix up to eight voices with mute"
```

---

### Task 3: `GameEvent` queue in `Game`

**Files:**
- Modify: `src/core/game.hpp`, `src/core/game.cpp`, `tests/game_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class GameEventType { Moved, Rotated, Held, HardDropped, Locked, LinesCleared, BombExploded, GarbageRose, LevelUp, ToppedOut };
  struct GameEvent { GameEventType type; int lines = 0; friend bool operator==(const GameEvent&, const GameEvent&) = default; };
  std::vector<GameEvent> Game::takeEvents();
  ```

- [ ] **Step 1: Write the failing tests** (append to the **end** of `tests/game_test.cpp`, after the bomb tests, so the `bombBoard` helper is in scope)

```cpp
namespace {

using Events = std::vector<GameEvent>;
constexpr GameEvent kMoved{GameEventType::Moved};
constexpr GameEvent kRotated{GameEventType::Rotated};
constexpr GameEvent kHeld{GameEventType::Held};
constexpr GameEvent kHardDropped{GameEventType::HardDropped};
constexpr GameEvent kLocked{GameEventType::Locked};
constexpr GameEvent kBombExploded{GameEventType::BombExploded};
constexpr GameEvent kGarbageRose{GameEventType::GarbageRose};
constexpr GameEvent kLevelUp{GameEventType::LevelUp};
constexpr GameEvent kToppedOut{GameEventType::ToppedOut};
constexpr GameEvent cleared(int lines) { return {GameEventType::LinesCleared, lines}; }

}  // namespace

TEST_CASE("game events: moves, rotations and holds are reported only when they happen") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::RotateCW);
    game.apply(Action::Hold);
    game.apply(Action::Hold);  // refused until the next lock
    CHECK(game.takeEvents() == Events{kRotated, kHeld});
    CHECK(game.takeEvents().empty());
}

TEST_CASE("game events: a blocked move is silent") {
    Game game(seedWithFirst(PieceType::T), 1);
    for (int i = 0; i < 5; ++i) game.apply(Action::Left);  // only 3 columns to the wall
    CHECK(game.takeEvents() == Events{kMoved, kMoved, kMoved});
}

TEST_CASE("game events: a hard drop reports the drop, not a lock") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped});
}

TEST_CASE("game events: a hard-dropped tetris reports four full rows at once") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    CHECK(game.takeEvents() == Events{kRotated, kHardDropped, cleared(4)});
    game.tick(kClearDelayMs);
    CHECK(game.takeEvents().empty());
}

TEST_CASE("game events: a piece that locks on its own reports Locked") {
    Game game(seedWithFirst(PieceType::T), 1);
    dropToFloor(game);
    CHECK(game.takeEvents().empty());
    game.tick(kLockDelayMs);
    CHECK(game.takeEvents() == Events{kLocked});
}

TEST_CASE("game events: bomb rows are reported apart from full rows") {
    SUBCASE("bombs only") {
        Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, kBombExploded});
    }
    SUBCASE("bombs and full rows") {
        Game game(seedWithFirst(PieceType::O), 1, bombBoard(1, 4, 2));
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, cleared(2), kBombExploded});
    }
}

TEST_CASE("game events: garbage rising after a lock is reported") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(2);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, kGarbageRose});
}

TEST_CASE("game events: reaching the next level is reported when the clear finishes") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(10, 4, 0));  // ten bomb rows clear as ten lines
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, kBombExploded});
    game.tick(kClearDelayMs);
    CHECK(game.level() == 2);
    CHECK(game.takeEvents() == Events{kLevelUp});
}

TEST_CASE("game events: every kind of top-out is reported once") {
    SUBCASE("block-out") {
        Board board;
        board.set(4, 20, PieceType::Z);
        Game game(seedWithFirst(PieceType::T), 1, board);
        CHECK(game.takeEvents() == Events{kToppedOut});
    }
    SUBCASE("lock-out") {
        Board board;
        for (int y = 0; y < kVisibleHeight; ++y)
            for (int x = 0; x < kBoardWidth - 1; ++x) board.set(x, y, PieceType::Z);
        Game game(seedWithFirst(PieceType::T), 1, board);
        game.apply(Action::HardDrop);
        CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
    }
    SUBCASE("garbage overflow") {
        Game game(seedWithFirst(PieceType::T), 1);
        game.receiveGarbage(kBoardHeight - 1);
        game.apply(Action::HardDrop);
        game.tick(1000);
        CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
    }
}

TEST_CASE("game events: resetting the board keeps events not yet taken") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    game.resetBoard();
    CHECK(game.takeEvents() == Events{kHardDropped, kToppedOut});
}
```

- [ ] **Step 2: Run them and check that they fail**

Run: `cmake --build build`
Expected: a compile error, `'GameEvent' was not declared` / `no member named 'takeEvents'`.

- [ ] **Step 3: Add the types and members to `src/core/game.hpp`**

After the `Action` enum:

```cpp
enum class GameEventType { Moved, Rotated, Held, HardDropped, Locked, LinesCleared, BombExploded, GarbageRose, LevelUp, ToppedOut };

// Something that just happened in a game, for sound. Locked is only for a piece that locks on its own.
struct GameEvent {
    GameEventType type;
    int lines = 0;  // LinesCleared only: full rows, 1-4; bomb rows are reported as BombExploded
    friend bool operator==(const GameEvent&, const GameEvent&) = default;
};
```

In `class Game`'s public section, after `takeOutgoingAttack()`:

```cpp
    // Events since the last call, oldest first; empties the list. resetBoard() leaves it alone.
    std::vector<GameEvent> takeEvents() { return std::exchange(events_, {}); }
```

Add `#include <utility>` to the includes. In the private section, change `void lockPiece();` to `void lockPiece(bool hardDropped = false);` and add:

```cpp
    void emit(GameEventType type, int lines = 0) { events_.push_back({type, lines}); }
    void topOut();
```

and the member `std::vector<GameEvent> events_;` after `outgoingAttack_`.

- [ ] **Step 4: Emit the events in `src/core/game.cpp`**

Add `topOut()` next to `spawn()`:

```cpp
void Game::topOut() {
    state_ = GameState::GameOver;
    emit(GameEventType::ToppedOut);
}
```

`spawn()`: replace `if (board_.collides(active_)) state_ = GameState::GameOver;  // block-out` with
`if (board_.collides(active_)) topOut();  // block-out`.

`applyGarbage()`:

```cpp
bool Game::applyGarbage() {
    if (pendingGarbage_ == 0) return true;
    const int bombX = static_cast<int>(garbageRng_() % kBoardWidth);
    if (!board_.pushGarbage(std::exchange(pendingGarbage_, 0), bombX)) return false;
    emit(GameEventType::GarbageRose);
    return true;
}
```

`apply()`, the Left and Right cases:

```cpp
    case Action::Left:
        if (tryMove(-1, 0)) {
            onPlayerMoved();
            emit(GameEventType::Moved);
        }
        break;
    case Action::Right:
        if (tryMove(1, 0)) {
            onPlayerMoved();
            emit(GameEventType::Moved);
        }
        break;
```

`rotate()`: add `emit(GameEventType::Rotated);` after `onPlayerMoved();` inside the `if`.

`hardDrop()`:

```cpp
void Game::hardDrop() {
    const int distance = ghostDistance();
    active_.pos.y -= distance;
    score_ += 2 * distance;
    emit(GameEventType::HardDropped);
    lockPiece(true);
}
```

`holdPiece()`: add `emit(GameEventType::Held);` right after `if (!canHold_) return;`.

`lockPiece()`, full replacement:

```cpp
void Game::lockPiece(bool hardDropped) {
    board_.lock(active_);
    canHold_ = true;
    const auto cells = active_.cells();
    const bool lockOut =
        std::all_of(cells.begin(), cells.end(), [](Point c) { return c.y >= kVisibleHeight; });
    if (lockOut) {
        topOut();
        return;
    }
    const std::vector<int> full = board_.fullRows();
    const std::vector<int> exploded = board_.bombRowsHitBy(active_);  // bomb rows are never full, so no overlap
    if (!full.empty()) emit(GameEventType::LinesCleared, static_cast<int>(full.size()));
    if (!exploded.empty()) emit(GameEventType::BombExploded);
    clearingRows_ = full;
    clearingRows_.insert(clearingRows_.end(), exploded.begin(), exploded.end());
    std::sort(clearingRows_.begin(), clearingRows_.end());
    if (!clearingRows_.empty()) {
        state_ = GameState::Clearing;
        clearTimerMs_ = 0;
        return;
    }
    if (!hardDropped) emit(GameEventType::Locked);
    if (!applyGarbage()) {
        topOut();
        return;
    }
    spawn(bag_.next());
}
```

`finishClear()`: capture `const int levelBefore = level_;` at the top, and after `level_ = levelFor(startLevel_, lines_);` add `if (level_ > levelBefore) emit(GameEventType::LevelUp);`.

`step()` keeps calling `lockPiece()` with no argument (a natural lock).

- [ ] **Step 5: Run the tests and check that they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all tests pass, including the existing game and duel tests.

- [ ] **Step 6: Commit**

```bash
git add src/core/game.hpp src/core/game.cpp tests/game_test.cpp
git commit -m "feat(core): report game events for sound"
```

---

### Task 4: `DuelEvent` queue in `DuelSession`

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`, `tests/duel_test.cpp`

**Interfaces:**
- Consumes: `Game::takeEvents()` (Task 3), only in a test.
- Produces: `enum class DuelEvent { CountdownTick, MatchStart, Attacked, KoMine, Won, Lost, Draw };` and `std::vector<DuelEvent> DuelSession::takeEvents();`

- [ ] **Step 1: Write the failing tests** (append to `tests/duel_test.cpp`)

```cpp
TEST_CASE("duel events: the countdown ticks each second, then the match starts") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("START 7 1");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "3"
    duel.update(1000);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "2"
    duel.update(999);
    CHECK(duel.takeEvents().empty());
    duel.update(1);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::CountdownTick});  // "1"
    duel.update(1000);
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::MatchStart});
}

TEST_CASE("duel events: incoming garbage and our KOs are reported, the opponent's KOs are not") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.takeEvents();
    duel.onMessage("ATK 3");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::Attacked});
    duel.onMessage("KO 1 0");
    CHECK(duel.takeEvents() == std::vector<DuelEvent>{DuelEvent::KoMine});
    duel.onMessage("KO 1 1");
    CHECK(duel.takeEvents().empty());
}

TEST_CASE("duel events: each result is reported, and an opponent leaving is silent") {
    for (const auto& c : {std::pair{"OVER WIN KO 3 1", DuelEvent::Won},
                          std::pair{"OVER LOSE TIME 2 5", DuelEvent::Lost},
                          std::pair{"OVER DRAW TIME 4 4", DuelEvent::Draw}}) {
        CAPTURE(c.first);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.takeEvents();
        duel.onMessage(c.first);
        CHECK(duel.takeEvents() == std::vector<DuelEvent>{c.second});
    }
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.takeEvents();
    duel.onMessage("LEFT");
    CHECK(duel.outcome() == DuelOutcome::OpponentLeft);
    CHECK(duel.takeEvents().empty());
}

TEST_CASE("duel events: our top-out survives the KO board reset") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    CHECK(duel.game()->takeEvents() ==
          std::vector<GameEvent>{{GameEventType::HardDropped}, {GameEventType::ToppedOut}});
}
```

Add `#include <utility>` to the includes of `tests/duel_test.cpp` if it isn't there.

- [ ] **Step 2: Run them and check that they fail**

Run: `cmake --build build`
Expected: a compile error, `'DuelEvent' was not declared`.

- [ ] **Step 3: Add the type and members to `src/app/duel.hpp`**

After the `DuelOutcome` enum:

```cpp
// Something that just happened in the duel, for sound. Our own top-out is reported by the Game.
enum class DuelEvent { CountdownTick, MatchStart, Attacked, KoMine, Won, Lost, Draw };
```

Public, after `bool frozen() const;`:

```cpp
    // Events since the last call, oldest first; empties the list.
    std::vector<DuelEvent> takeEvents() { return std::exchange(events_, {}); }
```

Add `#include <vector>` to the includes (`<utility>` is already there). Private member, after `koFlashMs_`: `std::vector<DuelEvent> events_;`.

- [ ] **Step 4: Emit the events in `src/app/duel.cpp`**

`onMessage`, the `Attack` branch: after `sentTheirs_ += attack->lines;` add `events_.push_back(DuelEvent::Attacked);`.

`onMessage`, the `KoCounts` branch:

```cpp
        if (inMatch()) {
            if (kos->mine > kosMine_) events_.push_back(DuelEvent::KoMine);
            kosMine_ = kos->mine;
            kosTheirs_ = kos->theirs;
        }
```

`update`, the countdown block:

```cpp
    if (phase_ == DuelPhase::Countdown) {
        const int before = countdownMs_;
        countdownMs_ -= ms;
        if (countdownMs_ <= 0) {
            countdownMs_ = 0;
            phase_ = DuelPhase::Playing;
            events_.push_back(DuelEvent::MatchStart);
        } else if ((before - 1) / 1000 != (countdownMs_ - 1) / 1000) {  // the shown second changed
            events_.push_back(DuelEvent::CountdownTick);
        }
        return;
    }
```

`startMatch`: add `events_.push_back(DuelEvent::CountdownTick);` at the end.

`finish`:

```cpp
void DuelSession::finish(DuelOutcome outcome) {
    phase_ = DuelPhase::Result;
    outcome_ = outcome;
    if (outcome == DuelOutcome::Won) events_.push_back(DuelEvent::Won);
    if (outcome == DuelOutcome::Lost) events_.push_back(DuelEvent::Lost);
    if (outcome == DuelOutcome::Draw) events_.push_back(DuelEvent::Draw);
}
```

- [ ] **Step 5: Run the tests and check that they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/app/duel.hpp src/app/duel.cpp tests/duel_test.cpp
git commit -m "feat(duel): report duel events for sound"
```

---

### Task 5: Event-to-sound cues

**Files:**
- Create: `src/app/sound_cues.hpp`, `src/app/sound_cues.cpp`, `tests/sound_cues_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `GameEvent`/`GameEventType` (Task 3), `DuelEvent` (Task 4), `Sound` (Task 1).
- Produces: `Sound soundFor(const GameEvent& event, bool inDuel);` and `Sound soundFor(DuelEvent event);`. Every event has a sound, so these return `Sound` rather than the `std::optional<Sound>` the spec mentions.

- [ ] **Step 1: Wire the build**

In `CMakeLists.txt`: add `src/app/sound_cues.cpp` to `tetris_duel`, change its link line to `target_link_libraries(tetris_duel PUBLIC tetris_core tetris_audio)`, and add `tests/sound_cues_test.cpp` to `tetris_tests`.

- [ ] **Step 2: Write the failing test `tests/sound_cues_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include "app/sound_cues.hpp"

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
    CHECK(soundFor(GameEvent{GameEventType::ToppedOut}, false) == Sound::GameOver);
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
```

- [ ] **Step 3: Run it and check that it fails**

Run: `cmake -B build && cmake --build build`
Expected: a compile error, `app/sound_cues.hpp: No such file`.

- [ ] **Step 4: Write `src/app/sound_cues.hpp`**

```cpp
#pragma once

#include "app/duel.hpp"
#include "audio/sound.hpp"
#include "core/game.hpp"

namespace tetris {

// In a duel a top-out is a KO against us, not the end of the game.
Sound soundFor(const GameEvent& event, bool inDuel);
Sound soundFor(DuelEvent event);

}  // namespace tetris
```

- [ ] **Step 5: Write `src/app/sound_cues.cpp`**

```cpp
#include "app/sound_cues.hpp"

namespace tetris {

Sound soundFor(const GameEvent& event, bool inDuel) {
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
        return event.lines >= 4 ? Sound::Tetris : event.lines == 3 ? Sound::Clear3
                                              : event.lines == 2   ? Sound::Clear2
                                                                   : Sound::Clear1;
    case GameEventType::BombExploded:
        return Sound::Bomb;
    case GameEventType::GarbageRose:
        return Sound::Garbage;
    case GameEventType::LevelUp:
        return Sound::LevelUp;
    case GameEventType::ToppedOut:
        return inDuel ? Sound::KoTheirs : Sound::GameOver;
    }
    return Sound::Move;
}

Sound soundFor(DuelEvent event) {
    switch (event) {
    case DuelEvent::CountdownTick:
        return Sound::CountdownTick;
    case DuelEvent::MatchStart:
        return Sound::Go;
    case DuelEvent::Attacked:
        return Sound::Attacked;
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

}  // namespace tetris
```

- [ ] **Step 6: Run the tests and check that they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all tests pass, including 3 new `cues:` cases.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/app/sound_cues.hpp src/app/sound_cues.cpp tests/sound_cues_test.cpp
git commit -m "feat(app): map game and duel events to sounds"
```

---

### Task 6: `AudioOut` and playing events each frame

**Files:**
- Create: `src/app/audio_out.hpp`, `src/app/audio_out.cpp`
- Modify: `src/app/app.hpp`, `src/app/app.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `Mixer` (Task 2), `soundFor` (Task 5), `Game::takeEvents`, `DuelSession::takeEvents`.
- Produces: `class AudioOut { AudioOut(); ~AudioOut(); void play(Sound); void setMuted(bool); }` and `App::audio`. Task 7 calls `app.audio.setMuted`.

There's no automated test here (SDL device I/O). The check is a build plus listening.

- [ ] **Step 1: Write `src/app/audio_out.hpp`**

```cpp
#pragma once

#include <SDL3/SDL.h>

#include <vector>

#include "audio/mixer.hpp"

namespace tetris {

inline constexpr int kAudioSampleRate = 48000;

// Plays sound effects on the default output device. If none can be opened it logs once and stays silent.
class AudioOut {
public:
    AudioOut();  // needs SDL_Init to have run
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;  // SDL holds `this` for the callback
    AudioOut& operator=(const AudioOut&) = delete;

    void play(Sound sound);
    void setMuted(bool muted);

private:
    static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additionalBytes, int totalBytes);

    Mixer mixer_{kAudioSampleRate};
    std::vector<float> buffer_;  // reused by the callback
    SDL_AudioStream* stream_ = nullptr;
};

}  // namespace tetris
```

- [ ] **Step 2: Write `src/app/audio_out.cpp`**

```cpp
#include "app/audio_out.hpp"

#include <cstdio>

namespace tetris {

AudioOut::AudioOut() {
    buffer_.reserve(4096);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "Sound off: %s\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kAudioSampleRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioOut::feed, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
        std::fprintf(stderr, "Sound off: %s\n", SDL_GetError());
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

AudioOut::~AudioOut() {
    if (stream_ == nullptr) return;
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::play(Sound sound) {
    if (stream_ == nullptr) return;
    SDL_LockAudioStream(stream_);  // SDL holds this lock while it runs feed()
    mixer_.play(sound);
    SDL_UnlockAudioStream(stream_);
}

void AudioOut::setMuted(bool muted) {
    if (stream_ == nullptr) return;
    SDL_LockAudioStream(stream_);
    mixer_.setMuted(muted);
    SDL_UnlockAudioStream(stream_);
}

void SDLCALL AudioOut::feed(void* userdata, SDL_AudioStream* stream, int additionalBytes, int) {
    auto& self = *static_cast<AudioOut*>(userdata);
    const int frames = additionalBytes / static_cast<int>(sizeof(float));
    if (frames <= 0) return;
    self.buffer_.resize(static_cast<std::size_t>(frames));
    self.mixer_.mix(self.buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, self.buffer_.data(), frames * static_cast<int>(sizeof(float)));
}

}  // namespace tetris
```

- [ ] **Step 3: Add the files to the build**

In `CMakeLists.txt`, add `src/app/audio_out.cpp` to `add_executable(tetris ...)` (after `src/app/render.cpp`) and add `tetris_audio` to its `target_link_libraries`.

- [ ] **Step 4: Own `AudioOut` in `App` and play events**

`src/app/app.hpp`: add `#include "app/audio_out.hpp"`, and the member `AudioOut audio;` after `InputMapper input;`.

`src/app/app.cpp`: add `#include "app/sound_cues.hpp"`. In the anonymous namespace, before `handleKey`:

```cpp
// Plays what the solo or duel game and the duel session reported this frame.
void playEvents(App& app) {
    const bool inDuel = app.screen == Screen::Duel;
    Game* game = inDuel ? app.duel.game() : (app.game ? &*app.game : nullptr);
    if (game != nullptr)
        for (const GameEvent& event : game->takeEvents()) app.audio.play(soundFor(event, inDuel));
    for (DuelEvent event : app.duel.takeEvents()) app.audio.play(soundFor(event));
}
```

At the end of `step()`, after `if (app.screen == Screen::Duel) app.duel.update(ms);`, add `playEvents(app);`.

`App` is created inside `Host` after `SDL_Init` in `main.cpp` and destroyed before SDL quits, so nothing in `main.cpp` changes.

- [ ] **Step 5: Build and run the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: everything builds without warnings and all tests pass.

- [ ] **Step 6: Listen on the desktop**

Run: `./build/tetris`
Expected: move, rotate, hold, hard drop, natural lock, line clears (try a double and a tetris), level-up after 10 lines, and the game-over slide all make the sounds from the spec. Nothing crackles while holding left to auto-repeat. If a sound is clearly wrong (for example far too loud), adjust its row in `notesFor` and re-run `./build/tetris_tests -tc="audio:*"`.

- [ ] **Step 7: Check the web build and a duel**

Run: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`, then `go -C server run .` and open the printed URL in two browser tabs.
Expected: the build succeeds; after the first key press the sounds play; a duel plays the countdown ticks, "go", attack warnings, KO sounds and a result jingle; leaving one tab plays nothing on the other.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/app/audio_out.hpp src/app/audio_out.cpp src/app/app.hpp src/app/app.cpp
git commit -m "feat(app): play sound effects through SDL audio"
```

---

### Task 7: Mute key, menu toggle, badge and README

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.hpp`, `src/app/app.cpp`, `README.md`

**Interfaces:**
- Consumes: `AudioOut::setMuted` (Task 6).
- Produces: `UiCommand::ToggleSound`; `StartMenu::muted`; `drawOverlay(const Game&, const Fonts&, bool muted)`; `void drawMutedBadge(const Fonts&)`; `void App::toggleSound()`.

UI drawing has no automated test here (`ui_test.cpp` only checks IO config). Verification is a build plus a manual check.

- [ ] **Step 1: Extend `src/app/render.hpp`**

Add `ToggleSound` to the end of `enum class UiCommand`. Add to `StartMenu`, after `error`:

```cpp
    bool muted = false;                       // sound effects off for this session
```

Change the `drawOverlay` declaration and add the badge:

```cpp
// Pause and game-over menus; draws nothing while playing.
UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted);
// A small MUTED label in the top-right corner, over every screen.
void drawMutedBadge(const Fonts& fonts);
```

- [ ] **Step 2: Draw the items and the badge in `src/app/render.cpp`**

In the anonymous namespace, after `menuButton`:

```cpp
// The ### keeps one ImGui ID while the label flips, so keyboard focus stays on the item.
bool soundButton(bool muted) { return menuButton(muted ? "Sound: Off###sound" : "Sound: On###sound"); }
```

`drawStartScreen`: right after the `Solo`/`Play` button line, add:

```cpp
    if (soundButton(menu.muted)) command = UiCommand::ToggleSound;
```

`drawOverlay`: change the signature to `UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted)` and in the paused branch, after the `Restart` button, add:

```cpp
        if (soundButton(muted)) command = UiCommand::ToggleSound;
```

After `drawOverlay`, add:

```cpp
void drawMutedBadge(const Fonts& fonts) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float size = ImGui::GetFontSize();
    const char* text = "MUTED";
    const float width = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - width - size, viewport->WorkPos.y + size);
    drawLabel(ImGui::GetForegroundDrawList(), fonts, size, pos, text);
}
```

(`<cfloat>` is already included for `FLT_MAX`.)

- [ ] **Step 3: Handle the toggle in `src/app/app.hpp` / `app.cpp`**

`app.hpp`: add `void toggleSound();  // flips mute for the session` after `closeDuel()`.

`app.cpp`: add `#include "imgui.h"`, then:

```cpp
void App::toggleSound() {
    menu.muted = !menu.muted;
    audio.setMuted(menu.muted);
}
```

In `applyCommand`, add:

```cpp
    case UiCommand::ToggleSound:
        app.toggleSound();
        break;
```

In `handleKey`, after the fullscreen block and before the `if (key.repeat || app.activeGame() == nullptr) return;` line:

```cpp
    // M mutes on any screen, but not while typing a room code.
    if (key.down && !key.repeat && key.scancode == SDL_SCANCODE_M && !ImGui::GetIO().WantTextInput) {
        app.toggleSound();
        return;
    }
```

In `drawFrame`, change `command = drawOverlay(*app.game, fonts);` to `command = drawOverlay(*app.game, fonts, app.menu.muted);`, and before `applyCommand(app, command);` add:

```cpp
    if (app.menu.muted) drawMutedBadge(fonts);
```

- [ ] **Step 4: Document the key in `README.md`**

Add this section after "Web build" and before "Hosting LAN duels":

```markdown
## Controls

Arrows move and soft-drop, Up or X rotates clockwise, Z or Ctrl rotates counter-clockwise, Space hard-drops,
C or Shift holds, Esc or P pauses, M mutes the sound, and Ctrl+Cmd+F or Alt+Enter toggles fullscreen.
```

- [ ] **Step 5: Build and run the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: everything builds without warnings and all tests pass.

- [ ] **Step 6: Check by hand on the desktop and the web**

Run: `./build/tetris`, and the web build as in Task 6 Step 7.
Expected: `M` toggles mute on the start screen, during play and in a duel, and `MUTED` shows in the top-right corner while muted. The start menu and pause menu show `Sound: On/Off`, and selecting it with Enter or the mouse toggles it and keeps keyboard focus on the item. In the browser, typing `M` into the room-code box enters an `M` and doesn't mute. Unmuting doesn't replay old sounds.

- [ ] **Step 7: Commit**

```bash
git add src/app/render.hpp src/app/render.cpp src/app/app.hpp src/app/app.cpp README.md
git commit -m "feat(app): mute key, sound menu toggle and MUTED badge"
```
