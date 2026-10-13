#pragma once

struct ImFont;

namespace tetris {

struct Fonts {
    ImFont* ui = nullptr;   // ImGui default font, for menus
    ImFont* hud = nullptr;  // Press Start 2P, for titles and HUD
};

// Call once after ImGui::CreateContext(). Sizes are chosen per draw call (ImGui 1.92 dynamic fonts).
Fonts loadFonts();

}  // namespace tetris
