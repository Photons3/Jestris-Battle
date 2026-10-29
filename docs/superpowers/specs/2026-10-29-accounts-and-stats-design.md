# Design: Accounts, saved settings, and career stats

## Context

Jestris Battle's Go server (`server/`) hosts the web build and the rooms. A browser is a WebSocket. The first message is `HELLO <name>`, and that name lasts for the connection. Match numbers (pieces, attack, KOs, lines sent, and the rest of `GameStats`) exist only on the result screen. Solo bests and the handling, keys, and volume settings live in `localStorage` under `jestris-battle.settings` (`src/app/settings_store_web.cpp`), or in the SDL preferences folder on desktop. The server has no database.

This adds accounts on that same server, stored in a SQLite file on the VM, so a registered player comes back as the same person with the same settings and a career record. Guests keep playing as they do now, and the server writes nothing for them.

The desktop build never talks to this server. It stays as it is.

## Goals and non-goals

- **Goals:**
  - Register a username and a password, and log in again later.
  - Play as a guest with no server row.
  - For a logged-in player, save the settings the game already remembers, the solo bests, and a career record of online duels and royales.
  - Write the online record on the server when the match ends, from the match the server already re-simulates.
  - A logged-in player can change their password and log out.
  - The SQLite file survives a server restart on the VM.
- **Non-goals:**
  - Accounts, login, or a server save on the desktop build.
  - Saving anything for a guest, including moving a guest's local bests onto an account they make afterwards.
  - Vs-Bot matches. They stay off the career record.
  - A list of past matches. The record is running totals.
  - Showing one player's record to anyone else. No leaderboard, and no stats on the room roster.
  - Changing the username after registration.
  - Email, password reset, or recovering a forgotten password. A forgotten password is a lost account.
  - Marathon and Zen personal bests. Those modes stay unscored.
  - Saving the start-level slider or the bot difficulty. Those stay for the visit.

## Decisions

These were settled in conversation. Calls made while writing this down are marked **(call)**.

- A guest types a name and plays. The server stores nothing. The browser's existing `localStorage` settings still work for that guest, on that browser.
- A registered player's username is the name rooms show. The name field is not editable while they are logged in.
- Usernames use the display-name rules already in `validName` / `isValidName`: 1 to 12 of `A-Z a-z 0-9 _ - .`. Uniqueness is case-insensitive, so `Sam` and `sam` are one account. The room still adds a numeric suffix when a guest's name collides with someone already in the room. Two accounts cannot collide.
- Settings that move onto the account are the ones `Settings` already holds, apart from the name and the bests, which are stored beside it: keys, mute, music on or off, both volumes, the public-room tick, and DAS / ARR / SDF (`src/app/settings.hpp`).
- Solo bests are Sprint time, Cheese time, and Blitz score. The browser reports a finished run. The server keeps a new value only when it is better: a lower time for Sprint and Cheese, a higher score for Blitz. A worse or equal report does not replace the stored best. **(call)** A client can claim a best the server never saw. That limit is accepted. The server does not re-simulate solo.
- Online career numbers are written by the server for account players only. Vs-Bot is not among them.
- The username does not change. Password change asks for the current password and a new one.
- **(call)** The record is read on the start menu, and only by the logged-in player.
- **(call)** One account has one live WebSocket. A new connection with the same account disconnects the older one, so a refresh replaces the tab instead of putting the same person in the room twice.
- **(call)** There is no password recovery.

## 1. SQLite

The server opens one SQLite file for its whole life.

- **(call)** Driver is `modernc.org/sqlite`, so the server stays pure Go and the VM does not need CGO.
- **(call)** Flag `-db` (default `jestris.db`, relative to the process working directory). `scripts/install-service.sh` already sets `WorkingDirectory` to the checkout, so the default file sits there and survives `scripts/restart.sh`.
- Enable WAL on open.
- Create the tables below with `CREATE TABLE IF NOT EXISTS` on startup. No migration tool. A missing file is created.

```sql
CREATE TABLE users (
  id            INTEGER PRIMARY KEY,
  username      TEXT NOT NULL COLLATE NOCASE UNIQUE,
  password_hash TEXT NOT NULL,
  created_at    TEXT NOT NULL
);

CREATE TABLE sessions (
  token_hash TEXT PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id),
  created_at TEXT NOT NULL
);

CREATE TABLE settings (
  user_id INTEGER PRIMARY KEY REFERENCES users(id),
  body    TEXT NOT NULL DEFAULT ''
);

CREATE TABLE solo_bests (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id),
  sprint_ms   INTEGER,
  cheese_ms   INTEGER,
  blitz_score INTEGER
);

CREATE TABLE career (
  user_id          INTEGER PRIMARY KEY REFERENCES users(id),
  duel_wins        INTEGER NOT NULL DEFAULT 0,
  duel_losses      INTEGER NOT NULL DEFAULT 0,
  duel_draws       INTEGER NOT NULL DEFAULT 0,
  kos_dealt        INTEGER NOT NULL DEFAULT 0,
  kos_taken        INTEGER NOT NULL DEFAULT 0,
  royale_matches   INTEGER NOT NULL DEFAULT 0,
  royale_wins      INTEGER NOT NULL DEFAULT 0,
  royale_place_sum INTEGER NOT NULL DEFAULT 0,
  pieces           INTEGER NOT NULL DEFAULT 0,
  attack           INTEGER NOT NULL DEFAULT 0,
  sent             INTEGER NOT NULL DEFAULT 0,
  garbage_cleared  INTEGER NOT NULL DEFAULT 0,
  cancelled        INTEGER NOT NULL DEFAULT 0,
  tetrises         INTEGER NOT NULL DEFAULT 0,
  tspins           INTEGER NOT NULL DEFAULT 0,
  max_combo        INTEGER NOT NULL DEFAULT 0,
  ms_played        INTEGER NOT NULL DEFAULT 0
);
```

`users.username` is stored as registered. `COLLATE NOCASE` makes the unique check case-insensitive.

Registering inserts the user and an empty `settings`, `solo_bests`, and `career` row in one transaction.

**(call)** Passwords are bcrypt (`golang.org/x/crypto/bcrypt`) at the library's default cost. The plaintext is never stored or logged. A password is 8 to 72 bytes (bcrypt's own limit). No other complexity rules.

**(call)** A session token is 32 random bytes, hex-encoded in the cookie. The table stores the SHA-256 of those bytes. Logout deletes that row.

## 2. HTTP API

The page and the API are the same origin. The web client calls them with `fetch` and `credentials: 'include'`, through a small JS bridge next to `src/app/settings_store_web.cpp`. The desktop build has no bridge and no account UI.

Cookie name `jestris_session`, `Path=/`, `HttpOnly`, `SameSite=Lax`, `Max-Age` 30 days. Add `Secure` when the request arrived over HTTPS (the connection itself, or `X-Forwarded-Proto: https` behind the proxy in the README).

JSON bodies. Errors are a plain string the menu can show. Wrong login and unknown user return the same text, `wrong username or password`, and the same status.

| Method and path | Who | What |
|---|---|---|
| `POST /api/register` `{username, password}` | logged out | Create the account and a session. `409` if the username is taken or invalid. `400` if the password length is wrong. |
| `POST /api/login` `{username, password}` | logged out | Set the cookie. `401` on a mismatch. |
| `POST /api/logout` | logged in | Delete this session, clear the cookie, and disconnect that account's live WebSocket if it has one. |
| `GET /api/me` | logged in | `{username, settings, sprintMs, cheeseMs, blitzScore, career}`. `settings` is the stored text. Bests are JSON numbers or `null`. `401` with no session. |
| `PUT /api/settings` | logged in | Body is the settings text from `serializeSettings`. Stored as `settings.body`. The server does not read `name=` or the best lines out of it. |
| `POST /api/password` `{current, next}` | logged in | Check `current`, store `next`. Keep this session. Delete every other session for the user. |
| `POST /api/bests` `{mode, result}` | logged in | `mode` is `sprint`, `cheese`, or `blitz`. `result` is a non-negative int. Update that column only when the new value is better. Respond with the three stored bests. |

**(call)** Login failures are capped at 10 per username per 15 minutes. Registrations are capped at 5 per client IP per hour. Over the cap, `429` and the menu shows a short "try again later". The counters live in memory and reset when the process restarts. The registration limit uses `X-Forwarded-For`'s first address only when the TCP peer is loopback (the proxy on this machine, as in the README). Any other peer is counted by its own remote address, and a forwarded header from it is ignored. Login failures stay keyed by username.

A failed `PUT /api/settings` or `POST /api/bests` does not stop the game. The next save tries again. A failed career write is logged and the match still ends normally.

## 3. The WebSocket

`Hub.ServeHTTP` already has the HTTP request. Before the read loop it looks up `jestris_session`. A valid session sets `player.accountID` and `player.name` from the account. `HELLO` from that connection is ignored, so a client cannot wear a different name.

A guest has `accountID == 0` and still sends `HELLO`, as today.

If another live player already holds the same `accountID`, that older player is disconnected the same way a full send queue kicks someone (`player.disconnect`). The new connection continues. A guest never kicks anyone.

**(call)** Logout does the same disconnect. The room then runs the existing leave path, so a logout during a match drops that player the way a closed tab does. The career write for that match follows the leave rules in section 5 (a royale place counts, a duel `LEFT` does not). The next connection from that browser is a guest.

Leaving a room, host migration, and the match rules are unchanged.

## 4. Settings and solo bests on the client

`loadSettingsText` / `saveSettingsText` stay the guest store.

- Logged out, including after logout: read and write `jestris-battle.settings` as today. Logout loads that guest text back into memory, so the account's keys do not remain as the guest's.
- Logged in: `GET /api/me` replaces the in-memory `Settings`. `playerName` becomes the username. `PersonalBests` become the three API values. The guest `localStorage` key is not written.
- Every `saveSettings()` while logged in sends `PUT /api/settings` with `serializeSettings` and does not touch `localStorage`.
- A finished Sprint, Cheese, or Blitz that improves the in-memory best also sends `POST /api/bests`. The client then replaces its three bests with the response, so another device's better best is what the buttons show.

The start-level slider and the bot difficulty are not in `Settings` and are not sent.

## 5. Career stats

`career` is one row per account. Rates are computed when `GET /api/me` answers, from the stored totals, using the definitions in `2026-10-14-match-stats-design.md`:

| Shown | From |
|---|---|
| PPS | `pieces / (ms_played / 1000)` |
| APM | `attack / (ms_played / 60000)` |
| VS | `(attack + garbage_cleared) / (ms_played / 1000) * 100` |

Zero `ms_played` yields zero for all three. The menu also shows duel wins, losses, and draws, KOs dealt and taken, royale matches, royale wins, and average place (`royale_place_sum / royale_matches`, omitted when the count is zero).

`max_combo` is the greater of the stored value and the match's max. Every other counter is a sum.

### Where the numbers come from

The hub already knows the verdict, the KO counts, the lines sent (`match.sent`), and a royale place. It does not know `GameStats`. The sim does (`Game::stats()`), and each player's sim time is `sim_t`.

Add one wasm export, `sim_stats`, that writes that player's `GameStats` integers plus `sim_t` into the existing sim buffer as a short space-separated line. The Go runner reads it. Do not parse a `SNAP` for this.

Read those figures **before** `endMatch`, which drops the runner.

- **Duel `finish` (an `OVER`):** for each participant with an account, add the verdict (win, loss, or draw), `kos_dealt` and `kos_taken` from the match maps, `sent` from `match.sent`, and the sim line. A duel that ends because someone left (`LEFT` in `Hub.leave`, no `OVER`) writes nothing for either player.
- **Royale elimination:** when `eliminate` assigns a place, including a disconnect, add that account's place (`royale_matches`, `royale_wins` when the place is 1, `royale_place_sum`) and their sim line and `sent`. When that call also names the winner (place 1), record the winner the same way, before `endMatch`.
- **No runner** (the sim never started, or it has already been dropped): still record the verdict, KOs, place, and `sent`. Add nothing for the sim counters or `ms_played`.
- Guests are skipped. Vs-Bot never reaches this server.

`sent` stays the hub's count, which in enforce mode is the garbage the sim's attacks caused to be routed. `attack` stays `GameStats.attack`, which includes garbage that cancelled incoming lines and so can be larger than `sent`.

## 6. Start menu (web only)

The duel section of the start menu (`src/app/render.cpp`) is the only new UI. The desktop start menu has no duel section and does not change.

Logged out, above the existing guest name field:

- Username and password fields.
- Register and Log in.
- A failed call shows its error under the buttons. The guest name field, rooms, and solo modes stay usable.

Logged in, the guest name field is replaced by the username, not editable:

- Log out.
- Change password: current, new, and a button.
- A Record block: duel W-L-D, KOs dealt and taken, royale wins and matches, average place, PPS, APM, VS.
- Sprint, Cheese, and Blitz buttons keep showing the best beside them. Those bests are the account's.

None of this appears in a room, on a board, or on the result screen.

## 7. Tests

Go tests use a temporary `-db` file (or an injected `*sql.DB` on the store the hub holds):

- Register, reject a duplicate regardless of case, log in, reject a bad password, `GET /api/me`, log out, and then `GET /api/me` is `401`.
- `PUT /api/settings` round-trips the text.
- `POST /api/bests` keeps a faster Sprint and ignores a slower one. Same for Cheese. Blitz keeps the higher score.
- `POST /api/password` rejects a wrong current password, accepts a right one, and the old password no longer logs in.
- A finished duel with two accounts increments both career rows, including a draw. A guest in that seat increments nothing. A `LEFT` disconnect increments nothing.
- A royale records each account's place, and a disconnect's place, and does not record a guest.

The C++ side keeps the existing settings tests. Add a test only if the client gains a pure function (for example, building the bests request). The JS fetch bridge is not unit-tested.

## What this does not change

Solo rules, the practice bot, room codes, host start, bomb duels, royale targeting, the match protocol besides ignoring `HELLO` for an authenticated socket, server authority, and the desktop settings file.
