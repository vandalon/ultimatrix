// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"

void Clockface::setup(ClockTime *) {
  _started = millis();
  _reported = _started;
  _display->setRotation(0);
  const uint32_t clearAt = micros();
  for (int frame = 0; frame < 32; ++frame) _display->fillScreenRGB888(0, 0, 0);
  const uint32_t clearUs = (micros() - clearAt) / 32;
  const uint32_t fillAt = micros();
  for (int frame = 0; frame < 32; ++frame) _display->fillScreenRGB888(64, 128, 192);
  const uint32_t fillUs = (micros() - fillAt) / 32;
  Serial.printf("[Scanner] draw benchmark: clear=%u us, fill=%u us\n", (unsigned)clearUs, (unsigned)fillUs);
  benchmark();
}

// Fastest of several full frames, so Wi-Fi and other tasks running at the same time do not decide the result.
void Clockface::benchmark() {
  uint32_t best = UINT32_MAX;
  for (int run = 0; run < 12; ++run) {
    const uint32_t at = micros();
    for (int y = 0; y < _display->height(); ++y)
      for (int x = 0; x < _display->width(); ++x)
        _display->drawPixelRGB888(x, y, x * 4, y * 4, (x ^ y) * 4);
    const uint32_t us = micros() - at;
    if (us < best) best = us;
  }
  uint32_t floorUs = UINT32_MAX; // same loop, but every pixel is rejected by the bounds check: the cost around the write
  for (int run = 0; run < 12; ++run) {
    const uint32_t at = micros();
    for (int y = 0; y < _display->height(); ++y)
      for (int x = 0; x < _display->width(); ++x)
        _display->drawPixelRGB888(x + 1000, y, x * 4, y * 4, (x ^ y) * 4);
    const uint32_t us = micros() - at;
    if (us < floorUs) floorUs = us;
  }
  Serial.printf("[Scanner] RGB888 frame, fastest of 12: %u us (rejected pixels: %u us)\n", (unsigned)best, (unsigned)floorUs);
}

#ifdef UM_PATTERN
// Static test card for judging banding, tints and ghosting by eye or camera (build with UM_PATTERN=1 set in the environment).
// It goes through the normal brightness path, so it shows what the clock shows at the configured brightness.
#ifdef UM_PATTERN_EDGE
// Edge card (UM_PATTERN=2): lit pixels and lines on the outermost rows/columns with black around them, plus a centre block, to see stray light on the edges.
static void drawPattern(MatrixPanel_I2S_DMA *d) {
  d->fillScreenRGB888(0, 0, 0);
  for (int i = 8; i < 56; i += 12) {
    d->drawPixelRGB888(0, i, 255, 255, 255);   // left edge column
    d->drawPixelRGB888(63, i + 4, 255, 255, 255);  // right edge column
    d->drawPixelRGB888(i + 4, 0, 255, 255, 255);   // top row
    d->drawPixelRGB888(i, 63, 255, 255, 255);  // bottom row
  }
  d->fillRect(0, 24, 1, 8, 255, 255, 255);    // short line down the left edge
  d->fillRect(63, 32, 1, 8, 255, 255, 255);   // short line down the right edge
  d->fillRect(24, 0, 8, 1, 255, 255, 255);    // short line along the top
  d->fillRect(32, 63, 8, 1, 255, 255, 255);   // short line along the bottom
  d->fillRect(26, 26, 12, 12, 255, 255, 255); // bright centre block
  d->drawPixelRGB888(0, 0, 255, 0, 0);        // corners: red, green, blue, white
  d->drawPixelRGB888(63, 0, 0, 255, 0);
  d->drawPixelRGB888(0, 63, 0, 0, 255);
  d->drawPixelRGB888(63, 63, 255, 255, 255);
}
#else
static void drawPattern(MatrixPanel_I2S_DMA *d) {
  d->fillScreenRGB888(0, 0, 0);
  for (int x = 0; x < 64; ++x) {
    for (int y = 0; y < 8; ++y) d->drawPixelRGB888(x, y, x, x, x);                // dark ramp, one level per column (0..63)
    for (int y = 8; y < 16; ++y) d->drawPixelRGB888(x, y, x * 4, x * 4, x * 4);   // full grey ramp
    for (int y = 16; y < 20; ++y) d->drawPixelRGB888(x, y, x * 4, 0, 0);          // red ramp
    for (int y = 20; y < 24; ++y) d->drawPixelRGB888(x, y, 0, x * 4, 0);          // green ramp
    for (int y = 24; y < 28; ++y) d->drawPixelRGB888(x, y, 0, 0, x * 4);          // blue ramp
  }
  // White at very low levels (4, 8, 16, 32): the yellow-tint case
  const uint8_t low[4] = {4, 8, 16, 32};
  for (int i = 0; i < 4; ++i) d->fillRect(i * 16, 28, 16, 4, low[i], low[i], low[i]);

  // Ghosting on black: single pixels, short lines, a block and a dense checker, every one with black all around it
  for (int x = 8; x < 64; x += 16) d->drawPixelRGB888(x, 34, 255, 255, 255);
  d->fillRect(4, 38, 8, 1, 255, 255, 255);   // horizontal line
  d->fillRect(20, 36, 1, 8, 255, 255, 255);  // vertical line
  d->fillRect(32, 38, 3, 3, 255, 255, 255);  // 3x3 block
  for (int y = 34; y < 46; ++y)
    for (int x = 44; x < 60; ++x)
      if ((x + y) & 1) d->drawPixelRGB888(x, y, 255, 255, 255);  // checker

  // The same single pixels on dark grey (16) and mid grey (64), as the word clock's background would be
  d->fillRect(0, 48, 32, 16, 16, 16, 16);
  d->fillRect(32, 48, 32, 16, 64, 64, 64);
  for (int x = 8; x < 64; x += 16) d->drawPixelRGB888(x, 52, 255, 255, 255);
  d->fillRect(4, 58, 6, 1, 255, 255, 255);
  d->fillRect(36, 56, 1, 6, 255, 255, 255);
  d->fillRect(48, 57, 5, 5, 255, 255, 255);
}
#endif
#endif

void Clockface::update() {
  const uint32_t now = millis();
  if (now - _reported >= 10000) {
    _reported = now;
    const auto stats = _display->getRingStats();
    Serial.printf("[Scanner] refill mean=%u max=%u us, row=%u us, missed=%u\n",
                  (unsigned)(stats.meanCycles / getCpuFrequencyMhz()),
                  (unsigned)(stats.maxCycles / getCpuFrequencyMhz()),
                  (unsigned)stats.rowPeriodUs, (unsigned)stats.missed);
  }
#ifdef UM_PATTERN
  drawPattern(_display);
  _display->flipDMABuffer();
  return;
#endif
  if (!_benched && now - _started > 30000) {
    _benched = true;
    benchmark();
  }
  const int span = _display->width() - 1;
  const int phase = ((now - _started) / 14) % (2 * span);
  const int x = phase < span ? phase : 2 * span - phase;
  _display->fillScreenRGB888(0, 0, 0);
  for (int dx = -1; dx <= 1; ++dx)
    for (int y = 0; y < _display->height(); ++y)
      _display->drawPixelRGB888(x + dx, y, 255, 255, 255);
  _display->flipDMABuffer();
}
