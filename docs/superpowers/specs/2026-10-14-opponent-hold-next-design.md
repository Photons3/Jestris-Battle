# Design: The opponent's hold and next queue

## Context
In a duel the opponent's side shows only their board, score, lines and pending garbage. The snapshot they send (`SNAP <score> <lines> <pending> <200 cells>`) carries nothing else, so there is no hold piece and no next queue for them. The bomb-battle spec listed this as a non-goal. Most duel games (TETR.IO, Tetris 99, Puyo Puyo Tetris) show both: seeing what the opponent holds and what is coming tells you whether a big attack is likely, which is half of reading a duel.

This adds the opponent's hold piece and five-piece next queue to the snapshot and draws them in the column to the right of their well.

## Goals and non-goals
- **Goals:**
  - The opponent's HOLD piece, dimmed while their hold is locked (they already held this turn), exactly like your own.
  - The opponent's NEXT queue, 5 pieces, the same length as yours.
  - No change to the duel layout's width, so the cell size (and your own board) stays the same.
  - No server change.
- **Non-goals (possible follow-ups):**
  - The opponent's level, ghost piece, combo or callouts.
  - Spectators (see "Later" in the room-lobby spec).
  - Interoperating with older builds. A client on the old build and one on the new build reject each other's snapshots. The server serves the web build, so both players normally run the same version, and a refresh fixes a stale tab.
  - Smoother updates. The hold and queue update on the existing 100 ms snapshot cadence, so they lag the opponent's real game by up to 100 ms, as their board already does.

## 1. The snapshot (`src/net/protocol.hpp`)

```cpp
inline constexpr int kSnapshotNext = 5;  // the opponent's next queue, as long as your own

struct Snapshot {
    int score = 0;
    int lines = 0;
    int pending = 0;
    std::array<Cell, kSnapshotCells> cells{};
    Cell hold;                                // nullopt: nothing held
    bool holdLocked = false;                  // hold already used this turn; drawn dimmed
    std::array<Cell, kSnapshotNext> next{};   // nullopt only in a default (never received) snapshot
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};
```

- The new fields go **after** `cells` so the existing positional initializers (`Snapshot{score, lines, pending, {}}`) keep compiling with the same meaning.
- `snapshotOf(game)` fills `hold` from `game.hold()`, `holdLocked` from `!game.canHold()` and `next` from `game.next(kSnapshotNext)`.
- After a duel KO, `Game::resetBoard()` clears the hold, so the next snapshot carries no hold. The queue continues from the same bag.
- `operator==` stays defaulted, so a change to the hold or the queue alone is a change and goes out on the next 100 ms tick. `DuelSession` needs no change: it already sends a snapshot whenever `snapshotOf` differs from the last one sent.

## 2. Wire format

```
SNAP <score> <lines> <pending> <hold> <next> <200 cells>
SNAP 1200 8 0 t IOSZL ....(200 cells)
```

| Field | Characters | Meaning |
|---|---|---|
| `<hold>` | exactly 1: `.`, one of `IOTSZJL`, or one of `iotszjl` | Nothing held; the held piece; the held piece while hold is locked |
| `<next>` | exactly 5, each `.` or one of `IOTSZJL` | The next pieces, soonest first |
| `<cells>` | unchanged | As before: 200 cells, row-major from the bottom row, `.` or `IOTSZJLGB` |

- The decoder requires exactly 6 arguments. The old 4-argument form is rejected.
- `G` (garbage) and `B` (bomb) are rejected in `<hold>` and `<next>`: they are cell types, never pieces a player holds or is dealt.
- A full snapshot is about 225 bytes, far under the server's 4096-byte message limit.
- The server relays `SNAP` verbatim without parsing it (`server/hub.go`), so it needs no change.

## 3. Drawing (`src/app/render.cpp`)

`drawOpponent` fills the 5-cell column at `kStatsCol`, right of the opponent's well, from the top of the well down:

1. **HOLD** label, then the held piece as a mini piece (`0.7` of a cell, the same size as yours), at alpha `0.35` while `holdLocked`, otherwise `1.0`. Nothing under the label when nothing is held.
2. **NEXT** label, then up to 5 mini pieces spaced `2.5 × mini` apart, the same spacing as your own queue. Empty slots draw nothing.
3. **SCORE** and **LINES**, as today, below the queue.

In cells from the top of the well: HOLD label at 0, held piece at 1, NEXT label at 3, queue from 4 to about 12.75, then SCORE at about 13.75 and LINES 2.1 below it. The stack ends around row 18 of 20. An I piece is 2.8 cells wide in the 5-cell column. The layout's 46 columns are unchanged.

Before the first snapshot of a match arrives, the default snapshot draws empty HOLD and NEXT panels, the same as an empty board.

## 4. Testing

- **Protocol (doctest):**
  - The round-trip list gains a snapshot with a hold, a locked hold and a full queue, and keeps one with no hold.
  - Exact wire strings: `encode(Snapshot{})` is `"SNAP 0 0 0 . ..... " + 200 dots`; a locked T hold encodes as `t`.
  - Malformed: the old 4-argument `SNAP`; a hold of `G`, `B`, `g`, `x` or two characters; a next of 4 or 6 characters, or containing `G` or `B`.
  - `snapshotOf`: a fresh game has no hold, the hold unlocked, and `next` equal to `game.next(5)`. After `Action::Hold`, `hold` is the first piece and `holdLocked` is true. After `resetBoard()`, `hold` is empty again.
- **Duel:** with the board unchanged, holding a piece still sends a new snapshot on the next interval.
- **Server:** no change; its tests relay opaque `SNAP` text already.

## 5. Docs
- The `SNAP` row in the LAN-duel spec's message table gets the new fields.
- The bomb-battle spec's non-goal "showing the opponent's hold and next queue" points here.
