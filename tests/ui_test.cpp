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
