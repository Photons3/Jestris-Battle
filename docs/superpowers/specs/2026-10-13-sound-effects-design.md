# Design: Synthesized sound effects

## Context
The game has no audio. This adds retro sound effects that the code generates at startup: no audio files, no licences, nothing extra for the web build to download. SDL3 already ships an audio API (`SDL_OpenAudioDeviceStream`) that works on the desktop and in the Emscripten build, so no new dependency is needed.

## Goals and non-goals
- **Goals:** sound effects for solo play and duels; a pure, unit-tested synth and mixer; a mute key and a menu toggle; the game running silently when no audio device is available.
- **Non-goals:** music (Korobeiniki can come later on top of the same synth), a volume slider, saving the mute setting between sessions, menu navigation sounds, sounds for the opponent's moves, a soft-drop sound.

## 1. Events

`Game` and `DuelSession` each keep a `std::vector` of events and hand them over through `takeEvents()`, which returns the list and empties it. This follows the existing `takeOutgoingAttack()` pattern: the core stays free of SDL, and the events can be unit-tested.

### `GameEvent` (`tetris_core`, `core/game.hpp`)

| Event | Emitted when |
|---|---|
| `Moved` | Left or Right succeeds, auto-repeat included. A blocked move emits nothing. |
| `Rotated` | `rotate()` finds a valid rotation (with or without a kick). |
| `Held` | `holdPiece()` swaps. A refused hold emits nothing. |
| `HardDropped` | `hardDrop()`. A hard drop never also emits `Locked`. |
| `Locked` | A piece locks on its own after the lock delay without clearing anything. |
| `LinesCleared{n}` | `lockPiece()` finds `n` (1–4) **full** rows. Bomb rows are not counted. |
| `BombExploded` | `lockPiece()` gets a non-empty result from `bombRowsHitBy`. |
| `GarbageRose` | `applyGarbage()` actually pushes rows. |
| `LevelUp` | `finishClear()` raises `level_`. |
| `ToppedOut` | Any switch to `GameOver` (block-out, lock-out, garbage overflow). It fires exactly once. |

`GameEvent` is a struct `{ GameEventType type; int lines = 0; }`, where `lines` is used only by `LinesCleared`.

A lock that both fills rows and hits a bomb emits both `LinesCleared{n}` and `BombExploded`. Clear and bomb events are emitted when the piece locks, not 300 ms later when the rows are removed, so the sound lands with the flash.

`resetBoard()` does **not** touch the event list. After a duel KO it runs in the same frame as the top-out, and clearing the list would lose the `ToppedOut` event.

### `DuelEvent` (`app/duel.hpp`)

| Event | Emitted when |
|---|---|
| `CountdownTick` | `START` begins the countdown, and again each time the countdown crosses a whole second (3, 2, 1). |
| `MatchStart` | The countdown reaches 0 and the phase becomes `Playing`. |
| `Attacked` | An `Attack` message adds garbage to the local game. |
| `KoMine` | A `KoCounts` message raises our KO count (we KO'd the opponent). |
| `Won` / `Lost` / `Draw` | `finish()` with that outcome. `OpponentLeft` emits nothing. |

A rise in the opponent's KO count (our own top-out) emits nothing in `DuelSession`. The local `Game` already reported it as `ToppedOut`, without network delay, and the cue mapping below plays the KO sound for it in a duel.

## 2. Event-to-sound mapping (`app/sound_cues.{hpp,cpp}`, in `tetris_duel`)

These are pure functions with no SDL:

```cpp
std::optional<Sound> soundFor(const GameEvent& event, bool inDuel);
std::optional<Sound> soundFor(DuelEvent event);
```

| Event | Sound |
|---|---|
| `Moved` | `Move` |
| `Rotated` | `Rotate` |
| `Held` | `Hold` |
| `HardDropped` | `HardDrop` |
| `Locked` | `Lock` |
| `LinesCleared{1..3}` | `Clear1`, `Clear2`, `Clear3` |
| `LinesCleared{4}` | `Tetris` |
| `BombExploded` | `Bomb` |
| `GarbageRose` | `Garbage` |
| `LevelUp` | `LevelUp` |
| `ToppedOut` | `GameOver` solo, `KoTheirs` in a duel |
| `CountdownTick` / `MatchStart` | `CountdownTick` / `Go` |
| `Attacked` | `Attacked` |
| `KoMine` | `KoMine` |
| `Won` / `Lost` / `Draw` | `Won` / `Lost` / `Draw` |

`App::step` reads events once per frame, after the game has been advanced **and** after `duel.update()`. It takes events from the active solo game or the duel game and from the `DuelSession`, maps them, and passes each resulting `Sound` to the mixer.

## 3. `tetris_audio` (new library, `src/audio/`, no SDL)

### `sound.hpp`
`enum class Sound { Move, Rotate, Hold, HardDrop, Lock, Clear1, Clear2, Clear3, Tetris, Bomb, Garbage, LevelUp, GameOver, CountdownTick, Go, Attacked, KoMine, KoTheirs, Won, Lost, Draw, Count };`

### `synth.{hpp,cpp}`
- `std::vector<float> render(Sound sound, int sampleRate)`.
- Each sound is a short list of notes. Each note has a waveform (`Square`, `Triangle` or `Noise`), a start and end frequency (a linear slide), a start offset, a duration, a volume, and a linear attack (≤ 5 ms) and decay.
- Overall levels: quiet cues (`Move`, `Rotate`, `Lock`, `CountdownTick`) at about 0.3; big cues (`Tetris`, `Bomb`, `KoMine`, `KoTheirs`, `Won`) at about 0.8; everything else in between.
- Character: `Move` is a ~25 ms quiet square blip; `Rotate` is a slightly higher blip; `Hold` is a two-note chirp; `HardDrop` is a low noise thump; `Lock` is a soft click; `Clear1–3` are rising arpeggios that get longer and higher with more lines; `Tetris` is a four-note fanfare; `Bomb` is a noise burst with a downward slide; `Garbage` is a low rumble; `LevelUp` is a major-chord sting; `GameOver` is a descending slide; `Go` is a higher version of `CountdownTick`; `Won`/`Lost`/`Draw` are short jingles (rising, falling, flat).
- Every sound is shorter than 1.5 s. The noise generator uses a fixed seed, so the output is deterministic.

### `mixer.{hpp,cpp}`
- `Mixer(int sampleRate)` renders every `Sound` once into a cache.
- `play(Sound)` starts a voice, which is a cache index plus a position. Up to 8 voices play at once; a 9th replaces the oldest.
- `mix(float* out, int frames)` writes the sum of the active voices × master gain (0.5), clamped to [-1, 1]. Voices that reach the end are removed.
- `setMuted(bool)` / `muted()`. While muted, `mix` writes silence and `play` is ignored, so muted sounds don't build up and burst out on unmute.

## 4. `AudioOut` (`src/app/audio_out.{hpp,cpp}`, the only SDL audio code)

- At startup: `SDL_InitSubSystem(SDL_INIT_AUDIO)`, then `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, {F32, 1 channel, 48000 Hz}, callback, this)`, then resume the device.
- The callback asks the `Mixer` for exactly the requested number of frames and puts them on the stream.
- `play()` and `setMuted()` from the main thread run under `SDL_LockAudioStream` / `SDL_UnlockAudioStream`, which serialises them with the callback.
- If any step fails, it logs one line to stderr, and from then on `play()` and `setMuted()` do nothing (the mute flag is still tracked for the UI). The game starts and runs normally.
- **Browser:** SDL 3.4.2's Emscripten backend resumes the suspended `AudioContext` on the first user input (`autoResumeAudioContext`), so no extra code is needed. Sounds triggered before the first input are dropped by the browser, and that's fine.
- `App` owns `AudioOut`. `main.cpp` keeps calling `SDL_Init(SDL_INIT_VIDEO)`, and `AudioOut` adds the audio subsystem itself.

## 5. Mute

- **`M` key:** toggles mute on any screen, unless an ImGui text field has focus (`ImGui::GetIO().WantTextInput`), so typing a room code that contains `M` doesn't mute. `M` is handled in `handleKey` before the active-game check, not through `InputMapper`.
- **Menu item:** `Sound: On` / `Sound: Off` on the start menu and the pause menu. Choosing it emits a new `UiCommand::ToggleSound`.
- **Indicator:** while muted, a small `MUTED` label is drawn in a corner on every screen.
- The setting lasts for the current session only and is not saved. It starts unmuted.

## 6. Build

- New `tetris_audio` static library (`src/audio/synth.cpp`, `src/audio/mixer.cpp`) with the project warnings, linked into `tetris` and `tetris_tests`.
- `src/app/sound_cues.cpp` is added to `tetris_duel`, which links `tetris_audio` for the `Sound` enum.
- `src/app/audio_out.cpp` is added to the `tetris` executable (desktop and web).
- README: add `M` (mute) to the controls.

## 7. Tests (doctest, native)

- **`game_test.cpp`:**
  - A successful move or rotation emits `Moved` / `Rotated`; a blocked move emits nothing.
  - A hold emits `Held`; a second hold before the next lock emits nothing.
  - A hard drop with no clear emits exactly `HardDropped`; a hard drop that completes 4 rows emits `HardDropped, LinesCleared{4}`.
  - A natural lock emits `Locked`.
  - Landing on a bomb emits `BombExploded`, and its rows are not counted in `LinesCleared`.
  - Garbage rising after a lock emits `GarbageRose`.
  - Crossing a 10-line boundary emits `LevelUp`.
  - Block-out, lock-out and garbage overflow each emit a single `ToppedOut`.
  - `takeEvents()` empties the list; `resetBoard()` keeps pending events.
- **`duel_test.cpp`:**
  - The countdown emits 3 `CountdownTick`s and then `MatchStart`.
  - An `Attack` message emits `Attacked`.
  - `KoCounts` raising our count emits `KoMine`; raising theirs emits nothing.
  - `OVER` emits `Won`/`Lost`/`Draw`; the opponent leaving emits nothing.
- **`sound_cues_test.cpp`:** the mapping table, including `ToppedOut` → `GameOver` in solo and `KoTheirs` in a duel, and `LinesCleared{4}` → `Tetris`.
- **`audio_test.cpp`:**
  - Every `Sound` renders a buffer that isn't empty, is shorter than 1.5 s, stays within ±1 and has a peak above 0.05.
  - Rendering is deterministic.
  - `Mixer` with no voices gives silence; two voices add together and are clamped; muted gives silence and ignores `play`; a 9th voice replaces the oldest; finished voices are removed.
- **By hand** (`AudioOut` has no automated test): hear each sound on the desktop and in the browser; `M` mutes and shows `MUTED`; typing `M` in the room code doesn't mute; the menu toggle works; with no output device, the game starts silently.
