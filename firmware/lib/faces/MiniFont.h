// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// A 3x5 pixel font of capitals, digits and a few marks, for the small print of the pixel-art faces. Header only.
#pragma once

#include "FaceKit.h"

namespace facekit {

// Three bits per row, left pixel = bit 2; five rows per glyph
inline const uint8_t *miniGlyph(char c) {
  static const uint8_t DIGITS[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
                                        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,2,2},{7,5,7,5,7},{7,5,7,1,7}};
  static const uint8_t LETTERS[26][5] = {{2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},
                                         {3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},
                                         {5,7,7,5,5},{7,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,7,3},{6,5,6,5,5},
                                         {3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
                                         {5,5,2,2,2},{7,1,2,4,7}};
  static const uint8_t BLANK[5] = {0,0,0,0,0}, DOT[5] = {0,0,0,0,2}, DASH[5] = {0,0,7,0,0}, SLASH[5] = {1,1,2,4,4},
                       COLON[5] = {0,2,0,2,0}, BANG[5] = {2,2,2,0,2}, PERCENT[5] = {5,1,2,4,5};
  if (c >= '0' && c <= '9') return DIGITS[c - '0'];
  if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'];
  if (c >= 'a' && c <= 'z') return LETTERS[c - 'a'];
  switch (c) {
    case '.': return DOT;
    case '-': return DASH;
    case '/': return SLASH;
    case ':': return COLON;
    case '!': return BANG;
    case '%': return PERCENT;
    default: return BLANK;
  }
}

inline int miniWidth(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n ? n * 4 - 1 : 0;
}

inline void miniText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t color) {
  for (; *s; s++, x += 4) {
    const uint8_t *g = miniGlyph(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g[row] >> (2 - col) & 1) put(d, x + col, y + row, color);
  }
}

inline void miniCentered(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t color) {
  miniText(d, s, (W - miniWidth(s)) / 2, y, color);
}

}  // namespace facekit
