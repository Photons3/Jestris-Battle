# Tetris

C++20 Tetris on SDL3 and Dear ImGui. Plays solo on the desktop or in the browser, and
as a 1v1 duel between two browsers on the same LAN.

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

Arrows move and soft-drop, Up or X rotates clockwise, Z or Ctrl rotates counter-clockwise, Space hard-drops,
C or Shift holds, Esc or P pauses, M mutes the sound, and Ctrl+Cmd+F or Alt+Enter toggles fullscreen. The Music item on the start and pause menus turns the background music off or on.

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

## Playing a duel

1. One player picks Create room and shares the 4-letter code.
2. The friend types the code and joins; the match starts after a short countdown.
3. Clearing 2, 3 or 4 lines sends 1, 2 or 4 garbage lines to the opponent. Clearing
   lines cancels incoming garbage first. Each garbage batch carries a column of bombs:
   land a piece directly on the top bomb to blow up the whole stack below it. Exploded
   rows count as cleared lines.
4. Topping out is a KO: your board resets and your opponent scores a KO. The first to
   3 KOs wins. When the 2:00 clock runs out, more KOs wins, then more lines sent; a full
   tie is a draw. Either side can ask for a rematch.
