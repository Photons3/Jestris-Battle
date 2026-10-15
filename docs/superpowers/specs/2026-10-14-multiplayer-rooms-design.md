# Design: Rooms for up to 16 players, host start and battle royale

## Context
Today a room has exactly two seats and the match starts the moment the second player joins (`server/hub.go`, `DuelSession`). This spec extends the LAN duel (`2026-10-13-lan-duel-design.md`), the KO duel (`2026-10-13-bomb-battle-design.md`), player names (`2026-10-13-player-names-design.md`) and the room lobby (`2026-10-13-room-lobby-design.md`) so that:

- **Bigger rooms:** a room holds up to 16 players.
- **Host start:** the room's host decides when each match starts.
- **Battle royale for 3+ players:** one top-out eliminates you, there's no clock, and the last player standing wins. Garbage has an open hole instead of bombs, the way Tetr.io and Tetris 99 do it.
- **1v1 stays as it is:** a match with exactly 2 players uses today's duel rules (first to 3 KOs, 2:00 clock, lines sent as the tiebreak), including the stats table.

Where this spec contradicts the earlier ones, this spec wins. Specifically, it replaces:
- the two-seat room;
- starting on the second join;
- `REMATCH` and `OPP`;
- "a room leaves the list when its second player joins and is deleted when either player leaves".

Each of those specs gets a "Partly superseded" pointer to this one when it ships.

## Goals and non-goals
- **Goals:**
  - Rooms of 2–16 players.
  - A room screen with the roster and a host-only Start button, plus host migration.
  - Late joiners waiting for the next match.
  - The mode chosen by participant count (2 = duel, 3–16 = royale).
  - Royale: random-target attacks, elimination, placements and a time-based gravity floor.
  - A mini-board grid of opponents.
  - Returning to the room after every match.
  - Public rooms, Quick match and invite links that keep working with bigger rooms.
  - The practice bot that keeps working.
- **Non-goals:** more than 16 players, targeting strategies or badges, teams, spectators who aren't in the room, a host setting that forces the mode, kicking players, chat, throttling snapshots for big rooms, new sounds, bot opponents in royale, any change to solo play or the desktop build.

## 1. Rules

### Mode
The mode is fixed when the host starts a match, from the number of participants:

| Participants | Mode | Rules |
|---|---|---|
| 2 | Duel | Unchanged: top-out is a KO that resets your board; first to 3 KOs wins; 2:00 clock, then more KOs, then more lines sent; a full tie is a draw. |
| 3–16 | Royale | Top-out eliminates you. No clock. Last player alive wins. Everyone gets a place (1 = winner). Hole garbage instead of bombs. |

### Royale details
- **Attacks:** the attack table, cancelling, garbage travel and the per-lock cap are unchanged. The server sends each accepted `ATK n` to **one living opponent chosen at random**, picked separately for every attack.
- **Hole garbage (no bombs):** each garbage row is gray with exactly one open cell, the hole. All rows from **one attack** (one `ATK`) share the same hole column, so you can dig them out down one well. Each new attack picks a new random column, different from the previous attack's. The column is chosen when the attack is received and stored with it, so an attack that the 8-lines-per-lock cap splits across several locks keeps its column. Hole rows clear like any full line and count toward Tetrises, T-spins, back-to-back and the garbage-cleared stat as normal. Bombs, detonation and the two-high bomb pairing stay in the duel only.
- **Elimination:** a player who tops out is out. Their place is the number of players alive just before they died (the first out of 9 is #9). Their board stays frozen on the game-over state, and they keep watching the other boards until the match ends.
- **Disconnects:** a participant who disconnects mid-match is eliminated the same way.
- **End:** when one player is left, that player is placed #1 and the match ends.
- **Gravity floor:** to keep matches from stalling, the level used is `max(normal level, startLevel + floor(elapsed / 30 s))`. `elapsed` counts from the end of the countdown. There is no level cap: gravity speed already stops rising at level 20 (`kGravityLevelCap`). The displayed level, and the level used for scoring, follow the level actually used.

## 2. Protocol
Plain-text WebSocket messages, space-separated, as before. Anything malformed, unknown, out of range or with the wrong argument count decodes to nothing and is ignored. Names keep the existing rules (`HELLO`, 1–12 of `A–Z a–z 0–9 _ - .`, server default `Player`).

**Player IDs:** 1–16, unique within a room, the lowest free number at join time.

**Room names:** two players with the same name get the lowest numeric suffix that makes the name unique (`Sam`, `Sam2`), shortening the base so the result stays within 12 characters. These room names are what the roster shows.

### Changed

| Message | Change |
|---|---|
| `ROOM <CODE> <yourId>` | Sent to **every** player who enters a room, whether by `CREATE`, `JOIN` or `QUICK`, and carries their ID. |
| `START <seed> <lvl> <id>…` | Lists the 2–16 participant IDs. Sent only to participants; a client whose ID isn't listed ignores it. |
| `SNAP …` (server → client) | Relayed to every other participant as `SNAP <fromId> …`. The client → server form is unchanged. |
| `ATK <n>` | Unchanged on the wire. Duel: sent to the opponent. Royale: sent to a random living opponent. |
| `DEAD` | Duel: a KO as today. Royale: an elimination. |
| `ROOMS [<CODE>:<lvl>:<host>:<n>]…` | Each entry gains the player count `n` (1–15). `host` is the current host's room name. |
| `ERR room is full` | Now sent at 16 players. |

### New

| Message | Direction | Meaning |
|---|---|---|
| `PLAYERS <hostId> <id>:<name>:<m>…` | server → everyone in the room | The full roster in join order, with 1–16 entries. `m` is `1` if the player is in the current match and `0` otherwise. Sent on every join, leave, host change, match start and match end. |
| `BEGIN` | host → server | Start a match. Ignored unless the sender is the host, the room has 2 or more players and no match is running. |
| `OUT <id> <place>` | server → every participant | Royale only. Player `id` is out in place `place` (2–16). When one player is left, `OUT <winnerId> 1` follows and ends the match. |

### Removed
`REMATCH` and `OPP`. Names come from `PLAYERS`.

### Unchanged
`HELLO`, `CREATE <lvl> [PUB|PRIV]`, `JOIN <CODE>`, `LIST`, `UNLIST`, `QUICK <lvl>`, `KO` and `OVER` (duel only), `LEFT` (duel only: the opponent disconnected mid-match) and `ERR`.

### Server-only verbs
`ROOM`, `PLAYERS`, `START`, `KO`, `OVER`, `OUT`, `LEFT`, `ERR` and `ROOMS` are sent only by the server. If a client sends one, the server drops it.

## 3. Server (`server/hub.go`)

### Rooms
- A room holds up to 16 players in join order, the host, the start level, its visibility, and the current match (if any).
- **Join:** the 17th player gets `ERR room is full`. Joining while a match is running is allowed; that player waits in the room. Every successful create, join or quick join sends `ROOM` to the new player and `PLAYERS` to the whole room.
- **Host:** the creator. When the host leaves, the player who has been in the room longest becomes host, and a new `PLAYERS` is broadcast. The room is deleted when its last player leaves.
- **Public rooms:** a public room is listed while it has fewer than 16 players, whether or not a match is running. Watchers get a new `ROOMS` whenever a listed room appears, disappears, or changes its player count or host.
- **Quick match:** `QUICK` joins the oldest listed room that has **no match running**, otherwise the oldest listed room, otherwise opens a public room at the requested level.
- **Start:** a valid `BEGIN` makes every player in the room a participant and picks the mode from their count. It sends `START` with a fresh seed to each participant and broadcasts `PLAYERS` with the in-match flags set. A duel arms the match timer as today; a royale arms no timer.

### Match end
- **Duel:** at 3 KOs, at time-up, or when one of the two participants leaves (the other gets `LEFT`).
- **Royale:** when `OUT … 1` is sent.
- When a match ends, the room returns to its between-matches state, the in-match flags clear, `PLAYERS` is broadcast, and the host can `BEGIN` again.

### Relaying and refereeing
- `SNAP` from a participant goes to every other participant as `SNAP <fromId> …`. From a non-participant it is dropped.
- `ATK` and `DEAD` from a non-participant, an eliminated player, or outside a match are dropped. Other unknown verbs are no longer relayed.
- The random target picker is injectable, so tests are deterministic.
- Writes still happen outside the lock with the 2 s timeout.

## 4. Client session (`DuelSession`)

### Phases
The existing `Lobby` phase (the public room browser) keeps its name and meaning. A new **`Room`** phase is the between-matches state inside a room. `Waiting` now means only "request sent, no `ROOM` yet".

| Phase | Meaning | Leaves on |
|---|---|---|
| Idle | Not connected to any room or lobby. | `openLobby` → Lobby; `createRoom` / `joinRoom` / `quickMatch` → Waiting |
| Lobby | Watching the public room list (unchanged). | as today |
| Waiting | Request sent, no `ROOM` yet. | `ROOM` → Room; `ERR` → Idle or back to Lobby, as today; `leave()` → Idle |
| Room | In a room, no match for this player. Holds the roster, `myId` and `hostId`. | `START` that lists `myId` → Countdown; `leave()` → Idle |
| Countdown | As today. | countdown done → Playing; match end → Result |
| Playing | As today, per mode. | match end → Result |
| Result | Outcome shown; the room is between matches again. | `START` that lists `myId` → Countdown; `leave()` → Idle |

- `PLAYERS` updates the roster and host in Room, Countdown, Playing and Result.
- `isHost()` is `myId == hostId`. `begin()` sends `BEGIN` only in Room or Result, only for the host, and only with a roster of 2 or more.
- `requestRematch()`, `rematchRequested()`, `opponentGone()` and `opponentName()` are removed.

### Match state
- `mode()`: Duel or Royale, from the length of the `START` list.
- `opponents()`: a map `id → OpponentView { name, Snapshot, alive, place }` with one entry per other participant, built from `START` and named from the latest roster. This replaces the single `opponent()` snapshot. `SNAP <fromId>` updates the matching entry and is ignored for IDs not in the map.
- The duel screen, the duel stats table and `resultReasonText` read the single opponent from this map.

### Duel mode
Unchanged behaviour: KO flash and board reset, the 2:00 display clock and 0:00 freeze, `KO` and `OVER`, and `LEFT` giving the `OpponentLeft` outcome.

### Royale mode
- **Local top-out:** send the final `SNAP` (if it changed) and `DEAD` once. The local game stays in GameOver (no reset), and `eliminated()` becomes true.
- **`OUT <id> <place>`** for another player marks that opponent dead with its place. For this player it sets `myPlace()`.
- **`OUT <id> 1`** ends the match: outcome **Won** if `id` is this player, otherwise **Lost**, with `myPlace()` already set from this player's own `OUT`.
- **No match clock:** `frozen()` is false in royale Playing, and `playedMs()` counts up from the end of the countdown (for PPS/APM).
- **Gravity floor:** every frame in Playing, the session sets the game's floor from the elapsed match time (section 1).

### Robustness (carried over and extended, each pinned by a test)
- Messages in Idle change nothing.
- `ATK`, `KO`, `OVER`, `OUT` and `LEFT` act only in Countdown or Playing.
- `START` is accepted only in Room or Result, and only if it lists `myId`.
- `ERR` is fatal only in Waiting.
- Connection loss from any non-Idle phase goes to Idle with "Connection lost".

## 5. Practice bot (`src/bot/local_match`)
`LocalMatch` keeps playing the server for the offline duel, in the new protocol:
- `CREATE` gets `ROOM <code> 1`, `PLAYERS 1 1:<you>:1 2:Bot:1` and then `START <seed> <lvl> 1 2` straight away. The practice duel still starts without a Start click.
- `BEGIN` starts the next match, replacing `REMATCH`.
- Its `SNAP`s go out as `SNAP 2 …`.
- Duel rules only; the bot never plays royale.

## 6. Core (`tetris_core`)
- `Game::setLevelFloor(int)`: gravity, scoring and the reported level use `max(normal level, floor)`. The floor defaults to 0 (no effect), so solo play and duels are unchanged.
- `GameOptions::garbage`, an enum `GarbageStyle { Bombs, Holes }`, defaults to `Bombs`. `makeDuelGame` picks `Holes` for a royale `START` and `Bombs` for a duel.
- With `Holes`, `receiveGarbage` draws the batch's hole column from the garbage RNG (any column except the previous batch's) and stores it on the `GarbageBatch`. When garbage rises, each batch's rows are pushed with its own column.
- A new `Board::pushGarbageHoles(const std::vector<int>& holeColumns)` pushes one gray row per entry, bottom row first, with an empty cell in the given column. It refuses an overflowing push and leaves the board unchanged, like `pushGarbage`. `fullRows` needs no change, since hole rows become full only when the hole is filled.

## 7. Screens

### Start screen and lobby
- Unchanged, apart from each row in Browse rooms showing the player count ("Sam · Lv 3 · 5/16").

### Room (replaces the "waiting for an opponent" screen)
- The room code in large type, the invite link as today, and "Players n/16".
- The roster in join order: ★ marks the host, your own row is highlighted, and players who aren't in the running match show "(next match)".
- The host sees **Start**, disabled with "Need 2+ players" while alone. Everyone else sees "Waiting for the host to start…".
- **Leave** for everyone.

### Countdown and Playing
- **Duel:** the current layout, unchanged.
- **Royale:** your own well at full size on the left, with hold, next, score and level. The remaining width holds a grid of mini boards, one per opponent in ID order. Columns and rows are chosen to give the largest cell size that fits (for example 2×1 for 2 opponents, 4×2 for 8, 5×3 for 15). Each mini board shows the name, a pending-garbage meter and the board, with no hold, next or ghost. An eliminated opponent's board is dimmed and labelled with its place ("#7").
- **Eliminated:** an "OUT · #7" overlay over your own well while the grid keeps updating.

### Result
- **Duel:** unchanged (title, reason line and stats table), but the Rematch button becomes the host controls below.
- **Royale:** "YOU WIN" or "#4 of 9", then standings in place order with each player's name and attack (taken from their last snapshot's stats).
- Below that, the room controls: the host's **Start next match**, everyone else's "Waiting for the host…", and **Leave**. The roster includes players who joined mid-match. The boards stay visible behind the menu.

## 8. Testing
- **Server (`go test -race`, with deterministic codes, seeds and target picker):**
  - Up to 16 players; the 17th gets `ERR room is full`.
  - Room-name suffixes, including at the 12-character limit.
  - `ROOM` with the ID for create, join and quick join; `PLAYERS` on every change.
  - `BEGIN` only works for the host, with at least 2 players, outside a match.
  - Host migration when the host leaves; an empty room is deleted.
  - `START` lists exactly the current players, and a mid-match joiner gets no `START`.
  - Public rooms: listed until full, with the player count and current host; Quick match prefers a room with no match running.
  - All existing duel tests pass with 2 players (KOs, timer, `OVER`, `LEFT`), adapted to `BEGIN`.
  - Royale:
    - `ATK` only reaches living opponents, never the sender.
    - `SNAP` reaches every other participant with the sender's ID.
    - `DEAD` gives `OUT` with the correct place.
    - A disconnect counts as an elimination.
    - The last player alive gets `OUT … 1`, and the room returns to between matches.
    - Repeated `DEAD` and traffic from eliminated players or non-participants are dropped.
    - Server-only verbs from clients are dropped.
- **Protocol (doctest):** round trips and exact wire format for every new or changed message. Malformed inputs: bad IDs, 0 or more than 16 roster or `START` entries, bad names, places out of range, `ROOMS` counts out of range.
- **Session (doctest):**
  - Waiting → Room on `ROOM`.
  - `begin()` gating.
  - `START` ignored when it doesn't list `myId`.
  - The mode is picked from the participant count.
  - Royale:
    - Elimination keeps the board and sends `DEAD` once.
    - `OUT` updates the opponent map.
    - `OUT <me> 1` gives Won; another player's `OUT … 1` gives Lost with this player's place.
  - Result → Countdown on the next `START`.
  - The robustness rules above.
  - The existing duel tests, adapted to the new join flow.
- **Bot (doctest):** the `LocalMatch` tests, adapted to `ROOM`/`PLAYERS`/`START` with IDs and to `BEGIN`.
- **Core:** `setLevelFloor` raises gravity, scoring and the reported level, and never lowers them.
- **Core, hole garbage:**
  - Rows from one attack share a hole column, and the next attack uses a different column.
  - A batch split by the per-lock cap keeps its column.
  - No bomb cells ever appear with `Holes`.
  - Filling the hole clears the row and counts as a garbage-cleared line.
  - An overflowing push is a top-out and leaves the board unchanged.
  - `Bombs` behaviour is unchanged, pinned by the existing tests.
- **Manual (no headless ImGui harness):**
  - 4–5 localhost tabs: lobby list counts, Quick match, room screen, host start, a late joiner, royale to the end, standings, starting the next match, the host leaving in the room and mid-match, and a phone-landscape grid.
  - A 2-tab duel and a bot practice match, to confirm both are unchanged apart from host start.

## 9. Docs
README "Playing a duel" is rewritten for rooms of up to 16, host start, the duel and royale modes, and royale's hole garbage. The earlier specs listed in Context get a "Partly superseded" pointer to this one.

## Decisions
- **One room model, rules by player count:** a 2-player match is the existing duel and 3–16 is royale, so there's one room flow and the 1v1 stays as it is.
- **Battle royale with one-life elimination** for 3+ players, rather than extending the KO count format.
- **Hole garbage in royale, bombs in the duel:** royale follows the common guideline style (one hole column per attack, a new column for each attack). The duel keeps its bomb garbage.
- **Random living target** per attack, chosen by the server, with no targeting controls.
- **No clock in royale**, with a gravity floor rising one level every 30 s instead.
- **Host-only start**, with host migration to the longest-present player, and back to the room after every match instead of a rematch vote. The bot practice is the one exception: it starts straight away.
- **Names stay as they are** (the existing Name field and `HELLO`), with room-unique suffixes for duplicates such as several `Player`s.
- **No snapshot throttling:** a `SNAP` is about 450 bytes, so a full 16-player room costs about 1.1 MB/s (around 9 Mbit/s) out of the server and about 70 KB/s into each browser. That's fine on a LAN or a typical VPS; throttling can come later if needed.
