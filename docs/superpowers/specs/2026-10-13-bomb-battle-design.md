# Design: Bomb garbage, KO matches and a Tetris Battle duel screen

## Context
This extends the LAN duel in `docs/superpowers/specs/2026-10-13-lan-duel-design.md`. The goal is a duel that plays and looks like Tetris Battle / Notris Foes:

- **Bomb garbage:** the garbage hole is filled with a bomb that you set off by landing a piece on it, instead of an open gap that you fill.
- **KO match rules:** topping out is a KO that resets your board, and the first player to 3 KOs wins. A 2-minute timer ends the match early, and then more KOs wins, with lines sent breaking a tie.
- **Layout A duel screen:** two equal-size boards with a scoreboard column between them.

Solo play and the desktop build are unchanged. Where this spec contradicts the LAN duel spec (garbage gaps, "first top-out loses", `DEAD` being relayed), this spec wins. The LAN duel spec gets a pointer to this one when it ships.

## Goals and non-goals
- **Goals:** bomb garbage with chain detonation; bomb rows counted as cleared lines for the attack; KO-and-reset on top-out; first to 3 KOs wins; a 2:00 match timer decided by KOs, then lines sent; the server deciding the result so both players always see the same outcome; the layout A duel screen.
- **Non-goals:** a configurable timer length in the UI, showing the opponent's hold and next queue (added later, see `2026-10-14-opponent-hold-next-design.md`), T-spin/combo/back-to-back bonuses, avatars or player names (bars say YOU / FOE), sound, changes to solo mode.

## 1. Bomb garbage (`tetris_core`)

### Cells
- New cell type `PieceType::Bomb`, appended after `Garbage`. The bag never deals it (the dealable count stays 7). It goes over the wire as `B` in snapshots.
- `PieceType::Garbage` stays for the gray filler cells of a garbage row.

### Rising
- `Board::pushGarbage(rows, bombX)` works like today, except that column `bombX` gets a `Bomb` cell instead of staying empty. Every row in one batch shares `bombX`, so a batch forms a vertical bomb stack. Each batch draws a new column from the existing garbage RNG.
- The rising rules are unchanged: pending garbage rises all at once only when a piece locks without clearing anything (where "clearing" now includes detonations, see below); a clear cancels pending garbage first; a push that would move a filled cell off the top, or leave no room to spawn, is a top-out.

### Bomb rows are never "full"
A bomb row has all 10 cells filled. `Board::fullRows()` skips any row that contains a `Bomb` cell, so bomb rows don't clear the moment they rise.

### Detonation
When a piece locks:

1. **Find hits.** For each cell `(x, y)` of the locked piece with `y > 0`, if `board(x, y - 1)` is a `Bomb`, row `y - 1` is hit.
2. **Chain.** From each hit row, walk downwards in the same column while the cell is a `Bomb`. Every one of those rows explodes too. The walk stops at the first cell in that column that isn't a bomb. The walk follows the column, not the batch: if an older batch happened to roll the same `bombX` and touches the newer one, it explodes as well. No extra per-cell batch state is kept.
3. **Combine.** Exploded rows join the normal full rows from the same lock, without duplicates. If the set isn't empty, the game enters the existing `Clearing` state, and all those rows flash and are removed together.
4. **Attack and score.** `cleared = normal rows + exploded rows`. The attack is `attackFor(min(cleared, 4))` from the existing 0/1/2/4 table, so 2 bombs send 1, 4 bombs send 4, and 1 bomb plus a triple send 4. Cancelling pending garbage, `lines_` and `lineClearScore` all use `cleared` the same way, with the score also capped at 4 lines.
5. A lock that detonates counts as a clearing lock, so pending garbage is held back, the same as after a normal clear.

New API: `Board::bombRowsHitBy(const Piece&) -> std::vector<int>` returns the hit rows plus their chain, sorted and without duplicates. `Game::lockPiece` merges it with `fullRows()`. A piece that sits beside a bomb, or above a bomb with a gap in between, sets nothing off.

Only cells of the piece that just locked can set off a bomb. When rows are removed and the cells above drop, a cell that lands on a bomb does not detonate it, matching the existing naive-gravity line clears.

### Tests (doctest)
- `pushGarbage` puts a bomb at `bombX` in every new row, and a refused push leaves the board unchanged.
- `fullRows` ignores a bomb row.
- Landing directly on a bomb clears its row; landing beside a bomb does nothing.
- The chain clears the whole stack and stops at the first non-bomb cell.
- A clear that leaves a cell resting on a bomb does not detonate it.
- A bomb hit and a normal clear in one lock add up to one attack value; 5+ total rows are capped at the 4-line attack.
- A detonating lock holds pending garbage back; cancelling works with bomb clears.
- Top-out from oversized garbage still works.

## 2. KO matches, timer and the server referee

### Rules
- After `START`, both clients show the 3 s countdown, then a **2:00 match clock**.
- **Topping out is a KO.** The client that topped out:
  1. sends its final `SNAP` (if it changed) and then `DEAD`;
  2. resets its own `Game`: empty board, pending garbage dropped, the same bag continuing, hold cleared, score/lines/level kept;
  3. shows a "KO!" flash over its well for 1000 ms, during which input and gravity are paused for that player only. The match clock keeps running.
  The opponent's board is not touched, and a KO gives the KOer no bonus beyond the count.
- **Garbage during the KO flash** is queued into the fresh game, as normal. It rises on the first lock that doesn't clear anything, so a large attack can cause another KO on the first piece. The garbage isn't dropped, because the server has already counted it as sent.
- **Win conditions** (decided only by the server):
  - The first player to score **3 KOs** wins at once (reason `KO`).
  - When the clock runs out (reason `TIME`), the player with **more KOs** wins. If the KOs are tied, the player with **more lines sent** wins. If both are tied, it's a **draw**.
- "Lines sent" is the total of `n` across a player's accepted `ATK n` messages during the match. That is the net garbage after cancelling, the same number the opponent receives. Defending by cancelling earns no credit, so the server can check the number without trusting the client.
- **0:00 freeze:** when a client's local match clock reaches 0, that player's input and gravity freeze until `OVER` arrives. The server allows a grace window (below) so attacks from the last moment before the freeze still count.

### Protocol changes

| Message | Direction | Meaning |
|---|---|---|
| `ATK <n>` | client → server → opponent | Unchanged on the wire (n = 1–40). The server checks it, adds `n` to the sender's lines sent, then relays it. A malformed `ATK` is dropped and not counted. |
| `DEAD` | client → server | The sender topped out. The server handles it and **no longer relays it**. |
| `KO <mine> <theirs>` | server → each player | KO counts after a KO, from the receiver's point of view (0–3 each). |
| `OVER <WIN\|LOSE\|DRAW> <KO\|TIME> <mySent> <theirSent>` | server → each player | Match over: outcome, reason and final lines-sent totals from the server's tally. |

`KO` and `OVER` join `ROOM`, `START`, `LEFT` and `ERR` as server-only verbs: if a client sends one, the server drops it. The client decoder rejects counts outside 0–3, unknown outcome or reason tokens, and negative or non-numeric lines sent.

### Server (`server/hub.go`)
- Each room gets a match state, reset by every `START`:
  - `kos [2]int`, `sent [2]int`
  - `over bool`
  - `matchID uint64`, incremented per `START`
  - `timer` (a stoppable handle from the injected clock)
- **On `START`** (join or rematch): reset the state, bump `matchID`, and arm the timer for `countdown + matchLength + matchGrace` (3 s + 2 min + 500 ms by default). Each client starts its clock when `START` arrives, so its clock runs one-way latency behind the server's. The grace window covers that gap, so an `ATK` sent just before a client's 0:00 freeze is still counted. The server's countdown constant must match the client's `kCountdownMs`, and each one should cite the other in a comment.
- **`ATK`:** if the match isn't over and `n` is 1–40, add `n` to `sent[seat]` and relay it. Otherwise drop it.
- **`DEAD`:** if the match isn't over, `kos[opponent]++`.
  - Below 3: send `KO` to both players.
  - At 3: send `KO` to both, then `OVER WIN|LOSE KO …`, mark the match over and stop the timer.
- **Timer fires:** take the hub lock. Ignore it if the room is gone, `matchID` has changed or the match is already over. Otherwise compare `kos`, then `sent` if the KOs are tied, send `OVER … TIME` to both, and mark the match over. Writes happen after the lock is released, as today.
- **Rematch** is accepted only once the match is over (`REMATCH` before `OVER` is ignored). It starts a fresh match the same way as today.
- **Leaving** stops the timer before the room is deleted. `LEFT` behaves as before.
- **Injection for tests:** `Hub` gains `afterFunc func(d time.Duration, f func()) stopper` (real: `time.AfterFunc`), `matchLength time.Duration` and `matchGrace time.Duration` (default 500 ms). A new `-match` flag (default `2m`) sets `matchLength` for manual testing; the README documents it.

### Client (`DuelSession`)
- New state, reset on `START`:
  - `kosMine_`, `kosTheirs_` (set only by `KO`)
  - `sentMine_` (added locally from outgoing attacks), `sentTheirs_` (added from incoming `ATK`)
  - `matchClockMs_`: starts at 120000, counts down only in Playing (including during the KO flash), and stops at 0
  - `koFlashMs_`
  - `result_`: outcome, reason and the server's final lines-sent totals
- The local clock and the sent counters are for display only. The `OVER` totals are what the result screen shows. The KO counts on the result screen come from the last `KO` message, so `OVER` doesn't need to carry them.
- **`frozen()`** is true during the KO flash or once `matchClockMs_` reaches 0 in Playing. While it's true, the app skips `game->tick` and doesn't apply input. `Game` stays free of duel rules: duels have `allowPause = false`, and the freeze lives only in the session.
- `DuelOutcome` becomes `None | Won | Lost | Draw | OpponentLeft`. A `DuelReason` of `KOs | Time` comes from `OVER`.
- **Local top-out** (game reaches GameOver while Playing): send `SNAP` if it changed, send `DEAD`, replace the game with a reset one, and start the KO flash. The phase stays Playing.
- **Incoming `KO`:** update the counts. **Incoming `OVER`:** go to Result with that outcome and reason.
- **Phase gating** follows the existing rules: `ATK`, `KO` and `OVER` act only in Countdown or Playing. An incoming `DEAD` is ignored everywhere (the server no longer relays it).
- `Game` gains a `resetBoard()` that clears the board, pending garbage, hold, the active piece and clearing state, then spawns from the continuing bag. Score, lines and level stay. It works from GameOver as well as Playing.

### Tests
- **Server (`go test -race`, fake clock, deterministic seeds):**
  - `ATK` adds to lines sent and is still relayed; a malformed `ATK` is dropped
  - `DEAD` gives `KO` to both players, and the third KO gives `OVER WIN/LOSE KO`
  - timeout win, loss and draw
  - at timeout, KOs decide even when the KO leader sent fewer lines; lines sent decide only when the KOs are tied
  - an `ATK` arriving inside the grace window is counted
  - `ATK`/`DEAD` after `OVER` are ignored
  - a stale timer after a rematch is ignored
  - leaving mid-match stops the timer
  - `REMATCH` before `OVER` is ignored
  - client-sent `KO`/`OVER` are dropped
- **Protocol (doctest):** round trip and exact wire format for `KO` and `OVER`; malformed cases (wrong argument count, bad tokens, out-of-range counts); snapshots carrying `B`.
- **Session (doctest):**
  - a top-out sends `DEAD`, resets the board and stays in Playing
  - `KO` updates the counts
  - each `OVER` variant reaches Result with the right outcome and reason
  - `OVER`/`KO` are ignored outside Countdown and Playing
  - the clock counts down and stops at 0
  - `frozen()` is true during the KO flash and once the clock hits 0; the clock keeps running during the flash
  - garbage received during the KO flash is queued into the fresh game
  - the sent counters tally both ways
- **Game:** `resetBoard` keeps score, lines and level and clears everything else.

## 3. Duel screen (layout A)

Solo mode keeps its current look. The duel view (Countdown, Playing and Result) is redrawn.

### Layout, left to right
1. **Your HOLD** panel.
2. **Your well**, with a name bar above it: a blue-to-purple gradient, "YOU", and three KO pips (●○○) for the KOs you have scored.
3. **Your NEXT queue** (5 pieces), then SCORE and LEVEL.
4. **Your garbage meter**, on the inner edge of your well.
5. **The center scoreboard column:**
   - TIME `m:ss`, drawn in red under 0:10
   - "VS"
   - `KO 1 : 0`
   - `SENT 12 : 7`
   Your number is always on the left.
6. **The opponent's garbage meter**, on the inner edge of their well.
7. **The opponent's well, the same cell size as yours**, with a red-to-orange "FOE" name bar and KO pips.
8. **The opponent's SCORE and LINES.** There is no hold or next for the opponent; the snapshot doesn't carry them. (Added later: `2026-10-14-opponent-hold-next-design.md` stacks HOLD and NEXT above these.)

`computeDuelLayout` budgets about 45 cell-widths (hold 5, well 10, next 5, meter 1, center 7, meter 1, well 10, stats 5, plus gutters) and 22 rows plus the name bar. The cell size is the smaller of the width fit and the height fit, so phone landscape works at a smaller scale. The reference mockup is `.superpowers/brainstorm/*/content/duel-layout-a-v2.html` (local only, gitignored).

### Look
- A dark navy radial-gradient background behind the duel view.
- **Bomb cell:** the gray garbage bevel with a dark circle in the middle and a small orange fuse dot in the top-right. Rows mid-detonation flash white, like normal clears.
- **KO flash:** a large yellow "KO!" with an orange glow, centered on your well for 1000 ms.
- **Countdown:** the existing 3-2-1 overlay, centered on the whole view.
- **Result:** a centered menu showing the outcome title (YOU WIN / YOU LOSE / DRAW), then a reason line:
  - "3 KOs"
  - "Time up · KO 2 : 1" when the KOs decided it
  - "Time up · KO 1 : 1 · 12 : 7 sent" when lines sent decided it, or for a draw
  - "Opponent left the match"

  Your numbers are on the left. Then Rematch (hidden when the opponent has gone, replaced by "Waiting for opponent..." once requested) and Leave.

### Testing
The duel screen is checked by hand, as before. Two tabs on localhost, server started with `-match 20s`:
- bomb hit
- chain of 3 or more bombs
- bomb plus a normal clear together
- KO and reset
- 3-KO win
- timeout win (decided by KOs and by lines sent) and timeout draw
- the 0:00 freeze, and a clear in the last instant still counting
- rematch after each ending
- opponent closing their tab mid-match
- phone-landscape width

## Decisions
- **Bomb trigger:** a piece cell resting directly on a bomb detonates it (option B), rather than clearing a line next to the bomb row.
- **Bomb attack:** exploded rows count as cleared lines in the existing 0/1/2/4 table.
- **Bomb strength:** bombs are deliberately cheap to return: any piece that reaches the column answers a whole batch. This makes for a fast back-and-forth game, and the 4-line attack cap limits it.
- **Chain:** hitting a bomb takes every touching bomb below it in that column, even when two batches happened to share a column.
- **No cascade:** only the piece that just locked can set off a bomb.
- **Match rules:** first to 3 KOs; at the end of the 2:00 timer, more KOs wins, then more lines sent, then a draw; a KO resets only the loser's board and gives the KOer no bonus.
- **Lines sent is net:** garbage after cancelling, which the server can check from `ATK` alone.
- **Garbage during the KO flash is queued**, not dropped, so sent and received always match.
- **Time-up:** clients freeze at their local 0:00. The server waits a 500 ms grace window beyond that, so a clear in the last instant still counts.
- **Server referee:** the server counts lines sent and KOs, owns the clock and sends one verdict to both players, so close finishes can't produce two winners.
- **Layout A:** equal-size boards with a center scoreboard (Tetris Battle style), not mirrored full panels or a big/small split.
