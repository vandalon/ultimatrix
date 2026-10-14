// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <WiFi.h>
#include <Fonts/Picopixel.h>

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::update() {
  String address = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("NO WI-FI");
  if (!_dirty && address == _shown) return;
  _shown = address; _dirty = false;
  MatrixPanel_I2S_DMA &d = *_display;
  d.fillScreen(0);
  d.setFont(nullptr);
  d.setTextSize(1);
  d.setTextColor(d.color565(0, 200, 255));
  d.setCursor(2, 6); d.print("ULTIMATRIX");
  d.setTextColor(d.color565(255, 255, 255));
  d.setCursor(8, 24); d.print("CHOOSE A");
  d.setCursor(8, 34); d.print("FACE AT");
  d.setFont(&Picopixel);
  d.setTextColor(d.color565(255, 200, 0));
  int16_t x, y; uint16_t w, h;
  d.getTextBounds(address.c_str(), 0, 0, &x, &y, &w, &h);
  d.setCursor((64 - (int)w) / 2 - x, 54);
  d.print(address);
  d.setFont(nullptr);
  d.flipDMABuffer();
}
