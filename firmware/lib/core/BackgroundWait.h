#pragma once

#include <Arduino.h>
#include <functional>

// A wait that doesn't freeze the clock: the animations (a goal, a card) and the messages that stay on
// screen for a few seconds used to delay(), so the settings page and the notification port didn't answer
// until they were over. serviceWait() runs the background work set by main.cpp while it waits.

inline std::function<void()> &backgroundWork() {
  static std::function<void()> work;
  return work;
}

inline void serviceWait(unsigned long ms) {
  unsigned long until = millis() + ms;
  while ((long)(millis() - until) < 0) {
    if (backgroundWork()) backgroundWork()();
    delay(5);
  }
}
