#include "app/render.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
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
Rgb lighten(Rgb c) { return {c.r + (255 - c.r) * 2 / 5, c.g + (255 - c.g) * 2 / 5, c.b + (255 - c.b) * 2 / 5}; }
Rgb darken(Rgb c) { return {c.r * 3 / 5, c.g * 3 / 5, c.b * 3 / 5}; }
constexpr Rgb kGarbageColor{128, 128, 136};
Rgb pieceRgb(PieceType type) {
    if (type == PieceType::Garbage || type == PieceType::Bomb) return kGarbageColor;
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

void drawCell(ImDrawList* dl, ImVec2 p, float size, PieceType type, float alpha) {
    const Rgb base = pieceRgb(type);
    drawBevelRect(dl, p, ImVec2(p.x + size, p.y + size), bevelFor(size), base, lighten(base), darken(base), alpha);
    if (type == PieceType::Bomb) {
        const int a = static_cast<int>(alpha * 255.0f);
        dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.55f), size * 0.28f, IM_COL32(25, 25, 32, a));
        dl->AddCircleFilled(ImVec2(p.x + size * 0.74f, p.y + size * 0.24f), std::max(1.0f, size * 0.09f),
                            IM_COL32(255, 140, 0, a));  // fuse
    }
}

void drawWell(ImDrawList* dl, const Layout& l) {
    const ImVec2 top = cellTopLeft(l, 0, kVisibleHeight - 1);
    const ImVec2 bottom(top.x + kBoardWidth * l.cell, top.y + kVisibleHeight * l.cell);
    dl->AddRectFilled(top, bottom, IM_COL32(18, 18, 26, 255));
    dl->AddRect(ImVec2(top.x - 2, top.y - 2), ImVec2(bottom.x + 2, bottom.y + 2), IM_COL32(110, 110, 140, 255),
                0.0f, 0, 2.0f);
}

void drawBoardCells(ImDrawList* dl, const Layout& l, const Game& game) {
    for (int y = 0; y < kDrawnRows; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = game.board().at(x, y)) drawCell(dl, cellTopLeft(l, x, y), l.cell, *c, rowAlpha(y));
    for (int y : game.clearingRows()) {
        if (y >= kDrawnRows) continue;
        const ImVec2 p = cellTopLeft(l, 0, y);
        dl->AddRectFilled(p, ImVec2(p.x + kBoardWidth * l.cell, p.y + l.cell), IM_COL32(255, 255, 255, 220));
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

void drawSidePanels(ImDrawList* dl, const Layout& l, const Game& game, const Fonts& fonts) {
    const float textSize = l.cell * 0.6f;
    const float mini = l.cell * 0.7f;
    const float top = cellTopLeft(l, 0, kVisibleHeight - 1).y;

    const float holdX = l.origin.x - 5.5f * l.cell;
    drawLabel(dl, fonts, textSize, ImVec2(holdX, top), "HOLD");
    if (const auto held = game.hold())
        drawMiniPiece(dl, ImVec2(holdX, top + l.cell), mini, *held, game.canHold() ? 1.0f : 0.35f);

    const float rightX = l.origin.x + (kBoardWidth + 1) * l.cell;
    drawLabel(dl, fonts, textSize, ImVec2(rightX, top), "NEXT");
    const std::vector<PieceType> next = game.next(5);
    for (int i = 0; i < static_cast<int>(next.size()); ++i)
        drawMiniPiece(dl, ImVec2(rightX, top + l.cell + i * 2.5f * mini), mini, next[i], 1.0f);

    float y = top + l.cell + 5 * 2.5f * mini + l.cell;
    const std::array<std::pair<const char*, int>, 3> stats{
        {{"SCORE", game.score()}, {"LEVEL", game.level()}, {"LINES", game.lines()}}};
    char value[32];
    for (const auto& [label, number] : stats) {
        drawLabel(dl, fonts, textSize, ImVec2(rightX, y), label);
        std::snprintf(value, sizeof value, "%d", number);
        drawLabel(dl, fonts, textSize, ImVec2(rightX, y + textSize * 1.4f), value);
        y += textSize * 3.5f;
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

// `comboUnderHold` is false in duels, where the combo sits in the center column instead.
void drawPlayfield(ImDrawList* dl, const Layout& layout, const Game& game, const Fonts& fonts,
                   const ComboLabel& combo, bool comboUnderHold) {
    drawWell(dl, layout);
    if (game.state() == GameState::Paused) return;
    drawBoardCells(dl, layout, game);
    if (game.state() == GameState::Playing) drawActivePiece(dl, layout, game);
    drawSidePanels(dl, layout, game, fonts);
    if (comboUnderHold) drawComboLabel(dl, layout, fonts, combo);
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
constexpr Rgb kGridColor{16, 24, 50};

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

// Red bar showing garbage waiting to rise, one cell tall per line, with its left edge at `x`.
void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending) {
    if (pending <= 0) return;
    const float height = static_cast<float>(std::min(pending, kVisibleHeight)) * cell;
    dl->AddRectFilled(ImVec2(x, bottom - height), ImVec2(x + 0.35f * cell, bottom), IM_COL32(230, 40, 40, 255));
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
    const ImU32 label = IM_COL32(150, 160, 190, 255);
    const ImU32 value = IM_COL32(235, 235, 245, 255);
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

// The opponent's well at full size, then SCORE and LINES; the snapshot carries no hold or next.
void drawOpponent(ImDrawList* dl, const DuelLayout& layout, const Snapshot& snapshot, const Fonts& fonts) {
    const Layout& l = layout.theirs;
    drawWell(dl, l);
    for (int y = 0; y < kVisibleHeight; ++y)
        for (int x = 0; x < kBoardWidth; ++x)
            if (const Cell c = snapshot.cells[y * kBoardWidth + x]) drawCell(dl, cellTopLeft(l, x, y), l.cell, *c, 1.0f);

    const float statsX = layout.left + kStatsCol * l.cell;
    const float textSize = l.cell * 0.6f;
    float y = wellTop(l);
    const std::array<std::pair<const char*, int>, 2> stats{{{"SCORE", snapshot.score}, {"LINES", snapshot.lines}}};
    char value[32];
    for (const auto& [label, number] : stats) {
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y), label);
        std::snprintf(value, sizeof value, "%d", number);
        drawLabel(dl, fonts, textSize, ImVec2(statsX, y + textSize * 1.4f), value);
        y += textSize * 3.5f;
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

float menuButtonWidth() { return ImGui::GetFontSize() * 12.0f; }

bool menuButton(const char* label, float width = menuButtonWidth()) {
    return ImGui::Button(label, ImVec2(width, 0.0f));
}

// The ### keeps one ImGui ID while the label flips, so keyboard focus stays on the item.
bool soundButton(bool muted, float width = menuButtonWidth()) {
    return menuButton(muted ? "Sound: Off###sound" : "Sound: On###sound", width);
}
bool musicButton(bool off, float width = menuButtonWidth()) {
    return menuButton(off ? "Music: Off###music" : "Music: On###music", width);
}

void drawDuelBoards(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, const StartMenu& menu) {
    const Game& game = *duel.game();
    const char* mine = menu.playerName.empty() ? "YOU" : menu.playerName.c_str();
    const char* theirs = duel.opponentName().empty() ? "FOE" : duel.opponentName().c_str();
    const DuelLayout layout = computeDuelLayout();
    const float cell = layout.mine.cell;
    drawGridBackground(cell, layout.mine.origin);
    ImDrawList* dl = beginBoardWindow();
    drawNameBar(dl, fonts, layout.mine, layout.top, mine, kMyPlateColor, duel.kosMine());
    drawPlayfield(dl, layout.mine, game, fonts, combo, false);
    drawGarbageMeter(dl, layout.left + kMyMeterCol * cell, wellBottom(layout.mine), cell, game.pendingGarbage());
    drawScoreboard(dl, fonts, layout, duel, combo);
    drawGarbageMeter(dl, layout.left + kTheirMeterCol * cell, wellBottom(layout.theirs), cell,
                     duel.opponent().pending);
    drawNameBar(dl, fonts, layout.theirs, layout.top, theirs, kTheirPlateColor, duel.kosTheirs());
    drawOpponent(dl, layout, duel.opponent(), fonts);
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
    const float width = std::max(menuTitle(fonts, "OPEN ROOMS"), menuButtonWidth());
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

void ComboLabel::update(int combo, int ms) {
    if (combo >= 1) {
        shown = combo;
        fadeMs = kComboFadeMs;
        return;
    }
    fadeMs = std::max(0, fadeMs - ms);
    if (fadeMs == 0) shown = 0;
}

UiCommand drawStartScreen(const Fonts& fonts, StartMenu& menu) {
    UiCommand command = UiCommand::None;
    beginMenu("##start");
    // Every row spans the title's width, so the menu is one even column under it.
    const float width = std::max(menuTitle(fonts, "JESTRIS BATTLE"), menuButtonWidth());
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt("##level", &menu.startLevel, kMinStartLevel, kMaxStartLevel, "Start level: %d",
                     ImGuiSliderFlags_AlwaysClamp);
    if (menuButton(menu.duelsEnabled ? "Solo" : "Play", width)) command = UiCommand::Play;
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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
            const std::string label = "Join room " + menu.inviteCode;
            if (menuButton(label.c_str(), width)) command = UiCommand::JoinInvite;
            ImGui::PopStyleColor();
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
        if ((ImGui::Button("Join room", ImVec2(half, 0.0f)) || entered) && complete) command = UiCommand::JoinRoom;
        ImGui::EndDisabled();
        if (!menu.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", menu.error.c_str());
    } else if (menuButton("Quit", width)) {  // a browser tab has nothing to quit
        command = UiCommand::QuitApp;
    }
    ImGui::End();
    return command;
}

UiCommand drawControlsScreen(const Fonts& fonts, const Keymap& keymap, ControlsMenu& controls) {
    UiCommand command = UiCommand::None;
    beginMenu("##controls");
    menuTitle(fonts, "CONTROLS");
    const float slotWidth = ImGui::CalcTextSize("Press a key...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
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
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Esc cancels, Backspace clears");
    // The note line is always there, so the centered window doesn't jump when one appears.
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", controls.note.empty() ? " " : controls.note.c_str());
    ImGui::Spacing();
    if (menuButton("Reset to defaults")) command = UiCommand::ResetControls;
    if (menuButton("Back")) command = UiCommand::CloseControls;
    ImGui::End();
    return command;
}

void drawGame(const Game& game, const Fonts& fonts, const ComboLabel& combo) {
    const Layout layout = computeLayout();
    drawGridBackground(layout.cell, layout.origin);
    ImDrawList* dl = beginBoardWindow();
    drawPlayfield(dl, layout, game, fonts, combo, true);
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

void drawMutedBadge(const Fonts& fonts) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float size = ImGui::GetFontSize();
    const char* text = "MUTED";
    const float width = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - width - size, viewport->WorkPos.y + size);
    drawLabel(ImGui::GetForegroundDrawList(), fonts, size, pos, text);
}

UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, StartMenu& menu) {
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
                ImGui::InputText("##link", link, sizeof link, ImGuiInputTextFlags_ReadOnly);
                if (menu.canCopyLink && menuButton("Copy link")) command = UiCommand::CopyInviteLink;
            }
        }
        if (menuButton("Cancel")) command = UiCommand::LeaveDuel;
        ImGui::End();
        break;
    case DuelPhase::Countdown:
        drawDuelBoards(duel, fonts, combo, menu);
        drawCountdown(duel.countdownMs(), fonts);
        drawVersus(fonts, menu.playerName, duel.opponentName());
        break;
    case DuelPhase::Playing:
        drawDuelBoards(duel, fonts, combo, menu);
        if (showGoBanner(duel.matchClockMs())) drawGo(fonts);
        break;
    case DuelPhase::Result:
        drawDuelBoards(duel, fonts, combo, menu);
        command = drawDuelResult(duel, fonts);
        break;
    }
    return command;
}

}  // namespace tetris
