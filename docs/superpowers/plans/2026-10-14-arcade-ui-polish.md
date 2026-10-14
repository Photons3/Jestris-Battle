# Arcade UI Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every screen use the same language as the beveled nameplates (`drawNameBar`) and the minos themselves: flat colors, bevels, hard pixel shadows, no smooth gradients or stock widget looks.

| # | Today | After |
|---|---|---|
| 1 | Navy radial glow from 16 stacked translucent discs | Flat dark fill with a faint one-cell grid lined up with the wells |
| 2 | Stock `StyleColorsDark()` menus, default font, green invite button | Square arcade style, Press Start 2P everywhere, beveled buttons, gold accent |
| 3 | `KO 1 : 0` in the center, repeating the plates | `COMBO x3`, moved from under HOLD in duels |
| 4 | Big gold `VS` during play | `VS` on the countdown screen; during play a small `FIRST TO 3` label |
| 5 | Plain red garbage bar | Stacked gray garbage cells, turning red and blinking at 4+ lines |
| 6 | Plain white countdown and names | Hard drop shadow, a pop on each digit, `GO!` when play starts |

**Architecture:** Everything stays in `src/app/render.cpp` except the style hook in `main.cpp` and the default font in `fonts.cpp`. Timing rules that can be tested become small pure functions declared in `render.hpp`, like `ComboLabel` today. Drawing code is checked by eye.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build.

**Builds on:** commit `feat(app): beveled block nameplates in duels`, which added `bevelFor`, `drawBevelRect` and the plate colors (`kMyPlateColor`, `kTheirPlateColor`, `kKoPipColor`).

## Global Constraints

- Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`. Desktop Debug builds keep ASan/UBSan.
- No smooth gradients and no rounded corners anywhere. Pixel positions that make up a bevel or a grid line are `std::floor`ed so edges stay crisp.
- Colors come from the existing `Rgb` helpers (`lighten`, `darken`, `color`) or new `constexpr Rgb` constants next to the plate colors. No new ad-hoc `IM_COL32` literals except pure black or white.
- Solo play keeps its COMBO label under HOLD. Only the duel moves it to the center.
- The server and protocol are untouched.
- Conventional commits scoped like the existing history (`feat(app): …`). Work on `develop`.

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- UI tests only: `./build/tetris_ui_tests -tc="ui:*"`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/app/render.hpp` | `countdownPopScale`, `showGoBanner`, `garbageMeterDanger`, `applyArcadeStyle`, their constants |
| `src/app/render.cpp` | `drawShadowText`, grid background, center column, countdown/VS/GO, garbage cells, menu bevels |
| `src/app/fonts.cpp` | Press Start 2P becomes the default font |
| `src/app/main.cpp` | calls `applyArcadeStyle` after `StyleColorsDark` |
| `tests/ui_test.cpp` | tests for the pure helpers and the style |

---

### Task 1: Hard-shadow text helper

**Files:**
- Modify: `src/app/render.cpp`

**Interfaces:**
- Produces: `void drawShadowText(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, ImU32 col, const char* text, float offset, ImU32 shadow = IM_COL32(0, 0, 0, 220))`, in the anonymous namespace, plus a centered variant `drawCenteredShadow(…, float centerX, float y, …)`.

- [ ] **Step 1: Implement**

Put it just after `drawBevelRect`:

```cpp
// Text with a hard, unblurred shadow `offset` points down and right, like the nameplates.
void drawShadowText(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, ImU32 col, const char* text,
                    float offset, ImU32 shadow = IM_COL32(0, 0, 0, 220)) {
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    dl->AddText(fonts.hud, size, ImVec2(pos.x + offset, pos.y + offset), shadow, text);
    dl->AddText(fonts.hud, size, pos, col, text);
}
```

Have `drawNameBar` use it instead of its two `AddText` calls (shadow color `color(darken(darken(base)), 1.0f)`, offset `bevel`). Add `drawCenteredShadow` beside `drawCentered`, measuring with `CalcTextSizeA` the same way.

- [ ] **Step 2: Build and run tests**

Build and run the full test command. Everything passes and the nameplates look the same as before.

- [ ] **Step 3: Commit**

```bash
git commit -am "refactor(app): shared hard-shadow text helper"
```

---

### Task 2: Grid background (item 1)

**Files:**
- Modify: `src/app/render.cpp`

- [ ] **Step 1: Implement**

Add, next to the plate colors:

```cpp
constexpr Rgb kBackdropColor{6, 10, 28};
constexpr Rgb kGridColor{16, 24, 50};
```

Replace `drawDuelBackground()` with `drawGridBackground(float cell, ImVec2 anchor)`:
- Fill the whole viewport with `kBackdropColor` on `ImGui::GetBackgroundDrawList()`.
- Draw 1px vertical and horizontal lines in `kGridColor` every `cell` points. The lines are offset so that one passes through `anchor`, which keeps the grid lined up with the wells' cells:

```cpp
const float startX = anchor.x - std::ceil((anchor.x - vp->Pos.x) / cell) * cell;
for (float x = std::floor(startX); x < vp->Pos.x + vp->Size.x; x += cell)
    bg->AddRectFilled(ImVec2(x, vp->Pos.y), ImVec2(x + 1.0f, vp->Pos.y + vp->Size.y), color(kGridColor, 1.0f));
// same for y with anchor.y
```

Use filled 1px rects, not `AddLine`, so the lines aren't anti-aliased across two pixels.

Call sites:
- `drawDuelBoards`: `drawGridBackground(cell, layout.mine.origin)`.
- `drawGame` (solo): `const Layout layout = computeLayout(); drawGridBackground(layout.cell, layout.origin);` then pass `layout` to `drawPlayfield`. Solo had no backdrop before, so this makes both modes match.

Delete the comment about faking a radial gradient.

- [ ] **Step 2: Check by eye**

Desktop build, then start a solo game. The grid is faint (it should be barely visible next to the board), the lines fall exactly on the cell borders of the well, and the well's own dark fill still covers the grid inside it. Resize the window: the grid stays lined up.

- [ ] **Step 3: Commit**

```bash
git commit -am "feat(app): grid backdrop instead of the radial glow"
```

---

### Task 3: Center column: COMBO replaces KO, FIRST TO 3 replaces VS (items 3 and 4, play half)

**Files:**
- Modify: `src/app/render.cpp`

**Interfaces:**
- `drawScoreboard(dl, fonts, layout, duel, combo)` gains `const ComboLabel& combo`.
- `drawPlayfield(…, const ComboLabel& combo, bool comboUnderHold)` gains the flag. `drawGame` passes `true`, `drawDuelBoards` passes `false`.

- [ ] **Step 1: Implement**

New center stack in `drawScoreboard`, top to bottom (y steps unchanged where possible):

1. `TIME` and the clock, as today.
2. Where `VS` was: `FIRST TO 3` (built from `kKosToWin`) at `cell * 0.45f`, in `kKoPipColor`, with a hard shadow. It explains the pips on the plates.
3. Where `KO` was: the label `COMBO`, then the value `x3` from `combo.shown`, drawn with alpha `combo.alpha()` so it fades like the solo label. When `combo.shown == 0` draw `-` in the label color instead.
4. `SENT` as today.

Remove the `KO` line and the `VS` line. In `drawPlayfield`, only call `drawComboLabel` when `comboUnderHold` is true.

- [ ] **Step 2: Check by eye**

In a duel: clear lines back to back and the center shows `x1`, `x2`, … then fades to `-` after the combo breaks. Nothing shows under HOLD. In solo the combo is still under HOLD.

- [ ] **Step 3: Commit**

```bash
git commit -am "feat(app): combo and match target in the duel scoreboard"
```

---

### Task 4: Countdown, VS and GO! (items 4 and 6)

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Produces, in `render.hpp`:

```cpp
inline constexpr int kCountdownPopMs = 150;  // each digit settles from 1.3x to 1x over this long
inline constexpr int kGoBannerMs = 400;      // "GO!" shows for this long once play starts

float countdownPopScale(int countdownMs);  // 1.3 the frame a digit appears, easing to 1.0
bool showGoBanner(int matchClockMs);       // true for the first kGoBannerMs of the match clock
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ui: each countdown digit pops in, then settles") {
    CHECK(countdownPopScale(3000) == doctest::Approx(1.3f));  // "3" just appeared
    CHECK(countdownPopScale(3000 - kCountdownPopMs / 2) == doctest::Approx(1.15f));
    CHECK(countdownPopScale(3000 - kCountdownPopMs) == doctest::Approx(1.0f));
    CHECK(countdownPopScale(2500) == doctest::Approx(1.0f));
    CHECK(countdownPopScale(2000) == doctest::Approx(1.3f));  // "2" just appeared
}

TEST_CASE("ui: GO! shows only at the very start of the match") {
    CHECK(showGoBanner(kMatchMs));
    CHECK(showGoBanner(kMatchMs - kGoBannerMs + 1));
    CHECK_FALSE(showGoBanner(kMatchMs - kGoBannerMs));
    CHECK_FALSE(showGoBanner(0));
}
```

`kMatchMs` comes from `app/duel.hpp`, which `render.hpp` already includes.

- [ ] **Step 2: Run them and watch them fail**

`./build/tetris_ui_tests -tc="ui:*"` fails to compile.

- [ ] **Step 3: Implement the helpers**

```cpp
float countdownPopScale(int countdownMs) {
    // The digit shown is ceil(ms / 1000); it appeared when ms was last a whole second.
    const int sinceShown = (1000 - countdownMs % 1000) % 1000;
    const float t = std::min(1.0f, static_cast<float>(sinceShown) / kCountdownPopMs);
    return 1.3f - 0.3f * t;
}

bool showGoBanner(int matchClockMs) { return matchClockMs > kMatchMs - kGoBannerMs; }
```

- [ ] **Step 4: Implement the drawing**

- `drawCountdown`: draw the digit at `size * countdownPopScale(ms)`, still centered, with `drawCenteredShadow` and an offset of `std::floor(size * 0.06f)`.
- `drawVersus` becomes three pieces on one line: the local name in white, `VS` in `kKoPipColor`, the opponent name in white. Measure all three with `CalcTextSizeA`, center the whole line, and give each piece the hard shadow. Keep the "nothing until both names are known" rule.
- New `drawGo(const Fonts&)`: `GO!` in the KO flash style (orange 8-direction outline, gold fill) at `vp->Size.y * 0.15f`, centered on the screen, on the foreground draw list. Factor the outline loop out of `drawKoFlash` into `drawOutlinedText(dl, fonts, size, pos, fill, outline, thickness)` and use it in both.
- `drawDuel`, `DuelPhase::Playing` case: after `drawDuelBoards`, `if (showGoBanner(duel.matchClockMs())) drawGo(fonts);`.

- [ ] **Step 5: Run the tests and check by eye**

Tests pass. In a duel: each digit pops in, `JESTER VS LUMINE` has a gold `VS` and shadows, and `GO!` flashes for under half a second when play starts and never again (not after a KO, not after a rematch countdown until play starts again).

- [ ] **Step 6: Commit**

```bash
git commit -am "feat(app): countdown pop, VS line and GO! banner"
```

---

### Task 5: Garbage meter as cells (item 5)

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Produces, in `render.hpp`:

```cpp
inline constexpr int kGarbageDangerLines = 4;  // the meter turns red and blinks from this many pending lines
bool garbageMeterDanger(int pending);
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("ui: the garbage meter warns from four pending lines") {
    CHECK_FALSE(garbageMeterDanger(0));
    CHECK_FALSE(garbageMeterDanger(kGarbageDangerLines - 1));
    CHECK(garbageMeterDanger(kGarbageDangerLines));
}
```

- [ ] **Step 2: Run it and watch it fail, then implement**

`bool garbageMeterDanger(int pending) { return pending >= kGarbageDangerLines; }`

Rewrite `drawGarbageMeter(dl, x, bottom, cell, pending)`:
- Width `std::floor(cell * 0.5f)`. That still fits both gutters: mine runs 15.75–16.25 cells against NEXT at 16.5, theirs 29.9–30.4 against their well at 30.5.
- One beveled rect per pending line (capped at `kVisibleHeight`), each one `cell` tall, stacked up from `bottom`, using `drawBevelRect` with `bevelFor(cell)`.
- Calm: `kGarbageColor` with its `lighten`/`darken` edges, the same gray as garbage in the well.
- Danger: `Rgb{230, 40, 40}` edges likewise, and blink by drawing at alpha `0.45f` during every other 250 ms (`static_cast<int>(ImGui::GetTime() * 4.0) % 2`).

- [ ] **Step 3: Check by eye**

Have the opponent send 1–3 lines: gray cells line up with the well rows. Send 4 or more: the stack turns red and blinks. The meter never overlaps NEXT or the opponent's well.

- [ ] **Step 4: Commit**

```bash
git commit -am "feat(app): garbage meter drawn as garbage cells"
```

---

### Task 6: Arcade menu style (item 2)

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/fonts.cpp`, `src/app/main.cpp`
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Produces: `void applyArcadeStyle(ImGuiStyle& style);` in `render.hpp` (forward-declare `struct ImGuiStyle;` next to `ImGuiIO`).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("ui: the arcade style has square corners and the plate colors") {
    ImGui::CreateContext();
    ImGuiStyle& style = ImGui::GetStyle();
    applyArcadeStyle(style);
    CHECK(style.WindowRounding == 0.0f);
    CHECK(style.FrameRounding == 0.0f);
    CHECK(style.GrabRounding == 0.0f);
    CHECK(style.FrameBorderSize > 0.0f);
    ImGui::DestroyContext();
}
```

- [ ] **Step 2: Run it and watch it fail, then implement**

`applyArcadeStyle` in `render.cpp`, built only from the `Rgb` constants:
- Every `*Rounding` is 0. `WindowBorderSize` and `FrameBorderSize` are 2. `WindowPadding` is (20, 20), `FramePadding` (12, 8), `ItemSpacing` (8, 10).
- `WindowBg`: dark navy panel, slightly lighter than `kBackdropColor` (about `{14, 20, 44}`). `Border`: the well border gray `{110, 110, 140}`.
- `Button` / `ButtonHovered` / `ButtonActive`: `kMyPlateColor`, `lighten`, `darken`.
- `FrameBg` and its variants: the well fill `{18, 18, 26}` and slightly lighter shades.
- `SliderGrab`, `SliderGrabActive`, `CheckMark`, `NavCursor`: `kKoPipColor`, so focus and accents are gold.
- `Header*` and `TableRowBgAlt`: plate blue at low alpha, for the lobby table.
- `Text` `{235, 235, 245}`, `TextDisabled` the scoreboard label color `{150, 160, 190}`.

In `main.cpp`, call `applyArcadeStyle(style)` right after `ImGui::StyleColorsDark()` and before `ScaleAllSizes`, so the padding still scales.

- [ ] **Step 3: Bevels on menu buttons**

In `menuButton`, after `ImGui::Button`, draw the bevel edges over the item with `ImGui::GetWindowDrawList()` and `GetItemRectMin/Max`: a 2px light strip on the top and left, a 2px dark strip on the bottom and right, swapped while `ImGui::IsItemActive()` so the button looks pressed. Switch the start screen's raw `ImGui::Button("Join room", …)` to `menuButton` so it gets the bevel too (it already sits inside `BeginDisabled`).

The green invite button becomes gold: push `ImGuiCol_Button` as `kKoPipColor` and `ImGuiCol_Text` as near-black for that one button.

- [ ] **Step 4: Press Start 2P in the menus**

In `loadFonts`, after adding the HUD font, set `io.FontDefault = fonts.hud`. Then pick a base size that keeps menus readable: set `style.FontSizeBase` (ImGui 1.92) to 12 in `applyArcadeStyle`, and check by eye. Press Start 2P is much wider than the default font, so walk every screen and fix anything that clips: the lobby table, the Controls key table (`slotWidth` already measures its text), the waiting screen's invite link field and the result reason text. `menuButtonWidth()` is font-relative, so most rows scale on their own. Keep `fonts.ui` for any field where the pixel font is unreadable at small sizes (likely the invite-link field); note any such exception in a comment.

- [ ] **Step 5: Run tests and check every screen by eye**

Tests pass. Check the start, Controls, pause, game over, lobby, waiting and result screens in both the desktop and web builds. Everything is square and beveled, keyboard focus shows in gold, nothing clips at phone-landscape size, and the existing "menus are keyboard-navigable" test still passes.

- [ ] **Step 6: Commit**

```bash
git commit -am "feat(app): arcade style for the menus"
```

---

### Task 7: Final manual check

- [ ] **Step 1: Full duel on the VPS**

`scripts/build.sh`, `scripts/run.sh`, then two browsers:

1. Start screen: square beveled buttons in the pixel font, gold invite button with a `?room=` link.
2. Countdown: popping digits, `NAME VS NAME` with a gold `VS`, then `GO!` once.
3. Play: grid backdrop lined up with both wells, `FIRST TO 3` under the clock, `COMBO` in the center, garbage cells that turn red and blink at 4+.
4. KO: the pip fills on the plate and the `KO!` flash still shows. There is no `KO 1 : 0` anywhere.
5. Result and rematch: styled menu; `GO!` shows again only when the rematch starts.
6. Solo: grid backdrop, combo still under HOLD.

- [ ] **Step 2: Commit any fixes**

Fixes found here get their own `fix(app): …` commits.
