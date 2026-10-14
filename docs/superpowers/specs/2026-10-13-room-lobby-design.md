# Design: Room lobby, quick match and invite links

## Context
Today the only way into a duel is a private 4-letter code: one player creates a room and the other must be told the code (`server/hub.go`, `DuelSession::createRoom`/`joinRoom`). This spec adds ways to find a game without passing a code around: public rooms listed in a lobby, a one-button quick match, and invite links. Private code rooms keep working exactly as they do now.

It builds on `2026-10-13-player-names-design.md` (the lobby lists rooms by the host's name, so names ship first) and extends `2026-10-13-lan-duel-design.md`. Match rules, the duel screen and the desktop build are unchanged.

## Goals and non-goals
- **Goals:** a public/private choice when creating a room; a live list of open public rooms you can join with one click; Quick match, which joins the oldest open public room or opens one; `?room=CODE` invite links; error handling that returns you to the list instead of the start screen.
- **Non-goals (v1):** spectators, more than two players per room, rooms that outlive their host, King of the Hill rotation, skill-based matchmaking, chat, room passwords, persistence across server restarts. See "Later".

## 1. Concepts
- **Room visibility:** every room is `public` or `private`. Private is the default and matches today's behaviour. A public room also has a code, and the code still works for joining it.
- **Open room:** a room with one player and a free seat. Only open public rooms appear in the lobby. A room leaves the list the moment its second player joins, and is deleted when either player leaves (unchanged).
- **Lobby watcher:** a connected player who is not in a room and has asked for the list. The server pushes list updates to watchers only.

## 2. Protocol

### Changed
| Message | Change |
|---|---|
| `CREATE <lvl> [PUB\|PRIV]` | Optional second argument. Missing means `PRIV`, so old clients keep working. Anything else gets `ERR bad room options`. |

### New
| Message | Direction | Meaning |
|---|---|---|
| `LIST` | client -> server | Start watching the lobby. The server replies with `ROOMS` at once and again whenever the list changes. |
| `UNLIST` | client -> server | Stop watching. Also implied by `CREATE`, `JOIN` and `QUICK`. |
| `ROOMS [<CODE>:<lvl>:<host>]...` | server -> watcher | The full current list of open public rooms, oldest first, at most 20 entries. Zero entries is the bare word `ROOMS`. |
| `QUICK <lvl>` | client -> server | Join the oldest open public room, or, if there is none, create a public room at level `lvl`. |

- `ROOMS` entries use `:` as separators, which names and codes never contain (see the names spec). At 20 entries of at most `4+1+2+1+12` characters plus spaces the message stays under 450 bytes, well inside the 4096-byte limit.
- `QUICK` replies the same way the existing verbs do: `ROOM <code>` if it created a room (the player then waits as a creator), or `OPP` + `START` if it joined one. The client needs no new reply type.
- **Quick match and levels:** a joiner plays at the room's start level, not their own. `START` already carries the level, and the countdown shows it ("Level 7"). Matching by level is listed under "Later".
- `ROOMS` is added to the server-only verb list; `LIST`, `UNLIST` and `QUICK` are handled by the hub and never relayed. Inside a room, `LIST`/`UNLIST` are dropped and `QUICK` gets `ERR already in a room`, like `CREATE`/`JOIN` today.
- Codec additions in `src/net/protocol.{hpp,cpp}`: `CreateRoom` gains `bool isPublic`; new `ListRooms`, `UnlistRooms`, `QuickMatch{int startLevel}`, and `RoomList{std::vector<RoomInfo>}` with `RoomInfo{std::string code; int startLevel; std::string host;}`. A `ROOMS` message with any malformed entry decodes to nullopt as a whole.

## 3. Server (`server/hub.go`)
- `room` gains `public bool`, `host string` (the creator's name at creation time) and `seq uint64`, a hub-wide counter that orders rooms oldest first.
- `Hub` gains `watchers map[*player]bool`.
- **Broadcasting:** any change to the open public set (public create, join, quick, leave of an open room) builds one `ROOMS` message and appends it to the `outgoing` list for every watcher. This happens under the existing lock and is sent by the existing `deliver`, so there is no new goroutine. The open set is computed by scanning `rooms`. At LAN and small-VPS scale (hundreds of rooms) that is cheaper than keeping a second index in sync.
- **Quick:** under the lock, pick the open public room with the lowest `seq` that the caller didn't create; join it if there is one, otherwise `create(p, lvl, public)`. Two simultaneous `QUICK`s therefore pair with each other instead of both opening rooms, because the second sees the first's room.
- **Leaving** drops the player from `watchers` as well as from its room.
- **Limits:** `maxRooms = 500` total. A `CREATE` or a room-opening `QUICK` beyond that gets `ERR server is full`. The list is capped at 20 entries; older open rooms stay joinable and are still picked first by Quick match.

## 4. Client

### Session (`DuelSession`)
- New phase **`Lobby`**, between Idle and Waiting: connected, not in a room, watching the list. `DuelPhase` becomes `Idle, Lobby, Waiting, Countdown, Playing, Result`.
- New calls: `openLobby()` (sends `LIST`, Idle -> Lobby) and `quickMatch(int startLevel)` (sends `QUICK`, Idle or Lobby -> Waiting). `createRoom` gains a `bool isPublic` argument, and `createRoom`/`joinRoom` are now also valid from Lobby.
- `rooms()` returns the last `RoomList`. `ROOMS` is accepted only in Lobby; elsewhere it is ignored.
- `fromLobby_` records whether Waiting was entered from Lobby. If `ERR` arrives in Waiting with that flag set, the session goes back to Lobby instead of Idle: it keeps the error text, re-sends `LIST`, and keeps the connection. This is the common "room is full" case after clicking a stale entry. Without the flag the existing rule applies (back to Idle).
- `music`/`sound_cues`: Lobby uses the menu cue, the same as Idle.

### App (`src/app/app.cpp`)
- New `UiCommand`s: `BrowseRooms` (`openDuel()`, then `duel.openLobby()`), `QuickMatch` (`openDuel()`, then `duel.quickMatch(level)`), `JoinListedRoom` (`duel.joinRoom(code)` without reconnecting), and `TogglePublic`.
- `openDuel()` already connects and switches to `Screen::Duel`. It now also sends `HELLO` (names spec). `closeDuel()` is unchanged and is how Lobby's Back works.
- The Public toggle is saved in `Settings` (`publicRooms=0|1`), default off, so a host who always plays public doesn't re-tick it.

### Start screen (browser only)
The "1v1 DUEL" section becomes, top to bottom:
1. Name field (names spec).
2. **Quick match** button: the one-click path, so it goes first.
3. **Browse rooms** button.
4. **Create room** button with a **Public** checkbox on the same row (button about 2/3 of the width, checkbox 1/3).
5. Code box + **Join room**, unchanged.
6. The last error, unchanged.

### Lobby screen (`DuelPhase::Lobby`)
- Title `OPEN ROOMS`, then an ImGui table with columns **Host**, **Level** and an unlabeled **Join** button per row, oldest first. It scrolls when the rows don't fit, which matters at phone-landscape height.
- The empty state reads "No open rooms yet." with the Quick match and Create public room buttons right under it.
- Footer row: **Quick match**, **Create public room**, **Back**.
- The last error (for example "room is full") shows in red above the table and clears on the next successful action.
- There is no Refresh button: the list is pushed live.

### Waiting screen
- Under the code: "Public room, listed in Browse rooms" or "Private room, share the code", so the host knows whether to send the code around.
- Rooms created by Quick match say "Looking for an opponent..." instead of "Waiting for an opponent to join...".
- An **invite link** row (section 5).

## 5. Invite links (web only)
- **Link format:** `<page origin>/?room=ABCD`. The waiting screen shows it as selectable text. A **Copy link** button uses `navigator.clipboard.writeText` when `window.isSecureContext` is true. Plain-HTTP LAN pages are not a secure context and have no clipboard API, so there the button is hidden and the text is selected for a manual copy.
- **Reading the link:** at startup a small `EM_JS` helper in the web-only code returns `new URLSearchParams(location.search).get('room')` and then strips the parameter with `history.replaceState`, so a reload doesn't re-join a dead room. A valid code is copied into `StartMenu::joinCode`. The desktop stub returns nothing.
- **No auto-join:** the start screen shows an extra highlighted **Join room ABCD** button at the top of the duel section. One click joins. That click is also the user gesture browsers need before audio can start, which an automatic join would not get.

## 6. Error handling
| Situation | Behaviour |
|---|---|
| Clicked a room that filled or closed a moment ago | Back to the lobby with "room is full" / "unknown room"; the list is already updated |
| Quick match while the server is at `maxRooms` with no open room | Start screen error "server is full" |
| Server restarts while in the lobby | "Connection lost" on the start screen (existing rule) |
| Invite link with a malformed code | Ignored; normal start screen |
| Invite link to a room that no longer exists | The join fails with "unknown room" on the start screen |
| Old client creating a room | Private room, never listed (no `PUB` argument) |

## 7. Testing
- **Protocol (doctest):** `CREATE` with and without `PUB`/`PRIV`; `ROOMS` with 0, 1 and 20 entries; malformed entries (missing field, bad code, bad level, invalid name, extra `:`); `QUICK` level range; `LIST`/`UNLIST` with stray arguments rejected.
- **Duel session (doctest):** Idle -> Lobby -> Waiting -> Countdown; `ROOMS` only applies in Lobby; `ERR` in Waiting returns to Lobby when `fromLobby_` is set and to Idle otherwise; `quickMatch` handles both the `ROOM` reply and the `OPP`+`START` reply.
- **Server (`go test -race`, injected codes and seeds):**
  - private rooms are never listed;
  - a public room appears to watchers and disappears when it fills or its host leaves;
  - every change pushes exactly one `ROOMS` to each watcher, and nothing goes to non-watchers;
  - `CREATE`/`JOIN`/`QUICK`/disconnect unsubscribe the player;
  - Quick joins the oldest open public room, opens one when there is none, and two back-to-back `QUICK`s pair up;
  - the 20-entry cap and `maxRooms`;
  - a client's `ROOMS` is not relayed.
- **Manual:** three tabs (two browsing, one creating a public room); a live list update; joining from the list; a stale-entry error; Quick match from two tabs; a private room staying hidden; an invite link on plain HTTP (no Copy button) and behind HTTPS (Copy works); phone-landscape scrolling of a long list.

## 8. Build order
1. Player names (separate spec).
2. Public flag, `LIST`/`UNLIST`/`ROOMS`, Lobby phase and screen.
3. Quick match.
4. Invite links.

Each step ships on its own and stays compatible with the previous one.

## Decisions
- **The server pushes the list instead of clients polling:** it is simpler on the client (no refresh timer), the list is never stale, and the hub already batches writes outside its lock.
- **Only open rooms are listed:** without spectators a full room is useless to show.
- **Quick match ignores level:** with few players, splitting the queue by level would leave everyone waiting. The level is visible in the list and on the countdown.
- **Lobby is a `DuelSession` phase** instead of a separate class, because it shares the connection, the error handling and the transitions into Waiting.
- **Invite links prefill instead of auto-joining,** because of browser audio-gesture rules and to avoid surprise joins from old links.

## Later
- **Spectators:** let a room hold watchers, tag `SNAP` with a seat (`SNAP <seat> ...`), add a two-remote-boards view, and list Playing rooms in the lobby with a Watch button.
- **King of the Hill:** rooms that outlive their host, with the winner staying and the next spectator stepping in.
- **Level-aware Quick match:** prefer rooms within ±2 levels and widen the range after a few seconds.
- **A per-IP cap on open rooms,** if a public VPS gets spammed.
- **An online counter** (`ROOMS` could lead with the number of connected players).
