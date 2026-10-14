#pragma once

#include <vector>

#include "core/game.hpp"

namespace tetris {

enum class Key { Left, Right, SoftDrop, HardDrop, RotateCW, RotateCCW, Hold, Pause };

inline constexpr int kMinDasMs = 0;
inline constexpr int kMaxDasMs = 500;
inline constexpr int kMinArrMs = 0;
inline constexpr int kMaxArrMs = 100;
inline constexpr int kMinSdf = 5;
inline constexpr int kMaxSdf = 40;

// How held keys behave; the player sets these on the Controls screen.
struct Handling {
    int dasMs = 167;  // delay before a held direction starts repeating
    int arrMs = 33;   // interval between repeats; 0 shifts straight to the wall
    int softDropFactor = kDefaultSoftDropFactor;  // kSoftDropInstant, or kMinSdf to kMaxSdf
    friend bool operator==(const Handling&, const Handling&) = default;
};

// Brings each field into its range. A soft-drop factor other than kSoftDropInstant is at least kMinSdf.
Handling clampHandling(Handling handling);

// Turns key presses into game actions, including DAS/ARR auto-repeat. Knows nothing about SDL.
class InputMapper {
public:
    // Applies from the next tick; a direction already held keeps its DAS and ARR progress.
    void setHandling(const Handling& handling) { handling_ = handling; }
    const Handling& handling() const { return handling_; }
    void press(Key key, std::vector<Action>& out);
    void release(Key key);
    void tick(std::vector<Action>& out);  // advances 1 ms
    void releaseAll();  // keeps the handling
    bool softDrop() const { return softDrop_; }

private:
    void startShift(int dir, std::vector<Action>& out);

    Handling handling_;
    bool leftHeld_ = false;
    bool rightHeld_ = false;
    bool softDrop_ = false;
    int dir_ = 0;  // -1 left, +1 right, 0 none
    int dasMs_ = 0;
    int arrMs_ = 0;
};

}  // namespace tetris
