#pragma once

#include <SDL3/SDL.h>

#include <optional>

#include "app/input.hpp"

namespace tetris {

std::optional<Key> keyForScancode(SDL_Scancode scancode);
// ⌃⌘F (macOS standard) or ⌥Enter (PC convention).
bool isFullscreenChord(const SDL_KeyboardEvent& key);

}  // namespace tetris
