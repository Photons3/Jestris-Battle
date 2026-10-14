# Garbage Travel Delay and Per-Lock Cap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Incoming duel garbage travels 333 ms before it can rise, at most 8 lines rise per lock, and the meter shows travelling lines dimmed above ready ones for both players.

**Architecture:** `Game` replaces its pending-garbage counter with a queue of batches that count down in `step()`; cancelling and rising work on the queue oldest first. `Snapshot` carries a `ready` count next to `pending` so the opponent's meter can show the same split. `drawGarbageMeter` draws the two parts in different strengths.

**Tech Stack:** C++20, doctest, Dear ImGui, CMake (native tests; the web build is Emscripten).

**Spec:** `docs/superpowers/specs/2026-10-14-garbage-delay-cap-design.md`

## Global Constraints

- `kGarbageTravelMs = 333`; `kMaxGarbagePerLock = 8`; both in `src/core/game.hpp`.
- Travel counts down only in `GameState::Playing` and `GameState::Clearing`.
- Cancelling and rising both take from the queue oldest first; travelling lines never rise.
- `pendingGarbage()` keeps meaning "all queued lines"; `readyGarbage()` is the ready part.
- Wire form: `SNAP <score> <lines> <pending> <ready> <pieces> <attack> <garbageCleared> <cancelled> <tetrises> <tSpins> <maxCombo> <hold> <next> <cells>` (14 arguments); `ready > pending`, a negative `ready` and the 13-argument form are rejected.
- Danger red and blink still key on total pending (`garbageMeterDanger(pending)`, 4+ lines); travelling cells draw at about 40% alpha and never blink.
- No change to `server/`, bomb placement or garbage shape. No surge.
- Run tests with: `cmake -S . -B build && cmake --build build && ./build/tetris_tests && ./build/tetris_ui_tests` (needs SDL3 installed, as for any native build).

## Review Focus

1. **Many small attacks within 333 ms** (e.g. three `ATK 1` in a row): each is its own batch with its own timer; the later ones must not become ready early. Pinned by Task 1's "each batch travels on its own clock" test.
2. **A non-clearing lock while everything is still travelling:** nothing rises and no `GarbageRose` event or sound fires. Pinned by Task 1's "nothing ready, nothing rises" test.
3. **Cancelling more than the ready lines:** the attack must eat into travelling batches after the ready ones, and send only the rest. Pinned by Task 1's oldest-first cancel test.
4. **A KO flash or pause during travel:** the timer must hold, not jump when play resumes. Pinned by Task 1's pause test; the duel freeze reuses the same path (a frozen duel doesn't tick its game).
5. **An old or hand-written `SNAP` with `ready > pending`:** must be rejected rather than drawing more solid cells than the meter total. Pinned by Task 2's malformed-snapshot test.

---

### Task 1: Garbage queue in `Game`

**Files:**
- Modify: `src/core/game.hpp` (constants; `receiveGarbage`, `pendingGarbage`, new `readyGarbage`; private queue)
- Modify: `src/core/game.cpp` (`receiveGarbage`, `resetBoard`, `applyGarbage`, `step`, `finishClear`)
- Modify: `tests/test_helpers.hpp` (new `topOutWithGarbage`)
- Test: `tests/game_test.cpp`, `tests/duel_test.cpp`, `tests/sound_cues_test.cpp`

**Interfaces:**
- Produces (in `game.hpp`, namespace `tetris`):
  - `inline constexpr int kGarbageTravelMs = 333;`
  - `inline constexpr int kMaxGarbagePerLock = 8;`
  - `void Game::receiveGarbage(int lines, int travelMs = kGarbageTravelMs);` (tests pass 0 for garbage that is ready at once)
  - `int Game::pendingGarbage() const;` (sum of all batches)
  - `int Game::readyGarbage() const;` (sum of batches with `msLeft == 0`)
- Produces (in `tests/test_helpers.hpp`): `inline void topOutWithGarbage(tetris::Game& game);` — receives `kBoardHeight - 1` ready lines, then hard-drops (at most 10 times) until `state() == GameState::GameOver`, calling `game.takeEvents()` before each drop so only the final lock's events remain.

- [ ] **Step 1: Write the failing tests** in `tests/game_test.cpp`

```cpp
TEST_CASE("game: incoming garbage travels before it can rise") {
    Game game(seedWithFirstTwo(PieceType::T, PieceType::O), 1);
    game.receiveGarbage(2);
    CHECK(game.pendingGarbage() == 2);
    CHECK(game.readyGarbage() == 0);
    game.apply(Action::HardDrop);                 // locks while the garbage is still travelling
    CHECK(game.board().garbageRows() == 0);
    CHECK(game.pendingGarbage() == 2);
    game.tick(kGarbageTravelMs - 1);
    CHECK(game.readyGarbage() == 0);
    game.tick(1);
    CHECK(game.readyGarbage() == 2);
    game.apply(Action::HardDrop);
    CHECK(game.pendingGarbage() == 0);
    CHECK(game.board().garbageRows() == 2);
}
```

Also add:
- `"game: nothing ready, nothing rises"` — `receiveGarbage(3)`, hard drop at once; `takeEvents()` has no `GarbageRose`; `pendingGarbage() == 3`.
- `"game: each batch travels on its own clock"` — `receiveGarbage(1)`, `tick(200)`, `receiveGarbage(1)`, `tick(kGarbageTravelMs - 200)`; `readyGarbage() == 1`; `tick(200)`; `readyGarbage() == 2`.
- `"game: at most kMaxGarbagePerLock lines rise per lock"` — `receiveGarbage(12, 0)`, hard drop; `pendingGarbage() == 4`; garbage rows on the board `== 8` (use `board().garbageRows()`); next hard drop; `pendingGarbage() == 0`, `garbageRows() == 12`.
- `"game: clears cancel the oldest garbage first"` — on `boardWithWell(4, 5)` with an I first: `receiveGarbage(2, 0)` then `receiveGarbage(3)`; tetris (`dropUprightI`, `tick(kClearDelayMs)`) attacks 4: `pendingGarbage() == 1`, `readyGarbage() == 0` (the ready 2 went first, then 2 of the travelling 3; the clear takes `kClearDelayMs` = 300 ms, so the last line is still 33 ms from ready), `takeOutgoingAttack() == 0`.
- `"game: travel stops while paused"` — `receiveGarbage(1)`, `pause()`, `tick(1000)`, `readyGarbage() == 0`, `resume()`, `tick(kGarbageTravelMs)`, `readyGarbage() == 1` (pausing is allowed by default).
- `"game: resetBoard empties the garbage queue"` — `receiveGarbage(2, 0)`, `receiveGarbage(3)`, `resetBoard()`; both counts 0.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build && ./build/tetris_tests -tc="game: *garbage*,game: nothing ready*,game: each batch*,game: at most*,game: clears cancel*,game: travel stops*"`
Expected: compile error (`readyGarbage`, `kGarbageTravelMs`, two-argument `receiveGarbage` undefined).

- [ ] **Step 3: Implement the queue in `src/core/game.{hpp,cpp}`**

- Private `struct GarbageBatch { int lines; int msLeft; };` and `std::deque<GarbageBatch> incoming_;` replace `pendingGarbage_`.
- `receiveGarbage`: assert `lines >= 0 && travelMs >= 0`; append unless `lines == 0`.
- `step()`: before the existing state handling, when `state_` is `Playing` or `Clearing`, lower each batch's `msLeft` toward 0.
- `finishClear()`: `cancelled = std::min(attack, pendingGarbage())`; remove `cancelled` lines from the front of the queue (shrink, pop empty batches). Sending and `stats_` stay as today.
- `applyGarbage()`: take up to `kMaxGarbagePerLock` lines from front batches with `msLeft == 0`, stopping at the first travelling batch; return `true` with no event if none; otherwise pass that count to the existing `bombColumnsFor(...)`/`pushGarbage` path.
- `resetBoard()`: `incoming_.clear()`.
- Update the comment on `receiveGarbage` to describe travel and the cap.

- [ ] **Step 4: Update existing tests for the new rules**

- In `tests/game_test.cpp`, tests that expect garbage to rise on the next lock receive it ready: `receiveGarbage(n, 0)` in "pending garbage rises when a piece locks without clearing", the bomb-pair test, "a clearing lock holds garbage back until the next lock", and "game events: garbage rising after a lock is reported".
- Tests that relied on one lock raising `kBoardHeight - 1` or 25 lines:
  - "garbage that pushes blocks off the top ends the game" becomes `topOutWithGarbage(game); CHECK(game.state() == GameState::GameOver);`
  - "garbage that leaves no room to spawn ends the game": build the game on `boardWithWell(kVisibleHeight - kMaxGarbagePerLock - 2, 0)`, `receiveGarbage(kMaxGarbagePerLock, 0)`, hard drop, expect `GameOver`.
  - "resetBoard after a top-out…", "game events: … garbage overflow" and "game events: resetting the board keeps events…" use `topOutWithGarbage(game)`; the event checks stay `Events{kHardDropped, kToppedOut}` because the helper drains earlier events.
- In `tests/duel_test.cpp` and `tests/sound_cues_test.cpp`, replace each `receiveGarbage(kBoardHeight - 1); …apply(Action::HardDrop);` pair with `topOutWithGarbage(*duel.game());` (or `topOutWithGarbage(game);`).
- Tests that only check `pendingGarbage()` after an `ATK`, or cancelling, stay unchanged.

- [ ] **Step 5: Run the whole suite**

Run: `cmake --build build && ./build/tetris_tests`
Expected: all test cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/core/game.hpp src/core/game.cpp tests/test_helpers.hpp tests/game_test.cpp tests/duel_test.cpp tests/sound_cues_test.cpp
git commit -m "feat(core): garbage travels 333 ms and rises at most 8 lines per lock"
```

---

### Task 2: `ready` in the snapshot

**Files:**
- Modify: `src/net/protocol.hpp` (`Snapshot::ready`)
- Modify: `src/net/protocol.cpp` (`snapshotOf`, `encode`, `decodeSnapshot`, argument count)
- Modify: `docs/superpowers/specs/2026-10-13-lan-duel-design.md` (SNAP row)
- Test: `tests/protocol_test.cpp`

**Interfaces:**
- Consumes: `Game::readyGarbage()` (Task 1).
- Produces: `int Snapshot::ready = 0;` declared directly after `pending`, commented `// the part of pending that can rise now`.

- [ ] **Step 1: Write the failing tests** in `tests/protocol_test.cpp`

- In the round-trip test set `snapshot.ready = 2;` (its `pending` is 3).
- Every `SNAP` literal in the file gains one `0 ` after `pending`, e.g. `"SNAP 0 0 0 0 0 0 0 0 0 0 0 . ..... "`. Add one exact check: a snapshot with `pending = 5, ready = 3` encodes as `"SNAP 0 0 5 3 0 0 0 0 0 0 0 . ..... " + empty`.
- New `"protocol: malformed snapshot ready counts are rejected"`: `ready > pending` (`"SNAP 0 0 2 3 0 0 0 0 0 0 0 . ..... "`), negative ready, non-numeric ready, and the 13-argument form (today's string) all fail `decode`.
- In "a snapshot shows the visible field…": after `game.receiveGarbage(3)`, `CHECK(snapshotOf(game).ready == 0)`; after `game.tick(kGarbageTravelMs)`, `CHECK(snapshotOf(game).ready == 3)`. Put the tick before `dropToFloor` so the later cell checks are unchanged.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build && ./build/tetris_tests -tc="protocol:*"`
Expected: compile error (`ready` is not a member of `Snapshot`).

- [ ] **Step 3: Implement**

- `snapshotOf`: `ready = game.readyGarbage()`.
- `encode`: write `ready` after `pending`.
- `decodeSnapshot`: expect one more argument (every later index shifts by one; the stats loop starts at 5, hold at 12); parse `ready` with `parseNumber(…, 0, *pending)`.
- Duel spec: in the SNAP row, add `<ready>` after `<pending>` and "the part of pending garbage that can rise now; see `2026-10-14-garbage-delay-cap-design.md`".

- [ ] **Step 4: Run the whole suite**

Run: `cmake --build build && ./build/tetris_tests`
Expected: all test cases pass (`duel_test`'s `Snapshot{…, .pending = 1}` still compiles with `ready` defaulting to 0).

- [ ] **Step 5: Commit**

```bash
git add src/net/protocol.hpp src/net/protocol.cpp tests/protocol_test.cpp docs/superpowers/specs/2026-10-13-lan-duel-design.md
git commit -m "feat(net): ready garbage count in snapshots"
```

---

### Task 3: Two-shade garbage meter

**Files:**
- Modify: `src/app/render.hpp` (declare `drawGarbageMeter`)
- Modify: `src/app/render.cpp` (`drawGarbageMeter` and its two call sites in `drawDuelBoards`)
- Modify: `README.md` (one line)
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Consumes: `Game::readyGarbage()` (Task 1), `Snapshot::ready` (Task 2).
- Produces: `void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending, int ready);` moved out of the anonymous namespace and declared in `render.hpp` so the UI test can draw it.

- [ ] **Step 1: Write the failing test** in `tests/ui_test.cpp`

```cpp
TEST_CASE("ui: the garbage meter draws travelling lines above ready ones") {
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::GetIO().Fonts->Build();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const int before = dl->VtxBuffer.Size;
    drawGarbageMeter(dl, 10.0f, 800.0f, 30.0f, 5, 2);
    const int mixed = dl->VtxBuffer.Size - before;
    drawGarbageMeter(dl, 10.0f, 800.0f, 30.0f, 0, 0);
    CHECK(dl->VtxBuffer.Size - before == mixed);  // nothing pending draws nothing
    CHECK(mixed > 0);
    ImGui::EndFrame();
    ImGui::DestroyContext();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/tetris_ui_tests -tc="ui: the garbage meter*"`
Expected: compile error (`drawGarbageMeter` not declared, or wrong arity).

- [ ] **Step 3: Implement**

- `drawGarbageMeter(…, pending, ready)`: for cell `i` from the bottom (`i < min(pending, kVisibleHeight)`), cells with `i < ready` keep today's look (danger color and blink when `garbageMeterDanger(pending)`); cells with `i >= ready` use the same base color at alpha `0.4f` with no blink. Clamp `ready` to `[0, pending]`.
- Call sites: `drawGarbageMeter(…, game.pendingGarbage(), game.readyGarbage())` and `drawGarbageMeter(…, duel.opponent().pending, duel.opponent().ready)`.
- README: next to the duel rules, add "Incoming garbage travels for a third of a second before it can rise (the meter shows it dimmed), and at most 8 lines rise per lock."

- [ ] **Step 4: Run both suites**

Run: `cmake --build build && ./build/tetris_tests && ./build/tetris_ui_tests`
Expected: all test cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/app/render.hpp src/app/render.cpp tests/ui_test.cpp README.md
git commit -m "feat(app): garbage meter shows travelling lines dimmed"
```
