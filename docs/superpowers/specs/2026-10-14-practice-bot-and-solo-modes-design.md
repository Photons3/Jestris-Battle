# Design: Practice bot and solo modes

## Context
Two gaps keep players from coming back. First, a duel (`2026-10-13-lan-duel-design.md`, `2026-10-13-bomb-battle-design.md`) needs a second person, so nobody can practise it when the lobby is empty. Second, solo play is one open-ended marathon with nothing to beat. This spec adds a **practice bot** you can duel offline and four standard **solo modes** with saved personal bests: Sprint, Blitz, Cheese and Zen.

The design was settled in a brainstorming session. Both features are in one spec, but the plan should build them in this order: core rules → solo session → PBs and screens → bot brain → local referee. The bot reuses the countdown, freeze and result patterns the solo modes establish.

## Goals and non-goals
- **Goals:**
  - From the start menu, on both desktop and browser builds, start a duel against a bot (Easy, Medium or Hard) or any of Marathon, Sprint 40L, Blitz 2:00, Cheese 10L and Zen.
  - Every timed mode ends with a result screen showing the run and the personal best (PB).
  - PBs survive restarts on the same device.
  - Bot duels use exactly the existing duel rules and screens.
- **Non-goals:**
  - online leaderboards, replays or ghosts
  - bot tucks, spins or lookahead beyond the current and hold piece (the bot never aims for T-spins, though the game still scores them if one happens)
  - choosing the cheese size
  - saving or resuming a run in progress
  - changes to `DuelSession`, the wire protocol or the Go server

## Understanding (from brainstorming)
- **Stated:** a bot opponent, even a simple placement scorer, helps when the lobby is empty. Sprint (40 lines), Blitz (2 minutes for score), Cheese race (dig through garbage) and Zen are the standard modes. Sprint times and Blitz scores keep players coming back. The core `Game` already does most of this.
- **Agreed assumptions:**
  - The bot plays the normal duel rules: bomb garbage, 3 KOs, 2:00.
  - PBs are local to the device: localStorage in the browser, a file on desktop.
  - Everything works offline, with no server.
- **Success:** each mode starts from the menu, ends cleanly with a result and PB, and the bot gives a winnable but real duel at each difficulty.

## 1. Core rule changes (`tetris_core`)
Small, mode-agnostic additions. `Game` never learns what a "mode" is.

- **`GameOptions::linesPerLevel`** (default `kLinesPerLevel` = 10). `levelFor(startLevel, lines, linesPerLevel)` takes it as a parameter. Blitz uses 5. Duels and other modes keep 10.
- Scoring is unchanged: the guideline `awardScore` (T-spins, back-to-back, combos, perfect clears) applies in every mode, so Blitz rewards all of it.
- **`Board::pushCheese(int rows, std::mt19937& rng)`** fills the bottom `rows` rows with `PieceType::Garbage`, leaving exactly one empty hole per row. Each row's hole column differs from the row below it, and there are no bombs. It is only used to build the Cheese starting board, which goes to the existing `Game(seed, level, Board, options)` constructor. Unlike duel garbage (`pushGarbage`: full rows with a bomb, cleared only by detonation), cheese rows clear as normal full lines.
- **`Board::garbageRows()`** returns how many rows contain at least one `Garbage` cell (`Bomb` cells don't count). Cheese uses it for progress and the finish check.

## 2. `SoloSession` (`src/app/solo.{hpp,cpp}`, in `tetris_duel`)
It wraps one `Game` the same way `DuelSession` does. No SDL, fully unit-tested.

```
enum class SoloMode  { Marathon, Sprint, Blitz, Cheese, Zen };
enum class SoloPhase { Countdown, Playing, Finished, ToppedOut };

SoloSession(SoloMode, std::uint32_t seed, int startLevel);
void update(int ms);        // after the app has advanced the game by ms
bool frozen() const;        // true unless Playing: the app must not tick the game or feed input
int  clockMs() const;       // elapsed (Sprint, Cheese) or remaining (Blitz); 0 for Marathon and Zen
std::optional<int> result() const;  // set once Finished: time in ms, or Blitz score
int  progress() const;      // Sprint: lines (of 40); Cheese: garbage rows left; else 0
std::vector<SoloEvent> takeEvents();  // CountdownTick, Start, Finished
Game& game();
```

| Mode | Start | Clock | Finishes when | Result | Top-out |
|---|---|---|---|---|---|
| Marathon | menu level | none | never (game over ends it) | none | normal GAME OVER |
| Sprint 40L | level 1 | counts up | `lines() >= 40` | time (ms) | ToppedOut, no record |
| Blitz 2:00 | menu level, `linesPerLevel = 5` | counts down from 120000 | clock reaches 0 | score | ToppedOut, no record |
| Cheese 10L | level 1, 10 cheese rows | counts up | `garbageRows() == 0` | time (ms) | ToppedOut, no record |
| Zen | menu level | none | never | none | `resetBoard()`, play continues |

Rules:
- **Countdown:** Sprint, Blitz and Cheese begin with a `kCountdownMs` 3-2-1 countdown, using the existing countdown tick sound cue. Marathon and Zen start straight into Playing.
- **Finish checks** run in `update()` after the game has ticked. In Blitz at 0:00, a clear that's still animating is ticked through first so it scores, the same way `DuelSession` flushes at 0:00. In Sprint and Cheese the clear that reaches the goal has to finish (`state() == Playing`) before `lines()` / `garbageRows()` reflect it. The finishing time is the clock at that moment.
- **Pausing** stays allowed (`allowPause = true`). The clock only advances while the game state is `Playing` or `Clearing`, so pausing never costs or gains time. The board is already hidden while paused.
- **Restart** creates a new `SoloSession` with a fresh seed and the same mode.

## 3. Personal bests (in the existing `Settings`)
PBs go into the settings that are already saved between launches (`src/app/settings.{hpp,cpp}`, stored by `settings_store_desktop.cpp` / `settings_store_web.cpp`). That means no new storage files.
- `solo.hpp` defines `struct PersonalBests { std::optional<int> sprintMs, cheeseMs, blitzScore; }` with `std::optional<int> best(SoloMode) const` and `bool submit(SoloMode, int result)`. `submit` returns true when the run is a new PB: a lower time, or a higher score. The first finished run is always a PB. The struct sits in `tetris_duel` because `tetris_settings` already depends on that library, so the reverse would be circular.
- `Settings` gains `PersonalBests bests`. `serializeSettings` writes `sprint=`, `cheese=` and `blitz=` lines, only when set. `parseSettings` reads them and skips values that aren't positive integers, as it already does for every other bad entry.
- After a new PB, `App` calls the existing `saveSettings()`. Corrupt or missing values just read as "no PB".

## 4. Screens and app wiring
- **Start screen:**
  - **SOLO** section: buttons for Marathon, Sprint 40L, Blitz 2:00, Cheese 10L and Zen. Timed modes show their PB in dim text beside the button (`PB 1:02.31`, `PB 48,200`).
  - The start-level slider stays, captioned "Marathon, Blitz, Zen".
  - **PRACTICE** section: `vs Bot` with an Easy / Medium / Hard selector, on every build.
  - **1v1 DUEL** (create/join room) stays browser-only, as now.
- **HUD:** the existing playfield plus a mode line in the side panel:
  - Sprint `0:42.18` and `31 / 40`
  - Cheese `0:18.04` and `4 left`
  - Blitz `1:12` counting down, red under 10 s like the duel clock
  - Zen `ZEN` and the line count
  - The countdown overlay reuses the duel's countdown drawing.
- **Result overlay** for timed modes (replaces GAME OVER):
  - Title `FINISHED` or `TOPPED OUT`.
  - The result in large text.
  - `NEW PB!` or `PB <best>`.
  - **Retry** and **Quit to menu** buttons. R retries.
  - A new PB plays the existing win cue.
  - Marathon keeps today's GAME OVER overlay. Zen has no result, and you leave from the pause menu.
- **`App`:**
  - `std::optional<Game> game` becomes `std::optional<SoloSession> solo` on `Screen::Playing`.
  - `UiCommand::Play` becomes `PlaySolo` with `menu.soloMode`.
  - New `UiCommand::PlayBot` with `menu.botLevel`.
  - `activeGame()` returns the solo game only while `!solo->frozen()`.
  - Music follows the solo game as it does today.
- **Duel screen:** unchanged. The bot's name arrives through the existing `OpponentName` message (section 6), so the name bar, the VS line and "… left" all show it with no render changes.

## 5. Bot brain (`src/bot/`, new `tetris_bot` library: depends on `tetris_core` only)
`planner` and `bot_player` make up `tetris_bot`. `local_match` (section 6) needs `protocol` and `makeDuelGame`, so it builds in `tetris_duel`, which links `tetris_bot`.

### Planner
- For the active piece, and for the hold option (the held piece, or the next piece if hold is empty), it lists the input sequences `[Hold] + {none, CW, CW×2, CCW} + 0–9 × Left/Right + HardDrop`.
- Each sequence is simulated on a **copy of the `Board`** using `Board::collides` and `rotateWithKicks`, the same calls `Game` makes, so planned and executed results match. Sequences that land on an already-seen final placement are dropped. There are no tucks or spins, which leaves about 80 candidates per piece at most.
- Each resulting board (after locking, removing full rows, and removing the rows `bombRowsHitBy` reports) is scored with named-constant weights:
  - `−0.51 × aggregateHeight − 0.36 × holes − 0.18 × bumpiness + 0.76 × linesCleared`
  - `+ kBombBonus × rowsExploded`
  - `− kDangerPenalty × max(0, maxHeight − 14)`
- Output: candidates ranked best first, each with its action list.

### `BotPlayer`
```
enum class BotLevel { Easy, Medium, Hard };
BotPlayer(BotLevel, std::uint32_t seed);
void update(Game&, int ms);   // only ever calls game.apply()
```
- When the game is `Playing` and no plan is queued, the bot waits `thinkMs`, plans, then applies one action every `inputMs`.
- With probability `mistakeRate` it takes the 2nd or 3rd ranked candidate (chosen at random) instead of the best.
- Gravity may move the piece while inputs are being applied. A shift that's blocked is harmless, the plan always ends in a hard drop, and the next piece gets a fresh plan.
- Seeded `std::mt19937`, so the same seed gives the same game.

| Preset | thinkMs | inputMs | mistakeRate | target pace |
|---|---|---|---|---|
| Easy | 900 | 120 | 25 % | ~0.8 pieces/s |
| Medium | 450 | 60 | 8 % | ~1.5 pieces/s |
| Hard | 200 | 25 | 0 % | ~2.5 pieces/s |

## 6. Local referee (`src/bot/local_match.{hpp,cpp}`)
`LocalMatch : Connection` stands in for `server/hub.go` plus a remote client, inside the same process. Because of it, `DuelSession` works unchanged against the bot.

- **`Connection` gains `virtual void update(int ms) {}`**, a no-op by default. `App::step` calls `connection->update(ms)` after `duel.update(ms)`, so the human's last attack before 0:00 is queued before the referee checks the clock.
- **Messages from the human** (decoded with `protocol`):
  - `CreateRoom{level, isPublic}` → queue `OpponentName{"Bot-Easy" | "Bot-Medium" | "Bot-Hard"}` (valid under `isValidName`), then `StartMatch{seed, level}`. There is no room code, so the "CONNECTING" panel shows for one frame.
  - `Attack{n}` → `bot.receiveGarbage(n)`, and add n to the human's lines sent.
  - `Dead` → KO for the bot.
  - `Rematch` → the bot always accepts, so queue a new `StartMatch` with a fresh seed.
  - `Snapshot` → ignored.
  - `Hello`, `JoinRoom`, `QuickMatch`, `ListRooms`, `UnlistRooms` → ignored.
- **The bot's side:**
  - Its own `Game` from `makeDuelGame(start)`, so it gets the same seed and the same pieces.
  - It doesn't tick during the `kCountdownMs` countdown.
  - `BotPlayer::update`, then `game.tick`, in 1 ms steps.
  - `takeOutgoingAttack()` → `Attack`, added to the bot's lines sent.
  - `snapshotOf()` every `kSnapshotIntervalMs` when it has changed.
  - On top-out: `resetBoard()`, a KO for the human, then frozen for `kKoFlashMs`.
- **Refereeing**, matching `hub.go`:
  - `KoCounts` to the human after every KO.
  - The first to `kKosToWin` → `MatchOver{KOs}`.
  - At `kCountdownMs + kMatchMs + 500 ms` → `MatchOver{Time}`.
  - The verdict compares KOs, then lines sent, otherwise it's a draw.
  - Attacks and deaths after the match ends are ignored, and the bot stops ticking.
- `receive()` hands over the queued messages, already encoded. `closed()` is always false. Leaving is the existing `closeDuel()`.
- **App:** `openBotDuel()` first runs `closeDuel()` if a connection is open, so a lobby socket never lingers. Then it sets `connection = std::make_unique<LocalMatch>(menu.botLevel, seed)` (no `Hello`, which `openDuel()` sends only for real servers), `screen = Screen::Duel`, and calls `duel.createRoom(menu.startLevel)`. `main.cpp`'s `frameBudgetMs` catch-up already covers any duel in a match, so a hidden tab replays the gap for the bot too.

## 7. Testing
All tests use doctest and live in `tetris_tests` unless noted.
- **Core:**
  - `linesPerLevel` changes `levelFor` and in-game level-ups.
  - `pushCheese` leaves one hole per row, neighbouring holes differ, and there are no bombs.
  - `garbageRows` counts correctly before and after clears.
- **SoloSession** (scripted games built from test boards and helpers):
  - Sprint finishes at exactly 40 lines with the right time.
  - Blitz finishes at 0:00 and counts a clear that's still animating.
  - Cheese finishes when the last garbage row clears.
  - Top-out gives ToppedOut with no result.
  - Zen resets and keeps going.
  - The clock doesn't advance while paused.
  - The countdown blocks play and emits ticks.
- **PBs in settings** (`settings_test.cpp`):
  - The three PB fields round-trip through serialize and parse, and an unset field stays unset.
  - Bad values are skipped.
  - `submitRecord` direction is correct for times and for score.
  - The first run is always a PB.
- **Planner:**
  - An O piece on an empty board gives 9 distinct placements.
  - It fills the gap in a 9/10 row.
  - It prefers landing on a bomb.
  - Hold candidates appear only when `canHold()`.
- **BotPlayer:**
  - Hard with a fixed seed and no garbage survives 500 pieces.
  - Simulated pace over 60 s is within ±25 % of each preset's target.
  - Same seed gives the same board.
- **LocalMatch** driven by a real `DuelSession`:
  - Three bot top-outs give Won/KOs.
  - Time-up verdict cases (KO lead, then lines sent, then draw) mirror `server/hub_test.go`.
  - Garbage reaches both sides.
  - A rematch restarts with a new seed.
  - The session's `opponentName()` is the bot's name.
  - Nothing is accepted after `MatchOver`.
- **UI** (`tetris_ui_tests`): the pure text helpers (`formatClock`, `formatScore`) are unit-tested. The bot path from the menu (`LocalMatch` reaching `DuelPhase::Countdown` with a real `DuelSession`) is covered by the LocalMatch tests, because `app.cpp` is not linked into any test target. The menu is checked by hand on the desktop build.
