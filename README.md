# Jestris Battle

C++20 Tetris on SDL3 and Dear ImGui. Plays solo on the desktop or in the browser, as a practice
duel against a bot, and in rooms of up to 16 browsers on the same LAN: a 1v1 duel for two players, and a
battle royale for three or more.

## Desktop build and tests

Needs Homebrew SDL3 (`brew install sdl3`) and CMake 3.25 or newer.

    cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
    ./build/tetris

## Web build

    brew install emscripten
    emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web

emcc needs Python 3.10 or newer. If the system `python3` is older, prefix both
commands with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14` (or any Python >= 3.10).

## Controls

Arrows move and soft-drop, Up or X rotates clockwise, Z or Left Ctrl rotates counter-clockwise, Space hard-drops,
Left Shift or C holds, Esc or P pauses, M mutes the sound, and Ctrl+Cmd+F or Alt+Enter toggles fullscreen. The Music item on the start and pause menus turns the background music off or on, and the Sound volume and Music volume sliders under them set how loud each one plays.

Controls on the start and pause menus changes the game keys: each action has two key slots.
Click one (or press Enter on it) and press the new key; Esc cancels and Backspace clears the
slot. A key already used elsewhere moves to the new action. M, Enter, Esc and Backspace can't
be assigned, and Pause always keeps a key.

The Handling section under the keys sets DAS (how long a held direction waits before it repeats,
default 167 ms), ARR (the time between repeats, default 33 ms; 0 moves the piece straight to the wall)
and SDF (soft-drop speed as a multiple of gravity, default 20x; Instant drops the piece to the floor
without locking it). Drag a slider, or focus it with the arrow keys and press Space, then Left/Right,
then Space again. Reset to defaults resets the handling along with the keys.

The keys, handling, sound and music settings (volumes included) and your solo personal bests are saved (localStorage
in the browser, the SDL preferences folder on desktop).

## Solo modes

The SOLO section of the start menu has five modes:

- **Marathon**: the classic endless game, from the chosen start level, until you top out.
- **Sprint 40L**: clear 40 lines as fast as you can, from level 1.
- **Blitz 2:00**: score as much as you can in two minutes. The level rises every 5 lines.
- **Cheese 10L**: dig out 10 garbage rows, each with one hole, as fast as you can, from level 1.
- **Zen**: no clock and no game over; topping out just clears the board. Leave from the pause menu.

The start level slider applies to Marathon, Blitz, Zen and vs Bot. Sprint, Blitz and Cheese start after a
3-second countdown, and the side panel shows the clock (Blitz counts down and turns red under 10
seconds). When a timed run ends, the result screen shows your time or score and your personal best;
Retry (or R) starts again. Only a finished run counts toward a best: the fastest Sprint and Cheese
times and the highest Blitz score are shown beside their buttons on the start menu.

## Practice bot

The PRACTICE section of the start menu plays a 1v1 duel against a bot, offline, on the desktop and in
the browser alike. Pick Easy, Medium or Hard and press vs Bot. The bot is named Bot-Easy, Bot-Medium or
Bot-Hard. Easy plays about 0.7 pieces per second and makes mistakes, Medium about 1.3, Hard about 2.5.
The match has the same countdown, garbage and KOs as a network duel and starts without a Start click;
Start next match plays again, and Leave returns to the menu.

## Hosting LAN duels

    go -C server run .

Needs Go 1.27 or newer. The server serves the web build and relays matches. It prints
the `http://<lan-ip>:8080` URLs players open; allow the incoming connection if the
macOS firewall asks. Flags: `-addr` (default `0.0.0.0:8080`), `-dir` (default
`../build-web`) and `-match` (match length, default `2m`). `-match` is for testing: the client's
on-screen clock always starts at 2:00, so with `-match 20s` the server's verdict arrives early
(the red clock and the 0:00 freeze only show at the default length).

### Server authority

The server re-simulates every match from the inputs each client sends, using the C++ core
compiled to WebAssembly (`server/sim/jestris_sim.wasm`), so attacks, KOs and snapshots come
from the server's own game. Flags:

- `-authority log|enforce` (default `log`). `log` simulates every match and only logs
  disagreements; `enforce` makes the simulation decide and sends `RESYNC` when a rule is broken.
- `-lead` (default `250ms`): how far a player's game may run ahead of the server's match clock.
- `-lag` (default `1s`): how far behind the match clock a player's game is forced forward.
- `-garbage-window` (default `750ms`): time a player has to accept sent garbage.
- `-max-resyncs` (default `10`): `RESYNC`s per 10 s before a player is disconnected (`enforce` only).

`jestris_sim.wasm` is a build output and is git-ignored, but the server embeds it, so it must
exist before any `go build` or `go test`. `scripts/build.sh` (and the Dockerfile) build it
first, then run `go test` and `go build`. To build it by hand:

    source ~/emsdk/emsdk_env.sh
    emcmake cmake -S . -B build-sim -DCMAKE_BUILD_TYPE=Release -DTETRIS_SIM_ONLY=ON
    cmake --build build-sim --target jestris_sim
    cp build-sim/jestris_sim.wasm server/sim/

Rollout:

1. Deploy with the default `-authority log` and let real matches run.
2. Grep the server log for `would-resync:` (rules `bad-in`, `backdated`, `garbage-bad`,
   `garbage-late`) and read the per-player summaries
   `authority room=... player=... claimedSent=... simAttack=... claimedDead=... simTopOuts=... wouldResync=...`.
   Look for lines where `claimedSent` differs from `simAttack`. Honest play shows none; fix the
   bug or loosen a timing flag until it doesn't.
3. Restart with `-authority enforce`.

## Hosting on a Debian VPS

    scripts/setup.sh   # once: apt packages, emsdk into ~/emsdk, Go into /usr/local/go
    scripts/build.sh   # web build into build-web/, jestris_sim.wasm, server into build-server/
    scripts/run.sh     # serves on 0.0.0.0:8080; extra flags go to the server
    scripts/restart.sh # stops a running server, rebuilds, and starts it; same flags as run.sh

Plain `http://` and `ws://` work out of the box. The game connects with `wss://` whenever the
page was loaded over `https://`, so HTTPS works in either of two ways:

- **Reverse proxy (recommended).** Let nginx or Caddy handle the certificate and forward to
  the server on `127.0.0.1:8080` (`scripts/run.sh -addr 127.0.0.1:8080`). Caddy needs only
  `game.example.com { reverse_proxy 127.0.0.1:8080 }`. For nginx, pass the WebSocket upgrade
  and the Host header through:

      location / {
          proxy_pass http://127.0.0.1:8080;
          proxy_http_version 1.1;
          proxy_set_header Upgrade $http_upgrade;
          proxy_set_header Connection "upgrade";
          proxy_set_header Host $host;
      }

  If your proxy can't pass the Host header, allow the public hostname with
  `-origins game.example.com` (comma-separated), or room joins fail with a 403.
- **Built-in TLS.** `scripts/run.sh -addr 0.0.0.0:443 -cert fullchain.pem -key privkey.pem`
  serves HTTPS and WSS directly (for example with Let's Encrypt files from certbot).

The server pings every connection every 25 seconds, so proxies don't drop a player waiting
in a room.

## Playing a duel

1. Type the name you want the other players to see in the Name field (it is remembered). Letters,
   digits and `_ - .`, up to 12 characters. Two players with the same name in a room get a number
   added to the second one (`Sam`, `Sam2`).
2. A room holds up to 16 players. Quick match joins the oldest open public room (preferring one
   that isn't mid-match), or opens a new one. Browse rooms lists the open public rooms with their
   player counts; click Join on one. Create room makes a room with a 4-letter code (tick Public to
   also list it in Browse rooms). Share the code, or the invite link on the room screen, which
   opens the game with a Join button ready.
3. The room screen lists everyone in join order; a star marks the host. The host presses Start
   once at least 2 players are in, and everyone in the room plays after a short countdown.
   Anyone who joins while a match is running is shown as "(next match)" and plays in the next
   one. After each match everyone returns to the room, and the host presses Start next match.
   If the host leaves, the player who has been in the room longest becomes the host.
4. The number of players in the match picks the mode: 2 players play the KO duel, and 3 to 16
   play a battle royale.
5. Clearing 2, 3 or 4 lines sends 1, 2 or 4 garbage lines, following the
   [Tetris Guideline](https://tetris.wiki/Garbage): a T-spin single, double or triple sends 2, 4
   or 6 (a T-spin mini sends like a plain clear), back-to-back tetrises and T-spins add 1, combos
   add 1 to 5, and a perfect clear adds 10. The same clears score the guideline points in solo
   and matches, and a callout under HOLD names them. Garbage waiting for you is cancelled first:
   each line you would send removes one queued line, and only the rest is sent.
   Queued garbage rises when a piece locks without clearing anything.
   Incoming garbage travels for a third of a second before it can rise (the meter shows it dimmed),
   and at most 8 lines rise per lock.

### The duel (2 players)

Garbage goes to your opponent, and every garbage row has one bomb. A bomb lands on a lone bomb
right above it 90% of the time, but never stacks more than two high. Land a piece directly on the
top bomb to blow up the bombs touching it below. Exploded rows score and send like cleared lines,
but only full rows count as a tetris or toward back-to-back.

Topping out is a KO: your board resets and your opponent scores a KO. The first to 3 KOs wins.
When the 2:00 clock runs out, more KOs wins, then more lines sent; a full tie is a draw.

### Battle royale (3 to 16 players)

Your own well is on the left and every opponent's board is in a grid of small boards beside it.

- One top-out and you're out. Your place is the number of players still alive when you topped
  out, and you keep watching the other boards until the match ends.
- Each attack goes to a random living opponent.
- Garbage has no bombs: each garbage row is gray with one open hole, and all rows from one attack
  share the same hole column, so you can dig them out down one well. The next attack uses a
  different column.
- There's no clock. Gravity rises one level every 30 seconds to keep the match moving.
- The last player standing wins. The result screen shows the standings with each player's attack.
