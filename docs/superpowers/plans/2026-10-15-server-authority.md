# Server Authority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The server re-runs every player's game from timestamped inputs and is the only authority on attacks, KOs, eliminations, snapshots and results. The client keeps predicting locally, so input stays instant.

**Architecture:**
- A new C++ `MatchGame` holds the match rules: KO reset and flash, royale elimination, gravity floor, time-up, and an event log. It steps 1 ms at a time.
- The browser's `DuelSession` plays through it and streams `IN` batches.
- The same `MatchGame` is compiled into `jestris_sim.wasm`, which the Go server runs with wazero. Each running match has its own goroutine (`matchRunner`).
- The runner applies the timing rules and routes `GARB`, KOs and eliminations through the existing hub referee code.
- `-authority=log` only compares and logs; `-authority=enforce` makes the server's simulation final.

**Tech Stack:** C++20, doctest, emscripten (`emcc`), Go 1.27 with `github.com/coder/websocket` and `github.com/tetratelabs/wazero` (pure Go, keeps `CGO_ENABLED=0`).

**Spec:** `docs/superpowers/specs/2026-10-14-server-authority-design.md`

**Scope:** spec section 7, Release 1 plus the `enforce` switch. Release 2 (clients stop sending `ATK`, `DEAD` and `SNAP`; the server drops log mode) is a later plan that follows the log review.

## Global Constraints

- **Match time `t`:** integer ms from the end of the countdown. `MatchGame::advance` moves it 1 ms per step.
- **Wire tokens, exactly:** `L R CW CCW HD H LW RW S+ S- G<id>x<lines>`. An entry is `<t>:<token>`.
- **Messages:** `IN <tEnd> [entries]`, `SDF <n>`, `GARB <id> <lines>`, `RESYNC <t> [entries]`. `IN` carries at most 64 entries (`kMaxInputEvents` in `src/net/protocol.hpp`, `maxInputEvents` in `server/hub.go`, kept in sync with a comment as for `kMaxNameLength`).
- **SDF:** `0` (instant) or 5–40 (`kMinSdf`–`kMaxSdf`). Missing means 20 (`kDefaultSoftDropFactor`).
- **Constants** in `src/net/match_game.hpp`, moved there from `src/app/duel.hpp`: `kMatchMs = 120000`, `kKoFlashMs = 1000`, `kRoyaleLevelStepMs = 30000`.
- **Server flags and defaults:** `-authority log|enforce` (default `log`), `-lead 250ms`, `-lag 1s`, `-garbage-window 750ms`, `-max-resyncs 10` (per 10 s, enforce only). Snapshot and runner tick interval: 100 ms. Runner channel capacity: 64 per participant.
- **Floating point:** `tetris_core`, `tetris_duel` and the sim target compile with `-ffp-contract=off` on every platform.
- **`jestris_sim.wasm`:** git-ignored, never committed, built before `go test` and `go build`. `server/sim/testdata/*.txt` fixtures *are* committed.
- **Unchanged:** solo modes, vs Bot (`LocalMatch`), the lobby and rooms, and wire messages not listed above. Every existing test passes except those a task names as adapted.

## Verification Commands

- **C++:** `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`. A single suite: `./build/tetris_tests -tc="match_game:*"`.
- **WASM module:** `emcmake cmake -S . -B build-sim -DCMAKE_BUILD_TYPE=Release -DTETRIS_SIM_ONLY=ON && cmake --build build-sim --target jestris_sim && cp build-sim/jestris_sim.wasm server/sim/`.
- **Go:** `go -C server test -race ./...`. A single test: `go -C server test -race -run TestName ./...`. Benchmarks: `go -C server test -run '^$' -bench . ./sim`.

## Review Focus

1. **Hidden tab for 30 s, then back.** The player must not be kicked. At most a couple of `RESYNC`s are sent after waking, and none while hidden. Pinned in Task 9, `TestHiddenTabIsNotKicked`.
2. **Garbage arrives during the duel KO flash or after a royale elimination.** It must be logged and accepted the same way on both sides. Accepting while eliminated is a no-op in `Game`, but it is still logged. Pinned in Task 2, "match_game: garbage accepted while frozen replays identically".
3. **`RESYNC` racing in-flight `IN`s.** The client re-applies its own events after `t`, and a later `GARB` for an id already in the `RESYNC` log is ignored. Pinned in Task 4, "duel: RESYNC keeps local events after t and ignores a GARB it already has".
4. **A match ends while the runner is mid-step.** The runner's outputs must not touch a newer match in the same room. Pinned in Task 8, `TestRunnerOutputsForEndedMatchAreDropped`.
5. **A player leaves mid-match.** The runner stops processing their messages, their royale elimination happens once through `leave()`, and the forced clock advance must not raise a second elimination. Pinned in Task 9, `TestLeaverIsNotEliminatedTwice`.

---

### Task 1: Deterministic core and state hash

**Files:**
- Modify: `CMakeLists.txt` (`target_compile_options` of `tetris_core` and `tetris_duel`: add `-ffp-contract=off`)
- Modify: `src/core/game.hpp`, `src/core/game.cpp`
- Test: `tests/game_test.cpp`

**Interfaces:**
- Produces: `std::uint64_t Game::stateHash() const;` — FNV-1a 64 over:
  - every board cell (empty, or type + 1, including bomb flags if the cell type has them);
  - active piece type, x, y, rotation;
  - hold (or none) and `canHold_`;
  - the next 5 pieces;
  - each `incoming_` batch (lines, msLeft, holeX);
  - `lastHoleX_`, `outgoingAttack_`, score, lines, level, `levelFloor_`, combo, back-to-back, state;
  - `std::bit_cast<std::uint64_t>(dropTimerMs_)`, lock timer, lock resets, lowest y, clear timer.

  Field order is fixed as listed. Hash integers as their little-endian bytes.

- [ ] **Step 1: Write the failing tests** in `tests/game_test.cpp`:
  - `"game: equal seeds and inputs give equal state hashes"`: two `Game(42, 1)`; apply `Left, RotateCW, HardDrop` and `tick(500)` to both; `CHECK(a.stateHash() == b.stateHash())`.
  - `"game: the state hash changes with a move, a tick and garbage"`: take a hash, then after each of `apply(Action::Left)`, `tick(1)`, `receiveGarbage(1)`, `CHECK` that it differs from the previous hash.
- [ ] **Step 2: Run** `./build/tetris_tests -tc="game: *state hash*"`. Expected: compile error, because `stateHash` doesn't exist yet.
- [ ] **Step 3: Implement** `stateHash` as specified, and add `-ffp-contract=off` to both libraries.
- [ ] **Step 4: Run** the full C++ suite. Expected: all pass.
- [ ] **Step 5: Commit** `feat(core): state hash for replay checks; no FP contraction`.

### Task 2: `MatchGame` — shared match rules

**Files:**
- Create: `src/net/match_game.hpp`, `src/net/match_game.cpp` (added to `tetris_duel`)
- Modify: `src/net/protocol.hpp` (`LogCode`, `LogEntry` live here, see below), `src/app/duel.hpp` (remove the three moved constants and include `net/match_game.hpp`)
- Test: `tests/match_game_test.cpp` (add to `tetris_tests`)

**Interfaces:**
- Produces, in `src/net/protocol.hpp` (so the protocol can encode them without including `match_game.hpp`):
  ```cpp
  enum class LogCode : std::uint8_t { Left, Right, RotateCW, RotateCCW, HardDrop, Hold, LeftWall, RightWall,
                                      SoftOn, SoftOff, Garbage };
  struct LogEntry { int t = 0; LogCode code = LogCode::Left; int garbageId = 0; int garbageLines = 0;
                    friend bool operator==(const LogEntry&, const LogEntry&) = default; };
  ```
- Produces, in `src/net/match_game.hpp`:
  ```cpp
  inline constexpr int kMatchMs = 120000;
  inline constexpr int kKoFlashMs = 1000;
  inline constexpr int kRoyaleLevelStepMs = 30000;
  struct MatchSetup { StartMatch start; int softDropFactor = kDefaultSoftDropFactor; bool royale() const; };  // royale: 3+ players
  class MatchGame {
  public:
      explicit MatchGame(const MatchSetup& setup);  // Game built like makeDuelGame (hole garbage in a royale)
      void input(Action action);                    // Pause is ignored and not logged
      void softDrop(bool on);                       // logs SoftOn/SoftOff only on a change
      void acceptGarbage(int id, int lines);        // receiveGarbage(lines) and log Garbage{id, lines}
      void advance(int ms);
      int t() const;
      bool frozen() const;                          // in the KO flash, at/after time-up (duel), or eliminated
      bool eliminated() const;
      int topOuts() const;                          // KOs (duel) or 1 once eliminated (royale)
      int koFlashMs() const;                        // ms of KO flash left, for display
      bool hasGarbage(int id) const;                // id already logged
      std::vector<LogEntry> takeUnsent();
      const std::vector<LogEntry>& log() const;
      const MatchSetup& setup() const;
      Game& game(); const Game& game() const;
      std::uint64_t hash() const;                   // game().stateHash() mixed with t, frozen-until, topOuts
      static MatchGame replay(const MatchSetup& setup, const std::vector<LogEntry>& log, int upTo);
  };
  ```
- **The rules for one step of `advance`:**
  1. Royale: `setLevelFloor(startLevel + t / kRoyaleLevelStepMs)`.
  2. If not frozen: `setSoftDrop`, `setSoftDropFactor(sdf)`, `tick(1)`.
  3. Top-out: in a duel, `resetBoard()` and freeze until `t + 1 + kKoFlashMs`, so the next `kKoFlashMs` steps don't tick; in a royale, eliminated.
  4. Duel: when `t + 1 == kMatchMs`, run `while (state == Clearing) tick(1)` and freeze for good.
  5. `t++`.
- **`replay`:** applies each entry before the step at its `t`, in log order, and stops at `upTo`. `input`, `softDrop` and `acceptGarbage` stamp the current `t()`. `takeUnsent` returns entries logged since the last call.

- [ ] **Step 1: Write the failing tests** in `tests/match_game_test.cpp`. Use `StartMatch{seed, 1, {1, 2}}` for a duel and `{1, 2, 3}` for a royale. A helper `topOut(MatchGame&)` hard-drops until `topOuts()` rises, calling `advance(1)` after each `input(HardDrop)`.
  - `"match_game: a duel top-out resets the board and freezes exactly kKoFlashMs"`: after `topOut`, `t0 = t()`. `frozen()` stays true through `advance` until `t() == t0 + kKoFlashMs`, then becomes false. The board is empty and `topOuts() == 1`.
  - `"match_game: a royale top-out eliminates for good"`: `eliminated()`; `advance(60000)` keeps `frozen()` and leaves `game().state() == GameOver`.
  - `"match_game: the royale floor rises at exactly 30 s"`: at `t() == 29999` the level is 1; after `advance(1)` it is 2. The same holds when reaching it by `advance(29999)` + `advance(1)` and by 16 ms chunks.
  - `"match_game: time-up settles a clear and freezes"`: build a line clear that starts at `kMatchMs - 10` (use `boardWithWell` via a setup on `game()`, then hard-drop an I). After `advance(20)`, `game().state() != Clearing`, `frozen()`, and `game().takeOutgoingAttack() > 0`.
  - `"match_game: input and garbage are logged at the current t, soft drop only on change"`: check the exact `log()` vector, and that `takeUnsent()` returns it once and is then empty.
  - `"match_game: replay matches live play however frames are split"`: script 3000 random events over 120 s from `std::mt19937(7)` (actions; soft drop on/off; garbage every ~5 s). Apply the script live with `advance` split three ways: 1 ms steps, 16 ms frames with events applied at frame boundaries (events then logged at the boundary `t`), and one `advance(130000)` with events applied before it. For each live run, `replay(setup, live.log(), live.t()).hash() == live.hash()`. Run this for both a duel and a royale.
  - `"match_game: garbage accepted while frozen replays identically"`: accept garbage during a KO flash and after a royale elimination. Replay gives the same hash.
- [ ] **Step 2: Run** `./build/tetris_tests -tc="match_game:*"`. Expected: compile failure.
- [ ] **Step 3: Implement** `MatchGame` per the interface and rules. Move the three constants, and leave `duel.hpp`'s other constants where they are.
- [ ] **Step 4: Run** the full C++ suite. Expected: all pass.
- [ ] **Step 5: Commit** `feat(net): MatchGame steps match rules per millisecond with a replayable log`.

### Task 3: Protocol messages `IN`, `SDF`, `GARB`, `RESYNC`

**Files:**
- Modify: `src/net/protocol.hpp`, `src/net/protocol.cpp`
- Test: `tests/protocol_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  inline constexpr int kMaxInputEvents = 64;  // maxInputEvents in server/hub.go
  struct InputBatch { int tEnd = 0; std::vector<LogEntry> entries; };  // IN
  struct SoftDropSetting { int factor = kDefaultSoftDropFactor; };     // SDF
  struct GarbageIn { int id = 0; int lines = 0; };                     // GARB, from the server
  struct Resync { int t = 0; std::vector<LogEntry> entries; };         // RESYNC, from the server
  ```
  Each gets `operator==` and is appended to `Message`.
- **Decode rules:**
  - Integers are non-negative.
  - `IN`: at most `kMaxInputEvents` entries, entry `t` never decreasing, every entry `t ≤ tEnd`.
  - `RESYNC`: no size cap, `t` never decreasing.
  - `SDF`: `0` or 5–40.
  - `GARB`: `id ≥ 1`, lines 1–40.
  - Anything else is `nullopt`.

- [ ] **Step 1: Write the failing tests:**
  - `"protocol: IN round-trips every token"`: an `InputBatch` with one entry of each `LogCode` (garbage `{3, 2}`) encodes to exactly `"IN 900 10:L 11:R 12:CW 13:CCW 14:HD 15:H 16:LW 17:RW 18:S+ 19:S- 20:G3x2"` and decodes back.
  - `"protocol: SDF, GARB and RESYNC round-trip"`: `"SDF 0"`, `"SDF 40"`, `"GARB 5 4"`, `"RESYNC 120 3:HD 100:G1x2"`.
  - `"protocol: malformed input batches are rejected"`: 65 entries; `"IN 10 11:L"`; `"IN 10 5:L 4:R"`; `"IN 10 5:X"`; `"IN 10 5:G0x2"`; `"SDF 3"`; `"SDF 41"`; `"GARB 0 1"`; `"GARB 1 41"`; `"IN -1"`.
- [ ] **Step 2: Run** `./build/tetris_tests -tc="protocol:*"`. Expected: compile failure.
- [ ] **Step 3: Implement** the encoder and decoder, with a token table shared by `IN` and `RESYNC`.
- [ ] **Step 4: Run** the full C++ suite. Expected: pass.
- [ ] **Step 5: Commit** `feat(net): IN, SDF, GARB and RESYNC messages`.

### Task 4: Client plays through `MatchGame`

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`, `src/app/app.cpp` (`step()`, `activeGame()`), `src/app/render.cpp` only if it reads `koFlashMs`/`frozen` differently (it should keep reading `DuelSession`)
- Test: `tests/duel_test.cpp` (adapt and add), `tests/ui_test.cpp` (must still pass)

**Interfaces:**
- Consumes: `MatchGame`, `MatchSetup` (Task 2); `InputBatch`, `SoftDropSetting`, `GarbageIn`, `Resync` (Task 3).
- Produces:
  - `DuelSession(Send send, int softDropFactor = kDefaultSoftDropFactor)`. This replaces the `MakeGame` parameter; check `tests/duel_test.cpp` and `app.cpp` for callers. `void setSoftDropFactor(int)` takes effect at the next `START`.
  - `MatchGame* match()` and `const MatchGame* match() const` (null outside a match). `game()` keeps returning `&match()->game()`.
  - `void step(int ms)`: advances the match 1 ms at a time while `Playing`; a no-op otherwise.
  - `update(ms)` keeps running the countdown, clocks, events and sending.
  - `frozen()` becomes `phase_ == Playing && (match_->frozen() || matchClockMs_ == 0)` for a duel, and `match_->frozen()` for a royale.
  - `koFlashMs()` returns `match_->koFlashMs()`.
- **Behavior:**
  - **`startMatch`:** build `MatchSetup{start, sdf}` and send `SDF <sdf>` once.
  - **`update` while `Playing`:**
    1. Send `IN <t> <takeUnsent()>` if it has entries, or else if 100 ms of `t` have passed since the last `IN`.
    2. *After* `IN`, send the old messages:
       - `ATK n` from `takeOutgoingAttack` (keep the `Sent` event and tally);
       - the `SNAP` on its interval;
       - `DEAD` once per rise of `match_->topOuts()`.
    3. Push `KoMine` / KO flash from `topOuts()` rising in a duel; set `eliminated_` from `match_->eliminated()` in a royale.
  - Delete the frame-end top-out and `setLevelFloor` code from `update`; `MatchGame` owns both now.
  - **`GARB id n`** while `inMatch()`: if `match_->hasGarbage(id)`, ignore it; otherwise `acceptGarbage(id, n)`, the `Attacked` event, and `sentTheirs += n`.
  - **`ATK n`** (vs Bot only): `acceptGarbage(0, n)` plus the same event and tally. Ids of 0 are never checked by `hasGarbage`.
  - **`RESYNC t log`:**
    1. `own` = entries of `match_->log()` with `t' > t`.
    2. `now = match_->t()`.
    3. `match_ = MatchGame::replay(setup, log, t)`.
    4. Re-apply `own` at their times while advancing to `now`.
    5. Discard `takeUnsent()`.
    6. `std::fprintf(stderr, "resync t=%d hash %016llx -> %016llx\n", …)`.
  - **`LEFT` in a royale match:** `finish(DuelOutcome::OpponentLeft)`. `resultReasonText` returns `"Match ended"` for a royale `OpponentLeft`.
  - **`app.cpp step()`:** in a duel, use `match->input` and `match->softDrop` instead of `game->apply` and `setSoftDrop`, and call `duel.step(1)` per ms instead of `game->tick(1)`. Solo is unchanged. The SDF setting goes to `duel.setSoftDropFactor` whenever handling changes or a duel opens.

- [ ] **Step 1: Adapt the existing tests that drive the game directly** to drive it through `duel.match()->input(...)` and `duel.step(...)`:
  - `"topping out sends a final snapshot and DEAD…"`
  - `"the KO flash freezes…"`
  - `"royale: a top-out sends the final SNAP and DEAD once…"`
  - `"royale: no clock freeze and the level floor rises every 30 s"`
  - `"a clear still animating at 0:00…"`
  - every test using `MakeGame`

  Also rewrite `"royale: LEFT is ignored in a royale"` as `"royale: LEFT ends a royale as Match ended"`. Their assertions otherwise stay the same.
- [ ] **Step 2: Write the new failing tests:**
  - `"duel: START sends SDF with the session's factor"`: `setSoftDropFactor(0)`, then `START …`; `wire.sent` contains `"SDF 0"`.
  - `"duel: inputs go out as IN before ATK"`: hard-drop a line clear via `match()->input` and `step(1)`, then `update(1)`; the first `IN` precedes `ATK`, and it ends with `":HD"`.
  - `"duel: an idle player sends an IN heartbeat every 100 ms"`: `step(100)` + `update(100)` sends exactly `"IN 100"`.
  - `"duel: GARB is accepted once, logged as G<id>x<lines>"`: `GARB 1 3` twice → `pendingGarbage() == 3`, and the next `IN` contains `":G1x3"` once.
  - `"duel: RESYNC keeps local events after t and ignores a GARB it already has"`: play events at t=50 and t=150 to `t = 200`, then receive `RESYNC 100 50:HD 80:G1x2`. Afterwards `match()->t() == 200`, the log is `50:HD, 80:G1x2, 150:<own>`, and a following `GARB 1 2` leaves `pendingGarbage` unchanged.
  - `"duel: RESYNC to the same log leaves the board unchanged"`: the hash before equals the hash after.
- [ ] **Step 3: Run** `./build/tetris_tests -tc="duel*,royale*"`. Expected: the new tests fail; adapted tests fail to compile until the interface lands.
- [ ] **Step 4: Implement** the interface and behavior above.
- [ ] **Step 5: Run** the full C++ suite, including `tetris_ui_tests` and `local_match_test` (vs Bot must still play: it now receives `IN` and `SDF` and ignores them). Expected: all pass.
- [ ] **Step 6: Commit** `feat(app): duel client predicts through MatchGame and streams IN`.

### Task 5: Golden replay fixtures

**Files:**
- Create: `tests/match_game_fixture_test.cpp` (add to `tetris_tests`), `server/sim/testdata/duel-7.txt`, `duel-1234.txt`, `royale-7.txt`, `royale-99.txt`

**Interfaces:**
- Produces the fixture format, one directive per line, read by Task 7:
  ```
  setup <seed> <startLevel> <players> <sdf>
  ev <t> <token>                 # token as on the wire, e.g. HD, S+, G3x2
  check <t> <attackTotal> <topOuts> <hash-hex16>
  ```
  `check` lines come every 10000 ms and at the end. `attackTotal` is the sum of `takeOutgoingAttack()` drained at each check.
- **Generation:** the events are the scripted random run from Task 2, seeded by the fixture seed, over 120 s for a duel and until elimination or 120 s for a royale.

- [ ] **Step 1: Write the test** `"match_game fixtures: checked-in fixtures match the native replay"`. For each of the four files, generate the content in memory and `CHECK` that it equals the file's text, read from `TETRIS_SOURCE_DIR "/server/sim/testdata/…"`. If the environment variable `UPDATE_FIXTURES=1` is set, write the file instead. Pass the `TETRIS_SOURCE_DIR` define through CMake: `target_compile_definitions(tetris_tests PRIVATE TETRIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")`.
- [ ] **Step 2: Run** `UPDATE_FIXTURES=1 ./build/tetris_tests -tc="match_game fixtures*"`, then run it again without the variable. Expected: the second run passes, and `git status` shows the four new files.
- [ ] **Step 3: Commit** `test: golden MatchGame replay fixtures for the server`.

### Task 6: `jestris_sim.wasm` module and build plumbing

**Files:**
- Create: `src/sim/sim_api.cpp`
- Modify: `CMakeLists.txt`, `scripts/build.sh`, `Dockerfile`, `.gitignore` (add `server/sim/jestris_sim.wasm`)

**Interfaces:**
- Produces these `extern "C"` exports, each marked `__attribute__((export_name("…")))`. Handles index a `std::vector<std::optional<MatchGame>>` and are reused after `sim_free`.
  ```
  int  sim_new(uint32 seed, int level, int players, int sdf)   // StartMatch.players = {1..players}
  void sim_free(int h)
  void sim_input(int h, int code)            // LogCode value; Garbage is not accepted here
  void sim_accept_garbage(int h, int id, int lines)
  void sim_advance(int h, int ms)
  int  sim_t(int h)
  int  sim_take_attack(int h)                // game().takeOutgoingAttack()
  int  sim_top_outs(int h)                   // MatchGame::topOuts()
  int  sim_eliminated(int h)
  uint64 sim_hash(int h)
  int  sim_snapshot(int h)                   // writes encode(snapshotOf(game())) minus the leading "SNAP " into the buffer; returns its length
  char* sim_buffer()                         // a static 4096-byte buffer
  ```
- **Target `jestris_sim`:** built only when `EMSCRIPTEN` and `TETRIS_SIM_ONLY` are both set. `TETRIS_SIM_ONLY` skips SDL, imgui and the game targets, so this build needs no SDL. Sources: the `tetris_core` sources, `src/net/protocol.cpp`, `src/net/match_game.cpp`, `src/sim/sim_api.cpp`.
- **Link options:** `-O2 -sSTANDALONE_WASM --no-entry -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=262144 -sSTACK_SIZE=65536 -fno-exceptions`, compiled with `-ffp-contract=off`. The output is `jestris_sim.wasm`. Emscripten's reactor `_initialize` export must be called once by the host (Task 7).
- **`scripts/build.sh`:** before "Testing and building the server", build the module with the command from Verification Commands and copy it to `server/sim/`.
- **`Dockerfile`:** in the `web` stage, also run the sim build. In the `server` stage, `COPY --from=web /src/build-sim/jestris_sim.wasm ./sim/` before `go test`.

- [ ] **Step 1: Build** with the Verification Commands module line. Expected: `server/sim/jestris_sim.wasm` exists and is under 400 KB.
- [ ] **Step 2: Check the exports:** `wasm-objdump -x server/sim/jestris_sim.wasm | grep -c 'sim_'` (wabt ships with emsdk) prints 12, and `_initialize` is exported. The imports are only `wasi_snapshot_preview1.*`.
- [ ] **Step 3: Verify the native build is unaffected:** the C++ suite still passes.
- [ ] **Step 4: Commit** `build: jestris_sim.wasm target for the server`.

### Task 7: Go `server/sim` package on wazero

**Files:**
- Create: `server/sim/sim.go`, `server/sim/sim_test.go`
- Modify: `server/go.mod`, `server/go.sum` (`go get github.com/tetratelabs/wazero@latest`)

**Interfaces:**
- Consumes: the exports from Task 6 and the fixtures from Task 5.
- Produces:
  ```go
  package sim
  //go:embed jestris_sim.wasm
  var wasm []byte
  type Engine struct{ /* runtime + compiled module */ }
  func NewEngine(ctx context.Context) (*Engine, error)     // compiler runtime, WASI instantiated, CompileModule once
  func (e *Engine) NewMatch(ctx context.Context) (*Match, error) // anonymous instance (WithName("")), calls _initialize
  type Match struct{ /* module + export funcs */ }
  func (m *Match) New(seed uint32, level, players, sdf int) (int, error)
  func (m *Match) Input(h, code int) error
  func (m *Match) AcceptGarbage(h, id, lines int) error
  func (m *Match) Advance(h, ms int) error
  func (m *Match) T(h int) (int, error)
  func (m *Match) TakeAttack(h int) (int, error)
  func (m *Match) TopOuts(h int) (int, error)
  func (m *Match) Eliminated(h int) (bool, error)
  func (m *Match) Hash(h int) (uint64, error)
  func (m *Match) Snapshot(h int) (string, error)
  func (m *Match) Close() error
  // LogCode values, matching enum class LogCode in src/net/protocol.hpp.
  const (CodeLeft = iota; CodeRight; CodeRotateCW; CodeRotateCCW; CodeHardDrop; CodeHold; CodeLeftWall; CodeRightWall; CodeSoftOn; CodeSoftOff)
  func ParseToken(tok string) (code int, garbageID, garbageLines int, ok bool) // "HD" -> CodeHardDrop; "G3x2" -> -1, 3, 2
  ```
  A WASM trap is returned as an error, never a panic. After a trap, every method returns an error.

- [ ] **Step 1: Write the failing tests:**
  - `TestFixturesReplay`: for each `testdata/*.txt`, feed the `ev` lines through `Input` / `AcceptGarbage` with `Advance` between them, and at each `check` compare `T`, the summed `TakeAttack`, `TopOuts` and `Hash`. Any mismatch fails with the file and line.
  - `TestParseToken`: covers every token, plus a rejection of `"X"`, `"G0x1"` and `"G1x41"`.
  - `TestSnapshotTracksState`: the `Snapshot` of a fresh game is non-empty, has no leading `"SNAP "`, and is the same on two calls in a row. After `Input(CodeHardDrop)` + `Advance(h, 1)`, it differs.
  - `TestTrapBecomesError`: `Advance(99, 1)` on an unknown handle must trap (`sim_api` calls `__builtin_trap()` for a bad handle) and return an error; the next call also returns an error.
  - `TestTwoMatchesAreIndependent`: two `NewMatch` from one `Engine`; advancing one leaves the other's `Hash` unchanged.
  - `BenchmarkTwoMinuteGame`: a 120 s game with a hard drop every 333 ms. Report ns/op; the target is under 20 ms.
  - `BenchmarkRoomMemory`: 16 handles in one `Match`, reporting memory pages via `Memory().Size()`.
- [ ] **Step 2: Run** `go -C server test ./sim`. Expected: compile failure.
- [ ] **Step 3: Implement** the package.
- [ ] **Step 4: Run** `go -C server test -race ./...` and the benchmarks. Expected: pass. Record the benchmark numbers in the commit message. If the 2-minute game takes over 20 ms, note it and continue (the spec makes it a separate optimization).
- [ ] **Step 5: Commit** `feat(server): run the game core in wazero`.

### Task 8: Match runner in log mode

**Files:**
- Create: `server/authority.go`, `server/authority_test.go`
- Modify: `server/hub.go` (`Hub` fields; `begin`, `route`, `endMatch`, `leave`), `server/main.go` (flags)

**Interfaces:**
- Consumes: `sim.Match`, `sim.ParseToken` (Task 7).
- Produces:
  ```go
  type gameSim interface { // *sim.Match satisfies it; tests use a scripted fake
      New(seed uint32, level, players, sdf int) (int, error); Input(h, code int) error
      AcceptGarbage(h, id, lines int) error; Advance(h, ms int) error; T(h int) (int, error)
      TakeAttack(h int) (int, error); TopOuts(h int) (int, error); Eliminated(h int) (bool, error)
      Hash(h int) (uint64, error); Snapshot(h int) (string, error); Close() error
  }
  type authority struct {
      mode string // "log" | "enforce"
      lead, lag, garbageWindow time.Duration
      maxResyncs int // per 10 s
  }
  // New Hub fields: auth authority; newSim func() (gameSim, error); now func() time.Time; manualRunners bool
  type matchRunner struct { /* match *match; room *room; id uint64; sim gameSim; players map[*player]*playerSim;
                              in chan runnerMsg; stop chan struct{}; wall0 time.Time */ }
  func (r *matchRunner) drain(h *Hub)          // handle every queued runnerMsg, then apply effects
  func (r *matchRunner) tick(h *Hub, now time.Time)
  func (r *matchRunner) finish()               // log the summary, close the sim
  type runnerMsg struct { from *player; kind msgKind; text string; to *player; id, lines int }
  // kinds: msgIN, msgSDF, msgClaimedAttack (log mode: route() relayed ATK; runner sends GARB), msgClaimedDead, msgLeft
  ```
- **`playerSim`** holds:
  - `h`;
  - `t` (server-side match time);
  - `reportedEnd`;
  - `sdfSeen`;
  - `pending map[int]garbageWindow{lines, t0}`;
  - `nextGarbageID`;
  - `canonical []string` (entries as `t:token`);
  - `needResync`, `sawINSinceMark`, `resyncTimes []time.Time`;
  - `simAttack`, `simTopOuts`, `claimedSent`, `claimedDead`;
  - `wouldResync map[string]int`;
  - `lastSnap string`;
  - `left bool`.

  The sim game is created lazily at the first `IN` or `SDF`, with the SDF if seen (else 20). `t` starts at 0.
- **Algorithm — `handleIN(ps, text)`:**
  1. Parse `IN tEnd entries`. If it doesn't parse, has more than 64 entries, has a decreasing `t`, has an entry outside `[reportedEnd, tEnd]`, has `tEnd < reportedEnd`, or has `tEnd > wall+lead`: `mark("bad-in")` and return.
  2. For each entry:
     - If `e.t < ps.t`: `mark("backdated")` and continue.
     - Otherwise `advanceTo(ps, e.t)`.
     - Apply a garbage token if its id is pending and `e.t ≥ t0`; append the canonical `G<id>x<serverLines>`. Otherwise `mark("garbage-bad")`.
     - Apply any other token with `Input` and append it to canonical.
  3. `advanceTo(ps, tEnd)`, then `reportedEnd = tEnd` and `sawINSinceMark = true`.
- **Algorithm — `advanceTo(ps, target)`:**
  1. Take pending batches in `t0` order. For each batch with `t0+window ≤ target`: `Advance` to `t0+window`, `AcceptGarbage` (forced), append canonical, `mark("garbage-late")`.
  2. `Advance` to `target`.
  3. Collect `TakeAttack` into `simAttack` and the effects, and `TopOuts`/`Eliminated` into the effects.
- **`tick(now)`:**
  1. Run `advanceTo(ps, wallMs - lag)` for every live player whose `t` is behind. This is a forced advance and does not `mark`.
  2. Produce a `SNAP <id> <args>` effect when a snapshot changed (enforce only).
  3. Send `RESYNC <t> <canonical>` (enforce only) if `needResync && sawINSinceMark` and the last `RESYNC` was at least 100 ms ago; then clear both flags.
- **`mark(rule)`:** `needResync = true; sawINSinceMark = false; wouldResync[rule]++`. In log mode, `log.Printf("would-resync:%s room=%s player=%d t=%d", …)` the first time each rule happens for that player.
- **Effects** are applied in one `h.mu` section:
  - First check `room.match == r.match && room.matchID == r.id`; otherwise drop everything.
  - In log mode, claimed attacks become `GARB <id> <n>` to the target, using the existing duel/royale targeting (refactor `Hub.attack` into `validateAttack` + `routeGarbage(p, n) *player`). The target's `pending[id] = {n, target.t}`.
  - Enqueue through `enqueue()`.
- **In log mode, `route()`:**
  - `ATK`, `DEAD` and `SNAP` keep today's handling.
  - Additionally, `ATK` pushes `msgClaimedAttack` instead of enqueuing `ATK` itself, and `DEAD` pushes `msgClaimedDead`.
  - `IN` and `SDF` from a participant push `msgIN`/`msgSDF`. A full channel drops the message and the runner marks `bad-in` on its next drain (count the drops per player with an atomic counter).
  - A non-participant's `IN` is ignored.
- **`begin()`:** creates the runner (`h.newSim()`; on error log it and fall back to no runner, which is log mode only). In non-manual mode it starts `go r.loop(h)` (a 100 ms ticker calling `tick`, plus `drain` on each channel receive, until `stop`).
- **`endMatch()`:** `close(stop)` non-blocking and set `match.runner = nil`. The runner calls `finish()` itself, which logs `authority room=%s player=%d claimedSent=%d simAttack=%d claimedDead=%d simTopOuts=%d wouldResync=%v` per player.
- **`leave()`:** pushes `msgLeft`.
- **Flags** go in `main.go` per Global Constraints.

- [ ] **Step 1: Write the failing tests.** Use `directRoyale`-style direct hubs with `manualRunners = true`, an injectable `now`, and a fake `gameSim` that records calls and returns scripted attacks and top-outs per handle.
  - `TestLogModeRelaysOldTrafficUnchanged`: the existing ATK/DEAD/SNAP traffic is byte-for-byte the same, except the receiver gets `GARB 1 2` where it used to get `ATK 2`.
  - `TestINStepsSimToEachEventThenTEnd`: `IN 120 10:L 50:HD` gives the fake calls `Advance(h,10) Input(L) Advance(h,40) Input(HD) Advance(h,70)`.
  - `TestRunningAheadIsRejected`: `now = wall0 + 1s` and `IN 1300` gives no `Advance`, and the log has `would-resync:bad-in`.
  - `TestHorizonForcesAdvanceAndDropsBackdated`: `tick` at `wall0 + 3s` advances to 2000; a later `IN 2100 1500:L` drops `L`, marks `backdated` and advances to 2100.
  - `TestGarbageWindow`:
    - `G1x2` at `t0+100` is accepted;
    - one at `t0+800` was already forced at `t0+750` by `advanceTo`, so it is dropped as `garbage-bad`;
    - one at `t0-1` is dropped;
    - an unknown id is dropped.
  - `TestBadINShapes`: 65 entries; a decreasing `t`; an entry beyond `tEnd`; `tEnd` below the previous `tEnd`. All are dropped whole.
  - `TestSummaryLoggedAtMatchEnd`: claimed vs sim totals appear in the log line.
  - `TestRunnerOutputsForEndedMatchAreDropped`: queue an `IN`, end the match and `BEGIN` a new one, then `drain` the old runner. No messages reach any queue.
- [ ] **Step 2: Run** `go -C server test -race -run 'Test(LogMode|IN|Running|Horizon|Garbage|BadIN|Summary|Runner)' ./...`. Expected: fail.
- [ ] **Step 3: Implement** per the interfaces and algorithms above.
- [ ] **Step 4: Run** the full `go -C server test -race ./...`. Expected: all pass, including every existing hub test unchanged except the ones that expected `ATK` (now `GARB <id> <n>`). List those adaptations in the commit message.
- [ ] **Step 5: Commit** `feat(server): per-match runner simulates players in log mode`.

### Task 9: Enforce mode

**Files:**
- Modify: `server/authority.go`, `server/hub.go`
- Test: `server/authority_test.go`

**Interfaces:**
- Consumes: Task 8's runner, effects and `routeGarbage`.
- **Behavior when `auth.mode == "enforce"`:**
  - `route()` drops `ATK`, `DEAD` and `SNAP` from clients.
  - **Runner effects:**
    - Each `TakeAttack` n > 0: `m.sent[p] += n`, then `routeGarbage(p, n)` with `GARB`.
    - Each rise in `TopOuts`: in a duel `r.scoreKO(p)`; in a royale `r.eliminate(p)` plus a roster broadcast when the match ended, as `knockOut` does. Factor that out of `knockOut` as `topOut(p)`.
    - Snapshots and `RESYNC`s per Task 8's `tick`.
  - **Abuse:** a player whose `resyncTimes` within the last 10 s exceed `maxResyncs` is kicked via the existing `kick` channel (the same path as a queue overflow).
  - **A sim error** (any `gameSim` method returning an error): `log.Printf("sim error room=%s seed=%d player=%d: %v", …)`. Under the lock, if the match is current, send `LEFT` to every connected participant, `r.endMatch()`, and broadcast `PLAYERS`.
  - **An eliminated or left player:** the runner stops calling `advanceTo` for them (no forced advance after elimination or leaving).
- **Integration test:** with the real `sim.Engine`, a duel where player 1 sends `IN` batches of hard drops 20 ms apart reaches a KO. Both players get `KO 0 1` / `KO 1 0`, with no client `DEAD` sent.

- [ ] **Step 1: Write the failing tests:**
  - `TestEnforceIgnoresForgedAttackAndDead`: `ATK 40` and `DEAD` from a client change nothing (no `GARB`, `KO` or `OUT`).
  - `TestEnforceRoutesSimAttackAsGarb`: the fake returns attack 4 for player 1, so player 2 gets `GARB 1 4` and `m.sent` is 4. A royale variant with `pickTarget` checks the target.
  - `TestEnforceSimTopOutScoresKOAndEliminates`: covers both duel and royale, including the royale end and `PLAYERS`.
  - `TestEnforceSnapshotsFromSimOnlyWhenChanged`: two ticks with the same fake snapshot send one `SNAP 1 …`.
  - `TestResyncWaitsForNextINAndRateLimits`: a mark followed by a tick with no `IN` sends nothing; after an `IN`, the next tick sends one `RESYNC <t> …` containing the canonical entries; two marks within 100 ms produce one `RESYNC`.
  - `TestHiddenTabIsNotKicked`: no `IN` for 30 s of ticks while garbage arrives, then one `IN` with late `G` tokens. At most 2 `RESYNC`s, and the player is not kicked.
  - `TestAbuserIsKicked`: 11 bad `IN`s spaced 150 ms apart with ticks between them kick the player.
  - `TestSimErrorEndsMatch`: the fake's `Advance` returns an error, so every participant gets `LEFT` and the room returns to between matches.
  - `TestLeaverIsNotEliminatedTwice`: a royale player leaves; ticks for 5 s produce no second `OUT` for them.
  - `TestRealSimDuelKO`: the integration test above. It skips only if `sim.NewEngine` fails.
- [ ] **Step 2: Run** them. Expected: fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `go -C server test -race ./...`. Expected: all pass.
- [ ] **Step 5: Commit** `feat(server): enforce mode makes the simulation final`.

### Task 10: Docs

**Files:**
- Modify: `README.md` (server flags and the rollout steps), `docs/superpowers/specs/2026-10-14-multiplayer-rooms-design.md` and `2026-10-13-lan-duel-design.md` (a "Partly superseded by `2026-10-14-server-authority-design.md`" line, as earlier specs do)

- [ ] **Step 1:** Document in `README.md`:
  - `-authority`, `-lead`, `-lag`, `-garbage-window` and `-max-resyncs`;
  - that `scripts/build.sh` now builds `jestris_sim.wasm`;
  - the rollout: run `log`, grep the logs for `would-resync:` and for summaries where `claimedSent != simAttack`, then switch to `enforce`.
- [ ] **Step 2: Run** the full C++ and Go suites plus `scripts/build.sh` if emsdk is available. Expected: all green.
- [ ] **Step 3: Commit** `docs: server authority flags and rollout`.
