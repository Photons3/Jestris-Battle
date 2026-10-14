#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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

TEST_CASE("ui: the clear callout names each part of a special clear") {
    using Lines = std::vector<CalloutLine>;
    CHECK(calloutLines({4}) == Lines{{"TETRIS", CalloutTone::Tetris}});
    CHECK(calloutLines({2, Spin::Full}) == Lines{{"T-SPIN", CalloutTone::Spin}, {"DOUBLE", CalloutTone::Spin}});
    CHECK(calloutLines({0, Spin::Mini}) == Lines{{"T-SPIN", CalloutTone::Spin}, {"MINI", CalloutTone::Spin}});
    CHECK(calloutLines({3, Spin::Full, 2}) ==
          Lines{{"B2B x2", CalloutTone::BackToBack}, {"T-SPIN", CalloutTone::Spin}, {"TRIPLE", CalloutTone::Spin}});
    CHECK(calloutLines({4, Spin::None, 1, true}) == Lines{{"B2B x1", CalloutTone::BackToBack},
                                                          {"TETRIS", CalloutTone::Tetris},
                                                          {"PERFECT", CalloutTone::Perfect},
                                                          {"CLEAR", CalloutTone::Perfect}});
    CHECK(calloutLines({1, Spin::None, 0, true}) ==
          Lines{{"PERFECT", CalloutTone::Perfect}, {"CLEAR", CalloutTone::Perfect}});
}

TEST_CASE("ui: the clear callout pops in, holds, then fades out") {
    ClearCallout callout;
    CHECK(callout.alpha() == 0.0f);
    callout.show({4});
    CHECK(callout.popScale() == doctest::Approx(1.25f));
    CHECK(callout.alpha() == 1.0f);
    callout.update(kCalloutPopMs);
    CHECK(callout.popScale() == doctest::Approx(1.0f));
    callout.update(kCalloutMs - kCalloutPopMs - kCalloutFadeMs / 2);
    CHECK(callout.alpha() == doctest::Approx(0.5f));
    callout.update(kCalloutFadeMs);
    CHECK(callout.ms == 0);
    CHECK(callout.alpha() == 0.0f);
    callout.show({2, Spin::Full});  // a new one starts over
    CHECK(callout.alpha() == 1.0f);
}

TEST_CASE("ui: an attack trail speeds up along its arc, lands, then is dropped after its burst") {
    CHECK(trailProgress(0) == 0.0f);
    CHECK(trailProgress(kTrailFlightMs / 2) < 0.5f);
    CHECK(trailProgress(kTrailFlightMs) == doctest::Approx(1.0f));
    CHECK(trailProgress(kTrailMs) == doctest::Approx(1.0f));
    AttackTrails trails;
    trails.launch(true, 3.0f);
    trails.update(kTrailMs / 2);
    trails.launch(false, 10.0f);
    trails.update(kTrailMs - kTrailMs / 2 - 1);
    REQUIRE(trails.live.size() == 2);
    trails.update(1);
    REQUIRE(trails.live.size() == 1);
    CHECK_FALSE(trails.live[0].outgoing);
}

TEST_CASE("ui: only a perfect clear flashes over the well, and only at first") {
    ClearCallout callout;
    callout.show({4});
    CHECK_FALSE(callout.perfectFlash());
    callout.show({4, Spin::None, 0, true});
    CHECK(callout.perfectFlash());
    callout.update(kPerfectFlashMs);
    CHECK_FALSE(callout.perfectFlash());
    CHECK(callout.ms > 0);
}

namespace {

// One headless ImGui frame of the Controls screen, so table and ID mistakes trip ImGui's asserts.
UiCommand controlsFrame(const Keymap& keymap, ControlsMenu& controls, Handling& handling) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    const UiCommand command = drawControlsScreen(Fonts{}, keymap, handling, controls);
    ImGui::EndFrame();
    return command;
}

UiCommand controlsFrame(const Keymap& keymap, ControlsMenu& controls) {
    Handling handling;
    return controlsFrame(keymap, controls, handling);
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

TEST_CASE("ui: the start screen draws the Name field and keeps the saved name") {
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::GetIO().Fonts->Build();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    StartMenu menu;
    menu.duelsEnabled = true;
    menu.playerName = "Sam";
    std::snprintf(menu.nameEdit, sizeof menu.nameEdit, "%s", menu.playerName.c_str());
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        CHECK(drawStartScreen(Fonts{}, menu) == UiCommand::None);
        ImGui::EndFrame();
    }
    CHECK(menu.playerName == "Sam");
    ImGui::DestroyContext();
}

TEST_CASE("ui: the lobby draws its room list") {
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::GetIO().Fonts->Build();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    DuelSession duel([](const std::string&) {});
    duel.openLobby();
    duel.onMessage("ROOMS ABCD:3:Sam WXYZ:5:Kai");
    StartMenu menu;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        CHECK(drawDuel(duel, Fonts{}, ComboLabel{}, ClearCallout{}, AttackTrails{}, menu) == UiCommand::None);
        ImGui::EndFrame();
    }
    CHECK(menu.pickedRoom.empty());
    ImGui::DestroyContext();
}

TEST_CASE("ui: each countdown digit pops in, then settles") {
    CHECK(countdownPopScale(3000) == doctest::Approx(1.3f));  // "3" just appeared
    CHECK(countdownPopScale(3000 - kCountdownPopMs / 2) == doctest::Approx(1.15f));
    CHECK(countdownPopScale(3000 - kCountdownPopMs) == doctest::Approx(1.0f));
    CHECK(countdownPopScale(2500) == doctest::Approx(1.0f));
    CHECK(countdownPopScale(2000) == doctest::Approx(1.3f));  // "2" just appeared
}

TEST_CASE("ui: GO! shows only at the very start of the match") {
    CHECK(showGoBanner(kMatchMs));
    CHECK(showGoBanner(kMatchMs - kGoBannerMs + 1));
    CHECK_FALSE(showGoBanner(kMatchMs - kGoBannerMs));
    CHECK_FALSE(showGoBanner(0));
}

TEST_CASE("ui: the garbage meter warns from four pending lines") {
    CHECK_FALSE(garbageMeterDanger(0));
    CHECK_FALSE(garbageMeterDanger(kGarbageDangerLines - 1));
    CHECK(garbageMeterDanger(kGarbageDangerLines));
}

TEST_CASE("ui: the arcade style has square corners and the plate colors") {
    ImGui::CreateContext();
    ImGuiStyle& style = ImGui::GetStyle();
    applyArcadeStyle(style);
    CHECK(style.WindowRounding == 0.0f);
    CHECK(style.FrameRounding == 0.0f);
    CHECK(style.GrabRounding == 0.0f);
    CHECK(style.FrameBorderSize > 0.0f);
    ImGui::DestroyContext();
}

TEST_CASE("ui: the SDF slider's top step is an instant soft drop") {
    CHECK(sdfSliderValue(kSoftDropInstant) == kMaxSdf + 1);
    CHECK(sdfFromSlider(kMaxSdf + 1) == kSoftDropInstant);
    CHECK(sdfSliderValue(20) == 20);
    CHECK(sdfFromSlider(20) == 20);
    CHECK(sdfFromSlider(kMinSdf) == kMinSdf);
}

TEST_CASE("ui: the handling sliders change from the keyboard and ask to be saved") {
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::GetIO().Fonts->Build();
    const Keymap keymap = defaultKeymap();
    ControlsMenu controls;
    Handling handling;
    controlsFrame(keymap, controls, handling);
    controlsFrame(keymap, controls, handling);
    auto press = [&](ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        UiCommand command = controlsFrame(keymap, controls, handling);
        ImGui::GetIO().AddKeyEvent(key, false);
        if (command == UiCommand::None) command = controlsFrame(keymap, controls, handling);
        return command;
    };
    // From Move left's first key, down past the other key rows lands on the DAS slider. Space starts editing it,
    // Right raises it, and Space again finishes the edit.
    for (int i = 0; i < kActionCount; ++i) press(ImGuiKey_DownArrow);
    std::vector<UiCommand> commands;
    for (ImGuiKey key : {ImGuiKey_Space, ImGuiKey_RightArrow, ImGuiKey_Space}) commands.push_back(press(key));
    CHECK(handling.dasMs > Handling{}.dasMs);
    CHECK(handling.arrMs == Handling{}.arrMs);
    CHECK(handling.softDropFactor == Handling{}.softDropFactor);
    for (int i = 0; i < 3; ++i) commands.push_back(controlsFrame(keymap, controls, handling));
    CHECK(std::find(commands.begin(), commands.end(), UiCommand::SaveHandling) != commands.end());
    CHECK_FALSE(controls.capturing);
    ImGui::DestroyContext();
}

TEST_CASE("ui: solo clocks and scores read naturally") {
    CHECK(formatClock(0) == "0:00.00");
    CHECK(formatClock(42180) == "0:42.18");
    CHECK(formatClock(62310) == "1:02.31");
    CHECK(formatClock(600000) == "10:00.00");
    CHECK(formatScore(0) == "0");
    CHECK(formatScore(48200) == "48,200");
    CHECK(formatScore(1234567) == "1,234,567");
}
