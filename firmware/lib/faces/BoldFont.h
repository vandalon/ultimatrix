// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The fonts of the arcade faces: bold capitals 7 px tall with 2 px strokes (5 wide; Y and T are 6, I is 2), the block digits of
// the alternative number font at any size (with an even-footed 1), and the thick 12 x 22 digits of the Pong clock. Header only.
#pragma once

#include "FaceKit.h"
#include "NumericFont.h"
#include <string.h>

namespace facekit {

struct BoldGlyph { char c; const char *rows[7]; };
inline const BoldGlyph *boldGlyph(char c) {
  static const BoldGlyph GLYPHS[] = {
    {'C', {".XXX.", "XXXXX", "XX...", "XX...", "XX...", "XXXXX", ".XXX."}},
    {'O', {".XXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}},
    {'U', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", ".XXX."}},
    {'Y', {"XX..XX", "XX..XX", ".XXXX.", "..XX..", "..XX..", "..XX..", "..XX.."}},
    {'M', {"XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XX.XX"}},
    {'P', {"XXXX.", "XXXXX", "XX.XX", "XXXXX", "XXXX.", "XX...", "XX..."}},
    {'E', {"XXXXX", "XXXXX", "XX...", "XXXX.", "XX...", "XXXXX", "XXXXX"}},
    {'R', {"XXXX.", "XXXXX", "XX.XX", "XXXX.", "XXXX.", "XX.XX", "XX.XX"}},
    {'T', {"XXXXXX", "XXXXXX", "..XX..", "..XX..", "..XX..", "..XX..", "..XX.."}},
    {'W', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "XX.XX"}},
    {'I', {"XX", "XX", "XX", "XX", "XX", "XX", "XX"}},
    {'N', {"XX..XX", "XXX.XX", "XXXXXX", "XX.XXX", "XX..XX", "XX..XX", "XX..XX"}},
    {'S', {".XXXX", "XXXXX", "XX...", ".XXX.", "...XX", "XXXXX", "XXXX."}},
    {'L', {"XX...", "XX...", "XX...", "XX...", "XX...", "XXXXX", "XXXXX"}},
    {'G', {".XXX.", "XXXXX", "XX...", "XX.XX", "XX.XX", "XXXXX", ".XXX."}},
    {'A', {".XXX.", "XXXXX", "XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX"}},
    {'V', {"XX.XX", "XX.XX", "XX.XX", "XX.XX", "XX.XX", ".XXX.", "..X.."}},
    {'D', {"XXXX.", "XXXXX", "XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXX."}},
    {'H', {"XX.XX", "XX.XX", "XX.XX", "XXXXX", "XXXXX", "XX.XX", "XX.XX"}},
  };
  for (const BoldGlyph &g : GLYPHS) if (g.c == c) return &g;
  return nullptr;
}
inline int boldWidth(const char *text) {   // of the first line
  int w = 0;
  for (; *text && *text != '\n'; text++) {
    const BoldGlyph *g = boldGlyph(*text);
    w += (g ? (int)strlen(g->rows[0]) : 3) + 1;
  }
  return w > 0 ? w - 1 : 0;
}
inline void boldText(MatrixPanel_I2S_DMA &d, const char *text, int x, int y, uint16_t colour) {
  for (; *text && *text != '\n'; text++) {
    const BoldGlyph *g = boldGlyph(*text);
    if (!g) { x += 4; continue; }
    for (int row = 0; row < 7; row++)
      for (int col = 0; g->rows[row][col]; col++)
        if (g->rows[row][col] == 'X') put(d, x + col, y + row, colour);
    x += (int)strlen(g->rows[0]) + 1;
  }
}

// A digit of the alternative number font, w x h pixels (2 px strokes from 6 x 10 up); the 1's foot reaches as far left as right
inline void blockDigit(MatrixPanel_I2S_DMA &d, int value, int x, int y, int w, int h, uint16_t colour) {
  const int stroke = (w + 2) / 4;
  for (int row = 0; row < h; row++)
    for (int col = 0; col < w; col++)
      if (numericfont::pixel(value, col, row, w, h) || (value == 1 && col == 0 && row >= h - stroke)) put(d, x + col, y + row, colour);
}

// A number in block digits, pitch w + 1; returns the width
inline int blockNumber(MatrixPanel_I2S_DMA &d, long value, int x, int y, int w, int h, uint16_t colour, bool measureOnly = false) {
  char text[16];
  snprintf(text, sizeof(text), "%ld", value < 0 ? 0 : value);
  const int n = strlen(text);
  if (!measureOnly) for (int i = 0; i < n; i++) blockDigit(d, text[i] - '0', x + i * (w + 1), y, w, h, colour);
  return n * (w + 1) - 1;
}

// The thick 12 x 22 digits (4 px strokes, one pixel fatter each way than an 11 x 21 digit) and a matching L, as row bitmasks built once
inline const uint16_t *thickRows(int glyph) {   // 0..9 are digits, 10 is the L
  static uint16_t rows[11][22];
  static bool ready = false;
  if (!ready) {
    for (int value = 0; value < 10; value++) {
      auto lit = [&](int px, int py) { return px >= 0 && py >= 0 && py < 21 && (numericfont::pixel(value, px, py, 11, 21) || (value == 1 && px == 0 && py >= 18) || (value == 1 && px <= 2 && py < 7 && numericfont::pixel(1, px + 1, py, 11, 21))); };
      for (int row = 0; row < 22; row++) {
        uint16_t mask = 0;
        for (int col = 0; col < 12; col++)
          if (lit(col, row) || lit(col - 1, row) || lit(col, row - 1) || lit(col - 1, row - 1)) mask |= 1 << (11 - col);
        rows[value][row] = mask;
      }
    }
    for (int row = 0; row < 22; row++) rows[10][row] = row >= 18 ? 0xFFF : 0xF00;
    ready = true;
  }
  return rows[glyph];
}
inline void thickGlyph(MatrixPanel_I2S_DMA &d, int glyph, int x, int y, uint16_t colour) {   // as greedy rectangles
  uint16_t left[22];
  memcpy(left, thickRows(glyph), sizeof(left));
  for (int row = 0; row < 22; row++)
    for (int col = 0; col < 12;) {
      if (!(left[row] >> (11 - col) & 1)) { col++; continue; }
      const int start = col;
      uint16_t run = 0;
      while (col < 12 && (left[row] >> (11 - col) & 1)) { run |= 1 << (11 - col); col++; }
      int height = 1;
      while (row + height < 22 && (left[row + height] & run) == run) { left[row + height] &= ~run; height++; }
      left[row] &= ~run;
      box(d, x + start, y + row, col - start, height, colour);
    }
}

}  // namespace facekit
