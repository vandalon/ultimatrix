#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "pong"

class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate() {}
    void steer(int dir);   // 0 up, 2 down, 1 or 3 stop: play the left paddle yourself from the web page; -1 gives it back to the computer
    int ditherPolicy() const override { return 0; }  // flat shapes: the pattern only adds noise
};
