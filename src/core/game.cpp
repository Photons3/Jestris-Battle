#include "core/game.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <utility>

namespace tetris {

double gravityMsPerRow(int level) {
    const int l = std::min(level, kGravityLevelCap);
    return std::pow(0.8 - (l - 1) * 0.007, l - 1) * 1000.0;
}

int lineClearScore(int lines, int level) {
    static constexpr std::array<int, 5> kBase{0, 100, 300, 500, 800};
    assert(lines >= 0 && lines <= 4);
    return kBase[lines] * level;
}

int levelFor(int startLevel, int lines) { return startLevel + lines / kLinesPerLevel; }

int clampStartLevel(int level) { return std::clamp(level, kMinStartLevel, kMaxStartLevel); }

namespace {
constexpr std::uint32_t kGarbageSeedSalt = 0x9E3779B9u;  // keeps bomb columns independent of the bag
constexpr int kMaxScoredLines = 4;  // bombs can clear more rows at once; attack and score stop at a tetris
}  // namespace

int attackFor(int lines) {
    static constexpr std::array<int, 5> kAttack{0, 0, 1, 2, 4};
    assert(lines >= 0 && lines <= 4);
    return kAttack[lines];
}

int comboBonus(int combo) {
    if (combo >= 5) return 3;
    if (combo >= 3) return 2;
    return combo >= 1 ? 1 : 0;
}

Game::Game(std::uint32_t seed, int startLevel, Board board, GameOptions options)
    : board_(std::move(board)),
      bag_(seed),
      options_(options),
      garbageRng_(seed ^ kGarbageSeedSalt),
      startLevel_(startLevel),
      level_(startLevel) {
    assert(startLevel >= 1);
    spawn(bag_.next());
}

void Game::receiveGarbage(int lines) {
    assert(lines >= 0);
    pendingGarbage_ += lines;
}

int Game::takeOutgoingAttack() { return std::exchange(outgoingAttack_, 0); }

void Game::resetBoard() {
    board_ = Board{};
    pendingGarbage_ = 0;
    hold_.reset();
    canHold_ = true;
    combo_ = -1;
    clearingRows_.clear();
    clearTimerMs_ = 0;
    state_ = GameState::Playing;
    spawn(bag_.next());
}

bool Game::applyGarbage() {
    if (pendingGarbage_ == 0) return true;
    const int bombX = static_cast<int>(garbageRng_() % kBoardWidth);
    if (!board_.pushGarbage(std::exchange(pendingGarbage_, 0), bombX)) return false;
    emit(GameEventType::GarbageRose);
    return true;
}

void Game::apply(Action action) {
    if (action == Action::Pause) {
        if (state_ == GameState::Paused)
            resume();
        else
            pause();
        return;
    }
    if (state_ != GameState::Playing) return;
    switch (action) {
    case Action::Left:
        if (tryMove(-1, 0)) {
            onPlayerMoved();
            emit(GameEventType::Moved);
        }
        break;
    case Action::Right:
        if (tryMove(1, 0)) {
            onPlayerMoved();
            emit(GameEventType::Moved);
        }
        break;
    case Action::RotateCW:
        rotate(RotateDir::CW);
        break;
    case Action::RotateCCW:
        rotate(RotateDir::CCW);
        break;
    case Action::HardDrop:
        hardDrop();
        break;
    case Action::Hold:
        holdPiece();
        break;
    case Action::Pause:
        break;
    }
}

void Game::tick(int ms) {
    for (int i = 0; i < ms; ++i) step();
}

void Game::pause() {
    if (!options_.allowPause) return;
    if (state_ != GameState::Playing && state_ != GameState::Clearing) return;
    resumeState_ = state_;
    state_ = GameState::Paused;
}

void Game::resume() {
    if (state_ == GameState::Paused) state_ = resumeState_;
}

int Game::ghostDistance() const {
    Piece probe = active_;
    int distance = 0;
    for (;;) {
        --probe.pos.y;
        if (board_.collides(probe)) return distance;
        ++distance;
    }
}

void Game::step() {
    if (state_ == GameState::Clearing) {
        if (++clearTimerMs_ >= kClearDelayMs) finishClear();
        return;
    }
    if (state_ != GameState::Playing) return;
    if (onGround()) {
        dropTimerMs_ = 0.0;
        if (++lockTimerMs_ >= kLockDelayMs) lockPiece();
        return;
    }
    dropTimerMs_ += softDrop_ ? kSoftDropFactor : 1;
    const double msPerRow = gravityMsPerRow(level_);
    while (dropTimerMs_ >= msPerRow) {
        if (!tryMove(0, -1)) {
            dropTimerMs_ = 0.0;
            break;
        }
        dropTimerMs_ -= msPerRow;
        if (softDrop_) ++score_;
    }
}

void Game::spawn(PieceType type) {
    active_ = spawnPiece(type);
    lowestY_ = active_.pos.y;
    lockTimerMs_ = 0;
    lockResets_ = 0;
    dropTimerMs_ = 0.0;
    if (board_.collides(active_)) topOut();  // block-out
}

void Game::topOut() {
    state_ = GameState::GameOver;
    emit(GameEventType::ToppedOut);
}

bool Game::tryMove(int dx, int dy) {
    Piece moved = active_;
    moved.pos.x += dx;
    moved.pos.y += dy;
    if (board_.collides(moved)) return false;
    active_ = moved;
    updateLowest();
    return true;
}

void Game::rotate(RotateDir dir) {
    if (const auto rotated = rotateWithKicks(board_, active_, dir)) {
        active_ = *rotated;
        updateLowest();
        onPlayerMoved();
        emit(GameEventType::Rotated);
    }
}

void Game::updateLowest() {
    if (active_.pos.y < lowestY_) {
        lowestY_ = active_.pos.y;
        lockResets_ = 0;
        lockTimerMs_ = 0;
    }
}

void Game::onPlayerMoved() {
    if (lockTimerMs_ > 0 && lockResets_ < kMaxLockResets) {
        lockTimerMs_ = 0;
        ++lockResets_;
    }
}

bool Game::onGround() const {
    Piece below = active_;
    --below.pos.y;
    return board_.collides(below);
}

void Game::hardDrop() {
    const int distance = ghostDistance();
    active_.pos.y -= distance;
    score_ += 2 * distance;
    emit(GameEventType::HardDropped);
    lockPiece(true);
}

void Game::holdPiece() {
    if (!canHold_) return;
    emit(GameEventType::Held);
    const PieceType current = active_.type;
    spawn(hold_ ? *hold_ : bag_.next());
    hold_ = current;
    canHold_ = false;
}

void Game::lockPiece(bool hardDropped) {
    board_.lock(active_);
    canHold_ = true;
    const auto cells = active_.cells();
    const bool lockOut =
        std::all_of(cells.begin(), cells.end(), [](Point c) { return c.y >= kVisibleHeight; });
    if (lockOut) {
        topOut();
        return;
    }
    const std::vector<int> full = board_.fullRows();
    const std::vector<int> exploded = board_.bombRowsHitBy(active_);  // bomb rows are never full, so no overlap
    combo_ = full.empty() && exploded.empty() ? -1 : combo_ + 1;
    if (!full.empty()) emit(GameEventType::LinesCleared, static_cast<int>(full.size()));
    if (!exploded.empty()) emit(GameEventType::BombExploded);
    if (combo_ >= 1) emit(GameEventType::Combo, combo_);
    clearingRows_ = full;
    clearingRows_.insert(clearingRows_.end(), exploded.begin(), exploded.end());
    std::sort(clearingRows_.begin(), clearingRows_.end());
    if (!clearingRows_.empty()) {
        state_ = GameState::Clearing;
        clearTimerMs_ = 0;
        return;
    }
    if (!hardDropped) emit(GameEventType::Locked);
    if (!applyGarbage()) {
        topOut();
        return;
    }
    spawn(bag_.next());
}

void Game::finishClear() {
    const int levelBefore = level_;
    const int cleared = static_cast<int>(clearingRows_.size());
    const int counted = std::min(cleared, kMaxScoredLines);
    const int attack = attackFor(counted) + comboBonus(combo_);
    const int cancelled = std::min(attack, pendingGarbage_);
    pendingGarbage_ -= cancelled;
    outgoingAttack_ += attack - cancelled;
    score_ += lineClearScore(counted, level_);
    lines_ += cleared;
    level_ = levelFor(startLevel_, lines_);
    if (level_ > levelBefore) emit(GameEventType::LevelUp);
    board_.removeRows(clearingRows_);
    clearingRows_.clear();
    state_ = GameState::Playing;
    spawn(bag_.next());
}

}  // namespace tetris
