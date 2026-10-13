#include <SDL3/SDL.h>

#include <fstream>
#include <sstream>

#include "app/settings_store.hpp"

namespace tetris {
namespace {

// Per-user folder, e.g. ~/Library/Application Support/Jestris/Jestris Battle/. Empty if unavailable.
std::string settingsPath() {
    char* dir = SDL_GetPrefPath("Jestris", "Jestris Battle");
    if (dir == nullptr) return {};
    std::string path = std::string(dir) + "settings.txt";
    SDL_free(dir);
    return path;
}

}  // namespace

std::string loadSettingsText() {
    const std::string path = settingsPath();
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void saveSettingsText(const std::string& text) {
    const std::string path = settingsPath();
    if (path.empty()) return;
    // Write a temp file then rename, so a crash mid-write never leaves a half file.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file || !(file << text) || !file.flush()) return;
    }
    SDL_RenamePath(temp.c_str(), path.c_str());
}

}  // namespace tetris
