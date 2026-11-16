// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NightClock.h"
#include "NumericFont.h"
#include "PanelLayout.h"
#include "Settings.h"

namespace {
void digit(MatrixPanel_I2S_DMA &gfx, int value, int left, int top, int w, int h, uint16_t colour) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (numericfont::pixel(value, x, y, w, h)) gfx.drawPixel(left + x, top + y, colour);
}
}

// The time in binary: four columns (hour tens/ones, minute tens/ones), rows 8-4-2-1, a lit dot is a filled disc, an unlit one an empty ring, both in the night colour
static void binaryDraw(MatrixPanel_I2S_DMA &gfx, int hour, int minute, uint16_t colour) {
  const bool tall = layout::height >= 64;
  const int dot = tall ? 7 : 4, pitchX = tall ? 9 : 6, pitchY = tall ? 11 : 6, gap = 4;
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  const int total = 4 * pitchX + gap - (pitchX - dot);
  const int left = (layout::width - total) / 2, top = (layout::height - (3 * pitchY + dot)) / 2;
  gfx.fillScreen(0);
  for (int c = 0; c < 4; ++c) {
    const int x = left + c * pitchX + (c >= 2 ? gap : 0);
    for (int r = 0; r < 4; ++r) {
      const bool lit = (digits[c] >> (3 - r)) & 1;
      const int cut = dot >= 7 ? 2 : 1;
      for (int j = 0; j < dot; ++j)
        for (int i = 0; i < dot; ++i) {
          const int corner = min(i, dot - 1 - i) + min(j, dot - 1 - j);
          if (corner < cut) continue;
          const bool edge = i == 0 || j == 0 || i == dot - 1 || j == dot - 1 || corner == cut;
          if (lit || edge) gfx.drawPixel(x + i, top + r * pitchY + j, colour);   // lit: a filled disc, unlit: an empty ring (dimming this low does not work)
        }
    }
  }
  gfx.flipDMABuffer();
}

void NightClock::draw(MatrixPanel_I2S_DMA &gfx, int hour, int minute, bool use24h) {
  int key = hour * 60 + minute + (use24h ? 0 : 1440);
  uint16_t colour = 0x3000;  // dark red: full red is far too bright in the dark (one step below the old 0x3800: the 8 MHz panel clock lights the dimmest levels a little more)
#ifdef UM_PACMAN
  const auto &settings = *Settings::getInstance();
  colour = settings.nightColor();
  key += settings.color * 4096;  // a live colour change also redraws within the same minute
#endif
  if (Settings::getInstance()->nightBinary) key += 1 << 20;
  if (key == _shown) return;
  _shown = key;
  if (Settings::getInstance()->nightBinary) { binaryDraw(gfx, hour, minute, colour); return; }
  const bool tall = layout::height >= 64;
  const int w = tall ? 11 : 8, h = tall ? 24 : 16, gap = 2, colon = tall ? 8 : 6, stroke = (w + 2) / 4;
  const int total = 4 * w + 2 * gap + colon;
  const int left = (layout::width - total) / 2, top = (layout::height - h) / 2;
  gfx.fillScreen(0);
  if (use24h || hour > 9) digit(gfx, hour / 10, left, top, w, h, colour);  // a 12-hour clock has no leading zero
  digit(gfx, hour % 10, left + w + gap, top, w, h, colour);
  const int colonX = left + 2 * w + gap + (colon - stroke) / 2;
  gfx.fillRect(colonX, top + h / 3 - stroke / 2, stroke, stroke, colour);
  gfx.fillRect(colonX, top + 2 * h / 3 - stroke / 2, stroke, stroke, colour);
  const int minutesX = left + 2 * w + gap + colon;
  digit(gfx, minute / 10, minutesX, top, w, h, colour);
  digit(gfx, minute % 10, minutesX + w + gap, top, w, h, colour);
  gfx.flipDMABuffer();
}
