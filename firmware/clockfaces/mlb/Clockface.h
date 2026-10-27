// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "mlb"

// Today's MLB games, one at a time: the time on top, both teams with their score, and under them the bases, count and outs,
// the first pitch time or FINAL. With games in play only those show; a run lights up the row of the team that scored.
// Without games the clock shows alone.
class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate();
    void steer(int dir);   // 1 plays the demo (every screen once, made-up data), 0 stops it
    int ditherPolicy() const override { return 0; }  // flat shapes: the pattern only adds noise
};
