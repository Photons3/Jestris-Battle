#include "bot/planner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <tuple>

#include "core/srs.hpp"

namespace tetris {

namespace {

struct Rotations {
    std::vector<Action> actions;
    std::vector<RotateDir> dirs;
};

const std::array<Rotations, 4>& rotationPrefixes() {
    static const std::array<Rotations, 4> prefixes{{
        {{}, {}},
        {{Action::RotateCW}, {RotateDir::CW}},
        {{Action::RotateCW, Action::RotateCW}, {RotateDir::CW, RotateDir::CW}},
        {{Action::RotateCCW}, {RotateDir::CCW}},
    }};
    return prefixes;
}

// Adds every landing spot reachable from `start`; `lead` is prepended to each action list (e.g. Hold).
void addCandidates(const Board& board, const Piece& start, const std::vector<Action>& lead,
                   std::vector<Placement>& out) {
    const auto firstOwn = static_cast<std::ptrdiff_t>(out.size());  // dedupe within this candidate set only
    for (const Rotations& rot : rotationPrefixes()) {
        Piece rotated = start;
        bool ok = true;
        for (RotateDir dir : rot.dirs) {
            const std::optional<Piece> next = rotateWithKicks(board, rotated, dir);
            if (!next) { ok = false; break; }
            rotated = *next;
        }
        if (!ok) continue;

        for (int shift = -(kBoardWidth - 1); shift <= kBoardWidth - 1; ++shift) {
            Piece moved = rotated;
            const int step = shift < 0 ? -1 : 1;
            bool reached = true;
            for (int i = 0; i < std::abs(shift); ++i) {
                Piece trial = moved;
                trial.pos.x += step;
                if (board.collides(trial)) { reached = false; break; }
                moved = trial;
            }
            if (!reached) continue;

            Piece landed = moved;
            for (;;) {
                Piece below = landed;
                --below.pos.y;
                if (board.collides(below)) break;
                landed = below;
            }

            const auto key = [](const Piece& p) {
                const auto c = p.cells();
                std::array<std::tuple<int, int>, 4> k{};
                for (int i = 0; i < 4; ++i) k[i] = {c[i].x, c[i].y};
                std::sort(k.begin(), k.end());
                return k;
            };
            const auto landedKey = key(landed);
            const bool seen = std::any_of(out.begin() + firstOwn, out.end(), [&](const Placement& p) {
                return key(p.landed) == landedKey;
            });
            if (seen) continue;

            Placement placement;
            placement.actions = lead;
            placement.actions.insert(placement.actions.end(), rot.actions.begin(), rot.actions.end());
            const Action side = shift < 0 ? Action::Left : Action::Right;
            placement.actions.insert(placement.actions.end(), static_cast<std::size_t>(std::abs(shift)), side);
            placement.actions.push_back(Action::HardDrop);
            placement.landed = landed;
            placement.score = evaluateBoard(board, landed);
            out.push_back(std::move(placement));
        }
    }
}

}  // namespace

double evaluateBoard(const Board& board, const Piece& piece) {
    Board after = board;
    after.lock(piece);
    std::vector<int> rows = after.fullRows();
    const int lines = static_cast<int>(rows.size());
    const std::vector<int> bombs = after.bombRowsHitBy(piece);
    rows.insert(rows.end(), bombs.begin(), bombs.end());
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    after.removeRows(rows);

    std::array<int, kBoardWidth> heights{};
    int holes = 0;
    for (int x = 0; x < kBoardWidth; ++x) {
        int top = 0;
        for (int y = kBoardHeight - 1; y >= 0; --y)
            if (after.at(x, y)) { top = y + 1; break; }
        heights[x] = top;
        for (int y = 0; y < top; ++y)
            if (!after.at(x, y)) ++holes;
    }
    int totalHeight = 0, maxHeight = 0, bumpiness = 0;
    for (int x = 0; x < kBoardWidth; ++x) {
        totalHeight += heights[x];
        maxHeight = std::max(maxHeight, heights[x]);
        if (x + 1 < kBoardWidth) bumpiness += std::abs(heights[x] - heights[x + 1]);
    }

    return kHeightWeight * totalHeight + kHolesWeight * holes + kBumpinessWeight * bumpiness +
           kLinesWeight * lines + kBombBonus * static_cast<double>(bombs.size()) -
           kDangerPenalty * std::max(0, maxHeight - kDangerHeight);
}

std::vector<Placement> rankPlacements(const Game& game) {
    std::vector<Placement> out;
    addCandidates(game.board(), game.active(), {}, out);
    if (game.canHold()) {
        const PieceType type = game.hold() ? *game.hold() : game.next(1)[0];
        const Piece spawned = spawnPiece(type);
        if (!game.board().collides(spawned)) addCandidates(game.board(), spawned, {Action::Hold}, out);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Placement& a, const Placement& b) { return a.score > b.score; });
    return out;
}

}  // namespace tetris
