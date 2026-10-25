#pragma once

#include <Arduino.h>
#include "IClockface.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#define CLOCKFACE_NAME "invaders"

class Clockface: public IClockface {
  private:
    ClockTime* _dateTime;
    MatrixPanel_I2S_DMA* _display;

  public:
    Clockface(MatrixPanel_I2S_DMA* display);
    void setup(ClockTime *dateTime);
    void update();
    void invalidate() {}
    void steer(int dir);   // 1 right, 3 left, 0 fire (keeps firing), 2 stop moving and firing: play yourself from the web page; -1 gives the cannon back to the computer
    int ditherPolicy() const override { return 0; }  // flat sprites: the pattern only adds noise
};
