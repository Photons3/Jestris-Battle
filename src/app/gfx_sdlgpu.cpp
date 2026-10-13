#include <cstdio>

#include "app/gfx.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

namespace tetris {
namespace {

SDL_GPUDevice* device = nullptr;
SDL_Window* target = nullptr;

}  // namespace

SDL_WindowFlags gfxPrepareWindow() { return 0; }

bool gfxInit(SDL_Window* window) {
    device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (device == nullptr || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "SDL_GPU setup failed: %s\n", SDL_GetError());
        return false;
    }
    target = window;
    SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&info);
    return true;
}

void gfxNewFrame() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

void gfxRender() {
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUTexture* swapchain = nullptr;
    SDL_WaitAndAcquireGPUSwapchainTexture(commands, target, &swapchain, nullptr, nullptr);
    if (swapchain != nullptr && !minimized) {
        ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commands);
        SDL_GPUColorTargetInfo colorTarget{};
        colorTarget.texture = swapchain;
        colorTarget.clear_color = SDL_FColor{0.06f, 0.06f, 0.09f, 1.0f};
        colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTarget.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, commands, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(commands);
}

void gfxShutdown() {
    SDL_WaitForGPUIdle(device);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    SDL_ReleaseWindowFromGPUDevice(device, target);
    SDL_DestroyGPUDevice(device);
}

}  // namespace tetris
