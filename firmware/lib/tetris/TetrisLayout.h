// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>
#include "TetrisDigits.h"

// Where the digits of the Tetris clock go. No drawing and no state: hour, minute, day and month in, positions out.
namespace tetris {

constexpr int kScale = 2;          // panel pixels per block
constexpr int kDigitSpacing = 7;   // blocks between digit origins
constexpr int kGroupGap = 3;       // extra blocks between the first and the second pair of digits
constexpr int kTimeBottom = 26;    // row where the time lands
constexpr int kDateBottom = 58;    // row where the date lands
constexpr int kSeparatorY = 45;    // the white mark between day and month
constexpr int kSeparatorWidth = 4;
constexpr int kSeparatorHeight = 2;
constexpr int kColonTop = 10;      // the two colon squares, kScale * 2 pixels each
constexpr int kColonBottom = 18;
constexpr int kBlank = -1;         // a digit slot that shows nothing

// Pixels from the line's x to the left of the digit in this slot (0..3)
constexpr int slotOffset(int slot) { return slot * kDigitSpacing * kScale + (slot >= 2 ? kGroupGap * kScale : 0); }

struct TimeLayout {
  int8_t digits[4];  // hours, minutes; kBlank in front of a one-digit hour
  int8_t x;
  int8_t colonX;
};

struct DateLayout {
  int8_t digits[4];  // day, month; kBlank for a missing tens or the unused slot after a one-digit month
  int8_t x;
  int8_t separatorX;
};

TimeLayout timeLayout(int hour, int minute);
DateLayout dateLayout(int day, int month);

}  // namespace tetris
