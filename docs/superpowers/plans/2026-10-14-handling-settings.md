# Handling Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Player-set DAS, ARR (0 = straight to the wall) and SDF (with Instant), saved with the settings and edited in a Handling section on the Controls screen. Spec: `docs/superpowers/specs/2026-10-14-handling-settings-design.md`.

**Architecture:** `tetris_core` gains wall shifts (`Action::LeftWall`/`RightWall`) and a settable soft-drop factor. `InputMapper` takes a `Handling` struct in place of the DAS/ARR constants. `settings.cpp` stores it. `render.cpp` draws three sliders, and `App` passes the values to the mapper and the game.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build.

## Global Constraints

- The defaults (DAS 167 ms, ARR 33 ms, SDF 20×) must reproduce today's behavior exactly. The existing tests keep passing, rewritten only to use `Handling{}` where they used the removed constants.
- `tetris_core` must not include anything from `src/app`. `kSoftDropInstant` lives in `core/game.hpp`.
- Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`.
- The server and protocol are untouched.
- Conventional commits scoped like the existing history (`feat(core): …`, `feat(app): …`). Work on `develop`.

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/core/game.hpp/.cpp` | `Action::LeftWall/RightWall`, `kSoftDropInstant`, `setSoftDropFactor`, remove `kSoftDropFactor` |
| `src/app/input.hpp/.cpp` | `Handling`, range constants, `clampHandling`, `InputMapper::setHandling`, ARR 0, remove `kDasMs`/`kArrMs` |
| `src/app/settings.hpp/.cpp` | `Settings::handling`, `das=`/`arr=`/`sdf=` lines |
| `src/app/render.hpp/.cpp` | Handling sliders, `UiCommand::SaveHandling`, SDF slider helpers |
| `src/app/app.hpp/.cpp` | `App::handling`, applying and saving it, reset |
| `tests/{game,input,settings,ui}_test.cpp` | see each task |
| `README.md` | Controls paragraph |

---

### Task 1: Wall shifts in `Game`

**Files:** `src/core/game.hpp`, `src/core/game.cpp`, `tests/game_test.cpp`

**Interfaces:**
- Produces: `Action::LeftWall`, `Action::RightWall` (appended after `Pause` so existing values keep their order).

- [ ] **Step 1: Write the failing tests** (`game:` prefix like the rest of the file)
  - On an empty board, `RightWall` moves the spawned piece until its rightmost cell is in column 9. `takeEvents()` holds exactly one `Moved`.
  - A filled cell in the path stops the piece just before it.
  - Another `RightWall` at the wall emits no event and leaves the piece where it is.
  - On the ground, a wall shift resets the lock timer once. Do it `kMaxLockResets` times along with taps and check the piece still locks, as in the existing lock-reset test.
  - A T rotated into a T-spin slot and then wall-shifted earns no T-spin. Use the existing T-spin fixture with a column that lets it move at least one cell.
- [ ] **Step 2: Implement**

```cpp
// in Game::apply's switch
case Action::LeftWall:
case Action::RightWall:
    shiftToWall(action == Action::LeftWall ? -1 : 1);
    break;

void Game::shiftToWall(int dx) {
    bool moved = false;
    while (tryMove(dx, 0)) moved = true;
    if (!moved) return;
    onPlayerMoved();
    emit(GameEventType::Moved);
}
```

  `tryMove` already clears `rotatedLast_` and calls `updateLowest()`. Check `sound_cues.cpp` and any `switch (action)` (`grep -rn "Action::Hold" src`) for switches that need the new cases to compile warning-free.
- [ ] **Step 3: Build and run tests.** All pass.
- [ ] **Step 4: Commit:** `feat(core): shift-to-wall actions`

---

### Task 2: Settable soft-drop factor with Instant

**Files:** `src/core/game.hpp`, `src/core/game.cpp`, `tests/game_test.cpp`

**Interfaces:**
- Produces: `inline constexpr int kSoftDropInstant = 0;`, `void Game::setSoftDropFactor(int factor)`, `int softDropFactor_ = 20;`
- Removes: `kSoftDropFactor`. Its one use in `step()` and the test "soft drop is 20x gravity" switch to the member and the literal 20.

- [ ] **Step 1: Write the failing tests**
  - Factors 5 and 40 fall at 5× and 40× gravity at level 1. Copy the existing 20× test and vary the factor.
  - Instant: with soft drop held, one `tick(1)` puts the piece at `ghostDistance() == 0`, score += rows fallen, state is still `Playing` and the piece hasn't locked. After `kLockDelayMs` more ticks it locks.
  - Instant after a hold: the new piece also reaches the floor on the next tick.
- [ ] **Step 2: Implement.** In `step()`, after the `onGround()` branch:

```cpp
if (softDrop_ && softDropFactor_ == kSoftDropInstant) {
    const int rows = ghostDistance();
    active_.pos.y -= rows;
    if (rows > 0) rotatedLast_ = false;
    score_ += rows;
    updateLowest();
    dropTimerMs_ = 0.0;
    return;
}
dropTimerMs_ += softDrop_ ? softDropFactor_ : 1;
```

  Clearing `rotatedLast_` matches a normal soft drop, which goes through `tryMove`. The lock delay starts on the next tick through the existing `onGround()` branch.
- [ ] **Step 3: Build and run tests.**
- [ ] **Step 4: Commit:** `feat(core): settable soft-drop factor with an instant option`

---

### Task 3: `Handling` and the input mapper

**Files:** `src/app/input.hpp`, `src/app/input.cpp`, `tests/input_test.cpp`

**Interfaces:**
- Produces: `struct Handling`, `clampHandling`, `kMinDasMs = 0`, `kMaxDasMs = 500`, `kMinArrMs = 0`, `kMaxArrMs = 100`, `kMinSdf = 5`, `kMaxSdf = 40`, `InputMapper::setHandling(const Handling&)`, `const Handling& handling() const`.
- Removes: `kDasMs`, `kArrMs`.

- [ ] **Step 1: Rewrite the existing tests** to read `Handling{}.dasMs` / `.arrMs`, then add the failing ones:
  - `setHandling({100, 10, 20})`: the first repeat comes at 100 ms, then one every 10 ms.
  - DAS 0: the press emits one `Right`, then the next ticks repeat at the ARR rate.
  - ARR 0: no actions during DAS; once it charges, one `RightWall` per tick, every tick.
  - `releaseAll()` after `setHandling` keeps the handling (`input.handling() == custom`).
  - `clampHandling`: values above and below range clamp, `kSoftDropInstant` stays, an SDF of 3 becomes 5.
- [ ] **Step 2: Implement.** `tick()`:

```cpp
void InputMapper::tick(std::vector<Action>& out) {
    if (dir_ == 0) return;
    if (dasMs_ < handling_.dasMs) {
        if (++dasMs_ < handling_.dasMs) return;
        if (handling_.arrMs > 0) {  // the shift that ends DAS; at ARR 0 the wall shift below takes its place
            out.push_back(shiftAction(dir_));
            return;
        }
    }
    if (handling_.arrMs == 0) {
        out.push_back(dir_ < 0 ? Action::LeftWall : Action::RightWall);
        return;
    }
    if (++arrMs_ >= handling_.arrMs) {
        arrMs_ = 0;
        out.push_back(shiftAction(dir_));
    }
}
```

  With DAS 0, `dasMs_ < 0` is false on the first tick, so ARR takes over at once, which is what the spec asks for. `releaseAll()` becomes `const Handling h = handling_; *this = InputMapper{}; handling_ = h;`.
- [ ] **Step 3: Build and run tests.** Note: with the defaults this `tick()` must emit exactly what the old one did. The rewritten old tests prove that.
- [ ] **Step 4: Commit:** `feat(app): configurable DAS and ARR, with ARR 0 shifting to the wall`

---

### Task 4: Save the handling

**Files:** `src/app/settings.hpp`, `src/app/settings.cpp`, `tests/settings_test.cpp`

- [ ] **Step 1: Failing tests**
  - `{das 80, arr 0, sdf instant}` round-trips through serialize/parse.
  - `das=9999` → 500, `arr=-5` → 0, `sdf=2` → 5.
  - `das=abc`, `das=12x` and an empty value keep the defaults.
  - A version-1 file from before this change (no handling lines) loads `Handling{}`.
- [ ] **Step 2: Implement.** `Settings` gets `Handling handling;` last. Write `das=`, `arr=`, `sdf=` (`instant` for `kSoftDropInstant`) after `public=`. Parse with a small `std::optional<int> parseInt(std::string_view)` built on `from_chars`, the same way `parseScancode` already parses. Run `clampHandling` on the result once after the loop.
- [ ] **Step 3: Build and run tests.**
- [ ] **Step 4: Commit:** `feat(app): save the handling settings`

---

### Task 5: Apply the handling in `App`

**Files:** `src/app/app.hpp`, `src/app/app.cpp`

- [ ] **Step 1: Implement**
  - `App` gets `Handling handling;`. The constructor sets it from `saved.handling` and calls `input.setHandling(handling)`.
  - `App::saveSettings()` passes `handling` as the last aggregate member.
  - `step()`: next to `game->setSoftDrop(app.input.softDrop());` add `game->setSoftDropFactor(app.handling.softDropFactor);`.
  - New `UiCommand::SaveHandling` case: `handling = clampHandling(handling); input.setHandling(handling); saveSettings();`.
  - `UiCommand::ResetControls` also does `handling = {}; input.setHandling(handling);` and sets the note `"Keys and handling reset"`.
- [ ] **Step 2: Build and run tests.** Everything passes. The app behaves as before because the defaults are unchanged.
- [ ] **Step 3: Commit:** `feat(app): apply the saved handling to input and soft drop`

---

### Task 6: Handling sliders on the Controls screen

**Files:** `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.cpp`, `tests/ui_test.cpp`

**Interfaces:**
- `drawControlsScreen(const Fonts&, const Keymap&, Handling&, ControlsMenu&)` gains `Handling&`. Update the call in `app.cpp` and the test helper `controlsFrame`.
- `int sdfSliderValue(int softDropFactor)` and `int sdfFromSlider(int value)`, declared in `render.hpp` for testing: Instant ↔ `kMaxSdf + 1`.

- [ ] **Step 1: Failing tests**
  - `sdfSliderValue(kSoftDropInstant) == kMaxSdf + 1`, `sdfFromSlider(kMaxSdf + 1) == kSoftDropInstant`, and the two map 20 ↔ 20.
  - The Controls screen draws with a `Handling`. Navigate focus down past the key rows to the DAS slider (the number of Down presses = `kActionCount`) and press Right; `handling.dasMs` goes up. On release, the frame returns `UiCommand::SaveHandling`. Adjust if ImGui reports the edit on a later frame.
- [ ] **Step 2: Implement.** After `EndTable()`:

```cpp
ImGui::Spacing();
menuSubtitle(fonts, "HANDLING");  // or the existing section-label style; check render.cpp for one first
bool edited = false;
auto slider = [&](const char* label, const char* id, int* value, int lo, int hi, const char* format) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(sliderWidth);
    ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    edited |= ImGui::IsItemDeactivatedAfterEdit();
};
slider("DAS", "##das", &handling.dasMs, kMinDasMs, kMaxDasMs, "%d ms");
slider("ARR", "##arr", &handling.arrMs, kMinArrMs, kMaxArrMs, handling.arrMs == 0 ? "0 ms (instant)" : "%d ms");
int sdf = sdfSliderValue(handling.softDropFactor);
slider("SDF", "##sdf", &sdf, kMinSdf, kMaxSdf + 1, sdf > kMaxSdf ? "Instant" : "%dx");
handling.softDropFactor = sdfFromSlider(sdf);
if (edited) command = UiCommand::SaveHandling;
```

  The width of the sliders matches the key table: the label column is the width of "Rotate CCW", and the slider is the width of the two key slot columns together.
- [ ] **Step 3: Check by eye.** Desktop build at the 640×720 minimum window size. The whole Controls menu, including Back, is visible. If not, give `beginMenu` a max height and let the window scroll (check where `beginMenu` sets its size). Also check the web build in a short browser window. Play solo with ARR 0 and SDF Instant, and play a LAN duel against yourself in two tabs to check a match plays normally with different handling on each side.
- [ ] **Step 4: Build and run tests.**
- [ ] **Step 5: Commit:** `feat(app): handling sliders on the Controls screen`

---

### Task 7: README

- [ ] In the Controls section, after the key-binding paragraph, add: "The Handling section under the keys sets DAS (how long a held direction waits before repeating, default 167 ms), ARR (time between repeats, default 33 ms; 0 moves the piece straight to the wall) and SDF (soft-drop speed as a multiple of gravity, default 20×; Instant drops the piece to the floor without locking it). Reset to defaults resets them along with the keys."
- [ ] Commit: `docs: handling settings`
