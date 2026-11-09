#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "tetris"

class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate();
    void steer(int dir);   // 0 rotate, 1 right, 2 down, 3 left, 4 drop: play from the web page; -1 gives the board back to the computer
    int ditherPolicy() const override { return 0; }  // flat blocks: the pattern only adds noise
};
