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
