#pragma once
#include <stdint.h>

// Page timing starts when a screen is entered, independent of boot uptime.
class ScreenTimer {
public:
  void enter(int state, uint32_t now) {
    if (state != _state) { _state = state; _enteredAt = now; }
  }
  uint32_t elapsed(uint32_t now) const { return now - _enteredAt; }
  uint32_t page(uint32_t now, uint32_t duration) const {
    return duration ? elapsed(now) / duration : 0;
  }
private:
  int _state = -1;
  uint32_t _enteredAt = 0;
};
