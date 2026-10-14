#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/game.hpp"

namespace tetris {

enum class SoloMode { Marathon, Sprint, Blitz, Cheese, Zen };
enum class SoloPhase { Countdown, Playing, Finished, ToppedOut };
enum class SoloEvent { CountdownTick, Start, Finished };

inline constexpr int kSprintLines = 40;
inline constexpr int kBlitzMs = 120000;
inline constexpr int kCheeseRows = 10;
inline constexpr int kBlitzLinesPerLevel = 5;

struct SoloRules {
    SoloMode mode = SoloMode::Marathon;
    int goalLines = 0;    // Sprint: finish at this many lines; 0 = none
    int timeLimitMs = 0;  // Blitz: clock counts down from this; 0 = counts up (or no clock)
    bool countdown = false;
};

SoloRules rulesFor(SoloMode mode);
// Sprint and Cheese start at level 1; Cheese begins with its garbage rows; Blitz levels up every few lines.
Game makeSoloGame(SoloMode mode, std::uint32_t seed, int startLevel);
bool isTimed(SoloMode mode);  // Sprint, Blitz, Cheese: the modes with a clock

// One solo run: the optional countdown, the clock and the finish rule around a Game. The caller ticks the Game
// and routes input; call update() once per frame with the same elapsed time. Knows nothing about SDL.
class SoloSession {
public:
    SoloSession(SoloRules rules, Game game);

    void update(int ms);
    void pause();  // pauses the game only while Playing: during the countdown nothing could unpause it

    SoloMode mode() const { return rules_.mode; }
    SoloPhase phase() const { return phase_; }
    bool frozen() const { return phase_ != SoloPhase::Playing; }  // the game must not tick or take input
    int countdownMs() const { return countdownMs_; }
    int clockMs() const;      // elapsed, or remaining for a time limit; 0 for Marathon and Zen
    int progress() const;     // Sprint: lines; Cheese: garbage rows left; else 0
    std::optional<int> result() const;  // Finished only: ms (Sprint, Cheese) or score (Blitz)
    std::vector<SoloEvent> takeEvents() { return std::exchange(events_, {}); }
    Game& game() { return game_; }
    const Game& game() const { return game_; }

private:
    void finish();

    SoloRules rules_;
    Game game_;
    SoloPhase phase_ = SoloPhase::Playing;
    int countdownMs_ = 0;
    int elapsedMs_ = 0;
    std::vector<SoloEvent> events_;
};

// Best results per mode: lower time for Sprint and Cheese, higher score for Blitz.
struct PersonalBests {
    std::optional<int> sprintMs, cheeseMs, blitzScore;
    std::optional<int> best(SoloMode mode) const;  // nullopt for Marathon and Zen
    bool submit(SoloMode mode, int result);         // true if a new PB
    friend bool operator==(const PersonalBests&, const PersonalBests&) = default;
};

}  // namespace tetris
