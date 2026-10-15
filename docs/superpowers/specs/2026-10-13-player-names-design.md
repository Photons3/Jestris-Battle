# Design: Player names

> **Partly superseded** by `2026-10-14-multiplayer-rooms-design.md`: `OPP` is removed; names come from the `PLAYERS` roster, with room-unique suffixes for duplicate names.

## Context
Duels today are anonymous: the hub's `player` is just a WebSocket, and the duel screen's name bars say YOU and FOE (`drawDuelBoards` in `src/app/render.cpp`). This spec lets each player pick a display name that is remembered between visits and shown to the opponent. It extends `2026-10-13-lan-duel-design.md` and `2026-10-13-bomb-battle-design.md`, and it is the base for the room lobby in `2026-10-13-room-lobby-design.md`, which lists rooms by the host's name.

Solo play and the desktop build behave as today, except that the name is stored in the settings file on both.

## Goals and non-goals
- **Goals:** a name field on the start screen; a random default so nobody is nameless; the name saved with the other settings; the server learning each player's name and telling each player their opponent's name; both names on the duel screen.
- **Non-goals:** accounts, passwords, unique names, ranking, profanity filtering (see "Later"), changing names during a match, Unicode names.

## 1. Name rules
- **Length:** 1 to 12 characters (`kMaxNameLength = 12` in `src/net/protocol.hpp`, `maxNameLength` in `server/hub.go`). Twelve characters fit the 10-cell name bar at its current text size; check this against the widest glyphs (`W`) when implementing and lower it if needed.
- **Characters:** `A-Z a-z 0-9 _ - .` only. No spaces (the protocol splits on spaces), no `:` (the lobby's `ROOMS` entries use it as a separator), no `=` or newline (the settings format is `name=value` per line).
- **Validation lives in one pure function on each side:** `bool isValidName(std::string_view)` next to `isRoomCode` in `src/net/protocol.cpp`, and `validName(string) bool` in the hub. The server is authoritative: it never trusts the client's check.
- **Default:** `Player` followed by 4 random digits (`Player4821`), generated the first time settings are loaded without a name and saved straight away, so the player keeps it across visits.
- **Duplicates are allowed.** Identity is per connection; two `Player4821`s in one match is harmless.

## 2. Settings (`src/app/settings.{hpp,cpp}`)
- `Settings` gains `std::string playerName;` (empty means "not chosen yet").
- `serializeSettings` writes `name=<playerName>`. `parseSettings` reads it and keeps it only if `isValidName` passes; otherwise it stays empty, so a hand-edited or corrupt file never puts a bad name on the wire.
- After loading, the app fills an empty name with the random default and saves. The random digits come from the app's existing RNG source (not the bag), so tests can inject it.
- The settings version stays the same: an old file simply has no `name=` line.

## 3. Start screen
- In the browser build the "1v1 DUEL" section gets a **Name** field as its first row, above Create room, spanning the menu width: `ImGui::InputTextWithHint("##name", "NAME", ...)` with the same width rules as the other rows.
- Typing is filtered with an `ImGuiInputTextFlags_CallbackCharFilter` callback that drops any character outside the allowed set, and the buffer is `kMaxNameLength + 1` bytes, so the field can never hold an invalid name except an empty one.
- The name is committed when the field loses focus or Enter is pressed (`ImGui::IsItemDeactivatedAfterEdit()`): a non-empty value is saved with `App::saveSettings()`; an empty value reverts to the last saved name.
- While the field is being edited, game keys and menu shortcuts are not triggered. ImGui's `WantTextInput` already covers this for the join code box; the name field is handled the same way.
- The desktop start screen has no duel section and so no name field. The setting still exists there (harmless, and it keeps one settings format).
- `StartMenu` (`src/app/render.hpp`) gains `char playerName[kMaxNameLength + 1]`, filled from `Settings` at startup.

## 4. Protocol

| Message | Direction | Meaning |
|---|---|---|
| `HELLO <name>` | client -> server | My display name. Sent as the first message on every new connection. |
| `OPP <name>` | server -> client | Your opponent's name. Sent to both seats immediately before the first `START` of a room. |

- `HELLO` is sent by `App::openDuel()` right after `connectToServer()`. `Connection` already queues messages sent before the socket opens, so it always arrives before `CREATE`/`JOIN`.
- `HELLO` is accepted only while the player is not in a room; inside a room it is dropped, so names can't change mid-match. A later `HELLO` outside a room replaces the stored name. That is the hook for the lobby, where a player stays connected between rooms.
- An invalid name, or a connection that never sends `HELLO` (an old client), is named `Player` on the server.
- `OPP` is sent once per room, when the second player joins, before `START`. Rematches reuse it because names can't change inside a room. It is added to the server-only verb list in `Hub.route` (`ROOM, START, LEFT, ERR, KO, OVER, OPP`), so a client can't forge it. `HELLO` is handled by the hub and never relayed.
- Codec: `HelloName{std::string name}` and `OpponentName{std::string name}` join the `Message` variant. `decode` requires exactly one argument that passes `isValidName`; anything else is nullopt.
- Back-compatibility: an old client ignores `OPP` (it decodes to nothing) and never sends `HELLO`; a new client against an old server gets no `OPP` and shows the fallback `FOE`.

## 5. Client (`DuelSession`, render)
- `DuelSession` gains `const std::string& opponentName() const` (empty until known).
- `OPP` is accepted only in `Waiting` (it always precedes `START`); everywhere else it is ignored, per the existing phase-gating rules. `reset()` clears it, so leaving a room forgets the name.
- `drawDuelBoards` draws the local player's name on the left name bar and `opponentName()` on the right, falling back to `YOU` and `FOE` when either is empty. The bar colours stay as they are (blue/purple for you, red/orange for the opponent), so a duel between two `Player`s is still easy to read.
- The countdown overlay shows `<you> vs <them>` under the big digit.
- The result title stays `YOU WIN` / `YOU LOSE` / `DRAW`. The reason line from `resultReasonText` gets the opponent's name where it says "Opponent", for example "Sam left the match".

## 6. Server (`server/hub.go`)
- `player` gains `name string`, initialised to `Player`.
- `route` handles `HELLO` before the `p.room == nil` switch: valid and not in a room, store it; otherwise drop it.
- `join` sends `OPP <name>` to both seats (each gets the other's name) and then the `START` messages that `startMatch` already returns.

## 7. Testing
- **Protocol (doctest):** `isValidName` table (empty, 12 and 13 characters, each allowed punctuation, space, `:`, `=`, non-ASCII); `HELLO`/`OPP` round trip; malformed `HELLO`/`OPP` (no argument, two arguments, invalid name) decode to nullopt.
- **Settings (doctest):** `name=` round trip; an invalid stored name parses as empty; a file without `name=` parses as empty.
- **Duel session (doctest):** `OPP` in Waiting sets the name; `OPP` in any other phase is ignored; leaving clears it.
- **Server (`go test -race`):** names from `HELLO` reach the opponent in `OPP` before `START`; a missing or invalid `HELLO` gives `Player`; `HELLO` inside a room is ignored; a client's `OPP` is not relayed.
- **Manual:** two tabs with different names see each other's names on the bars and the countdown; a reload keeps the name; an empty field reverts; typing a space or `:` does nothing.

## Decisions
- **Free-form names, no accounts:** there is no persistent server state today, and names are cosmetic.
- **A strict ASCII character set** instead of escaping, so every existing space-split parser and the `name=value` settings format keep working unchanged.
- **`HELLO` per connection** instead of a name argument on `CREATE`/`JOIN`: one place to send it, and the lobby needs the name before any room exists.
- **A random default** instead of a mandatory prompt, so the existing two-click "Create room, share the code" flow doesn't get a new step.

## Later
- A small server-side blocklist that maps offensive names to `Player` before they appear in the public room list.
- Per-player colours chosen alongside the name.
