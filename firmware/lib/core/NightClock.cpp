// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NightClock.h"
#include "NumericFont.h"
#include "PanelLayout.h"
#ifdef UM_PACMAN
#include "Settings.h"
#endif

namespace {
void digit(MatrixPanel_I2S_DMA &gfx, int value, int left, int top, int w, int h, uint16_t colour) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (numericfont::pixel(value, x, y, w, h)) gfx.drawPixel(left + x, top + y, colour);
}
}

void NightClock::draw(MatrixPanel_I2S_DMA &gfx, int hour, int minute, bool use24h) {
  int key = hour * 60 + minute + (use24h ? 0 : 1440);
  uint16_t colour = 0x3800;  // dark red: full red is far too bright in the dark
#ifdef UM_PACMAN
  const auto &settings = *Settings::getInstance();
  colour = settings.nightColor();
  key += settings.color * 4096;  // a live colour change also redraws within the same minute
#endif
  if (key == _shown) return;
  _shown = key;
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
