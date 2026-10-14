# Design: Handling settings (DAS, ARR, SDF)

## Context
Auto-shift and soft drop are compile-time constants today: `kDasMs = 167` and `kArrMs = 33` in `src/app/input.hpp`, and `kSoftDropFactor = 20` in `src/core/game.hpp`. Competitive players expect to tune all three, as they can in TETR.IO and Jstris. Most of them play with a short DAS, ARR 0 (the piece goes straight to the wall) and instant soft drop. Without these settings, experienced players feel slow here no matter how good they are.

This adds the three settings to a Handling section on the Controls screen. They are saved with the rest of the settings and apply in solo and duels.

## Goals and non-goals
- **Goals:**
  - Player-set DAS (delay before auto-shift), ARR (auto-repeat rate) and SDF (soft-drop factor), in milliseconds and multiples.
  - ARR 0 moves the piece straight to the wall. SDF "Instant" drops the piece straight to the floor while soft drop is held, without locking it.
  - The defaults stay the current values, so nobody's game changes until they change a setting.
  - Saved in the existing settings file. Older builds ignore the new lines, and files saved by older builds load with the defaults.
- **Non-goals (possible follow-ups):**
  - 180° rotation. When it comes, it should use the TETR.IO SRS+ 180 kick table.
  - DCD (the brief auto-shift pause after a rotate, hold or hard drop).
  - Keeping the DAS charge when switching directions. Today a switch restarts DAS, and that stays.
  - Showing values in frames.
  - Any server or protocol change. Handling is purely client-side: each client simulates its own `Game`, and the opponent only sees snapshots and attacks.

## 1. The settings

| Setting | Default | Range | Meaning |
|---|---|---|---|
| DAS | 167 ms | 0–500 ms, step 1 | How long a direction is held before it starts repeating |
| ARR | 33 ms | 0–100 ms, step 1 | Time between repeats once DAS has charged. **0 = straight to the wall** |
| SDF | 20× | 5–40×, plus **Instant** | Soft-drop gravity as a multiple of normal gravity. Instant drops to the floor right away |

They live in one struct in `src/app/input.hpp` beside `InputMapper` (it has no SDL dependency, so `settings.cpp` and the tests can use it):

```cpp
// kSoftDropInstant (0) comes from core/game.hpp, see section 3.
struct Handling {
    int dasMs = 167;
    int arrMs = 33;              // 0: shift straight to the wall
    int softDropFactor = 20;     // kSoftDropInstant, or kMinSdf to kMaxSdf
    friend bool operator==(const Handling&, const Handling&) = default;
};
Handling clampHandling(Handling h);  // brings each field into its range; any other SDF below kMinSdf becomes kMinSdf
```

The range constants (`kMinDasMs`, `kMaxDasMs`, `kMinArrMs`, `kMaxArrMs`, `kMinSdf`, `kMaxSdf`) go next to it. `kDasMs` and `kArrMs` are removed, and the defaults live only in `Handling`'s member initializers.

## 2. Auto-shift (`InputMapper`)

- `InputMapper` gains `void setHandling(const Handling&)` and keeps a copy. `tick()` uses it in place of `kDasMs` and `kArrMs`.
- **`releaseAll()` must keep the handling.** Today it does `*this = InputMapper{}`, which would quietly reset everything to the defaults on every new game. It should save the handling, reset, and put it back.
- **DAS 0:** a press still makes one immediate move. The first tick after it counts as DAS already charged, so ARR takes over from there.
- **ARR 0:** once DAS has charged, every tick emits a new `Action::LeftWall` / `Action::RightWall` (one per tick, not one per cell), for as long as the key is held. Emitting it every tick, rather than once, means a piece that spawns, rotates or is held while the key is down slides at once, as in TETR.IO.
- Changing the handling while a direction is held (the player can't reach the Controls screen mid-move, but this keeps it well-defined) applies from the next tick. The current `dasMs_`/`arrMs_` counters are kept.

## 3. Shift to wall and instant soft drop (`Game`, `tetris_core`)

### `Action::LeftWall` and `Action::RightWall`
- New `Action` values. `Game::apply` moves the piece one cell at a time in that direction until the next cell is blocked.
- If it moved at all: call `onPlayerMoved()` **once** (one lock reset, the same as a single tap) and emit **one** `Moved` event, so the sound plays once instead of up to nine times. If it couldn't move, it does nothing and emits nothing, so holding against the wall at ARR 0 is silent.
- `rotatedLast_` is cleared as in `tryMove`, and `updateLowest()` runs as usual.

### Soft-drop factor
- `kSoftDropFactor` is removed. `Game` gains `void setSoftDropFactor(int factor)` (default 20; `kSoftDropInstant` = instant), stored in `softDropFactor_`. `kSoftDropInstant` is declared in `core/game.hpp` and `input.hpp` reuses it, so the core doesn't depend on the app.
- `step()` with soft drop held:
  - **Factor N:** unchanged, `dropTimerMs_ += N`.
  - **Instant:** move down `ghostDistance()` rows in one step, scoring 1 point per row like any soft drop. Don't lock: the piece sits on the floor and the normal lock delay runs, so the player can still slide and spin it. A piece that rises (a kick) or a newly spawned piece drops again on the next tick while the key is held.
- Solo and duels use the same rule. A duel opponent sees the result only through snapshots, so it doesn't matter that the two players' factors differ.

## 4. Settings file (`settings.cpp`)

- Three new lines, written after `public=`: `das=167`, `arr=33`, and `sdf=20` or `sdf=instant`.
- Parsing: integers with `std::from_chars` (the whole value must parse), then `clampHandling`. A missing or unparsable line keeps that field's default. `version` stays `"1"`: older builds already skip unknown names (`actionIndex` returns `nullopt`), so no version bump is needed.
- `Settings` gains `Handling handling;` as its last member, so the existing aggregate construction in `App::saveSettings()` only needs one more argument.

## 5. The Handling section on the Controls screen

Under the key table, before the "Esc cancels…" hint:

```
HANDLING
DAS   [=====|--------]  167 ms
ARR   [==|-----------]   33 ms
SDF   [=====|--------]  20x
```

- Three `ImGui::SliderInt`s with `ImGuiSliderFlags_AlwaysClamp`, styled by the existing arcade style (`SliderGrab` is already themed). The formats are `"%d ms"`. ARR shows `"0 ms (instant)"` at 0. For SDF the slider runs from `kMinSdf` to `kMaxSdf + 1`, where the top step stands for Instant and its label reads `"Instant"`. A small helper maps between slider position and `softDropFactor` so this is testable.
- Keyboard navigation works as for the start-level slider: Up/Down moves between rows, Left/Right changes the value.
- **Saving:** a slider that finishes an edit (`IsItemDeactivatedAfterEdit`) returns a new `UiCommand::SaveHandling`. `App` handles it by pushing the handling to `input` and `game`, then calling `saveSettings()`. The values themselves are edited in place in a `Handling&` passed to `drawControlsScreen`, so the slider moves smoothly while dragging.
- **Reset to defaults** resets the handling as well as the keys. It's one button, with a "Keys and handling reset" note so nobody is surprised.
- The Controls window gets taller. The plan checks it still fits at the smallest supported window (the 640×720 desktop minimum set in `main.cpp`, and a short browser window). If not, the menu window becomes scrollable.

## 6. Where the handling is applied (`App`)

- `App` holds `Handling handling`, loaded from `Settings` in the constructor and passed to `input.setHandling` straight away.
- `step()` calls `game->setSoftDropFactor(handling.softDropFactor)` next to the existing `setSoftDrop` call, so a paused solo game picks up a change made on the Controls screen the moment it resumes. A duel picks it up at once as well, though the menus can't be reached during a match.

## 7. Tests (doctest)

**`tests/input_test.cpp`:**
- The existing DAS/ARR tests use `Handling{}` values instead of the removed constants.
- A custom DAS and ARR (e.g. 100/10) repeat on that schedule.
- DAS 0: the press moves, then repeats begin on the next ticks at the ARR rate.
- ARR 0: nothing during DAS; once DAS charges, `RightWall` is emitted every tick.
- `releaseAll()` keeps the handling.
- Releasing and falling back to the other direction still restarts DAS with the custom value.

**`tests/game_test.cpp`:**
- `LeftWall` / `RightWall` move to the wall, stop at a block in the way, emit exactly one `Moved` event and give one lock reset.
- At the wall, a wall shift emits nothing.
- Instant soft drop puts the piece on the floor in one tick, scores 1 point per row, and doesn't lock until the lock delay runs out.
- A factor of 5 and of 40 fall at those speeds, like the existing test for 20.
- A wall shift clears `rotatedLast_`, so a T-spin is not awarded after it.

**`tests/settings_test.cpp`:**
- The handling round-trips, including `sdf=instant`.
- Out-of-range values clamp, junk keeps the defaults, and a file without the lines loads the defaults.

**`tests/ui_test.cpp`:**
- The Controls screen draws with the Handling section. Moving keyboard focus onto the DAS slider and pressing Right raises DAS.
- The SDF slider-position helper maps both ways, Instant included.

## 8. Docs
README, Controls section: one paragraph on the Handling sliders and what ARR 0 and SDF Instant do.
