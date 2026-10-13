#pragma once
#include <Adafruit_GFX.h>
class Locator {
public:
  static Adafruit_GFX *getDisplay() { return display(); }
  static void provide(Adafruit_GFX *value) { display() = value; }
private:
  static Adafruit_GFX *&display() { static Adafruit_GFX *value = nullptr; return value; }
};
