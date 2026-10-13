#include <doctest/doctest.h>

#include <vector>

#include "app/input.hpp"

using namespace tetris;

namespace {

using Actions = std::vector<Action>;

Actions tickFor(InputMapper& input, int ms) {
    Actions out;
    for (int i = 0; i < ms; ++i) input.tick(out);
    return out;
}

}  // namespace

TEST_CASE("input: pressing a direction moves once immediately") {
    InputMapper input;
    Actions out;
    input.press(Key::Left, out);
    CHECK(out == Actions{Action::Left});
}

TEST_CASE("input: holding a direction repeats after DAS, then every ARR") {
    InputMapper input;
    Actions out;
    input.press(Key::Right, out);
    CHECK(tickFor(input, kDasMs - 1).empty());
    CHECK(tickFor(input, 1) == Actions{Action::Right});
    CHECK(tickFor(input, kArrMs - 1).empty());
    CHECK(tickFor(input, 1) == Actions{Action::Right});
}

TEST_CASE("input: releasing the direction stops auto-repeat") {
    InputMapper input;
    Actions out;
    input.press(Key::Left, out);
    tickFor(input, 200);
    input.release(Key::Left);
    CHECK(tickFor(input, 500).empty());
}

TEST_CASE("input: the newest direction wins, and releasing it falls back to the other") {
    InputMapper input;
    Actions out;
    input.press(Key::Left, out);
    input.press(Key::Right, out);
    CHECK(out == Actions{Action::Left, Action::Right});
    input.release(Key::Right);
    CHECK(tickFor(input, kDasMs - 1).empty());
    CHECK(tickFor(input, 1) == Actions{Action::Left});
}

TEST_CASE("input: releasing the inactive direction does not restart DAS") {
    InputMapper input;
    Actions out;
    input.press(Key::Right, out);
    input.press(Key::Left, out);
    tickFor(input, 100);
    input.release(Key::Right);
    CHECK(tickFor(input, kDasMs - 101).empty());
    CHECK(tickFor(input, 1) == Actions{Action::Left});
}

TEST_CASE("input: one-shot keys emit their action once") {
    InputMapper input;
    Actions out;
    for (Key key : {Key::HardDrop, Key::RotateCW, Key::RotateCCW, Key::Hold, Key::Pause}) input.press(key, out);
    CHECK(out == Actions{Action::HardDrop, Action::RotateCW, Action::RotateCCW, Action::Hold, Action::Pause});
    CHECK(tickFor(input, 1000).empty());
}

TEST_CASE("input: soft drop follows the key") {
    InputMapper input;
    Actions out;
    input.press(Key::SoftDrop, out);
    CHECK(out.empty());
    CHECK(input.softDrop());
    input.release(Key::SoftDrop);
    CHECK_FALSE(input.softDrop());
}

TEST_CASE("input: releaseAll forgets held keys") {
    InputMapper input;
    Actions out;
    input.press(Key::Left, out);
    input.press(Key::SoftDrop, out);
    input.releaseAll();
    CHECK_FALSE(input.softDrop());
    CHECK(tickFor(input, 500).empty());
}
