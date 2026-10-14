# Opponent Hold and Next Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the opponent's hold piece (dimmed while locked) and five-piece next queue in the column right of their well. Spec: `docs/superpowers/specs/2026-10-14-opponent-hold-next-design.md`.

**Architecture:** `Snapshot` gains `hold`, `holdLocked` and `next`. `snapshotOf` fills them, and `SNAP` carries two new fields before the cells. `DuelSession` already resends a snapshot whenever one differs from the last, so it is untouched. `drawOpponent` draws HOLD and NEXT above SCORE and LINES.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build.

## Global Constraints

- The server is untouched: it relays `SNAP` verbatim.
- The duel layout stays 46 columns wide. `kDuelColumns` and the column constants don't change.
- New `Snapshot` fields go after `cells` so the positional initializers `Snapshot{score, lines, pending, {}}` (in `protocol.cpp`, `protocol_test.cpp` and `duel_test.cpp`) keep their meaning.
- The decoder is strict: exactly 6 arguments. The old 4-argument form is rejected.
- Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`.
- Conventional commits scoped like the existing history (`feat(net): …`, `feat(app): …`). Work on `develop`.

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/net/protocol.hpp` | `kSnapshotNext`, `Snapshot::hold`, `holdLocked`, `next` |
| `src/net/protocol.cpp` | `snapshotOf`, `SNAP` encode and decode |
| `src/app/render.cpp` | `drawOpponent` draws HOLD and NEXT |
| `tests/protocol_test.cpp` | round trip, wire format, malformed cases, `snapshotOf` |
| `tests/duel_test.cpp` | a hold-only change sends a snapshot |
| `docs/superpowers/specs/2026-10-13-lan-duel-design.md` | `SNAP` row |
| `docs/superpowers/specs/2026-10-13-bomb-battle-design.md` | non-goal points to the new spec |

---

### Task 1: Hold and next in the snapshot

**Files:** `src/net/protocol.hpp`, `src/net/protocol.cpp`, `tests/protocol_test.cpp`, `tests/duel_test.cpp`

**Interfaces:**
- Produces: `inline constexpr int kSnapshotNext = 5;`, `Cell Snapshot::hold`, `bool Snapshot::holdLocked`, `std::array<Cell, kSnapshotNext> Snapshot::next`.
- Wire: `SNAP <score> <lines> <pending> <hold> <next> <cells>`.

- [ ] **Step 1: Write the failing tests** (`protocol:` / `duel:` prefixes like the rest of each file)
  - Round trip: give the existing `snapshot` in the round-trip list `hold = PieceType::T`, `holdLocked = true` and `next = {I, O, S, Z, L}`. Add a second snapshot with `hold = PieceType::J`, unlocked.
  - Wire format: `encode(Snapshot{})` is `"SNAP 0 0 0 . ..... " + std::string(kSnapshotCells, '.')`. The bomb case becomes `"SNAP 0 0 0 . ..... B" + …`. A snapshot with a locked T hold and next `IOSZL` encodes as `"SNAP 0 0 0 t IOSZL " + …`, and with the T unlocked as `T`.
  - Malformed (each built from a valid 200-dot cell string so only the named field is wrong):
    - the old form `"SNAP 0 0 0 " + cells`;
    - hold `G`, `B`, `g`, `b`, `x`, `TT`, empty (two spaces);
    - next `IOSZ`, `IOSZLJ`, `IOSZG`, `IOSZB`, `iosz l`.
    - Update `"SNAP 0 0 0 ..."` and the `badCell` case to the 6-argument form so they still test what they meant (the short cell string, the bad cell).
  - `snapshotOf`: replace `CHECK(snapshotOf(game) == Snapshot{})` with a check that `cells` is all empty, `hold` is empty, `holdLocked` is false and `next` matches `game.next(kSnapshotNext)`. Then `game.apply(Action::Hold)`: `hold == PieceType::T` and `holdLocked`. Then `game.resetBoard()`: `hold` is empty and `holdLocked` is false.
  - Duel (`tests/duel_test.cpp`): after `startMatch` and one interval (one `SNAP` sent), `duel.game()->apply(Action::Hold)` and another interval sends a second `SNAP` whose hold field (the 5th space-separated token) is lowercase. The spawned piece sits above the visible field, so only the hold and queue changed.
- [ ] **Step 2: Implement**

```cpp
// protocol.hpp, after kSnapshotCells
inline constexpr int kSnapshotNext = 5;  // the opponent's next queue, as long as your own

// in Snapshot, after cells
Cell hold;                               // nullopt: nothing held
bool holdLocked = false;                 // hold already used this turn; drawn dimmed
std::array<Cell, kSnapshotNext> next{};  // soonest first; nullopt only before the first SNAP arrives
```

```cpp
// protocol.cpp, in the anonymous namespace
constexpr std::string_view kPieceChars = "IOTSZJL";  // the dealable prefix of kCellChars
constexpr std::string_view kLockedHoldChars = "iotszjl";

char holdChar(Cell hold, bool locked) {
    if (!hold) return kEmptyCell;
    return (locked ? kLockedHoldChars : kPieceChars)[static_cast<int>(*hold)];
}

// A dealable piece or '.', never G or B.
bool parsePiece(char c, Cell& out) {
    if (c == kEmptyCell) {
        out = std::nullopt;
        return true;
    }
    const std::size_t i = kPieceChars.find(c);
    if (i == std::string_view::npos) return false;
    out = static_cast<PieceType>(i);
    return true;
}

bool parseHold(std::string_view text, Cell& hold, bool& locked) {
    if (text.size() != 1) return false;
    const std::size_t i = kLockedHoldChars.find(text[0]);
    locked = i != std::string_view::npos;
    if (locked) {
        hold = static_cast<PieceType>(i);
        return true;
    }
    return parsePiece(text[0], hold);
}
```

  - `decodeSnapshot` reads `parts[4]` as the hold and `parts[5]` as the next queue (exactly `kSnapshotNext` chars, each through `parsePiece`), and `parts[6]` as the cells. `decode` matches `verb == "SNAP" && args == 6`.
  - `encode` writes `holdChar(m.hold, m.holdLocked)`, a space, the next chars (`cellChar` is fine: they are never `G`/`B`), a space, then the cells.
  - `snapshotOf` sets `hold = game.hold()`, `holdLocked = !game.canHold()`, and copies `game.next(kSnapshotNext)` into `next`.
  - Check that `kPieceChars` is consistent with `PieceType` (I, O, T, S, Z, J, L first, in that order), the same assumption `kCellChars` already makes.
- [ ] **Step 3: Build and run tests.** All pass. `server` tests are unaffected (`go -C server test ./...` still passes if Go is installed).
- [ ] **Step 4: Commit:** `feat(net): the opponent's hold and next queue in snapshots`

---

### Task 2: Draw the opponent's hold and next

**Files:** `src/app/render.cpp`

**Interfaces:**
- Consumes: `Snapshot::hold`, `holdLocked`, `next`; `drawMiniPiece(dl, topLeft, cell, type, alpha)`; `drawLabel`.

- [ ] **Step 1: Implement.** In `drawOpponent`, before SCORE and LINES, in the column at `statsX`:

```cpp
const float mini = l.cell * 0.7f;  // the same size as your own HOLD and NEXT
float y = wellTop(l);
drawLabel(dl, fonts, textSize, ImVec2(statsX, y), "HOLD");
if (snapshot.hold)
    drawMiniPiece(dl, ImVec2(statsX, y + l.cell), mini, *snapshot.hold, snapshot.holdLocked ? 0.35f : 1.0f);
y += 3.0f * l.cell;
drawLabel(dl, fonts, textSize, ImVec2(statsX, y), "NEXT");
for (int i = 0; i < kSnapshotNext; ++i)
    if (const Cell piece = snapshot.next[i])
        drawMiniPiece(dl, ImVec2(statsX, y + l.cell + i * 2.5f * mini), mini, *piece, 1.0f);
y += l.cell + kSnapshotNext * 2.5f * mini + l.cell;
// then the existing SCORE / LINES loop from this y
```

  Update `drawOpponent`'s comment ("the snapshot carries no hold or next") to describe the new column. Use the same `0.35f` and `0.7f` as `drawSidePanels`. If they are bare literals there, lift them into named constants shared by both, e.g. `kMiniScale` and `kLockedHoldAlpha`.
- [ ] **Step 2: Build, run tests, and check the look.** Run a local duel (`go -C server run .` plus two browser tabs on the web build) or, if the web build isn't available, temporarily feed a hand-made snapshot to `drawOpponent` on desktop. Check that:
  - the I piece fits the column;
  - the stack ends above the bottom of the well;
  - a locked hold is dim;
  - after a KO the opponent's hold empties.
  Drop any temporary code before committing.
- [ ] **Step 3: Commit:** `feat(app): draw the opponent's hold and next queue`

---

### Task 3: Docs

**Files:** `docs/superpowers/specs/2026-10-13-lan-duel-design.md`, `docs/superpowers/specs/2026-10-13-bomb-battle-design.md`

- [ ] **Step 1:** In the LAN-duel message table, change the `SNAP` row to `SNAP <score> <lines> <pending> <hold> <next> <200 cells>`. Describe hold (`.`, `IOTSZJL`, lowercase while locked) and next (5 chars, `.` or `IOTSZJL`), and add `B` to the cell characters.
- [ ] **Step 2:** In the bomb-battle non-goals, replace "showing the opponent's hold and next queue" with "showing the opponent's hold and next queue (added later, see `2026-10-14-opponent-hold-next-design.md`)". In layout item 8, note the same.
- [ ] **Step 3: Commit:** `docs: the opponent's hold and next in the duel specs`
