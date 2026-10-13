#include <doctest/doctest.h>

#include "app/settings.hpp"

using namespace tetris;

namespace {

bool isDefault(const Settings& settings) {
    return settings.keys.keys == defaultKeymap().keys && !settings.muted && !settings.musicOff;
}

}  // namespace

TEST_CASE("settings: a saved file loads back the same") {
    Settings settings;
    settings.keys.bind(Key::HardDrop, 1, SDL_SCANCODE_W);
    settings.keys.bind(Key::Left, 1, SDL_SCANCODE_X);
    settings.muted = true;
    settings.musicOff = true;
    const Settings loaded = parseSettings(serializeSettings(settings));
    CHECK(loaded.keys.keys == settings.keys.keys);
    CHECK(loaded.muted);
    CHECK(loaded.musicOff);
}

TEST_CASE("settings: the default keys load back, Escape included") {
    const Settings loaded = parseSettings(serializeSettings(Settings{}));
    CHECK(loaded.keys.keys == defaultKeymap().keys);
}

TEST_CASE("settings: nothing saved, garbage or another version gives the defaults") {
    CHECK(isDefault(parseSettings("")));
    CHECK(isDefault(parseSettings("\x01\x02 not a settings file")));
    CHECK(isDefault(parseSettings("version=2\nsound=off\nHardDrop=26 0\n")));
    CHECK(isDefault(parseSettings("sound=off\n")));  // no version line
}

TEST_CASE("settings: Windows line endings are accepted") {
    const Settings loaded = parseSettings("version=1\r\nsound=off\r\nmusic=on\r\n");
    CHECK(loaded.muted);
    CHECK_FALSE(loaded.musicOff);
}

TEST_CASE("settings: a missing action keeps its default keys") {
    const Settings loaded = parseSettings("version=1\nHardDrop=26 0\n");
    CHECK(loaded.keys.lookup(SDL_SCANCODE_W) == Key::HardDrop);
    CHECK(loaded.keys.lookup(SDL_SCANCODE_LEFT) == Key::Left);
}

TEST_CASE("settings: unusable keys become empty slots") {
    // 9999 is out of range, 16 is M (reserved), "abc" is not a number.
    const Settings loaded = parseSettings("version=1\nHardDrop=9999 16\nHold=abc 6\n");
    CHECK(loaded.keys.at(Key::HardDrop, 0) == SDL_SCANCODE_UNKNOWN);
    CHECK(loaded.keys.at(Key::HardDrop, 1) == SDL_SCANCODE_UNKNOWN);
    CHECK(loaded.keys.at(Key::Hold, 0) == SDL_SCANCODE_UNKNOWN);
    CHECK(loaded.keys.at(Key::Hold, 1) == SDL_SCANCODE_C);
}

TEST_CASE("settings: a key listed twice stays with the first action") {
    const Settings loaded = parseSettings("version=1\nLeft=26 0\nRight=26 79\n");
    CHECK(loaded.keys.lookup(SDL_SCANCODE_W) == Key::Left);
    CHECK(loaded.keys.at(Key::Right, 0) == SDL_SCANCODE_UNKNOWN);
    CHECK(loaded.keys.at(Key::Right, 1) == SDL_SCANCODE_RIGHT);
}

TEST_CASE("settings: Pause always ends up with a key") {
    const Settings loaded = parseSettings("version=1\nPause=0 0\n");
    CHECK(loaded.keys.at(Key::Pause, 0) == SDL_SCANCODE_ESCAPE);
}
