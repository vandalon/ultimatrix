// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#ifdef UM_FOOTBALL_ONLY
#include <Settings.h>
#endif

namespace numericfont {
inline bool enabled() {
#ifdef UM_FOOTBALL_ONLY
  return Settings::getInstance()->numberFont == 1;
#else
  return false;
#endif
}

inline bool smallPixel(int digit, int x, int y) {
  static const uint8_t ROW_OF[7] = {0, 1, 1, 2, 3, 3, 4}, COL_OF[4] = {0, 1, 1, 2};
  static const uint8_t DIGITS[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
  };
  static const uint8_t ONE[7] = {2, 6, 2, 2, 2, 2, 7};
  if (x < 0 || x >= 4 || y < 0 || y >= 7) return false;
  return digit == 1 ? (ONE[y] >> (3 - x) & 1) : (DIGITS[digit][ROW_OF[y]] >> (2 - COL_OF[x]) & 1);
}

// The larger block digits use the same width for stems and horizontal bars.
inline uint8_t pixel(int digit, int x, int y, int w, int h) {
  static const uint8_t SEGMENTS[10] = {119, 36, 93, 109, 46, 107, 123, 37, 127, 111};
  int dw = h >= 20 ? w : (w < 10 ? w : 10), left = (w - dw) / 2;
  x -= left;
  if (x < 0 || x >= dw) return 0;
  int stroke = (dw + 2) / 4, middle = (h - stroke) / 2;
  if (digit == 1) {
    int stem = (dw - stroke + 1) / 2;
    bool flag = (y > 0 && y < stroke && x >= stem - y && x < stem) ||
                (y >= stroke && y < 2 * stroke - 1 && x >= stem - stroke && x < stem - 1 - (y - stroke));
    bool lit = (x >= stem && x < stem + stroke) || (y >= h - stroke && x >= 1) || flag;
    return lit ? 32 : 0;
  }
  uint8_t segments = SEGMENTS[digit];
  bool lit = ((segments & 1) && y < stroke) ||
             ((segments & 2) && x < stroke && y < middle + stroke) ||
             ((segments & 4) && x >= dw - stroke && y < middle + stroke) ||
             ((segments & 8) && y >= middle && y < middle + stroke) ||
             ((segments & 16) && x < stroke && y >= middle) ||
             ((segments & 32) && x >= dw - stroke && y >= middle) ||
             ((segments & 64) && y >= h - stroke);
  return lit ? 32 : 0;
}
}
