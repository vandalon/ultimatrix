// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TetrisScreens.h"
#include <string.h>
#include <TetrisDigits.h>
#include <TetrisLayout.h>

using namespace tetrisview;
using namespace tetris;

// The original ran a step every 50 ms, 30 ms while starting up and for the date, and as fast as it could in the last
// seconds of a minute so that it always finished before the minute changed. The face is called every 50 ms, so a
// step that is due early is taken together with the next one, and "as fast as it could" is a handful of steps.
static const uint32_t kStepMs = 50, kFastStepMs = 30;
static const uint8_t kMostSteps = 4, kRushSteps = 5;

void TetrisScreens::reset() {
  memset(&_frame, 0, sizeof(_frame));
  for (Slot &slot : _frame.time) slot.digit = kBlank;
  for (Slot &slot : _frame.date) slot.digit = kBlank;
  _frame.phase = Idle;
  _dateKey = _timeKey = 0xFFFF;
  _nextStepAt = 0;
  _firstRun = true;
}

void TetrisScreens::beginDate(const Input &in) {
  const DateLayout layout = dateLayout(in.day, in.month);
  _frame.phase = DateFalling;
  _frame.dateX = layout.x;
  _frame.separatorX = layout.separatorX;
  // Only the date is on the screen while it falls; the time falls again afterwards
  for (int i = 0; i < 4; i++) {
    _frame.date[i] = {layout.digits[i], 0, 0};
    _frame.time[i] = {kBlank, 0, 0};
  }
  _frame.colon = 0;
  _dateKey = in.month * 32 + in.day;
  _timeKey = 0xFFFF;
  _nextStepAt = in.now + interval(true, in.second);
}

void TetrisScreens::beginTime(const Input &in) {
  const TimeLayout layout = timeLayout(in.hour, in.minute);
  _frame.phase = TimeFalling;
  _frame.timeX = layout.x;
  _frame.colonX = layout.colonX;
  // Only the digits that changed fall again
  for (int i = 0; i < 4; i++)
    if (layout.digits[i] != _frame.time[i].digit) _frame.time[i] = {layout.digits[i], 0, 0};
  _frame.colon = 0;
  _timeKey = in.hour * 60 + in.minute;
  _nextStepAt = in.now + interval(false, in.second);
}

// Milliseconds between steps of the date (date=true) or the time
uint32_t TetrisScreens::interval(bool date, int second) const {
  if (date) return second < 58 ? kFastStepMs : 0;
  return second >= 59 ? 0 : _firstRun ? kFastStepMs : kStepMs;
}

uint8_t TetrisScreens::stepsDue(uint32_t now, uint32_t every) {
  if (every == 0) {
    _nextStepAt = now;
    return kRushSteps;
  }
  uint8_t due = 0;
  while ((int32_t)(now - _nextStepAt) >= 0 && due < kMostSteps) {
    _nextStepAt += every;
    due++;
  }
  if (due == kMostSteps) _nextStepAt = now + every;  // far behind: do not catch up
  return due;
}

// One row down for the piece in flight in every slot. True while anything is still falling.
bool TetrisScreens::stepSlots(Slot (&slots)[4]) {
  bool falling = false;
  for (Slot &slot : slots) {
    if (slot.digit < 0) continue;
    const Digit &digit = kDigits[slot.digit];
    if (slot.piece >= digit.count) continue;
    if (++slot.fall > digit.pieces[slot.piece].stop) {
      slot.fall = 0;
      slot.piece++;
    }
    if (slot.piece < digit.count) falling = true;
  }
  return falling;
}

const Frame &TetrisScreens::advance(const Input &in) {
  if (_frame.phase == Idle) {
    const uint16_t dateKey = in.month * 32 + in.day;
    const uint16_t timeKey = in.hour * 60 + in.minute;
    if (dateKey != _dateKey) {
      beginDate(in);
    } else if (timeKey != _timeKey) {
      beginTime(in);
    } else {
      _frame.colon = in.second % 2 == 0;
    }
    return _frame;
  }

  const bool date = _frame.phase == DateFalling;
  const uint8_t steps = stepsDue(in.now, interval(date, in.second));
  for (uint8_t i = 0; i < steps; i++) {
    if (stepSlots(date ? _frame.date : _frame.time)) continue;
    // Landed: the screen is idle until the next change
    _frame.phase = Idle;
    if (!date) _firstRun = false;
    _frame.colon = in.second % 2 == 0;
    break;
  }
  return _frame;
}
