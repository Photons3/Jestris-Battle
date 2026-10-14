#include <doctest/doctest.h>

#include "app/settings.hpp"
#include "net/protocol.hpp"

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

TEST_CASE("settings: the player name round-trips") {
    Settings settings;
    settings.playerName = "Sam_99";
    CHECK(parseSettings(serializeSettings(settings)).playerName == "Sam_99");
}

TEST_CASE("settings: a missing or invalid name loads as empty") {
    CHECK(parseSettings("version=1\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=two words\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=abcdefghijklm\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=\n").playerName.empty());
}

TEST_CASE("settings: the default name is Player and four digits") {
    CHECK(defaultPlayerName(4821) == "Player4821");
    CHECK(defaultPlayerName(7) == "Player0007");
    CHECK(defaultPlayerName(123456) == "Player3456");
    CHECK(isValidName(defaultPlayerName(0xFFFFFFFFu)));
}

TEST_CASE("settings: the public-room choice round-trips and defaults to off") {
    Settings settings;
    CHECK_FALSE(parseSettings(serializeSettings(settings)).publicRooms);
    settings.publicRooms = true;
    CHECK(parseSettings(serializeSettings(settings)).publicRooms);
}

TEST_CASE("settings: the handling round-trips, instant soft drop included") {
    Settings settings;
    settings.handling = {80, 0, kSoftDropInstant};
    CHECK(parseSettings(serializeSettings(settings)).handling == settings.handling);
    settings.handling = {0, 12, 35};
    CHECK(parseSettings(serializeSettings(settings)).handling == settings.handling);
}

TEST_CASE("settings: out-of-range handling is clamped and junk keeps the defaults") {
    CHECK(parseSettings("version=1\ndas=9999\narr=-5\nsdf=2\n").handling == Handling{kMaxDasMs, 0, kMinSdf});
    CHECK(parseSettings("version=1\ndas=abc\narr=12x\nsdf=\n").handling == Handling{});
}

TEST_CASE("settings: the volumes round-trip and default to 100") {
    CHECK(Settings{}.soundVolume == 100);
    CHECK(Settings{}.musicVolume == 100);
    Settings settings;
    settings.soundVolume = 35;
    settings.musicVolume = 0;
    const Settings loaded = parseSettings(serializeSettings(settings));
    CHECK(loaded.soundVolume == 35);
    CHECK(loaded.musicVolume == 0);
}

TEST_CASE("settings: out-of-range volumes are clamped, junk and old files keep 100") {
    const Settings clamped = parseSettings("version=1\nsoundvol=250\nmusicvol=-4\n");
    CHECK(clamped.soundVolume == 100);
    CHECK(clamped.musicVolume == 0);
    const Settings junk = parseSettings("version=1\nsoundvol=loud\nmusicvol=\n");
    CHECK(junk.soundVolume == 100);
    CHECK(junk.musicVolume == 100);
    const Settings old = parseSettings("version=1\nsound=off\nmusic=on\n");
    CHECK(old.soundVolume == 100);
    CHECK(old.musicVolume == 100);
}

TEST_CASE("settings: a file saved before handling existed loads the default handling") {
    CHECK(parseSettings("version=1\nsound=off\npublic=on\n").handling == Handling{});
}

TEST_CASE("settings: personal bests load back, and unset ones stay unset") {
    Settings saved;
    saved.bests.sprintMs = 62310;
    saved.bests.blitzScore = 48200;
    const Settings loaded = parseSettings(serializeSettings(saved));
    CHECK(loaded.bests == saved.bests);
    CHECK_FALSE(loaded.bests.cheeseMs);
    CHECK(serializeSettings(Settings{}).find("sprint=") == std::string::npos);
}

TEST_CASE("settings: bad personal bests are skipped") {
    const std::string good = serializeSettings(Settings{});
    const Settings loaded = parseSettings(good + "sprint=-5\nblitz=abc\ncheese=0\n");
    CHECK(loaded.bests == PersonalBests{});
    CHECK(loaded.keys.keys == defaultKeymap().keys);
}
