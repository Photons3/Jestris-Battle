#pragma once

#include <optional>
#include <string>
#include <utility>

#include "app/duel.hpp"
#include "app/fonts.hpp"
#include "app/keymap.hpp"
#include "core/game.hpp"

struct ImGuiIO;
struct ImGuiStyle;

namespace tetris {

enum class Screen { Start, Playing, Duel, Controls };
enum class UiCommand { None, Play, Resume, Restart, QuitToMenu, QuitApp, CreateRoom, JoinRoom, Rematch, LeaveDuel, ToggleSound, ToggleMusic, OpenControls, CloseControls, ResetControls, SaveName, BrowseRooms, QuickMatch, JoinListedRoom, CreatePublicRoom, SavePublic, JoinInvite, CopyInviteLink };

struct StartMenu {
    int startLevel = 1;
    bool duelsEnabled = false;                // browser build only
    char joinCode[kRoomCodeLength + 1] = {};  // typed room code, uppercase
    std::string error;                        // why the last duel attempt ended, e.g. "unknown room"
    bool muted = false;                       // sound effects off for this session
    bool musicOff = false;                    // background music off for this session
    std::string playerName;                   // the saved display name, always valid
    char nameEdit[kMaxNameLength + 1] = {};   // the Name field while it is being edited
    bool publicRooms = false;                 // Create room lists the room in the lobby
    std::string pickedRoom;                   // the code clicked in the lobby, for JoinListedRoom
    std::string inviteCode;                   // from a ?room= link; shows a Join room button until used
    std::string inviteLinkBase;               // the page address plus "?room="; empty on desktop
    bool canCopyLink = false;                 // the browser offers the Clipboard API
};

// The Controls screen: which slot is waiting for a key, and what the last key press changed.
struct ControlsMenu {
    std::optional<std::pair<Key, int>> capturing;  // action and slot (0 or 1)
    std::string note;                               // e.g. "X removed from Rotate CW"
    Screen returnTo = Screen::Start;                // where Back goes: the start screen or the paused game
};

inline constexpr int kComboFadeMs = 500;

// The COMBO xN label beside the local board. It shows the live combo and fades out once the combo breaks.
struct ComboLabel {
    int shown = 0;   // the combo on the label; 0 draws nothing
    int fadeMs = 0;  // fade time left; stays full while the combo is live
    void update(int combo, int ms);  // combo is Game::combo(), or -1 with no game
    float alpha() const { return shown > 0 ? static_cast<float>(fadeMs) / kComboFadeMs : 0.0f; }
};

inline constexpr int kCountdownPopMs = 150;  // each digit settles from 1.3x to 1x over this long
inline constexpr int kGoBannerMs = 400;      // "GO!" shows for this long once play starts

float countdownPopScale(int countdownMs);  // 1.3 the frame a digit appears, easing to 1.0
bool showGoBanner(int matchClockMs);       // true for the first kGoBannerMs of the match clock

inline constexpr int kGarbageDangerLines = 4;  // the meter turns red and blinks from this many pending lines
bool garbageMeterDanger(int pending);

// IO settings for the app: no imgui.ini, and keyboard navigation so menus work without a mouse.
void configureImGuiIo(ImGuiIO& io);
// Square corners, beveled frames, the plate and gold colors, and Press Start 2P at a readable base size. Call
// after StyleColorsDark and before ScaleAllSizes.
void applyArcadeStyle(ImGuiStyle& style);
UiCommand drawStartScreen(const Fonts& fonts, StartMenu& menu);
// Each action with its two key buttons; clicking one sets `controls.capturing`.
UiCommand drawControlsScreen(const Fonts& fonts, const Keymap& keymap, ControlsMenu& controls);
// Well, pieces, hold/next panels and stats. The board and panels are hidden while paused.
void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo);
// Pause and game-over menus; draws nothing while playing.
UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted, bool musicOff);
// A small MUTED label in the top-right corner, over every screen.
void drawMutedBadge(const Fonts& fonts);
// Lobby, waiting room, countdown, both boards and the result menu, depending on the session phase. `menu`
// supplies our name, and the lobby writes the clicked room to it.
UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, StartMenu& menu);

}  // namespace tetris
