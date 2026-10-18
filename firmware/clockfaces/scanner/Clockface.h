// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "scanner"

class Clockface : public IClockface {
public:
  explicit Clockface(MatrixPanel_I2S_DMA *display) : _display(display) {}
  void setup(ClockTime *) override;
  void update() override;
  bool handlesNight() const override { return true; }

private:
  MatrixPanel_I2S_DMA *_display;
  uint32_t _started = 0;
  uint32_t _reported = 0;
  bool _benched = false;
  void benchmark();
};
