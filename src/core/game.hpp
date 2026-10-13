#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "core/bag.hpp"
#include "core/board.hpp"
#include "core/piece.hpp"
#include "core/srs.hpp"

namespace tetris {

enum class GameState { Playing, Clearing, Paused, GameOver };
enum class Action { Left, Right, RotateCW, RotateCCW, HardDrop, Hold, Pause };
enum class GameEventType { Moved, Rotated, Held, HardDropped, Locked, LinesCleared, BombExploded, GarbageRose, LevelUp, ToppedOut, Combo };

// Something that just happened in a game, for sound. Locked is only for a piece that locks on its own.
struct GameEvent {
    GameEventType type;
    int value = 0;  // LinesCleared: full rows, 1-4 (bomb rows are reported as BombExploded). Combo: the combo, 1+
    friend bool operator==(const GameEvent&, const GameEvent&) = default;
};

inline constexpr int kLockDelayMs = 500;
inline constexpr int kMaxLockResets = 15;
inline constexpr int kClearDelayMs = 300;
inline constexpr int kSoftDropFactor = 20;
inline constexpr int kGravityLevelCap = 20;
inline constexpr int kLinesPerLevel = 10;
inline constexpr int kMinStartLevel = 1;
inline constexpr int kMaxStartLevel = 15;

// Milliseconds per row of gravity. Levels above 20 use the level-20 speed.
double gravityMsPerRow(int level);
// Points for clearing `lines` (1–4) rows at `level`.
int lineClearScore(int lines, int level);
int levelFor(int startLevel, int lines);
// Clamps a requested start level into the selectable range.
int clampStartLevel(int level);

// Garbage lines sent to the opponent for clearing `lines` (1–4) rows at once.
int attackFor(int lines);
// Extra garbage for clearing during a combo; `combo` is Game::combo() at the clearing lock.
int comboBonus(int combo);

struct GameOptions {
    bool allowPause = true;  // duels turn pausing off so one player can't freeze the match
};

class Game {
public:
    Game(std::uint32_t seed, int startLevel, Board board = {}, GameOptions options = {});

    // Queues garbage from the opponent. It rises when a piece next locks without clearing lines.
    void receiveGarbage(int lines);
    int pendingGarbage() const { return pendingGarbage_; }
    // Garbage this game has sent since the last call; resets the count to zero.
    int takeOutgoingAttack();
    // Events since the last call, oldest first; empties the list. resetBoard() leaves it alone.
    std::vector<GameEvent> takeEvents() { return std::exchange(events_, {}); }
    // Starts over on an empty board after a duel KO. Drops pending garbage, the hold and any clear in
    // progress, then spawns the next piece from the same bag. Score, lines and level carry on.
    void resetBoard();

    void apply(Action action);
    void setSoftDrop(bool on) { softDrop_ = on; }
    void tick(int ms);  // advances in 1 ms steps
    void pause();       // no-op unless Playing or Clearing
    void resume();      // no-op unless Paused

    const Board& board() const { return board_; }
    const Piece& active() const { return active_; }
    int ghostDistance() const;  // rows the active piece can still fall
    std::optional<PieceType> hold() const { return hold_; }
    bool canHold() const { return canHold_; }
    std::vector<PieceType> next(int n) const { return bag_.peek(n); }
    const std::vector<int>& clearingRows() const { return clearingRows_; }
    int score() const { return score_; }
    int level() const { return level_; }
    int lines() const { return lines_; }
    // Clearing locks in a row, minus one: -1 with no combo, 0 after the first clear, 1 after the second...
    int combo() const { return combo_; }
    GameState state() const { return state_; }

private:
    void step();
    void spawn(PieceType type);
    bool tryMove(int dx, int dy);
    void rotate(RotateDir dir);
    void updateLowest();
    void onPlayerMoved();
    bool onGround() const;
    void hardDrop();
    void holdPiece();
    void lockPiece(bool hardDropped = false);
    void finishClear();
    void emit(GameEventType type, int value = 0) { events_.push_back({type, value}); }
    void topOut();
    bool applyGarbage();  // false if the garbage pushed blocks off the top

    Board board_;
    Bag bag_;
    GameOptions options_;
    std::mt19937 garbageRng_;  // picks bomb columns; need not match the opponent's
    Piece active_{};
    std::optional<PieceType> hold_;
    bool canHold_ = true;
    bool softDrop_ = false;
    GameState state_ = GameState::Playing;
    GameState resumeState_ = GameState::Playing;
    std::vector<int> clearingRows_;
    double dropTimerMs_ = 0.0;
    int lockTimerMs_ = 0;
    int lockResets_ = 0;
    int lowestY_ = 0;
    int clearTimerMs_ = 0;
    int startLevel_;
    int level_;
    int lines_ = 0;
    int score_ = 0;
    int pendingGarbage_ = 0;
    int outgoingAttack_ = 0;
    int combo_ = -1;
    std::vector<GameEvent> events_;
};

}  // namespace tetris
