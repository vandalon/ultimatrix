// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "nfl"

// This week's NFL games, one at a time: quarter and clock on top, both teams with their score, the ball on a strip of field with the down and
// distance, the timeouts. A score plays a short text flash; without games the clock shows with the next game and a football flying about.
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
