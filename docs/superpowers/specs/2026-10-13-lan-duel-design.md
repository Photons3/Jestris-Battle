# Design: LAN 1v1 Tetris duels

> **Partly superseded:** garbage gaps, "first top-out loses" and relaying `DEAD` are replaced by bomb garbage, KO matches and the server referee in `2026-10-13-bomb-battle-design.md`.

## Context
The single-player Tetris in `docs/superpowers/specs/2026-10-13-tetris-design.md` (C++20, SDL3, Dear ImGui, pure `tetris_core`) is extended so that friends on the same LAN can play 1v1 duels with garbage attacks. One person runs a small Go server on their Mac and everyone else opens `http://<host-ip>:8080` in a browser. The game is compiled to WebAssembly, so there is nothing to install on the other devices. The native desktop build stays single-player and unchanged.

The design was settled in a brainstorming session; the "Changes since brainstorming" list at the end records where the implementation plan (`docs/superpowers/plans/2026-10-13-lan-duel.md`) departs from it.

## Goals and non-goals
- **Goals:** two players per room, joined by a 4-letter code; both play the same piece sequence; clears send garbage to the opponent; each player sees the opponent's board live; rematch; clean handling of disconnects and bad input.
- **Non-goals:** more than two players, matchmaking beyond room codes, accounts, spectators, internet play or TLS, anti-cheat, T-spin/combo/back-to-back attack bonuses, a duel mode on the desktop build, replays.

## 1. Architecture and components
Each browser simulates **only its own board** (sync model A: own board plus events). The server never runs Tetris; it pairs players, picks the seed, and relays messages. The opponent's board is display-only, rebuilt from periodic snapshots.

Dependency direction is `app -> net -> core`, with the server as a separate program that shares only the text protocol.

| Component | Location | Role |
|---|---|---|
| `tetris_core` (extended) | `src/core/` | `PieceType::Garbage`, `Board::pushGarbage`, and in `Game`: attack table, pending-garbage queue, outgoing-attack counter, a no-pause option. Still pure C++, no SDL or ImGui. |
| Wire protocol | `src/net/protocol.{hpp,cpp}` | Message structs, `encode`/`decode`, and `Snapshot` (the visible 10x20 field with the falling piece merged in). Pure and unit-tested. |
| `DuelSession` | `src/app/duel.{hpp,cpp}` | Client-side state machine for one player's duel. Owns the local `Game` during a match; sends attacks, snapshots and top-out through a `send` callback. No sockets, no SDL. |
| `Connection` | `src/net/connection.hpp`, `ws_client_web.cpp`, `connection_none.cpp` | Text-message link to the server. Browser build uses the Emscripten WebSocket API; the desktop build gets a stub, and `duelsSupported()` is false there. |
| App layer | `src/app/app.{hpp,cpp}` | Platform-independent app logic split out of `main.cpp`: screens, command handling, `step`, `drawFrame`, `pumpNetwork`. |
| Graphics backend | `src/app/gfx.hpp`, `gfx_sdlgpu.cpp`, `gfx_webgl.cpp` | One small interface; SDL_GPU on desktop, WebGL in the browser. |
| Render | `src/app/render.{hpp,cpp}` | Start menu, waiting room, countdown, side-by-side duel view, result menu. |
| Relay server | `server/` (Go, module `tetris-server`) | `Hub` (rooms, codes, seeds, relay) and an HTTP file server for the web build. One dependency: `github.com/coder/websocket`. |

Because every player loads the same `index.wasm` from the host, the piece order for a given seed is identical on both sides by construction.

## 2. Garbage rules
Deliberately simple guideline-ish rules:

- **Attack table:** clearing 1 / 2 / 3 / 4 lines sends 0 / 1 / 2 / 4 garbage lines. No T-spin, combo or back-to-back bonuses.
- **Cancelling:** the attack from a clear first cancels the player's own pending garbage; only the remainder is sent to the opponent.
- **Receiving:** incoming `ATK n` is added to the pending count. It does not appear on the board immediately.
- **Rising:** pending garbage rises only when a piece **locks without clearing any lines**, all at once. The whole batch shares one randomly chosen gap column; each batch picks its own column. A lock that clears lines holds garbage back until the next non-clearing lock.
- **Garbage cells:** a new cell type, `PieceType::Garbage`, drawn gray. It is never dealt by the bag (the dealable type count stays 7).
- **Top-out:** if raising garbage would push a filled cell off the top of the board, or leaves no room for the next piece to spawn, the player is game over. This holds for any size of batch, including one larger than the board, and never crashes. A refused push leaves the board unchanged.
- **Gap column RNG:** each client uses its own generator seeded from the match seed with a fixed salt, so gap columns are independent of the piece bag. It does not need to match the opponent's.
- **No pausing in duels:** the duel game is created with pausing disabled, so neither player can freeze the match (Esc, P and focus loss do nothing to the game).
- Pending garbage is visible to both players as a red meter beside the well.

## 3. Protocol and session state machine

### Transport and framing
Plain-text WebSocket messages, one message per frame, `VERB arg arg ...`, space-separated. The server reads at most 4096 bytes per message. Anything malformed, unknown, out of range or with the wrong argument count decodes to nothing and is ignored. Start level is 1-15; room codes are 4 uppercase letters from `ABCDEFGHJKLMNPQRSTUVWXYZ` (no I or O).

### Messages
| Message | Direction | Meaning | Who reads it |
|---|---|---|---|
| `CREATE <lvl>` | client -> server | Open a room with start level `lvl` | Server (replies `ROOM` or `ERR bad start level`) |
| `JOIN <CODE>` | client -> server | Join room `CODE` | Server (replies `START` to both, or `ERR unknown room` / `ERR room is full`) |
| `ROOM <CODE>` | server -> creator | Room created; show this code | Client |
| `START <seed> <lvl>` | server -> both | Begin a match with this bag seed and start level | Client |
| `ATK <n>` | client -> opponent, relayed unchanged | `n` (1-40) garbage lines sent | Opponent client; server only forwards |
| `SNAP <score> <lines> <pending> <ready> <hold> <next> <200 cells>` | client -> opponent, relayed unchanged | Board snapshot: score, lines, pending garbage; `<ready>`, the part of pending garbage that can rise now (see `2026-10-14-garbage-delay-cap-design.md`); the held piece (`.` for none, `IOTSZJL`, lowercase while hold is locked); the next 5 pieces (`.` or `IOTSZJL`); then 200 characters, row-major from the bottom row, `.` for empty, `IOTSZJLGB` for pieces, garbage and bombs, and `iotszjl` for the falling piece's ghost. Hold, next and the ghost were added later, see `2026-10-14-opponent-hold-next-design.md` | Opponent client; server only forwards |
| `DEAD` | client -> opponent, relayed unchanged | Sender has topped out | Opponent client; server only forwards |
| `REMATCH` | client -> server | Sender wants another match | Server (restarts when both seats have asked) |
| `LEFT` | server -> remaining player | The opponent disconnected | Client |
| `ERR <reason>` | server -> client | Request refused, with a human-readable reason | Client (shown on the start screen) |

The server understands only `CREATE`, `JOIN` and `REMATCH`, and produces `ROOM`, `START`, `LEFT` and `ERR`. Game verbs sent from inside a room are forwarded to the other player as-is; `ROOM`, `START`, `LEFT` and `ERR` from a client are dropped, since only the server sends them. Traffic from a client that is not in a room (for example a late `SNAP` after `LEFT`) is dropped silently, and `CREATE`/`JOIN` from a client already in a room gets `ERR already in a room`.

### Snapshot timing
Snapshots are sent every 100 ms and only when they differ from the last one sent. When a player tops out, a final snapshot goes out immediately before `DEAD` (only if it changed), so the opponent sees the losing board.

### Session state machine (`DuelSession`)
Phases: `Idle -> Waiting -> Countdown -> Playing -> Result`.

| Phase | Meaning | Entered by | Leaves on |
|---|---|---|---|
| Idle | No duel; the start screen is shown. The initial and terminal state. | construction, `leave()`, `ERR`, connection loss | `createRoom` or `joinRoom` -> Waiting |
| Waiting | Request sent. Creator sees the room code once `ROOM` arrives (before that: "Connecting"). | `createRoom`, `joinRoom` | `START` -> Countdown; `ERR` -> Idle (reason kept as the error); `LEFT` -> Idle ("Opponent left"); `leave()` -> Idle |
| Countdown | Local game exists (same seed as opponent, pause disabled), 3000 ms countdown shown, input ignored. Incoming `ATK` is queued and `DEAD` is honoured (the two clocks drift). | `START` | countdown reaches zero -> Playing; incoming `DEAD` -> Result (Won); `LEFT` -> Result (OpponentLeft) |
| Playing | The local game runs. Outgoing attacks are sent, snapshots go out on the interval, incoming `ATK` queues garbage, `SNAP` updates the opponent view. | countdown finished | local game over -> send final `SNAP` (if changed) and `DEAD`, Result (Lost); incoming `DEAD` -> Result (Won); `LEFT` -> Result (OpponentLeft) |
| Result | Outcome shown with a Rematch / Leave menu. | any of the above | `START` (only after this player requested a rematch and the opponent is present) -> Countdown; `leave()` -> Idle |

`DuelOutcome`: `None` (no result yet, and after a fresh `START`), `Won`, `Lost`, `OpponentLeft`.
- `Lost` comes from the local top-out (which then sends `DEAD`), and `Won` from an incoming `DEAD`; `OpponentLeft` comes from `LEFT` while a match was in progress or counting down, and is presented as a win ("YOU WIN / Opponent left the match").
- `LEFT` also sets an `opponentGone` flag. While it is set, rematch is unavailable: `requestRematch` sends nothing and the Rematch button is hidden. A `LEFT` that arrives during Result keeps the existing outcome and sets the flag.
- `requestRematch` is valid only in Result and sends `REMATCH` once; the UI then shows "Waiting for opponent...". When the server has `REMATCH` from both seats it sends a fresh `START` with a new seed, which resets the session to Countdown with outcome `None`, no pending garbage and no rematch request.

Robustness rules, each pinned by a test:
- **Ignored once idle:** messages arriving in Idle (late `SNAP`/`LEFT`/`START` after leaving) change nothing and set no error.
- **Phase-gated effects:** `ATK` and `DEAD` act only in Countdown or Playing (not Waiting or Result), so stray relay traffic cannot end or restart a match, while drift between the two countdowns loses nothing.
- **START is gated:** accepted only in Waiting, or in Result after this player requested a rematch while the opponent is present. A stray or forged `START` anywhere else is ignored.
- **ERR is fatal only in Waiting:** it returns to Idle with the server's reason, so the player can try again; mid-match it is ignored.
- **LEFT while Waiting** returns to Idle with "Opponent left".
- **Connection loss:** from any non-Idle phase, `onDisconnected` goes to Idle with error "Connection lost".

## 4. Web build and duel UI

### Web build
- Emscripten compiles the same sources to `build-web/index.html`, `index.js`, `index.wasm` (named `index.html` so `/` serves the game). Built with `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release`.
- **No pthreads or SharedArrayBuffer**: plain-HTTP LAN pages are not a secure context, so those features are unavailable.
- SDL3 comes from Emscripten's port (`-sUSE_SDL=3`); ImGui uses its OpenGL3 backend on the default WebGL1 / GLSL 100 path. SDL_GPU is desktop-only.
- A custom HTML shell holds a single full-window canvas and stops arrow keys and space from scrolling the page. The canvas follows the browser window size. ImGui works in CSS pixels (scale 1) while SDL applies the device pixel ratio.
- Tests run natively only; the web target returns before the test targets are defined.

### Start screen
On desktop it is unchanged: level slider, Play, Quit. In the browser it shows the level slider, **Solo**, and a "1v1 DUEL" section with **Create room**, a code box (auto-uppercase, 4 characters, Enter submits) with **Join room** (disabled until 4 letters are typed), and the last error in red under the box. There is no Quit in the browser.

### Duel screens
- **Waiting:** shows "Connecting" until the code arrives, then the code in large type and "Waiting for an opponent to join...", with Cancel. It shows the code only, not the URL, because friends already have the URL.
- **Countdown:** the local well is drawn with a large 3-2-1 overlay.
- **Playing:** side-by-side view. The local well is full size, with the hold panel on its left and next queue, score, level and lines on its right. The opponent's well is on the far right at 0.6 scale, with OPPONENT / SCORE / LINES labels. A red garbage meter (one cell tall per pending line, capped at the visible height) sits beside each well.
- **Result:** both boards stay visible under a "YOU WIN" / "YOU LOSE" menu with Rematch and Leave; see the state machine for the opponent-left variant.
- Keys control the local game only while a match is Playing (not in countdown or result). Held keys are released between matches, so a direction held through the end of a match does not auto-move the first piece of the rematch.

## 5. Server, error handling and testing

### Server
- `go -C server run .` serves `../build-web` and the `/ws` endpoint on `0.0.0.0:8080`; flags `-addr` and `-dir` override. It refuses to start if `index.html`, `index.js` or `index.wasm` is missing and prints the build command. On start it prints every LAN IPv4 URL plus localhost.
- **Static files are an allowlist:** only `/` (as `index.html`), `/index.html`, `/index.js` and `/index.wasm` are served; everything else, including directory listings and the rest of the build directory, is 404. `.wasm` is served as `application/wasm`.
- **Hub:** a mutex-guarded map of rooms. A room has two seats, the creator's start level, and a rematch flag per seat. Room codes are 4 letters and re-rolled on collision; seeds are random 32-bit values.
- **Joining:** a second player's `JOIN` fills the room and sends `START` to both. A third gets `ERR room is full`; an unknown code gets `ERR unknown room`; `CREATE` with a bad level gets `ERR bad start level`.
- **Leaving:** when a player's connection ends the room is deleted and the other player (if any) is detached from it and sent `LEFT`. Their later traffic is dropped as "not in a room".
- **Rematch:** each seat's `REMATCH` is recorded; when both are set the flags clear and a new `START` with a new seed goes to both.
- **Writes** happen outside the lock with a 2 s timeout; a failed write closes that connection and its read loop does the cleanup.

### Error handling
| Situation | Behaviour |
|---|---|
| Opponent disconnects mid-match | "YOU WIN / Opponent left the match", Rematch hidden, Leave returns to the menu |
| Wrong or full room code | Error shown on the start screen; the player can try again; no hang |
| Server stopped or unreachable | Back to the start screen with "Connection lost" |
| Garbage larger than the board, or junk on the wire | Top-out, never a crash; malformed messages are ignored |
| Messages at the wrong time (late `SNAP` after `LEFT`, `START` after leaving, relay before joining) | Ignored; they never kick a player to the menu or restart a match |

### Testing
- **Core (doctest, native):** `pushGarbage` (gap column, refusal on overflow, zero rows), attack table, cancelling both ways, rising only on non-clearing locks, held back after a clear, top-out from oversized garbage or no spawn room, pause disabled.
- **Protocol (doctest):** round trip of every message, exact wire format, a table of malformed inputs, snapshot content (falling piece merged in, pending count).
- **Duel session (doctest):** every phase transition and rule listed in section 3, driven through a fake `send` callback with an injected game factory for line-clear scenarios.
- **Server (`go test -race`):** create/join, relay, join errors, bad level, traffic outside a room, disconnect notification, rematch needing both players, server-only verbs not relayed, the static-file allowlist (including the `.wasm` MIME type); with deterministic codes and seeds injected.
- **Manual:** the duel UI is verified by hand because there is no headless ImGui harness for the duel screens. Checklist: two browser tabs on localhost (create, join, countdown, garbage, win/lose, rematch, opponent closing a tab, bad code, server stopped), then two real devices on the LAN, including resizing and phone-landscape width.
- Desktop solo play must be unchanged: existing tests keep passing under ASan/UBSan.

## Decisions
- **Simple garbage:** the fixed 0/1/2/4 table with cancelling and batched, non-clearing-lock rising; no combo or T-spin bonuses.
- **Room codes:** 4-letter codes created by one player and typed by the other, instead of discovery or matchmaking.
- **Go server:** a small standalone Go program serves the web build and relays WebSocket messages; the C++ code stays a client only.
- **Browser-only duels:** duels exist only in the WebAssembly build served by the host, so friends need no install and the desktop build is untouched.
- **Sync model A (own board plus events):** each client simulates only its own board and sends attacks plus periodic snapshots, instead of lockstep or a server-authoritative simulation.

## Changes since brainstorming
- Snapshots are sent on the 100 ms interval only, not also on every lock.
- The server uses `math/rand/v2` instead of `crypto/rand`; secrecy isn't needed.
- The bag-order pin test is dropped: every player loads the same `index.wasm` from the host, so the piece order is identical by construction.
- Web output is named `index.html`, so `/` serves the game.
- The web build uses ImGui's default WebGL1/GLSL 100 path.
- The waiting screen shows the code only, since friends already have the URL.
- Duel UI is verified manually, because there is no headless ImGui harness for the duel screens.

## Changes during implementation
- `ATK` and `DEAD` are honoured in Countdown as well as Playing, because the two clients' countdowns drift (hidden tabs get no frames; frames are clamped to 100 ms).
- `START` in Result is accepted only after this player requested a rematch (and the opponent is present).
- `ERR` is fatal only in Waiting; `LEFT` in Waiting returns to Idle with "Opponent left".
- The server relays only game verbs and drops `ROOM`, `START`, `LEFT` and `ERR` sent by clients.
- The server serves only `index.html`, `index.js` and `index.wasm` instead of the whole build directory.
- `Lost` comes from the local top-out (which then sends `DEAD`) and `Won` from an incoming `DEAD`; the final snapshot goes out only if it changed.
- A top-level `README.md` documents the native, web and server workflows.
