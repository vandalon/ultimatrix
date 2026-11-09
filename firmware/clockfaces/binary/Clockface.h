// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "binary"

// Binary clock: hours, minutes and seconds as six columns of dots (each digit in binary, 8-4-2-1), the day (5 bits) and the month
// (4 bits) as two small rows of dots above them.
class Clockface : public IClockface {
public:
  explicit Clockface(MatrixPanel_I2S_DMA *display) : _display(display) {}
  void setup(ClockTime *time) override;
  void update() override;
  int ditherPolicy() const override { return 0; }       // flat dots: the pattern only adds noise

private:
  MatrixPanel_I2S_DMA *_display;
  ClockTime *_time = nullptr;
  uint8_t _cur[6] = {0}, _prev[6] = {0};   // what each column shows now and what it showed before its last change
  uint32_t _changedAt[6] = {0};
  bool _ready = false;
  void refresh(uint32_t now);
};
