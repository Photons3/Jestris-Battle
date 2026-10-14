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

void drawCell(ImDrawList* dl, ImVec2 p, float size, PieceType type, float alpha) {
    const Rgb base = pieceRgb(type);
    const float bevel = std::max(1.0f, std::floor(size * 0.12f));
    const ImVec2 q(p.x + size, p.y + size);
    dl->AddRectFilled(p, q, color(base, alpha));
    dl->AddRectFilled(p, ImVec2(q.x, p.y + bevel), color(lighten(base), alpha));  // top
    dl->AddRectFilled(p, ImVec2(p.x + bevel, q.y), color(lighten(base), alpha));  // left
    dl->AddRectFilled(ImVec2(p.x, q.y - bevel), q, color(darken(base), alpha));   // bottom
    dl->AddRectFilled(ImVec2(q.x - bevel, p.y), q, color(darken(base), alpha));   // right
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

void drawPlayfield(ImDrawList* dl, const Layout& layout, const Game& game, const Fonts& fonts,
                   const ComboLabel& combo) {
    drawWell(dl, layout);
    if (game.state() == GameState::Paused) return;
    drawBoardCells(dl, layout, game);
    if (game.state() == GameState::Playing) drawActivePiece(dl, layout, game);
    drawSidePanels(dl, layout, game, fonts);
    drawComboLabel(dl, layout, fonts, combo);
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

// Red bar showing garbage waiting to rise, one cell tall per line, with its left edge at `x`.
void drawGarbageMeter(ImDrawList* dl, float x, float bottom, float cell, int pending) {
    if (pending <= 0) return;
    const float height = static_cast<float>(std::min(pending, kVisibleHeight)) * cell;
    dl->AddRectFilled(ImVec2(x, bottom - height), ImVec2(x + 0.35f * cell, bottom), IM_COL32(230, 40, 40, 255));
}

// ImGui has no radial gradient, so stacked translucent discs fake one over a dark navy fill.
void drawDuelBackground() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 10, 28, 255));
    constexpr int kRings = 16;
    const float radius = std::max(vp->Size.x, vp->Size.y) * 0.6f;
    for (int i = 0; i < kRings; ++i)
        bg->AddCircleFilled(vp->GetCenter(), radius * (1.0f - static_cast<float>(i) / kRings),
                            IM_COL32(30, 50, 110, 14));
}

void drawNameBar(ImDrawList* dl, const Fonts& fonts, const Layout& well, float top, const char* name, ImU32 from,
                 ImU32 to, int kos) {
    const float cell = well.cell;
    const ImVec2 p0(well.origin.x, top);
    const ImVec2 p1(well.origin.x + kBoardWidth * cell, top + 1.1f * cell);
    dl->AddRectFilledMultiColor(p0, p1, from, to, to, from);
    const float textSize = cell * 0.7f;
    dl->AddText(fonts.hud, textSize, ImVec2(p0.x + 0.3f * cell, p0.y + (p1.y - p0.y - textSize) * 0.5f),
                IM_COL32(255, 255, 255, 255), name);
    const float radius = cell * 0.25f;
    for (int i = 0; i < kKosToWin; ++i) {
        const ImVec2 center(p1.x - cell * (0.5f + 0.8f * static_cast<float>(kKosToWin - 1 - i)), (p0.y + p1.y) * 0.5f);
        if (i < kos)
            dl->AddCircleFilled(center, radius, IM_COL32(255, 220, 60, 255));
        else
            dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 200), 0, 1.5f);
    }
}

void drawCentered(ImDrawList* dl, const Fonts& fonts, float size, float centerX, float y, const char* text,
                  ImU32 col) {
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(fonts.hud, size, ImVec2(centerX - extent.x * 0.5f, y), col, text);
}

// TIME, VS, KO and SENT, with your number on the left.
void drawScoreboard(ImDrawList* dl, const Fonts& fonts, const DuelLayout& layout, const DuelSession& duel) {
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
    drawCentered(dl, fonts, cell * 1.4f, cx, y, "VS", IM_COL32(255, 200, 60, 255));
    y += cell * 3.5f;
    std::snprintf(text, sizeof text, "%d : %d", duel.kosMine(), duel.kosTheirs());
    drawCentered(dl, fonts, cell * 0.5f, cx, y, "KO", label);
    drawCentered(dl, fonts, cell * 0.8f, cx, y + cell * 0.8f, text, value);
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

// A large yellow "KO!" with an orange glow, centered on the well.
void drawKoFlash(ImDrawList* dl, const Fonts& fonts, const Layout& well) {
    const char* text = "KO!";
    const float size = well.cell * 3.0f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pos(well.origin.x + (kBoardWidth * well.cell - extent.x) * 0.5f,
                     (wellTop(well) + wellBottom(well) - extent.y) * 0.5f);
    const float glow = std::max(1.0f, std::floor(well.cell * 0.15f));
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx != 0 || dy != 0)
                dl->AddText(fonts.hud, size, ImVec2(pos.x + dx * glow, pos.y + dy * glow), IM_COL32(255, 120, 0, 200),
                            text);
    dl->AddText(fonts.hud, size, pos, IM_COL32(255, 230, 40, 255), text);
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
    drawDuelBackground();
    ImDrawList* dl = beginBoardWindow();
    drawNameBar(dl, fonts, layout.mine, layout.top, mine, IM_COL32(40, 110, 255, 255), IM_COL32(150, 60, 230, 255),
                duel.kosMine());
    drawPlayfield(dl, layout.mine, game, fonts, combo);
    drawGarbageMeter(dl, layout.left + kMyMeterCol * cell, wellBottom(layout.mine), cell, game.pendingGarbage());
    drawScoreboard(dl, fonts, layout, duel);
    drawGarbageMeter(dl, layout.left + kTheirMeterCol * cell, wellBottom(layout.theirs), cell,
                     duel.opponent().pending);
    drawNameBar(dl, fonts, layout.theirs, layout.top, theirs, IM_COL32(230, 40, 40, 255), IM_COL32(255, 150, 30, 255),
                duel.kosTheirs());
    drawOpponent(dl, layout, duel.opponent(), fonts);
    if (duel.koFlashMs() > 0) drawKoFlash(dl, fonts, layout.mine);
    ImGui::End();
}

void drawCountdown(int ms, const Fonts& fonts) {
    char text[8];
    std::snprintf(text, sizeof text, "%d", (ms + 999) / 1000);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float size = vp->Size.y * 0.15f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    ImGui::GetForegroundDrawList()->AddText(
        fonts.hud, size,
        ImVec2(vp->Pos.x + (vp->Size.x - extent.x) * 0.5f, vp->Pos.y + (vp->Size.y - extent.y) * 0.5f),
        IM_COL32(255, 255, 255, 255), text);
}

// "Sam vs Kai" under the countdown digit; nothing until both names are known.
void drawVersus(const Fonts& fonts, const std::string& mine, const std::string& theirs) {
    if (mine.empty() || theirs.empty()) return;
    const std::string text = mine + " vs " + theirs;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float size = vp->Size.y * 0.04f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    ImGui::GetForegroundDrawList()->AddText(
        fonts.hud, size, ImVec2(vp->Pos.x + (vp->Size.x - extent.x) * 0.5f, vp->Pos.y + vp->Size.y * 0.62f),
        IM_COL32(255, 255, 255, 230), text.c_str());
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

}  // namespace

void configureImGuiIo(ImGuiIO& io) {
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

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
        if (menuButton("Create room", width)) command = UiCommand::CreateRoom;
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
    ImDrawList* dl = beginBoardWindow();
    drawPlayfield(dl, computeLayout(), game, fonts, combo);
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

UiCommand drawDuel(const DuelSession& duel, const Fonts& fonts, const ComboLabel& combo, const StartMenu& menu) {
    UiCommand command = UiCommand::None;
    switch (duel.phase()) {
    case DuelPhase::Idle:
        break;
    case DuelPhase::Waiting:
        beginMenu("##waiting");
        if (duel.roomCode().empty()) {
            menuTitle(fonts, "CONNECTING");
        } else {
            menuTitle(fonts, "ROOM CODE");
            ImGui::PushFont(fonts.hud, ImGui::GetFontSize() * 3.0f);
            ImGui::TextUnformatted(duel.roomCode().c_str());
            ImGui::PopFont();
            ImGui::TextUnformatted("Waiting for an opponent to join...");
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
        break;
    case DuelPhase::Result:
        drawDuelBoards(duel, fonts, combo, menu);
        command = drawDuelResult(duel, fonts);
        break;
    }
    return command;
}

}  // namespace tetris
