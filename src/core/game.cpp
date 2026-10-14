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

int levelFor(int startLevel, int lines, int linesPerLevel) { return startLevel + lines / linesPerLevel; }

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
    static constexpr std::array<int, 11> kBonus{0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5};  // combo 10 and up send 5
    if (combo <= 0) return 0;
    return kBonus[std::min(combo, static_cast<int>(kBonus.size()) - 1)];
}

bool isDifficult(const ClearAward& award) {
    return award.lines > 0 && (award.lines == 4 || award.spin != Spin::None);
}

int countedLines(const ClearAward& award) { return std::min(award.lines + award.bombRows, kMaxScoredLines); }

int awardScore(const ClearAward& award, int combo, int level) {
    static constexpr std::array<int, 5> kMini{100, 200, 400, 500, 800};
    static constexpr std::array<int, 5> kFull{400, 800, 1200, 1600, 1600};
    static constexpr std::array<int, 5> kPerfect{0, 800, 1200, 1800, 2000};
    const int lines = countedLines(award);
    int points = award.spin == Spin::Full ? kFull[lines] : award.spin == Spin::Mini ? kMini[lines] : lineClearScore(lines, 1);
    if (award.backToBack > 0) points = points * 3 / 2;
    if (lines > 0 && combo >= 1) points += 50 * combo;
    if (award.perfectClear) points += lines == 4 && award.backToBack > 0 ? 3200 : kPerfect[lines];
    return points * level;
}

int awardAttack(const ClearAward& award, int combo) {
    static constexpr std::array<int, 5> kFull{0, 2, 4, 6, 6};
    const int lines = countedLines(award);
    if (lines == 0) return 0;
    int attack = award.spin == Spin::Full ? kFull[lines] : attackFor(lines);  // a mini sends as plain
    if (award.backToBack > 0) ++attack;
    attack += comboBonus(combo);
    if (award.perfectClear) attack += 10;
    return attack;
}

Spin tSpinFor(const Board& board, const Piece& piece, int kick) {
    if (piece.type != PieceType::T) return Spin::None;
    const int cx = piece.pos.x + 1;  // the T's center is the middle of its 3x3 box
    const int cy = piece.pos.y - 1;
    auto filled = [&](int dx, int dy) {
        const int x = cx + dx;
        const int y = cy + dy;
        return !Board::inBounds(x, y) || board.at(x, y).has_value();
    };
    const bool upLeft = filled(-1, 1), upRight = filled(1, 1), downLeft = filled(-1, -1), downRight = filled(1, -1);
    if (upLeft + upRight + downLeft + downRight < 3) return Spin::None;
    bool front = false;  // both corners on the side the T points to
    switch (piece.rotation) {
    case Rotation::Zero:
        front = upLeft && upRight;
        break;
    case Rotation::R:
        front = upRight && downRight;
        break;
    case Rotation::Two:
        front = downLeft && downRight;
        break;
    case Rotation::L:
        front = upLeft && downLeft;
        break;
    }
    return front || kick == 4 ? Spin::Full : Spin::Mini;
}

std::vector<int> bombColumnsFor(int rows, int aboveX, int aboveRun, std::mt19937& rng) {
    std::vector<int> columns(static_cast<std::size_t>(std::max(rows, 0)));
    int previous = aboveX;
    int run = aboveX < 0 ? 0 : aboveRun;
    for (auto it = columns.rbegin(); it != columns.rend(); ++it) {  // top row first, under the existing stack
        const bool pair = previous >= 0 && run == 1 && static_cast<int>(rng() % 100) < kBombPairPercent;
        if (pair) {
            ++run;
        } else if (previous >= 0) {  // any other column
            const int x = static_cast<int>(rng() % (kBoardWidth - 1));
            previous = x >= previous ? x + 1 : x;
            run = 1;
        } else {
            previous = static_cast<int>(rng() % kBoardWidth);
            run = 1;
        }
        *it = previous;
    }
    return columns;
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

void Game::receiveGarbage(int lines, int travelMs) {
    assert(lines >= 0 && travelMs >= 0);
    if (lines > 0) incoming_.push_back({lines, travelMs});
}

int Game::pendingGarbage() const {
    int total = 0;
    for (const GarbageBatch& batch : incoming_) total += batch.lines;
    return total;
}

int Game::readyGarbage() const {
    int total = 0;
    for (const GarbageBatch& batch : incoming_)
        if (batch.msLeft == 0) total += batch.lines;
    return total;
}

int Game::takeOutgoingAttack() { return std::exchange(outgoingAttack_, 0); }

void Game::resetBoard() {
    board_ = Board{};
    incoming_.clear();
    hold_.reset();
    canHold_ = true;
    combo_ = -1;
    backToBack_ = -1;
    clearingRows_.clear();
    clearTimerMs_ = 0;
    state_ = GameState::Playing;
    spawn(bag_.next());
}

bool Game::applyGarbage() {
    int rising = 0;
    while (!incoming_.empty() && incoming_.front().msLeft == 0 && rising < kMaxGarbagePerLock) {
        GarbageBatch& batch = incoming_.front();
        const int take = std::min(batch.lines, kMaxGarbagePerLock - rising);
        rising += take;
        batch.lines -= take;
        if (batch.lines == 0) incoming_.pop_front();
    }
    if (rising == 0) return true;
    int aboveX = -1;
    for (int x = 0; x < kBoardWidth; ++x)
        if (board_.at(x, 0) == PieceType::Bomb) aboveX = x;
    int aboveRun = 0;
    while (aboveX >= 0 && aboveRun < kBoardHeight && board_.at(aboveX, aboveRun) == PieceType::Bomb) ++aboveRun;
    const std::vector<int> columns = bombColumnsFor(rising, aboveX, aboveRun, garbageRng_);
    if (!board_.pushGarbage(columns)) return false;
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
    case Action::LeftWall:
    case Action::RightWall:
        shiftToWall(action == Action::LeftWall ? -1 : 1);
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
    if (state_ == GameState::Playing || state_ == GameState::Clearing)
        for (GarbageBatch& batch : incoming_) batch.msLeft = std::max(batch.msLeft - 1, 0);
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
    if (softDrop_ && softDropFactor_ == kSoftDropInstant) {
        const int rows = ghostDistance();  // more than 0: the piece is not on the ground
        active_.pos.y -= rows;
        rotatedLast_ = false;
        score_ += rows;
        updateLowest();
        dropTimerMs_ = 0.0;
        return;
    }
    dropTimerMs_ += softDrop_ ? softDropFactor_ : 1;
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
    rotatedLast_ = false;
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
    rotatedLast_ = false;
    updateLowest();
    return true;
}

// One lock reset and one Moved event however far it goes, like a single move; nothing if it is already blocked.
void Game::shiftToWall(int dx) {
    bool moved = false;
    while (tryMove(dx, 0)) moved = true;
    if (!moved) return;
    onPlayerMoved();
    emit(GameEventType::Moved);
}

void Game::rotate(RotateDir dir) {
    if (const auto rotated = rotateWithKicks(board_, active_, dir, &lastKick_)) {
        active_ = *rotated;
        rotatedLast_ = true;
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
    if (distance > 0) rotatedLast_ = false;
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
    const Spin spin = rotatedLast_ ? tSpinFor(board_, active_, lastKick_) : Spin::None;
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
    clearingRows_ = full;
    clearingRows_.insert(clearingRows_.end(), exploded.begin(), exploded.end());
    std::sort(clearingRows_.begin(), clearingRows_.end());
    award_ = {static_cast<int>(full.size()), spin};
    award_.bombRows = static_cast<int>(exploded.size());
    if (award_.lines > 0) {  // only full rows move the back-to-back chain
        backToBack_ = isDifficult(award_) ? backToBack_ + 1 : -1;
        award_.backToBack = isDifficult(award_) ? std::max(backToBack_, 0) : 0;
    }
    if (!clearingRows_.empty()) award_.perfectClear = clearEmptiesBoard();
    ++stats_.pieces;
    if (award_.lines == 4) ++stats_.tetrises;
    if (award_.spin != Spin::None) ++stats_.tSpins;
    stats_.maxCombo = std::max(stats_.maxCombo, combo_);
    if (!full.empty()) emit(GameEventType::LinesCleared, static_cast<int>(full.size()));
    if (!exploded.empty()) emit(GameEventType::BombExploded);
    if (combo_ >= 1) emit(GameEventType::Combo, combo_);
    if (award_.spin != Spin::None || award_.lines == 4 || award_.perfectClear)
        events_.push_back({GameEventType::SpecialClear, 0, award_});
    if (!clearingRows_.empty()) {
        state_ = GameState::Clearing;
        clearTimerMs_ = 0;
        return;
    }
    score_ += awardScore(award_, combo_, level_);  // a T-spin that clears nothing still scores
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
    const int attack = awardAttack(award_, combo_);
    const int cancelled = std::min(attack, pendingGarbage());
    for (int left = cancelled; left > 0;) {
        GarbageBatch& batch = incoming_.front();
        const int take = std::min(batch.lines, left);
        batch.lines -= take;
        left -= take;
        if (batch.lines == 0) incoming_.pop_front();
    }
    outgoingAttack_ += attack - cancelled;
    stats_.attack += attack;
    stats_.cancelled += cancelled;
    score_ += awardScore(award_, combo_, level_);
    lines_ += cleared;
    level_ = levelFor(startLevel_, lines_, options_.linesPerLevel);
    if (level_ > levelBefore) emit(GameEventType::LevelUp);
    for (int y : clearingRows_) {
        for (int x = 0; x < kBoardWidth; ++x) {
            const auto cell = board_.at(x, y);
            if (cell == PieceType::Garbage || cell == PieceType::Bomb) {
                ++stats_.garbageCleared;
                break;
            }
        }
    }
    board_.removeRows(clearingRows_);
    clearingRows_.clear();
    state_ = GameState::Playing;
    spawn(bag_.next());
}

bool Game::clearEmptiesBoard() const {
    for (int y = 0; y < kBoardHeight; ++y) {
        if (std::binary_search(clearingRows_.begin(), clearingRows_.end(), y)) continue;
        for (int x = 0; x < kBoardWidth; ++x)
            if (board_.at(x, y)) return false;
    }
    return true;
}

}  // namespace tetris
