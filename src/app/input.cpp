#include "app/input.hpp"

#include <algorithm>

namespace tetris {
namespace {

Action shiftAction(int dir) { return dir < 0 ? Action::Left : Action::Right; }

}  // namespace

Handling clampHandling(Handling handling) {
    handling.dasMs = std::clamp(handling.dasMs, kMinDasMs, kMaxDasMs);
    handling.arrMs = std::clamp(handling.arrMs, kMinArrMs, kMaxArrMs);
    if (handling.softDropFactor != kSoftDropInstant)
        handling.softDropFactor = std::clamp(handling.softDropFactor, kMinSdf, kMaxSdf);
    return handling;
}

void InputMapper::press(Key key, std::vector<Action>& out) {
    switch (key) {
    case Key::Left:
        leftHeld_ = true;
        startShift(-1, out);
        break;
    case Key::Right:
        rightHeld_ = true;
        startShift(1, out);
        break;
    case Key::SoftDrop:
        softDrop_ = true;
        break;
    case Key::HardDrop:
        out.push_back(Action::HardDrop);
        break;
    case Key::RotateCW:
        out.push_back(Action::RotateCW);
        break;
    case Key::RotateCCW:
        out.push_back(Action::RotateCCW);
        break;
    case Key::Hold:
        out.push_back(Action::Hold);
        break;
    case Key::Pause:
        out.push_back(Action::Pause);
        break;
    }
}

void InputMapper::release(Key key) {
    if (key == Key::SoftDrop) {
        softDrop_ = false;
        return;
    }
    if (key != Key::Left && key != Key::Right) return;
    const int releasedDir = key == Key::Left ? -1 : 1;
    (key == Key::Left ? leftHeld_ : rightHeld_) = false;
    if (dir_ != releasedDir) return;
    // Fall back to the other direction if it is still held; its DAS starts over.
    dir_ = leftHeld_ ? -1 : (rightHeld_ ? 1 : 0);
    dasMs_ = 0;
    arrMs_ = 0;
}

void InputMapper::tick(std::vector<Action>& out) {
    if (dir_ == 0) return;
    if (dasMs_ < handling_.dasMs) {
        if (++dasMs_ < handling_.dasMs) return;
        if (handling_.arrMs > 0) {  // the shift that ends DAS; at ARR 0 the wall shift below takes its place
            out.push_back(shiftAction(dir_));
            return;
        }
    }
    if (handling_.arrMs == 0) {  // every tick, so a new or rotated piece slides at once
        out.push_back(dir_ < 0 ? Action::LeftWall : Action::RightWall);
        return;
    }
    if (++arrMs_ >= handling_.arrMs) {
        arrMs_ = 0;
        out.push_back(shiftAction(dir_));
    }
}

void InputMapper::releaseAll() {
    const Handling handling = handling_;
    *this = InputMapper{};
    handling_ = handling;
}

void InputMapper::startShift(int dir, std::vector<Action>& out) {
    dir_ = dir;
    dasMs_ = 0;
    arrMs_ = 0;
    out.push_back(shiftAction(dir));
}

}  // namespace tetris
