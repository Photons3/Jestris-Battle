#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "app/keymap.hpp"
#include "app/solo.hpp"

namespace tetris {

inline constexpr int kMaxVolume = 100;  // the volume sliders run 0 to 100 percent

// What is remembered between launches.
struct Settings {
    Keymap keys = defaultKeymap();
    bool muted = false;
    bool musicOff = false;
    int soundVolume = kMaxVolume;  // percent, 0 to kMaxVolume
    int musicVolume = kMaxVolume;
    std::string playerName;  // empty until one is chosen or generated
    bool publicRooms = false;  // Create room makes a public room
    Handling handling;
    PersonalBests bests;  // best solo results; each stays unset until a run sets it
};

// "Player" and four digits taken from `random`, for a player who has not picked a name.
std::string defaultPlayerName(std::uint32_t random);

// One "name=value" per line. Keys are stored as scancode numbers, since names like "," would
// clash with the format.
std::string serializeSettings(const Settings& settings);
// Never fails: a missing or unknown version gives the defaults, bad entries are skipped.
Settings parseSettings(std::string_view text);

}  // namespace tetris
