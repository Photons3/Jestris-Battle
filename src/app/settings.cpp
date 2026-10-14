#include "app/settings.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>

#include "net/protocol.hpp"

namespace tetris {
namespace {

constexpr std::string_view kVersion = "1";

// Names in the file; independent of actionName() so UI wording can change freely.
constexpr std::string_view kActionKeys[kActionCount] = {"Left",     "Right",     "SoftDrop", "HardDrop",
                                                        "RotateCW", "RotateCCW", "Hold",     "Pause"};

std::optional<int> actionIndex(std::string_view name) {
    for (int i = 0; i < kActionCount; ++i)
        if (kActionKeys[i] == name) return i;
    return std::nullopt;
}

// A storable key: a known scancode the Controls screen could have assigned, or Escape, which
// can't be picked there (it cancels) but is Pause's default.
SDL_Scancode parseScancode(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return SDL_SCANCODE_UNKNOWN;
    if (value <= SDL_SCANCODE_UNKNOWN || value >= SDL_SCANCODE_COUNT) return SDL_SCANCODE_UNKNOWN;
    const auto scancode = static_cast<SDL_Scancode>(value);
    return isReservedKey(scancode) && scancode != SDL_SCANCODE_ESCAPE ? SDL_SCANCODE_UNKNOWN : scancode;
}

// Up to kKeySlots space-separated scancodes; anything unusable becomes an empty slot.
std::array<SDL_Scancode, kKeySlots> parseSlots(std::string_view text) {
    std::array<SDL_Scancode, kKeySlots> slots{};
    for (SDL_Scancode& slot : slots) {
        const size_t space = text.find(' ');
        slot = parseScancode(text.substr(0, space));
        if (space == std::string_view::npos) break;
        text.remove_prefix(space + 1);
    }
    return slots;
}

}  // namespace

std::string defaultPlayerName(std::uint32_t random) {
    char digits[5];
    std::snprintf(digits, sizeof digits, "%04u", static_cast<unsigned>(random % 10000));
    return "Player" + std::string(digits);
}

std::string serializeSettings(const Settings& settings) {
    std::string out = "version=" + std::string(kVersion) + "\n";
    out += settings.muted ? "sound=off\n" : "sound=on\n";
    out += settings.musicOff ? "music=off\n" : "music=on\n";
    if (isValidName(settings.playerName)) out += "name=" + settings.playerName + "\n";
    for (int i = 0; i < kActionCount; ++i) {
        out += kActionKeys[i];
        out += '=';
        for (int slot = 0; slot < kKeySlots; ++slot) {
            if (slot > 0) out += ' ';
            out += std::to_string(static_cast<int>(settings.keys.keys[i][slot]));
        }
        out += '\n';
    }
    return out;
}

Settings parseSettings(std::string_view text) {
    Settings settings;
    Keymap loaded = settings.keys;
    bool versionOk = false;
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (name == "version") {
            versionOk = value == kVersion;
        } else if (name == "sound") {
            settings.muted = value == "off";
        } else if (name == "music") {
            settings.musicOff = value == "off";
        } else if (name == "name") {
            if (isValidName(value)) settings.playerName = std::string(value);
        } else if (const std::optional<int> action = actionIndex(name)) {
            loaded.keys[*action] = parseSlots(value);
        }
    }
    if (!versionOk) return Settings{};

    // Copy slot by slot so a key listed twice stays with its first action.
    settings.keys.keys = {};
    for (int action = 0; action < kActionCount; ++action)
        for (int slot = 0; slot < kKeySlots; ++slot) {
            const SDL_Scancode key = loaded.keys[action][slot];
            if (key != SDL_SCANCODE_UNKNOWN && !settings.keys.lookup(key))
                settings.keys.keys[action][slot] = key;
        }
    settings.keys.ensurePauseKey();
    return settings;
}

}  // namespace tetris
