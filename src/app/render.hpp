#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/duel.hpp"
#include "app/fonts.hpp"
#include "app/keymap.hpp"
#include "app/settings.hpp"
#include "app/solo.hpp"
#include "bot/bot_player.hpp"
#include "core/game.hpp"

struct ImDrawList;
struct ImGuiIO;
struct ImGuiStyle;

namespace tetris {

enum class Screen { Start, Playing, Duel, Controls };
enum class UiCommand { None, PlaySolo, PlayBot, Resume, Restart, QuitToMenu, QuitApp, CreateRoom, JoinRoom, BeginMatch, LeaveDuel, ToggleSound, ToggleMusic, ChangeVolume, SaveVolume, OpenControls, CloseControls, ResetControls, SaveHandling, SaveName, BrowseRooms, QuickMatch, JoinListedRoom, CreatePublicRoom, SavePublic, JoinInvite, CopyInviteLink };

struct StartMenu {
    int startLevel = 1;
    SoloMode soloMode = SoloMode::Marathon;   // the solo button last clicked, for PlaySolo
    BotLevel botLevel = BotLevel::Medium;     // the practice bot's strength, for PlayBot
    PersonalBests bests;                      // shown beside the timed modes; saved with the settings
    bool duelsEnabled = false;                // browser build only
    char joinCode[kRoomCodeLength + 1] = {};  // typed room code, uppercase
    std::string error;                        // why the last duel attempt ended, e.g. "unknown room"
    bool muted = false;                       // sound effects off for this session
    bool musicOff = false;                    // background music off for this session
    int soundVolume = kMaxVolume;             // effects volume in percent, saved with the settings
    int musicVolume = kMaxVolume;             // music volume in percent, saved with the settings
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

inline constexpr int kCalloutMs = 1800;      // how long a special clear stays named beside the board
inline constexpr int kCalloutFadeMs = 500;   // the last part of that, fading out
inline constexpr int kCalloutPopMs = 150;    // the text settles from 1.25x to 1x over this long
inline constexpr int kPerfectFlashMs = 1000; // PERFECT CLEAR also flashes over the well for this long

// The callout under the HOLD panel naming the last special clear: B2B xN, T-SPIN (MINI) DOUBLE, TETRIS, PERFECT
// CLEAR. A new one replaces it at full strength.
struct ClearCallout {
    ClearAward shown;  // the clear on the callout
    int ms = 0;        // time left on screen; 0 draws nothing
    void show(const ClearAward& award) {
        shown = award;
        ms = kCalloutMs;
    }
    void update(int elapsedMs) { ms = std::max(0, ms - elapsedMs); }
    float alpha() const { return std::min(1.0f, static_cast<float>(ms) / kCalloutFadeMs); }
    float popScale() const;  // 1.25 when it appears, easing to 1.0
    bool perfectFlash() const { return shown.perfectClear && ms > kCalloutMs - kPerfectFlashMs; }
};

enum class CalloutTone { BackToBack, Spin, Tetris, Perfect };  // picks each line's color

struct CalloutLine {
    std::string text;
    CalloutTone tone;
    friend bool operator==(const CalloutLine&, const CalloutLine&) = default;
};

// The callout's lines, top to bottom, each short enough for the HOLD column.
std::vector<CalloutLine> calloutLines(const ClearAward& award);

inline constexpr int kTrailFlightMs = 420;  // an attack's flight from one well to the other's garbage meter
inline constexpr int kTrailImpactMs = 180;  // then a burst on the meter
inline constexpr int kTrailMs = kTrailFlightMs + kTrailImpactMs;

// Garbage in flight in a duel: each attack arcs from the sender's well to the receiver's garbage meter.
struct AttackTrail {
    bool outgoing;  // ours, flying to their meter
    float fromRow;  // board row it leaves from, in the sender's well
    int ms = 0;     // time since launch
};

struct AttackTrails {
    std::vector<AttackTrail> live;
    void launch(bool outgoing, float fromRow) { live.push_back({outgoing, fromRow}); }
    void update(int ms);  // ages each trail and drops the ones that have landed
};

// How far along its arc a trail `ms` into its flight is: 0 to 1, slow off the well and fast into the meter.
float trailProgress(int ms);

inline constexpr int kCountdownPopMs = 150;  // each digit settles from 1.3x to 1x over this long
inline constexpr int kGoBannerMs = 400;      // "GO!" shows for this long once play starts

float countdownPopScale(int countdownMs);  // 1.3 the frame a digit appears, easing to 1.0
bool showGoBanner(int matchClockMs);       // true for the first kGoBannerMs of the match clock

inline constexpr int kGarbageDangerLines = 4;  // the meter turns red and blinks from this many pending lines
bool garbageMeterDanger(int pending);
inline constexpr float kTravellingGarbageAlpha = 0.4f;  // meter cells for garbage that cannot rise yet
// Garbage waiting to rise, as a stack of beveled garbage cells, one per line, with its left edge at `x`: the bottom
// `ready` cells solid, the travelling ones above them dimmed. From kGarbageDangerLines the meter turns red and
// its ready cells blink every 250 ms.
void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending, int ready);

// A royale's opponents as a grid of mini boards.
struct GridShape {
    int cols = 1;
    int rows = 1;
    float cell = 0.0f;  // mini-board cell size in points
};
// Mini boards are kMiniCols x kMiniRows cells (well 10 + meter 1 + gap 1, by visible 20 + name 1 + gap 1).
inline constexpr int kMiniCols = 12;
inline constexpr int kMiniRows = 22;
// The cols x rows grid for `boards` mini boards in a width x height area with the largest cell;
// ties go to fewer columns.
GridShape royaleGrid(int boards, float width, float height);
inline constexpr float kMiniCellCap = 0.75f;  // a mini board's cell, at most, as a fraction of your own
// The mini boards' cell in whole pixels: the grid's cell, capped so they stay smaller than your own well.
float royaleMiniCell(float gridCell, float ownCell);

// The room screen's line under the code: listed, private, or a neutral hint when this player can't know.
const char* roomVisibilityText(RoomVisibility visibility);

// One roster line on the room and result screens: a star on the host, " (next match)" on a player who isn't in
// the running match, and `me` for our own row, drawn in our plate color.
struct RosterRow {
    std::string text;
    bool me = false;
};
std::vector<RosterRow> rosterRows(const std::vector<RosterEntry>& roster, int hostId, int myId);

// IO settings for the app: no imgui.ini, and keyboard navigation so menus work without a mouse.
void configureImGuiIo(ImGuiIO& io);
// Square corners, beveled frames, the plate and gold colors, and Press Start 2P at a readable base size. Call
// after StyleColorsDark and before ScaleAllSizes.
void applyArcadeStyle(ImGuiStyle& style);
UiCommand drawStartScreen(const Fonts& fonts, StartMenu& menu);
// Each action with its two key buttons; clicking one sets `controls.capturing`.
// Below them, the Handling sliders edit `handling` in place; letting go of one returns SaveHandling.
UiCommand drawControlsScreen(const Fonts& fonts, const Keymap& keymap, Handling& handling, ControlsMenu& controls);

// The SDF slider runs from kMinSdf to one step past kMaxSdf, which stands for an instant soft drop.
int sdfSliderValue(int softDropFactor);
int sdfFromSlider(int value);
// Well, pieces, hold/next panels and stats. The board and panels are hidden while paused.
void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo, const ClearCallout& callout);
// Pause and game-over menus; draws nothing while playing. The pause menu edits the audio fields of `menu`.
UiCommand drawOverlay(const Game& game, const Fonts& fonts, StartMenu& menu);
std::string formatClock(int ms);     // "0:42.18": minutes, seconds and hundredths
std::string formatScore(int score);  // "48,200"
// The mode line under the side-panel stats: the clock and lines left, the Blitz time left, or ZEN.
void drawSoloStatus(const SoloSession& solo, const Fonts& fonts);
// The countdown, the result of a timed run (Retry, or R, returns Restart), else the pause and game-over menus.
UiCommand drawSoloOverlay(const SoloSession& solo, bool newBest, const Fonts& fonts, StartMenu& menu);
// A small MUTED label in the top-right corner, over every screen.
void drawMutedBadge(const Fonts& fonts);
// Lobby, connecting, the room screen with its roster, countdown, both boards and the result menu, depending on
// the session phase. Room and Result end with the host's Start, or the wait for it, and Leave. `menu` supplies our
// name, and the lobby writes the clicked room to it.
UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, const ClearCallout& callout,
                   const AttackTrails& trails, StartMenu& menu);

}  // namespace tetris
