#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>

#include "app/app.hpp"
#include "app/fonts.hpp"
#include "app/gfx.hpp"
#include "app/render.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

using namespace tetris;

namespace {

constexpr Uint64 kMaxFrameMs = 100;  // longer stalls are dropped, not replayed

struct Host {
    SDL_Window* window = nullptr;
    bool gfxReady = false;
    Fonts fonts;
    App app;
    Uint64 last = 0;
};

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    auto* host = new Host;
    *appstate = host;
#ifdef __EMSCRIPTEN__
    const float scale = 1.0f;  // ImGui works in CSS pixels; SDL applies devicePixelRatio itself
#else
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
#endif
    host->window = SDL_CreateWindow("Tetris", static_cast<int>(800 * scale), static_cast<int>(900 * scale),
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                                        gfxPrepareWindow());
    if (host->window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
#ifndef __EMSCRIPTEN__
    SDL_SetWindowMinimumSize(host->window, 640, 720);
    SDL_SetWindowPosition(host->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
    SDL_ShowWindow(host->window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    configureImGuiIo(ImGui::GetIO());
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    if (!gfxInit(host->window)) return SDL_APP_FAILURE;
    host->gfxReady = true;
    host->fonts = loadFonts();
    host->last = SDL_GetTicks();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto& host = *static_cast<Host*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);
    handleEvent(host.app, host.window, *event);
    return host.app.quit ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto& host = *static_cast<Host*>(appstate);
    const Uint64 now = SDL_GetTicks();
    pumpNetwork(host.app);
    step(host.app, static_cast<int>(std::min(now - host.last, kMaxFrameMs)));
    host.last = now;
    if (SDL_GetWindowFlags(host.window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return SDL_APP_CONTINUE;
    }
    gfxNewFrame();
    ImGui::NewFrame();
    drawFrame(host.app, host.fonts);
    ImGui::Render();
    gfxRender();
    return host.app.quit ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    auto* host = static_cast<Host*>(appstate);
    if (host == nullptr) return;
    if (host->gfxReady) gfxShutdown();
    if (ImGui::GetCurrentContext() != nullptr) ImGui::DestroyContext();
    if (host->window != nullptr) SDL_DestroyWindow(host->window);
    delete host;  // SDL calls SDL_Quit() after this returns
}
