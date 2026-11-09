// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TetrisGameView.h"

namespace tetrisgame {

namespace {

// 3 x 5 glyphs, row by row; only what the clock needs: digits, the month capitals and L for the level
struct Glyph {
  char c;
  const char *rows;
};
const Glyph kGlyphs[] = {
    {'0', "111101101101111"}, {'1', "010110010010111"}, {'2', "111001111100111"}, {'3', "111001111001111"},
    {'4', "101101111001001"}, {'5', "111100111001111"}, {'6', "111100111101111"}, {'7', "111001001001001"},
    {'8', "111101111101111"}, {'9', "111101111001111"}, {'A', "010101111101101"}, {'B', "110101110101110"},
    {'C', "011100100100011"}, {'D', "110101101101110"}, {'E', "111100110100111"}, {'F', "111100110100100"},
    {'G', "011100101101111"}, {'J', "001001001101010"}, {'L', "100100100100111"}, {'M', "101111111101101"},
    {'N', "111101101101101"}, {'O', "111101101101111"}, {'P', "111101111100100"}, {'R', "110101110101101"},
    {'S', "011100010001110"}, {'T', "111010010010010"}, {'U', "101101101101111"}, {'V', "101101101101010"},
    {'Y', "101101010010010"}};
const char *const kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

constexpr int kCell = 3;      // panel pixels per board cell
constexpr int kBoardX = 1;    // board columns start here; borders on x 0, 31 and the bottom row
constexpr int kColumnX = 33;  // the side column

struct Painter {
  MatrixPanel_I2S_DMA &d;

  uint16_t rgb(uint8_t r, uint8_t g, uint8_t b, float k = 1.0f) const { return d.color565(r * k, g * k, b * k); }

  uint16_t piece(uint8_t colour, float k = 1.0f) const {
    static const uint8_t kRgb[8][3] = {{0, 0, 0},     {0, 220, 230}, {240, 220, 0}, {170, 60, 220},
                                       {60, 200, 60}, {230, 50, 50}, {50, 80, 230}, {240, 140, 20}};
    return rgb(kRgb[colour][0], kRgb[colour][1], kRgb[colour][2], k);
  }

  void glyph(char c, int x, int y, int scale, uint16_t colour) const {
    for (const Glyph &g : kGlyphs) {
      if (g.c != c) continue;
      for (int i = 0; i < 15; i++)
        if (g.rows[i] == '1') d.fillRect(x + (i % 3) * scale, y + (i / 3) * scale, scale, scale, colour);
      return;
    }
  }

  // Text at 3 x 5 or a multiple of it; one blank column between the letters
  void text(const char *s, int x, int y, int scale, uint16_t colour) const {
    for (; *s; s++, x += 3 * scale + 1) glyph(*s, x, y, scale, colour);
  }

  void cell(int cx, int cy, uint16_t colour) const {
    if (cy < 0) return;
    d.fillRect(kBoardX + cx * kCell, cy * kCell, kCell, kCell, colour);
  }
};

}  // namespace

void draw(MatrixPanel_I2S_DMA &display, const Game &game, const Clock &clock) {
  Painter p{display};
  display.fillScreen(0);

  const uint16_t frame = p.rgb(40, 40, 60);
  display.drawFastVLine(0, 0, 64, frame);
  display.drawFastVLine(31, 0, 64, frame);
  display.drawFastHLine(0, 63, 32, frame);

  const bool over = game.phase == Over;
  const bool dimmed = over && (game.phaseMs / 200) % 2 == 0;
  for (int row = 0; row < kRows; row++) {
    bool clearing = false;
    if (game.phase == Clearing)
      for (uint8_t i = 0; i < game.clearCount; i++) clearing = clearing || game.clearRows[i] == row;
    for (int col = 0; col < kCols; col++) {
      const uint8_t v = game.board[row][col];
      if (!v) continue;
      float k = 1.0f;
      if (clearing && !game.flashLit()) k = 0.15f;
      if (dimmed) k = 0.35f;
      p.cell(col, row, over ? p.rgb(200, 60, 60, k) : p.piece(v, k));
    }
  }
  if (game.phase == Falling) {
    int8_t c[4][2];
    Game::cells(game.type, game.rot, c);
    const int ly = game.landingY(game.type, game.rot, game.x, game.y);
    for (auto &q : c) p.cell(game.x + q[0], ly + q[1], p.piece(game.type + 1, 0.28f));
    for (auto &q : c) p.cell(game.x + q[0], game.y + q[1], p.piece(game.type + 1));
  }

  // Hours and minutes side by side, no colon: the colours tell them apart
  char buf[8];
  const uint16_t hours = p.rgb(210, 215, 255), minutes = p.rgb(255, 165, 70);
  snprintf(buf, sizeof(buf), "%d", clock.hour);
  p.text(buf, kColumnX + 1 + (clock.hour < 10 ? 7 : 0), 4, 2, hours);
  snprintf(buf, sizeof(buf), "%02d", clock.minute);
  p.text(buf, 50, 4, 2, minutes);

  // Date like 25 OCT, centred on the column
  const uint16_t dateColour = p.rgb(120, 200, 170);
  const int dayLen = clock.day > 9 ? 2 : 1;
  const int dateX = 48 - (dayLen * 4 - 1 + 4 + 11) / 2;
  snprintf(buf, sizeof(buf), "%d", clock.day);
  p.text(buf, dateX, 19, 1, dateColour);
  p.text(kMonths[(clock.month + 11) % 12], dateX + dayLen * 4 + 3, 19, 1, dateColour);

  // Next piece
  int8_t n[4][2];
  Game::cells(game.next, 0, n);
  for (auto &q : n) display.fillRect(42 + q[0] * kCell, 31 + q[1] * kCell, kCell, kCell, p.piece(game.next + 1));

  snprintf(buf, sizeof(buf), "%05lu", (unsigned long)(game.score > 99999 ? 99999 : game.score));
  p.text(buf, 38, 45, 1, p.rgb(240, 240, 240));
  const uint16_t levelColour = p.rgb(150, 150, 200);
  p.glyph('L', 41, 55, 1, levelColour);
  snprintf(buf, sizeof(buf), "%02d", game.level > 99 ? 99 : game.level);
  p.text(buf, 46, 55, 1, levelColour);
}

}  // namespace tetrisgame
