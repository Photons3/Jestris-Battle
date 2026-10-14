// src/app/app.cpp
#include "app/app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

#include "app/keymap.hpp"
#include "app/settings.hpp"
#include "app/settings_store.hpp"
#include "app/sound_cues.hpp"
#include "imgui.h"

namespace tetris {
namespace {

void applyCommand(App& app, UiCommand command) {
    switch (command) {
    case UiCommand::None:
        break;
    case UiCommand::Play:
    case UiCommand::Restart:
        app.newGame();
        break;
    case UiCommand::ToggleSound:
        app.toggleSound();
        break;
    case UiCommand::ToggleMusic:
        app.toggleMusic();
        break;
    case UiCommand::Resume:
        if (app.game) app.game->resume();
        break;
    case UiCommand::QuitToMenu:
        app.game.reset();
        app.screen = Screen::Start;
        break;
    case UiCommand::QuitApp:
        app.quit = true;
        break;
    case UiCommand::SaveName:
        app.saveSettings();
        break;
    case UiCommand::CreateRoom:
        app.openDuel();
        app.duel.createRoom(app.menu.startLevel, app.menu.publicRooms);
        break;
    case UiCommand::CreatePublicRoom:
        app.openDuel();
        app.duel.createRoom(app.menu.startLevel, true);
        break;
    case UiCommand::BrowseRooms:
        app.openDuel();
        app.duel.openLobby();
        break;
    case UiCommand::QuickMatch:
        app.openDuel();
        app.duel.quickMatch(app.menu.startLevel);
        break;
    case UiCommand::JoinListedRoom:
        app.duel.joinRoom(app.menu.pickedRoom);
        break;
    case UiCommand::SavePublic:
        app.saveSettings();
        break;
    case UiCommand::JoinInvite:
        std::snprintf(app.menu.joinCode, sizeof app.menu.joinCode, "%s", app.menu.inviteCode.c_str());
        app.menu.inviteCode.clear();
        app.openDuel();
        app.duel.joinRoom(app.menu.joinCode);
        break;
    case UiCommand::CopyInviteLink:
        copyInviteLink(app.duel.roomCode());
        break;
    case UiCommand::JoinRoom:
        app.openDuel();
        app.duel.joinRoom(app.menu.joinCode);
        break;
    case UiCommand::Rematch:
        app.duel.requestRematch();
        break;
    case UiCommand::LeaveDuel:
        app.closeDuel();
        break;
    case UiCommand::OpenControls:
        app.controls = ControlsMenu{};
        app.controls.returnTo = app.screen;
        app.input.releaseAll();
        app.screen = Screen::Controls;
        break;
    case UiCommand::CloseControls:
        app.controls.capturing.reset();
        app.screen = app.controls.returnTo;  // a solo game is still paused underneath
        break;
    case UiCommand::ResetControls:
        app.keymap = defaultKeymap();
        app.handling = {};
        app.input.setHandling(app.handling);
        app.controls.capturing.reset();
        app.controls.note = "Keys and handling reset";
        app.saveSettings();
        break;
    case UiCommand::SaveHandling:
        app.handling = clampHandling(app.handling);
        app.input.setHandling(app.handling);
        app.saveSettings();
        break;
    }
}

// The local game on screen: the solo game, or our duel game.
Game* shownGame(App& app) {
    if (app.screen == Screen::Duel) return app.duel.game();
    return app.game ? &*app.game : nullptr;
}

float middleRow(const std::vector<int>& rows) {
    if (rows.empty()) return 0.0f;
    return static_cast<float>(rows.front() + rows.back()) * 0.5f;
}

// The row just above the opponent's highest block, where their attacks take off.
float stackTopRow(const Snapshot& snapshot) {
    for (int y = kVisibleHeight - 1; y >= 0; --y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (snapshot.cells[y * kBoardWidth + x]) return static_cast<float>(std::min(y + 1, kVisibleHeight - 1));
    return 0.0f;
}

// Plays what the solo or duel game and the duel session reported this frame, and launches attack trails.
void playEvents(App& app) {
    const bool inDuel = app.screen == Screen::Duel;
    // While hidden the events are still drained, so waking up doesn't play a backlog all at once.
    if (Game* game = shownGame(app))
        for (const GameEvent& event : game->takeEvents()) {
            if (event.type == GameEventType::SpecialClear) app.callout.show(event.award);
            if (event.type == GameEventType::LinesCleared || event.type == GameEventType::BombExploded)
                app.lastClearRow = middleRow(game->clearingRows());
            if (const auto sound = soundFor(event, inDuel); sound && !app.hidden) app.audio.play(*sound);
        }
    for (DuelEvent event : app.duel.takeEvents()) {
        if (event == DuelEvent::Sent) app.trails.launch(true, app.lastClearRow);
        if (event == DuelEvent::Attacked) app.trails.launch(false, stackTopRow(app.duel.opponent()));
        if (const auto sound = soundFor(event); sound && !app.hidden) app.audio.play(*sound);
    }
}

// What the music should be doing for the current screen.
MusicCue musicCue(App& app) {
    if (app.screen == Screen::Playing && app.game) return soloMusic(*app.game);
    if (app.screen == Screen::Duel) return duelMusic(app.duel.phase());
    return {};
}

// The key pressed while a Controls slot waits for one: assigns it, clears the slot or cancels.
void captureKey(App& app, SDL_Scancode scancode) {
    const auto [action, slot] = *app.controls.capturing;
    app.controls.capturing.reset();
    if (scancode == SDL_SCANCODE_ESCAPE) return;
    if (scancode == SDL_SCANCODE_BACKSPACE) {
        app.keymap.clear(action, slot);
    } else if (isReservedKey(scancode)) {
        app.controls.note = std::string(keyName(scancode)) + " is reserved";
        return;
    } else if (const std::optional<Key> takenFrom = app.keymap.bind(action, slot, scancode)) {
        app.controls.note = std::string(keyName(scancode)) + " removed from " + actionName(*takenFrom);
    }
    app.saveSettings();
}

void handleKey(App& app, SDL_Window* window, const SDL_KeyboardEvent& key) {
    if (key.down && isFullscreenChord(key)) {
        const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(window, !fullscreen);
        return;
    }
    if (app.controls.capturing) {
        if (key.down && !key.repeat) captureKey(app, key.scancode);
        return;
    }
    // M mutes on any screen, but not while typing a room code.
    if (key.down && !key.repeat && key.scancode == SDL_SCANCODE_M && !ImGui::GetIO().WantTextInput) {
        app.toggleSound();
        return;
    }
    if (key.repeat || app.activeGame() == nullptr) return;  // DAS/ARR replaces OS key repeat
    const std::optional<Key> mapped = app.keymap.lookup(key.scancode);
    if (!mapped) return;
    if (key.down)
        app.input.press(*mapped, app.pending);
    else
        app.input.release(*mapped);
}

}  // namespace

App::App()
    : duel([this](const std::string& text) {
          if (connection) connection->send(text);
      }) {
    menu.duelsEnabled = duelsSupported();
    const Settings saved = parseSettings(loadSettingsText());
    keymap = saved.keys;
    handling = saved.handling;
    bests = saved.bests;
    input.setHandling(handling);
    menu.muted = saved.muted;
    menu.musicOff = saved.musicOff;
    menu.publicRooms = saved.publicRooms;
    menu.playerName = saved.playerName;
    if (menu.playerName.empty()) {
        menu.playerName = defaultPlayerName(static_cast<std::uint32_t>(SDL_GetTicksNS()));
        saveSettings();  // keep the generated name across visits
    }
    std::snprintf(menu.nameEdit, sizeof menu.nameEdit, "%s", menu.playerName.c_str());
    std::string invite = takeInviteCode();
    std::transform(invite.begin(), invite.end(), invite.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (isRoomCode(invite)) menu.inviteCode = invite;
    menu.inviteLinkBase = inviteLinkBase();
    menu.canCopyLink = canCopyToClipboard();
    audio.setMuted(menu.muted);
    audio.setMusicMuted(menu.musicOff);
}

void App::saveSettings() {
    saveSettingsText(serializeSettings(
        {keymap, menu.muted, menu.musicOff, menu.playerName, menu.publicRooms, handling, bests}));
}

void App::newGame() {
    syncMusic({});  // a new game always starts the song from the top
    menu.startLevel = clampStartLevel(menu.startLevel);
    game.emplace(static_cast<std::uint32_t>(SDL_GetTicksNS()), menu.startLevel);
    input.releaseAll();
    pending.clear();
    combo = {};
    callout = {};
    screen = Screen::Playing;
}

void App::openDuel() {
    if (!connection) {  // the lobby keeps its connection when it opens a room
        connection = connectToServer();
        if (connection) connection->send(encode(Hello{menu.playerName}));  // queued until the socket opens
    }
    menu.error.clear();
    screen = Screen::Duel;
}

void App::toggleSound() {
    menu.muted = !menu.muted;
    audio.setMuted(menu.muted);
    saveSettings();
}

void App::toggleMusic() {
    menu.musicOff = !menu.musicOff;
    audio.setMusicMuted(menu.musicOff);
    saveSettings();
}

void App::syncMusic(const MusicCue& cue) {
    if (cue.mode == MusicMode::Stopped) {
        if (music != MusicMode::Stopped) audio.stopMusic();
    } else {
        audio.setMusicTempo(cue.bpm);  // before start, so the first note is already at this tempo
        if (music == MusicMode::Stopped) audio.startMusic();
        audio.setMusicPaused(cue.mode == MusicMode::Paused);
    }
    music = cue.mode;
}

void App::closeDuel() {
    duel.leave();
    connection.reset();
    screen = Screen::Start;
}

Game* App::activeGame() {
    if (screen == Screen::Playing) return game ? &*game : nullptr;
    if (screen == Screen::Duel && duel.phase() == DuelPhase::Playing && !duel.frozen()) return duel.game();
    return nullptr;
}

void handleEvent(App& app, SDL_Window* window, const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
        app.quit = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (app.game) app.game->pause();
        app.input.releaseAll();  // key-up events are lost while unfocused
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        handleKey(app, window, event.key);
        break;
    default:
        break;
    }
}

void setHidden(App& app, bool hidden) {
    if (hidden == app.hidden) return;
    app.hidden = hidden;
    if (hidden) app.input.releaseAll();  // key-up events are lost while hidden
}

void pumpNetwork(App& app) {
    if (!app.connection) return;
    for (const std::string& text : app.connection->receive()) app.duel.onMessage(text);
    if (app.connection->closed()) app.duel.onDisconnected();
    if (app.duel.phase() == DuelPhase::Idle) {  // refused by the server, or the link dropped
        app.menu.error = app.duel.error();
        app.connection.reset();
        app.screen = Screen::Start;
    }
}

// Advances input auto-repeat and the game in 1 ms steps.
void step(App& app, int ms) {
    Game* game = app.activeGame();
    if (game == nullptr) {
        app.input.releaseAll();  // keys held across a countdown or result must not auto-repeat into a match
        app.pending.clear();
    } else {
        auto flush = [&] {
            for (Action action : app.pending) game->apply(action);
            app.pending.clear();
        };
        flush();
        for (int i = 0; i < ms; ++i) {
            app.input.tick(app.pending);
            flush();
            game->setSoftDrop(app.input.softDrop());
            game->setSoftDropFactor(app.handling.softDropFactor);
            game->tick(1);
        }
    }
    if (app.screen == Screen::Duel) app.duel.update(ms);
    playEvents(app);
    const Game* shown = shownGame(app);
    app.combo.update(shown != nullptr ? shown->combo() : -1, ms);
    app.callout.update(ms);
    app.trails.update(ms);
    app.syncMusic(musicCue(app));
}

void drawFrame(App& app, const Fonts& fonts) {
    UiCommand command = UiCommand::None;
    switch (app.screen) {
    case Screen::Start:
        command = drawStartScreen(fonts, app.menu);
        break;
    case Screen::Playing:
        drawGame(*app.game, fonts, app.combo, app.callout);
        command = drawOverlay(*app.game, fonts, app.menu.muted, app.menu.musicOff);
        break;
    case Screen::Duel:
        command = drawDuel(app.duel, fonts, app.combo, app.callout, app.trails, app.menu);
        break;
    case Screen::Controls:
        command = drawControlsScreen(fonts, app.keymap, app.handling, app.controls);
        break;
    }
    if (app.menu.muted) drawMutedBadge(fonts);
    applyCommand(app, command);
}

}  // namespace tetris
