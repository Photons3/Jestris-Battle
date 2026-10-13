# Combos, Jingles and Music Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:**
- A standard combo counter with a rising sound per hit (topping out at step 7), combo garbage in duels and a `COMBO xN` label.
- Longer KO/win/lose/draw jingles.
- Synthesized Korobeiniki background music with its own on/off toggle.

**Architecture:**
- `Game` counts combos and reports a `Combo` event, the same way it reports clears.
- The pure `tetris_audio` library gains `Combo1..7` sounds, bigger jingles and a real-time `Music` sequencer that the `Mixer` adds under the effects.
- Pure helpers in `sound_cues` turn the screen state into a desired music state. `App` applies it each frame through `AudioOut`.
- Spec: `docs/superpowers/specs/2026-10-13-combo-and-music-design.md`.

**Tech Stack:** C++20, SDL3 audio streams, Dear ImGui, doctest, CMake, Emscripten for the web build.

## Global Constraints

- **No audio files and no new dependencies.** Every sound and the music are generated in code.
- **No SDL outside `AudioOut`.** `tetris_core`, `tetris_audio` and `tetris_duel` must not include SDL. Only `src/app/audio_out.cpp` touches SDL audio.
- **Audio format.** Mono float32 at 48000 Hz. Every effect is shorter than 2.5 s (`kMaxSoundMs = 2500`). Up to 8 effect voices. Master gain 0.5. Music gain 0.4 (on top of master). Output clamped to [-1, 1].
- **Combo counting.** `combo_` starts at -1. Any lock that removes rows (full or bomb) increments it. A lock that removes nothing resets it to -1. `resetBoard()` resets it to -1. A `Combo` event fires from combo 1.
- **Combo bonus.** 0 for combo ≤ 0, 1 for 1–2, 2 for 3–4, 3 for ≥ 5. It's added to `attackFor(counted)` before garbage cancellation.
- **Combo sound.** Combo n plays `Combo{min(n, 7)}`. The pitches climb G5, A5, B5, C6, D6, E6, F♯6.
- **Solo top-out.** It plays `Lost`. `Sound::GameOver` is removed.
- **Music.** Korobeiniki in A minor, played A A B (96 beats) and looped. Melody square at 0.35, bass triangle at 0.4.
  - Solo tempo `min(120 + 8 × (level − 1), 200)` BPM. Duel tempo 150 BPM.
  - Plays in solo `Playing`/`Clearing` and duel `Playing`. Paused in solo `Paused`. Stopped everywhere else.
- **Mutes.** `M` mutes everything. `Music: On/Off` (start and pause menus) mutes only the music. Both last for the session only and start on. While either mute is on, the song keeps advancing.
- **No server or protocol change.**
- **Code style.** Match the surrounding code: `tetris` namespace, `kConstant` names, short `//` comments only where they say something non-obvious, `-Wall -Wextra -Wpedantic` clean.
- **Build and test.** `cmake --build build && ctest --test-dir build --output-on-failure`.
  - One test: `./build/tetris_tests -tc="audio:*"`. UI tests: `./build/tetris_ui_tests`.
  - If `build/` doesn't exist, run `cmake -B build` first.
- **Web build check.** `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`.
  - Prefix it with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14` if the system Python is older than 3.10.

## File map

| File | Responsibility |
|---|---|
| `src/audio/sound.hpp` | `Sound` enum: add `Combo1..7`, remove `GameOver`, add `kComboSteps` |
| `src/audio/synth.{hpp,cpp}` | Combo notes, bigger jingles, `kMaxSoundMs = 2500`. Uses `osc.hpp` from Task 3. |
| `src/audio/osc.hpp` (new) | `Wave`, `advancePhase`, `squareWave`, `triangleWave`, shared by synth and music |
| `src/audio/music.{hpp,cpp}` (new) | Korobeiniki score and the `Music` sequencer |
| `src/audio/mixer.{hpp,cpp}` | Owns a `Music` and mixes it in under the effects |
| `src/core/game.{hpp,cpp}` | `combo_`, the `Combo` event, `comboBonus`, `GameEvent::lines` renamed to `value` |
| `src/app/sound_cues.{hpp,cpp}` | `Combo` → `ComboN`, solo top-out → `Lost`, the music-state helpers |
| `src/app/audio_out.{hpp,cpp}` | Locked music wrappers |
| `src/app/app.{hpp,cpp}` | Music sync each frame, music toggle, combo label state |
| `src/app/render.{hpp,cpp}` | `Music: On/Off` item, `ComboLabel` and drawing it |
| `tests/test_helpers.hpp`, `tests/game_test.cpp`, `tests/audio_test.cpp`, `tests/sound_cues_test.cpp`, `tests/ui_test.cpp` | Tests |
| `CMakeLists.txt`, `README.md` | Build and docs |

---

### Task 1: Combo sounds and bigger jingles

**Files:**
- Modify: `src/audio/sound.hpp`, `src/audio/synth.hpp`, `src/audio/synth.cpp`, `src/app/sound_cues.cpp`
- Test: `tests/audio_test.cpp`, `tests/sound_cues_test.cpp`

**Interfaces:**
- Produces:
  - `Sound::Combo1` … `Sound::Combo7`, consecutive in the enum.
  - `inline constexpr int kComboSteps = 7;` in `audio/sound.hpp`.
  - `kMaxSoundMs == 2500`.
  - `Sound::GameOver` no longer exists.
  - `soundFor(GameEvent{ToppedOut}, false) == Sound::Lost`.

- [ ] **Step 1: Write the failing tests**

In `tests/audio_test.cpp`, add this helper inside the existing first anonymous namespace (after `peakOf`):

```cpp
int zeroCrossings(const std::vector<float>& samples, std::size_t count) {
    int crossings = 0;
    for (std::size_t i = 1; i < count && i < samples.size(); ++i)
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) ++crossings;
    return crossings;
}
```

Then add after the `"audio: rendering is deterministic"` test case:

```cpp
TEST_CASE("audio: sounds may run up to 2.5 s for the jingles") { CHECK(kMaxSoundMs == 2500); }

TEST_CASE("audio: each combo step is higher than the one before") {
    const std::size_t mainNote = kRate * 80 / 1000;  // inside the 90 ms square note
    int previous = 0;
    for (int i = 0; i < kComboSteps; ++i) {
        CAPTURE(i);
        const Sound sound = static_cast<Sound>(static_cast<int>(Sound::Combo1) + i);
        const int crossings = zeroCrossings(render(sound, kRate), mainNote);
        CHECK(crossings > previous);
        previous = crossings;
    }
}

TEST_CASE("audio: win and lose jingles last well over a second") {
    CHECK(render(Sound::Won, kRate).size() > static_cast<std::size_t>(kRate * 3 / 2));
    CHECK(render(Sound::Lost, kRate).size() > static_cast<std::size_t>(kRate * 2));
}
```

In `tests/sound_cues_test.cpp`, change the first line of the `"cues: a top-out ends a solo game but is a KO in a duel"` case to:

```cpp
    CHECK(soundFor(GameEvent{GameEventType::ToppedOut}, false) == Sound::Lost);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: the build fails. `kComboSteps` and `Sound::Combo1` aren't declared.

- [ ] **Step 3: Update the `Sound` enum**

Replace the body of `src/audio/sound.hpp` (inside the namespace) with:

```cpp
enum class Sound {
    Move, Rotate, Hold, HardDrop, Lock, Clear1, Clear2, Clear3, Tetris, Bomb, Garbage, LevelUp,
    Combo1, Combo2, Combo3, Combo4, Combo5, Combo6, Combo7,  // keep consecutive: indexed from Combo1
    CountdownTick, Go, Attacked, KoMine, KoTheirs, Won, Lost, Draw,
    Count  // not a sound: the number of sounds
};

inline constexpr int kSoundCount = static_cast<int>(Sound::Count);
inline constexpr int kComboSteps = 7;  // Combo1-Combo7; longer combos repeat Combo7
```

- [ ] **Step 4: Raise the length limit**

In `src/audio/synth.hpp`, change `inline constexpr int kMaxSoundMs = 1500;` to:

```cpp
inline constexpr int kMaxSoundMs = 2500;
```

- [ ] **Step 5: Add the combo notes and the new jingles in `synth.cpp`**

Add `#include <array>` to the includes. Replace the two pitch-constant lines with:

```cpp
constexpr float kC3 = 130.81f, kA3 = 220.0f;
constexpr float kC4 = 261.63f, kD4 = 293.66f, kE4 = 329.63f, kG4 = 392.0f, kA4 = 440.0f;
constexpr float kC5 = 523.25f, kE5 = 659.26f, kG5 = 783.99f, kC6 = 1046.5f;
// Combo1-Combo7 climb a G major scale from G5, above the clear arpeggios.
constexpr std::array<float, kComboSteps> kComboHz{783.99f, 880.0f, 987.77f, 1046.5f, 1174.66f, 1318.51f, 1479.98f};
```

In `notesFor`, delete the `case Sound::GameOver:` block. Add this after the `case Sound::LevelUp:` block:

```cpp
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
```

Replace the `KoMine`, `KoTheirs`, `Won`, `Lost` and `Draw` cases with:

```cpp
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
```

- [ ] **Step 6: Map a solo top-out to `Lost`**

In `src/app/sound_cues.cpp`, change the `ToppedOut` case to:

```cpp
    case GameEventType::ToppedOut:
        return inDuel ? Sound::KoTheirs : Sound::Lost;
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ./build/tetris_tests -tc="audio:*,cues:*"`
Expected: no errors or warnings. All test cases pass, including the existing "every sound renders a short, audible, in-range buffer" case with the new limit.

- [ ] **Step 8: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 9: Commit**

```bash
git add src/audio/sound.hpp src/audio/synth.hpp src/audio/synth.cpp src/app/sound_cues.cpp tests/audio_test.cpp tests/sound_cues_test.cpp
git commit -m "feat(audio): rising combo notes and longer KO, win and lose jingles"
```

---

### Task 2: Combo counter, event and garbage bonus

**Files:**
- Modify: `src/core/game.hpp`, `src/core/game.cpp`, `src/app/sound_cues.cpp`, `tests/test_helpers.hpp`
- Test: `tests/game_test.cpp`, `tests/sound_cues_test.cpp`

**Interfaces:**
- Consumes: `Sound::Combo1`, `kComboSteps` (Task 1).
- Produces:
  - `GameEventType::Combo`.
  - `GameEvent::value` (renamed from `lines`).
  - `int Game::combo() const`.
  - `int comboBonus(int combo)`.
  - `soundFor(GameEvent{Combo, n}, inDuel) == Combo{min(n, 7)}`.

- [ ] **Step 1: Add the test helper**

Append to `tests/test_helpers.hpp`:

```cpp
// First seed whose bag deals `first` and then `second`.
inline std::uint32_t seedWithFirstTwo(tetris::PieceType first, tetris::PieceType second) {
    for (std::uint32_t seed = 0;; ++seed) {
        const std::vector<tetris::PieceType> dealt = tetris::Bag(seed).peek(2);
        if (dealt[0] == first && dealt[1] == second) return seed;
    }
}
```

Add `#include <vector>` to its includes.

- [ ] **Step 2: Write the failing game tests**

In `tests/game_test.cpp`, find the anonymous namespace that defines `bombBoard`. Add these two functions inside it, after `bombBoard`:

```cpp
// Rows 0-3 are full except column 5, where an upright I clears all four. The two rows above are open only at
// columns 4-5, so after that clear they are the bottom rows, and an O dropped from spawn clears them.
Board comboBoard() {
    Board board;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) board.set(x, y, PieceType::Z);
    for (int y = 4; y < 6; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 4 && x != 5) board.set(x, y, PieceType::Z);
    return board;
}

// Two bomb rows (bomb in column 4) under four rows open only at column 5. An upright I clears the four, then
// an O dropped from spawn lands on the bomb.
Board comboBombBoard() {
    Board board;
    board.pushGarbage(2, 4);
    for (int y = 2; y < 6; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 5) board.set(x, y, PieceType::Z);
    return board;
}
```

Add these test cases after the `"game: resetBoard during a clear drops the clear"` test case:

```cpp
TEST_CASE("game: combo counts consecutive clearing locks and resets on a lock that clears nothing") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    CHECK(game.combo() == -1);
    dropUprightI(game);
    CHECK(game.combo() == 0);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);  // the O completes the two rows left behind
    CHECK(game.combo() == 1);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);  // lands on an empty floor
    CHECK(game.combo() == -1);
}

TEST_CASE("game: a lock that only sets off a bomb keeps the combo going") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBombBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);
    CHECK(game.clearingRows() == std::vector<int>{0, 1});
    CHECK(game.combo() == 1);
}

TEST_CASE("game: resetBoard ends the combo") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.combo() == 0);
    game.resetBoard();
    CHECK(game.combo() == -1);
}

TEST_CASE("game: combo bonus adds 1 line from combo 1, 2 from combo 3 and 3 from combo 5") {
    CHECK(comboBonus(-1) == 0);
    CHECK(comboBonus(0) == 0);
    CHECK(comboBonus(1) == 1);
    CHECK(comboBonus(2) == 1);
    CHECK(comboBonus(3) == 2);
    CHECK(comboBonus(4) == 2);
    CHECK(comboBonus(5) == 3);
    CHECK(comboBonus(10) == 3);
}

TEST_CASE("game: a combo clear sends its bonus on top of the line attack") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.takeOutgoingAttack() == 4);  // combo 0: no bonus
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.takeOutgoingAttack() == attackFor(2) + comboBonus(1));
}

TEST_CASE("game: pending garbage cancels a combo bonus like any other attack") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    game.takeOutgoingAttack();
    game.receiveGarbage(1);
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.pendingGarbage() == 0);
    CHECK(game.takeOutgoingAttack() == 1);  // 1 line + 1 bonus, minus the 1 cancelled
}
```

In the events anonymous namespace (where `cleared(int lines)` is defined), add:

```cpp
constexpr GameEvent comboOf(int n) { return {GameEventType::Combo, n}; }
```

Add this test case at the end of the file:

```cpp
TEST_CASE("game events: a combo is reported from the second clear in a row") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    CHECK(game.takeEvents() == Events{kRotated, kHardDropped, cleared(4)});
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);
    CHECK(game.takeEvents() == Events{kHardDropped, cleared(2), comboOf(1)});
}
```

- [ ] **Step 3: Write the failing cue test**

Append to `tests/sound_cues_test.cpp`:

```cpp
TEST_CASE("cues: combos climb seven steps and then repeat the top one") {
    for (int n = 1; n <= kComboSteps; ++n) {
        CAPTURE(n);
        CHECK(soundFor(GameEvent{GameEventType::Combo, n}, false) ==
              static_cast<Sound>(static_cast<int>(Sound::Combo1) + n - 1));
    }
    CHECK(soundFor(GameEvent{GameEventType::Combo, 8}, false) == Sound::Combo7);
    CHECK(soundFor(GameEvent{GameEventType::Combo, 20}, true) == Sound::Combo7);
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | grep error | head -5`
Expected: compile errors. `combo`, `comboBonus` and `GameEventType::Combo` aren't declared.

- [ ] **Step 5: Implement in `game.hpp`**

Change the enum to:

```cpp
enum class GameEventType { Moved, Rotated, Held, HardDropped, Locked, LinesCleared, BombExploded, GarbageRose, LevelUp, ToppedOut, Combo };
```

Change the struct to:

```cpp
// Something that just happened in a game, for sound. Locked is only for a piece that locks on its own.
struct GameEvent {
    GameEventType type;
    int value = 0;  // LinesCleared: full rows, 1-4 (bomb rows are reported as BombExploded). Combo: the combo, 1+
    friend bool operator==(const GameEvent&, const GameEvent&) = default;
};
```

After the `attackFor` declaration add:

```cpp
// Extra garbage for clearing during a combo; `combo` is Game::combo() at the clearing lock.
int comboBonus(int combo);
```

In the public section of `Game`, after `int lines() const { return lines_; }`, add:

```cpp
    // Clearing locks in a row, minus one: -1 with no combo, 0 after the first clear, 1 after the second...
    int combo() const { return combo_; }
```

Change the private `emit` to:

```cpp
    void emit(GameEventType type, int value = 0) { events_.push_back({type, value}); }
```

After `int outgoingAttack_ = 0;` add:

```cpp
    int combo_ = -1;
```

- [ ] **Step 6: Implement in `game.cpp`**

After `attackFor` add:

```cpp
int comboBonus(int combo) {
    if (combo >= 5) return 3;
    if (combo >= 3) return 2;
    return combo >= 1 ? 1 : 0;
}
```

In `Game::resetBoard()`, after `canHold_ = true;`, add:

```cpp
    combo_ = -1;
```

In `Game::lockPiece`, replace the two `emit` lines after `exploded` is computed with:

```cpp
    combo_ = full.empty() && exploded.empty() ? -1 : combo_ + 1;
    if (!full.empty()) emit(GameEventType::LinesCleared, static_cast<int>(full.size()));
    if (!exploded.empty()) emit(GameEventType::BombExploded);
    if (combo_ >= 1) emit(GameEventType::Combo, combo_);
```

In `Game::finishClear`, change the attack line to:

```cpp
    const int attack = attackFor(counted) + comboBonus(combo_);
```

- [ ] **Step 7: Map the event to a sound**

In `src/app/sound_cues.cpp`, add `#include <algorithm>`. Replace `event.lines` with `event.value` in the `LinesCleared` case (three places). Add this case before the closing brace of the `GameEvent` switch:

```cpp
    case GameEventType::Combo:
        return static_cast<Sound>(static_cast<int>(Sound::Combo1) + std::clamp(event.value, 1, kComboSteps) - 1);
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ctest --test-dir build --output-on-failure`
Expected: no errors or warnings. `100% tests passed`. The existing attack, bomb and duel tests still pass because they only make single clears.

- [ ] **Step 9: Commit**

```bash
git add src/core/game.hpp src/core/game.cpp src/app/sound_cues.cpp tests/test_helpers.hpp tests/game_test.cpp tests/sound_cues_test.cpp
git commit -m "feat(core): count combos, report them and send combo garbage"
```

---

### Task 3: `Music` sequencer with the Korobeiniki score

**Files:**
- Create: `src/audio/osc.hpp`, `src/audio/music.hpp`, `src/audio/music.cpp`
- Modify: `src/audio/synth.cpp`, `CMakeLists.txt`
- Test: `tests/audio_test.cpp`

**Interfaces:**
- Produces:
  - `enum class Wave { Square, Triangle, Noise }` in `audio/osc.hpp`, namespace `tetris`.
  - `bool advancePhase(double& phase, float hz, int sampleRate)`, `float squareWave(double)`, `float triangleWave(double)`.
  - `struct ScoreNote { int midi; float beats; }`.
  - `const std::vector<ScoreNote>& melodyScore()` and `bassScore()`.
  - `float beatsOf(const std::vector<ScoreNote>&)`.
  - `inline constexpr float kScoreBeats = 96.0f;` and `inline constexpr float kDefaultBpm = 120.0f;`.
  - `class Music` with `Music(int sampleRate)`, `start()`, `stop()`, `setPaused(bool)`, `setTempo(float bpm)`, `bool playing() const` and `void mix(float* out, int frames, float gain = 1.0f)`.

- [ ] **Step 1: Write the failing tests**

Add `#include "audio/music.hpp"` to the includes of `tests/audio_test.cpp`. Append:

```cpp
namespace {

std::vector<float> playMusic(Music& music, int frames, float gain = 1.0f) {
    std::vector<float> out(static_cast<std::size_t>(frames), 0.0f);
    music.mix(out.data(), frames, gain);
    return out;
}

}  // namespace

TEST_CASE("music: both voices of the score last 96 beats") {
    CHECK(beatsOf(melodyScore()) == doctest::Approx(kScoreBeats));
    CHECK(beatsOf(bassScore()) == doctest::Approx(kScoreBeats));
}

TEST_CASE("music: silent until started and after stopping") {
    Music music(kRate);
    CHECK_FALSE(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) == 0.0f);
    music.start();
    CHECK(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) > 0.1f);
    music.stop();
    CHECK_FALSE(music.playing());
    CHECK(peakOf(playMusic(music, 4800)) == 0.0f);
}

TEST_CASE("music: two players produce the same samples") {
    Music a(kRate);
    Music b(kRate);
    a.start();
    b.start();
    CHECK(playMusic(a, kRate) == playMusic(b, kRate));
}

TEST_CASE("music: pausing writes nothing and resumes exactly where it stopped") {
    Music paused(kRate);
    Music straight(kRate);
    paused.start();
    straight.start();
    const std::vector<float> first = playMusic(paused, 1000);
    paused.setPaused(true);
    CHECK(paused.playing());
    CHECK(peakOf(playMusic(paused, 1000)) == 0.0f);
    paused.setPaused(false);
    const std::vector<float> resumed = playMusic(paused, 1000);
    const std::vector<float> reference = playMusic(straight, 2000);
    CHECK(std::equal(first.begin(), first.end(), reference.begin()));
    CHECK(std::equal(resumed.begin(), resumed.end(), reference.begin() + 1000));
}

TEST_CASE("music: start rewinds to the top") {
    Music music(kRate);
    music.start();
    const std::vector<float> opening = playMusic(music, 2000);
    playMusic(music, kRate);
    music.start();
    CHECK(playMusic(music, 2000) == opening);
}

TEST_CASE("music: the gain scales what is added") {
    Music full(kRate);
    Music quiet(kRate);
    full.start();
    quiet.start();
    const std::vector<float> a = playMusic(full, 2000);
    const std::vector<float> b = playMusic(quiet, 2000, 0.25f);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == doctest::Approx(a[i] * 0.25f));
}

TEST_CASE("music: a tempo change waits for the next note") {
    Music changed(kRate);
    Music steady(kRate);
    changed.start();
    steady.start();
    std::vector<float> a = playMusic(changed, 100);
    changed.setTempo(240.0f);
    const std::vector<float> rest = playMusic(changed, 11900);
    a.insert(a.end(), rest.begin(), rest.end());
    CHECK(a == playMusic(steady, 12000));  // the first bass note lasts half a beat: 12000 samples at 120 BPM
}

TEST_CASE("music: the loop repeats exactly, and a faster tempo shortens it") {
    Music music(kRate);
    music.setTempo(240.0f);
    music.start();
    const int loop = static_cast<int>(kScoreBeats) * kRate * 60 / 240;  // 12000 samples per beat
    const std::vector<float> out = playMusic(music, loop + kRate);
    CHECK(std::equal(out.begin(), out.begin() + kRate, out.begin() + loop));
    CHECK(peakOf(std::vector<float>(out.begin() + loop - 4800, out.begin() + loop + 4800)) > 0.1f);  // no gap

    Music slower(kRate);
    slower.start();  // 120 BPM: halfway through the song at the same point
    const std::vector<float> half = playMusic(slower, loop + kRate);
    CHECK_FALSE(std::equal(half.begin(), half.begin() + kRate, half.begin() + loop));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | grep error | head -3`
Expected: `audio/music.hpp` not found.

- [ ] **Step 3: Write `src/audio/osc.hpp`**

```cpp
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
```

- [ ] **Step 4: Make `synth.cpp` use it**

In `src/audio/synth.cpp`:
- Add `#include "audio/osc.hpp"` after `#include "audio/synth.hpp"`.
- Delete the line `enum class Wave { Square, Triangle, Noise };` from the anonymous namespace.
- In `render`, replace everything from `const double before = phase;` through the closing brace of the `switch (note.wave)` with:

```cpp
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
```

- [ ] **Step 5: Write `src/audio/music.hpp`**

```cpp
#pragma once

#include <array>
#include <cstddef>
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

private:
    struct Voice {
        const std::vector<ScoreNote>* score;
        Wave wave;
        float volume;
        std::size_t index = 0;
        int pos = 0;     // samples into the current note
        int length = 0;  // samples in the current note
        float hz = 0.0f;
        double phase = 0.0;
    };

    void rewind();
    void beginNote(Voice& voice);
    float nextSample(Voice& voice);

    int sampleRate_;
    float bpm_ = kDefaultBpm;
    bool started_ = false;
    bool paused_ = false;
    std::array<Voice, 2> voices_;
};

}  // namespace tetris
```

- [ ] **Step 6: Write `src/audio/music.cpp`**

```cpp
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

void Music::setTempo(float bpm) { bpm_ = std::max(bpm, 1.0f); }

void Music::mix(float* out, int frames, float gain) {
    if (!started_ || paused_) return;
    for (int i = 0; i < frames; ++i) {
        float sample = 0.0f;
        for (Voice& voice : voices_) sample += nextSample(voice);
        out[i] += sample * gain;
    }
}

void Music::rewind() {
    for (Voice& voice : voices_) {
        voice.index = 0;
        beginNote(voice);
    }
}

// The phase restarts with each note, so every pass through the loop is sample-for-sample the same.
void Music::beginNote(Voice& voice) {
    const ScoreNote& note = (*voice.score)[voice.index];
    voice.pos = 0;
    voice.length = std::max(1, static_cast<int>(std::lround(note.beats * 60.0f / bpm_ * static_cast<float>(sampleRate_))));
    voice.hz = 440.0f * std::exp2(static_cast<float>(note.midi - 69) / 12.0f);
    voice.phase = 0.0;
}

float Music::nextSample(Voice& voice) {
    if (voice.pos >= voice.length) {
        voice.index = (voice.index + 1) % voice.score->size();
        beginNote(voice);
    }
    advancePhase(voice.phase, voice.hz, sampleRate_);
    const float wave = voice.wave == Wave::Square ? squareWave(voice.phase) : triangleWave(voice.phase);
    const float sample = wave * voice.volume * envelope(voice.pos, voice.length, sampleRate_);
    ++voice.pos;
    return sample;
}

}  // namespace tetris
```

- [ ] **Step 7: Add it to the build**

In `CMakeLists.txt`, change the `tetris_audio` source list to:

```cmake
add_library(tetris_audio STATIC
  src/audio/synth.cpp
  src/audio/mixer.cpp
  src/audio/music.cpp)
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ./build/tetris_tests -tc="audio:*,music:*"`
Expected: no errors or warnings, and all pass. If `"music: a tempo change waits for the next note"` fails, check that `setTempo` doesn't call `beginNote`. A tempo change must only apply at the next note.

- [ ] **Step 9: Commit**

```bash
git add src/audio/osc.hpp src/audio/music.hpp src/audio/music.cpp src/audio/synth.cpp CMakeLists.txt tests/audio_test.cpp
git commit -m "feat(audio): synthesize a looping Korobeiniki with a tempo control"
```

---

### Task 4: Mix the music in, and expose it through `AudioOut`

**Files:**
- Modify: `src/audio/mixer.hpp`, `src/audio/mixer.cpp`, `src/app/audio_out.hpp`, `src/app/audio_out.cpp`
- Test: `tests/audio_test.cpp`

**Interfaces:**
- Consumes: `Music` (Task 3).
- Produces:
  - `inline constexpr float kMusicGain = 0.4f;`.
  - On `Mixer`: `startMusic()`, `stopMusic()`, `setMusicPaused(bool)`, `setMusicTempo(float)`, `setMusicMuted(bool)`, `bool musicMuted() const`, `bool musicPlaying() const`.
  - The same five setters on `AudioOut`: `startMusic()`, `stopMusic()`, `setMusicPaused(bool)`, `setMusicTempo(float)`, `setMusicMuted(bool)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/audio_test.cpp`:

```cpp
TEST_CASE("mixer: music plays under the effects at the music gain") {
    Mixer mixer(kRate);
    mixer.startMusic();
    CHECK(mixer.musicPlaying());
    Music reference(kRate);
    reference.start();
    const std::vector<float> expected = playMusic(reference, 2000, kMusicGain * kMasterGain);
    const std::vector<float> out = mixFrames(mixer, 2000);
    for (std::size_t i = 0; i < out.size(); ++i) CHECK(out[i] == doctest::Approx(expected[i]));
}

TEST_CASE("mixer: muting the music leaves only the effects") {
    Mixer withMusic(kRate);
    withMusic.startMusic();
    withMusic.setMusicMuted(true);
    CHECK(withMusic.musicMuted());
    withMusic.play(Sound::Tetris);
    Mixer effectsOnly(kRate);
    effectsOnly.play(Sound::Tetris);
    CHECK(mixFrames(withMusic, 4000) == mixFrames(effectsOnly, 4000));
}

TEST_CASE("mixer: muted music keeps its place in the song") {
    for (bool masterMute : {true, false}) {
        CAPTURE(masterMute);
        Mixer muted(kRate);
        Mixer reference(kRate);
        muted.startMusic();
        reference.startMusic();
        if (masterMute)
            muted.setMuted(true);
        else
            muted.setMusicMuted(true);
        CHECK(peakOf(mixFrames(muted, 1000)) == 0.0f);
        CHECK(muted.musicPlaying());
        muted.setMuted(false);
        muted.setMusicMuted(false);
        mixFrames(reference, 1000);
        CHECK(mixFrames(muted, 1000) == mixFrames(reference, 1000));
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | grep error | head -3`
Expected: `startMusic` is not a member of `Mixer`.

- [ ] **Step 3: Extend `mixer.hpp`**

Add `#include "audio/music.hpp"` after `#include "audio/sound.hpp"`. After `kMasterGain` add:

```cpp
inline constexpr float kMusicGain = 0.4f;  // music sits under the effects
```

In the public section, after `int activeVoices() const ...`, add:

```cpp
    void startMusic() { music_.start(); }
    void stopMusic() { music_.stop(); }
    void setMusicPaused(bool paused) { music_.setPaused(paused); }
    void setMusicTempo(float bpm) { music_.setTempo(bpm); }
    void setMusicMuted(bool muted) { musicMuted_ = muted; }  // the song keeps advancing while muted
    bool musicMuted() const { return musicMuted_; }
    bool musicPlaying() const { return music_.playing(); }
```

Change the `setMuted` comment to `// muting also stops the effects that are playing; the music keeps its place`. In the private section, after `bool muted_ = false;`, add:

```cpp
    Music music_;
    bool musicMuted_ = false;
```

- [ ] **Step 4: Mix it in `mixer.cpp`**

Change the constructor's first line to:

```cpp
Mixer::Mixer(int sampleRate) : music_(sampleRate) {
```

In `Mixer::mix`, insert before the final clamp loop:

```cpp
    const float musicGain = muted_ || musicMuted_ ? 0.0f : kMusicGain * kMasterGain;
    music_.mix(out, frames, musicGain);  // runs even when silent, so the song stays in time
```

- [ ] **Step 5: Add the `AudioOut` wrappers**

In `src/app/audio_out.hpp`, change the class comment to `// Plays sound effects and music on the default output device. If none can be opened it logs once and stays silent.` After `void setMuted(bool muted);` add:

```cpp
    void startMusic();
    void stopMusic();
    void setMusicPaused(bool paused);
    void setMusicTempo(float bpm);
    void setMusicMuted(bool muted);
```

In the private section, before `static void SDLCALL feed(...)`, add:

```cpp
    // Runs `change` on the mixer under the stream lock, which SDL also holds while it runs feed().
    template <class Change>
    void locked(Change change) {
        if (stream_ == nullptr) return;
        SDL_LockAudioStream(stream_);
        change(mixer_);
        SDL_UnlockAudioStream(stream_);
    }
```

In `src/app/audio_out.cpp`, replace the bodies of `play` and `setMuted` and add the new methods:

```cpp
void AudioOut::play(Sound sound) {
    locked([sound](Mixer& m) { m.play(sound); });
}

void AudioOut::setMuted(bool muted) {
    locked([muted](Mixer& m) { m.setMuted(muted); });
}

void AudioOut::startMusic() {
    locked([](Mixer& m) { m.startMusic(); });
}

void AudioOut::stopMusic() {
    locked([](Mixer& m) { m.stopMusic(); });
}

void AudioOut::setMusicPaused(bool paused) {
    locked([paused](Mixer& m) { m.setMusicPaused(paused); });
}

void AudioOut::setMusicTempo(float bpm) {
    locked([bpm](Mixer& m) { m.setMusicTempo(bpm); });
}

void AudioOut::setMusicMuted(bool muted) {
    locked([muted](Mixer& m) { m.setMusicMuted(muted); });
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ctest --test-dir build --output-on-failure`
Expected: no errors or warnings. `100% tests passed`, including the existing mixer tests, because the music isn't started there.

- [ ] **Step 7: Commit**

```bash
git add src/audio/mixer.hpp src/audio/mixer.cpp src/app/audio_out.hpp src/app/audio_out.cpp tests/audio_test.cpp
git commit -m "feat(audio): mix music under the effects with its own mute"
```

---

### Task 5: Drive the music from the game, and add the `Music` menu item

**Files:**
- Modify: `src/app/sound_cues.hpp`, `src/app/sound_cues.cpp`, `src/app/app.hpp`, `src/app/app.cpp`, `src/app/render.hpp`, `src/app/render.cpp`, `README.md`
- Test: `tests/sound_cues_test.cpp`

**Interfaces:**
- Consumes: the `AudioOut` music setters (Task 4).
- Produces:
  - `enum class MusicMode { Stopped, Playing, Paused }`.
  - `struct MusicCue { MusicMode mode; float bpm; }`.
  - `inline constexpr float kDuelBpm = 150.0f;`.
  - `float soloBpm(int level)`, `MusicCue soloMusic(const Game&)` and `MusicCue duelMusic(DuelPhase)`.
  - `UiCommand::ToggleMusic`, `StartMenu::musicOff`, and the new `drawOverlay(const Game&, const Fonts&, bool muted, bool musicOff)` signature.

- [ ] **Step 1: Write the failing tests**

In `tests/sound_cues_test.cpp`, add `#include "test_helpers.hpp"` after the existing include. Append:

```cpp
TEST_CASE("cues: solo music speeds up 8 BPM a level from 120, up to 200") {
    CHECK(soloBpm(1) == 120.0f);
    CHECK(soloBpm(2) == 128.0f);
    CHECK(soloBpm(10) == 192.0f);
    CHECK(soloBpm(11) == 200.0f);
    CHECK(soloBpm(15) == 200.0f);
}

TEST_CASE("cues: solo music plays, pauses with the game and stops at game over") {
    Game game(seedWithFirst(PieceType::T), 5);
    CHECK(soloMusic(game) == MusicCue{MusicMode::Playing, soloBpm(5)});
    game.pause();
    CHECK(soloMusic(game) == MusicCue{MusicMode::Paused, soloBpm(5)});
    game.resume();
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    REQUIRE(game.state() == GameState::GameOver);
    CHECK(soloMusic(game).mode == MusicMode::Stopped);
}

TEST_CASE("cues: solo music keeps playing through a line clear") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.state() == GameState::Clearing);
    CHECK(soloMusic(game).mode == MusicMode::Playing);
}

TEST_CASE("cues: duel music plays only while the match runs") {
    CHECK(duelMusic(DuelPhase::Playing) == MusicCue{MusicMode::Playing, kDuelBpm});
    for (DuelPhase phase : {DuelPhase::Idle, DuelPhase::Waiting, DuelPhase::Countdown, DuelPhase::Result}) {
        CAPTURE(static_cast<int>(phase));
        CHECK(duelMusic(phase).mode == MusicMode::Stopped);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | grep error | head -3`
Expected: `soloBpm` was not declared.

- [ ] **Step 3: Add the music helpers**

In `src/app/sound_cues.hpp`, after the `soundFor` declarations, add:

```cpp
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
```

In `src/app/sound_cues.cpp`, before the closing namespace brace, add:

```cpp
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

MusicCue duelMusic(DuelPhase phase) {
    if (phase == DuelPhase::Playing) return {MusicMode::Playing, kDuelBpm};
    return {};
}
```

- [ ] **Step 4: Run the cue tests to verify they pass**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ./build/tetris_tests -tc="cues:*"`
Expected: all pass.

- [ ] **Step 5: Add the menu item**

In `src/app/render.hpp`, add `ToggleMusic` to the end of `UiCommand`:

```cpp
enum class UiCommand { None, Play, Resume, Restart, QuitToMenu, QuitApp, CreateRoom, JoinRoom, Rematch, LeaveDuel, ToggleSound, ToggleMusic };
```

In `StartMenu`, after `muted`, add:

```cpp
    bool musicOff = false;                    // background music off for this session
```

Change the `drawOverlay` declaration to:

```cpp
UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted, bool musicOff);
```

In `src/app/render.cpp`, after `soundButton`, add:

```cpp
bool musicButton(bool off) { return menuButton(off ? "Music: Off###music" : "Music: On###music"); }
```

In `drawStartScreen`, after the `soundButton` line, add:

```cpp
    if (musicButton(menu.musicOff)) command = UiCommand::ToggleMusic;
```

Change the `drawOverlay` definition's signature to match the header. After its `soundButton` line, add:

```cpp
        if (musicButton(musicOff)) command = UiCommand::ToggleMusic;
```

- [ ] **Step 6: Wire the app**

In `src/app/app.hpp`:
- Add `#include "app/sound_cues.hpp"` after `#include "app/render.hpp"`.
- Add `MusicMode music = MusicMode::Stopped;  // what the music is doing, as last applied` after `AudioOut audio;`.
- After `void toggleSound();` add:

```cpp
    void toggleMusic();                   // flips the music on or off for the session
    void syncMusic(const MusicCue& cue);  // starts, pauses, retimes or stops the music to match `cue`
```

In `src/app/app.cpp`, add this case to `applyCommand`, after `ToggleSound`:

```cpp
    case UiCommand::ToggleMusic:
        app.toggleMusic();
        break;
```

In `App::newGame()`, add this line first:

```cpp
    syncMusic({});  // a new game always starts the song from the top
```

After `App::toggleSound()`, add:

```cpp
void App::toggleMusic() {
    menu.musicOff = !menu.musicOff;
    audio.setMusicMuted(menu.musicOff);
}

void App::syncMusic(const MusicCue& cue) {
    if (cue.mode == MusicMode::Stopped) {
        if (music != MusicMode::Stopped) audio.stopMusic();
    } else {
        audio.setMusicTempo(cue.bpm);  // before start, so the first note is already at this tempo
        if (music == MusicMode::Stopped) audio.startMusic();
        audio.setMusicPaused(cue.mode == MusicMode::Paused);
    }
    music = cue.mode;
}
```

In the anonymous namespace, after `playEvents`, add:

```cpp
// What the music should be doing for the current screen.
MusicCue musicCue(App& app) {
    if (app.screen == Screen::Playing && app.game) return soloMusic(*app.game);
    if (app.screen == Screen::Duel) return duelMusic(app.duel.phase());
    return {};
}
```

At the end of `step()`, after `playEvents(app);`, add:

```cpp
    app.syncMusic(musicCue(app));
```

In `drawFrame`, change the overlay call to:

```cpp
        command = drawOverlay(*app.game, fonts, app.menu.muted, app.menu.musicOff);
```

- [ ] **Step 7: Update the README**

In `README.md`, change the controls sentence on line 24 to:

```
C or Shift holds, Esc or P pauses, M mutes the sound, and Ctrl+Cmd+F or Alt+Enter toggles fullscreen. The Music item on the start and pause menus turns the background music off or on.
```

- [ ] **Step 8: Build and run everything**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ctest --test-dir build --output-on-failure`
Expected: no errors or warnings. `100% tests passed`.

- [ ] **Step 9: Check by ear (desktop)**

Run: `./build/tetris`.
- Music is silent on the start menu.
- Play: Korobeiniki starts from the top.
- Esc pauses it, and Resume continues where it left off.
- Restart from the pause menu starts the song over.
- At level-up the tempo rises slightly.
- At game over the music stops and the falling `Lost` jingle plays alone.
- `Music: Off` silences only the music. Effects keep playing.
- `M` silences everything.

- [ ] **Step 10: Commit**

```bash
git add src/app/sound_cues.hpp src/app/sound_cues.cpp src/app/app.hpp src/app/app.cpp src/app/render.hpp src/app/render.cpp tests/sound_cues_test.cpp README.md
git commit -m "feat(app): background music during play with a Music menu toggle"
```

---

### Task 6: `COMBO xN` label

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.hpp`, `src/app/app.cpp`
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Consumes: `Game::combo()` (Task 2).
- Produces:
  - `inline constexpr int kComboFadeMs = 500;`.
  - `struct ComboLabel { int shown; int fadeMs; void update(int combo, int ms); float alpha() const; }`.
  - `drawGame(const Game&, const Fonts&, const ComboLabel&)` and `drawDuel(const DuelSession&, const Fonts&, const ComboLabel&)`.

The text uses a plain `x` instead of `×`, because the HUD font is only guaranteed to have ASCII glyphs.

- [ ] **Step 1: Write the failing tests**

Append to `tests/ui_test.cpp`:

```cpp
TEST_CASE("ui: the combo label follows the live combo") {
    ComboLabel label;
    CHECK(label.shown == 0);
    CHECK(label.alpha() == 0.0f);
    label.update(-1, 16);
    CHECK(label.shown == 0);
    label.update(1, 16);
    CHECK(label.shown == 1);
    CHECK(label.alpha() == 1.0f);
    label.update(3, 16);
    CHECK(label.shown == 3);
}

TEST_CASE("ui: the combo label fades out once the combo breaks") {
    ComboLabel label;
    label.update(2, 16);
    label.update(-1, kComboFadeMs / 2);
    CHECK(label.shown == 2);
    CHECK(label.alpha() == doctest::Approx(0.5f));
    label.update(-1, kComboFadeMs / 2);
    CHECK(label.shown == 0);
    CHECK(label.alpha() == 0.0f);
}

TEST_CASE("ui: a new combo during the fade shows at full strength") {
    ComboLabel label;
    label.update(4, 16);
    label.update(-1, 100);
    label.update(1, 16);
    CHECK(label.shown == 1);
    CHECK(label.alpha() == 1.0f);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build 2>&1 | grep error | head -3`
Expected: `ComboLabel` was not declared.

- [ ] **Step 3: Declare `ComboLabel` and the new signatures**

In `src/app/render.hpp`, after `StartMenu`, add:

```cpp
inline constexpr int kComboFadeMs = 500;

// The COMBO xN label beside the local board. It shows the live combo and fades out once the combo breaks.
struct ComboLabel {
    int shown = 0;   // the combo on the label; 0 draws nothing
    int fadeMs = 0;  // fade time left; stays full while the combo is live
    void update(int combo, int ms);  // combo is Game::combo(), or -1 with no game
    float alpha() const { return shown > 0 ? static_cast<float>(fadeMs) / kComboFadeMs : 0.0f; }
};
```

Change the two declarations to:

```cpp
void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo);
```

```cpp
UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo);
```

- [ ] **Step 4: Implement it in `render.cpp`**

After `drawSidePanels`, add:

```cpp
// Under the HOLD panel, in yellow.
void drawComboLabel(ImDrawList* dl, const Layout& l, const Fonts& fonts, const ComboLabel& combo) {
    if (combo.shown <= 0) return;
    char text[24];
    std::snprintf(text, sizeof text, "COMBO x%d", combo.shown);
    const ImVec2 pos(l.origin.x - 5.5f * l.cell, cellTopLeft(l, 0, kVisibleHeight - 1).y + 4.0f * l.cell);
    dl->AddText(fonts.hud, l.cell * 0.6f, pos, IM_COL32(255, 220, 60, static_cast<int>(combo.alpha() * 255.0f)), text);
}
```

Change `drawPlayfield` to take and draw it:

```cpp
void drawPlayfield(ImDrawList* dl, const Layout& layout, const Game& game, const Fonts& fonts,
                   const ComboLabel& combo) {
    drawWell(dl, layout);
    if (game.state() == GameState::Paused) return;
    drawBoardCells(dl, layout, game);
    if (game.state() == GameState::Playing) drawActivePiece(dl, layout, game);
    drawSidePanels(dl, layout, game, fonts);
    drawComboLabel(dl, layout, fonts, combo);
}
```

Thread the label through:
- `drawDuelBoards(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo)`: its call becomes `drawPlayfield(dl, layout.mine, game, fonts, combo);`.
- `drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo)`: its call becomes `drawPlayfield(dl, computeLayout(), game, fonts, combo);`.
- `drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo)`: all three `drawDuelBoards(duel, fonts)` calls become `drawDuelBoards(duel, fonts, combo)`.

After `configureImGuiIo`, add:

```cpp
void ComboLabel::update(int combo, int ms) {
    if (combo >= 1) {
        shown = combo;
        fadeMs = kComboFadeMs;
        return;
    }
    fadeMs = std::max(0, fadeMs - ms);
    if (fadeMs == 0) shown = 0;
}
```

- [ ] **Step 5: Keep the label in `App`**

In `src/app/app.hpp`, after `MusicMode music ...`, add:

```cpp
    ComboLabel combo;
```

In `src/app/app.cpp`, replace the first two lines of `playEvents` with a shared helper. Add this above `playEvents`:

```cpp
// The local game on screen: the solo game, or our duel game.
Game* shownGame(App& app) {
    if (app.screen == Screen::Duel) return app.duel.game();
    return app.game ? &*app.game : nullptr;
}
```

and change `playEvents` to:

```cpp
void playEvents(App& app) {
    const bool inDuel = app.screen == Screen::Duel;
    if (Game* game = shownGame(app))
        for (const GameEvent& event : game->takeEvents()) app.audio.play(soundFor(event, inDuel));
    for (DuelEvent event : app.duel.takeEvents()) app.audio.play(soundFor(event));
}
```

In `step()`, after `playEvents(app);`, add:

```cpp
    const Game* shown = shownGame(app);
    app.combo.update(shown != nullptr ? shown->combo() : -1, ms);
```

In `drawFrame`, change the two draw calls to:

```cpp
        drawGame(*app.game, fonts, app.combo);
```

```cpp
        command = drawDuel(app.duel, fonts, app.combo);
```

- [ ] **Step 6: Build and run everything**

Run: `cmake --build build 2>&1 | grep -E "error|warning" ; ctest --test-dir build --output-on-failure`
Expected: no errors or warnings. `100% tests passed` (both `tetris_tests` and `tetris_ui_tests`).

- [ ] **Step 7: Check by eye and ear (desktop)**

Run: `./build/tetris`, and play until you chain clears. A practical way: start at level 1, stack a flat 9-wide pile with a 1-wide well, then clear singles back to back.
- From the second clear in a row, a yellow `COMBO x1`, `x2`, … appears under HOLD.
- Each clear adds a higher note on top of the clear sound.
- From x7 on, the note repeats.
- A lock with no clear fades the label out over half a second.

- [ ] **Step 8: Commit**

```bash
git add src/app/render.hpp src/app/render.cpp src/app/app.hpp src/app/app.cpp tests/ui_test.cpp
git commit -m "feat(app): show a fading COMBO label beside the board"
```

---

### Task 7: Web build and duel check

**Files:** none changed unless a problem turns up.

- [ ] **Step 1: Build the web version**

Run: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web 2>&1 | grep -E "error|warning|Linking"`
Expected: it links `index.html` with no errors or warnings. If emcc complains about Python, prefix it with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14`.

- [ ] **Step 2: Check a duel by hand**

Start the Go server and open two browser tabs, as the README describes for duels.
- Music is silent in the waiting room and the countdown, then starts at GO at a steady tempo.
- A KO plays the longer KO sting, and the music keeps going.
- A combo sends visibly more garbage to the other tab (watch its red meter).
- At match end the music stops and the 2-second win or lose jingle plays alone.
- `Music: Off` on the start menu stays off for the next match.

- [ ] **Step 3: Commit only if something had to change**

If the checks needed a fix, commit it with a `fix(...)` message that names what was wrong. Otherwise there is nothing to commit.
