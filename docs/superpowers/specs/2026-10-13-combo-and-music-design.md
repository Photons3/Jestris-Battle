# Design: Combos, bigger jingles and background music

## Context
The synthesized sound effects (`2026-10-13-sound-effects-design.md`) are in. Three things are missing:

- **Combos.** The game doesn't count consecutive clears, so chaining clears sounds the same as isolated ones.
- **Jingles.** `KoMine`, `KoTheirs`, `Won` and `Lost` exist but are 0.3–0.8 s blips. Solo game over is a single falling note.
- **Music.** There is none.

This adds a standard combo counter (with rising sounds and duel garbage), longer KO/win/lose jingles, and a synthesized Korobeiniki loop. The code still makes every sound itself, so there are still no audio files.

## Goals and non-goals
- **Goals:**
  - A combo counter in `Game` with a rising sound per hit that tops out at step 7, combo garbage in duels and a `COMBO ×N` label.
  - Longer, more satisfying KO/win/lose/draw jingles. Solo top-out plays the lose jingle.
  - Korobeiniki background music during solo games and duels. In solo it speeds up with level. It pauses, stops for the jingles, and has its own `Music: On/Off` toggle.
- **Non-goals:**
  - Music on menus, other tracks, a volume slider.
  - Saving the sound or music setting between sessions.
  - A combo score bonus.
  - Any server or protocol change.

## 1. Combos (`tetris_core`, `core/game.{hpp,cpp}`)

### Counting
- `int combo_ = -1;` plus a public `int combo() const`.
- In `lockPiece()`, once `clearingRows_` is built: if it isn't empty (full rows **or** bomb rows), do `++combo_`. Otherwise (a lock that removes nothing) set `combo_ = -1`. So the first clear is combo 0, the second in a row is combo 1, and so on.
- `resetBoard()` (duel KO) sets `combo_ = -1`.

### Event
- New `GameEventType::Combo`. When a lock removes rows and `combo_ >= 1` after the increment, `lockPiece()` emits `GameEvent{Combo, combo_}`. The value goes in the existing `lines` field, renamed `value` so it reads correctly for both uses. The event comes after `LinesCleared` / `BombExploded` in the same lock.
- Like the other clear events, it fires at lock time, so the sound lands with the flash.

### Garbage
- New free function `int comboBonus(int combo)`, declared next to `attackFor`:

  | combo | ≤ 0 | 1–2 | 3–4 | ≥ 5 |
  |---|---|---|---|---|
  | bonus | 0 | 1 | 2 | 3 |

- `finishClear()` computes `attack = attackFor(counted) + comboBonus(combo_)` and runs it through the existing cancel-against-`pendingGarbage_` path into `outgoingAttack_`.
- The server accepts any `ATK n` from 1 to 40 and only relays and totals it, so no server change is needed.
- In solo, `outgoingAttack_` is never sent, so the bonus has no effect there.

### UI (`render.cpp`)
- While `game.combo() >= 1`, a small `COMBO ×N` label is drawn beside the board (solo and duel, local board only).
- When the combo breaks, the label fades out over 500 ms. `App` keeps the last value and a fade timer.

## 2. Sounds (`tetris_audio`)

### New and changed `Sound` values
- **Add** `Combo1` … `Combo7`.
- **Remove** `GameOver`. Solo top-out uses `Lost`.

### `synth.cpp`
- `kMaxSoundMs` goes from 1500 to 2500.
- **`ComboN`:** one bright square note of ~90 ms at volume ~0.5, followed by a quiet triangle echo an octave up.
  - Pitch climbs a major scale starting at G5: G5, A5, B5, C6, D6, E6, F♯6 (N = 1…7).
  - It sits above the clear arpeggios, so it reads as a separate layer.
- **`KoMine`** (~0.8 s, big): a noise hit (~60 ms), then a square slide C5→C6 (150 ms), then a held triangle C6+G6 chord (500 ms).
- **`KoTheirs`** (~0.8 s, big): a noise crash sliding 2000→100 Hz (400 ms) with a square slide 330→80 Hz (700 ms) under it.
- **`Won`** (~2 s, big): square pickups C5-E5-G5 (100 ms each), then a held C6 with E6 and G6 triangles (1.2 s), over a C3 triangle bass.
- **`Lost`** (~2 s): a slow triangle line A4-G4-E4-D4-C4 (300 ms each), ending on a held low A3 (800 ms).
- **`Draw`** (~1.2 s): two identical flat triangle phrases E4-E4-D4.
- All other sounds are unchanged.

### `soundFor` (`app/sound_cues.cpp`)
- `GameEvent Combo{n}` → `Combo{min(n, 7)}`. So hit 7 and every hit after it repeat `Combo7`.
- `ToppedOut` → `Lost` solo, `KoTheirs` in a duel (unchanged).
- Everything else is unchanged.

## 3. Music (`tetris_audio`, new `audio/music.{hpp,cpp}`, no SDL)

### Score
- A `constexpr` table of Korobeiniki in A minor (the familiar E-B-C-D opening), played A, A, B (96 beats) and looped. It has two voices:
  - **Melody:** square wave, volume ~0.35.
  - **Bass:** triangle wave, volume ~0.4, alternating root and octave in eighth notes.
- Each entry is `{midiNote, beats}`. The score has no rests. The bass is generated from one root per bar. Each voice's beats add up to the same total, so the loop stays aligned.
- The piece is public domain (a 19th-century folk song).

### `class Music`
```cpp
class Music {
public:
    explicit Music(int sampleRate);
    void start();                 // from the top, unpaused
    void stop();                  // silent and rewound
    void setPaused(bool paused);  // keeps position; no effect while stopped
    void setTempo(float bpm);     // both voices switch together when the melody starts its next note
    bool playing() const;         // started, not stopped (paused still counts)
    void mix(float* out, int frames, float gain = 1.0f);  // ADDS gain × output into out; does nothing while stopped or paused
};
```
- Each voice keeps its note index, the beat and sample its note starts on, the sample it ends on, and an oscillator phase.
- At the end of a note it moves to the next note and wraps at the end of the score. Note ends come from one shared beat clock: an anchor (a sample and the beat it fell on) gives each note the end sample `anchorSample + round((noteEndBeat − anchorBeat) × samplesPerBeat)`, so rounding never adds up and the voices can't drift apart.
- `setTempo` only stores the new tempo. It takes effect when the melody starts its next note: every melody note starts on the eighth-note grid, where a bass note starts too, so both voices switch on the same sample. The clock re-anchors there and at each loop wrap. `start()` uses the latest tempo from the first note.
- Each note restarts its oscillator phase, ramps up over 5 ms, holds for 85% of its length, then releases over 20 ms. So note joins and the loop point don't click, and the output repeats exactly every loop.
- Fully deterministic. The square and triangle oscillators are shared with `synth.cpp` through a small internal header (`audio/osc.hpp`) instead of being copied.

### `Mixer` integration
- `Mixer` owns a `Music` and forwards to it: `startMusic()`, `stopMusic()`, `setMusicPaused(bool)`, `setMusicTempo(float)`, `setMusicMuted(bool)`, `musicMuted()`, `musicPlaying()`.
- `mix()`:
  1. Fills the buffer with the effect voices.
  2. Has `Music` add in its output, scaled by `kMusicGain = 0.4` × `kMasterGain`, or by 0 while either mute is on (so the song still advances).
  3. Applies `kMasterGain` and clamps.
- **Master mute (`M`)** behaves as before: silence, and `play()` is ignored. The music **keeps advancing** while master-muted, so unmuting doesn't restart the song.
- **Music mute** skips adding the music but keeps advancing it. Turning the music back on resumes in place, in time with the game.

### `AudioOut`
Adds locked wrappers for each new mixer call, the same pattern as `play()`. If no audio device opened, they do nothing, but the music-mute flag is still tracked for the UI.

## 4. App wiring (`app.cpp`)

The music state comes from what's on screen. Each frame, after events are handled, `App` works out a desired state using pure helpers in `sound_cues` (`soloMusic(const Game&)`, `duelMusic(DuelPhase)`, `soloBpm(int level)`), which are unit-tested:

| Situation | Music |
|---|---|
| Solo game `Playing` or `Clearing` | playing, tempo `min(120 + 8 × (level − 1), 200)` BPM |
| Solo game `Paused` | paused |
| Duel phase `Playing` (including the frame of a KO and its board reset) | playing, 150 BPM |
| Anything else: menus, solo `GameOver`, duel `Waiting`/`Countdown`/`Result` | stopped |

- **Stopped → playing** calls `startMusic()` (from the top). Playing ↔ paused calls `setMusicPaused`. Tempo is set every frame, which is idempotent.
- `App::newGame()` (Play and Restart) stops the music first, so even a Restart from the pause menu starts the song over.
- **Jingles.** The top-out or match-end frame switches the state to stopped, which is the same frame the `Lost` / `Won` / `Draw` / `KoTheirs` sound is triggered. So the jingle plays on its own.
- **Menu.** `Music: On` / `Music: Off` goes on the start menu and the pause menu, below `Sound`. It emits a new `UiCommand::ToggleMusic`. Music starts on and is not saved.
- **README:** mention the music toggle.

## 5. Build
- `src/audio/music.cpp` is added to `tetris_audio`.
- No new dependencies and no new libraries.

## 6. Tests (doctest, native)

- **`game_test.cpp`:**
  - Two clearing locks in a row give `combo()` 0 then 1, and the second emits `Combo{1}`. The first emits no `Combo` event.
  - A lock that clears nothing resets `combo()` to −1.
  - A bomb-only lock (no full rows) still increments the combo.
  - `resetBoard()` resets the combo to −1.
  - `comboBonus` matches the table (−1, 0, 1, 2, 3, 4, 5, 10).
  - In a combo of 3 with no pending garbage, a single clear sends `attackFor(1) + comboBonus(2)` = 1 line. With pending garbage, the bonus is cancelled first like any other attack.
- **`sound_cues_test.cpp`:**
  - `Combo{1..7}` → `Combo1..Combo7`. `Combo{8}` and `Combo{20}` → `Combo7`.
  - Solo `ToppedOut` → `Lost`.
- **`audio_test.cpp`:**
  - Every sound renders non-empty, shorter than 2.5 s, within ±1, with a peak above 0.05 (an existing test, with the new limit).
  - Each of `Combo1..7` has a higher dominant pitch than the one before (measured by zero-crossing count over the main note).
  - `Music`:
    - Silent before `start()` and after `stop()`.
    - Two `Music` instances produce identical samples.
    - Paused writes nothing, and after unpausing the output continues exactly where it stopped.
    - At a doubled tempo, the first note ends in about half the samples.
    - Rendering past one full loop has no all-zero gap longer than a rest at the loop point.
  - `Mixer`:
    - Music-muted output equals the effects-only output.
    - Master-muted output is silence while the music keeps its position.
- **By hand:**
  - Desktop and browser: music starts at `GO` and on solo start, pauses with the pause menu, speeds up on level-up, and stops for the jingles.
  - A 7+ combo climbs and then repeats the top note, and `COMBO ×N` shows and fades.
  - `Music: Off` silences only the music. `M` silences everything.
  - In a duel, a combo sends visibly more garbage to the opponent.
