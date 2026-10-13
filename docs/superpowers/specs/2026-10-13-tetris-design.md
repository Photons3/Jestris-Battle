# Plan: Tetris with SDL3 + SDL_GPU + Dear ImGui

## Context
`~/Documents/GitHub/game-test` is empty (not a git repo). The goal is a modern-guideline Tetris in C++20, built with the toolchain already verified on this Mac: Apple clang 21, CMake 4.4.4, Ninja, and Homebrew SDL3 3.4.18 (SDL_GPU uses Metal). SDL3 owns the window and input, and **Dear ImGui v1.92.9b draws everything**: the board is drawn as `ImDrawList` rectangles, and the HUD and menus are ImGui windows. The game rules live in a pure C++ library (`tetris_core`) with no SDL or ImGui dependency, unit-tested with doctest. All design decisions below were settled with the user over 20 grilling questions.

## Architecture
Two layers with one dependency direction: `app → core`.

### `tetris_core` (static lib, pure C++20, deterministic)
- `src/core/piece.{hpp,cpp}`: `PieceType {I,O,T,S,Z,J,L}`, `Rotation {0,R,2,L}`, SRS cell offsets for each piece and rotation, and the spawn position (rows 21–22).
- `src/core/srs.{hpp,cpp}`: SRS kick tables (one shared JLSTZ table, a separate I table, no kicks for O). `tryRotate(board, piece, dir) -> std::optional<Piece>`.
- `src/core/board.{hpp,cpp}`: a 10×40 grid (rows 0–19 visible, 20–39 hidden buffer), `collides`, `lock`, `fullRows() -> small vector`, `removeRows(rows)`.
- `src/core/bag.{hpp,cpp}`: 7-bag randomizer, seeded by the caller (`std::mt19937`; the app seeds from the clock, tests use a fixed seed). `peek(n)` feeds the 5-piece next queue.
- `src/core/game.{hpp,cpp}`: the `Game` class.
  - **States:** `Playing`, `Clearing`, `Paused`, `GameOver`.
  - **Inputs:**
    - `apply(Action)`, where `Action` is `Left, Right, RotateCW, RotateCCW, HardDrop, Hold, Pause`.
    - `setSoftDrop(bool)`.
    - `tick(int ms)`, which the app calls in **1 ms steps**.
  - **Gravity:**
    - Speed is `(0.8 − (L−1)·0.007)^(L−1)` seconds per row, where `L = min(level, 20)`.
    - Fractional rows accumulate between steps, and one step can move a piece several rows (this happens at high levels).
    - Soft drop runs at 20× gravity and scores 1 point per cell. Hard drop scores 2 points per cell and locks the piece immediately.
  - **Lock delay:**
    - 500 ms.
    - The timer restarts on a successful move or rotate, up to **15 restarts**.
    - The restart counter returns to zero when the piece reaches a new lowest row.
  - **Hold:** once per piece, and the held piece comes back in its spawn orientation. Initial rotation/hold (IRS/IHS) is not supported.
  - **Line clear:**
    - When a locked piece completes rows, the state switches to `Clearing` for **300 ms**. `clearingRows()` exposes those rows so the renderer can flash them.
    - Then the rows are removed, scored, and the next piece spawns.
    - During `Clearing`, every action except `Pause` is ignored.
  - **Scoring and level:**
    - Clears score 100 / 300 / 500 / 800 × level for 1 / 2 / 3 / 4 lines.
    - The level goes up every 10 lines and keeps rising past 20 (it remains the score multiplier), but gravity stays at the level-20 speed.
    - The starting level is passed to `Game(seed, startLevel)` and can be 1–15.
  - **Game over:** a new piece's spawn overlaps existing blocks (block-out), or a piece locks entirely above row 19 (lock-out).
  - **Read-only accessors:** board, active piece, ghost drop distance, hold piece plus its can-hold flag, `next(5)`, score, level, lines, and state.

### `tetris` (executable): the SDL3 + ImGui shell
- `src/app/main.cpp`:
  - **Window:**
    - Initialize with `SDL_Init(SDL_INIT_VIDEO)`. The game is keyboard-only for v1.
    - The window is resizable and high-pixel-density, with a minimum size of 640×720.
    - Fullscreen toggles on ⌃⌘F or ⌥Enter.
    - Create the GPU device with `SDL_CreateGPUDevice(MSL|METALLIB|SPIRV)`, then call `SDL_ClaimWindowForGPUDevice` and set the swapchain to `SDL_GPU_PRESENTMODE_VSYNC`.
  - **ImGui:** set up with `ImGui_ImplSDL3_InitForSDLGPU` and `ImGui_ImplSDLGPU3_Init`, using ImGui's dark theme.
  - **Each frame:**
    1. Poll events.
    2. Pass events to the input mapper.
    3. Add the frame time to an accumulator, **capped at 100 ms per frame**.
    4. Run 1 ms steps: advance input DAS/ARR, then call `game.tick(1)`.
    5. Build the ImGui frame.
    6. Acquire the command buffer and swapchain texture.
    7. Call `ImGui_ImplSDLGPU3_PrepareDrawData`.
    8. Run the render pass with `RenderDrawData`.
    9. Submit.
  - **Focus loss:** `SDL_EVENT_WINDOW_FOCUS_LOST` pauses automatically.
- `src/app/input.{hpp,cpp}`:
  - Turns key events into `Action`s and ignores OS key repeat (`event.key.repeat`).
  - Owns DAS (167 ms) and ARR (33 ms). Its timers keep charging during `Clearing`, so a held direction carries over to the next piece.
  - Default keys:
    - ←/→: move
    - ↓: soft drop
    - Space: hard drop
    - ↑ or X: rotate clockwise
    - Z or Ctrl: rotate counterclockwise
    - C or Shift: hold
    - Esc or P: pause
- `src/app/render.{hpp,cpp}` (ImGui only):
  - **Screen layout:** fixed ImGui windows positioned with `SetNextWindowPos/Size` every frame (no title bar, no move, no resize). The board's cell size comes from the window height, and the side panels scale with it.
  - **Cells:**
    - Beveled: a base `AddRectFilled` plus a lighter top-left edge and a darker bottom-right edge, using the guideline colors (I cyan, O yellow, T purple, S green, Z red, J blue, L orange).
    - The ghost piece is an outline (`AddRect`).
    - The parts of pieces in rows 21–22 are drawn at 50% opacity.
    - Rows that are clearing flash white.
  - **Side panels:** Hold on the left; the next 5 pieces, score, level and lines on the right.
  - **Screens:**
    - **Start:** a level slider (1–15), Play, and Quit.
    - **Paused:** the board is hidden; Resume, Restart and Quit-to-menu.
    - **Game Over:** final score; Restart and Quit-to-menu.
    - Restart begins a new game at the same starting level.
  - **Fonts:** the HUD and titles use **Press Start 2P** (OFL), compiled into the binary as `src/app/font_data.hpp` (generated once with ImGui's `misc/fonts/binary_to_compressed_c`, output committed). Small menus use ImGui's default font. Font sizes use 1.92's dynamic scaling.

## Project layout and build
```
game-test/
  CMakeLists.txt          # C++20; FetchContent: imgui v1.92.9b, doctest v2.4.12
  src/core/  src/app/  tests/
  assets/OFL.txt          # Press Start 2P license
  docs/superpowers/specs/2026-10-13-tetris-design.md
  .gitignore              # build/
```
- SDL3 comes from `find_package(SDL3 REQUIRED CONFIG)` (Homebrew) and links `SDL3::SDL3`.
- ImGui has no CMakeLists of its own, so we compile it into an `imgui` static library: the core files plus `backends/imgui_impl_sdl3.cpp` and `backends/imgui_impl_sdlgpu3.cpp`.
- Warnings `-Wall -Wextra -Wpedantic` apply to our own targets only.
- **Debug builds** compile everything with `-fsanitize=address,undefined`. **Release builds** use `-O2`.
- CTest runs `tetris_tests`.
- Output is a plain executable (`./build/tetris`); no `.app` bundle in v1.
- Git: `git init` locally, with no remote and no push.

## Build order
1. Scaffold:
   - Run `git init`, add `.gitignore`, and commit the spec document.
   - Set up CMake with FetchContent.
   - Show an empty window displaying ImGui's demo window, which proves SDL3, SDL_GPU, ImGui and vsync work together.
2. Core, **test-first**, in this order:
   1. Piece shapes.
   2. Board: collide, lock, find and remove full rows.
   3. SRS: the standard kick cases, I-piece wall kicks, and the T-spin-triple kick.
   4. 7-bag: every group of 7 pieces contains each type once, and a fixed seed gives a deterministic sequence.
   5. Game:
      - Gravity timing, including several rows in one step at level 20.
      - Lock delay, its 15-restart cap, and the counter reset on a new lowest row.
      - Hold once per piece.
      - The 300 ms clearing state, and that it ignores input.
      - Scoring, soft- and hard-drop points, level-up, and the gravity cap above level 20.
      - Block-out and lock-out.
3. App, in this order:
   1. Input mapper with DAS/ARR.
   2. Board and cell rendering (bevels, ghost piece, faded hidden rows, clear flash).
   3. Side panels.
   4. Start, Pause and Game Over screens.
   5. Fonts.
   6. Fullscreen, auto-pause on focus loss, the 100 ms cap, and the minimum window size.

Out of scope for v1: sound, T-spin/combo/back-to-back scoring, saved high scores, key remapping, gamepad, IRS/IHS, entry delay, the 150-line marathon mode, `.app` bundle, GitHub remote.

## Verification
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure` passes with no sanitizer reports.
- `./build/tetris` (in both Debug and Release builds) passes these manual checks:
  - The start-level slider works.
  - Pieces fall faster as the level rises.
  - The ghost piece shows where the piece will land.
  - Hold swaps pieces, and a second hold for the same piece is blocked.
  - The next queue shows 5 pieces.
  - Wall kicks work against the walls.
  - Clearing lines flashes the rows for about 300 ms, and a held direction keeps moving the next piece.
  - A 4-line clear at level 1 adds 800 points.
  - Pausing hides the board.
  - Switching to another app pauses the game.
  - ⌃⌘F and ⌥Enter toggle fullscreen.
  - Resizing rescales the board and text.
  - Restart and Quit-to-menu work.
