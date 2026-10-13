// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The timing half of the F1 face: which screen is on, which page of it, and whether the picture changed since the
// last one drawn. No drawing: Clockface draws the screen it names through F1Renderer.h.

#include <Arduino.h>
#include <ClockTime.h>
#include "F1Renderer.h"

namespace f1view {

enum class F1Screen { Live, Results, Between, Idle };
constexpr F1Screen LIVE = F1Screen::Live, LIST = F1Screen::Results;
constexpr F1Screen BETWEEN = F1Screen::Between, IDLE = F1Screen::Idle;

// Everything that makes the picture differ from the one before. The face draws again when it changes, so only values
// that change a pixel belong here: animation steps rather than raw milliseconds.
struct Frame {
  uint32_t version;
  int32_t view;  // F1Screen, or -1 for the loading screen
  int32_t hour, minute;
  int32_t page;
  uint32_t marks;
  int32_t rowPage;
  int32_t pulse;
  int32_t loadingDots;
  int32_t showIntervals;
};

struct ScreenInputs {
  ClockTime *dateTime;
  int hour, minute;
  uint32_t version;  // of the ticker's data
  bool intervals;    // the live race shows gaps between cars rather than to the leader
  int simScenario;   // the telnet simulator's screen, 0 when off
};

class F1Screens {
  public:
    // Picks the screen and page for now, steps the view state, and says whether to draw again
    bool advance(const ScreenInputs &in, F1Screen &view, int &page);
    // The loading screen: whether its picture changed (the wave moves every 60 ms)
    bool loadingChanged(unsigned long frame, int height);
    // Whatever was drawn over the face (notification, simulator) must be drawn again
    void reset() { _haveFrame = false; }

  private:
    Frame _frame;
    bool _haveFrame = false;
    char _lastRight[12] = "";
};

}  // namespace f1view
