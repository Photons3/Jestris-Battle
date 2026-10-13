#include "app/fonts.hpp"

#include "app/font_data.hpp"
#include "imgui.h"

namespace tetris {

Fonts loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    Fonts fonts;
    fonts.ui = io.Fonts->AddFontDefault();  // first font added = default font
    fonts.hud = io.Fonts->AddFontFromMemoryCompressedTTF(PressStart2P_compressed_data,
                                                         static_cast<int>(PressStart2P_compressed_size), 16.0f);
    return fonts;
}

}  // namespace tetris
