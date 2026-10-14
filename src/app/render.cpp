#include "app/render.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

namespace tetris {
namespace {

constexpr float kMargin = 16.0f;
constexpr int kDrawnRows = kVisibleHeight + 2;  // visible field plus the two spawn rows
constexpr float kLayoutColumns = 24.0f;         // 10 board columns + side panels
constexpr float kHiddenAlpha = 0.5f;

struct Rgb {
    int r, g, b;
};

// Guideline colors. Index matches PieceType.
constexpr std::array<Rgb, kPieceTypeCount> kPieceColors{{
    {0, 240, 240},  // I cyan
    {240, 240, 0},  // O yellow
    {160, 0, 240},  // T purple
    {0, 240, 0},    // S green
    {240, 0, 0},    // Z red
    {0, 0, 240},    // J blue
    {240, 160, 0},  // L orange
}};

ImU32 color(Rgb c, float alpha) { return IM_COL32(c.r, c.g, c.b, static_cast<int>(alpha * 255.0f)); }
ImVec4 colorVec(Rgb c, float alpha = 1.0f) { return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha); }
Rgb lighten(Rgb c) { return {c.r + (255 - c.r) * 2 / 5, c.g + (255 - c.g) * 2 / 5, c.b + (255 - c.b) * 2 / 5}; }
Rgb darken(Rgb c) { return {c.r * 3 / 5, c.g * 3 / 5, c.b * 3 / 5}; }
constexpr Rgb kGarbageColor{128, 128, 136};
constexpr Rgb kBombColor{255, 200, 20};  // hazard yellow, warmer than the O piece
constexpr Rgb kBombInk{24, 22, 20};
constexpr Rgb kWellFill{18, 18, 26};
constexpr Rgb kWellBorder{110, 110, 140};
constexpr Rgb kLabelColor{150, 160, 190};
constexpr Rgb kTextColor{235, 235, 245};
Rgb pieceRgb(PieceType type) {
    if (type == PieceType::Garbage) return kGarbageColor;
    if (type == PieceType::Bomb) return kBombColor;
    return kPieceColors[static_cast<int>(type)];
}
float rowAlpha(int y) { return y >= kVisibleHeight ? kHiddenAlpha : 1.0f; }

struct Layout {
    float cell;     // cell size in points
    ImVec2 origin;  // top-left of the topmost drawn row (y = 21)
};

Layout computeLayout() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float cell = std::floor(
        std::min((vp->Size.y - 2 * kMargin) / kDrawnRows, (vp->Size.x - 2 * kMargin) / kLayoutColumns));
    return {cell, ImVec2(vp->Pos.x + (vp->Size.x - kBoardWidth * cell) * 0.5f,
                         vp->Pos.y + (vp->Size.y - kDrawnRows * cell) * 0.5f)};
}

ImVec2 cellTopLeft(const Layout& l, int x, int y) {
    return ImVec2(l.origin.x + x * l.cell, l.origin.y + (kDrawnRows - 1 - y) * l.cell);
}

float bevelFor(float size) { return std::max(1.0f, std::floor(size * 0.12f)); }

// A filled rect with a lit top-left edge and a shaded bottom-right one; swap the edges to make it look sunken.
void drawBevelRect(ImDrawList* dl, ImVec2 p, ImVec2 q, float bevel, Rgb fill, Rgb topLeft, Rgb bottomRight,
                   float alpha = 1.0f) {
    dl->AddRectFilled(p, q, color(fill, alpha));
    dl->AddRectFilled(p, ImVec2(q.x, p.y + bevel), color(topLeft, alpha));       // top
    dl->AddRectFilled(p, ImVec2(p.x + bevel, q.y), color(topLeft, alpha));       // left
    dl->AddRectFilled(ImVec2(p.x, q.y - bevel), q, color(bottomRight, alpha));  // bottom
    dl->AddRectFilled(ImVec2(q.x - bevel, p.y), q, color(bottomRight, alpha));  // right
}

// Text with a hard, unblurred shadow `offset` points down and right, like the nameplates.
void drawShadowText(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, ImU32 col, const char* text,
                    float offset, ImU32 shadow = IM_COL32(0, 0, 0, 220)) {
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    dl->AddText(fonts.hud, size, ImVec2(pos.x + offset, pos.y + offset), shadow, text);
    dl->AddText(fonts.hud, size, pos, col, text);
}

// A hazard-yellow block with the radiation trefoil: three 60-degree blades around a hub, inside a ring.
void drawBombCell(ImDrawList* dl, ImVec2 p, float size, float alpha) {
    const float bevel = bevelFor(size);
    drawBevelRect(dl, p, ImVec2(p.x + size, p.y + size), bevel, kBombColor, lighten(kBombColor), darken(kBombColor),
                  alpha);
    const ImU32 ink = color(kBombInk, alpha);
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float inner = size * 0.5f - bevel;
    dl->AddCircle(c, inner - size * 0.04f, ink, 0, std::max(1.0f, size * 0.07f));
    const float bladeOuter = inner - size * 0.12f;
    constexpr float kPi = 3.14159265f;
    for (int i = 0; i < 3; ++i) {  // screen y points down, so the 90-degree blade points down
        const float mid = kPi * 0.5f + i * kPi * 2.0f / 3.0f;
        dl->PathLineTo(c);
        dl->PathArcTo(c, bladeOuter, mid - kPi / 6.0f, mid + kPi / 6.0f);
        dl->PathFillConvex(ink);
    }
    dl->AddCircleFilled(c, bladeOuter * 0.32f, color(kBombColor, alpha));  // gap between hub and blades
    dl->AddCircleFilled(c, bladeOuter * 0.2f, ink);                          // hub
}

void drawCell(ImDrawList* dl, ImVec2 p, float size, PieceType type, float alpha) {
    if (type == PieceType::Bomb) return drawBombCell(dl, p, size, alpha);
    const Rgb base = pieceRgb(type);
    drawBevelRect(dl, p, ImVec2(p.x + size, p.y + size), bevelFor(size), base, lighten(base), darken(base), alpha);
}

void drawWell(ImDrawList* dl, const Layout& l) {
    const ImVec2 top = cellTopLeft(l, 0, kVisibleHeight - 1);
    const ImVec2 bottom(top.x + kBoardWidth * l.cell, top.y + kVisibleHeight * l.cell);
    dl->AddRectFilled(top, bottom, color(kWellFill, 1.0f));
    dl->AddRect(ImVec2(top.x - 2, top.y - 2), ImVec2(bottom.x + 2, bottom.y + 2), color(kWellBorder, 1.0f), 0.0f, 0,
                2.0f);
}

// Like cellTopLeft, for a row partway between two (while the stack falls after a clear), snapped to whole pixels.
ImVec2 cellTopLeftAt(const Layout& l, int x, float y) {
    return ImVec2(l.origin.x + x * l.cell, std::floor(l.origin.y + (kDrawnRows - 1 - y) * l.cell));
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// A stable pseudo-random number in [0, 1) for one particle, so the clear animation needs no state of its own.
float hash01(int a, int b, int c) {
    std::uint32_t h = static_cast<std::uint32_t>(a) * 73856093u ^ static_cast<std::uint32_t>(b) * 19349663u ^
                      static_cast<std::uint32_t>(c) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65536.0f;
}

// The clear runs in two halves of kClearDelayMs. First each cleared cell flashes white and shrinks away,
// sweeping out from the middle of the row, and throws off a few fragments. Then the rows above fall into the gap,
// landing as the clear finishes so the next frame's board matches.
constexpr float kBurstEnd = 0.55f;     // the shrink ends here, as a fraction of the clear
constexpr float kFallStart = 0.45f;    // and the stack starts falling here
constexpr float kSweepDelay = 0.15f;   // the edge columns start this much later than the middle ones
constexpr int kFragments = 3;          // per cleared cell; bombs throw twice as many
constexpr float kGravity = 60.0f;      // fragments, in cells per second squared
constexpr float kClearSeconds = kClearDelayMs / 1000.0f;

void drawClearingCell(ImDrawList* dl, const Layout& l, int x, int y, PieceType type, float progress) {
    const float delay = std::abs(x - (kBoardWidth - 1) * 0.5f) / (kBoardWidth * 0.5f) * kSweepDelay;
    const float local = clamp01((progress - delay) / (kBurstEnd - kSweepDelay));
    const ImVec2 p = cellTopLeft(l, x, y);
    const ImVec2 center(p.x + l.cell * 0.5f, p.y + l.cell * 0.5f);
    const float alpha = rowAlpha(y);

    if (local < 1.0f) {
        const float size = std::floor(l.cell * (1.0f - local * local));
        const ImVec2 q(std::floor(center.x - size * 0.5f), std::floor(center.y - size * 0.5f));
        if (size >= 2.0f) drawCell(dl, q, size, type, alpha);
        const float white = clamp01(1.0f - local * 2.5f);  // a white flash over the first part
        dl->AddRectFilled(q, ImVec2(q.x + size, q.y + size), color({255, 255, 255}, white * alpha));
    }

    const bool bomb = type == PieceType::Bomb;
    if (bomb && local > 0.0f && local < 1.0f) {  // a shock ring
        dl->AddCircle(center, l.cell * (0.4f + 1.4f * local), color({255, 150, 30}, (1.0f - local) * alpha), 0,
                      std::max(1.0f, l.cell * 0.15f * (1.0f - local)));
    }

    const float t = (progress - delay) * kClearSeconds;  // seconds since this cell popped
    if (t <= 0.0f) return;
    const float life = clamp01(1.0f - (progress - delay) / (1.0f - delay));
    const Rgb shard = bomb ? Rgb{255, 170, 40} : lighten(pieceRgb(type));
    const float speed = bomb ? 1.6f : 1.0f;
    for (int i = 0; i < kFragments * (bomb ? 2 : 1); ++i) {
        const float vx = (hash01(x, y, i * 2) - 0.5f) * 16.0f * speed;
        const float vy = (6.0f + hash01(x, y, i * 2 + 1) * 10.0f) * speed;  // upward, in cells per second
        const float dx = vx * t;
        const float dy = vy * t - 0.5f * kGravity * t * t;
        const float size = std::max(1.0f, std::floor(l.cell * (0.25f + 0.2f * hash01(y, x, i)) * (0.4f + 0.6f * life)));
        const ImVec2 f(std::floor(center.x + dx * l.cell - size * 0.5f), std::floor(center.y - dy * l.cell - size * 0.5f));
        dl->AddRectFilled(f, ImVec2(f.x + size, f.y + size), color(shard, life * alpha));
    }
}

void drawBoardCells(ImDrawList* dl, const Layout& l, const Game& game) {
    const std::vector<int>& clearing = game.clearingRows();
    const float progress = clamp01(static_cast<float>(game.clearElapsedMs()) / kClearDelayMs);
    const float fall = clamp01((progress - kFallStart) / (1.0f - kFallStart));
    const float eased = fall * fall;  // accelerating, like gravity
    for (int y = 0; y < kBoardHeight; ++y) {
        if (std::binary_search(clearing.begin(), clearing.end(), y)) continue;
        const auto below = std::lower_bound(clearing.begin(), clearing.end(), y) - clearing.begin();
        const float shown = static_cast<float>(y) - static_cast<float>(below) * eased;
        if (shown > kDrawnRows - 1) continue;
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = game.board().at(x, y))
                drawCell(dl, cellTopLeftAt(l, x, shown), l.cell, *c, rowAlpha(static_cast<int>(std::lround(shown))));
    }
    for (int y : clearing) {
        if (y >= kDrawnRows) continue;
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = game.board().at(x, y)) drawClearingCell(dl, l, x, y, *c, progress);
    }
}

void drawActivePiece(ImDrawList* dl, const Layout& l, const Game& game) {
    Piece ghost = game.active();
    ghost.pos.y -= game.ghostDistance();
    const Rgb rgb = pieceRgb(ghost.type);
    for (Point c : ghost.cells()) {
        if (c.y >= kDrawnRows) continue;
        const ImVec2 p = cellTopLeft(l, c.x, c.y);
        dl->AddRect(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + l.cell - 1, p.y + l.cell - 1), color(rgb, 0.6f), 0.0f, 0,
                    2.0f);
    }
    for (Point c : game.active().cells())
        if (c.y < kDrawnRows) drawCell(dl, cellTopLeft(l, c.x, c.y), l.cell, game.active().type, rowAlpha(c.y));
}

void drawMiniPiece(ImDrawList* dl, ImVec2 topLeft, float cell, PieceType type, float alpha) {
    for (Point c : shapeCells(type, Rotation::Zero))
        drawCell(dl, ImVec2(topLeft.x + c.x * cell, topLeft.y + c.y * cell), cell, type, alpha);
}

void drawLabel(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, const char* text) {
    dl->AddText(fonts.hud, size, pos, IM_COL32(230, 230, 240, 255), text);
}

constexpr float kMiniScale = 0.7f;        // HOLD and NEXT pieces, in cells
constexpr float kLockedHoldAlpha = 0.35f;  // the held piece while hold is locked

constexpr int kSideStats = 3;            // SCORE, LEVEL, LINES under NEXT
constexpr float kStatPitch = 3.5f;       // each stat's height, in text sizes

float sideTextSize(const Layout& l) { return l.cell * 0.6f; }
float statsTop(const Layout& l) {
    return cellTopLeft(l, 0, kVisibleHeight - 1).y + l.cell + 5 * 2.5f * l.cell * kMiniScale + l.cell;
}

void drawSidePanels(ImDrawList* dl, const Layout& l, const Game& game, const Fonts& fonts) {
    const float textSize = sideTextSize(l);
    const float mini = l.cell * kMiniScale;
    const float top = cellTopLeft(l, 0, kVisibleHeight - 1).y;

    const float holdX = l.origin.x - 5.5f * l.cell;
    drawLabel(dl, fonts, textSize, ImVec2(holdX, top), "HOLD");
    if (const auto held = game.hold())
        drawMiniPiece(dl, ImVec2(holdX, top + l.cell), mini, *held, game.canHold() ? 1.0f : kLockedHoldAlpha);

    const float rightX = l.origin.x + (kBoardWidth + 1) * l.cell;
    drawLabel(dl, fonts, textSize, ImVec2(rightX, top), "NEXT");
    const std::vector<PieceType> next = game.next(5);
    for (int i = 0; i < static_cast<int>(next.size()); ++i)
        drawMiniPiece(dl, ImVec2(rightX, top + l.cell + i * 2.5f * mini), mini, next[i], 1.0f);

    float y = statsTop(l);
    const std::array<std::pair<const char*, int>, kSideStats> stats{
        {{"SCORE", game.score()}, {"LEVEL", game.level()}, {"LINES", game.lines()}}};
    char value[32];
    for (const auto& [label, number] : stats) {
        drawLabel(dl, fonts, textSize, ImVec2(rightX, y), label);
        std::snprintf(value, sizeof value, "%d", number);
        drawLabel(dl, fonts, textSize, ImVec2(rightX, y + textSize * 1.4f), value);
        y += textSize * kStatPitch;
    }
}

ImDrawList* beginBoardWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##game", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoInputs);
    return ImGui::GetWindowDrawList();
}

// Under the HOLD panel, in yellow.
void drawComboLabel(ImDrawList* dl, const Layout& l, const Fonts& fonts, const ComboLabel& combo) {
    if (combo.shown <= 0) return;
    char text[24];
    std::snprintf(text, sizeof text, "COMBO x%d", combo.shown);
    const ImVec2 pos(l.origin.x - 5.5f * l.cell, cellTopLeft(l, 0, kVisibleHeight - 1).y + 4.0f * l.cell);
    dl->AddText(fonts.hud, l.cell * 0.6f, pos, IM_COL32(255, 220, 60, static_cast<int>(combo.alpha() * 255.0f)), text);
}

Rgb calloutRgb(CalloutTone tone) {
    switch (tone) {
    case CalloutTone::BackToBack:
        return {255, 210, 40};
    case CalloutTone::Spin:
        return lighten(kPieceColors[static_cast<int>(PieceType::T)]);
    case CalloutTone::Tetris:
        return kPieceColors[static_cast<int>(PieceType::I)];
    case CalloutTone::Perfect:
        break;
    }
    return {255, 245, 200};
}

// Under the COMBO label, in the HOLD column: one short line per part of the clear, with a hard shadow.
void drawCallout(ImDrawList* dl, const Layout& l, const Fonts& fonts, const ClearCallout& callout) {
    if (callout.ms <= 0) return;
    const float size = std::floor(l.cell * 0.6f * callout.popScale());
    const int alpha = static_cast<int>(callout.alpha() * 255.0f);
    ImVec2 pos(l.origin.x - 5.5f * l.cell, cellTopLeft(l, 0, kVisibleHeight - 1).y + 5.5f * l.cell);
    for (const CalloutLine& line : calloutLines(callout.shown)) {
        drawShadowText(dl, fonts, size, pos, color(calloutRgb(line.tone), alpha / 255.0f), line.text.c_str(),
                       bevelFor(size), IM_COL32(0, 0, 0, alpha * 220 / 255));
        pos.y += l.cell * 0.9f;
    }
}

void drawOutlinedText(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, const char* text, ImU32 fill,
                      ImU32 outline, float thickness);

// PERFECT / CLEAR in large outlined letters across the middle of the well, fading with the callout.
void drawPerfectFlash(ImDrawList* dl, const Layout& l, const Fonts& fonts, const ClearCallout& callout) {
    if (!callout.perfectFlash()) return;
    const float size = std::floor(l.cell * 1.2f * callout.popScale());
    const float fade = static_cast<float>(callout.ms - (kCalloutMs - kPerfectFlashMs)) / kPerfectFlashMs;
    const float alpha = std::min(1.0f, fade * 3.0f);  // full for most of the flash, then a quick fade
    const float centerX = l.origin.x + kBoardWidth * l.cell * 0.5f;
    const float middle = cellTopLeft(l, 0, kVisibleHeight / 2).y;
    const std::array<const char*, 2> words{"PERFECT", "CLEAR"};
    for (int i = 0; i < 2; ++i) {
        const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, words[i]);
        const ImVec2 pos(centerX - extent.x * 0.5f, middle - extent.y * (1.1f - 1.2f * i));
        drawOutlinedText(dl, fonts, size, pos, words[i], color(calloutRgb(CalloutTone::Perfect), alpha),
                         IM_COL32(255, 120, 0, static_cast<int>(alpha * 200.0f)),
                         std::max(1.0f, std::floor(l.cell * 0.12f)));
    }
}

// `comboUnderHold` is false in duels, where the combo sits in the center column instead.
void drawPlayfield(ImDrawList* dl, const Layout& layout, const Game& game, const Fonts& fonts,
                   const ComboLabel& combo, const ClearCallout& callout, bool comboUnderHold) {
    drawWell(dl, layout);
    if (game.state() == GameState::Paused) return;
    drawBoardCells(dl, layout, game);
    if (game.state() == GameState::Playing) drawActivePiece(dl, layout, game);
    drawSidePanels(dl, layout, game, fonts);
    if (comboUnderHold) drawComboLabel(dl, layout, fonts, combo);
    drawCallout(dl, layout, fonts, callout);
    drawPerfectFlash(dl, layout, fonts, callout);
}

// Layout A, left to right, in cells: hold 5 | your well 10 | meter | next 5 | center 7 | meter | their well 10 |
// stats 5, plus gutters. The name bars sit above the 22 drawn rows.
constexpr float kDuelColumns = 46.0f;
constexpr float kNameBarRows = 1.5f;
constexpr float kDuelRows = kDrawnRows + kNameBarRows;
constexpr float kMyWellCol = 5.5f;  // drawSidePanels puts HOLD 5.5 cells left and NEXT 11 cells right of it
constexpr float kMyMeterCol = 15.75f;
constexpr float kCenterCol = 22.0f;
constexpr float kCenterColumns = 7.0f;
constexpr float kTheirMeterCol = 29.9f;
constexpr float kTheirWellCol = 30.5f;
constexpr float kStatsCol = 41.0f;
constexpr int kTimeWarningSeconds = 10;  // the clock turns red once it reads under 0:10

struct DuelLayout {
    Layout mine;    // the local well
    Layout theirs;  // the opponent's well, same cell size
    float left;     // left edge of the whole view
    float top;      // top of the name bars
};

DuelLayout computeDuelLayout() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float cell =
        std::floor(std::min((vp->Size.y - 2 * kMargin) / kDuelRows, (vp->Size.x - 2 * kMargin) / kDuelColumns));
    const float left = vp->Pos.x + (vp->Size.x - kDuelColumns * cell) * 0.5f;
    const float top = vp->Pos.y + (vp->Size.y - kDuelRows * cell) * 0.5f;
    const float rowsTop = top + kNameBarRows * cell;
    return {{cell, ImVec2(left + kMyWellCol * cell, rowsTop)},
            {cell, ImVec2(left + kTheirWellCol * cell, rowsTop)},
            left,
            top};
}

float wellBottom(const Layout& l) { return cellTopLeft(l, 0, 0).y + l.cell; }
float wellTop(const Layout& l) { return cellTopLeft(l, 0, kVisibleHeight - 1).y; }

constexpr Rgb kMyPlateColor{30, 90, 220};
constexpr Rgb kTheirPlateColor{220, 70, 30};
constexpr Rgb kKoPipColor{255, 210, 40};
constexpr Rgb kBackdropColor{6, 10, 28};
constexpr Rgb kPanelColor{14, 20, 44};  // menu windows, a step lighter than the backdrop
constexpr Rgb kGridColor{16, 24, 50};
constexpr Rgb kDangerColor{230, 40, 40};

// A flat dark fill with a faint one-cell grid. One line passes through `anchor`, so the grid lines up with a well.
// The lines are 1px filled rects rather than AddLine, which would anti-alias them across two pixels.
void drawGridBackground(float cell, ImVec2 anchor) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImVec2 end(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    bg->AddRectFilled(vp->Pos, end, color(kBackdropColor, 1.0f));
    if (cell < 2.0f) return;  // a window too small for the board; the steps below would never advance
    const ImU32 line = color(kGridColor, 1.0f);
    const float startX = anchor.x - std::ceil((anchor.x - vp->Pos.x) / cell) * cell;
    for (float x = std::floor(startX); x < end.x; x += cell)
        bg->AddRectFilled(ImVec2(x, vp->Pos.y), ImVec2(x + 1.0f, end.y), line);
    const float startY = anchor.y - std::ceil((anchor.y - vp->Pos.y) / cell) * cell;
    for (float y = std::floor(startY); y < end.y; y += cell)
        bg->AddRectFilled(ImVec2(vp->Pos.x, y), ImVec2(end.x, y + 1.0f), line);
}

// Garbage waiting to rise, as a stack of beveled garbage cells, one per line, with its left edge at `x`. From
// kGarbageDangerLines it turns red and blinks every 250 ms.
void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending) {
    if (pending <= 0) return;
    const bool danger = garbageMeterDanger(pending);
    const Rgb base = danger ? kDangerColor : kGarbageColor;
    const float alpha = danger && static_cast<int>(ImGui::GetTime() * 4.0) % 2 != 0 ? 0.45f : 1.0f;
    const float width = std::floor(cell * 0.5f);
    const float bevel = bevelFor(cell);
    x = std::floor(x);
    for (int i = 0; i < std::min(pending, kVisibleHeight); ++i) {
        const float lower = std::floor(bottom - static_cast<float>(i) * cell);
        drawBevelRect(dl, ImVec2(x, lower - cell), ImVec2(x + width, lower), bevel, base, lighten(base), darken(base),
                      alpha);
    }
}

// A nameplate built like one wide mino: flat player color, the cells' bevel, the name with a hard drop shadow,
// and a row of KO slots on the right that fill with gold mini-blocks as KOs land.
void drawNameBar(ImDrawList* dl, const Fonts& fonts, const Layout& well, float top, const char* name, Rgb base,
                 int kos) {
    const float cell = well.cell;
    const float bevel = bevelFor(cell);
    const ImVec2 p0(std::floor(well.origin.x), std::floor(top));
    const ImVec2 p1(p0.x + kBoardWidth * cell, p0.y + std::floor(1.1f * cell));
    drawBevelRect(dl, p0, p1, bevel, base, lighten(base), darken(base));

    const float textSize = std::floor(cell * 0.7f);
    const ImVec2 textPos(p0.x + std::floor(0.35f * cell), p0.y + std::floor((p1.y - p0.y - textSize) * 0.5f));
    drawShadowText(dl, fonts, textSize, textPos, IM_COL32(255, 255, 255, 255), name, bevel,
                   color(darken(darken(base)), 1.0f));

    const float pip = std::floor(cell * 0.5f);
    const float gap = std::floor(cell * 0.2f);
    const float pipBevel = std::max(1.0f, bevel - 1.0f);
    const float pipTop = p0.y + std::floor((p1.y - p0.y - pip) * 0.5f);
    for (int i = 0; i < kKosToWin; ++i) {
        const float x = p1.x - std::floor(0.35f * cell) - (pip + gap) * static_cast<float>(kKosToWin - i) + gap;
        const ImVec2 a(x, pipTop);
        const ImVec2 b(x + pip, pipTop + pip);
        if (i < kos)
            drawBevelRect(dl, a, b, pipBevel, kKoPipColor, lighten(kKoPipColor), darken(kKoPipColor));
        else  // sunken: the light edge moves to the bottom-right
            drawBevelRect(dl, a, b, pipBevel, darken(darken(base)), darken(darken(darken(base))), lighten(base));
    }
}

void drawCentered(ImDrawList* dl, const Fonts& fonts, float size, float centerX, float y, const char* text,
                  ImU32 col) {
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(fonts.hud, size, ImVec2(centerX - extent.x * 0.5f, y), col, text);
}

void drawCenteredShadow(ImDrawList* dl, const Fonts& fonts, float size, float centerX, float y, const char* text,
                        ImU32 col, float offset, ImU32 shadow = IM_COL32(0, 0, 0, 220)) {
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    drawShadowText(dl, fonts, size, ImVec2(centerX - extent.x * 0.5f, y), col, text, offset, shadow);
}

// TIME, the match target, your COMBO and SENT, with your number on the left. The plates already show the KOs.
void drawScoreboard(ImDrawList* dl, const Fonts& fonts, const DuelLayout& layout, const DuelSession& duel,
                    const ComboLabel& combo) {
    const float cell = layout.mine.cell;
    const float cx = layout.left + (kCenterCol + kCenterColumns * 0.5f) * cell;
    const ImU32 label = color(kLabelColor, 1.0f);
    const ImU32 value = color(kTextColor, 1.0f);
    char text[32];
    float y = wellTop(layout.mine) + cell;

    const int seconds = (duel.matchClockMs() + 999) / 1000;
    std::snprintf(text, sizeof text, "%d:%02d", seconds / 60, seconds % 60);
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "TIME", label);
    drawCentered(dl, fonts, cell * 1.1f, cx, y + cell * 0.8f, text,
                 seconds < kTimeWarningSeconds ? IM_COL32(255, 70, 70, 255) : value);
    y += cell * 4.0f;
    std::snprintf(text, sizeof text, "FIRST TO %d", kKosToWin);  // explains the pips on the plates
    drawCenteredShadow(dl, fonts, cell * 0.45f, cx, y, text, color(kKoPipColor, 1.0f), bevelFor(cell * 0.45f));
    y += cell * 3.5f;
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "COMBO", label);
    if (combo.shown > 0) {  // fades like the solo label once the combo breaks
        std::snprintf(text, sizeof text, "x%d", combo.shown);
        drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, text, color(kKoPipColor, combo.alpha()));
    } else {
        drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, "-", label);
    }
    y += cell * 3.5f;
    std::snprintf(text, sizeof text, "%d : %d", duel.sentMine(), duel.sentTheirs());
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "SENT", label);
    drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, text, value);
}

// The opponent's well at full size, then one column right of it: HOLD, NEXT, SCORE and LINES.
void drawOpponent(ImDrawList* dl, const DuelLayout& layout, const Snapshot& snapshot, const Fonts& fonts) {
    const Layout& l = layout.theirs;
    drawWell(dl, l);
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = snapshot.cells[y * kBoardWidth + x]) drawCell(dl, cellTopLeft(l, x, y), l.cell, *c, 1.0f);

    const float statsX = layout.left + kStatsCol * l.cell;
    const float textSize = l.cell * 0.6f;
    const float mini = l.cell * kMiniScale;
    float y = wellTop(l);
    drawLabel(dl, fonts, textSize, ImVec2(statsX, y), "HOLD");
    if (snapshot.hold)
        drawMiniPiece(dl, ImVec2(statsX, y + l.cell), mini, *snapshot.hold,
                      snapshot.holdLocked ? kLockedHoldAlpha : 1.0f);
    y += 3.0f * l.cell;
    drawLabel(dl, fonts, textSize, ImVec2(statsX, y), "NEXT");
    for (int i = 0; i < kSnapshotNext; ++i)
        if (const Cell piece = snapshot.next[i])
            drawMiniPiece(dl, ImVec2(statsX, y + l.cell + i * 2.5f * mini), mini, *piece, 1.0f);
    y += l.cell + kSnapshotNext * 2.5f * mini + l.cell;
    const std::array<std::pair<const char*, int>, 2> stats{{{"SCORE", snapshot.score}, {"LINES", snapshot.lines}}};
    char value[32];
    for (const auto& [label, number] : stats) {
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y), label);
        std::snprintf(value, sizeof value, "%d", number);
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y + textSize * 1.4f), value);
        y += textSize * kStatPitch;
    }
}

// `fill` text with an `outline` copy drawn `thickness` points away in all eight directions.
void drawOutlinedText(ImDrawList* dl, const Fonts& fonts, float size, ImVec2 pos, const char* text, ImU32 fill,
                      ImU32 outline, float thickness) {
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx != 0 || dy != 0)
                dl->AddText(fonts.hud, size, ImVec2(pos.x + dx * thickness, pos.y + dy * thickness), outline, text);
    dl->AddText(fonts.hud, size, pos, fill, text);
}

constexpr ImU32 kFlashFill = IM_COL32(255, 230, 40, 255);
constexpr ImU32 kFlashOutline = IM_COL32(255, 120, 0, 200);

// A large yellow "KO!" with an orange glow, centered on the well.
// Where an attack lands: the top of the garbage meter whose left edge is at `meterX`.
ImVec2 meterTop(const Layout& well, float meterX, int pending) {
    const int stacked = std::min(std::max(pending, 1), kVisibleHeight);
    return ImVec2(meterX + std::floor(well.cell * 0.5f) * 0.5f, wellBottom(well) - static_cast<float>(stacked) * well.cell);
}

ImVec2 bezier(ImVec2 a, ImVec2 c, ImVec2 b, float t) {
    const float u = 1.0f - t;
    return ImVec2(u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y);
}

// A pixel comet arcing over the center column: a bright head, a tail of shrinking squares, then a burst on the
// meter. Ours fly in our plate blue, theirs in their plate orange.
void drawAttackTrail(ImDrawList* dl, const AttackTrail& trail, ImVec2 from, ImVec2 to, float cell) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 control((from.x + to.x) * 0.5f, std::max(vp->Pos.y + cell, std::min(from.y, to.y) - 8.0f * cell));
    const Rgb tone = lighten(trail.outgoing ? kMyPlateColor : kTheirPlateColor);
    const float head = trailProgress(trail.ms);
    constexpr int kTail = 16;
    constexpr float kTailStep = 0.025f;  // of the flight, between tail squares
    if (trail.ms < kTrailFlightMs) {
        for (int i = kTail; i >= 0; --i) {
            const float t = head - static_cast<float>(i) * kTailStep;
            if (t < 0.0f) continue;
            const float fade = 1.0f - static_cast<float>(i) / (kTail + 1);
            const float size = std::max(1.0f, std::floor(cell * 0.8f * fade));
            const ImVec2 c = bezier(from, control, to, t);
            const ImVec2 p(std::floor(c.x - size * 0.5f), std::floor(c.y - size * 0.5f));
            dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color(i < 3 ? Rgb{255, 255, 255} : tone, fade));
        }
        const ImVec2 c = bezier(from, control, to, head);
        const float glow = std::floor(cell * 1.4f);
        const float core = std::floor(cell * 0.6f);
        dl->AddRectFilled(ImVec2(std::floor(c.x - glow * 0.5f), std::floor(c.y - glow * 0.5f)),
                          ImVec2(std::floor(c.x + glow * 0.5f), std::floor(c.y + glow * 0.5f)), color(tone, 0.45f));
        dl->AddRectFilled(ImVec2(std::floor(c.x - core * 0.5f), std::floor(c.y - core * 0.5f)),
                          ImVec2(std::floor(c.x + core * 0.5f), std::floor(c.y + core * 0.5f)),
                          IM_COL32(255, 255, 255, 255));
        return;
    }
    const float t = clamp01(static_cast<float>(trail.ms - kTrailFlightMs) / kTrailImpactMs);
    const float alpha = 1.0f - t;
    const float ring = std::floor(cell * (0.5f + 1.5f * t));
    dl->AddRect(ImVec2(to.x - ring, to.y - ring), ImVec2(to.x + ring, to.y + ring), color(tone, alpha), 0.0f, 0,
                std::max(1.0f, std::floor(cell * 0.2f * alpha)));
    for (int i = 0; i < 8; ++i) {  // sparks in eight directions
        const float angle = static_cast<float>(i) * 0.785398f;
        const float reach = cell * (0.4f + 1.8f * t);
        const float size = std::max(1.0f, std::floor(cell * 0.25f * alpha));
        const ImVec2 p(std::floor(to.x + std::cos(angle) * reach - size * 0.5f),
                       std::floor(to.y + std::sin(angle) * reach - size * 0.5f));
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color({255, 255, 255}, alpha));
    }
}

void drawAttackTrails(ImDrawList* dl, const DuelLayout& layout, const DuelSession& duel, const AttackTrails& trails) {
    const float cell = layout.mine.cell;
    for (const AttackTrail& trail : trails.live) {
        const Layout& sender = trail.outgoing ? layout.mine : layout.theirs;
        const ImVec2 from(sender.origin.x + kBoardWidth * cell * 0.5f,
                          cellTopLeftAt(sender, 0, trail.fromRow).y + cell * 0.5f);
        const ImVec2 to = trail.outgoing
                              ? meterTop(layout.theirs, layout.left + kTheirMeterCol * cell, duel.opponent().pending)
                              : meterTop(layout.mine, layout.left + kMyMeterCol * cell, duel.game()->pendingGarbage());
        drawAttackTrail(dl, trail, from, to, cell);
    }
}

void drawKoFlash(ImDrawList* dl, const Fonts& fonts, const Layout& well) {
    const char* text = "KO!";
    const float size = well.cell * 3.0f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pos(well.origin.x + (kBoardWidth * well.cell - extent.x) * 0.5f,
                     (wellTop(well) + wellBottom(well) - extent.y) * 0.5f);
    drawOutlinedText(dl, fonts, size, pos, text, kFlashFill, kFlashOutline, std::max(1.0f, std::floor(well.cell * 0.15f)));
}

// "GO!" in the KO flash style, centered on the screen, as play starts.
void drawGo(const Fonts& fonts) {
    const char* text = "GO!";
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float size = vp->Size.y * 0.15f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - extent.x) * 0.5f, vp->Pos.y + (vp->Size.y - extent.y) * 0.5f);
    drawOutlinedText(ImGui::GetForegroundDrawList(), fonts, size, pos, text, kFlashFill, kFlashOutline,
                     std::max(1.0f, std::floor(size * 0.05f)));
}

// Drops typed characters a name can't contain, so the field only ever holds a valid name or nothing.
int nameCharFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    return c < 128 && isNameChar(static_cast<char>(c)) ? 0 : 1;
}

void beginMenu(const char* id) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Never taller than the screen: a menu that doesn't fit scrolls (wheel or keyboard focus) instead of clipping.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), vp->WorkSize);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
}

// Returns the title's width, so a menu can size its rows to match it.
float menuTitle(const Fonts& fonts, const char* title) {
    ImGui::PushFont(fonts.hud, ImGui::GetFontSize() * 2.0f);
    const float width = ImGui::CalcTextSize(title).x;
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Spacing();
    return width;
}

// Press Start 2P is one em per character: wide enough for "Create public room" plus the frame padding.
float menuButtonWidth() { return ImGui::GetFontSize() * 21.0f; }

// A button with the minos' bevel: light top-left, dark bottom-right, swapped while held down. `base` is the
// button's fill color, and the edges follow the disabled alpha.
bool bevelButton(const char* label, ImVec2 size, Rgb base = kMyPlateColor) {
    const bool pressed = ImGui::Button(label, size);
    const ImVec2 p(std::floor(ImGui::GetItemRectMin().x), std::floor(ImGui::GetItemRectMin().y));
    const ImVec2 q(std::floor(ImGui::GetItemRectMax().x), std::floor(ImGui::GetItemRectMax().y));
    const float alpha = ImGui::GetStyle().Alpha;
    const bool held = ImGui::IsItemActive();
    const ImU32 topLeft = color(held ? darken(base) : lighten(base), alpha);
    const ImU32 bottomRight = color(held ? lighten(base) : darken(base), alpha);
    constexpr float kEdge = 2.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(q.x, p.y + kEdge), topLeft);
    dl->AddRectFilled(p, ImVec2(p.x + kEdge, q.y), topLeft);
    dl->AddRectFilled(ImVec2(p.x, q.y - kEdge), q, bottomRight);
    dl->AddRectFilled(ImVec2(q.x - kEdge, p.y), q, bottomRight);
    return pressed;
}

bool menuButton(const char* label, float width = menuButtonWidth(), Rgb base = kMyPlateColor) {
    return bevelButton(label, ImVec2(width, 0.0f), base);
}

// The ### keeps one ImGui ID while the label flips, so keyboard focus stays on the item.
bool soundButton(bool muted, float width = menuButtonWidth()) {
    return menuButton(muted ? "Sound: Off###sound" : "Sound: On###sound", width);
}
bool musicButton(bool off, float width = menuButtonWidth()) {
    return menuButton(off ? "Music: Off###music" : "Music: On###music", width);
}

void drawDuelBoards(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, const ClearCallout& callout,
                    const AttackTrails& trails, const StartMenu& menu) {
    const Game& game = *duel.game();
    const char* mine = menu.playerName.empty() ? "YOU" : menu.playerName.c_str();
    const char* theirs = duel.opponentName().empty() ? "FOE" : duel.opponentName().c_str();
    const DuelLayout layout = computeDuelLayout();
    const float cell = layout.mine.cell;
    drawGridBackground(cell, layout.mine.origin);
    ImDrawList* dl = beginBoardWindow();
    drawNameBar(dl, fonts, layout.mine, layout.top, mine, kMyPlateColor, duel.kosMine());
    drawPlayfield(dl, layout.mine, game, fonts, combo, callout, false);
    drawGarbageMeter(dl, layout.left + kMyMeterCol * cell, wellBottom(layout.mine), cell, game.pendingGarbage());
    drawScoreboard(dl, fonts, layout, duel, combo);
    drawGarbageMeter(dl, layout.left + kTheirMeterCol * cell, wellBottom(layout.theirs), cell,
                     duel.opponent().pending);
    drawNameBar(dl, fonts, layout.theirs, layout.top, theirs, kTheirPlateColor, duel.kosTheirs());
    drawOpponent(dl, layout, duel.opponent(), fonts);
    drawAttackTrails(dl, layout, duel, trails);
    if (duel.koFlashMs() > 0) drawKoFlash(dl, fonts, layout.mine);
    ImGui::End();
}

// The digit pops in at 1.3x and settles, centered on the screen, with a hard shadow.
void drawCountdown(int ms, const Fonts& fonts) {
    char text[12];
    std::snprintf(text, sizeof text, "%d", (ms + 999) / 1000);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float base = vp->Size.y * 0.15f;
    const float size = base * countdownPopScale(ms);
    const float height = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).y;
    drawCenteredShadow(ImGui::GetForegroundDrawList(), fonts, size, vp->GetCenter().x,
                       vp->Pos.y + (vp->Size.y - height) * 0.5f, text, IM_COL32(255, 255, 255, 255),
                       std::max(1.0f, std::floor(base * 0.06f)));
}

// "SAM VS KAI" under the countdown digit with a gold VS; nothing until both names are known.
void drawVersus(const Fonts& fonts, const std::string& mine, const std::string& theirs) {
    if (mine.empty() || theirs.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float size = vp->Size.y * 0.04f;
    const float offset = std::max(1.0f, std::floor(size * 0.1f));
    const std::array<std::pair<const char*, ImU32>, 3> pieces{{{mine.c_str(), IM_COL32(255, 255, 255, 255)},
                                                               {" VS ", color(kKoPipColor, 1.0f)},
                                                               {theirs.c_str(), IM_COL32(255, 255, 255, 255)}}};
    float width = 0.0f;
    for (const auto& [text, col] : pieces) width += fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    ImVec2 pos(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + vp->Size.y * 0.62f);
    for (const auto& [text, col] : pieces) {
        drawShadowText(ImGui::GetForegroundDrawList(), fonts, size, pos, col, text, offset);
        pos.x += fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    }
}

const char* resultTitle(DuelOutcome outcome) {
    switch (outcome) {
    case DuelOutcome::Lost:
        return "YOU LOSE";
    case DuelOutcome::Draw:
        return "DRAW";
    default:
        return "YOU WIN";
    }
}

UiCommand drawDuelResult(const DuelSession& duel, const Fonts& fonts) {
    UiCommand command = UiCommand::None;
    beginMenu("##result");
    menuTitle(fonts, resultTitle(duel.outcome()));
    ImGui::TextUnformatted(resultReasonText(duel).c_str());
    ImGui::Spacing();
    if (duel.opponentGone()) {
        if (duel.outcome() != DuelOutcome::OpponentLeft)
            ImGui::TextUnformatted(
                ((duel.opponentName().empty() ? std::string("Opponent") : duel.opponentName()) + " left.").c_str());
    } else if (duel.rematchRequested()) {
        ImGui::TextUnformatted("Waiting for opponent...");
    } else if (menuButton("Rematch")) {
        command = UiCommand::Rematch;
    }
    if (menuButton("Leave")) command = UiCommand::LeaveDuel;
    ImGui::End();
    return command;
}

constexpr int kLobbyVisibleRows = 8;  // the list scrolls past this many

UiCommand drawLobby(const DuelSession& duel, const Fonts& fonts, StartMenu& menu) {
    UiCommand command = UiCommand::None;
    beginMenu("##lobby");
    // Wide enough for the longest name next to the Level and Join columns.
    const std::string widest = std::string(kMaxNameLength, 'M') + " Level Join";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tableWidth = ImGui::CalcTextSize(widest.c_str()).x + style.CellPadding.x * 6.0f +
                             style.FramePadding.x * 2.0f + style.ScrollbarSize;
    const float width = std::max({menuTitle(fonts, "OPEN ROOMS"), menuButtonWidth(), tableWidth});
    if (!duel.error().empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", duel.error().c_str());
    if (duel.rooms().empty()) {
        ImGui::TextUnformatted("No open rooms yet.");
    } else {
        const float height = ImGui::GetFrameHeightWithSpacing() * kLobbyVisibleRows;
        if (ImGui::BeginTable("##rooms", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(width, height))) {
            ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const RoomInfo& room : duel.rooms()) {
                ImGui::PushID(room.code.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(room.host.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", room.startLevel);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Join")) {
                    menu.pickedRoom = room.code;
                    command = UiCommand::JoinListedRoom;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    if (menuButton("Quick match", width)) command = UiCommand::QuickMatch;
    if (menuButton("Create public room", width)) command = UiCommand::CreatePublicRoom;
    if (menuButton("Back", width)) command = UiCommand::LeaveDuel;
    ImGui::End();
    return command;
}

}  // namespace

void applyArcadeStyle(ImGuiStyle& style) {
    style.FontSizeBase = 12.0f;  // Press Start 2P is wide; 12 keeps the menus on a phone held sideways
    style.WindowRounding = style.ChildRounding = style.PopupRounding = style.FrameRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = style.TabRounding = style.ImageRounding = 0.0f;
    style.TreeLinesRounding = style.MenuItemRounding = 0.0f;
    style.WindowBorderSize = 2.0f;
    style.FrameBorderSize = 2.0f;
    // Tighter than chunky arcade padding, so the start screen's dozen rows still fit a phone held sideways.
    style.WindowPadding = ImVec2(16.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = colorVec(kPanelColor);
    c[ImGuiCol_Border] = colorVec(kWellBorder);
    c[ImGuiCol_Separator] = colorVec(kWellBorder);
    c[ImGuiCol_Button] = colorVec(kMyPlateColor);
    c[ImGuiCol_ButtonHovered] = colorVec(lighten(kMyPlateColor));
    c[ImGuiCol_ButtonActive] = colorVec(darken(kMyPlateColor));
    c[ImGuiCol_FrameBg] = colorVec(kWellFill);
    c[ImGuiCol_FrameBgHovered] = colorVec(lighten(kWellFill));
    c[ImGuiCol_FrameBgActive] = colorVec(lighten(lighten(kWellFill)));
    c[ImGuiCol_SliderGrab] = colorVec(kKoPipColor);
    c[ImGuiCol_SliderGrabActive] = colorVec(lighten(kKoPipColor));
    c[ImGuiCol_CheckMark] = colorVec(kKoPipColor);
    c[ImGuiCol_NavCursor] = colorVec(kKoPipColor);
    c[ImGuiCol_Header] = colorVec(kMyPlateColor, 0.35f);
    c[ImGuiCol_HeaderHovered] = colorVec(kMyPlateColor, 0.55f);
    c[ImGuiCol_HeaderActive] = colorVec(kMyPlateColor, 0.75f);
    c[ImGuiCol_TableHeaderBg] = colorVec(kMyPlateColor, 0.35f);
    c[ImGuiCol_TableRowBgAlt] = colorVec(kMyPlateColor, 0.12f);
    c[ImGuiCol_TextSelectedBg] = colorVec(kMyPlateColor, 0.6f);
    c[ImGuiCol_Text] = colorVec(kTextColor);
    c[ImGuiCol_TextDisabled] = colorVec(kLabelColor);
}

void configureImGuiIo(ImGuiIO& io) {
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

float countdownPopScale(int countdownMs) {
    // The digit shown is ceil(ms / 1000); it appeared when ms was last a whole second.
    const int sinceShown = (1000 - countdownMs % 1000) % 1000;
    const float t = std::min(1.0f, static_cast<float>(sinceShown) / kCountdownPopMs);
    return 1.3f - 0.3f * t;
}

bool showGoBanner(int matchClockMs) { return matchClockMs > kMatchMs - kGoBannerMs; }

bool garbageMeterDanger(int pending) { return pending >= kGarbageDangerLines; }

void ComboLabel::update(int combo, int ms) {
    if (combo >= 1) {
        shown = combo;
        fadeMs = kComboFadeMs;
        return;
    }
    fadeMs = std::max(0, fadeMs - ms);
    if (fadeMs == 0) shown = 0;
}

void AttackTrails::update(int ms) {
    for (AttackTrail& trail : live) trail.ms += ms;
    std::erase_if(live, [](const AttackTrail& trail) { return trail.ms >= kTrailMs; });
}

float trailProgress(int ms) {
    const float t = clamp01(static_cast<float>(ms) / kTrailFlightMs);
    return 0.4f * t + 0.6f * t * t;
}

float ClearCallout::popScale() const {
    const float t = std::min(1.0f, static_cast<float>(kCalloutMs - ms) / kCalloutPopMs);
    return 1.25f - 0.25f * t;
}

std::vector<CalloutLine> calloutLines(const ClearAward& award) {
    static constexpr std::array<const char*, 5> kLineNames{"", "SINGLE", "DOUBLE", "TRIPLE", "TETRIS"};
    std::vector<CalloutLine> lines;
    if (award.backToBack > 0) {
        char text[16];
        std::snprintf(text, sizeof text, "B2B x%d", award.backToBack);
        lines.push_back({text, CalloutTone::BackToBack});
    }
    if (award.spin != Spin::None) {
        lines.push_back({"T-SPIN", CalloutTone::Spin});
        if (award.spin == Spin::Mini) lines.push_back({"MINI", CalloutTone::Spin});
        if (award.lines > 0) lines.push_back({kLineNames[award.lines], CalloutTone::Spin});
    } else if (award.lines == 4) {
        lines.push_back({"TETRIS", CalloutTone::Tetris});
    }
    if (award.perfectClear) {
        lines.push_back({"PERFECT", CalloutTone::Perfect});
        lines.push_back({"CLEAR", CalloutTone::Perfect});
    }
    return lines;
}

UiCommand drawStartScreen(const Fonts& fonts, StartMenu& menu) {
    UiCommand command = UiCommand::None;
    beginMenu("##start");
    // Every row spans the title's width, so the menu is one even column under it.
    const float width = std::max(menuTitle(fonts, "JESTRIS BATTLE"), menuButtonWidth());
    ImGui::SeparatorText("SOLO");
    constexpr std::array<std::pair<SoloMode, const char*>, 5> kSoloButtons{{{SoloMode::Marathon, "Marathon"},
                                                                          {SoloMode::Sprint, "Sprint 40L"},
                                                                          {SoloMode::Blitz, "Blitz 2:00"},
                                                                          {SoloMode::Cheese, "Cheese 10L"},
                                                                          {SoloMode::Zen, "Zen"}}};
    for (const auto& [mode, label] : kSoloButtons) {
        if (menuButton(label, width)) {
            menu.soloMode = mode;
            command = UiCommand::PlaySolo;
        }
        if (const std::optional<int> best = menu.bests.best(mode)) {  // dim, right-aligned on the button row
            const std::string text =
                "PB " + (mode == SoloMode::Blitz ? formatScore(*best) : formatClock(*best));
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 max = ImGui::GetItemRectMax();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 pos(max.x - ImGui::GetStyle().FramePadding.x - size.x,
                             min.y + (max.y - min.y - size.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
        }
    }
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt("##level", &menu.startLevel, kMinStartLevel, kMaxStartLevel, "Start level: %d",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextDisabled("Marathon, Blitz, Zen");
    ImGui::Spacing();
    if (soundButton(menu.muted, width)) command = UiCommand::ToggleSound;
    if (musicButton(menu.musicOff, width)) command = UiCommand::ToggleMusic;
    if (menuButton("Controls", width)) command = UiCommand::OpenControls;
    if (menu.duelsEnabled) {
        ImGui::Spacing();
        ImGui::SeparatorText("1v1 DUEL");
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##name", "NAME", menu.nameEdit, sizeof menu.nameEdit,
                                 ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CharsNoBlank,
                                 nameCharFilter);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (menu.nameEdit[0] != '\0') {
                menu.playerName = menu.nameEdit;
                command = UiCommand::SaveName;
            } else {  // an empty name isn't allowed: put the saved one back
                std::snprintf(menu.nameEdit, sizeof menu.nameEdit, "%s", menu.playerName.c_str());
            }
        }
        if (!menu.inviteCode.empty()) {
            // Gold, so the invite stands out from the plate-blue buttons.
            ImGui::PushStyleColor(ImGuiCol_Button, colorVec(kKoPipColor));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colorVec(lighten(kKoPipColor)));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colorVec(darken(kKoPipColor)));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            const std::string label = "Join room " + menu.inviteCode;
            if (menuButton(label.c_str(), width, kKoPipColor)) command = UiCommand::JoinInvite;
            ImGui::PopStyleColor(4);
        }
        if (menuButton("Quick match", width)) command = UiCommand::QuickMatch;
        if (menuButton("Browse rooms", width)) command = UiCommand::BrowseRooms;
        // Create room and the Public box share one row.
        const float createWidth = width * 0.62f;
        if (menuButton("Create room", createWidth)) command = UiCommand::CreateRoom;
        ImGui::SameLine();
        if (ImGui::Checkbox("Public", &menu.publicRooms)) command = UiCommand::SavePublic;
        const bool complete = static_cast<int>(std::strlen(menu.joinCode)) == kRoomCodeLength;
        // The code box and Join room share one row, half each.
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        ImGui::SetNextItemWidth(half);
        const bool entered = ImGui::InputTextWithHint(
            "##code", "CODE", menu.joinCode, sizeof menu.joinCode,
            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(!complete);
        if ((menuButton("Join room", half) || entered) && complete) command = UiCommand::JoinRoom;
        ImGui::EndDisabled();
        if (!menu.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", menu.error.c_str());
    } else if (menuButton("Quit", width)) {  // a browser tab has nothing to quit
        command = UiCommand::QuitApp;
    }
    ImGui::End();
    return command;
}

int sdfSliderValue(int softDropFactor) { return softDropFactor == kSoftDropInstant ? kMaxSdf + 1 : softDropFactor; }

int sdfFromSlider(int value) { return value > kMaxSdf ? kSoftDropInstant : value; }

UiCommand drawControlsScreen(const Fonts& fonts, const Keymap& keymap, Handling& handling, ControlsMenu& controls) {
    UiCommand command = UiCommand::None;
    beginMenu("##controls");
    menuTitle(fonts, "CONTROLS");
    const float slotWidth = ImGui::CalcTextSize("Press a key...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float keysLeft = 0.0f;   // where the key columns start and end, window-relative, to line the sliders up
    float keysRight = 0.0f;
    if (ImGui::BeginTable("##keys", 1 + kKeySlots, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < kActionCount; ++i) {
            const auto action = static_cast<Key>(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(actionName(action));
            for (int slot = 0; slot < kKeySlots; ++slot) {
                ImGui::TableNextColumn();
                const bool waiting = controls.capturing == std::pair(action, slot);
                // The ### keeps one ImGui ID while the label changes, so keyboard focus stays put.
                const std::string label = std::string(waiting ? "Press a key..." : keyName(keymap.at(action, slot))) +
                                          "###bind" + std::to_string(i) + "_" + std::to_string(slot);
                if (ImGui::Button(label.c_str(), ImVec2(slotWidth, 0.0f))) {
                    controls.capturing = std::pair(action, slot);
                    controls.note.clear();
                }
                if (i == 0 && slot == 0) keysLeft = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x;
                if (i == 0 && slot == kKeySlots - 1) keysRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Esc cancels, Backspace clears");

    ImGui::Spacing();
    ImGui::SeparatorText("HANDLING");
    // Lined up with the key table: labels in its first column, sliders across both key columns.
    bool edited = false;
    auto slider = [&](const char* label, const char* id, int* value, int lo, int hi, const char* format) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(keysLeft);
        ImGui::SetNextItemWidth(keysRight - keysLeft);
        ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        edited |= ImGui::IsItemDeactivatedAfterEdit();
    };
    slider("DAS", "##das", &handling.dasMs, kMinDasMs, kMaxDasMs, "%d ms");
    slider("ARR", "##arr", &handling.arrMs, kMinArrMs, kMaxArrMs, handling.arrMs == 0 ? "0 ms (instant)" : "%d ms");
    int sdf = sdfSliderValue(handling.softDropFactor);
    slider("SDF", "##sdf", &sdf, kMinSdf, kMaxSdf + 1, sdf > kMaxSdf ? "Instant" : "%dx");
    handling.softDropFactor = sdfFromSlider(sdf);
    if (edited) command = UiCommand::SaveHandling;

    // The note line is always there, so the centered window doesn't jump when one appears.
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", controls.note.empty() ? " " : controls.note.c_str());
    ImGui::Spacing();
    if (menuButton("Reset to defaults")) command = UiCommand::ResetControls;
    if (menuButton("Back")) command = UiCommand::CloseControls;
    ImGui::End();
    return command;
}

void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo, const ClearCallout& callout) {
    const Layout layout = computeLayout();
    drawGridBackground(layout.cell, layout.origin);
    ImDrawList* dl = beginBoardWindow();
    drawPlayfield(dl, layout, game, fonts, combo, callout, true);
    ImGui::End();
}

UiCommand drawOverlay(const Game& game, const Fonts& fonts, bool muted, bool musicOff) {
    UiCommand command = UiCommand::None;
    if (game.state() == GameState::Paused) {
        beginMenu("##paused");
        menuTitle(fonts, "PAUSED");
        if (menuButton("Resume")) command = UiCommand::Resume;
        if (menuButton("Restart")) command = UiCommand::Restart;
        if (soundButton(muted)) command = UiCommand::ToggleSound;
        if (musicButton(musicOff)) command = UiCommand::ToggleMusic;
        if (menuButton("Controls")) command = UiCommand::OpenControls;
        if (menuButton("Quit to menu")) command = UiCommand::QuitToMenu;
        ImGui::End();
    } else if (game.state() == GameState::GameOver) {
        beginMenu("##gameover");
        menuTitle(fonts, "GAME OVER");
        ImGui::Text("Score: %d", game.score());
        if (menuButton("Restart")) command = UiCommand::Restart;
        if (menuButton("Quit to menu")) command = UiCommand::QuitToMenu;
        ImGui::End();
    }
    return command;
}

std::string formatClock(int ms) {
    ms = std::max(0, ms);
    char text[32];
    std::snprintf(text, sizeof text, "%d:%02d.%02d", ms / 60000, ms / 1000 % 60, ms / 10 % 100);
    return text;
}

std::string formatScore(int score) {
    std::string digits = std::to_string(score);
    const std::size_t first = digits.front() == '-' ? 1 : 0;
    for (std::size_t i = digits.size(); i > first + 3; i -= 3) digits.insert(i - 3, ",");
    return digits;
}

void drawSoloStatus(const SoloSession& solo, const Fonts& fonts) {
    if (solo.game().state() == GameState::Paused) return;  // hidden with the panels it sits under
    std::string first;
    std::string second;
    ImU32 firstColor = IM_COL32(230, 230, 240, 255);
    switch (solo.mode()) {
    case SoloMode::Sprint:
        first = formatClock(solo.clockMs());
        second = std::to_string(solo.progress()) + " / " + std::to_string(kSprintLines);
        break;
    case SoloMode::Cheese:
        first = formatClock(solo.clockMs());
        second = std::to_string(solo.progress()) + " left";
        break;
    case SoloMode::Blitz: {
        const int seconds = (solo.clockMs() + 999) / 1000;
        char text[16];
        std::snprintf(text, sizeof text, "%d:%02d", seconds / 60, seconds % 60);
        first = text;
        if (seconds < kTimeWarningSeconds) firstColor = IM_COL32(255, 70, 70, 255);
        break;
    }
    case SoloMode::Zen:
        first = "ZEN";
        break;
    case SoloMode::Marathon:
        return;
    }
    const Layout l = computeLayout();
    const float textSize = sideTextSize(l);
    const ImVec2 pos(l.origin.x + (kBoardWidth + 1) * l.cell, statsTop(l) + kSideStats * textSize * kStatPitch);
    ImDrawList* dl = beginBoardWindow();
    dl->AddText(fonts.hud, textSize, pos, firstColor, first.c_str());
    if (!second.empty()) drawLabel(dl, fonts, textSize, ImVec2(pos.x, pos.y + textSize * 1.4f), second.c_str());
    ImGui::End();
}

UiCommand drawSoloOverlay(const SoloSession& solo, const PersonalBests& bests, bool newBest, const Fonts& fonts,
                          bool muted, bool musicOff) {
    const SoloPhase phase = solo.phase();
    if (phase == SoloPhase::Countdown) {
        drawCountdown(solo.countdownMs(), fonts);
        return UiCommand::None;
    }
    if (!isTimed(solo.mode()) || (phase != SoloPhase::Finished && phase != SoloPhase::ToppedOut))
        return drawOverlay(solo.game(), fonts, muted, musicOff);

    const bool blitz = solo.mode() == SoloMode::Blitz;
    auto format = [blitz](int value) { return blitz ? formatScore(value) : formatClock(value); };
    UiCommand command = UiCommand::None;
    beginMenu("##soloresult");
    const float width = std::max(menuTitle(fonts, phase == SoloPhase::Finished ? "FINISHED" : "TOPPED OUT"),
                                 menuButtonWidth());
    // A top-out has no result: show how far the run got.
    const std::string result = format(solo.result().value_or(blitz ? solo.game().score() : solo.clockMs()));
    ImGui::PushFont(fonts.hud, ImGui::GetFontSize() * 2.5f);
    ImGui::TextUnformatted(result.c_str());
    ImGui::PopFont();
    if (newBest)
        ImGui::TextColored(colorVec(kKoPipColor), "NEW PB!");
    else if (const std::optional<int> best = bests.best(solo.mode()))
        ImGui::TextDisabled("PB %s", format(*best).c_str());
    ImGui::Spacing();
    if (menuButton("Retry", width) || ImGui::IsKeyPressed(ImGuiKey_R, false)) command = UiCommand::Restart;
    if (menuButton("Quit to menu", width)) command = UiCommand::QuitToMenu;
    ImGui::End();
    return command;
}

void drawMutedBadge(const Fonts& fonts) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float size = ImGui::GetFontSize();
    const char* text = "MUTED";
    const float width = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - width - size, viewport->WorkPos.y + size);
    drawLabel(ImGui::GetForegroundDrawList(), fonts, size, pos, text);
}

UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, const ClearCallout& callout,
                   const AttackTrails& trails, StartMenu& menu) {
    UiCommand command = UiCommand::None;
    switch (duel.phase()) {
    case DuelPhase::Idle:
        break;
    case DuelPhase::Lobby:
        command = drawLobby(duel, fonts, menu);
        break;
    case DuelPhase::Waiting:
        beginMenu("##waiting");
        if (duel.roomCode().empty()) {
            // A quick match may join straight into a countdown without ever getting a code.
            menuTitle(fonts, duel.quickMatching() ? "SEARCHING" : "CONNECTING");
        } else {
            menuTitle(fonts, "ROOM CODE");
            ImGui::PushFont(fonts.hud, ImGui::GetFontSize() * 3.0f);
            ImGui::TextUnformatted(duel.roomCode().c_str());
            ImGui::PopFont();
            ImGui::TextUnformatted(duel.quickMatching() ? "Looking for an opponent..."
                                                        : "Waiting for an opponent to join...");
            ImGui::TextUnformatted(duel.publicRoom() ? "Public room, listed in Browse rooms"
                                                     : "Private room, share the code");
            if (!menu.inviteLinkBase.empty()) {
                static char link[256];  // InputText needs a mutable buffer even when read-only
                std::snprintf(link, sizeof link, "%s%s", menu.inviteLinkBase.c_str(), duel.roomCode().c_str());
                ImGui::SetNextItemWidth(menuButtonWidth());
                // The one field left in the default font: a long URL in the pixel font is unreadable at this size.
                ImGui::PushFont(fonts.ui, 0.0f);
                ImGui::InputText("##link", link, sizeof link, ImGuiInputTextFlags_ReadOnly);
                ImGui::PopFont();
                if (menu.canCopyLink && menuButton("Copy link")) command = UiCommand::CopyInviteLink;
            }
        }
        if (menuButton("Cancel")) command = UiCommand::LeaveDuel;
        ImGui::End();
        break;
    case DuelPhase::Countdown:
        drawDuelBoards(duel, fonts, combo, callout, trails, menu);
        drawCountdown(duel.countdownMs(), fonts);
        drawVersus(fonts, menu.playerName, duel.opponentName());
        break;
    case DuelPhase::Playing:
        drawDuelBoards(duel, fonts, combo, callout, trails, menu);
        if (showGoBanner(duel.matchClockMs())) drawGo(fonts);
        break;
    case DuelPhase::Result:
        drawDuelBoards(duel, fonts, combo, callout, trails, menu);
        command = drawDuelResult(duel, fonts);
        break;
    }
    return command;
}

}  // namespace tetris
