// src/app/app.hpp
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/audio_out.hpp"
#include "app/duel.hpp"
#include "app/fonts.hpp"
#include "app/input.hpp"
#include "app/keymap.hpp"
#include "app/render.hpp"
#include "app/solo.hpp"
#include "app/sound_cues.hpp"
#include "core/game.hpp"
#include "net/connection.hpp"

namespace tetris {

// Everything between platform events and pixels: menus, key mapping and stepping the game.
struct App {
    App();
    App(const App&) = delete;  // `duel` sends through a lambda that captures `this`
    App& operator=(const App&) = delete;

    Screen screen = Screen::Start;
    StartMenu menu;
    ControlsMenu controls;
    Keymap keymap = defaultKeymap();
    Handling handling;  // DAS, ARR and soft-drop speed, as last saved
    PersonalBests bests;  // solo bests, as last saved; kept so saving the settings doesn't drop them
    std::optional<Game> game;  // solo game
    std::unique_ptr<Connection> connection;
    DuelSession duel;
    InputMapper input;
    AudioOut audio;
    MusicMode music = MusicMode::Stopped;  // what the music is doing, as last applied
    ComboLabel combo;
    ClearCallout callout;
    AttackTrails trails;      // duel attacks in flight
    float lastClearRow = 0;  // middle of our last clear, where the attack it sends takes off
    std::vector<Action> pending;
    bool quit = false;
    bool hidden = false;  // tab hidden or window minimized: keep stepping, skip drawing and sound

    void newGame();
    void openDuel();   // connects (once) and shows the duel screens
    void closeDuel();  // leaves the room, disconnects and returns to the start screen
    void toggleMusic();                   // flips the music on or off and saves it
    void syncMusic(const MusicCue& cue);  // starts, pauses, retimes or stops the music to match `cue`
    void toggleSound();  // flips mute and saves it
    void saveSettings();  // stores the keymap, handling, mute and music for the next launch
    Game* activeGame();  // the game the keys control: solo, or the duel game while a match is running
};

void handleEvent(App& app, SDL_Window* window, const SDL_Event& event);
void setHidden(App& app, bool hidden);  // call when the tab or window is hidden or shown again
void pumpNetwork(App& app);  // feeds received messages to the duel; once per frame, before step()
void step(App& app, int ms);                   // advances input auto-repeat and the game in 1 ms steps
void drawFrame(App& app, const Fonts& fonts);  // builds the ImGui frame and applies the chosen menu command

}  // namespace tetris
