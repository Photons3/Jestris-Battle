# Jestris Battle

C++20 Tetris on SDL3 and Dear ImGui. Plays solo on the desktop or in the browser, as a practice
duel against a bot, and as a 1v1 duel between two browsers on the same LAN.

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
Left Shift or C holds, Esc or P pauses, M mutes the sound, and Ctrl+Cmd+F or Alt+Enter toggles fullscreen. The Music item on the start and pause menus turns the background music off or on.

Controls on the start and pause menus changes the game keys: each action has two key slots.
Click one (or press Enter on it) and press the new key; Esc cancels and Backspace clears the
slot. A key already used elsewhere moves to the new action. M, Enter, Esc and Backspace can't
be assigned, and Pause always keeps a key.

The Handling section under the keys sets DAS (how long a held direction waits before it repeats,
default 167 ms), ARR (the time between repeats, default 33 ms; 0 moves the piece straight to the wall)
and SDF (soft-drop speed as a multiple of gravity, default 20x; Instant drops the piece to the floor
without locking it). Drag a slider, or focus it with the arrow keys and press Space, then Left/Right,
then Space again. Reset to defaults resets the handling along with the keys.

The keys, handling, sound and music settings and your solo personal bests are saved (localStorage
in the browser, the SDL preferences folder on desktop).

## Solo modes

The SOLO section of the start menu has five modes:

- **Marathon**: the classic endless game, from the chosen start level, until you top out.
- **Sprint 40L**: clear 40 lines as fast as you can, from level 1.
- **Blitz 2:00**: score as much as you can in two minutes. The level rises every 5 lines.
- **Cheese 10L**: dig out 10 garbage rows, each with one hole, as fast as you can, from level 1.
- **Zen**: no clock and no game over; topping out just clears the board. Leave from the pause menu.

The start level slider applies to Marathon, Blitz and Zen. Sprint, Blitz and Cheese start after a
3-second countdown, and the side panel shows the clock (Blitz counts down and turns red under 10
seconds). When a timed run ends, the result screen shows your time or score and your personal best;
Retry (or R) starts again. Only a finished run counts toward a best: the fastest Sprint and Cheese
times and the highest Blitz score are shown beside their buttons on the start menu.

## Practice bot

The PRACTICE section of the start menu plays a 1v1 duel against a bot, offline, on the desktop and in
the browser alike. Pick Easy, Medium or Hard and press vs Bot. The bot is named Bot-Easy, Bot-Medium or
Bot-Hard. Easy plays about 0.8 pieces per second and makes mistakes, Medium about 1.5, Hard about 2.5.
The match has the same countdown, garbage, KOs and rematch as a network duel, and Leave returns to
the menu.

## Hosting LAN duels

    go -C server run .

Needs Go 1.27 or newer. The server serves the web build and relays matches. It prints
the `http://<lan-ip>:8080` URLs players open; allow the incoming connection if the
macOS firewall asks. Flags: `-addr` (default `0.0.0.0:8080`), `-dir` (default
`../build-web`) and `-match` (match length, default `2m`). `-match` is for testing: the client's
on-screen clock always starts at 2:00, so with `-match 20s` the server's verdict arrives early
(the red clock and the 0:00 freeze only show at the default length).

## Hosting on a Debian VPS

    scripts/setup.sh   # once: apt packages, emsdk into ~/emsdk, Go into /usr/local/go
    scripts/build.sh   # web build into build-web/, server into build-server/
    scripts/run.sh     # serves on 0.0.0.0:8080; extra flags go to the server

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

1. Type the name you want your opponent to see in the Name field (it is remembered). Letters,
   digits and `_ - .`, up to 12 characters.
2. Quick match pairs you with whoever is waiting, or waits for the next player.
   Browse rooms lists open public rooms; click Join on one.
   Create room makes a room with a 4-letter code (tick Public to also list it in Browse rooms).
   Share the code, or the invite link on the waiting screen, which opens the game with a Join button ready.
3. The friend types the code and joins; the match starts after a short countdown.
4. Clearing 2, 3 or 4 lines sends 1, 2 or 4 garbage lines to the opponent, following the
   [Tetris Guideline](https://tetris.wiki/Garbage): a T-spin single, double or triple sends 2, 4
   or 6 (a T-spin mini sends like a plain clear), back-to-back tetrises and T-spins add 1, combos
   add 1 to 5, and a perfect clear adds 10. The same clears score the guideline points in solo
   and duels, and a callout under HOLD names them. Garbage waiting for you is cancelled first:
   each line you would send removes one queued line, and only the rest goes to the opponent.
   Queued garbage rises when a piece locks without clearing anything.
   Every garbage row has one bomb. A bomb lands on a lone bomb right above it 90% of the time,
   but never stacks more than two high. Land a piece directly on the top bomb to blow up the
   bombs touching it below. Exploded rows score and send like cleared lines, but only full
   rows count as a tetris or toward back-to-back.
5. Topping out is a KO: your board resets and your opponent scores a KO. The first to
   3 KOs wins. When the 2:00 clock runs out, more KOs wins, then more lines sent; a full
   tie is a draw. Either side can ask for a rematch.
