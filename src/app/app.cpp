// src/app/app.cpp
#include "app/app.hpp"

#include <cstdint>

#include "app/keymap.hpp"
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
    case UiCommand::CreateRoom:
        app.openDuel();
        app.duel.createRoom(app.menu.startLevel);
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
    }
}

// The local game on screen: the solo game, or our duel game.
Game* shownGame(App& app) {
    if (app.screen == Screen::Duel) return app.duel.game();
    return app.game ? &*app.game : nullptr;
}

// Plays what the solo or duel game and the duel session reported this frame.
void playEvents(App& app) {
    const bool inDuel = app.screen == Screen::Duel;
    // While hidden the events are still drained, so waking up doesn't play a backlog all at once.
    if (Game* game = shownGame(app))
        for (const GameEvent& event : game->takeEvents())
            if (!app.hidden) app.audio.play(soundFor(event, inDuel));
    for (DuelEvent event : app.duel.takeEvents())
        if (!app.hidden) app.audio.play(soundFor(event));
}

// What the music should be doing for the current screen.
MusicCue musicCue(App& app) {
    if (app.screen == Screen::Playing && app.game) return soloMusic(*app.game);
    if (app.screen == Screen::Duel) return duelMusic(app.duel.phase());
    return {};
}

void handleKey(App& app, SDL_Window* window, const SDL_KeyboardEvent& key) {
    if (key.down && isFullscreenChord(key)) {
        const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(window, !fullscreen);
        return;
    }
    // M mutes on any screen, but not while typing a room code.
    if (key.down && !key.repeat && key.scancode == SDL_SCANCODE_M && !ImGui::GetIO().WantTextInput) {
        app.toggleSound();
        return;
    }
    if (key.repeat || app.activeGame() == nullptr) return;  // DAS/ARR replaces OS key repeat
    const std::optional<Key> mapped = keyForScancode(key.scancode);
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
}

void App::newGame() {
    syncMusic({});  // a new game always starts the song from the top
    menu.startLevel = clampStartLevel(menu.startLevel);
    game.emplace(static_cast<std::uint32_t>(SDL_GetTicksNS()), menu.startLevel);
    input.releaseAll();
    pending.clear();
    combo = {};
    screen = Screen::Playing;
}

void App::openDuel() {
    connection = connectToServer();
    menu.error.clear();
    screen = Screen::Duel;
}

void App::toggleSound() {
    menu.muted = !menu.muted;
    audio.setMuted(menu.muted);
}

void App::toggleMusic() {
    menu.musicOff = !menu.musicOff;
    audio.setMusicMuted(menu.musicOff);
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
            game->tick(1);
        }
    }
    if (app.screen == Screen::Duel) app.duel.update(ms);
    playEvents(app);
    const Game* shown = shownGame(app);
    app.combo.update(shown != nullptr ? shown->combo() : -1, ms);
    app.syncMusic(musicCue(app));
}

void drawFrame(App& app, const Fonts& fonts) {
    UiCommand command = UiCommand::None;
    switch (app.screen) {
    case Screen::Start:
        command = drawStartScreen(fonts, app.menu);
        break;
    case Screen::Playing:
        drawGame(*app.game, fonts, app.combo);
        command = drawOverlay(*app.game, fonts, app.menu.muted, app.menu.musicOff);
        break;
    case Screen::Duel:
        command = drawDuel(app.duel, fonts, app.combo);
        break;
    }
    if (app.menu.muted) drawMutedBadge(fonts);
    applyCommand(app, command);
}

}  // namespace tetris
