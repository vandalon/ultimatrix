// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "pacman"

// The time in solid 6x10 block digits (as football and F1) over a full-panel Pac-Man maze, dimmed behind the time: a new random maze (mirrored, no dead ends, a tunnel, a ghost
// house, four power pills) for every level, with the four ghosts hunting as in the arcade game
class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate() {}
    int ditherPolicy() const override { return 0; }  // flat sprites and a maze: the pattern only adds noise
};
