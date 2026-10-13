#pragma once
#include <Arduino.h>
struct ConsoleStub {
  template <typename T> void print(const T &) {}
  void println(const String &) {}
};
extern ConsoleStub Console;
