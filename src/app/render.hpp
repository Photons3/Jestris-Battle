#pragma once

#include <string>

#include "app/duel.hpp"
#include "app/fonts.hpp"
#include "core/game.hpp"

struct ImGuiIO;

namespace tetris {

enum class Screen { Start, Playing, Duel };
enum class UiCommand { None, Play, Resume, Restart, QuitToMenu, QuitApp, CreateRoom, JoinRoom, Rematch, LeaveDuel, ToggleSound, ToggleMusic };

struct StartMenu {
    int startLevel = 1;
    bool duelsEnabled = false;                // browser build only
    char joinCode[kRoomCodeLength + 1] = {};  // typed room code, uppercase
    std::string error;                        // why the last duel attempt ended, e.g. "unknown room"
    bool muted = false;                       // sound effects off for this session
    bool musicOff = false;                    // background music off for this session
};

inline constexpr int kComboFadeMs = 500;

// The COMBO xN label beside the local board. It shows the live combo and fades out once the combo breaks.
struct ComboLabel {
    int shown = 0;   // the combo on the label; 0 draws nothing
    int fadeMs = 0;  // fade time left; stays full while the combo is live
    void update(int combo, int ms);  // combo is Game::combo(), or -1 with no game
    float alpha() const { return shown > 0 ? static_cast<float>(fadeMs) / kComboFadeMs : 0.0f; }
};

// IO settings for the app: no imgui.ini, and keyboard navigation so menus work without a mouse.
void configureImGuiIo(ImGuiIO& io);
UiCommand drawStartScreen(const Fonts& fonts, StartMenu& menu);
// Well, pieces, hold/next panels and stats. The board and panels are hidden while paused.
void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo);
// Pause and game-over menus; draws nothing while playing.
UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted, bool musicOff);
// A small MUTED label in the top-right corner, over every screen.
void drawMutedBadge(const Fonts& fonts);
// Waiting room, countdown, both boards and the result menu, depending on the session phase.
UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo);

}  // namespace tetris
