# Design: Server-authoritative matches without input lag

## Context
The duel server (`server/hub.go`) is a relay and referee that trusts every client's account of its own game:

| Client sends | Server checks | Cheat it allows |
|---|---|---|
| `ATK n` | 1 ≤ n ≤ 40 | Send 40 lines as often as you like |
| `DEAD` | nothing; it is voluntary | Never send it and you can never be KO'd or eliminated |
| `SNAP …` | relayed unchanged | Show opponents a fake board |
| game clock | nothing | Tick the game slower than real time (slower gravity) |

This spec makes the server the authority on every match result. Each client keeps simulating its own game locally, so input stays instant. The server re-runs every player's game from their timestamped inputs, using the same compiled C++ core, and only what the server computes counts: attacks, KOs, eliminations, snapshots and results.

It builds on the LAN duel (`2026-10-13-lan-duel-design.md`), the KO duel (`2026-10-13-bomb-battle-design.md`), garbage delay (`2026-10-14-garbage-delay-cap-design.md`) and rooms and royale (`2026-10-14-multiplayer-rooms-design.md`). Where this spec contradicts those, this spec wins: clients no longer send `ATK`, `DEAD` or `SNAP` once enforcement is on.

The rooms and royale work (PR #16) has merged: rooms of 2–16 players, host `BEGIN`, a royale client (`DuelMode::Royale`, `OpponentView`, standings) and per-player outbound queues in the hub. This spec is written against that code. Two of its client-side behaviors change here:

- **Royale gravity floor.** `DuelSession::update` sets `startLevel + playedMs_ / kRoyaleLevelStepMs` once per frame, so a level-up lands at a frame boundary. It moves into `MatchGame` and is applied every millisecond (section 4).
- **Top-outs, KO and elimination alike.** These are detected at the end of the frame. They also move into `MatchGame` and are detected at the exact millisecond.

## Goals and non-goals
- **Goals:**
  - Every accepted attack, KO, elimination and result comes from a legal input sequence replayed on the server.
  - No added latency for a player's own inputs. Attacks reach opponents after one network trip, as today.
  - Time cheats are bounded: no running ahead, no backdated inputs, no indefinitely delayed garbage.
  - Opponents only ever see boards the server computed.
  - A log-only rollout phase before enforcement.
- **Non-goals:**
  - Detecting input-automation bots or macros. Legal inputs from a script look like a perfect human's; that needs statistical detection, which is a separate project.
  - Hiding future pieces. Every client still gets the match seed.
  - Solo modes, the practice bot and the desktop build. These stay offline and unchanged.
  - Lag compensation beyond the windows in section 3.
  - Accounts, bans or reporting.

## Decisions taken during brainstorming
- **Threat model:** full integrity (option B). Hidden piece queues (option C) were not chosen.
- **On disagreement:** the server is the truth and the client corrects itself (`RESYNC`). A player is never forfeited. A log-only server mode comes first.
- **Simulation:** the C++ core is compiled to WASM and run in Go with wazero. A Go port and cgo were both rejected.

## 1. Architecture and data flow

```
browser                                  server (per running match: 1 goroutine + 1 wazero instance)
-------                                  -----------------------------------------------------------
keys -> MatchGame (predicts, instant)
     -> IN <tEnd> <t>:<code>...  ----->  MatchGame(player) replays inputs at their t
                                         -> attack?  route ATK / GARB to target (existing rules)
                                         -> top-out? scoreKO / eliminate (existing code)
                                         -> every 100 ms: SNAP <id> from the server's own board
GARB <id> <lines>            <-----
acceptGarbage -> logs G<id>  ----->      applies G<id> at the reported t, if inside the window
RESYNC <t> <log>             <-----      only when the server rejected or forced something
```

`MatchGame` (section 4) is one C++ class compiled into both the browser client and the server's WASM module, so the match rules exist in exactly one place.

## 2. Protocol

All times `t` are integer milliseconds of match time, counted from the end of the countdown. This is the clock `MatchGame::advance` moves forward one millisecond at a time.

### Client → server
- `SDF <n>`: the player's soft-drop factor (`0` = instant, otherwise `kMinSdf`–`kMaxSdf`, 5–40). Sent once between `START` and the first `IN`. If it's missing, the server assumes `kDefaultSoftDropFactor`. Any other value is a bad message.
- `IN <tEnd> [<t>:<code> ...]`: the client's game has reached `tEnd`, and these events happened at these times.
  - Events are in order, with `t` never decreasing. Each `t` must be ≥ the previous message's `tEnd` and ≤ this message's `tEnd`.
  - `tEnd` never decreases.
  - A message carries at most 64 events.
  - **Codes:**
    - `L R CW CCW HD H LW RW`: the `Action`s Left, Right, RotateCW, RotateCCW, HardDrop, Hold, LeftWall, RightWall. `Pause` is never sent.
    - `S+` / `S-`: soft drop pressed / released.
    - `G<id>x<lines>`: garbage batch `id` of `lines` lines accepted. The client's `lines` value is ignored; the server uses the amount it sent. The count is on the wire so that the same token can appear in a `RESYNC`, where the client needs it to replay.
  - **When it's sent:** at the end of every frame that produced events, and otherwise every 100 ms as a heartbeat.

### Server → client
- `GARB <id> <lines>`: incoming garbage, replacing `ATK` toward the receiver in both modes.
  - `id` counts up from 1 within the match, per receiver.
  - The garbage's look needs no data on the wire. Bomb columns (duel) and hole columns (royale) come from the receiver's seeded `garbageRng_`, which is identical on both sides.
- `RESYNC <t> [<t>:<code> ...]`: the canonical log of the receiver's own events from the start of the match, valid up to `t`.
- `SNAP`, `KO`, `OUT`, `OVER`, `PLAYERS`, `START` and `LEFT` are unchanged. `SNAP` now comes from the server's simulation.

### Server modes (`-authority`)
- **`log`** (default at first):
  - Clients send `IN` and also the old `ATK`, `DEAD` and `SNAP`. The server relays the old messages exactly as today.
  - In parallel, the server simulates every player and logs each disagreement.
  - It never sends `RESYNC`.
  - Garbage still goes to whoever the old routing picks, in the amount the sender claimed, but it is sent as `GARB <id> <lines>` rather than `ATK`. That way the receiver's acceptance time is logged, and the server's simulation receives the same batches the client did.
  - When the match ends, the server compares each player's claimed totals (lines sent through `ATK`, top-outs through `DEAD`) with its simulation's totals. It logs one summary line per player, plus the first `would-resync:<rule>` of each rule as it happens. Comparing totals at the end avoids a race: an `ATK` handled under the hub lock can arrive before the matching `IN` has been simulated.
  - The web client is served by this same server (`server/web.go`), so the client and server always ship together. "Release 1" and "Release 2" below are server deploys, not separate client rollouts.
- **`enforce`:**
  - The server ignores `ATK`, `DEAD` and `SNAP` from clients.
  - Attacks, KOs, eliminations and snapshots come from its own simulation.
  - Garbage goes out as `GARB`.
  - It sends `RESYNC` when a rule in section 3 is broken.

## 3. Timing rules

The server notes `wall0` = the time it sent `START` + `countdown`. A player's allowed range is checked against `wall = now − wall0` in ms. Every limit is a server flag, and in `log` mode each one only logs.

1. **No running ahead** (`-lead`, default 250 ms). An `IN` with `tEnd > wall + lead` is rejected.
2. **No rewriting the past** (`-lag`, default 1000 ms).
   - The server keeps each player's game at least at the horizon `wall − lag`. If the player's reported `tEnd` falls behind it, the server advances that game to the horizon with no inputs, so gravity keeps acting.
   - Any event later stamped before the server's current `t` for that player is dropped.
   - An honest hidden tab is unaffected. The client's catch-up (`frameBudgetMs`) replays the stall with no inputs, which is exactly what the server did, so the boards agree.
3. **Garbage can't be put off** (`-garbage-window`, default 750 ms).
   - When the server sends `GARB id`, it stores `T0` = that player's current server-side `t`.
   - `G<id>` must arrive with `T0 ≤ t ≤ T0 + window`.
   - If the server's `t` for that player passes `T0 + window` without it, the server applies the batch at `T0 + window` itself.
   - A `G<id>` stamped too early, or for an unknown or already-applied id, is dropped.
4. **Message shape.** A bad `IN` (it doesn't parse, has more than 64 events, a decreasing `t`, or an event outside `[previous tEnd, tEnd]`) is dropped whole.
5. **When a correction is sent.** In `enforce` mode, a rejected message or event, or forced garbage, marks the player as needing a `RESYNC`. A forced advance on its own does not: the client replays the same no-input stall, so the boards still agree.
   - A pending `RESYNC` goes out on the first 100 ms tick after that player's next `IN`, so a hidden tab that sends nothing receives nothing until it wakes.
   - At most one `RESYNC` per player per 100 ms. A single `RESYNC` covers everything that happened in that interval.
   - A `GARB` whose id the client already has in its log, because it arrived in a `RESYNC`, is ignored by the client.
6. **Abuse limit** (`-max-resyncs`, default 10 per 10 s, `enforce` only). A player over the limit is disconnected through the existing `leave()` path, so a malicious client can't burn server CPU with endless resyncs.

The server alone decides what goes in the canonical log. Accepted client events are added as they are, and forced events are added at the time the server applied them.

## 4. `MatchGame`: the shared match rules (C++)

New files: `src/net/match_game.{hpp,cpp}`. They're compiled into the client's `tetris_duel` library and into `jestris_sim.wasm`. `kMatchMs` and `kKoFlashMs` move from `src/app/duel.hpp` to `match_game.hpp`, so the server's module gets the same values. `duel.hpp` includes the header.

`LogCode` and `LogEntry` live in `src/net/protocol.hpp`, so the protocol can encode them without including `match_game.hpp`.

```cpp
// protocol.hpp
enum class LogCode : std::uint8_t { Left, Right, RotateCW, RotateCCW, HardDrop, Hold, LeftWall, RightWall,
                                    SoftOn, SoftOff, Garbage };
struct LogEntry { int t = 0; LogCode code = LogCode::Left; int garbageId = 0; int garbageLines = 0; };

// match_game.hpp
struct MatchSetup { StartMatch start; int softDropFactor = kDefaultSoftDropFactor; bool royale() const; };  // 3+ players

class MatchGame {
public:
    explicit MatchGame(const MatchSetup& setup);
    void input(Action action);                 // apply now, log at t()
    void softDrop(bool on);                    // logs only a change
    void acceptGarbage(int id, int lines);     // Game::receiveGarbage now, log G<id> at t()
    void advance(int ms);                      // 1 ms at a time, see rules below
    int t() const;
    bool frozen() const;                       // KO flash, time-up or eliminated
    bool eliminated() const;                   // royale
    int topOuts() const;                       // KOs (duel) or 1 once eliminated (royale), predicted locally
    int koFlashMs() const;                     // KO flash left, for display
    bool hasGarbage(int id) const;             // batch id already in the log
    std::vector<LogEntry> takeUnsent();        // entries not yet sent in an IN
    const std::vector<LogEntry>& log() const;
    const MatchSetup& setup() const;
    const Game& game() const; Game& game();
    std::uint64_t hash() const;                // see section 5
    static MatchGame replay(const MatchSetup& setup, const std::vector<LogEntry>& log, int upTo);
};
```

**Rules applied in each 1 ms step of `advance`, in this order:**
1. **Gravity floor (royale only):** `setLevelFloor(startLevel + t / kRoyaleLevelStepMs)`, the rule from the rooms spec. `kRoyaleLevelStepMs` moves to `match_game.hpp` along with `kMatchMs` and `kKoFlashMs`. Royale has no clock and no KO flash, so rule 4 and the duel freeze never apply to it.
2. **Tick:** if not frozen, `game.setSoftDrop(…)`, `setSoftDropFactor(sdf)`, then `tick(1)`.
3. **Top-out:**
   - **Duel:** `GameState::GameOver` means `resetBoard()` immediately and freeze until `t + 1 + kKoFlashMs`, so exactly the next `kKoFlashMs` steps don't tick.
   - **Royale:** `GameOver` means eliminated, frozen for good. The eliminated player's board stays in its game-over state, as `DuelSession` does today.
   - **Simultaneous royale top-outs:** the server decides the places. It handles each player's `IN` in the order the messages arrive, so a game that tops out later in server processing order can still be the last one alive and win. The client already handles `OUT <me> 1` after predicting its own elimination (`duel.cpp`: "survived a simultaneous top-out"), and that stays as is. Once the match is over, the server stops simulating an eliminated player's game and ignores any `IN` from them.
4. **Time-up (duel):** when `t + 1 == kMatchMs` (after exactly `kMatchMs` ticks), finish any clear still animating (`while Clearing: tick(1)`), then freeze for good.
5. `t++`.

This replaces the top-out handling at the end of the frame in `DuelSession::update`. That handling made the KO's exact millisecond depend on how frames were split, which the server can't reproduce.

**How the log is replayed:** events are applied before the tick at their `t`, in log order. A `Garbage` entry calls `receiveGarbage(lines)`. The client calls `input`, `softDrop` and `acceptGarbage` between `advance(1)` steps, which is exactly when `app.cpp step()` applies them today, so live play and replay match step for step.

## 5. Server simulation (Go + WASM)

### The `jestris_sim.wasm` module
- A new CMake target, emscripten only: `src/core` + `src/net/protocol.cpp` (`snapshotOf`, `encode`) + `src/net/match_game.cpp` + `src/sim/sim_api.cpp`.
- **Flags:** `-sSTANDALONE_WASM --no-entry -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=262144` (256 KB, or the smallest emscripten accepts), the same optimization level as the web client, and no JS glue.
- **C interface (`extern "C"`, handles index a table inside the module):**
  ```
  int  sim_new(u32 seed, int level, int players, int sdf)  void sim_free(int h)   // players: StartMatch ids 1..players
  void sim_input(int h, int code)                          void sim_accept_garbage(int h, int id, int lines)
  void sim_advance(int h, int ms)                          int  sim_t(int h)
  int  sim_take_attack(int h)                              int  sim_top_outs(int h)   // MatchGame::topOuts(), cumulative
  int  sim_eliminated(int h)                               u64  sim_hash(int h)
  int  sim_snapshot(int h)                                 // writes the encoded SNAP arguments into sim_buffer(); returns length
  char* sim_buffer()                                       // a static 4096-byte buffer the host reads from
  ```
- **`sim_hash`:** FNV-1a over the board cells, active piece, hold, the next 5, pending garbage batches (lines, msLeft, holeX), score, lines, level, combo, back-to-back, and the drop, lock and clear timers. It's used for logs and tests only, never sent on the wire.
- **Native builds:** `tetris_core` and `match_game.cpp` compile with `-ffp-contract=off` on every target. Clang on arm64 (Apple Silicon) otherwise fuses a multiply and an add into one instruction, which rounds differently, so the native fixtures would not match the WASM build.
- **Why it matches the browser:** WASM floating-point arithmetic is fully specified and the format has no fused multiply-add, so the gravity timer (`double`) computes the same in the browser and on the server. Both are built from the same source with the same emscripten version.
- **Packaging:**
  - `scripts/build.sh` and the Dockerfile build the module before `go test` and `go build`, in an emscripten stage, and copy it to `server/sim/jestris_sim.wasm`.
  - The file is git-ignored and never committed.
  - `server/sim` embeds it with `//go:embed`.

### `server/sim` package
- wazero `Runtime` + `CompileModule` once at startup.
- `sim.NewMatch()` instantiates the module for a match. It has methods wrapping each export, and `Close()`.
- A WASM trap or out-of-memory error comes back as a Go error, never a panic.

### Hub changes
- **A goroutine per match.** `begin()` creates a `matchRunner` for the match, which owns:
  - the `sim.Match` instance;
  - one handle per participant;
  - per player: the server-side `t`, canonical log, pending garbage windows, last `RESYNC` time and resync count;
  - a 100 ms ticker for the snapshot and horizon checks.
- **`route()`:** in a match, `IN` and `SDF` are pushed onto the runner's channel without simulating under `Hub.mu`. A full channel (capacity 64 per player) drops the message, which rule 4 then treats like any bad message.
- **The runner:**
  1. steps the simulation;
  2. collects outputs (attack lines with sender, top-outs, snapshots, `GARB`, `RESYNC`);
  3. takes `Hub.mu` briefly to apply them through the existing `scoreKO`, `eliminate`, `finish` and royale target selection (`pickTarget`) and to update `sent`, then `enqueue()`s every message before unlocking. These are main's per-player queues, so each recipient gets messages in the order the hub produced them, even when they come from different match runners.
- **Queue pressure:** a full 16-player room already sends each player about 150 `SNAP`s a second (`sendQueueLength = 1024`, kick on overflow). The server produces a player's `SNAP` only when it differs from that player's last one, which keeps traffic at or below today's. A `RESYNC` counts as one queued message.
- **Shutdown:** `endMatch()` stops the runner, which closes the instance.
- **A failed simulation:** the server logs `room, seed, player, err` and ends the match for everyone with `LEFT`, as when a player leaves. It never falls back to trusting clients.

### Budget (a full 16-player room is the worst case)
- **Memory:** about 256 KB per running match instance. Even with the 500-room cap all running matches, the total is about 128 MB.
- **CPU:**
  - The target is under 20 ms to simulate one 2-minute game at about 3 pieces per second.
  - If the benchmark misses that, the fix is inside the core: `Game::tick(ms)` jumps to the next timer event instead of looping per millisecond. That's a separate change, and this design doesn't depend on it.

## 6. Client changes

- **`DuelSession`:**
  - Holds a `std::optional<MatchGame>` instead of a `Game`.
  - `game()` keeps returning the inner `Game` for rendering. A new `match()` exposes the `MatchGame`.
  - `startMatch()` builds `MatchSetup` from `START`, the room mode and the current SDF, and sends `SDF`.
- **`app.cpp step()`:** in a duel, inputs go through `duel.match()->input` and `softDrop`, and each millisecond is `match->advance(1)`. Solo and vs Bot are unchanged. `activeGame()` uses `match.frozen()` for duels.
- **`DuelSession::update()`:**
  - Sends `IN` from `takeUnsent()` (or a heartbeat every 100 ms), and in `log` mode also the old messages.
  - Counts local `Sent` from `game().takeOutgoingAttack()` for sound and the live tally.
  - Shows the KO flash when `match.kos()` rises.
  - `KO`, `OUT` and `OVER` from the server stay final.
- **On `GARB id n`:** `match->acceptGarbage(id, n)`, the `Attacked` event, and add to `sentTheirs`.
- **On `RESYNC t log`:**
  1. `own = match->log()` entries with `t' > t`.
  2. `match = MatchGame::replay(setup, log, t)`.
  3. Re-apply `own` at their times while advancing to the previous local `t`.
  4. Mark everything up to there as sent. The server already has `own`, or will, because those messages are in flight.
  5. Log one line to the console with both hashes.

  The board snaps to the result. In the common honest cases, a hidden tab and a slow garbage acknowledgement, the replay gives the same board, so nothing changes on screen.

### The offline bot
`LocalMatch` (vs Bot) is unchanged. It ignores `IN` and `SDF`, and it keeps sending `ATK`, which the client treats as a garbage batch with id 0. Those batches are logged locally and never reach a server. Release 2's cleanup must keep `ATK` working for `LocalMatch`, or move the bot to `GARB`.

### `LEFT` in a royale
A simulation failure ends a royale with `LEFT` too. Today the client ignores `LEFT` outside a duel. It now ends a royale with `DuelOutcome::OpponentLeft`, and `resultReasonText` shows `Match ended` for a royale.

## 7. Rollout
1. **Release 1:** clients send `IN` + `SDF` alongside the old messages and understand `GARB` and `RESYNC`. The server runs `-authority=log`.
2. **Watch the logs** for `attack-mismatch`, `ko-mismatch` and `would-resync:<rule>`. Honest play must show none. Fix bugs or adjust limits until it does.
3. **Switch the server to `-authority=enforce`.**
4. **Release 2:** clients stop sending `ATK`, `DEAD` and `SNAP`. The server drops log mode and the relay code for them.

## 8. Testing
- **C++ (ctest), `MatchGame`:**
  - the KO reset and freeze at the exact millisecond;
  - royale elimination and the per-millisecond gravity floor (a level-up at the same `t` regardless of how frames are split);
  - the time-up rule;
  - the gravity floor;
  - `acceptGarbage`;
  - `replay(log)` gives the same `hash()` as live play when frames are split as 1 ms, 16 ms, or one 130 s catch-up frame. This proves the frame-boundary fix.
- **Golden fixtures:** a C++ test writes `server/sim/testdata/*.txt` (setup, log, expected attack, top-outs and hash at checkpoints) for a few seeds in both modes. These are checked in. The Go tests replay them through `jestris_sim.wasm` and must match exactly.
- **Go `server/sim`:** each export, a trap being turned into an error, and `Close`.
- **Go `hub_test.go`** (fake timers through `afterFunc`, plus an injectable clock for `wall`):
  - rule 1 (running ahead);
  - rule 2 (the forced advance and dropping backdated events);
  - rule 3 (in-window, late and early `G<id>`);
  - rule 4 (bad `IN`);
  - the `RESYNC` rate and abuse limits;
  - attack routing in duel and royale from the simulation;
  - in `enforce`, forged `ATK`, `DEAD` and `SNAP` being ignored;
  - in `log`, relayed traffic being byte-for-byte what it is today;
  - a simulation error ending the match.
- **Benchmarks:** `BenchmarkTwoMinuteGame` (target under 20 ms) and a memory check for a full 16-player room.
