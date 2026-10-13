#include <SDL3/SDL_opengles2.h>
#include <emscripten/em_asm.h>
#include <emscripten/html5.h>

#include <cstdio>

#include "app/gfx.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace tetris {
namespace {

SDL_Window* target = nullptr;
SDL_GLContext context = nullptr;

// Keeps the canvas the size of the browser window.
EM_BOOL onResize(int, const EmscriptenUiEvent* event, void*) {
    SDL_SetWindowSize(target, event->windowInnerWidth, event->windowInnerHeight);
    return EM_FALSE;
}

}  // namespace

SDL_WindowFlags gfxPrepareWindow() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    return SDL_WINDOW_OPENGL;
}

bool gfxInit(SDL_Window* window) {
    target = window;
    context = SDL_GL_CreateContext(window);
    if (context == nullptr) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(1);
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init(nullptr);
    SDL_SetWindowSize(window, EM_ASM_INT({ return window.innerWidth; }), EM_ASM_INT({ return window.innerHeight; }));
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onResize);
    return true;
}

void gfxNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

void gfxRender() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(target, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.06f, 0.06f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(target);
}

void gfxShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    SDL_GL_DestroyContext(context);
}

}  // namespace tetris
