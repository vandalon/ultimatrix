// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "bridge"

// Not a clock: the stop-over for clocks that ran the old firmware. It only tells you where to pick a face.
class Clockface: public IClockface {
  private:
    MatrixPanel_I2S_DMA* _display;
    String _shown;
    bool _dirty = true;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *) {}
    void update();
    void invalidate() { _dirty = true; }
};
