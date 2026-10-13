#pragma once

#include <SDL3/SDL.h>

namespace tetris {

// ImGui rendering backend: SDL_GPU on desktop (gfx_sdlgpu.cpp), WebGL in the browser (gfx_webgl.cpp).
SDL_WindowFlags gfxPrepareWindow();  // sets attributes needed before window creation; returns extra window flags
bool gfxInit(SDL_Window* window);    // after ImGui::CreateContext(); also starts the ImGui SDL3 platform backend
void gfxNewFrame();                  // before ImGui::NewFrame()
void gfxRender();                    // draws ImGui::GetDrawData() and presents
void gfxShutdown();                  // before ImGui::DestroyContext()

}  // namespace tetris
