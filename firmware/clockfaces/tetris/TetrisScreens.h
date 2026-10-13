// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The timing half of the Tetris face: when the date and the time start to fall, how fast, and when the clock is idle
// with a blinking colon. No drawing: Clockface draws the Frame it returns through TetrisRenderer.h.

#include <stdint.h>
#include "TetrisRenderer.h"

class TetrisScreens {
 public:
  struct Input {
    int hour, minute, second, day, month;
    uint32_t now;  // millis()
  };

  // Start over: the date falls first, then the time
  void reset();
  // Move on to `in.now`. The returned frame is valid until the next call.
  const tetrisview::Frame &advance(const Input &in);

 private:
  void beginDate(const Input &in);
  void beginTime(const Input &in);
  uint32_t interval(bool date, int second) const;
  uint8_t stepsDue(uint32_t now, uint32_t every);
  static bool stepSlots(tetrisview::Slot (&slots)[4]);

  tetrisview::Frame _frame;
  uint16_t _dateKey, _timeKey;  // what the digits were last set to (0xFFFF: nothing yet)
  uint32_t _nextStepAt;
  bool _firstRun;               // the time has not fallen yet since reset(): it falls faster
};
