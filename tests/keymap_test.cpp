#include <doctest/doctest.h>

#include <string>

#include "app/keymap.hpp"

using namespace tetris;

TEST_CASE("keymap: defaults keep the familiar keys") {
    const Keymap map = defaultKeymap();
    CHECK(map.lookup(SDL_SCANCODE_LEFT) == Key::Left);
    CHECK(map.lookup(SDL_SCANCODE_RIGHT) == Key::Right);
    CHECK(map.lookup(SDL_SCANCODE_DOWN) == Key::SoftDrop);
    CHECK(map.lookup(SDL_SCANCODE_SPACE) == Key::HardDrop);
    CHECK(map.lookup(SDL_SCANCODE_UP) == Key::RotateCW);
    CHECK(map.lookup(SDL_SCANCODE_X) == Key::RotateCW);
    CHECK(map.lookup(SDL_SCANCODE_Z) == Key::RotateCCW);
    CHECK(map.lookup(SDL_SCANCODE_LCTRL) == Key::RotateCCW);
    CHECK(map.lookup(SDL_SCANCODE_C) == Key::Hold);
    CHECK(map.lookup(SDL_SCANCODE_LSHIFT) == Key::Hold);
    CHECK(map.lookup(SDL_SCANCODE_ESCAPE) == Key::Pause);
    CHECK(map.lookup(SDL_SCANCODE_P) == Key::Pause);
    CHECK_FALSE(map.lookup(SDL_SCANCODE_RCTRL));
    CHECK_FALSE(map.lookup(SDL_SCANCODE_M));
    CHECK_FALSE(map.lookup(SDL_SCANCODE_UNKNOWN));
}

TEST_CASE("keymap: binding a free key adds it") {
    Keymap map = defaultKeymap();
    CHECK_FALSE(map.bind(Key::HardDrop, 1, SDL_SCANCODE_W));
    CHECK(map.lookup(SDL_SCANCODE_W) == Key::HardDrop);
    CHECK(map.lookup(SDL_SCANCODE_SPACE) == Key::HardDrop);
}

TEST_CASE("keymap: binding a used key moves it and reports where from") {
    Keymap map = defaultKeymap();
    CHECK(map.bind(Key::HardDrop, 0, SDL_SCANCODE_X) == Key::RotateCW);
    CHECK(map.lookup(SDL_SCANCODE_X) == Key::HardDrop);
    CHECK(map.at(Key::RotateCW, 1) == SDL_SCANCODE_UNKNOWN);
    CHECK(map.at(Key::RotateCW, 0) == SDL_SCANCODE_UP);
    CHECK_FALSE(map.lookup(SDL_SCANCODE_SPACE));  // replaced
}

TEST_CASE("keymap: moving a key between an action's own slots reports nothing") {
    Keymap map = defaultKeymap();
    CHECK_FALSE(map.bind(Key::RotateCW, 0, SDL_SCANCODE_X));
    CHECK(map.at(Key::RotateCW, 0) == SDL_SCANCODE_X);
    CHECK(map.at(Key::RotateCW, 1) == SDL_SCANCODE_UNKNOWN);
}

TEST_CASE("keymap: binding the key a slot already has changes nothing") {
    Keymap map = defaultKeymap();
    CHECK_FALSE(map.bind(Key::Hold, 0, SDL_SCANCODE_C));
    CHECK(map.keys == defaultKeymap().keys);
}

TEST_CASE("keymap: clearing Pause's last key puts Escape back") {
    Keymap map = defaultKeymap();
    map.bind(Key::Hold, 1, SDL_SCANCODE_P);  // takes P from Pause
    map.clear(Key::Pause, 0);
    CHECK(map.at(Key::Pause, 0) == SDL_SCANCODE_ESCAPE);
    CHECK(map.lookup(SDL_SCANCODE_P) == Key::Hold);
}

TEST_CASE("keymap: Escape returning to Pause is taken from any other action") {
    Keymap map = defaultKeymap();
    map.keys[static_cast<int>(Key::Pause)] = {SDL_SCANCODE_P, SDL_SCANCODE_UNKNOWN};
    map.keys[static_cast<int>(Key::Hold)][1] = SDL_SCANCODE_ESCAPE;
    map.clear(Key::Pause, 0);
    CHECK(map.lookup(SDL_SCANCODE_ESCAPE) == Key::Pause);
    CHECK(map.at(Key::Hold, 1) == SDL_SCANCODE_UNKNOWN);
}

TEST_CASE("keymap: mute, Enter, Escape and Backspace are reserved") {
    CHECK(isReservedKey(SDL_SCANCODE_M));
    CHECK(isReservedKey(SDL_SCANCODE_RETURN));
    CHECK(isReservedKey(SDL_SCANCODE_KP_ENTER));
    CHECK(isReservedKey(SDL_SCANCODE_ESCAPE));
    CHECK(isReservedKey(SDL_SCANCODE_BACKSPACE));
    CHECK_FALSE(isReservedKey(SDL_SCANCODE_W));
}

TEST_CASE("keymap: names for the Controls screen") {
    CHECK(std::string(actionName(Key::RotateCCW)) == "Rotate CCW");
    CHECK(std::string(keyName(SDL_SCANCODE_UNKNOWN)) == "-");
    CHECK(std::string(keyName(SDL_SCANCODE_SPACE)) == "Space");
}
