#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

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

struct Host {
    SDL_Window* window = nullptr;
    bool gfxReady = false;
    Fonts fonts;
    App app;
    Uint64 last = 0;
};

bool windowHidden(SDL_Window* window) {
#ifdef __EMSCRIPTEN__
    (void)window;
    EmscriptenVisibilityChangeEvent status;
    return emscripten_get_visibility_status(&status) == EMSCRIPTEN_RESULT_SUCCESS && status.hidden;
#else
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
#endif
}

void syncVisibility(Host& host) { setHidden(host.app, windowHidden(host.window)); }

#ifdef __EMSCRIPTEN__
// Browsers stop requestAnimationFrame in a hidden tab, which would freeze a duel mid-air on the
// opponent's screen. A timer keeps the loop going there (throttled to about once a second). This runs
// as an event watch because a stopped loop would never get to see the HIDDEN event in SDL_AppEvent.
bool switchLoopTiming(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_HIDDEN) emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 100);
    if (event->type == SDL_EVENT_WINDOW_SHOWN) emscripten_set_main_loop_timing(EM_TIMING_RAF, 1);
    return true;
}
#endif

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
    host->window = SDL_CreateWindow("Jestris Battle", static_cast<int>(800 * scale), static_cast<int>(900 * scale),
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
#ifdef __EMSCRIPTEN__
    SDL_AddEventWatch(switchLoopTiming, nullptr);
#endif

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
    // A key pressed for the Controls screen is a binding, not menu navigation.
    const bool capturedKey = event->type == SDL_EVENT_KEY_DOWN && host.app.controls.capturing;
    if (!capturedKey) ImGui_ImplSDL3_ProcessEvent(event);
    handleEvent(host.app, host.window, *event);
    return host.app.quit ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto& host = *static_cast<Host*>(appstate);
    const Uint64 now = SDL_GetTicks();
    syncVisibility(host);
    pumpNetwork(host.app);
    // A long gap is replayed in short steps, so the duel sends snapshots and freezes on KO or 0:00 as usual.
    int left = frameBudgetMs(now - host.last, host.app.screen == Screen::Duel && host.app.duel.inMatch());
    do {
        const int ms = std::min(left, kMaxFrameMs);
        step(host.app, ms);
        left -= ms;
    } while (left > 0);
    host.last = now;
    if (host.app.hidden) {
#ifndef __EMSCRIPTEN__
        SDL_Delay(10);
#endif
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
