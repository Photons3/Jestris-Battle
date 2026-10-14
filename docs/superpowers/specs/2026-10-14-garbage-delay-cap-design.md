# Design: Garbage travel delay and per-lock cap

## Context
In a duel, incoming garbage rises in full on the very next lock that clears nothing. An attack that lands just before you lock gives no chance to cancel it, and a big spike (a back-to-back tetris plus combo) can push 10+ rows at once and KO instantly.

This follows TETR.IO's two defaults that address that: incoming garbage **travels** for a short time before it can rise, and **at most 8 lines** rise per lock. Bombs already play the role of TETR.IO's "messiness" settings, so garbage shape does not change.

## Goals and non-goals
- **Goals:**
  - Incoming garbage waits `kGarbageTravelMs` = 333 ms (TETR.IO's 20 frames) of game time before it can rise.
  - At most `kMaxGarbagePerLock` = 8 lines rise per non-clearing lock; the rest stay ready for later locks.
  - The garbage meter shows travelling lines dimmed and ready lines solid, for both players.
  - Online duels and the practice bot get the same rules, since both run `Game`.
- **Non-goals:**
  - A late-match damage multiplier ("surge").
  - Making the delay or cap configurable.
  - Any change to the Go server.
  - Reading the old `SNAP` format.
  - Changing bomb placement or garbage shape.

## 1. Rules (`core/game.{hpp,cpp}`)

```cpp
inline constexpr int kGarbageTravelMs = 333;  // incoming garbage waits this long before it can rise
inline constexpr int kMaxGarbagePerLock = 8;  // most lines that rise after one lock
```

`Game` replaces `int pendingGarbage_` with a queue of batches, oldest first:

```cpp
struct GarbageBatch {
    int lines;
    int msLeft;  // 0: ready to rise
};
std::deque<GarbageBatch> incoming_;
```

- **Receiving.** `receiveGarbage(lines)` appends `{lines, kGarbageTravelMs}`. Zero lines appends nothing.
- **Travel.** Each 1 ms `step()` in the `Playing` or `Clearing` state lowers every batch's `msLeft` by 1, down to 0. It does not run while `Paused` or `GameOver`, and the duel does not tick a frozen game, so the timer also stops during the KO flash and after the clock reaches 0:00.
- **Cancelling.** `finishClear()` cancels `min(attack, pendingGarbage())` lines from the queue **oldest first**, ready or travelling alike, shrinking or removing batches. What is left of the attack is sent, as today. `stats_.cancelled` counts the same lines as today.
- **Rising.** `applyGarbage()` after a lock that clears nothing takes up to `kMaxGarbagePerLock` lines from the ready batches, oldest first, splitting the last batch if needed, and pushes them as one `pushGarbage` call with today's bomb-column rules. Travelling lines never rise. If nothing is ready, nothing rises and no `GarbageRose` event fires.
- **Accessors.**
  - `pendingGarbage()`: total lines in the queue, travelling and ready. Existing callers keep their meaning.
  - `readyGarbage()`: lines whose batch has `msLeft == 0`.
- **KO.** `resetBoard()` empties the queue.
- **Top-out.** Unchanged: if the rising lines push blocks off the top, the lock tops out.

## 2. Snapshot (`net/protocol.{hpp,cpp}`)

- `Snapshot` gains `int ready = 0;` after `pending`. `pending` stays the total.
- `snapshotOf` sets `ready = game.readyGarbage()`.
- Wire format, 14 arguments:

  ```
  SNAP <score> <lines> <pending> <ready> <pieces> <attack> <garbageCleared> <cancelled> <tetrises> <tSpins> <maxCombo> <hold> <next> <cells>
  ```

- Decoding rejects a negative `ready` or `ready > pending`, and the old 13-argument form.
- The server relays `SNAP` without reading it, so `server/` does not change. Both players need the same build, as with earlier `SNAP` changes.

## 3. Meter (`app/render.cpp`)

`drawGarbageMeter(dl, x, bottom, cell, pending, ready)`:

- Draws `min(pending, kVisibleHeight)` cells from the bottom up, as today.
- The bottom `ready` cells are solid, in the garbage color, or the danger color with today's blink once `garbageMeterDanger(pending)` holds (4+ total lines).
- The cells above them are travelling lines: the same color and bevel at about 40% alpha, never blinking.
- Your meter uses `game.pendingGarbage()` and `game.readyGarbage()`; the opponent's uses `snapshot.pending` and `snapshot.ready`.

## 4. Docs
- This spec.
- The `SNAP` row in `2026-10-13-lan-duel-design.md` notes the `ready` field and points here.
- README: one line on the 0.33 s garbage travel time and the 8-line cap per lock.

## Testing
- **Game** (`tests/game_test.cpp`):
  - Received garbage does not rise on a lock before 333 ms of ticks, and does rise on a lock after.
  - `pendingGarbage()` and `readyGarbage()` report total and ready lines as batches travel.
  - 12 ready lines rise as 8 on one non-clearing lock and 4 on the next.
  - A clear cancels oldest batches first: with a ready batch and a newer travelling batch, a small attack shrinks the ready one first; a larger attack also eats into the travelling one.
  - Pausing stops travel: ticks while paused leave the batch travelling.
  - `resetBoard()` empties the queue.
  - Existing garbage tests are updated to tick past the travel time where they expect garbage to rise.
- **Protocol** (`tests/protocol_test.cpp`):
  - `ready` survives an encode/decode round trip; the wire-format strings gain the `ready` field.
  - `ready > pending`, a negative `ready` and the 13-argument form are rejected.
  - `snapshotOf` reports `ready` as garbage travels.
- **Duel and bot** (`tests/duel_test.cpp`, `tests/local_match_test.cpp`): existing tests that expect garbage to rise tick past the travel time.
- **UI** (`tests/ui_test.cpp`): a headless frame draws a meter with both travelling and ready lines without tripping ImGui asserts.
