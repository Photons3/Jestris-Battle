#include "app/keymap.hpp"

namespace tetris {

std::optional<Key> Keymap::lookup(SDL_Scancode scancode) const {
    if (scancode == SDL_SCANCODE_UNKNOWN) return std::nullopt;
    for (int action = 0; action < kActionCount; ++action)
        for (SDL_Scancode key : keys[action])
            if (key == scancode) return static_cast<Key>(action);
    return std::nullopt;
}

std::optional<Key> Keymap::bind(Key action, int slot, SDL_Scancode scancode) {
    SDL_Scancode& target = keys[static_cast<int>(action)][slot];
    if (target == scancode) return std::nullopt;
    std::optional<Key> takenFrom;
    for (int other = 0; other < kActionCount; ++other)
        for (SDL_Scancode& key : keys[other])
            if (key == scancode && scancode != SDL_SCANCODE_UNKNOWN) {
                key = SDL_SCANCODE_UNKNOWN;
                if (other != static_cast<int>(action)) takenFrom = static_cast<Key>(other);
            }
    target = scancode;
    ensurePauseKey();
    return takenFrom;
}

void Keymap::clear(Key action, int slot) {
    keys[static_cast<int>(action)][slot] = SDL_SCANCODE_UNKNOWN;
    ensurePauseKey();
}

void Keymap::ensurePauseKey() {
    auto& pause = keys[static_cast<int>(Key::Pause)];
    for (SDL_Scancode key : pause)
        if (key != SDL_SCANCODE_UNKNOWN) return;
    for (auto& slots : keys)
        for (SDL_Scancode& key : slots)
            if (key == SDL_SCANCODE_ESCAPE) key = SDL_SCANCODE_UNKNOWN;
    pause[0] = SDL_SCANCODE_ESCAPE;
}

Keymap defaultKeymap() {
    Keymap map;
    auto set = [&map](Key action, SDL_Scancode first, SDL_Scancode second) {
        map.keys[static_cast<int>(action)] = {first, second};
    };
    set(Key::Left, SDL_SCANCODE_LEFT, SDL_SCANCODE_UNKNOWN);
    set(Key::Right, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN);
    set(Key::SoftDrop, SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN);
    set(Key::HardDrop, SDL_SCANCODE_SPACE, SDL_SCANCODE_UNKNOWN);
    set(Key::RotateCW, SDL_SCANCODE_UP, SDL_SCANCODE_X);
    set(Key::RotateCCW, SDL_SCANCODE_Z, SDL_SCANCODE_LCTRL);
    set(Key::Hold, SDL_SCANCODE_C, SDL_SCANCODE_LSHIFT);
    set(Key::Pause, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_P);
    return map;
}

bool isReservedKey(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_M:
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_BACKSPACE:
        return true;
    default:
        return false;
    }
}

const char* actionName(Key action) {
    switch (action) {
    case Key::Left:
        return "Move left";
    case Key::Right:
        return "Move right";
    case Key::SoftDrop:
        return "Soft drop";
    case Key::HardDrop:
        return "Hard drop";
    case Key::RotateCW:
        return "Rotate CW";
    case Key::RotateCCW:
        return "Rotate CCW";
    case Key::Hold:
        return "Hold";
    case Key::Pause:
        return "Pause";
    }
    return "";
}

const char* keyName(SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) return "-";
    const char* name = SDL_GetScancodeName(scancode);
    return name != nullptr && *name != '\0' ? name : "?";
}

bool isFullscreenChord(const SDL_KeyboardEvent& key) {
    const bool ctrlCmdF =
        key.scancode == SDL_SCANCODE_F && (key.mod & SDL_KMOD_CTRL) != 0 && (key.mod & SDL_KMOD_GUI) != 0;
    const bool altEnter = key.scancode == SDL_SCANCODE_RETURN && (key.mod & SDL_KMOD_ALT) != 0;
    return ctrlCmdF || altEnter;
}

}  // namespace tetris
