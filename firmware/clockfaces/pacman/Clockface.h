// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "pacman"

// The Pac-Man face of the Clockwise / clockwizer clock, ported: a random maze of 8 x 7 lanes with the time, the level, the score and
// the lives in a box in the middle of it, pacman eating his way round, and four ghosts that live in the box and leave through its floor.
// Everything is redrawn every frame.
class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate() {}
    void steer(int dir);   // 0 up, 1 right, 2 down, 3 left: play yourself from the web page; -1 gives him back to the computer
    int ditherPolicy() const override { return 0; }  // flat sprites and a maze: the pattern only adds noise
};
