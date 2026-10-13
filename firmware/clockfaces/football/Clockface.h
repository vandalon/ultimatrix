// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "football"


class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate();
    // Telnet S and N: the simulator is gone, these only say so
    const char *simulate();
    const char *simulateNext();
    // Telnet G: celebrate a made-up goal
    void testGoal();
    // Telnet Y, D and W: a made-up yellow card, red card ('y', 'r') or substitution ('s')
    void testIncident(char kind);
    // A goal or incident animation is playing (one frame per update): the preview tool steps it
    bool animating() const;
};
