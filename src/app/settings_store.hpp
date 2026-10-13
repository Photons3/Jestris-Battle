#pragma once

#include <string>

namespace tetris {

// The saved settings text: localStorage in the browser, a file in SDL's pref path on desktop.
// Both fail silently; an empty string means nothing was saved yet.
std::string loadSettingsText();
void saveSettingsText(const std::string& text);

}  // namespace tetris
