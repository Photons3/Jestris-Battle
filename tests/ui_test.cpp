#include <doctest/doctest.h>

#include "app/render.hpp"
#include "imgui.h"

using namespace tetris;

TEST_CASE("ui: menus are keyboard-navigable") {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    configureImGuiIo(io);
    CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
    CHECK(io.IniFilename == nullptr);
    ImGui::DestroyContext();
}

TEST_CASE("ui: the combo label follows the live combo") {
    ComboLabel label;
    CHECK(label.shown == 0);
    CHECK(label.alpha() == 0.0f);
    label.update(-1, 16);
    CHECK(label.shown == 0);
    label.update(1, 16);
    CHECK(label.shown == 1);
    CHECK(label.alpha() == 1.0f);
    label.update(3, 16);
    CHECK(label.shown == 3);
}

TEST_CASE("ui: the combo label fades out once the combo breaks") {
    ComboLabel label;
    label.update(2, 16);
    label.update(-1, kComboFadeMs / 2);
    CHECK(label.shown == 2);
    CHECK(label.alpha() == doctest::Approx(0.5f));
    label.update(-1, kComboFadeMs / 2);
    CHECK(label.shown == 0);
    CHECK(label.alpha() == 0.0f);
}

TEST_CASE("ui: a new combo during the fade shows at full strength") {
    ComboLabel label;
    label.update(4, 16);
    label.update(-1, 100);
    label.update(1, 16);
    CHECK(label.shown == 1);
    CHECK(label.alpha() == 1.0f);
}

namespace {

// One headless ImGui frame of the Controls screen, so table and ID mistakes trip ImGui's asserts.
UiCommand controlsFrame(const Keymap& keymap, ControlsMenu& controls) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    const UiCommand command = drawControlsScreen(Fonts{}, keymap, controls);
    ImGui::EndFrame();
    return command;
}

}  // namespace

TEST_CASE("ui: the controls screen draws, and a key button pressed from the keyboard waits for a key") {
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::GetIO().Fonts->Build();
    const Keymap keymap = defaultKeymap();
    ControlsMenu controls;
    controls.note = "X removed from Rotate CW";
    CHECK(controlsFrame(keymap, controls) == UiCommand::None);
    CHECK(controlsFrame(keymap, controls) == UiCommand::None);
    CHECK_FALSE(controls.capturing);

    // Down moves keyboard focus from Move left's first key to Move right's; Enter presses it.
    for (ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_Enter}) {
        ImGui::GetIO().AddKeyEvent(key, true);
        controlsFrame(keymap, controls);
        ImGui::GetIO().AddKeyEvent(key, false);
        controlsFrame(keymap, controls);
    }
    REQUIRE(controls.capturing);
    CHECK(controls.capturing->first == Key::Right);
    CHECK(controls.capturing->second == 0);
    CHECK(controls.note.empty());
    controlsFrame(keymap, controls);  // the "Press a key..." label keeps the same ID
    ImGui::DestroyContext();
}
