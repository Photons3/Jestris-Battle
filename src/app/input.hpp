#pragma once

#include <vector>

#include "core/game.hpp"

namespace tetris {

enum class Key { Left, Right, SoftDrop, HardDrop, RotateCW, RotateCCW, Hold, Pause };

inline constexpr int kDasMs = 167;  // delay before a held direction starts repeating
inline constexpr int kArrMs = 33;   // interval between repeats

// Turns key presses into game actions, including DAS/ARR auto-repeat. Knows nothing about SDL.
class InputMapper {
public:
    void press(Key key, std::vector<Action>& out);
    void release(Key key);
    void tick(std::vector<Action>& out);  // advances 1 ms
    void releaseAll();
    bool softDrop() const { return softDrop_; }

private:
    void startShift(int dir, std::vector<Action>& out);

    bool leftHeld_ = false;
    bool rightHeld_ = false;
    bool softDrop_ = false;
    int dir_ = 0;  // -1 left, +1 right, 0 none
    int dasMs_ = 0;
    int arrMs_ = 0;
};

}  // namespace tetris
