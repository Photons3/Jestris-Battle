#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <optional>

#include "app/input.hpp"

namespace tetris {

inline constexpr int kActionCount = 8;  // every Key, Left through Pause
inline constexpr int kKeySlots = 2;     // primary and secondary key per action

// Which keys play which action. SDL_SCANCODE_UNKNOWN marks an empty slot; a key is in at most one slot.
struct Keymap {
    std::array<std::array<SDL_Scancode, kKeySlots>, kActionCount> keys{};

    SDL_Scancode at(Key action, int slot) const { return keys[static_cast<int>(action)][slot]; }
    std::optional<Key> lookup(SDL_Scancode scancode) const;
    // Puts `scancode` in the slot. If another slot held it, that slot is cleared and its action returned.
    std::optional<Key> bind(Key action, int slot, SDL_Scancode scancode);
    void clear(Key action, int slot);
    // Pause must always have a key: with none, Escape goes back in its first slot.
    void ensurePauseKey();
};

Keymap defaultKeymap();
// Keys the Controls screen won't assign: M mutes, Enter is for fullscreen, Escape cancels and
// Backspace clears while waiting for a key. Escape is still Pause's default.
bool isReservedKey(SDL_Scancode scancode);
const char* actionName(Key action);       // "Move left", "Rotate CW", ...
const char* keyName(SDL_Scancode scancode);  // "Space", "X", ...; "-" for an empty slot

// ⌃⌘F (macOS standard) or ⌥Enter (PC convention).
bool isFullscreenChord(const SDL_KeyboardEvent& key);

}  // namespace tetris
