# Bomb Battle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the LAN duel into a Tetris Battle–style match: bomb garbage that a piece detonates by landing on it, KO-and-reset on top-out, first to 3 KOs or a 2:00 clock refereed by the server, and an equal-size two-board duel screen with a center scoreboard.

**Architecture:**
- `tetris_core` gains a `Bomb` cell, bomb-column garbage, detonation in `Game::lockPiece`, and `Game::resetBoard()` for KOs.
- The wire protocol gains two server-only verbs, `KO` and `OVER`.
- The Go hub stops relaying `DEAD`. It counts KOs and lines sent itself, owns the match timer, and sends one verdict to both players.
- `DuelSession` tracks the KO counts, the display clock, the KO flash and a `frozen()` gate. The app stops ticking the game while it is frozen.
- The duel screen in `render.cpp` is redrawn as layout A.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build, Go 1.27 + `github.com/coder/websocket`.

**Spec:** `docs/superpowers/specs/2026-10-13-bomb-battle-design.md`

## Global Constraints

- C++20. Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`. Desktop Debug builds keep ASan/UBSan.
- Solo play and the desktop build stay unchanged. Bombs only ever come from duel garbage.
- Web build: no pthreads or SharedArrayBuffer.
- `PieceType::Bomb` is appended after `Garbage`. The bag never deals it (`kPieceTypeCount` stays 7). It goes over the wire as `B` in snapshots.
- Attack is `attackFor(min(cleared, 4))` from the 0/1/2/4 table. The score is also capped at 4 lines. `lines_` adds the full `cleared` count.
- Countdown is 3000 ms (`kCountdownMs` in C++, `countdown` in Go, and each cites the other). The match is 2:00 (`kMatchMs = 120000`, Go `-match` default `2m`). The server grace is 500 ms. The KO flash is 1000 ms. 3 KOs win. `ATK n` is valid for n = 1–40.
- Timeout verdict: more KOs wins, then more lines sent (net, after cancelling), otherwise `DRAW`.
- `KO <mine> <theirs>` and `OVER <WIN|LOSE|DRAW> <KO|TIME> <mySent> <theirSent>` are server-only. The server drops them if a client sends them.
- The clock text is drawn in red under 0:10. Bars say YOU / FOE. Your number is always on the left.
- Conventional commits scoped like the existing history (`feat(core): …`, `feat(net): …`, `feat(server): …`, `feat(app): …`, `docs: …`).
- Work on branch `feat/bomb-battle`, not `main`.

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- One doctest group: `./build/tetris_tests -tc="board:*"` (wildcards allowed)
- Go tests: `go -C server test -race ./...` and `go -C server vet ./...`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web` (prefix with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14` if emcc complains about Python)

## File map

| File | Change |
|---|---|
| `src/core/piece.hpp` | `PieceType::Bomb` |
| `src/core/board.hpp/.cpp` | bombs in `pushGarbage`, `fullRows` skips bomb rows, `bombRowsHitBy` |
| `src/core/game.hpp/.cpp` | detonation in `lockPiece`, 4-line cap in `finishClear`, `resetBoard()` |
| `src/net/protocol.hpp/.cpp` | `B` cell char, `KoCounts`, `MatchOver`, `kKosToWin` |
| `server/hub.go`, `server/main.go` | match state, timer, referee, `-match` flag |
| `src/app/duel.hpp/.cpp` | KO counts, sent tallies, clock, KO flash, `frozen()`, Draw, `resultReasonText` |
| `src/app/app.cpp` | `activeGame()` returns null while frozen |
| `src/app/render.cpp` | bomb cell look, layout A duel screen, KO flash, result lines |
| `tests/*.cpp`, `server/hub_test.go` | tests per task |
| `README.md`, `docs/superpowers/specs/2026-10-13-lan-duel-design.md` | new rules, `-match` flag, pointer to this spec |

---

### Task 1: Bomb cells in garbage rows

**Files:**
- Modify: `src/core/piece.hpp:8-10`
- Modify: `src/core/board.hpp`, `src/core/board.cpp`
- Modify: `src/net/protocol.cpp:12` (cell chars)
- Modify: `src/app/render.cpp:41-43` (`pieceRgb`)
- Test: `tests/board_test.cpp`, `tests/game_test.cpp` (two existing garbage tests), `tests/protocol_test.cpp`

**Interfaces:**
- Produces: `PieceType::Bomb`; `bool Board::pushGarbage(int rows, int bombX)`; `std::vector<int> Board::bombRowsHitBy(const Piece& piece) const` (rows sorted bottom-up, no duplicates); `fullRows()` never lists a row containing a `Bomb`.

- [ ] **Step 0: Branch**

```bash
git checkout -b feat/bomb-battle
```

- [ ] **Step 1: Write the failing board tests**

In `tests/board_test.cpp`, replace the test `"board: garbage pushes the stack up and leaves one gap per row"` with the test below, and append the others at the end of the file:

```cpp
namespace {

// An O piece whose lower-left cell is (x, bottomY); it fills columns x..x+1, rows bottomY..bottomY+1.
Piece oAt(int x, int bottomY) { return Piece{PieceType::O, Rotation::Zero, {x - 1, bottomY + 1}}; }

}  // namespace

TEST_CASE("board: garbage pushes the stack up with a bomb in the same column of every row") {
    Board board;
    board.set(2, 0, PieceType::T);
    REQUIRE(board.pushGarbage(2, 7));
    CHECK(board.at(2, 2) == PieceType::T);
    CHECK_FALSE(board.at(2, 3).has_value());
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            CHECK(board.at(x, y) == (x == 7 ? PieceType::Bomb : PieceType::Garbage));
}
```

```cpp
TEST_CASE("board: a bomb row is never full") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    for (int x = 0; x < kBoardWidth; ++x) board.set(x, 1, PieceType::I);
    CHECK(board.fullRows() == std::vector<int>{1});
}

TEST_CASE("board: a piece resting directly on a bomb hits its row") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    CHECK(board.bombRowsHitBy(oAt(4, 1)) == std::vector<int>{0});
}

TEST_CASE("board: a piece beside a bomb, or above it with a gap, hits nothing") {
    Board board;
    REQUIRE(board.pushGarbage(1, 4));
    CHECK(board.bombRowsHitBy(oAt(2, 1)).empty());  // columns 2-3, beside the bomb
    CHECK(board.bombRowsHitBy(oAt(5, 1)).empty());  // columns 5-6, beside the bomb
    CHECK(board.bombRowsHitBy(oAt(4, 2)).empty());  // row 1 is empty between the piece and the bomb
}

TEST_CASE("board: the chain takes the bombs below and stops at the first non-bomb cell") {
    Board board;
    REQUIRE(board.pushGarbage(3, 4));  // older batch: bombs in column 4
    REQUIRE(board.pushGarbage(1, 6));  // newer batch underneath: column 4 is plain garbage in row 0
    CHECK(board.bombRowsHitBy(oAt(4, 4)) == std::vector<int>{1, 2, 3});
}

TEST_CASE("board: touching batches that share a column chain together") {
    Board board;
    REQUIRE(board.pushGarbage(2, 4));
    REQUIRE(board.pushGarbage(2, 4));
    CHECK(board.bombRowsHitBy(oAt(4, 4)) == std::vector<int>{0, 1, 2, 3});
}
```

- [ ] **Step 2: Update the two game tests that assumed an open gap**

In `tests/game_test.cpp`, in `"game: pending garbage rises when a piece locks without clearing"`, replace everything from the `countFilled` check to the end of the test with:

```cpp
    CHECK(countFilled(game.board()) == 4 + 2 * kBoardWidth);
    int bombX = -1;
    for (int x = 0; x < kBoardWidth; ++x)
        if (game.board().at(x, 0) == PieceType::Bomb) bombX = x;
    REQUIRE(bombX >= 0);
    for (int y = 0; y < 2; ++y)  // the same bomb column in every row of the batch
        for (int x = 0; x < kBoardWidth; ++x)
            CHECK(game.board().at(x, y) == (x == bombX ? PieceType::Bomb : PieceType::Garbage));
}
```

In `"game: a clearing lock holds garbage back until the next lock"`, change the last line to:

```cpp
    CHECK(countFilled(game.board()) == 3 + 4 + kBoardWidth);
```

- [ ] **Step 3: Add a bomb to the protocol round-trip test**

In `tests/protocol_test.cpp`, in `"protocol: every message survives an encode/decode round trip"`, after `snapshot.cells[kSnapshotCells - 1] = PieceType::I;` add:

```cpp
    snapshot.cells[1] = PieceType::Bomb;
```

and in `"protocol: the wire format is plain text"` add:

```cpp
    Snapshot bomb{};
    bomb.cells[0] = PieceType::Bomb;
    CHECK(encode(bomb) == "SNAP 0 0 0 B" + std::string(kSnapshotCells - 1, '.'));
```

- [ ] **Step 4: Run the tests and watch them fail to compile**

Run: `cmake --build build`
Expected: compile errors: `no member named 'Bomb' in 'tetris::PieceType'` and `no member named 'bombRowsHitBy'`.

- [ ] **Step 5: Implement**

`src/core/piece.hpp`: replace the comment and enum:

```cpp
// Garbage and Bomb only appear in board cells (sent by a duel opponent); the bag never deals them.
enum class PieceType : std::uint8_t { I, O, T, S, Z, J, L, Garbage, Bomb };
```

`src/core/board.hpp`: replace the `fullRows` and `pushGarbage` declarations and add `bombRowsHitBy`:

```cpp
    // Complete rows, bottom-up. A row holding a bomb is never complete; it clears only by detonation.
    std::vector<int> fullRows() const;
    void removeRows(const std::vector<int>& rows);
    // Pushes the stack up by `rows` and fills the bottom rows with garbage, with a bomb in column `bombX`.
    // Returns false and changes nothing if a filled cell would be pushed off the top.
    bool pushGarbage(int rows, int bombX);
    // Rows a just-locked `piece` sets off: each bomb directly under one of its cells, plus every bomb
    // touching that one below it in the same column. Sorted bottom-up, without duplicates.
    std::vector<int> bombRowsHitBy(const Piece& piece) const;
```

`src/core/board.cpp`: replace `fullRows` and `pushGarbage`, and add `bombRowsHitBy` after `pushGarbage`:

```cpp
std::vector<int> Board::fullRows() const {
    std::vector<int> rows;
    for (int y = 0; y < kBoardHeight; ++y) {
        bool full = true;
        for (int x = 0; x < kBoardWidth && full; ++x) {
            const Cell cell = cells_[index(x, y)];
            full = cell.has_value() && *cell != PieceType::Bomb;
        }
        if (full) rows.push_back(y);
    }
    return rows;
}
```

```cpp
bool Board::pushGarbage(int rows, int bombX) {
    assert(rows >= 0 && bombX >= 0 && bombX < kBoardWidth);
    rows = std::min(rows, kBoardHeight);
    for (int y = kBoardHeight - rows; y < kBoardHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (cells_[index(x, y)]) return false;
    for (int y = kBoardHeight - 1; y >= rows; --y)
        for (int x = 0; x < kBoardWidth; ++x) cells_[index(x, y)] = cells_[index(x, y - rows)];
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            cells_[index(x, y)] = x == bombX ? Cell{PieceType::Bomb} : Cell{PieceType::Garbage};
    return true;
}

std::vector<int> Board::bombRowsHitBy(const Piece& piece) const {
    std::vector<int> rows;
    for (Point c : piece.cells())
        for (int y = c.y - 1; y >= 0 && cells_[index(c.x, y)] == PieceType::Bomb; --y) rows.push_back(y);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}
```

`src/core/game.cpp`: in `applyGarbage`, rename the local `gapX` to `bombX` (both lines), and change the salt comment to `// keeps bomb columns independent of the bag`. In `game.hpp`, change the `garbageRng_` comment to `// picks bomb columns; need not match the opponent's`.

`src/net/protocol.cpp`: change the cell alphabet:

```cpp
constexpr std::string_view kCellChars = "IOTSZJLGB";  // index matches PieceType
```

`src/app/render.cpp`: make `pieceRgb` cover bombs, so nothing indexes past `kPieceColors`. The bomb decoration comes in Task 7.

```cpp
Rgb pieceRgb(PieceType type) {
    if (type == PieceType::Garbage || type == PieceType::Bomb) return kGarbageColor;
    return kPieceColors[static_cast<int>(type)];
}
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all tests pass, with no warnings in the build output.

- [ ] **Step 7: Commit**

```bash
git add src/core/piece.hpp src/core/board.hpp src/core/board.cpp src/core/game.hpp src/core/game.cpp src/net/protocol.cpp src/app/render.cpp tests/board_test.cpp tests/game_test.cpp tests/protocol_test.cpp
git commit -m "feat(core): bomb cells in garbage rows and Board::bombRowsHitBy"
```

---

### Task 2: Detonation when a piece locks

**Files:**
- Modify: `src/core/game.cpp` (`lockPiece`, `finishClear`)
- Test: `tests/game_test.cpp`

**Interfaces:**
- Consumes: `Board::bombRowsHitBy`, `Board::fullRows` (Task 1).
- Produces: a lock that hits bombs enters `GameState::Clearing` with `clearingRows()` = full rows ∪ exploded rows (sorted). In `finishClear`, attack and score use `min(cleared, 4)` and `lines()` adds the full `cleared`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/game_test.cpp`:

```cpp
namespace {

// `bombRows` garbage rows with a bomb in column `bombX`, and above them `openRows` rows that are full except
// columns 4-5, which is where a freshly spawned O lands.
Board bombBoard(int bombRows, int bombX, int openRows) {
    Board board;
    board.pushGarbage(bombRows, bombX);
    for (int y = bombRows; y < bombRows + openRows; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (x != 4 && x != 5) board.set(x, y, PieceType::Z);
    return board;
}

}  // namespace

TEST_CASE("game: landing on a bomb clears its whole stack and counts as cleared lines") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::Clearing);
    CHECK(game.clearingRows() == std::vector<int>{0, 1});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 2);
    CHECK(game.takeOutgoingAttack() == 1);
    CHECK(countFilled(game.board()) == 4);  // just the O, dropped to the floor
}

TEST_CASE("game: landing beside a bomb sets nothing off") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 7, 0));
    game.apply(Action::HardDrop);
    CHECK(game.state() == GameState::Playing);
    CHECK(game.lines() == 0);
    CHECK(countFilled(game.board()) == 2 * kBoardWidth + 4);
}

TEST_CASE("game: a bomb hit and a normal clear in one lock add up to one attack") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(1, 4, 2));
    game.apply(Action::HardDrop);
    CHECK(game.clearingRows() == std::vector<int>{0, 1, 2});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 3);
    CHECK(game.takeOutgoingAttack() == 2);
}

TEST_CASE("game: more than four cleared rows are capped at the four-line attack and score") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(4, 4, 2));
    const int distance = game.ghostDistance();
    game.apply(Action::HardDrop);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 6);
    CHECK(game.takeOutgoingAttack() == 4);
    CHECK(game.score() == 2 * distance + lineClearScore(4, 1));
}

TEST_CASE("game: a detonating lock cancels pending garbage and holds the rest back") {
    Game game(seedWithFirst(PieceType::O), 1, bombBoard(2, 4, 0));
    game.receiveGarbage(3);
    game.apply(Action::HardDrop);
    CHECK(game.pendingGarbage() == 3);
    game.tick(kClearDelayMs);
    CHECK(game.pendingGarbage() == 2);  // the 1-line attack cancelled one
    CHECK(game.takeOutgoingAttack() == 0);
    CHECK(countFilled(game.board()) == 4);  // nothing rose
}

TEST_CASE("game: a clear that drops a cell onto a bomb does not detonate it") {
    Board board;
    board.pushGarbage(1, 4);                                           // row 0: bomb in column 4
    for (int x = 0; x < kBoardWidth; ++x)
        if (x != 5) board.set(x, 1, PieceType::Z);                     // row 1: open only in column 5
    board.set(4, 2, PieceType::Z);                                     // falls onto the bomb after the clear
    Game game(seedWithFirst(PieceType::I), 1, board);
    dropUprightI(game);                                                // fills column 5, resting on garbage
    CHECK(game.clearingRows() == std::vector<int>{1});
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 1);
    CHECK(game.board().at(4, 0) == PieceType::Bomb);
    CHECK(game.board().at(4, 1) == PieceType::Z);
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build build && ./build/tetris_tests -tc="game:*"`
Expected: the new bomb tests FAIL (for example, `game.state() == GameState::Clearing` is false because `Playing` != `Clearing`). The 6-row test may abort in `lineClearScore` or `attackFor` on the `assert(lines >= 0 && lines <= 4)` once detonation exists, which is why Step 3 adds the cap.

- [ ] **Step 3: Implement**

In `src/core/game.cpp`, add a constant inside the existing anonymous namespace:

```cpp
constexpr int kMaxScoredLines = 4;  // bombs can clear more rows at once; attack and score stop at a tetris
```

In `Game::lockPiece`, replace `clearingRows_ = board_.fullRows();` with:

```cpp
    clearingRows_ = board_.fullRows();
    const std::vector<int> exploded = board_.bombRowsHitBy(active_);  // bomb rows are never full, so no overlap
    clearingRows_.insert(clearingRows_.end(), exploded.begin(), exploded.end());
    std::sort(clearingRows_.begin(), clearingRows_.end());
```

Replace the start of `Game::finishClear` (the lines that compute `cleared` through `score_ +=`):

```cpp
void Game::finishClear() {
    const int cleared = static_cast<int>(clearingRows_.size());
    const int counted = std::min(cleared, kMaxScoredLines);
    const int attack = attackFor(counted);
    const int cancelled = std::min(attack, pendingGarbage_);
    pendingGarbage_ -= cancelled;
    outgoingAttack_ += attack - cancelled;
    score_ += lineClearScore(counted, level_);
    lines_ += cleared;
```

(The rest of `finishClear` stays as it is. `<algorithm>` is already included, since `lockPiece` uses `std::all_of`.)

- [ ] **Step 4: Run all the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/core/game.cpp tests/game_test.cpp
git commit -m "feat(core): landing on a bomb detonates its stack as cleared lines"
```

---

### Task 3: `Game::resetBoard` for KOs

**Files:**
- Modify: `src/core/game.hpp`, `src/core/game.cpp`
- Test: `tests/game_test.cpp`

**Interfaces:**
- Produces: `void Game::resetBoard();`. It empties the board and drops pending garbage, the hold and any clear in progress. It sets `state()` to `Playing` and spawns `next(1)[0]` at `kSpawnPos`. `score()`, `lines()`, `level()` and the bag carry on. It works from `GameOver`, `Playing` or `Clearing`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/game_test.cpp`:

```cpp
TEST_CASE("game: resetBoard after a top-out empties everything but keeps score, lines and level") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    game.tick(kClearDelayMs);  // a tetris: 4 lines
    game.apply(Action::Hold);
    game.receiveGarbage(kBoardHeight - 1);
    game.apply(Action::HardDrop);
    REQUIRE(game.state() == GameState::GameOver);
    const int score = game.score();
    const PieceType next = game.next(1)[0];

    game.resetBoard();
    CHECK(game.state() == GameState::Playing);
    CHECK(countFilled(game.board()) == 0);
    CHECK(game.pendingGarbage() == 0);
    CHECK_FALSE(game.hold().has_value());
    CHECK(game.canHold());
    CHECK(game.score() == score);
    CHECK(game.lines() == 4);
    CHECK(game.level() == 1);
    CHECK(game.active().type == next);
    CHECK(game.active().pos == kSpawnPos);
}

TEST_CASE("game: resetBoard during a clear drops the clear") {
    Game game(seedWithFirst(PieceType::I), 1, boardWithWell(4, 5));
    dropUprightI(game);
    REQUIRE(game.state() == GameState::Clearing);
    game.resetBoard();
    CHECK(game.state() == GameState::Playing);
    CHECK(game.clearingRows().empty());
    CHECK(game.lines() == 0);
    CHECK(countFilled(game.board()) == 0);
    game.tick(kClearDelayMs);
    CHECK(game.lines() == 0);  // the dropped clear never finishes
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `cmake --build build`
Expected: `no member named 'resetBoard' in 'tetris::Game'`.

- [ ] **Step 3: Implement**

`src/core/game.hpp`, after `takeOutgoingAttack()`:

```cpp
    // Starts over on an empty board after a duel KO. Drops pending garbage, the hold and any clear in
    // progress, then spawns the next piece from the same bag. Score, lines and level carry on.
    void resetBoard();
```

`src/core/game.cpp`, after `Game::takeOutgoingAttack`:

```cpp
void Game::resetBoard() {
    board_ = Board{};
    pendingGarbage_ = 0;
    hold_.reset();
    canHold_ = true;
    clearingRows_.clear();
    clearTimerMs_ = 0;
    state_ = GameState::Playing;
    spawn(bag_.next());
}
```

- [ ] **Step 4: Run all the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/core/game.hpp src/core/game.cpp tests/game_test.cpp
git commit -m "feat(core): Game::resetBoard keeps score, lines and level for duel KOs"
```

---

### Task 4: `KO` and `OVER` on the wire

**Files:**
- Modify: `src/net/protocol.hpp`, `src/net/protocol.cpp`
- Test: `tests/protocol_test.cpp`

**Interfaces:**
- Produces (namespace `tetris`, in `net/protocol.hpp`):

```cpp
inline constexpr int kKosToWin = 3;
enum class MatchResult { Win, Lose, Draw };
enum class MatchReason { KOs, Time };
struct KoCounts { int mine = 0; int theirs = 0; };                      // "KO <mine> <theirs>"
struct MatchOver { MatchResult result; MatchReason reason; int mySent; int theirSent; };  // "OVER ..."
// Message gains KoCounts and MatchOver at the end of the variant.
```

- [ ] **Step 1: Write the failing tests**

In `tests/protocol_test.cpp`:

1. In the round-trip test, replace the `messages` vector with:

```cpp
    const std::vector<Message> messages{CreateRoom{3},
                                        JoinRoom{"ABCD"},
                                        RoomCreated{"WXYZ"},
                                        StartMatch{4000000000u, 15},
                                        Attack{4},
                                        snapshot,
                                        Dead{},
                                        Rematch{},
                                        OpponentLeft{},
                                        ServerError{"room is full"},
                                        KoCounts{1, 0},
                                        KoCounts{3, 2},
                                        MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7},
                                        MatchOver{MatchResult::Draw, MatchReason::Time, 0, 0}};
```

2. In `"protocol: the wire format is plain text"` add:

```cpp
    CHECK(encode(KoCounts{1, 0}) == "KO 1 0");
    CHECK(encode(MatchOver{MatchResult::Win, MatchReason::KOs, 12, 7}) == "OVER WIN KO 12 7");
    CHECK(encode(MatchOver{MatchResult::Lose, MatchReason::Time, 3, 9}) == "OVER LOSE TIME 3 9");
    CHECK(encode(MatchOver{MatchResult::Draw, MatchReason::Time, 4, 4}) == "OVER DRAW TIME 4 4");
```

3. Add a new test:

```cpp
TEST_CASE("protocol: malformed KO and OVER messages are rejected") {
    for (std::string_view text : {"KO", "KO 1", "KO 1 0 0", "KO 4 0", "KO 0 4", "KO -1 0", "KO x 0", "KO 1 0 ",
                                  "OVER", "OVER WIN KO 1", "OVER WIN KO 1 2 3", "OVER WON KO 1 2",
                                  "OVER WIN KOS 1 2", "OVER win KO 1 2", "OVER WIN KO -1 2", "OVER WIN KO 1 x",
                                  "OVER WIN TIME 1 2 "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `cmake --build build`
Expected: `use of undeclared identifier 'KoCounts'`.

- [ ] **Step 3: Implement**

`src/net/protocol.hpp`: after `kRoomCodeLength` add:

```cpp
inline constexpr int kKosToWin = 3;  // the first player to this many KOs wins the match
```

Change the message comment and add the new types before `using Message`:

```cpp
// Messages between a browser and the duel server. Attack and Snapshot are relayed to the opponent; Dead goes
// to the server, which answers with KoCounts and, when the match ends, MatchOver.
```

```cpp
enum class MatchResult { Win, Lose, Draw };
enum class MatchReason { KOs, Time };
struct KoCounts {  // from the receiver's point of view
    int mine = 0;
    int theirs = 0;
    friend bool operator==(const KoCounts&, const KoCounts&) = default;
};
struct MatchOver {  // the server's verdict, from the receiver's point of view
    MatchResult result = MatchResult::Draw;
    MatchReason reason = MatchReason::Time;
    int mySent = 0;
    int theirSent = 0;
    friend bool operator==(const MatchOver&, const MatchOver&) = default;
};

using Message = std::variant<CreateRoom, JoinRoom, RoomCreated, StartMatch, Attack, Snapshot, Dead, Rematch,
                             OpponentLeft, ServerError, KoCounts, MatchOver>;
```

`src/net/protocol.cpp`: add `#include <array>` with the other includes. In the anonymous namespace, after `kMaxInt`:

```cpp
constexpr std::array<std::string_view, 3> kResultTokens{"WIN", "LOSE", "DRAW"};  // index matches MatchResult
constexpr std::array<std::string_view, 2> kReasonTokens{"KO", "TIME"};          // index matches MatchReason

template <typename E, std::size_t N>
std::optional<E> parseToken(std::string_view text, const std::array<std::string_view, N>& tokens) {
    for (std::size_t i = 0; i < N; ++i)
        if (tokens[i] == text) return static_cast<E>(i);
    return std::nullopt;
}
```

In `encode`, replace the final `else` branch (the `ServerError` one) with:

```cpp
            } else if constexpr (std::is_same_v<T, ServerError>) {
                return "ERR " + m.reason;
            } else if constexpr (std::is_same_v<T, KoCounts>) {
                return "KO " + std::to_string(m.mine) + " " + std::to_string(m.theirs);
            } else {
                static_assert(std::is_same_v<T, MatchOver>);
                return "OVER " + std::string(kResultTokens[static_cast<int>(m.result)]) + " " +
                       std::string(kReasonTokens[static_cast<int>(m.reason)]) + " " + std::to_string(m.mySent) +
                       " " + std::to_string(m.theirSent);
            }
```

In `decode`, before the `} else if (args == 0) {` branch:

```cpp
    } else if (verb == "KO" && args == 2) {
        const auto mine = parseNumber(parts[1], 0, kKosToWin);
        const auto theirs = parseNumber(parts[2], 0, kKosToWin);
        if (mine && theirs) return KoCounts{*mine, *theirs};
    } else if (verb == "OVER" && args == 4) {
        const auto result = parseToken<MatchResult>(parts[1], kResultTokens);
        const auto reason = parseToken<MatchReason>(parts[2], kReasonTokens);
        const auto mySent = parseNumber(parts[3], 0, kMaxInt);
        const auto theirSent = parseNumber(parts[4], 0, kMaxInt);
        if (result && reason && mySent && theirSent) return MatchOver{*result, *reason, *mySent, *theirSent};
```

- [ ] **Step 4: Run all the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass. `DuelSession::onMessage` ignores the two new variants for now, because its `get_if` chain has no branch for them.

- [ ] **Step 5: Commit**

```bash
git add src/net/protocol.hpp src/net/protocol.cpp tests/protocol_test.cpp
git commit -m "feat(net): KO and OVER server verbs"
```

---

### Task 5: Server referee — KOs, lines sent and the match timer

**Files:**
- Modify: `server/hub.go`, `server/main.go`, `README.md` (hosting flags)
- Test: `server/hub_test.go`

**Interfaces:**
- Consumes: the wire formats from Task 4 (`KO a b`, `OVER WIN|LOSE|DRAW KO|TIME mine theirs`).
- Produces: `type stopper interface{ Stop() bool }`; `Hub` fields `afterFunc func(time.Duration, func()) stopper`, `matchLength`, `matchGrace time.Duration`; constant `countdown = 3 * time.Second`; flag `-match` (default `2m`).

- [ ] **Step 1: Write the failing tests**

In `server/hub_test.go`, add `"sync"` to the imports. Replace `newTestServer` with the fake clock plus two constructors:

```go
// fakeClock records match timers so a test decides when they fire.
type fakeClock struct {
	mu     sync.Mutex
	timers []*fakeTimer
}

type fakeTimer struct {
	clock   *fakeClock
	d       time.Duration
	f       func()
	stopped bool
}

func (c *fakeClock) afterFunc(d time.Duration, f func()) stopper {
	c.mu.Lock()
	defer c.mu.Unlock()
	t := &fakeTimer{clock: c, d: d, f: f}
	c.timers = append(c.timers, t)
	return t
}

// timer returns the i-th timer armed so far.
func (c *fakeClock) timer(i int) *fakeTimer {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.timers[i]
}

func (t *fakeTimer) Stop() bool {
	t.clock.mu.Lock()
	defer t.clock.mu.Unlock()
	wasRunning := !t.stopped
	t.stopped = true
	return wasRunning
}

func (t *fakeTimer) isStopped() bool {
	t.clock.mu.Lock()
	defer t.clock.mu.Unlock()
	return t.stopped
}

// fire runs the callback even after Stop, like a real timer that fired just before it was stopped.
func (t *fakeTimer) fire() { t.f() }

const testMatchLength = 20 * time.Second

func newTestHub(t *testing.T) (*httptest.Server, *fakeClock) {
	t.Helper()
	hub := NewHub()
	codes := 0
	hub.newCode = func() string { codes++; return fmt.Sprintf("AAA%c", 'A'+codes-1) }
	seed := uint32(100)
	hub.newSeed = func() uint32 { seed++; return seed }
	clock := &fakeClock{}
	hub.afterFunc = clock.afterFunc
	hub.matchLength = testMatchLength
	srv := httptest.NewServer(hub)
	t.Cleanup(srv.Close)
	return srv, clock
}

func newTestServer(t *testing.T) *httptest.Server {
	t.Helper()
	srv, _ := newTestHub(t)
	return srv
}
```

Replace `TestGameMessagesAreRelayed`:

```go
func TestGameMessagesAreRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("ATK 2")
	b.expect("ATK 2")
	b.send("SNAP 0 0 0 board")
	a.expect("SNAP 0 0 0 board")
}
```

In `TestServerOnlyVerbsAreNotRelayed`, extend the forged list:

```go
	for _, forged := range []string{"ROOM ZZZZ", "START 1 1", "LEFT", "ERR boom", "KO 3 0", "OVER WIN KO 0 0"} {
```

Replace `TestRematchNeedsBothPlayers` (a rematch now needs a finished match):

```go
func TestRematchNeedsBothPlayers(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("REMATCH")
	b.send("SNAP probe") // proves nothing was sent to a after a's REMATCH alone
	a.expect("SNAP probe")
	b.send("REMATCH")
	a.expect("START 102 3")
	b.expect("START 102 3")
}
```

Append the new tests:

```go
func TestAttacksAreCountedAndMalformedOnesDropped(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	for _, bad := range []string{"ATK", "ATK 0", "ATK 41", "ATK x", "ATK 2 2"} {
		a.send(bad)
	}
	a.send("ATK 2")
	b.expect("ATK 2")
	a.send("ATK 3")
	b.expect("ATK 3")
	b.send("ATK 1")
	a.expect("ATK 1")
	clock.timer(0).fire()
	a.expect("OVER WIN TIME 5 1")
	b.expect("OVER LOSE TIME 1 5")
}

func TestDeadIsAKOAndThreeKOsWin(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	for i := 1; i <= 3; i++ {
		b.send("DEAD")
		a.expect(fmt.Sprintf("KO %d 0", i))
		b.expect(fmt.Sprintf("KO 0 %d", i))
	}
	a.expect("OVER WIN KO 0 0")
	b.expect("OVER LOSE KO 0 0")
	if !clock.timer(0).isStopped() {
		t.Error("match timer still running after a KO win")
	}
}

func TestTimeUpGoesToMoreKOsBeforeLinesSent(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("ATK 5")
	b.expect("ATK 5")
	a.send("DEAD")
	a.expect("KO 0 1")
	b.expect("KO 1 0")
	clock.timer(0).fire()
	a.expect("OVER LOSE TIME 5 0")
	b.expect("OVER WIN TIME 0 5")
}

func TestTimeUpWithTiedKOsGoesToLinesSent(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("DEAD")
	a.expect("KO 0 1")
	b.expect("KO 1 0")
	b.send("DEAD")
	a.expect("KO 1 1")
	b.expect("KO 1 1")
	a.send("ATK 3")
	b.expect("ATK 3")
	b.send("ATK 1")
	a.expect("ATK 1")
	clock.timer(0).fire()
	a.expect("OVER WIN TIME 3 1")
	b.expect("OVER LOSE TIME 1 3")
}

func TestTimeUpWithEverythingTiedIsADraw(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("ATK 2")
	b.expect("ATK 2")
	b.send("ATK 2")
	a.expect("ATK 2")
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 2 2")
	b.expect("OVER DRAW TIME 2 2")
}

// Clients start their clocks when START arrives, a one-way latency after the server armed its timer.
// The grace window lets an ATK sent just before a client's 0:00 freeze still arrive in time to count.
func TestMatchTimerCoversCountdownMatchAndGrace(t *testing.T) {
	srv, clock := newTestHub(t)
	pair(t, srv)
	if got, want := clock.timer(0).d, countdown+testMatchLength+defaultMatchGrace; got != want {
		t.Errorf("timer armed for %v, want %v", got, want)
	}
}

func TestMessagesAfterOverAreIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("ATK 2")
	a.send("DEAD")
	clock.timer(0).fire() // firing again changes nothing
	a.send("SNAP probe")
	b.expect("SNAP probe")
}

func TestRematchBeforeOverIsIgnored(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("REMATCH")
	a.send("SNAP from a")
	b.expect("SNAP from a") // a's REMATCH has been handled
	b.send("REMATCH")
	b.send("SNAP from b")
	a.expect("SNAP from b") // a START would have reached a before this
}

func TestStaleTimerAfterRematchIsIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("REMATCH")
	b.send("REMATCH")
	a.expect("START 102 3")
	b.expect("START 102 3")
	clock.timer(0).fire() // the first match's timer, racing the rematch
	a.send("SNAP probe")
	b.expect("SNAP probe")
	clock.timer(1).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
}

func TestLeavingStopsTheMatchTimer(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
	if !clock.timer(0).isStopped() {
		t.Error("match timer still running after a player left")
	}
	clock.timer(0).fire() // the room is gone; this must do nothing
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `go -C server test -race ./...`
Expected: `undefined: stopper` / `hub.afterFunc undefined`.

- [ ] **Step 3: Implement `server/hub.go`**

Extend the constants:

```go
const (
	maxMessageBytes    = 4096
	writeTimeout       = 2 * time.Second
	codeLetters        = "ABCDEFGHJKLMNPQRSTUVWXYZ" // no I or O, which look like 1 and 0
	codeLength         = 4
	minStartLevel      = 1
	maxStartLevel      = 15
	maxAttack          = 40              // the client's ATK range is 1 to kBoardHeight
	kosToWin           = 3               // kKosToWin in src/net/protocol.hpp
	countdown          = 3 * time.Second // must match kCountdownMs in src/app/duel.hpp
	defaultMatchLength = 2 * time.Minute
	defaultMatchGrace  = 500 * time.Millisecond
)
```

Replace the `Hub` comment and struct, `room`, and `NewHub`:

```go
// Hub pairs browsers into two-player rooms, relays game messages between them and referees each match.
// It handles CREATE, JOIN, REMATCH, ATK and DEAD itself; other game messages go to the opponent unchanged,
// and the verbs only the server sends (ROOM, START, LEFT, ERR, KO, OVER) are dropped.
type Hub struct {
	mu          sync.Mutex
	rooms       map[string]*room
	newCode     func() string
	newSeed     func() uint32
	afterFunc   func(d time.Duration, f func()) stopper
	matchLength time.Duration
	matchGrace  time.Duration // covers clients starting their clocks a one-way latency after the server
}

// stopper is a match timer; *time.Timer satisfies it.
type stopper interface{ Stop() bool }

type room struct {
	code    string
	level   int
	players [2]*player
	rematch [2]bool
	// The current match, reset by every START.
	kos     [2]int
	sent    [2]int // lines sent: the total of each seat's accepted ATK n
	over    bool
	matchID uint64 // tells a stale timer from the current match's
	timer   stopper
}
```

```go
func NewHub() *Hub {
	return &Hub{
		rooms:       map[string]*room{},
		newCode:     randomCode,
		newSeed:     rand.Uint32,
		afterFunc:   func(d time.Duration, f func()) stopper { return time.AfterFunc(d, f) },
		matchLength: defaultMatchLength,
		matchGrace:  defaultMatchGrace,
	}
}
```

In `route`, replace the second `switch` with:

```go
	switch verb {
	case "CREATE", "JOIN":
		return []outgoing{{p, "ERR already in a room"}}
	case "REMATCH":
		return h.rematch(p)
	case "ATK":
		return p.room.attack(p, arg)
	case "DEAD":
		if text != "DEAD" {
			return nil
		}
		return p.room.knockOut(p)
	case "ROOM", "START", "LEFT", "ERR", "KO", "OVER":
		return nil // only the server sends these; never relay a forged one
	}
```

In `join`, replace `return r.start(h.newSeed())` with `return h.startMatch(r)`. In `rematch`, replace the start of the function so it reads:

```go
func (h *Hub) rematch(p *player) []outgoing {
	r := p.room
	if r.opponent(p) == nil || !r.over {
		return nil
	}
	r.rematch[r.seat(p)] = true
	if r.rematch[0] && r.rematch[1] {
		return h.startMatch(r)
	}
	return nil
}
```

In `leave`, stop the timer before deleting the room:

```go
	if r := p.room; r != nil {
		if r.timer != nil {
			r.timer.Stop()
		}
		delete(h.rooms, r.code)
```

Replace `func (r *room) start(seed uint32) []outgoing` with these:

```go
// startMatch resets the room's match state, arms the match timer and sends START to both players.
func (h *Hub) startMatch(r *room) []outgoing {
	if r.timer != nil {
		r.timer.Stop()
	}
	r.rematch = [2]bool{}
	r.kos, r.sent, r.over = [2]int{}, [2]int{}, false
	r.matchID++
	id := r.matchID
	r.timer = h.afterFunc(countdown+h.matchLength+h.matchGrace, func() { h.timeUp(r, id) })
	msg := fmt.Sprintf("START %d %d", h.newSeed(), r.level)
	return []outgoing{{r.players[0], msg}, {r.players[1], msg}}
}

// timeUp runs on the timer's goroutine and ends match id unless it is already over or replaced.
func (h *Hub) timeUp(r *room, id uint64) {
	h.mu.Lock()
	var out []outgoing
	if h.rooms[r.code] == r && r.matchID == id && !r.over {
		out = r.finish("TIME")
	}
	h.mu.Unlock()
	h.deliver(out)
}

func (r *room) playing() bool { return r.matchID > 0 && !r.over }

// attack counts a valid ATK toward the sender's lines sent and relays it in canonical form.
func (r *room) attack(p *player, arg string) []outgoing {
	n, err := strconv.Atoi(arg)
	if !r.playing() || err != nil || n < 1 || n > maxAttack {
		return nil
	}
	r.sent[r.seat(p)] += n
	return []outgoing{{r.opponent(p), fmt.Sprintf("ATK %d", n)}}
}

// knockOut scores a KO for p's opponent, tells both players, and ends the match at kosToWin.
func (r *room) knockOut(p *player) []outgoing {
	if !r.playing() {
		return nil
	}
	winner := 1 - r.seat(p)
	r.kos[winner]++
	out := make([]outgoing, 0, 4)
	for seat, q := range r.players {
		out = append(out, outgoing{q, fmt.Sprintf("KO %d %d", r.kos[seat], r.kos[1-seat])})
	}
	if r.kos[winner] >= kosToWin {
		out = append(out, r.finish("KO")...)
	}
	return out
}

// finish ends the match and sends each player the verdict from their own side.
func (r *room) finish(reason string) []outgoing {
	r.over = true
	if r.timer != nil {
		r.timer.Stop()
	}
	out := make([]outgoing, 0, 2)
	for seat, p := range r.players {
		msg := fmt.Sprintf("OVER %s %s %d %d", r.verdict(seat), reason, r.sent[seat], r.sent[1-seat])
		out = append(out, outgoing{p, msg})
	}
	return out
}

// verdict compares KOs, then lines sent, from seat's side.
func (r *room) verdict(seat int) string {
	mine, theirs := r.kos[seat], r.kos[1-seat]
	if mine == theirs {
		mine, theirs = r.sent[seat], r.sent[1-seat]
	}
	switch {
	case mine > theirs:
		return "WIN"
	case mine < theirs:
		return "LOSE"
	}
	return "DRAW"
}
```

- [ ] **Step 4: Add the `-match` flag in `server/main.go`**

After the `dir` flag:

```go
	match := flag.Duration("match", defaultMatchLength, "match length, e.g. 20s for quick manual tests")
```

Replace `mux.Handle("/ws", NewHub())` with:

```go
	hub := NewHub()
	hub.matchLength = *match
	mux.Handle("/ws", hub)
```

- [ ] **Step 5: Document the flag in `README.md`**

In "Hosting LAN duels", replace the flags sentence with:

```markdown
macOS firewall asks. Flags: `-addr` (default `0.0.0.0:8080`), `-dir` (default
`../build-web`) and `-match` (match length, default `2m`; `-match 20s` makes timeouts quick
to test).
```

- [ ] **Step 6: Run the Go tests**

Run: `go -C server test -race ./... && go -C server vet ./...`
Expected: `ok`, no vet findings, no race reports.

- [ ] **Step 7: Commit**

```bash
git add server/hub.go server/main.go server/hub_test.go README.md
git commit -m "feat(server): referee KOs, lines sent and the match timer"
```

---

### Task 6: `DuelSession` — KOs, clock, freeze and result

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/duel_test.cpp`

**Interfaces:**
- Consumes: `KoCounts`, `MatchOver`, `MatchResult`, `MatchReason`, `kKosToWin` (Task 4); `Game::resetBoard()` (Task 3).
- Produces (in `app/duel.hpp`):

```cpp
inline constexpr int kMatchMs = 120000;
inline constexpr int kKoFlashMs = 1000;
enum class DuelOutcome { None, Won, Lost, Draw, OpponentLeft };
using DuelReason = MatchReason;
// DuelSession members:
DuelReason reason() const; int kosMine() const; int kosTheirs() const;
int sentMine() const; int sentTheirs() const;  // live tally; the server's totals once OVER arrives
int matchClockMs() const; int koFlashMs() const; bool frozen() const;
// Free function:
std::string resultReasonText(const DuelSession& duel);  // "" outside Result
```

- [ ] **Step 1: Rewrite the tests that relied on a relayed `DEAD`**

In `tests/duel_test.cpp`, make these changes:

1. Replace `"duel: topping out sends a final snapshot and DEAD, and loses"` with:

```cpp
TEST_CASE("duel: topping out sends a final snapshot and DEAD, resets the board and keeps playing") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    REQUIRE(wire.sent.size() == 2);
    CHECK(wire.sent[0].starts_with("SNAP "));
    CHECK(wire.sent[1] == "DEAD");
    CHECK(duel.phase() == DuelPhase::Playing);
    CHECK(duel.game()->state() == GameState::Playing);
    CHECK(countFilled(duel.game()->board()) == 0);
    CHECK(duel.game()->pendingGarbage() == 0);
    CHECK(duel.koFlashMs() == kKoFlashMs);
}
```

2. Replace `"duel: the opponent topping out wins"` with:

```cpp
TEST_CASE("duel: a relayed DEAD is ignored now that the server referees") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("DEAD");
    CHECK(duel.phase() == DuelPhase::Playing);
}
```

3. In `"duel: rematch is sent once and START begins a fresh match"`, replace `duel.onMessage("DEAD");` with `duel.onMessage("KO 3 0");` followed by `duel.onMessage("OVER WIN KO 3 0");`. After the existing final check, add:

```cpp
    CHECK(duel.kosMine() == 0);
    CHECK(duel.sentMine() == 0);
    CHECK(duel.sentTheirs() == 0);
    CHECK(duel.matchClockMs() == kMatchMs);
```

4. In `"duel: START on the result screen is ignored without a rematch request"`, replace `duel.onMessage("DEAD");` with `duel.onMessage("OVER WIN KO 0 0");`.

5. Replace `"duel: the opponent topping out during our countdown wins"` with:

```cpp
TEST_CASE("duel: OVER during our countdown ends the match") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("START 7 1");
    REQUIRE(duel.phase() == DuelPhase::Countdown);
    duel.onMessage("OVER WIN KO 0 0");
    CHECK(duel.phase() == DuelPhase::Result);
    CHECK(duel.outcome() == DuelOutcome::Won);
}
```

6. In `"duel: line clears are sent as attacks"`, add at the end: `CHECK(duel.sentMine() == 4);`

- [ ] **Step 2: Write the new tests**

Append to `tests/duel_test.cpp`:

```cpp
TEST_CASE("duel: the KO flash freezes the game for 1000 ms while the clock keeps running") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    CHECK_FALSE(duel.frozen());
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    CHECK(duel.frozen());
    duel.update(kKoFlashMs - 1);
    CHECK(duel.frozen());
    CHECK(duel.matchClockMs() == kMatchMs - (kKoFlashMs - 1));
    duel.update(1);
    CHECK_FALSE(duel.frozen());
    CHECK(duel.koFlashMs() == 0);
}

TEST_CASE("duel: garbage arriving during the KO flash is queued on the fresh board") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.game()->receiveGarbage(kBoardHeight - 1);
    duel.game()->apply(Action::HardDrop);
    duel.update(0);
    REQUIRE(duel.frozen());
    duel.onMessage("ATK 3");
    CHECK(duel.game()->pendingGarbage() == 3);
    CHECK(duel.sentTheirs() == 3);
}

TEST_CASE("duel: KO updates both counts from the server") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("KO 1 2");
    CHECK(duel.kosMine() == 1);
    CHECK(duel.kosTheirs() == 2);
    CHECK(duel.phase() == DuelPhase::Playing);
}

TEST_CASE("duel: each OVER reaches the result screen with its outcome, reason and the server's totals") {
    struct Case {
        const char* message;
        DuelOutcome outcome;
        DuelReason reason;
        int mine, theirs;
    };
    for (const Case& c : {Case{"OVER WIN KO 3 1", DuelOutcome::Won, DuelReason::KOs, 3, 1},
                          Case{"OVER LOSE TIME 2 5", DuelOutcome::Lost, DuelReason::Time, 2, 5},
                          Case{"OVER DRAW TIME 4 4", DuelOutcome::Draw, DuelReason::Time, 4, 4}}) {
        CAPTURE(c.message);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        duel.onMessage("ATK 9");  // the live tally is replaced by the server's totals
        duel.onMessage(c.message);
        CHECK(duel.phase() == DuelPhase::Result);
        CHECK(duel.outcome() == c.outcome);
        CHECK(duel.reason() == c.reason);
        CHECK(duel.sentMine() == c.mine);
        CHECK(duel.sentTheirs() == c.theirs);
    }
}

TEST_CASE("duel: KO and OVER are ignored outside the countdown and play") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("OVER WIN KO 0 0");
    duel.onMessage("KO 1 0");
    CHECK(duel.phase() == DuelPhase::Waiting);
    CHECK(duel.kosMine() == 0);
    duel.onMessage("START 7 1");
    duel.update(kCountdownMs);
    duel.onMessage("OVER WIN KO 0 0");
    REQUIRE(duel.phase() == DuelPhase::Result);
    duel.onMessage("KO 3 3");
    duel.onMessage("OVER LOSE TIME 1 1");
    CHECK(duel.kosMine() == 0);
    CHECK(duel.outcome() == DuelOutcome::Won);
}

TEST_CASE("duel: the match clock runs only while playing, stops at 0 and freezes the game there") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("START 7 1");
    duel.update(kCountdownMs - 1);
    CHECK(duel.matchClockMs() == kMatchMs);
    duel.update(1);
    REQUIRE(duel.phase() == DuelPhase::Playing);
    duel.update(1000);
    CHECK(duel.matchClockMs() == kMatchMs - 1000);
    CHECK_FALSE(duel.frozen());
    duel.update(kMatchMs);
    CHECK(duel.matchClockMs() == 0);
    CHECK(duel.frozen());
    CHECK(duel.phase() == DuelPhase::Playing);  // waits for the server's OVER
    duel.update(5);
    CHECK(duel.matchClockMs() == 0);
}

TEST_CASE("duel: incoming attacks add to the opponent's sent tally") {
    Wire wire;
    DuelSession duel(wire.sender());
    startMatch(duel, wire);
    duel.onMessage("ATK 2");
    duel.onMessage("ATK 1");
    CHECK(duel.sentTheirs() == 3);
    CHECK(duel.sentMine() == 0);
}

TEST_CASE("duel: the result line explains how the match ended") {
    struct Case {
        std::vector<std::string> messages;
        std::string line;
    };
    const std::vector<Case> cases{
        {{"KO 3 0", "OVER WIN KO 0 0"}, "3 KOs"},
        {{"KO 0 3", "OVER LOSE KO 0 0"}, "3 KOs"},
        {{"KO 2 1", "OVER WIN TIME 3 9"}, "Time up \xC2\xB7 KO 2 : 1"},
        {{"KO 1 1", "OVER WIN TIME 12 7"}, "Time up \xC2\xB7 KO 1 : 1 \xC2\xB7 12 : 7 sent"},
        {{"OVER DRAW TIME 9 9"}, "Time up \xC2\xB7 KO 0 : 0 \xC2\xB7 9 : 9 sent"},
        {{"LEFT"}, "Opponent left the match"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.line);
        Wire wire;
        DuelSession duel(wire.sender());
        startMatch(duel, wire);
        CHECK(resultReasonText(duel).empty());
        for (const std::string& message : c.messages) duel.onMessage(message);
        CHECK(resultReasonText(duel) == c.line);
    }
}
```

- [ ] **Step 3: Run them and watch them fail to compile**

Run: `cmake --build build`
Expected: errors such as `no member named 'koFlashMs'`, `use of undeclared identifier 'kMatchMs'` and `'resultReasonText' was not declared`.

- [ ] **Step 4: Implement `src/app/duel.hpp`**

Replace the constants and enums block:

```cpp
inline constexpr int kCountdownMs = 3000;  // must match countdown in server/hub.go
inline constexpr int kSnapshotIntervalMs = 100;
inline constexpr int kMatchMs = 120000;    // display clock; the server's -match default is the same 2 minutes
inline constexpr int kKoFlashMs = 1000;

enum class DuelPhase { Idle, Waiting, Countdown, Playing, Result };
enum class DuelOutcome { None, Won, Lost, Draw, OpponentLeft };
using DuelReason = MatchReason;  // why a Won, Lost or Draw match ended, from the server's OVER
```

Change the class comment's last sentence to: `// reports attacks, snapshots and top-outs through `send`. The server referees KOs and the result.`

Add `#include <string>` if it isn't already there (it is). Add the accessors after `opponentGone()`:

```cpp
    DuelReason reason() const { return reason_; }
    int kosMine() const { return kosMine_; }  // KOs this player has scored, from the server's KO messages
    int kosTheirs() const { return kosTheirs_; }
    // Lines sent this match: a live tally while playing, then the server's totals once OVER arrives.
    int sentMine() const { return sentMine_; }
    int sentTheirs() const { return sentTheirs_; }
    int matchClockMs() const { return matchClockMs_; }  // display only; the server decides time-up
    int koFlashMs() const { return koFlashMs_; }        // > 0 while "KO!" shows after our top-out
    // While true the local game must not tick or take input: during the KO flash, or once the clock hits 0.
    bool frozen() const;
```

Add these private helpers next to `startMatch`/`finish`/`reset`:

```cpp
    bool inMatch() const { return phase_ == DuelPhase::Countdown || phase_ == DuelPhase::Playing; }
    void resetMatchStats();
```

Add these private members after `opponentGone_`:

```cpp
    DuelReason reason_ = DuelReason::KOs;
    int kosMine_ = 0;
    int kosTheirs_ = 0;
    int sentMine_ = 0;
    int sentTheirs_ = 0;
    int matchClockMs_ = 0;
    int koFlashMs_ = 0;
```

After the class, before the closing namespace:

```cpp
// The line under the result title, e.g. "Time up · KO 1 : 1 · 12 : 7 sent". Empty outside Result.
std::string resultReasonText(const DuelSession& duel);
```

- [ ] **Step 5: Implement `src/app/duel.cpp`**

Add `#include <algorithm>` and `#include <string_view>` above `#include <variant>`. In the file's top namespace, after `makeDuelGame`, add:

```cpp
namespace {

DuelOutcome outcomeOf(MatchResult result) {
    switch (result) {
    case MatchResult::Win:
        return DuelOutcome::Won;
    case MatchResult::Lose:
        return DuelOutcome::Lost;
    case MatchResult::Draw:
        return DuelOutcome::Draw;
    }
    return DuelOutcome::None;
}

constexpr std::string_view kSeparator = " \xC2\xB7 ";  // a spaced middle dot, UTF-8

std::string versus(int mine, int theirs) { return std::to_string(mine) + " : " + std::to_string(theirs); }

}  // namespace

bool DuelSession::frozen() const { return phase_ == DuelPhase::Playing && (koFlashMs_ > 0 || matchClockMs_ == 0); }

std::string resultReasonText(const DuelSession& duel) {
    if (duel.phase() != DuelPhase::Result) return {};
    if (duel.outcome() == DuelOutcome::OpponentLeft) return "Opponent left the match";
    if (duel.reason() == DuelReason::KOs) return std::to_string(kKosToWin) + " KOs";
    std::string text = "Time up" + std::string(kSeparator) + "KO " + versus(duel.kosMine(), duel.kosTheirs());
    if (duel.kosMine() == duel.kosTheirs())
        text += std::string(kSeparator) + versus(duel.sentMine(), duel.sentTheirs()) + " sent";
    return text;
}
```

In `onMessage`, replace the `Attack` branch and the `Dead` branch with:

```cpp
    } else if (const auto* attack = std::get_if<Attack>(&*message)) {
        // The two countdowns drift, so the opponent may already be playing while ours runs.
        if (inMatch()) {
            game_->receiveGarbage(attack->lines);
            sentTheirs_ += attack->lines;
        }
    } else if (const auto* snapshot = std::get_if<Snapshot>(&*message)) {
        opponent_ = *snapshot;
    } else if (const auto* kos = std::get_if<KoCounts>(&*message)) {
        if (inMatch()) {
            kosMine_ = kos->mine;
            kosTheirs_ = kos->theirs;
        }
    } else if (const auto* over = std::get_if<MatchOver>(&*message)) {
        if (inMatch()) {
            reason_ = over->reason;
            sentMine_ = over->mySent;
            sentTheirs_ = over->theirSent;
            finish(outcomeOf(over->result));
        }
```

(The `Snapshot` branch is unchanged; it's repeated above so you can see where the new branches go. Delete the `Dead` branch entirely, so an incoming `DEAD` falls through and is ignored.)

Replace the Playing part of `update` (everything after the Countdown block's `return; }`):

```cpp
    if (phase_ != DuelPhase::Playing) return;
    matchClockMs_ = std::max(0, matchClockMs_ - ms);
    koFlashMs_ = std::max(0, koFlashMs_ - ms);
    if (const int lines = game_->takeOutgoingAttack(); lines > 0) {
        send(Attack{lines});
        sentMine_ += lines;
    }
    const bool toppedOut = game_->state() == GameState::GameOver;
    sinceSnapshotMs_ += ms;
    if (sinceSnapshotMs_ >= kSnapshotIntervalMs || toppedOut) {
        sinceSnapshotMs_ = 0;
        const Snapshot snapshot = snapshotOf(*game_);
        if (snapshot != lastSent_) {
            send(snapshot);
            lastSent_ = snapshot;
        }
    }
    if (toppedOut) {  // a KO: the server counts it, we start over on an empty board
        send(Dead{});
        game_->resetBoard();
        koFlashMs_ = kKoFlashMs;
    }
}
```

Add `resetMatchStats` and call it from `startMatch` (after `rematchRequested_ = false;`) and from `reset` (at the end):

```cpp
void DuelSession::resetMatchStats() {
    reason_ = DuelReason::KOs;
    kosMine_ = 0;
    kosTheirs_ = 0;
    sentMine_ = 0;
    sentTheirs_ = 0;
    matchClockMs_ = kMatchMs;
    koFlashMs_ = 0;
}
```

- [ ] **Step 6: Run all the tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/app/duel.hpp src/app/duel.cpp tests/duel_test.cpp
git commit -m "feat(app): KO-and-reset, match clock, freeze and server verdicts in DuelSession"
```

---

### Task 7: Layout A duel screen, freeze wiring and docs

**Files:**
- Modify: `src/app/app.cpp` (`App::activeGame`)
- Modify: `src/app/render.cpp` (bomb cell, duel section)
- Modify: `README.md` ("Playing a duel"), `docs/superpowers/specs/2026-10-13-lan-duel-design.md` (pointer)

**Interfaces:**
- Consumes: `DuelSession::frozen()`, `koFlashMs()`, `kosMine()/kosTheirs()`, `sentMine()/sentTheirs()`, `matchClockMs()`, `outcome()`, `resultReasonText()` (Task 6); `kKosToWin` (Task 4); `PieceType::Bomb` (Task 1).
- Produces: nothing used by other tasks. Rendering is checked by hand.

- [ ] **Step 1: Gate input and ticks on `frozen()`**

`src/app/app.cpp`, in `App::activeGame`, replace the duel line:

```cpp
    if (screen == Screen::Duel && duel.phase() == DuelPhase::Playing && !duel.frozen()) return duel.game();
```

`step()` already releases held keys, drops pending actions and skips `game->tick` whenever `activeGame()` is null. It still calls `duel.update(ms)`, so the clock and the KO flash keep running.

- [ ] **Step 2: Draw the bomb cell**

`src/app/render.cpp`, at the end of `drawCell` (after the four bevel rectangles):

```cpp
    if (type == PieceType::Bomb) {
        const int a = static_cast<int>(alpha * 255.0f);
        dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.55f), size * 0.28f, IM_COL32(25, 25, 32, a));
        dl->AddCircleFilled(ImVec2(p.x + size * 0.74f, p.y + size * 0.24f), std::max(1.0f, size * 0.09f),
                            IM_COL32(255, 140, 0, a));  // fuse
    }
```

- [ ] **Step 3: Replace the duel drawing code**

In `src/app/render.cpp`, delete everything from `constexpr float kDuelLeftColumns = 6.0f;` through the end of `drawOpponent(...)`, and replace it with:

```cpp
// Layout A, left to right, in cells: hold 5 | your well 10 | meter | next 5 | center 7 | meter | their well 10 |
// stats 5, plus gutters. The name bars sit above the 22 drawn rows.
constexpr float kDuelColumns = 46.0f;
constexpr float kNameBarRows = 1.5f;
constexpr float kDuelRows = kDrawnRows + kNameBarRows;
constexpr float kMyWellCol = 5.5f;  // drawSidePanels puts HOLD 5.5 cells left and NEXT 11 cells right of it
constexpr float kMyMeterCol = 15.75f;
constexpr float kCenterCol = 22.0f;
constexpr float kCenterColumns = 7.0f;
constexpr float kTheirMeterCol = 29.9f;
constexpr float kTheirWellCol = 30.5f;
constexpr float kStatsCol = 41.0f;
constexpr int kTimeWarningMs = 10000;  // the clock turns red under 0:10

struct DuelLayout {
    Layout mine;    // the local well
    Layout theirs;  // the opponent's well, same cell size
    float left;     // left edge of the whole view
    float top;      // top of the name bars
};

DuelLayout computeDuelLayout() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float cell =
        std::floor(std::min((vp->Size.y - 2 * kMargin) / kDuelRows, (vp->Size.x - 2 * kMargin) / kDuelColumns));
    const float left = vp->Pos.x + (vp->Size.x - kDuelColumns * cell) * 0.5f;
    const float top = vp->Pos.y + (vp->Size.y - kDuelRows * cell) * 0.5f;
    const float rowsTop = top + kNameBarRows * cell;
    return {{cell, ImVec2(left + kMyWellCol * cell, rowsTop)},
            {cell, ImVec2(left + kTheirWellCol * cell, rowsTop)},
            left,
            top};
}

float wellBottom(const Layout& l) { return cellTopLeft(l, 0, 0).y + l.cell; }
float wellTop(const Layout& l) { return cellTopLeft(l, 0, kVisibleHeight - 1).y; }

// Red bar showing garbage waiting to rise, one cell tall per line, with its left edge at `x`.
void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending) {
    if (pending <= 0) return;
    const float height = static_cast<float>(std::min(pending, kVisibleHeight)) * cell;
    dl->AddRectFilled(ImVec2(x, bottom - height), ImVec2(x + 0.35f * cell, bottom), IM_COL32(230, 40, 40, 255));
}

// ImGui has no radial gradient, so stacked translucent discs fake one over a dark navy fill.
void drawDuelBackground() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 10, 28, 255));
    constexpr int kRings = 16;
    const float radius = std::max(vp->Size.x, vp->Size.y) * 0.6f;
    for (int i = 0; i < kRings; ++i)
        bg->AddCircleFilled(vp->GetCenter(), radius * (1.0f - static_cast<float>(i) / kRings),
                            IM_COL32(30, 50, 110, 14));
}

void drawNameBar(ImDrawList* dl, const Fonts& fonts, const Layout& well, float top, const char* name, ImU32 from,
                 ImU32 to, int kos) {
    const float cell = well.cell;
    const ImVec2 p0(well.origin.x, top);
    const ImVec2 p1(well.origin.x + kBoardWidth * cell, top + 1.1f * cell);
    dl->AddRectFilledMultiColor(p0, p1, from, to, to, from);
    const float textSize = cell * 0.7f;
    dl->AddText(fonts.hud, textSize, ImVec2(p0.x + 0.3f * cell, p0.y + (p1.y - p0.y - textSize) * 0.5f),
                IM_COL32(255, 255, 255, 255), name);
    const float radius = cell * 0.25f;
    for (int i = 0; i < kKosToWin; ++i) {
        const ImVec2 center(p1.x - cell * (0.5f + 0.8f * static_cast<float>(kKosToWin - 1 - i)), (p0.y + p1.y) * 0.5f);
        if (i < kos)
            dl->AddCircleFilled(center, radius, IM_COL32(255, 220, 60, 255));
        else
            dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 200), 0, 1.5f);
    }
}

void drawCentered(ImDrawList* dl, const Fonts& fonts, float size, float centerX, float y, const char* text,
                  ImU32 col) {
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(fonts.hud, size, ImVec2(centerX - extent.x * 0.5f, y), col, text);
}

// TIME, VS, KO and SENT, with your number on the left.
void drawScoreboard(ImDrawList* dl, const Fonts& fonts, const DuelLayout& layout, const DuelSession& duel) {
    const float cell = layout.mine.cell;
    const float cx = layout.left + (kCenterCol + kCenterColumns * 0.5f) * cell;
    const ImU32 label = IM_COL32(150, 160, 190, 255);
    const ImU32 value = IM_COL32(235, 235, 245, 255);
    char text[32];
    float y = wellTop(layout.mine) + cell;

    const int seconds = (duel.matchClockMs() + 999) / 1000;
    std::snprintf(text, sizeof text, "%d:%02d", seconds / 60, seconds % 60);
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "TIME", label);
    drawCentered(dl, fonts, cell * 1.1f, cx, y + cell * 0.8f, text,
                 duel.matchClockMs() < kTimeWarningMs ? IM_COL32(255, 70, 70, 255) : value);
    y += cell * 4.0f;
    drawCentered(dl, fonts, cell * 1.4f, cx, y, "VS", IM_COL32(255, 200, 60, 255));
    y += cell * 3.5f;
    std::snprintf(text, sizeof text, "%d : %d", duel.kosMine(), duel.kosTheirs());
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "KO", label);
    drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, text, value);
    y += cell * 3.5f;
    std::snprintf(text, sizeof text, "%d : %d", duel.sentMine(), duel.sentTheirs());
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "SENT", label);
    drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, text, value);
}

// The opponent's well at full size, then SCORE and LINES; the snapshot carries no hold or next.
void drawOpponent(ImDrawList* dl, const DuelLayout& layout, const Snapshot& snapshot, const Fonts& fonts) {
    const Layout& l = layout.theirs;
    drawWell(dl, l);
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = snapshot.cells[y * kBoardWidth + x]) drawCell(dl, cellTopLeft(l, x, y), l.cell, *c, 1.0f);

    const float statsX = layout.left + kStatsCol * l.cell;
    const float textSize = l.cell * 0.6f;
    float y = wellTop(l);
    const std::array<std::pair<const char*, int>, 2> stats{{{"SCORE", snapshot.score}, {"LINES", snapshot.lines}}};
    char value[32];
    for (const auto& [label, number] : stats) {
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y), label);
        std::snprintf(value, sizeof value, "%d", number);
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y + textSize * 1.4f), value);
        y += textSize * 3.5f;
    }
}

// A large yellow "KO!" with an orange glow, centered on the well.
void drawKoFlash(ImDrawList* dl, const Fonts& fonts, const Layout& well) {
    const char* text = "KO!";
    const float size = well.cell * 3.0f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pos(well.origin.x + (kBoardWidth * well.cell - extent.x) * 0.5f,
                     (wellTop(well) + wellBottom(well) - extent.y) * 0.5f);
    const float glow = std::max(1.0f, std::floor(well.cell * 0.15f));
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx != 0 || dy != 0)
                dl->AddText(fonts.hud, size, ImVec2(pos.x + dx * glow, pos.y + dy * glow), IM_COL32(255, 120, 0, 200),
                            text);
    dl->AddText(fonts.hud, size, pos, IM_COL32(255, 230, 40, 255), text);
}
```

Replace `drawDuelBoards` with:

```cpp
void drawDuelBoards(const DuelSession& duel, const Fonts& fonts) {
    const Game& game = *duel.game();
    const DuelLayout layout = computeDuelLayout();
    const float cell = layout.mine.cell;
    drawDuelBackground();
    ImDrawList* dl = beginBoardWindow();
    drawNameBar(dl, fonts, layout.mine, layout.top, "YOU", IM_COL32(40, 110, 255, 255), IM_COL32(150, 60, 230, 255),
                duel.kosMine());
    drawPlayfield(dl, layout.mine, game, fonts);
    drawGarbageMeter(dl, layout.left + kMyMeterCol * cell, wellBottom(layout.mine), cell, game.pendingGarbage());
    drawScoreboard(dl, fonts, layout, duel);
    drawGarbageMeter(dl, layout.left + kTheirMeterCol * cell, wellBottom(layout.theirs), cell,
                     duel.opponent().pending);
    drawNameBar(dl, fonts, layout.theirs, layout.top, "FOE", IM_COL32(230, 40, 40, 255), IM_COL32(255, 150, 30, 255),
                duel.kosTheirs());
    drawOpponent(dl, layout, duel.opponent(), fonts);
    if (duel.koFlashMs() > 0) drawKoFlash(dl, fonts, layout.mine);
    ImGui::End();
}
```

Replace `drawDuelResult` with:

```cpp
const char* resultTitle(DuelOutcome outcome) {
    switch (outcome) {
    case DuelOutcome::Lost:
        return "YOU LOSE";
    case DuelOutcome::Draw:
        return "DRAW";
    default:
        return "YOU WIN";
    }
}

UiCommand drawDuelResult(const DuelSession& duel, const Fonts& fonts) {
    UiCommand command = UiCommand::None;
    beginMenu("##result");
    menuTitle(fonts, resultTitle(duel.outcome()));
    ImGui::TextUnformatted(resultReasonText(duel).c_str());
    ImGui::Spacing();
    if (duel.opponentGone()) {
        if (duel.outcome() != DuelOutcome::OpponentLeft) ImGui::TextUnformatted("Opponent left.");
    } else if (duel.rematchRequested()) {
        ImGui::TextUnformatted("Waiting for opponent...");
    } else if (menuButton("Rematch")) {
        command = UiCommand::Rematch;
    }
    if (menuButton("Leave")) command = UiCommand::LeaveDuel;
    ImGui::End();
    return command;
}
```

`drawCountdown` stays as it is: it is already centered on the whole viewport.

- [ ] **Step 4: Build desktop and web and run every test**

Run: `cmake --build build && ctest --test-dir build --output-on-failure && go -C server test -race ./...`
Expected: all pass, no warnings.

Run: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`
Expected: `build-web/index.html`, `index.js` and `index.wasm` are rebuilt without warnings.

- [ ] **Step 5: Check solo play by hand**

Run `./build/tetris`. Play a few lines, pause and resume, and top out. Expected: it looks and behaves exactly as before (no bombs, same layout).

- [ ] **Step 6: Check the duel screen by hand**

Run `go -C server run . -match 20s` and open `http://localhost:8080` in two browser tabs. Create a room in one and join from the other. Check each item and note anything off:
- the layout matches the spec: HOLD, your well with the blue-purple YOU bar and pips, NEXT/SCORE/LEVEL, your meter, then the center TIME / VS / KO / SENT, their meter, their equal-size well with the red-orange FOE bar, and their SCORE/LINES
- the countdown is centered, and the clock starts at 2:00 then counts down (from 0:20 effectively, since the server cuts at 20 s; the client clock is display-only)
- bomb hit: send garbage, then land a piece on the bomb column top. That row flashes white and clears, and SENT goes up on the other side
- a chain of 3 or more bombs clears together
- a bomb plus a normal clear together
- KO and reset: the board empties, "KO!" shows for about 1 s, keys do nothing during it, the opponent's KO pip and the center `KO` update, and garbage sent during the flash appears on the meter
- 3-KO win: YOU WIN / "3 KOs" on one tab, YOU LOSE / "3 KOs" on the other
- timeout decided by KOs, timeout decided by lines sent, and a timeout draw. The reason lines match the spec. The middle dot `·` renders (if the menu font shows `?`, report it before changing anything)
- the clock turns red under 0:10. At 0:00 the game freezes until the result appears, and a clear made just before 0:00 still counts in the final SENT
- rematch after each kind of ending
- closing one tab mid-match: the other shows YOU WIN / "Opponent left the match", with Rematch hidden
- phone-landscape width (use the browser's device toolbar at about 844×390): everything fits at a smaller scale, with no clipping

- [ ] **Step 7: Update the docs**

`README.md`, in "Playing a duel", replace items 3 and 4 with:

```markdown
3. Clearing 2, 3 or 4 lines sends 1, 2 or 4 garbage lines to the opponent. Clearing
   lines cancels incoming garbage first. Each garbage batch carries a column of bombs:
   land a piece directly on the top bomb to blow up the whole stack below it. Exploded
   rows count as cleared lines.
4. Topping out is a KO: your board resets and your opponent scores a KO. The first to
   3 KOs wins. When the 2:00 clock runs out, more KOs wins, then more lines sent; a full
   tie is a draw. Either side can ask for a rematch.
```

`docs/superpowers/specs/2026-10-13-lan-duel-design.md`, insert after the title line:

```markdown

> **Partly superseded:** garbage gaps, "first top-out loses" and relaying `DEAD` are replaced by bomb garbage, KO matches and the server referee in `2026-10-13-bomb-battle-design.md`.
```

- [ ] **Step 8: Commit**

```bash
git add src/app/app.cpp src/app/render.cpp README.md docs/superpowers/specs/2026-10-13-lan-duel-design.md
git commit -m "feat(app): layout A duel screen with scoreboard, bomb cells and KO flash"
```
