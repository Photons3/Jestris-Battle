#include <emscripten/emscripten.h>

#include <cstdlib>

#include "app/settings_store.hpp"

// EM_JS bodies aren't scanned for the JS helpers they call, so list them here.
EM_JS_DEPS(settings_store, "$stringToNewUTF8,$UTF8ToString");

// localStorage can throw (private mode, blocked site data), so every access is guarded.
EM_JS(char*, readStoredSettings, (), {
    try {
        const text = localStorage.getItem("jestris-battle.settings");
        return text === null ? 0 : stringToNewUTF8(text);
    } catch (e) {
        return 0;
    }
});

EM_JS(void, writeStoredSettings, (const char* text), {
    try {
        localStorage.setItem("jestris-battle.settings", UTF8ToString(text));
    } catch (e) {
    }
});

namespace tetris {

std::string loadSettingsText() {
    char* text = readStoredSettings();
    if (text == nullptr) return {};
    std::string out(text);
    std::free(text);
    return out;
}

void saveSettingsText(const std::string& text) { writeStoredSettings(text.c_str()); }

}  // namespace tetris
