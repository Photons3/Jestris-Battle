# Practice Bot and Solo Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four solo modes with saved personal bests (Sprint 40L, Blitz 2:00, Cheese 10L, Zen), plus an offline duel against a placement-scoring bot at Easy, Medium or Hard.

**Architecture:**
- Solo modes: a socket-free `SoloSession` wraps `Game` the same way `DuelSession` does. It owns the clock, the countdown, the finish checks and Zen's reset. `Game` only gains a `linesPerLevel` option, and `Board` gains cheese rows.
- The bot: a planner scores every reachable hard-drop placement, and a `BotPlayer` presses real `Action`s at a preset pace.
- `LocalMatch` implements `Connection` and referees like `server/hub.go`, so `DuelSession` and the duel screen run against the bot unchanged.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build.

**Spec:** `docs/superpowers/specs/2026-10-14-practice-bot-and-solo-modes-design.md`

## Global Constraints

- C++20. Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`. Desktop Debug builds keep ASan/UBSan.
- `DuelSession`, the wire protocol and the Go server do not change.
- `tetris_bot` (planner, bot player) depends on `tetris_core` only. `local_match.cpp` builds in `tetris_duel`, which links `tetris_bot`.
- Mode values:
  - Sprint goal 40 lines, start level 1.
  - Blitz 120000 ms, `linesPerLevel = 5`, menu start level.
  - Cheese 10 rows, start level 1.
  - Zen at menu level, endless.
  - Marathon is today's game.
  - Countdown `kCountdownMs` (3000) for Sprint, Blitz and Cheese only.
- Cheese rows: `PieceType::Garbage` with one hole per row. A row's hole differs from the hole in the row below it. No bombs.
- Bot presets (thinkMs / inputMs / mistakeRate): Easy 900 / 120 / 0.25, Medium 450 / 60 / 0.08, Hard 200 / 25 / 0.0. Target pace about 0.8 / 1.5 / 2.5 pieces per second. **If the pace test fails, tune thinkMs. The pace targets win over the thinkMs values.**
- Planner weights: height −0.51, holes −0.36, bumpiness −0.18, lines +0.76, `kBombBonus` +0.9 per bomb row, `kDangerPenalty` 1.0 × max(0, maxHeight − 14).
- Bot names: `Bot-Easy`, `Bot-Medium`, `Bot-Hard`, each valid under `isValidName`.
- Referee:
  - time-up at `kCountdownMs + kMatchMs + 500` ms
  - verdict compares KOs, then lines sent, otherwise a draw
  - first to `kKosToWin` wins
  - `OpponentName` only before the first `StartMatch`
- PB lines in settings: `sprint=<ms>`, `cheese=<ms>`, `blitz=<score>`, written only when set. Anything that isn't a positive integer is skipped.
- Commits use conventional scopes like the history (`feat(core): …`, `feat(app): …`, `feat(bot): …`, `docs: …`). Work on branch `develop`.

## Commands

- Build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- One doctest group: `./build/tetris_tests -tc="solo:*"` (wildcards allowed)
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/core/game.hpp/.cpp` | `GameOptions::linesPerLevel`, `levelFor(…, linesPerLevel)` |
| `src/core/board.hpp/.cpp` | `pushCheese`, `garbageRows` |
| `src/app/solo.hpp/.cpp` (new) | `SoloMode`, `SoloRules`, `rulesFor`, `makeSoloGame`, `SoloSession`, `PersonalBests` |
| `src/app/sound_cues.hpp/.cpp` | `soundFor(SoloEvent)`, `soloMusic(const SoloSession&)` |
| `src/app/settings.hpp/.cpp` | `Settings::bests` |
| `src/app/render.hpp/.cpp` | SOLO and PRACTICE menu sections, solo HUD line, solo result overlay, `formatClock`, `formatScore` |
| `src/app/app.hpp/.cpp` | `solo` replaces `game`; `PlaySolo`, `PlayBot`; `connection->update` |
| `src/bot/planner.hpp/.cpp` (new) | `Placement`, `rankPlacements`, `evaluateBoard` |
| `src/bot/bot_player.hpp/.cpp` (new) | `BotLevel`, `BotPreset`, `presetFor`, `BotPlayer`, `botName` |
| `src/bot/local_match.hpp/.cpp` (new) | `LocalMatch : Connection`, `verdictFor` |
| `src/net/connection.hpp` | `virtual void update(int ms) {}` |
| `CMakeLists.txt` | `tetris_bot` library, new sources and tests |
| `tests/solo_test.cpp`, `tests/planner_test.cpp`, `tests/bot_player_test.cpp`, `tests/local_match_test.cpp` (new) and existing test files | tests per task |
| `README.md` | solo modes and practice bot |

## Review Focus

1. **Window focus lost during a solo countdown.** Expected: the run doesn't get stuck. The game must not be paused while input is blocked, because Esc couldn't unpause it. Pinned in Task 2 (`solo: pausing during the countdown does nothing`), and Task 4 routes focus loss through `SoloSession::pause()`.
2. **Quitting, restarting or topping out mid-run.** Expected: no PB is ever recorded. Only `Finished` has a `result()`. Pinned in Task 2 (`solo: a top-out ends the run with no result`), and Task 4 submits only on the Finished transition.
3. **A hidden tab catching up in big chunks.** `frameBudgetMs` replays up to 130 s in 100 ms steps. Expected: the bot keeps playing and time-up still fires inside one large `update`. Pinned in Task 7 (`local match: a long update still plays and ends on time`).
4. **Messages from the human at the wrong time:** `Rematch` mid-match, `Dead` or `Attack` after `MatchOver`. Expected: ignored, the same as the server. Pinned in Task 7.
5. **Corrupt or hand-edited PB values** (`sprint=-5`, `blitz=abc`, `cheese=0`). Expected: skipped, everything else still loads. Pinned in Task 3.

---

### Task 1: Core rules for the modes

**Files:**
- Modify: `src/core/game.hpp`, `src/core/game.cpp` (`levelFor`, `GameOptions`, the `levelFor` call in `finishClear`)
- Modify: `src/core/board.hpp`, `src/core/board.cpp`
- Test: `tests/game_test.cpp`, `tests/board_test.cpp`

**Interfaces:**
- Produces:
  - `int levelFor(int startLevel, int lines, int linesPerLevel = kLinesPerLevel)`
  - `GameOptions::linesPerLevel` (int, default `kLinesPerLevel`)
  - `void Board::pushCheese(int rows, std::mt19937& rng)` (fills rows 0..rows−1; expects an empty board below `rows`)
  - `int Board::garbageRows() const`

- [ ] **Step 1: Write the failing tests**

```cpp
// game_test.cpp
TEST_CASE("game: lines per level is settable") {
    CHECK(levelFor(1, 9) == 1);
    CHECK(levelFor(1, 10) == 2);
    CHECK(levelFor(1, 5, 5) == 2);
    CHECK(levelFor(3, 12, 5) == 5);
}

TEST_CASE("game: a faster level curve levels up after fewer lines") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5), GameOptions{.linesPerLevel = 2});
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 4);
    CHECK(game.level() == 3);
}

// board_test.cpp
TEST_CASE("board: cheese rows have one hole each, never stacked, and no bombs") {
    std::mt19937 rng(42);
    Board board;
    board.pushCheese(10, rng);
    int previousHole = -1;
    for (int y = 0; y < 10; ++y) {
        int holes = 0, hole = -1;
        for (int x = 0; x < kBoardWidth; ++x) {
            if (!board.at(x, y)) { ++holes; hole = x; }
            else CHECK(board.at(x, y) == PieceType::Garbage);
        }
        CHECK(holes == 1);
        CHECK(hole != previousHole);
        previousHole = hole;
    }
    for (int x = 0; x < kBoardWidth; ++x) CHECK_FALSE(board.at(x, 10));
    CHECK(board.garbageRows() == 10);
    CHECK(board.fullRows().empty());
}

TEST_CASE("board: garbage rows count rows with garbage, not bombs or pieces") {
    Board board;
    board.set(0, 0, PieceType::Garbage);
    board.set(1, 1, PieceType::Bomb);
    board.set(2, 2, PieceType::T);
    board.set(3, 3, PieceType::Garbage);
    CHECK(board.garbageRows() == 2);
}
```

- [ ] **Step 2: Run them and check they fail to compile** (`levelFor` arity, `linesPerLevel`, `pushCheese`, `garbageRows` not defined)

Run: `cmake --build build`

- [ ] **Step 3: Implement**
  - Add the `levelFor` parameter and make `finishClear` pass `options_.linesPerLevel`.
  - `pushCheese` picks each hole with `rng() % kBoardWidth`, redrawing until it differs from the previous row's hole.
  - `garbageRows` scans all `kBoardHeight` rows.

- [ ] **Step 4: Run the tests and check they pass**

Run: `cmake --build build && ./build/tetris_tests -tc="game:*,board:*"`
Expected: all pass, existing tests included.

- [ ] **Step 5: Commit** `feat(core): lines-per-level option and cheese garbage rows`

---

### Task 2: `SoloSession` and personal bests

**Files:**
- Create: `src/app/solo.hpp`, `src/app/solo.cpp` (add to `tetris_duel` in `CMakeLists.txt`)
- Modify: `src/app/sound_cues.hpp/.cpp`
- Test: `tests/solo_test.cpp` (add to `tetris_tests`), `tests/sound_cues_test.cpp`

**Interfaces:**
- Consumes: Task 1's `GameOptions::linesPerLevel`, `Board::pushCheese`, `Board::garbageRows`.
- Produces (namespace `tetris`):
```cpp
enum class SoloMode { Marathon, Sprint, Blitz, Cheese, Zen };
enum class SoloPhase { Countdown, Playing, Finished, ToppedOut };
enum class SoloEvent { CountdownTick, Start, Finished };
inline constexpr int kSprintLines = 40, kBlitzMs = 120000, kCheeseRows = 10, kBlitzLinesPerLevel = 5;

struct SoloRules {
    SoloMode mode = SoloMode::Marathon;
    int goalLines = 0;    // Sprint: finish at this many lines; 0 = none
    int timeLimitMs = 0;  // Blitz: clock counts down from this; 0 = counts up (or no clock)
    bool countdown = false;
};
SoloRules rulesFor(SoloMode mode);
Game makeSoloGame(SoloMode mode, std::uint32_t seed, int startLevel);  // level 1 + cheese board, Blitz lines per level
bool isTimed(SoloMode mode);  // Sprint, Blitz, Cheese

class SoloSession {
public:
    SoloSession(SoloRules rules, Game game);
    void update(int ms);
    void pause();             // pauses the game only while Playing
    SoloMode mode() const;
    SoloPhase phase() const;
    bool frozen() const;      // phase != Playing
    int countdownMs() const;
    int clockMs() const;      // elapsed, or remaining for a time limit
    int progress() const;     // Sprint: lines; Cheese: garbageRows(); else 0
    std::optional<int> result() const;  // Finished only: ms (Sprint, Cheese) or score (Blitz)
    std::vector<SoloEvent> takeEvents();
    Game& game();
    const Game& game() const;
};

struct PersonalBests {
    std::optional<int> sprintMs, cheeseMs, blitzScore;
    std::optional<int> best(SoloMode mode) const;  // nullopt for Marathon and Zen
    bool submit(SoloMode mode, int result);         // true if a new PB (lower ms; higher Blitz score)
    friend bool operator==(const PersonalBests&, const PersonalBests&) = default;
};

// sound_cues.hpp
std::optional<Sound> soundFor(SoloEvent event);  // CountdownTick and Start as the duel's; Finished: none
MusicCue soloMusic(const SoloSession& solo);      // Stopped unless Playing, then soloMusic(game)
```

- [ ] **Step 1: Write the failing tests** in `tests/solo_test.cpp`. Every rule below is tested with small custom `SoloRules` and `boardWithWell` games, plus `rulesFor` checks:

```cpp
TEST_CASE("solo: each mode's rules") {
    CHECK(rulesFor(SoloMode::Sprint).goalLines == 40);
    CHECK(rulesFor(SoloMode::Sprint).countdown);
    CHECK(rulesFor(SoloMode::Blitz).timeLimitMs == 120000);
    CHECK(rulesFor(SoloMode::Cheese).countdown);
    CHECK_FALSE(rulesFor(SoloMode::Zen).countdown);
    CHECK_FALSE(rulesFor(SoloMode::Marathon).countdown);
    CHECK(makeSoloGame(SoloMode::Sprint, 1, 9).level() == 1);
    CHECK(makeSoloGame(SoloMode::Zen, 1, 9).level() == 9);
    CHECK(makeSoloGame(SoloMode::Cheese, 1, 9).board().garbageRows() == 10);
}

TEST_CASE("solo: the countdown blocks play, ticks each second, then starts") {
    SoloSession solo(rulesFor(SoloMode::Sprint), makeSoloGame(SoloMode::Sprint, 1, 1));
    CHECK(solo.phase() == SoloPhase::Countdown);
    CHECK(solo.frozen());
    solo.update(1000);
    solo.update(1000);
    solo.update(999);
    CHECK(solo.phase() == SoloPhase::Countdown);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::Playing);
    CHECK(solo.clockMs() == 0);
    CHECK(solo.takeEvents() == std::vector{SoloEvent::CountdownTick, SoloEvent::CountdownTick,
                                           SoloEvent::CountdownTick, SoloEvent::Start});
}

TEST_CASE("solo: pausing during the countdown does nothing") {
    SoloSession solo(rulesFor(SoloMode::Blitz), makeSoloGame(SoloMode::Blitz, 1, 1));
    solo.pause();
    CHECK(solo.game().state() == GameState::Playing);
}

TEST_CASE("solo: sprint finishes at the goal with the time") {
    SoloSession solo({SoloMode::Sprint, 4}, Game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5)));
    solo.update(250);
    dropUprightI(solo.game());
    solo.game().tick(kClearDelayMs);
    solo.update(kClearDelayMs);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == 250 + kClearDelayMs);
    CHECK(solo.frozen());
}

TEST_CASE("solo: blitz ends at 0:00 and still counts a clear in progress") {
    SoloSession solo({SoloMode::Blitz, 0, 1000}, Game(seedWithFirst(PieceType::I), 1, boardWithWell(1, 5)));
    dropUprightI(solo.game());
    REQUIRE(solo.game().state() == GameState::Clearing);
    solo.update(1000);
    CHECK(solo.game().lines() == 1);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == solo.game().score());
}

TEST_CASE("solo: cheese finishes when the last garbage row clears") {
    Board board = boardWithWell(1, 5);       // then turn row 0's blocks into garbage
    for (int x = 0; x < kBoardWidth; ++x) if (board.at(x, 0)) board.set(x, 0, PieceType::Garbage);
    SoloSession solo({SoloMode::Cheese}, Game(seedWithFirst(PieceType::I), 1, board));
    dropUprightI(solo.game());
    solo.game().tick(kClearDelayMs);
    solo.update(kClearDelayMs);
    CHECK(solo.phase() == SoloPhase::Finished);
    CHECK(solo.result() == kClearDelayMs);
}

TEST_CASE("solo: a top-out ends the run with no result") {
    SoloSession solo({SoloMode::Sprint, 40}, Game(1, 1));
    for (int i = 0; i < 100 && solo.game().state() != GameState::GameOver; ++i) solo.game().apply(Action::HardDrop);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::ToppedOut);
    CHECK_FALSE(solo.result());
}

TEST_CASE("solo: zen clears the board and plays on after a top-out") {
    SoloSession solo({SoloMode::Zen}, Game(1, 1));
    for (int i = 0; i < 100 && solo.game().state() != GameState::GameOver; ++i) solo.game().apply(Action::HardDrop);
    solo.update(1);
    CHECK(solo.phase() == SoloPhase::Playing);
    CHECK(solo.game().state() == GameState::Playing);
    CHECK(countFilled(solo.game().board()) == 0);
}

TEST_CASE("solo: the clock stands still while paused") {
    SoloSession solo({SoloMode::Sprint, 40}, Game(1, 1));
    solo.update(100);
    solo.pause();
    solo.update(500);
    CHECK(solo.clockMs() == 100);
}

TEST_CASE("solo: personal bests keep the lower time and the higher score") {
    PersonalBests bests;
    CHECK(bests.submit(SoloMode::Sprint, 60000));
    CHECK_FALSE(bests.submit(SoloMode::Sprint, 61000));
    CHECK(bests.submit(SoloMode::Sprint, 59000));
    CHECK(bests.best(SoloMode::Sprint) == 59000);
    CHECK(bests.submit(SoloMode::Blitz, 1000));
    CHECK_FALSE(bests.submit(SoloMode::Blitz, 900));
    CHECK(bests.submit(SoloMode::Blitz, 1100));
    CHECK_FALSE(bests.best(SoloMode::Zen));
    CHECK_FALSE(bests.submit(SoloMode::Marathon, 5));
}
```
  In `sound_cues_test.cpp`, check that `soundFor(SoloEvent::CountdownTick) == soundFor(DuelEvent::CountdownTick)`, `soundFor(SoloEvent::Start) == soundFor(DuelEvent::MatchStart)`, and that `soloMusic(solo)` is `Stopped` during the countdown.

- [ ] **Step 2: Run and check they fail** (`solo.hpp` missing). Run: `cmake --build build`

- [ ] **Step 3: Implement `solo.hpp/.cpp` and the two sound-cue functions**

  `update(ms)` works in this order:
  1. **Countdown:** subtract ms. Emit `CountdownTick` when the shown second changes, using the same test as `DuelSession::update`, and one on construction. At ≤ 0, switch to Playing and emit `Start`.
  2. **Playing:**
     - Advance the clock only while the game state is `Playing` or `Clearing`.
     - With a time limit: when remaining reaches 0, tick the game 1 ms at a time while it's `Clearing`, then finish.
     - On `GameOver`: Zen calls `resetBoard()`. Every other mode goes to ToppedOut.
     - Sprint finishes once `lines() >= goalLines`. Cheese finishes once `garbageRows() == 0`.
  3. On finish, emit `Finished`.

  Marathon never finishes; it goes to ToppedOut at game over.

- [ ] **Step 4: Run and check they pass.** Run: `cmake --build build && ./build/tetris_tests -tc="solo:*,sound*"`

- [ ] **Step 5: Commit** `feat(app): solo session with sprint, blitz, cheese and zen rules`

---

### Task 3: Personal bests in the saved settings

**Files:**
- Modify: `src/app/settings.hpp`, `src/app/settings.cpp`
- Test: `tests/settings_test.cpp`

**Interfaces:**
- Consumes: `PersonalBests` (Task 2).
- Produces: `Settings::bests` (`PersonalBests`, default empty).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("settings: personal bests load back, and unset ones stay unset") {
    Settings saved;
    saved.bests.sprintMs = 62310;
    saved.bests.blitzScore = 48200;
    const Settings loaded = parseSettings(serializeSettings(saved));
    CHECK(loaded.bests == saved.bests);
    CHECK_FALSE(loaded.bests.cheeseMs);
    CHECK(serializeSettings(Settings{}).find("sprint=") == std::string::npos);
}

TEST_CASE("settings: bad personal bests are skipped") {
    const std::string good = serializeSettings(Settings{});
    const Settings loaded = parseSettings(good + "sprint=-5\nblitz=abc\ncheese=0\n");
    CHECK(loaded.bests == PersonalBests{});
    CHECK(loaded.keys == defaultKeymap());
}
```

- [ ] **Step 2: Run and check they fail.** Run: `cmake --build build`

- [ ] **Step 3: Implement.** Write the `sprint=`, `cheese=` and `blitz=` lines only when set. Parse them with the existing integer helper and accept values > 0 only. `kVersion` stays `"1"`, since older files simply have no PB lines.

- [ ] **Step 4: Run and check they pass.** Run: `./build/tetris_tests -tc="settings:*"`

- [ ] **Step 5: Commit** `feat(app): save personal bests with the settings`

---

### Task 4: Solo modes in the app and menus

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`
- Modify: `src/app/app.hpp`, `src/app/app.cpp`
- Modify: `README.md` (a "Solo modes" section)
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces:
  - `UiCommand::Play` renamed to `UiCommand::PlaySolo`
  - `StartMenu::soloMode` (`SoloMode`), `StartMenu::bests` (`PersonalBests`)
  - `App::solo` (`std::optional<SoloSession>`, replacing `App::game`)
  - `App::newSolo(SoloMode)`
  - `std::string formatClock(int ms)`: `"0:42.18"` (minutes, unpadded; seconds two digits; hundredths)
  - `std::string formatScore(int score)`: `"48,200"`
  - `void drawSoloStatus(const SoloSession&, const Fonts&)`
  - `UiCommand drawSoloOverlay(const SoloSession&, const PersonalBests&, bool newBest, const Fonts&, bool muted, bool musicOff)`

- [ ] **Step 1: Write the failing UI tests**

```cpp
TEST_CASE("ui: solo clocks and scores read naturally") {
    CHECK(formatClock(0) == "0:00.00");
    CHECK(formatClock(42180) == "0:42.18");
    CHECK(formatClock(62310) == "1:02.31");
    CHECK(formatClock(600000) == "10:00.00");
    CHECK(formatScore(0) == "0");
    CHECK(formatScore(48200) == "48,200");
    CHECK(formatScore(1234567) == "1,234,567");
}
```

- [ ] **Step 2: Run and check it fails.** Run: `cmake --build build`

- [ ] **Step 3: Render changes in `render.cpp`**
  - **Start screen:**
    - Replace the single Solo/Play button with a `SeparatorText("SOLO")` section: Marathon, Sprint 40L, Blitz 2:00, Cheese 10L, Zen. Each button sets `menu.soloMode` and returns `PlaySolo`.
    - Timed modes draw their PB right-aligned in dim text on the button row: `formatClock` for times, `formatScore` for Blitz.
    - The level slider label becomes `"Start level: %d"` with a dim caption under it: "Marathon, Blitz, Zen".
  - **`drawSoloStatus`:** one mode line in the side panel under the existing stats.
    - Sprint: `formatClock` and `"31 / 40"`.
    - Cheese: clock and `"4 left"`.
    - Blitz: remaining as `m:ss`, drawn red under 10 s.
    - Zen: `"ZEN"`.
    - Marathon: nothing.
  - **`drawSoloOverlay`:**
    - Countdown: call `drawCountdown`.
    - Finished or ToppedOut in a timed mode: title `FINISHED` or `TOPPED OUT`, the result in big text, then `NEW PB!` or `PB <best>`, then Retry (`Restart`) and Quit to menu. R also returns `Restart` (`ImGui::IsKeyPressed(ImGuiKey_R)`).
    - Otherwise fall back to `drawOverlay(game, …)` (pause menu, Marathon GAME OVER).

- [ ] **Step 4: App changes in `app.cpp`**
  - `newSolo(mode)` builds `SoloSession(rulesFor(mode), makeSoloGame(mode, seed, menu.startLevel))` and resets input, combo and callout the same way `newGame()` does today. Then remove `newGame()`.
  - `PlaySolo` calls `newSolo(menu.soloMode)`. `Restart` calls `newSolo(solo->mode())`.
  - `Resume` and `QuitToMenu` use `solo`.
  - `activeGame()` returns `&solo->game()` only while `!solo->frozen()`, which keeps Esc working while paused.
  - Focus loss calls `solo->pause()`.
  - In `step()`:
    - Call `solo->update(ms)` after the game ticks.
    - Play `soundFor(SoloEvent)` cues.
    - On `SoloEvent::Finished`: `menu.bests.submit(mode, *result())`. If it's a new PB, set `newBest = true` (a new `App` field reset by `newSolo`), call `saveSettings()` and play `soundFor(DuelEvent::Won)`.
  - `musicCue` uses `soloMusic(*solo)`.
  - Load `menu.bests` from `Settings` in `App()`, and include it in `saveSettings()`.
  - `drawFrame` for `Screen::Playing`: `drawGame`, then `drawSoloStatus`, then `drawSoloOverlay`.

- [ ] **Step 5: Build, run all tests, and check by hand**

Run: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass. Then run `./build/tetris` and check each of the following:
- Sprint counts down from 3, then the clock runs.
- Escape pauses and the clock holds.
- Losing focus during the countdown doesn't strand the run.
- Finishing shows FINISHED and NEW PB!, and a second, slower run shows the PB.
- The PB appears on the menu after restarting the app.
- Zen never ends.
- Cheese starts with 10 holed rows.

- [ ] **Step 6: Commit** `feat(app): solo mode menu, HUD and results with personal bests`

---

### Task 5: Bot planner

**Files:**
- Create: `src/bot/planner.hpp`, `src/bot/planner.cpp`
- Modify: `CMakeLists.txt` (new `tetris_bot` library with `src/bot/planner.cpp`, linking `tetris_core`; link it into `tetris_tests`)
- Test: `tests/planner_test.cpp`

**Interfaces:**
- Produces:
```cpp
inline constexpr double kHeightWeight = -0.51, kHolesWeight = -0.36, kBumpinessWeight = -0.18,
                        kLinesWeight = 0.76, kBombBonus = 0.9, kDangerPenalty = 1.0;
inline constexpr int kDangerHeight = 14;
struct Placement {
    std::vector<Action> actions;  // ends with HardDrop; starts with Hold for hold candidates
    Piece landed;                 // where the piece locks
    double score = 0.0;
};
// After locking `piece` on `board`, clearing its full rows and the bomb rows it sets off.
double evaluateBoard(const Board& board, const Piece& piece);
// Every distinct hard-drop placement for the active piece (and the hold option when canHold()), best first.
std::vector<Placement> rankPlacements(const Game& game);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("planner: an O piece on an empty board has 9 places to land") {
    const Game game(seedWithFirst(PieceType::O), 1);
    const auto ranked = rankPlacements(game);
    const auto own = std::count_if(ranked.begin(), ranked.end(),
                                   [](const Placement& p) { return p.actions.front() != Action::Hold; });
    CHECK(own == 9);
    for (const Placement& p : ranked) CHECK(p.actions.back() == Action::HardDrop);
}

TEST_CASE("planner: fills the gap in an almost full row") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    const Placement best = rankPlacements(game).front();
    for (Action a : best.actions) game.apply(a);
    CHECK(game.state() == GameState::Clearing);
}

TEST_CASE("planner: prefers setting off a bomb") {
    Board board;
    for (int x = 0; x < kBoardWidth; ++x) board.set(x, 0, x == 8 ? PieceType::Bomb : PieceType::Garbage);
    Game game(seedWithFirst(PieceType::O), 1, board);
    const Placement best = rankPlacements(game).front();
    const auto cells = best.landed.cells();
    CHECK(std::any_of(cells.begin(), cells.end(), [](Point c) { return c.x == 8 && c.y == 1; }));
}

TEST_CASE("planner: hold candidates only while holding is allowed") {
    Game game(seedWithFirst(PieceType::T), 1);
    auto holds = [&] { auto r = rankPlacements(game);
        return std::any_of(r.begin(), r.end(), [](const Placement& p) { return p.actions.front() == Action::Hold; }); };
    CHECK(holds());
    game.apply(Action::Hold);
    CHECK_FALSE(holds());
}

TEST_CASE("planner: planned moves land exactly where planned") {
    const Game start(seedWithFirst(PieceType::T), 1, boardWithWell(2, 0));
    for (const Placement& p : rankPlacements(start)) {
        Game game = start;
        for (Action a : p.actions) {
            if (a == Action::HardDrop) {  // check the landing spot just before the drop locks it
                Piece dropped = game.active();
                dropped.pos.y -= game.ghostDistance();
                CHECK(sortedCells(dropped.cells()) == sortedCells(p.landed.cells()));
            }
            game.apply(a);
        }
    }
}
```

- [ ] **Step 2: Run and check they fail.** Run: `cmake -B build && cmake --build build`

- [ ] **Step 3: Implement**
  - **Candidates:** for each piece (the active piece; for hold, `spawnPiece(hold ? *hold : next(1)[0])` with a leading `Hold`), try each rotation prefix in `{}, {CW}, {CW, CW}, {CCW}`, applied with `rotateWithKicks` on `game.board()`, and skip it if a rotation fails. Then for each shift −9..+9, step one column at a time and stop if blocked (skip a shift you couldn't fully make). Then drop until `collides`. Dedupe on `sortedCells(landed.cells())`.
  - **`evaluateBoard`:**
    1. Copy the board and `lock`.
    2. Gather `fullRows()` and `bombRowsHitBy(piece)` and remove them with `removeRows`.
    3. Column height = highest filled y + 1. Holes = empty cells under a column's top. Bumpiness = sum of |h[i] − h[i+1]|.
    4. Score with the weights, where lines = full-row count and the bomb bonus applies per bomb row.
  - Sort by score, highest first. Use a stable sort so ties are deterministic.

- [ ] **Step 4: Run and check they pass.** Run: `./build/tetris_tests -tc="planner:*"`

- [ ] **Step 5: Commit** `feat(bot): placement planner`

---

### Task 6: `BotPlayer`

**Files:**
- Create: `src/bot/bot_player.hpp`, `src/bot/bot_player.cpp` (add to `tetris_bot`)
- Test: `tests/bot_player_test.cpp`

**Interfaces:**
- Consumes: `rankPlacements` (Task 5).
- Produces:
```cpp
enum class BotLevel { Easy, Medium, Hard };
struct BotPreset { int thinkMs; int inputMs; double mistakeRate; };
BotPreset presetFor(BotLevel level);
std::string botName(BotLevel level);  // "Bot-Easy", "Bot-Medium", "Bot-Hard"
class BotPlayer {
public:
    BotPlayer(BotLevel level, std::uint32_t seed);
    void update(Game& game, int ms);  // only calls game.apply(); call before ticking the game each ms
    void reset();                     // drops the plan, e.g. after the board was reset by a KO
};
```

- [ ] **Step 1: Write the failing tests.** Use a helper `int playFor(BotLevel, std::uint32_t seed, int ms, Game&)` that runs `bot.update(game, 1); game.tick(1);` per ms and counts `HardDropped` events.

```cpp
TEST_CASE("bot: hard survives 500 pieces on its own") {
    Game game(7, 1);
    BotPlayer bot(BotLevel::Hard, 7);
    int pieces = 0;
    for (int ms = 0; pieces < 500 && game.state() != GameState::GameOver && ms < 600000; ++ms) {
        bot.update(game, 1);
        game.tick(1);
        for (const GameEvent& e : game.takeEvents()) pieces += e.type == GameEventType::HardDropped;
    }
    CHECK(pieces >= 500);
    CHECK(game.state() != GameState::GameOver);
}

TEST_CASE("bot: each level plays at its pace") {
    auto pps = [](BotLevel level) { Game game(3, 1); return playFor(level, 3, 60000, game) / 60.0; };
    const double easy = pps(BotLevel::Easy), medium = pps(BotLevel::Medium), hard = pps(BotLevel::Hard);
    CHECK(easy == doctest::Approx(0.8).epsilon(0.25));
    CHECK(medium == doctest::Approx(1.5).epsilon(0.25));
    CHECK(hard == doctest::Approx(2.5).epsilon(0.25));
}

TEST_CASE("bot: the same seed plays the same game") {
    Game a(5, 1), b(5, 1);
    playFor(BotLevel::Medium, 9, 20000, a);
    playFor(BotLevel::Medium, 9, 20000, b);
    CHECK(snapshotCells(a) == snapshotCells(b));  // compare boards cell by cell
}

TEST_CASE("bot: does nothing once the game is over") {
    Game game(1, 1);
    for (int i = 0; i < 100 && game.state() != GameState::GameOver; ++i) game.apply(Action::HardDrop);
    BotPlayer bot(BotLevel::Hard, 1);
    bot.update(game, 5000);  // must not crash or apply anything
    CHECK(game.state() == GameState::GameOver);
}

TEST_CASE("bot: names are valid player names") {
    for (BotLevel l : {BotLevel::Easy, BotLevel::Medium, BotLevel::Hard}) CHECK(isValidName(botName(l)));
}
```
  `snapshotCells` is a local helper that compares `board().at(x, y)` for every cell. `isValidName` comes from `net/protocol.hpp`, which `tetris_tests` already links.

- [ ] **Step 2: Run and check they fail.** Run: `cmake --build build`

- [ ] **Step 3: Implement**
  - `update` runs in 1 ms steps.
  - Outside `GameState::Playing`, do nothing; keep the queue, since a `Clearing` pause comes after the plan's final HardDrop and the queue is already empty then.
  - With an empty queue: count `thinkMs` down. At 0, call `rankPlacements`. With probability `mistakeRate`, pick rank 1 or 2 (uniformly, clamped to the size); otherwise rank 0. Queue its actions.
  - With a non-empty queue: apply the front action every `inputMs`, applying the first one right away.
  - Use `std::mt19937` seeded from the constructor.
  - If the pace test misses, tune the `thinkMs` values (Global Constraints).

- [ ] **Step 4: Run and check they pass.** Run: `./build/tetris_tests -tc="bot:*"`

- [ ] **Step 5: Commit** `feat(bot): bot player with easy, medium and hard presets`

---

### Task 7: `LocalMatch` referee

**Files:**
- Create: `src/bot/local_match.hpp`, `src/bot/local_match.cpp` (add to `tetris_duel`; `tetris_duel` links `tetris_bot`)
- Modify: `src/net/connection.hpp` (add `virtual void update(int /*ms*/) {}`, commented "advances a local opponent; network links do nothing")
- Test: `tests/local_match_test.cpp`

**Interfaces:**
- Consumes: `BotPlayer`, `botName`, `BotLevel` (Task 6); `makeDuelGame`, `snapshotOf`, `encode`/`decode`, `kCountdownMs`, `kMatchMs`, `kSnapshotIntervalMs`, `kKoFlashMs`, `kKosToWin` (existing).
- Produces:
```cpp
inline constexpr int kLocalMatchGraceMs = 500;  // like defaultMatchGrace in server/hub.go
// The verdict for the human, as server/hub.go's verdict(): KOs, then lines sent, else a draw.
MatchOver verdictFor(int kosMine, int kosTheirs, int sentMine, int sentTheirs, MatchReason reason);
class LocalMatch : public Connection {
public:
    LocalMatch(BotLevel level, std::uint32_t seed);
    void send(const std::string& text) override;
    std::vector<std::string> receive() override;
    bool closed() const override { return false; }
    void update(int ms) override;
    const Game* botGame() const;  // null before the first START
};
```

- [ ] **Step 1: Write the failing tests.** Use a helper `std::vector<Message> drain(LocalMatch&)` that decodes `receive()`.

```cpp
TEST_CASE("local match: verdicts follow the server's rules") {
    CHECK(verdictFor(2, 1, 0, 9, MatchReason::Time).result == MatchResult::Win);
    CHECK(verdictFor(1, 1, 5, 3, MatchReason::Time).result == MatchResult::Win);
    CHECK(verdictFor(1, 1, 3, 5, MatchReason::Time).result == MatchResult::Lose);
    CHECK(verdictFor(1, 1, 4, 4, MatchReason::Time).result == MatchResult::Draw);
    CHECK(verdictFor(3, 0, 1, 2, MatchReason::KOs) == MatchOver{MatchResult::Win, MatchReason::KOs, 1, 2});
}

TEST_CASE("local match: creating a room names the bot and starts a match") {
    LocalMatch match(BotLevel::Hard, 1);
    match.send(encode(CreateRoom{4, false}));
    const auto got = drain(match);
    REQUIRE(got.size() == 2);
    CHECK(std::get<OpponentName>(got[0]).name == "Bot-Hard");
    CHECK(std::get<StartMatch>(got[1]).startLevel == 4);
}

TEST_CASE("local match: the bot waits out the countdown, then plays and sends snapshots");
    // CREATE; drain; update(kCountdownMs - 1) gives no Snapshot; update(3000) gives at least one Snapshot

TEST_CASE("local match: three deaths lose by KOs");
    // CREATE; update(kCountdownMs); send Dead x3; drain shows KoCounts{0,1},{0,2},{0,3} then MatchOver Lose/KOs

TEST_CASE("local match: burying the bot wins by KOs");
    // CREATE; update(kCountdownMs); loop up to 200 times: send Attack{40}, update(2000), until MatchOver.
    // Expect KoCounts whose `mine` reaches 3 and MatchOver{Win, KOs}; mySent counts the human's ATK lines.

TEST_CASE("local match: time up gives the verdict on the totals");
    // CREATE; send Attack{3} after the countdown; update(kCountdownMs + kMatchMs + kLocalMatchGraceMs).
    // The last message is MatchOver with reason Time, equal to verdictFor(<KoCounts seen>, 3, <theirSent>, Time)

TEST_CASE("local match: a long update still plays and ends on time");
    // CREATE; one update(kCountdownMs + kMatchMs + kLocalMatchGraceMs): the bot placed pieces (snapshot not empty) and MatchOver Time arrives

TEST_CASE("local match: late and early messages are ignored");
    // Rematch mid-match: no new START. After MatchOver: Dead and Attack give no messages, the bot board stops changing.

TEST_CASE("local match: a rematch starts again with a new seed and no new name");
    // after MatchOver send Rematch: exactly one StartMatch with a different seed, no OpponentName

TEST_CASE("local match: a real duel session plays the bot") {
    std::unique_ptr<LocalMatch> match = std::make_unique<LocalMatch>(BotLevel::Hard, 2);
    DuelSession duel([&](const std::string& text) { match->send(text); });
    duel.createRoom(1);
    auto frame = [&](int ms) {
        for (const std::string& text : match->receive()) duel.onMessage(text);
        if (duel.game() && !duel.frozen() && duel.phase() == DuelPhase::Playing) duel.game()->tick(ms);
        duel.update(ms);
        match->update(ms);
    };
    frame(16);
    CHECK(duel.opponentName() == "Bot-Hard");
    CHECK(duel.phase() == DuelPhase::Countdown);
    for (int i = 0; i < 2000 && duel.sentTheirs() == 0; ++i) frame(16);
    CHECK(duel.sentTheirs() > 0);  // the bot's garbage reached us
}
```
  The tests written as one-line comments are outlines: write each one out in full, with the assertions its comment lists.

- [ ] **Step 2: Run and check they fail.** Run: `cmake -B build && cmake --build build`

- [ ] **Step 3: Implement**
  - Keep a `std::mt19937` from the constructor seed for match seeds and bot seeds.
  - **`send`** decodes the message, then:
    - `CreateRoom`: queue `OpponentName{botName}` on the first match only, then start. Starting means: new seed; `bot_ = makeDuelGame({seed, level})`; reset `BotPlayer`; clear KOs, sent counts, snapshot state and over; set the clock to 0; queue `StartMatch`.
    - `Attack`: only while playing and not over. `bot.receiveGarbage(n)` and add n to the human's sent count.
    - `Dead`: KO for the bot.
    - `Rematch`: only when over.
    - Everything else is ignored.
  - **`update(ms)`** runs in 1 ms steps while not over:
    - Advance the clock.
    - After `kCountdownMs`, unless the bot is in its KO flash: `bot.update(game, 1); game.tick(1)`.
    - `takeOutgoingAttack()` gives `Attack` and adds to the bot's sent count.
    - Queue a snapshot every `kSnapshotIntervalMs` when it changed.
    - On `GameOver`: `resetBoard()`, `bot.reset()`, KO for the human, flash `kKoFlashMs`.
    - At `kCountdownMs + kMatchMs + kLocalMatchGraceMs`: finish with `Time`.
  - **KO** sends `KoCounts{humanKos, botKos}`. At `kKosToWin` it finishes with `KOs`.
  - **Finish** queues `verdictFor(humanKos, botKos, humanSent, botSent, reason)` and sets over.

- [ ] **Step 4: Run and check they pass.** Run: `./build/tetris_tests -tc="local match:*"`

- [ ] **Step 5: Commit** `feat(bot): local referee so the duel screen plays the bot offline`

---

### Task 8: Practice bot in the app

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp` (PRACTICE section)
- Modify: `src/app/app.hpp`, `src/app/app.cpp`
- Modify: `CMakeLists.txt` (link `tetris_bot` where `tetris_duel` is linked, if not transitive)
- Modify: `README.md` ("Practice bot" section; the intro no longer says duels need two browsers)

**Interfaces:**
- Consumes: `LocalMatch`, `BotLevel` (Tasks 6 and 7).
- Produces: `UiCommand::PlayBot`; `StartMenu::botLevel` (`BotLevel`, default Medium); `App::openBotDuel()`.

- [ ] **Step 1: Render.** Under the SOLO section, on every build, add a `SeparatorText("PRACTICE")` row: an Easy / Medium / Hard selector (three `RadioButton`s, or a combo the width of the menu), then a `vs Bot` button that returns `PlayBot`. The 1v1 DUEL section stays behind `menu.duelsEnabled`.

- [ ] **Step 2: App**
  - `openBotDuel()`:
    1. If a connection is open, call `closeDuel()`.
    2. `connection = std::make_unique<LocalMatch>(menu.botLevel, seed from SDL_GetTicksNS())`.
    3. `menu.error.clear(); screen = Screen::Duel; duel.createRoom(menu.startLevel);`
  - In `step()`, right after `duel.update(ms)`: `if (app.connection) app.connection->update(ms);`
  - `PlayBot` calls `openBotDuel()`.

- [ ] **Step 3: Build, run all tests, and check by hand**

Run: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass. Then on `./build/tetris` (desktop, which has no server), check each of the following:
- vs Bot on Easy shows the countdown and the name `Bot-Easy`.
- The bot stacks and clears, and garbage flows both ways.
- Topping out three times shows the loss.
- Rematch restarts straight away.
- Leave returns to the menu.

If emscripten is available, run the web build and check that the bot also works there next to Quick match.

- [ ] **Step 4: Commit** `feat(app): practice duel against the bot from the start screen`

---

### Task 9: Final verification

- [ ] **Step 1:** `cmake -B build && cmake --build build 2>&1 | grep -i warning` prints nothing. Then `ctest --test-dir build --output-on-failure` passes.
- [ ] **Step 2:** The web build compiles (`emcmake …` as in Commands), if emscripten is installed. If it isn't, say so in the hand-off.
- [ ] **Step 3:** `git status` is clean and the history has one commit per task. Push `develop`.
