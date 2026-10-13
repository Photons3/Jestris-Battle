#include "app/keymap.hpp"

namespace tetris {

std::optional<Key> keyForScancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_LEFT:
        return Key::Left;
    case SDL_SCANCODE_RIGHT:
        return Key::Right;
    case SDL_SCANCODE_DOWN:
        return Key::SoftDrop;
    case SDL_SCANCODE_SPACE:
        return Key::HardDrop;
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_X:
        return Key::RotateCW;
    case SDL_SCANCODE_Z:
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        return Key::RotateCCW;
    case SDL_SCANCODE_C:
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        return Key::Hold;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_P:
        return Key::Pause;
    default:
        return std::nullopt;
    }
}

bool isFullscreenChord(const SDL_KeyboardEvent& key) {
    const bool ctrlCmdF =
        key.scancode == SDL_SCANCODE_F && (key.mod & SDL_KMOD_CTRL) != 0 && (key.mod & SDL_KMOD_GUI) != 0;
    const bool altEnter = key.scancode == SDL_SCANCODE_RETURN && (key.mod & SDL_KMOD_ALT) != 0;
    return ctrlCmdF || altEnter;
}

}  // namespace tetris
