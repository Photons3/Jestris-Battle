# Multiplayer Rooms Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rooms of up to 16 players with a host-only Start. A 2-player match keeps today's KO duel; 3–16 players play a battle royale with hole garbage, random targets, elimination and placements.

**Architecture:** The Go hub becomes an N-seat room server with a roster, a host, `BEGIN`, and a per-match referee that picks duel or royale rules from the participant count. The C++ protocol gains player IDs (`ROOM`, `START`, relayed `SNAP`), `PLAYERS`, `BEGIN` and `OUT`. `DuelSession` gains a `Room` phase and a map of opponents. The core `Game` gains hole-style garbage and a level floor. The UI gains a room screen, a mini-board grid and royale standings. The offline bot (`LocalMatch`) speaks the new protocol.

**Tech Stack:** C++20, doctest, Dear ImGui (headless UI tests in `tests/ui_test.cpp`), Go with `github.com/coder/websocket`.

**Spec:** `docs/superpowers/specs/2026-10-14-multiplayer-rooms-design.md`

## Global Constraints

- Max players per room: **16** (`kMaxRoomPlayers` in `src/net/protocol.hpp`, `maxRoomPlayers` in `server/hub.go`; keep the two in sync with a comment, as with `kMaxNameLength`).
- Player IDs are 1–16, the lowest free number at join time, unique within a room.
- Room-unique names: base name, else the lowest suffix `2, 3, …` that is free, with the base shortened so the result is at most 12 characters.
- Mode: exactly 2 participants = Duel (unchanged rules: 3 KOs, 2:00, lines-sent tiebreak); 3–16 = Royale.
- Royale gravity floor: `startLevel + playedMs / 30000` (`kRoyaleLevelStepMs = 30000`), no level cap.
- Royale garbage: one hole column per attack (per `ATK`), different from the previous attack's column; no bombs.
- Wire verbs only the server sends: `ROOM PLAYERS START KO OVER OUT LEFT ERR ROOMS`. `REMATCH` and `OPP` are removed. Unknown verbs from clients in a room are dropped, not relayed.
- Exact strings: `ERR room is full`; room screen "Players n/16", "Need 2+ players", "Waiting for the host to start…"; result "Waiting for the host…", "Start next match"; eliminated overlay "OUT · #7"; royale result title "#4 of 9".
- Solo play and the desktop build must not change behaviour; every existing test not named below as adapted must still pass.

## Verification Commands

- C++: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`. A single suite: `./build/tetris_tests -tc="duel:*"` or `./build/tetris_ui_tests`.
- Go: `go -C server test -race ./...`. A single test: `go -C server test -race -run TestName ./...`.

## Review Focus

1. **A reused player ID mid-match:** player 3 leaves a royale and a newcomer gets ID 3. The opponent view must keep the old player's name and state. Pinned in Task 7, "royale: opponent names are fixed at START even when an ID is reused".
2. **A late joiner is invisible to the match:** they get no `SNAP`, `ATK` or `OUT`, and are never chosen as a target. Pinned in Task 5, `TestLateJoinerGetsNoMatchTraffic`.
3. **Leaves bring a royale down to one player, including the host leaving:** a winner is declared and the host migrates. Pinned in Task 5, `TestLeavesEndRoyaleAndMoveHost`.
4. **A stale duel timer:** a duel ends by `LEFT`, the host begins a new duel, and the old timer must not end it. Pinned in Task 4, `TestStaleTimerAfterLeftAndBeginIsIgnored`.
5. **`DEAD` racing the end of a royale:** the last two top out together. The second `DEAD` arrives after `OUT … 1` and must be dropped. Pinned in Task 5, `TestDeadAfterRoyaleEndIsDropped`.

---

### Task 1: Hole garbage in the core

**Files:**
- Modify: `src/core/board.hpp`, `src/core/board.cpp`, `src/core/game.hpp`, `src/core/game.cpp`
- Test: `tests/board_test.cpp`, `tests/game_test.cpp`

**Interfaces:**
- Produces:
  - `enum class GarbageStyle { Bombs, Holes };`
  - `GameOptions::garbage` (default `GarbageStyle::Bombs`).
  - `bool Board::pushGarbageHoles(const std::vector<int>& holeColumns);` One gray row per entry, bottom row first, with an empty cell in the given column. Returns false and leaves the board unchanged if a filled cell would be pushed off the top (same refusal rule as `pushGarbage`).

- [ ] **Step 1: Write the failing tests**

`tests/board_test.cpp`:
```cpp
TEST_CASE("board: pushGarbageHoles adds gray rows with one hole each") {
    Board board;
    REQUIRE(board.pushGarbageHoles({3, 3, 7}));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            CHECK(board.at(x, y) == (x == (y < 2 ? 3 : 7) ? Cell{} : Cell{PieceType::Garbage}));
    CHECK(board.fullRows().empty());
}
TEST_CASE("board: an overflowing pushGarbageHoles is refused and changes nothing") {
    Board board;
    board.set(0, kBoardHeight - 1, PieceType::I);
    const Board before = board;
    CHECK_FALSE(board.pushGarbageHoles({0}));
    CHECK(board == before);
}
```

`tests/game_test.cpp` (use the existing helpers there for locking pieces and expiring travel time; garbage rises on a non-clearing lock):
- `"game: hole garbage from one attack shares a column and the next attack moves it"`: a `Game` with `GameOptions{.allowPause = false, .garbage = GarbageStyle::Holes}`. `receiveGarbage(3)` then `receiveGarbage(2)`, let both travel, lock one non-clearing piece. Check:
  - the bottom 5 rows each hold exactly one empty cell;
  - rows 0–2 share a hole column and rows 3–4 share another, different column;
  - no cell on the board is `PieceType::Bomb`.
- `"game: an attack split by the per-lock cap keeps its hole column"`: `receiveGarbage(12)`, lock twice (8 rows, then 4). All 12 garbage rows have the same hole column.
- `"game: filling a hole clears the row and counts as garbage cleared"`: after one row of hole garbage rises, drop an `I` piece vertically into the hole on a board shaped so the row completes. Then `lines() == 1` and `stats().garbageCleared == 1`.
- `"game: hole garbage that overflows is a top-out"`: `receiveGarbage(kBoardHeight)` on a board with a stack, then lock. Then `state() == GameState::GameOver`.

- [ ] **Step 2: Run the tests to make sure they fail**

Run: `cmake --build build && ./build/tetris_tests -tc="board:*,game:*"`
Expected: compile errors for `pushGarbageHoles`, `GarbageStyle` and `GameOptions::garbage`.

- [ ] **Step 3: Implement**
  - `Board::pushGarbageHoles` mirrors `pushGarbage(const std::vector<int>&)`, writing an empty cell instead of a bomb.
  - `GarbageBatch` gains `int holeX = -1`.
  - With `Holes`, `receiveGarbage` draws `holeX` from `garbageRng_`, re-rolling while it equals the previous batch's column (store `lastHoleX_`, initially -1).
  - `applyGarbage` builds one column per rising row from the batches it takes from, and calls `pushGarbageHoles` when `options_.garbage == Holes`. The bomb path stays exactly as it is.

- [ ] **Step 4: Run the tests to make sure they pass**

Run: `./build/tetris_tests`
Expected: all pass, including the existing bomb tests.

- [ ] **Step 5: Commit**

```bash
git add src/core tests/board_test.cpp tests/game_test.cpp
git commit -m "feat(core): hole-style garbage, one column per attack"
```

---

### Task 2: Level floor in the core

**Files:**
- Modify: `src/core/game.hpp`, `src/core/game.cpp`
- Test: `tests/game_test.cpp`

**Interfaces:**
- Produces: `void Game::setLevelFloor(int floor);` `level()`, gravity and scoring use `std::max(levelFor(startLevel_, lines_, linesPerLevel), floor)`. The default floor is 0. Raising the floor above the current level emits `GameEventType::LevelUp` once.

- [ ] **Step 1: Write the failing tests**
  - `"game: the level floor raises level and gravity"`: `Game game(1, 1)`, `setLevelFloor(5)`. Then `level() == 5`, and a piece falls one row after `gravityMsPerRow(5)` ms of `tick`, not `gravityMsPerRow(1)`.
  - `"game: the level floor never lowers the level"`: start level 8, `setLevelFloor(3)`. Then `level() == 8`.
  - `"game: line-clear score uses the floored level"`: `setLevelFloor(4)`, clear one line. The score increases by `lineClearScore(1, 4)`.
  - `"game: raising the floor emits LevelUp once"`: `setLevelFloor(3)` twice. The events contain exactly one `LevelUp`.

- [ ] **Step 2: Run them to make sure they fail**

Run: `./build/tetris_tests -tc="game: the level floor*,game: line-clear score uses*,game: raising the floor*"`
Expected: compile error, `setLevelFloor` is not a member.

- [ ] **Step 3: Implement**

Add `int levelFloor_ = 0`. Recompute `level_` in `setLevelFloor` and at the existing `level_ = levelFor(...)` site as `std::max(levelFor(...), levelFloor_)`.

- [ ] **Step 4: Run all tests**

Run: `./build/tetris_tests`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/core tests/game_test.cpp
git commit -m "feat(core): level floor for royale gravity"
```

---

### Task 3: Protocol messages with player IDs

**Files:**
- Modify: `src/net/protocol.hpp`, `src/net/protocol.cpp`, `src/app/duel.cpp` (compile fixes only), `src/bot/local_match.cpp` (wire format only)
- Test: `tests/protocol_test.cpp`; update the wire strings in `tests/duel_test.cpp`, `tests/local_match_test.cpp` and `tests/ui_test.cpp`

**Interfaces:**
- Produces (in `protocol.hpp`):
```cpp
inline constexpr int kMaxRoomPlayers = 16;  // maxRoomPlayers in server/hub.go
struct RoomEntered { std::string code; int playerId = 1; };            // ROOM <CODE> <id>; replaces RoomCreated
struct StartMatch { std::uint32_t seed = 0; int startLevel = 1; std::vector<int> players; };  // START seed lvl id... (2-16 unique ids)
struct PlayerSnapshot { int fromId = 0; Snapshot snapshot; };          // SNAP <id> <the 14 Snapshot args>
struct RosterEntry { int id = 0; std::string name; bool inMatch = false; };
struct Roster { int hostId = 0; std::vector<RosterEntry> players; };  // PLAYERS <hostId> id:name:0|1 ... (1-16, unique ids, hostId listed)
struct Begin {};                                                       // BEGIN
struct PlayerOut { int id = 0; int place = 0; };                      // OUT <id> <place>, place 1-16
// RoomInfo gains: int players = 1;  // ROOMS entry CODE:lvl:host:n, n 1-15
```
  - Every struct keeps the file's `friend bool operator==(...) = default;` pattern.
  - All of them join `Message`.
  - `Rematch` and `OpponentName` stay for now; Task 6 removes them.
  - A client-sent `SNAP` (14 args) still decodes to `Snapshot`. The 15-arg form decodes to `PlayerSnapshot`.

- [ ] **Step 1: Write the failing tests** in `tests/protocol_test.cpp`
  - Extend the round-trip list with: `RoomEntered{"ABCD", 3}`, `StartMatch{7, 2, {1, 2}}`, `StartMatch{7, 2, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}}`, `PlayerSnapshot{5, snapshot}`, `Roster{2, {{1, "Sam", true}, {2, "Sam2", false}}}`, `Begin{}`, `PlayerOut{4, 9}`, and `RoomList{{{"ABCD", 3, "Sam", 5}}}`.
  - Exact wire checks:
```cpp
CHECK(encode(RoomEntered{"ABCD", 3}) == "ROOM ABCD 3");
CHECK(encode(StartMatch{7, 2, {1, 4, 9}}) == "START 7 2 1 4 9");
CHECK(encode(Roster{2, {{1, "Sam", true}, {2, "Kim", false}}}) == "PLAYERS 2 1:Sam:1 2:Kim:0");
CHECK(encode(Begin{}) == "BEGIN");
CHECK(encode(PlayerOut{4, 9}) == "OUT 4 9");
CHECK(encode(RoomList{{{"ABCD", 3, "Sam", 5}}}) == "ROOMS ABCD:3:Sam:5");
CHECK(encode(PlayerSnapshot{5, Snapshot{}}) == "SNAP 5 " + encode(Snapshot{}).substr(5));
```
  - Add to the malformed table:
    - `"ROOM ABCD"`, `"ROOM ABCD 0"`, `"ROOM ABCD 17"`
    - `"START 7 2"`, `"START 7 2 1"`, `"START 7 2 1 1"`, `"START 7 2 1 17"`, and a `START` with 17 IDs
    - `"PLAYERS 1"`, `"PLAYERS 3 1:Sam:1"` (host not listed), `"PLAYERS 1 1:Sam:2"`, `"PLAYERS 1 1:Sam:1 1:Kim:0"`, `"PLAYERS 1 1:bad name:1"`, `"PLAYERS 1 1:Sam"`
    - `"OUT 0 2"`, `"OUT 3 0"`, `"OUT 3 17"`, `"BEGIN now"`
    - `"ROOMS ABCD:3:Sam:0"`, `"ROOMS ABCD:3:Sam:16"`, `"ROOMS ABCD:3:Sam"`
    - `"SNAP 0 …"` and `"SNAP 17 …"`, built from a valid snapshot's args.

- [ ] **Step 2: Run them to make sure they fail**

Run: `./build/tetris_tests -tc="protocol:*"`
Expected: compile errors for the new types.

- [ ] **Step 3: Implement the codec**
  - Parse `PLAYERS` entries by splitting on `:`. A name with a `:` already fails `isValidName`.
  - Then make the tree compile and the existing suites pass with the new wire format, without changing behaviour:
    - `duel.cpp` handles `RoomEntered` where it handled `RoomCreated`, and copies `PlayerSnapshot::snapshot` into `opponent_`.
    - `local_match.cpp` queues `RoomEntered{"BOTS", 1}` before its first `START`, sends `StartMatch{seed, lvl, {1, 2}}`, and sends its snapshots as `PlayerSnapshot{2, snapshot}`.
    - In the tests, change `"ROOM ABCD"` to `"ROOM ABCD 1"`, `"START 7 1"` to `"START 7 1 1 2"`, and incoming `"SNAP …"` to `"SNAP 2 …"`.

- [ ] **Step 4: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/net src/app/duel.cpp src/bot/local_match.cpp tests
git commit -m "feat(net): player ids, PLAYERS, BEGIN and OUT in the protocol"
```

---

### Task 4: Server rooms of up to 16 with host start (duel rules)

**Files:**
- Modify: `server/hub.go`
- Test: `server/hub_test.go`

**Interfaces:**
- Consumes: the wire formats from Task 3.
- Produces (Go, used by Task 5):
```go
const maxRoomPlayers = 16 // kMaxRoomPlayers in src/net/protocol.hpp
type room struct {
	code    string
	level   int
	public  bool
	seq     uint64
	players []*player // join order
	hostID  int
	match   *match // nil between matches
	matchID uint64
	timer   stopper
}
type player struct {
	conn     *websocket.Conn
	room     *room
	name     string // from HELLO
	id       int    // in the room, 1-16
	roomName string // name made unique in the room
}
type match struct {
	royale  bool
	players []*player // participants, in START order
	kos     map[*player]int
	sent    map[*player]int
	alive   map[*player]bool // Task 5
}
func uniqueName(base string, taken func(string) bool) string
func (r *room) rosterMessage() string // "PLAYERS <hostID> id:name:m ..."
func (r *room) broadcast(text string) []outgoing
```

**Message order** (the tests rely on it):
- **A join:** `ROOM <code> <id>` to the new player, then `PLAYERS` to every room member in join order. Then `ROOMS` to watchers if the room is public.
- **A valid `BEGIN`:** the same `START` to each participant in join order, then `PLAYERS`.
- **Duel end** (3rd KO, time-up or `LEFT`): `OVER` (or `LEFT`) as today, then `PLAYERS` with all flags 0.

- [ ] **Step 1: Rewrite and add tests in `server/hub_test.go`**
  - Add a helper `joinRoom(t, srv, code, name) *client` that sends `HELLO name`, then `JOIN code`, and expects `ROOM code <id>`.
  - Adapt the existing tests:
    - Every create or join expects the new `ROOM` and `PLAYERS` lines.
    - Matches start with the host's `BEGIN`, and `START` lines carry `1 2`.
    - Incoming `SNAP` relays as `SNAP <id> …`.
    - Delete `TestRematchNeedsBothPlayers`, `TestRematchBeforeOverIsIgnored` and `TestNamesAreSentBeforeStart`.
    - `TestStaleTimerAfterRematchIsIgnored` becomes `TestStaleTimerAfterBeginIsIgnored`.
    - `TestHostLeavingRemovesTheRoom` becomes `TestHostLeavingMovesTheHost`.
  - New tests:
    - `TestSixteenPlayersFitAndTheSeventeenthIsRefused`: 16 joins succeed; the 17th gets `ERR room is full`.
    - `TestDuplicateNamesGetSuffixes`: three `Sam`s give `Sam`, `Sam2` and `Sam3`, and twelve-character `ABCDEFGHIJKL` twice gives `ABCDEFGHIJK2`.
    - `TestJoinSendsRoomAndRoster`: the second player gets `ROOM AAAA 2`; both get `PLAYERS 1 1:Ann:0 2:Bob:0`.
    - `TestOnlyTheHostCanBegin`: a non-host `BEGIN` sends nothing. The host alone sending `BEGIN` sends nothing. The host with 2 players gets `START 101 1 1 2` on both clients, then `PLAYERS 1 1:Ann:1 2:Bob:1`.
    - `TestBeginDuringAMatchIsIgnored`.
    - `TestLateJoinerWaitsForTheNextMatch`: a third player joins mid-duel. They get `ROOM` and `PLAYERS` (flag 0 for them), and no `START`. After `OVER` and a new `BEGIN`, `START` lists `1 2 3`.
    - `TestHostLeavingMovesTheHost`: the host leaves a 3-player room. The others get `PLAYERS 2 …`, and the room still accepts joins.
    - `TestLastPlayerLeavingDeletesTheRoom`: a later `JOIN` gets `ERR unknown room`.
    - `TestDuelLeftEndsTheMatchButKeepsTheRoom`: one participant disconnects mid-duel. The other gets `LEFT`, then `PLAYERS 2 2:Bob:0`, and can `BEGIN` again once someone joins.
    - `TestStaleTimerAfterLeftAndBeginIsIgnored`: in a 3-player room, the duel between 1 and 2 ends by 2 leaving. Host 1 begins a duel with player 3. Firing the first match's timer sends nothing.
    - `TestUnknownVerbsAreNotRelayed`: `HELLO2 x` inside a room reaches no one.
    - `TestPublicRoomsListPlayerCountUntilFull`: `ROOMS AAAA:1:Ann:1`, then `ROOMS AAAA:1:Ann:2` after a join. The room is unlisted at 16 and listed again at 15.
    - `TestQuickPrefersARoomWithNoMatch`: the older public room has a match running and the newer one doesn't. `QUICK` joins the newer one.

- [ ] **Step 2: Run the tests to make sure they fail**

Run: `go -C server test -race ./...`
Expected: FAIL; the new expectations don't match the 2-seat hub.

- [ ] **Step 3: Implement the hub changes**
  - `route` handles `BEGIN`, `ATK`, `DEAD` and `SNAP`, and drops everything else inside a room. `SNAP` is relayed as `"SNAP " + strconv.Itoa(p.id) + " " + rest` to the other participants.
  - IDs are the lowest free number.
  - `leave` removes the player from `players`. It deletes an empty room, otherwise sets `hostID = players[0].id` if the host left, ends a duel with `LEFT` if the leaver was a participant, and broadcasts `PLAYERS`.
  - `openPublic` lists `len(players) < maxRoomPlayers`, and the `ROOMS` host is the current host's `roomName`.
  - The duel referee (`attack`, `knockOut`, `finish`, `verdict`, `timeUp`) keeps its logic, indexed by participant instead of seat.

- [ ] **Step 4: Run the tests to make sure they pass**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server
git commit -m "feat(server): rooms of up to 16 with a host-only BEGIN"
```

---

### Task 5: Server royale referee

**Files:**
- Modify: `server/hub.go`
- Test: `server/hub_test.go`

**Interfaces:**
- Consumes: the Task 4 types.
- Produces: `Hub.pickTarget func(n int) int`, an index into the sender's living opponents in START order (default `rand.IntN`). The test hub sets it to always return 0 unless a test overrides it.

**Rules:**
- **`BEGIN` with 3 or more players:** `match.royale = true`, everyone alive, no timer.
- **`ATK n`:** valid only from a living participant. Count it in `sent`, then send `ATK n` to `living[pickTarget(len(living))]`, where `living` is the sender's living opponents.
- **`DEAD`:** valid only from a living participant. Its place is the number alive before it died. Send `OUT <id> <place>` to every connected participant. If one player is left, send `OUT <winnerId> 1` to every connected participant, end the match (`r.match = nil`) and broadcast `PLAYERS`.
- **A participant who leaves mid-royale** is handled like `DEAD`, but the leaver gets nothing. `PLAYERS` follows as usual.

- [ ] **Step 1: Write the failing tests**
  - `TestThreePlayersStartARoyaleWithoutTimer`: `START 101 1 1 2 3` reaches all three, and no timer is armed (`len(clock.timers) == 0`).
  - `TestRoyaleAttackGoesToAPickedLivingOpponent`: with `pickTarget` returning 1, player 1's `ATK 2` reaches player 3 only. After player 2 is out, `pickTarget` sees `n == 1` (record it).
  - `TestRoyaleSnapReachesEveryOtherParticipant`: player 2's `SNAP …` arrives at players 1 and 3 as `SNAP 2 …`.
  - `TestRoyaleDeadGivesPlacesAndTheLastWins`: with 4 players, player 2's `DEAD` makes everyone get `OUT 2 4`; player 4's gives `OUT 4 3`; player 1's gives `OUT 1 2`, then `OUT 3 1`, then `PLAYERS 1 …:0 …`.
  - `TestEliminatedPlayersCannotAttackOrDieAgain`: after `OUT 2 3`, player 2's `ATK 3` and `DEAD` produce nothing.
  - `TestDeadAfterRoyaleEndIsDropped`: after `OUT 3 1`, a `DEAD` from player 3 produces nothing, and the host can `BEGIN` again.
  - `TestLeavesEndRoyaleAndMoveHost`: in a 3-player royale, host 1 disconnects. Players 2 and 3 get `OUT 1 3` and `PLAYERS 2 …`. Then player 2 disconnects: player 3 gets `OUT 2 2`, `OUT 3 1` and `PLAYERS 3 3:…:0`.
  - `TestLateJoinerGetsNoMatchTraffic`: player 4 joins mid-royale and gets `ROOM` and `PLAYERS` only. A `SNAP` and an `ATK` from player 1 (with `pickTarget` returning the last index) never reach player 4. Player 4 gets no `OUT` when player 2 dies.
  - `TestServerOnlyVerbsFromClientsAreDropped`: `PLAYERS 1 1:x:1`, `OUT 1 1` and `ROOM AAAA 1` sent by a client reach no one.

- [ ] **Step 2: Run them to make sure they fail**

Run: `go -C server test -race -run 'Royale|LateJoiner|DeadAfter|LeavesEnd|ThreePlayers' ./...`
Expected: FAIL.

- [ ] **Step 3: Implement** the rules above in `hub.go`.

- [ ] **Step 4: Run all server tests**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server
git commit -m "feat(server): battle royale for 3-16 players"
```

---

### Task 6: Session room phase, roster and host start (and the bot)

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`, `src/bot/local_match.hpp`, `src/bot/local_match.cpp`, `src/net/protocol.hpp`, `src/net/protocol.cpp` (remove `Rematch` and `OpponentName`), `src/app/app.cpp`, `src/app/render.hpp`, `src/app/render.cpp` (only to keep compiling: `UiCommand::Rematch` becomes `UiCommand::BeginMatch`, and the result's Rematch button becomes "Start next match" for the host; the full room UI is Task 8)
- Test: `tests/duel_test.cpp`, `tests/local_match_test.cpp`, `tests/protocol_test.cpp`

**Interfaces:**
- Consumes: Task 3's messages.
- Produces (in `duel.hpp`):
```cpp
enum class DuelPhase { Idle, Lobby, Waiting, Room, Countdown, Playing, Result };
enum class DuelMode { Duel, Royale };
struct OpponentView {
    int id = 0;
    std::string name;   // fixed at START
    Snapshot snapshot;
    bool alive = true;
    int place = 0;      // 0 until OUT
};
// DuelSession:
void begin();                                       // BEGIN; only for the host in Room or Result with roster().size() >= 2
bool isHost() const;                                // myId() == hostId()
int myId() const;
int hostId() const;
const std::vector<RosterEntry>& roster() const;
DuelMode mode() const;
const std::vector<OpponentView>& opponents() const; // other participants, sorted by id
const Snapshot& opponent() const;                   // opponents().front().snapshot, or an empty Snapshot
const std::string& opponentName() const;            // opponents().front().name, or ""
int playedMs() const;                               // ms spent Playing this match (counts up in both modes)
```
  - Removed: `requestRematch`, `rematchRequested`, `opponentGone`, and the `Rematch` and `OpponentName` messages.
  - `LocalMatch`:
    - On `CreateRoom`, it queues `RoomEntered{"BOTS", 1}`, then `Roster{1, {{1, humanName, true}, {2, botName(level_), true}}}`, then `StartMatch{seed, lvl, {1, 2}}`. `humanName` comes from a received `Hello`, else `"You"`.
    - `Begin` starts the next match when `over_`, replacing `Rematch`.

- [ ] **Step 1: Write and adapt the failing tests** (`tests/duel_test.cpp`)
  - Change the `startMatch` helper to: `createRoom(1)`, `ROOM ABCD 1`, `PLAYERS 1 1:Me:0 2:Foe:0`, `START 7 1 1 2`, then the countdown.
  - New tests:
    - `"duel: ROOM moves Waiting to Room with my id"`: `phase() == DuelPhase::Room`, `myId() == 1`, `roomCode() == "ABCD"`.
    - `"duel: PLAYERS updates roster and host"`: after `PLAYERS 2 1:Me:0 2:Foe:0`, `hostId() == 2`, `isHost() == false`, and `roster().size() == 2`.
    - `"duel: begin sends BEGIN only for the host with two or more players"`: alone it sends nothing; as a non-host it sends nothing; as the host with 2 it sends `"BEGIN"` in Room and again in Result.
    - `"duel: START without my id is ignored"`: `START 7 1 2 3` in Room leaves the phase at `Room`.
    - `"duel: START with two ids is a duel with one named opponent"`: `mode() == DuelMode::Duel`, `opponents().size() == 1`, `opponentName() == "Foe"`.
    - `"duel: SNAP from a participant updates that opponent; unknown ids are ignored"`.
    - `"duel: Result goes back to Countdown on the next START"`.
  - Delete the rematch tests. The existing KO, clock, `OVER` and `LEFT` tests keep their assertions.
  - `tests/local_match_test.cpp`:
    - `"local match: CREATE answers ROOM, PLAYERS and START with two ids"`;
    - `"local match: BEGIN after OVER starts the next match"` (replaces the rematch test);
    - bot snapshots now arrive as `SNAP 2 …`.

- [ ] **Step 2: Run them to make sure they fail**

Run: `./build/tetris_tests -tc="duel:*,local match:*"`
Expected: compile errors (`DuelPhase::Room`, `begin`, `opponents`).

- [ ] **Step 3: Implement**
  - `opponents_` is rebuilt at each `START` from its ID list, minus `myId_`. Names come from the current roster, falling back to `"P<id>"`.
  - `PLAYERS` is accepted in Room, Countdown, Playing and Result.
  - `LEFT` in a duel still gives the `OpponentLeft` outcome.
  - `playedMs_` replaces the clock-derived `playedMs()`: it counts Playing time and stops at the duel's 0:00.
  - In `app.cpp`: `UiCommand::BeginMatch` calls `app.duel.begin()`. Check `duelMusic` and any `switch (phase)` for the new `Room` phase (treat it like `Waiting`).

- [ ] **Step 4: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src tests
git commit -m "feat(app): room phase, roster and host-started matches"
```

---

### Task 7: Session royale mode

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/duel_test.cpp`

**Interfaces:**
- Consumes: `GarbageStyle` (Task 1), `Game::setLevelFloor` (Task 2), `PlayerOut` (Task 3), `opponents()` (Task 6).
- Produces:
  - `inline constexpr int kRoyaleLevelStepMs = 30000;`
  - `bool DuelSession::eliminated() const;`
  - `int DuelSession::myPlace() const;` (0 until this player's `OUT`)
  - `int DuelSession::participants() const;` (the length of the last `START` list)
  - `makeDuelGame(const StartMatch&)` uses `GarbageStyle::Holes` when `start.players.size() >= 3`.

- [ ] **Step 1: Write the failing tests** (helper `startRoyale`: `ROOM ABCD 1`, a `PLAYERS` with 4 players, `START 7 1 1 2 3 4`, then the countdown)
  - `"royale: START with three or more ids is a royale with hole garbage"`: `mode() == DuelMode::Royale` and `opponents().size() == 3`. After an `ATK 2` and a non-clearing lock, the board has no `Bomb` cell.
  - `"royale: a top-out sends the final SNAP and DEAD once and keeps the board"`: force a top-out with the duel tests' injected `MakeGame`. Two `update` calls send exactly one `"DEAD"`. `eliminated()` is true and the game stays in `GameState::GameOver`.
  - `"royale: OUT marks an opponent out with its place"`: `OUT 3 4` gives opponent 3 `alive == false` and `place == 4`, and the phase stays `Playing`.
  - `"royale: OUT with place 1 for me is a win"`: `OUT 2 4`, `OUT 3 3`, `OUT 4 2`, `OUT 1 1`. Then `phase() == Result`, `outcome() == Won` and `myPlace() == 1`.
  - `"royale: someone else's OUT 1 is a loss with my place"`: top out, `OUT 1 3`, `OUT 2 2`, `OUT 4 1`. Then `outcome() == Lost` and `myPlace() == 3`.
  - `"royale: no clock freeze and the level floor rises every 30 s"`: after `update(kMatchMs + 1000)`, `frozen()` is false. After `update(kRoyaleLevelStepMs)` from the start of play, `game()->level() == 2` (start level 1).
  - `"royale: opponent names are fixed at START even when an ID is reused"`: after the start, `PLAYERS 1 1:Me:1 2:Bo:1 3:New:0 4:Di:1`. `opponents()` entry 3 still has its START-time name, and a later `OUT 3 4` still marks it.
  - `"royale: OUT outside a match is ignored"`: in Room or Result it changes nothing.

- [ ] **Step 2: Run them to make sure they fail**

Run: `./build/tetris_tests -tc="royale:*"`
Expected: FAIL / compile errors.

- [ ] **Step 3: Implement**
  - In `update`, Royale mode does not reset on top-out. It sends `Dead` once (guarded by `eliminated_`) and stops sending attacks and snapshots afterwards.
  - It calls `game_->setLevelFloor(startLevel_ + playedMs_ / kRoyaleLevelStepMs)` each Playing frame while not eliminated.
  - `frozen()` applies the KO flash and 0:00 rules in Duel mode only.

- [ ] **Step 4: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/app tests/duel_test.cpp
git commit -m "feat(app): battle royale session for 3-16 players"
```

---

### Task 8: Room screen, room controls and lobby counts

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.cpp`
- Test: `tests/ui_test.cpp`

**Interfaces:**
- Consumes: Task 6's session API, and `RoomInfo::players`.
- Produces:
  - `UiCommand::BeginMatch` (already added in Task 6).
  - Static helpers in `render.cpp`: `UiCommand drawRoom(const DuelSession&, const Fonts&, StartMenu&)`, and `UiCommand drawRoomControls(const DuelSession&)` (the Start/waiting/Leave block shared by Room and Result).

**Screen contents:**
- **Waiting:** shows only "CONNECTING", or "SEARCHING" for Quick match, plus Cancel.
- **Room:**
  - The code and the invite link block, moved from Waiting unchanged, and the public/private line.
  - `"Players %d/16"`.
  - One row per roster entry: ★ before the host, the own row in the plate highlight color, and `" (next match)"` after every entry with `inMatch == false` whenever any roster entry has `inMatch == true` (a match is running).
  - Then `drawRoomControls`:
    - Host: a "Start" button. It is disabled with the text "Need 2+ players" when the roster has fewer than 2, and returns `BeginMatch` when clicked.
    - Non-host: "Waiting for the host to start…".
    - Everyone: "Leave", which returns `LeaveDuel`.
- **Result:** the same `drawRoomControls`, with the host button labelled "Start next match" and the waiting text "Waiting for the host…". The `opponentGone` and Rematch branches are removed.
- **Lobby rows:** they show `"%s · Lv %d · %d/16"` (host, level, players).

- [ ] **Step 1: Write the failing UI tests** (same setup as `"ui: the duel result draws the stats table"`)
  - `"ui: the room screen draws the roster and the host's Start"`: a session in Room as the host of 3. Two frames of `drawDuel` return `UiCommand::None`.
  - `"ui: the room screen draws for a non-host and a lone host"`: two sessions, two frames each, no crash, `None`.
  - `"ui: the duel result draws the room controls"`: adapt the existing stats-table test to the new join flow (`ROOM`, `PLAYERS`, `START 7 1 1 2`).
  - `"ui: the lobby draws room player counts"`: adapt the existing lobby test to `ROOMS ABCD:3:Sam:5`.

- [ ] **Step 2: Run them to make sure they fail**

Run: `cmake --build build && ./build/tetris_ui_tests`
Expected: the Room-phase test fails (nothing is drawn for `Room`) or doesn't compile.

- [ ] **Step 3: Implement** the screens described above.

- [ ] **Step 4: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/app tests/ui_test.cpp
git commit -m "feat(app): room screen with roster and host start"
```

---

### Task 9: Royale boards, eliminated overlay and standings

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.cpp`, `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/ui_test.cpp`, `tests/duel_test.cpp`

**Interfaces:**
- Consumes: Task 7's session API.
- Produces:
```cpp
// render.hpp
struct GridShape { int cols = 1; int rows = 1; float cell = 0.0f; };
// Mini boards are kMiniCols x kMiniRows cells (well 10 + meter 1 + gap 1, by visible 20 + name 1 + gap 1).
inline constexpr int kMiniCols = 12;
inline constexpr int kMiniRows = 22;
// The cols x rows grid for `boards` mini boards in a width x height area with the largest cell;
// ties go to fewer columns.
GridShape royaleGrid(int boards, float width, float height);
// duel.hpp
struct Standing { int place = 0; std::string name; int attack = 0; bool me = false; };
std::vector<Standing> royaleStandings(const DuelSession& duel);  // place order; empty outside a royale Result
std::string royaleResultTitle(const DuelSession& duel);           // "YOU WIN", or "#4 of 9"
```

**Layout and behaviour:**
- **Royale boards:** own well, hold, next, score and level drawn as in the duel. The cell size is computed like `computeDuelLayout` but with a 28-column budget for the own side. The remaining width to the right and the full height go to `royaleGrid(opponents().size(), …)`. Each mini board draws:
  - the name;
  - `drawGarbageMeter` at mini size;
  - the snapshot's `cells` with no hold, next or ghost;
  - a dimmed board with `"#%d"` when `!alive`.
- **Eliminated:** `"OUT · #%d"` (or `"OUT"` while `myPlace() == 0`) is drawn over the own well.
- **Royale Result:** `royaleResultTitle`, then a standings table (Place / Name / Attack, own row highlighted), then `drawRoomControls`. The duel result is unchanged.
- **In `app.cpp`:** attack trails launch only in Duel mode.
- **Standings:** `royaleStandings` uses each opponent's `place` and its last snapshot's `stats.attack`; for this player, `myPlace()` and `game()->stats().attack`.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST_CASE("ui: the royale grid picks the shape with the largest cells") {
    CHECK(royaleGrid(2, 900.0f, 900.0f).cols == 2);   CHECK(royaleGrid(2, 900.0f, 900.0f).rows == 1);
    CHECK(royaleGrid(8, 900.0f, 900.0f).cols == 4);   CHECK(royaleGrid(8, 900.0f, 900.0f).rows == 2);
    CHECK(royaleGrid(15, 900.0f, 900.0f).cols == 5);  CHECK(royaleGrid(15, 900.0f, 900.0f).rows == 3);
    CHECK(royaleGrid(1, 900.0f, 900.0f).cols == 1);
}
```
  - `"ui: royale boards draw while playing, eliminated, and in the result"`: a 4-player royale session. Two frames each in Playing, after this player's top-out, and in Result. All return `None`.
  - In `tests/duel_test.cpp`:
    - `"royale: standings list places in order with attack"`: after `OUT 2 4`, `OUT 1 3`, `OUT 4 2`, `OUT 3 1`, the places are `{1, 2, 3, 4}` and the names match the START roster.
    - `"royale: the result title is the place out of the field"`: `"#3 of 4"`, and `"YOU WIN"` for the winner.

- [ ] **Step 2: Run them to make sure they fail**

Run: `cmake --build build && ./build/tetris_ui_tests && ./build/tetris_tests -tc="royale:*"`
Expected: compile errors (`royaleGrid`, `royaleStandings`).

- [ ] **Step 3: Implement** what is described above. `royaleGrid` tries `cols = 1..boards`, with `rows = ceil(boards / cols)` and `cell = min(width / (cols * kMiniCols), height / (rows * kMiniRows))`, and keeps the first strictly larger cell.

- [ ] **Step 4: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/app tests
git commit -m "feat(app): royale mini-board grid, OUT overlay and standings"
```

---

### Task 10: Docs and manual check

**Files:**
- Modify: `README.md` ("Playing a duel"), `docs/superpowers/specs/2026-10-13-lan-duel-design.md`, `2026-10-13-bomb-battle-design.md`, `2026-10-13-player-names-design.md`, `2026-10-13-room-lobby-design.md` (one "Partly superseded" line each, pointing to `2026-10-14-multiplayer-rooms-design.md`)

- [ ] **Step 1: Rewrite README "Playing a duel"**

Cover:
- rooms of up to 16 players;
- the host presses Start, and late joiners play the next match;
- 2 players play the KO duel as before;
- 3–16 play a battle royale: one top-out and you're out, garbage has one open hole per attack, each attack goes to a random living opponent, gravity rises every 30 s, and the last player standing wins.

Keep the existing attack and bomb rules text for the duel.

- [ ] **Step 2: Add the superseded pointers** to the four earlier specs.

- [ ] **Step 3: Full verification**

Run: `ctest --test-dir build --output-on-failure && go -C server test -race ./...`
Expected: all pass.

- [ ] **Step 4: Manual check (web build + server)**

Build with `scripts/build.sh` and run `scripts/run.sh -match 20s`. Then check:
- 4–5 localhost tabs:
  - names, and the Browse rooms counts;
  - Quick match into a room;
  - host Start, and a late joiner waiting;
  - a royale played to the end, with hole garbage visible;
  - the standings, and starting the next match;
  - the host leaving in the room and mid-match;
  - the grid in a phone-landscape-sized window.
- A 2-tab duel and a vs-Bot match, to confirm both behave as before apart from host start.

- [ ] **Step 5: Commit**

```bash
git add README.md docs
git commit -m "docs: rooms of up to 16 and battle royale"
```
