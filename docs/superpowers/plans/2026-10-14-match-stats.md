# Duel Stats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Live PPS and APM in the duel scoreboard, plus a result-screen table with PPS, APM, VS, Sent, Cancelled, Tetrises, T-spins and Max combo for both players.

**Architecture:** `Game` keeps a `GameStats` struct of running totals. `Snapshot` carries it to the opponent inside `SNAP`, which the server relays unread. `DuelSession` derives played time from the match clock, a pure `ratesOf` turns totals into rates, and a pure `resultStatRows` formats the table that `render.cpp` lays out.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui, doctest, Emscripten for the web build.

**Spec:** `docs/superpowers/specs/2026-10-14-match-stats-design.md`

## Global Constraints

- The Go server is untouched. `server/hub_test.go` stays as it is.
- `GameStats stats{}` goes **last** in `Snapshot`, after `next`, so existing designated initializers keep compiling. On the wire the stats go between `pending` and the hold (spec §2).
- The `SNAP` decoder is strict: exactly 13 arguments. The current 6-argument form is rejected.
- Stat formats: PPS `%.2f`, APM `%.1f`, VS `%.1f`, counts as integers, max combo `-` when 0.
- The duel layout stays 46 columns wide. No column constant changes.
- Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`.
- Conventional commits scoped like the history (`feat(core): …`, `feat(net): …`, `feat(app): …`), on `develop`.

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- One suite: `./build/tetris_tests -tc="game: stats*"` (doctest filter; use the test-case name prefix)
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/core/game.hpp`, `src/core/game.cpp` | `GameStats`, `Game::stats()`, counting in `lockPiece` and `finishClear` |
| `src/net/protocol.hpp`, `src/net/protocol.cpp` | `Snapshot::stats`, `snapshotOf`, `SNAP` encode and decode |
| `src/app/duel.hpp`, `src/app/duel.cpp` | `playedMs()`, `MatchRates`, `ratesOf`, `StatRow`, `resultStatRows` |
| `src/app/render.cpp` | PPS and APM scoreboard rows, `drawCenteredFit`, result table |
| `tests/game_test.cpp`, `tests/protocol_test.cpp`, `tests/duel_test.cpp`, `tests/local_match_test.cpp`, `tests/ui_test.cpp` | tests per task |

## Review Focus

1. **No opponent snapshot yet.** The opponent leaves, or `OVER` arrives, before any `SNAP`. The "Them" column should show zeros and `-`, not stale numbers from the previous match. Test in Task 3.
2. **Rematch.** The second match must start with zero stats and `playedMs() == 0`, with no leftovers from match one, for both us and the opponent. Test in Task 3.
3. **Practice bot.** The bot's snapshots must carry its stats, or the "Them" column is all zeros against the bot. Test in Task 2.
4. **A KO during a clear.** `resetBoard()` drops a clear in progress, so that clear's attack is never counted, but its piece and tetris were counted at lock. That is accepted: the tetris did happen. The stats must still survive the reset. Test in Task 1.
5. **Long values in the 7-cell centre column**, e.g. `123.4 : 98.7`. They must shrink rather than overlap the wells. Checked manually in Task 4.

---

### Task 1: Count stats in `Game`

**Files:**
- Modify: `src/core/game.hpp` (new struct above `class Game`, getter, member), `src/core/game.cpp` (`lockPiece`, `finishClear`)
- Test: `tests/game_test.cpp` (append at the end; the anonymous-namespace boards `tSpinDoubleBoard`, `bombBoard` and `comboBoard` are already in scope)

**Interfaces:**
- Produces: `struct GameStats { int pieces, attack, garbageCleared, cancelled, tetrises, tSpins, maxCombo; }`, all defaulting to 0 and in that order, with defaulted `operator==`. Also `const GameStats& Game::stats() const`.

- [ ] **Step 1: Write the failing tests** (prefix every name with `game: stats`)

```cpp
TEST_CASE("game: stats count locked pieces, not holds or the lock-out piece") {
    Game game(seedWithFirst(PieceType::T), 1);
    game.apply(Action::Hold);
    CHECK(game.stats().pieces == 0);
    game.apply(Action::HardDrop);
    game.apply(Action::HardDrop);
    CHECK(game.stats().pieces == 2);

    Board board;  // the lock-out board from "lock-out ends the game..."
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth - 1; ++x) board.set(x, y, PieceType::Z);
    Game out(seedWithFirst(PieceType::T), 1, board);
    out.apply(Action::HardDrop);
    REQUIRE(out.state() == GameState::GameOver);
    CHECK(out.stats().pieces == 0);
}

TEST_CASE("game: stats count tetrises, and attack before cancelling") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    game.receiveGarbage(3);
    dropUprightI(game);
    game.tick(kClearDelayMs);
    CHECK(game.stats().tetrises == 1);
    CHECK(game.stats().attack == 4);
    CHECK(game.stats().cancelled == 3);
    CHECK(game.takeOutgoingAttack() == 1);

    Game twoLines(seedWithFirst(PieceType::I), 1, boardWithWell(2, 5));
    dropUprightI(twoLines);
    twoLines.tick(kClearDelayMs);
    CHECK(twoLines.stats().tetrises == 0);
    CHECK(twoLines.stats().attack == 1);
}

TEST_CASE("game: stats count garbage rows cleared, by bomb or by a piece") {
    Game bombs(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
    bombs.apply(Action::HardDrop);
    bombs.tick(kClearDelayMs);
    CHECK(bombs.stats().garbageCleared == 2);

    Board board = boardWithWell(1, 5);  // row 0 becomes a garbage row with a hole at column 5
    for (int x = 0; x < kBoardWidth; ++x)
        if (x != 5) board.set(x, 0, PieceType::Garbage);
    Game piece(seedWithFirst(PieceType::I), 1, board);
    dropUprightI(piece);
    piece.tick(kClearDelayMs);
    CHECK(piece.stats().garbageCleared == 1);
}

TEST_CASE("game: stats count T-spins with and without lines, not a plain T drop") {
    Game spin(seedWithFirst(PieceType::T), 1, tSpinDoubleBoard());
    spin.apply(Action::RotateCW);
    dropToFloor(spin);
    spin.apply(Action::RotateCW);
    spin.apply(Action::HardDrop);
    CHECK(spin.stats().tSpins == 1);

    Board noLines = tSpinDoubleBoard();
    noLines.set(9, 0, std::nullopt);
    noLines.set(9, 1, std::nullopt);
    Game zero(seedWithFirst(PieceType::T), 1, noLines);
    zero.apply(Action::RotateCW);
    dropToFloor(zero);
    zero.apply(Action::RotateCW);
    zero.apply(Action::HardDrop);
    CHECK(zero.stats().tSpins == 1);
    CHECK(zero.stats().attack == 0);

    Game plain(seedWithFirst(PieceType::T), 1, tSpinDoubleBoard());
    plain.apply(Action::RotateCW);
    dropToFloor(plain);
    plain.apply(Action::HardDrop);
    CHECK(plain.stats().tSpins == 0);
}

TEST_CASE("game: stats keep the highest combo after it breaks") {
    Game game(seedWithFirstTwo(PieceType::I, PieceType::O), 1, comboBoard());
    dropUprightI(game);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    game.apply(Action::HardDrop);  // clears nothing; the combo breaks
    CHECK(game.combo() == -1);
    CHECK(game.stats().maxCombo == 1);
}

TEST_CASE("game: stats survive resetBoard, even mid-clear") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);  // Clearing; the attack is counted only when the clear finishes
    REQUIRE(game.state() == GameState::Clearing);
    game.resetBoard();
    CHECK(game.stats().pieces == 1);
    CHECK(game.stats().tetrises == 1);
    CHECK(game.stats().attack == 0);
}
```

- [ ] **Step 2: Run them and confirm they fail to compile** (`stats` is not a member of `Game`)

Run: `cmake --build build 2>&1 | grep -m1 "stats"`

- [ ] **Step 3: Implement** `GameStats` (with the field comments from spec §1) and `Game::stats()`. Then count as spec §1 lists:
  - `pieces`, `tetrises` (`award_.lines == 4`), `tSpins` (`award_.spin != Spin::None`) and `maxCombo` in `lockPiece()`, after the lock-out return and after `award_` is built.
  - `garbageCleared`, `attack` and `cancelled` in `finishClear()`, counting garbage rows before `removeRows`.
  - `resetBoard()` does not touch `stats_`.

- [ ] **Step 4: Run all C++ tests and confirm they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`

- [ ] **Step 5: Commit** — `feat(core): count pieces, attack, garbage, tetrises, T-spins and max combo`

---

### Task 2: Carry stats in `SNAP`

**Files:**
- Modify: `src/net/protocol.hpp` (`Snapshot`), `src/net/protocol.cpp` (`snapshotOf`, encoder, `decodeSnapshot`, the `verb == "SNAP" && args == 6` branch becomes `args == 13`)
- Test: `tests/protocol_test.cpp`, `tests/local_match_test.cpp`

**Interfaces:**
- Consumes: `GameStats`, `Game::stats()` (Task 1)
- Produces: `GameStats Snapshot::stats{}`, the last member. Wire form: `SNAP <score> <lines> <pending> <pieces> <attack> <garbageCleared> <cancelled> <tetrises> <tSpins> <maxCombo> <hold> <next> <cells>`

- [ ] **Step 1: Update and add the tests**
  - Round trip: give the existing `snapshot` in "every message survives…" the stats `{.pieces = 40, .attack = 12, .garbageCleared = 5, .cancelled = 3, .tetrises = 2, .tSpins = 1, .maxCombo = 4}`.
  - Plain text: every expected `"SNAP 0 0 0 . ..... "` becomes `"SNAP 0 0 0 0 0 0 0 0 0 0 . ..... "`, and the `held` cases get the same seven zeros. Add:
    ```cpp
    Snapshot counted{};
    counted.stats = {40, 12, 5, 3, 2, 1, 4};
    CHECK(encode(counted) == "SNAP 0 0 0 40 12 5 3 2 1 4 . ..... " + empty);
    ```
  - Malformed: update the existing `SNAP` strings to the 13-argument form so each one still fails only for its own reason. Add a test case named `"protocol: malformed snapshot stats are rejected"`:
    ```cpp
    const std::string cells(kSnapshotCells, '.');
    CHECK(decode("SNAP 0 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 . ..... " + cells).has_value());          // the 6-argument form
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 0 0 0 . ..... " + cells).has_value());  // one stat short
    CHECK_FALSE(decode("SNAP 0 0 0 0 -1 0 0 0 0 0 . ..... " + cells).has_value());
    CHECK_FALSE(decode("SNAP 0 0 0 0 0 0 x 0 0 0 . ..... " + cells).has_value());
    ```
  - In `local_match_test.cpp`, add `"local match: the bot's snapshots carry its stats"`. Use `LocalMatch match(BotLevel::Hard, 1)` and `startPlaying(match)`, then `match.update(5000)`. Take the last `Snapshot` in `allOf<Snapshot>(drain(match))` and `CHECK(last.stats == match.botGame()->stats())` and `CHECK(last.stats.pieces > 0)`. (`update` may need to run in 1000 ms steps if a single large step trips the frame clamp; follow the existing tests' pattern.)

- [ ] **Step 2: Run the tests and confirm they fail** (the round trip and plain-text checks)

Run: `cmake --build build && ./build/tetris_tests -tc="protocol*,local match*"`

- [ ] **Step 3: Implement.** `snapshotOf` sets `snapshot.stats = game.stats()`. The encoder writes the seven numbers after `pending`. `decodeSnapshot` parses `parts[4..10]` with `parseNumber(part, 0, kMaxInt)`; hold, next and cells move to `parts[11]`, `parts[12]` and `parts[13]`.

- [ ] **Step 4: Run all C++ tests and confirm they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`. `duel_test`'s `"opponent snapshots are kept for display"` still passes, because it encodes through `encode`.

- [ ] **Step 5: Commit** — `feat(net): match stats in snapshots`

---

### Task 3: Played time, rates and the result rows in `DuelSession`

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/duel_test.cpp`

**Interfaces:**
- Consumes: `Snapshot::stats` (Task 2), `Game::stats()` (Task 1)
- Produces:
  - `int DuelSession::playedMs() const`, which returns `kMatchMs - matchClockMs_`.
  - `struct MatchRates { double pps = 0.0; double apm = 0.0; double vs = 0.0; };`
  - `MatchRates ratesOf(const GameStats& stats, int playedMs);`
  - `struct StatRow { std::string label; std::string mine; std::string theirs; }` with defaulted `operator==`.
  - `std::vector<StatRow> resultStatRows(const DuelSession& duel);`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("duel stats: rates are per second and per minute of play, zero before any") {
    CHECK(ratesOf(GameStats{.pieces = 5}, 0).pps == 0.0);
    const MatchRates r = ratesOf(GameStats{.pieces = 60, .attack = 30, .garbageCleared = 10}, 60000);
    CHECK(r.pps == doctest::Approx(1.0));
    CHECK(r.apm == doctest::Approx(30.0));
    CHECK(r.vs == doctest::Approx(66.6667).epsilon(0.0001));
}

TEST_CASE("duel stats: played time counts only play, and stops with the match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("START 7 1");
    duel.update(kCountdownMs - 1);
    CHECK(duel.playedMs() == 0);
    duel.update(1);  // ends the countdown; update() returns there, so play time starts with the next call
    CHECK(duel.playedMs() == 0);
    duel.update(1500);
    CHECK(duel.playedMs() == 1500);
    duel.onMessage("OVER WIN KO 0 0");
    duel.update(1000);
    CHECK(duel.playedMs() == 1500);
}

TEST_CASE("duel stats: our snapshots carry our stats, theirs land in opponent()") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->apply(Action::HardDrop);
    duel.update(kSnapshotIntervalMs);
    REQUIRE_FALSE(wire.sent.empty());
    const auto sent = decode(wire.sent.back());
    REQUIRE(sent.has_value());
    CHECK(std::get<Snapshot>(*sent).stats.pieces == 1);
    Snapshot theirs{};
    theirs.stats.tetrises = 2;
    duel.onMessage(encode(theirs));
    CHECK(duel.opponent().stats.tetrises == 2);
}

TEST_CASE("duel stats: the result table, ours on the left") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    CHECK(resultStatRows(duel).empty());
    duel.update(60000);
    Snapshot theirs{};
    theirs.stats = {.pieces = 90, .attack = 45, .garbageCleared = 15, .cancelled = 6, .tetrises = 3, .tSpins = 2,
                    .maxCombo = 5};
    duel.onMessage(encode(theirs));
    duel.onMessage("OVER LOSE TIME 4 39");
    const std::vector<StatRow> rows = resultStatRows(duel);
    std::vector<std::string> labels;
    for (const StatRow& row : rows) labels.push_back(row.label);
    CHECK(labels == std::vector<std::string>{"PPS", "APM", "VS", "Sent", "Cancelled", "Tetrises", "T-spins",
                                             "Max combo"});
    CHECK(rows[0] == StatRow{"PPS", "0.00", "1.50"});
    CHECK(rows[1] == StatRow{"APM", "0.0", "45.0"});
    CHECK(rows[2] == StatRow{"VS", "0.0", "100.0"});
    CHECK(rows[3] == StatRow{"Sent", "4", "39"});
    CHECK(rows[7] == StatRow{"Max combo", "-", "5"});
}

TEST_CASE("duel stats: no opponent snapshot shows zeros, and a rematch starts from zero") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    Snapshot theirs{};
    theirs.stats.pieces = 30;
    duel.onMessage(encode(theirs));
    duel.game()->apply(Action::HardDrop);
    duel.update(5000);
    duel.onMessage("OVER WIN KO 0 0");
    duel.requestRematch();
    duel.onMessage("START 8 1");
    duel.update(kCountdownMs);
    CHECK(duel.playedMs() == 0);
    CHECK(duel.game()->stats() == GameStats{});
    CHECK(duel.opponent().stats == GameStats{});
    duel.onMessage("LEFT");  // leaves before sending a single SNAP
    const std::vector<StatRow> rows = resultStatRows(duel);
    REQUIRE(rows.size() == 8);
    CHECK(rows[0].theirs == "0.00");
    CHECK(rows[7].theirs == "-");
}
```

(The table test's "mine" values are zero because the local game locked no piece. The test only needs the formatting and the column order.)

- [ ] **Step 2: Run them and confirm they fail to compile**

Run: `cmake --build build 2>&1 | grep -m1 "playedMs\|ratesOf"`

- [ ] **Step 3: Implement** `playedMs()` in the header as above, and `ratesOf` from spec §3: PPS = pieces ÷ s, APM = attack ÷ min, VS = (attack + garbageCleared) ÷ s × 100, and all zero when `playedMs <= 0`. Then `resultStatRows` from the spec §5 table: return empty unless `phase() == DuelPhase::Result`. "Mine" is `game()->stats()` (empty `GameStats` if `game()` is null), "theirs" is `opponent().stats`, and Sent comes from `sentMine()` / `sentTheirs()`. Use `std::snprintf` for the decimals, like the rest of `duel.cpp` and `render.cpp`.

- [ ] **Step 4: Run all C++ tests and confirm they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`

- [ ] **Step 5: Commit** — `feat(app): played time, PPS/APM/VS and the result stats rows`

---

### Task 4: Draw the live rows and the result table

**Files:**
- Modify: `src/app/render.cpp` (`drawScoreboard` ~line 479, new `drawCenteredFit` beside `drawCentered`, `drawDuelResult` ~line 769)
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Consumes: `playedMs()`, `ratesOf`, `resultStatRows` (Task 3)
- Produces: `void drawCenteredFit(ImDrawList* dl, const Fonts& fonts, float size, float centerX, float y, const char* text, ImU32 col, float maxWidth)`. It scales `size` down by `maxWidth / width` when the text is wider than `maxWidth`, then calls `drawCentered`.

- [ ] **Step 1: Write the failing UI test** `"ui: the duel result draws the stats table"`. Set it up like `"ui: the lobby draws its room list"`, but take the session to `Result`: `createRoom(1)`, `"START 7 1"`, `update(kCountdownMs + 1000)`, `"OVER WIN KO 3 1"`. Draw two frames with `drawDuel(...)` and `CHECK` that each returns `UiCommand::None`. The test fails only if ImGui asserts on an unbalanced table, so first write it against the current code and confirm it passes. Then keep it as a regression test while adding the table. (TDD has no red step here, because the drawing is not observable headlessly.)

- [ ] **Step 2: Implement the scoreboard rows** in `drawScoreboard`, after SENT:
  - Change the spacing after COMBO from `3.5f` to `3.0f`.
  - Add `y += cell * 3.0f;`, a `PPS` label and the `"%.2f : %.2f"` value.
  - Then `y += cell * 3.0f;`, an `APM` label and the `"%.1f : %.1f"` value.
  - Mine is `ratesOf(game->stats(), duel.playedMs())` and theirs is `ratesOf(duel.opponent().stats, duel.playedMs())`. `drawDuelBoards` already has `const Game& game = *duel.game()`; pass it in or read `duel.game()`.
  - Draw the PPS and APM values with `drawCenteredFit(..., kCenterColumns * cell)`, using the same sizes and colours as SENT.

- [ ] **Step 3: Implement the result table** in `drawDuelResult`, between `TextUnformatted(resultReasonText...)` and the `Spacing()` before the controls:
  - `ImGui::BeginTable("##stats", 3, ImGuiTableFlags_SizingFixedFit)`.
  - Headers: `""`, `"You"`, then `opponentName()` or `"Them"` if it's empty.
  - Then one row per `resultStatRows(duel)` entry, and `EndTable()` only when `BeginTable` returned true.

- [ ] **Step 4: Run all C++ tests and confirm they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`

- [ ] **Step 5: Check it manually in the browser**
  - Build for the web and run `go -C server run .`.
  - Play **vs Bot** for a while: the PPS and APM rows update, and the APM value stays inside the centre column.
  - Finish the match: the table shows both columns, and "Them" is non-zero.
  - Play a two-tab online duel: each tab's "Them" column matches the other tab's "You" column, to within the last piece.
  - Save screenshots of the scoreboard and of the result table to the scratchpad.

- [ ] **Step 6: Commit** — `feat(app): live PPS and APM rows and the result stats table`
