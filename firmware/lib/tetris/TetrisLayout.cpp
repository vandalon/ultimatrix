// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TetrisLayout.h"

namespace tetris {

TimeLayout timeLayout(int hour, int minute) {
  TimeLayout out;
  out.digits[0] = hour < 10 ? kBlank : hour / 10;
  out.digits[1] = hour % 10;
  out.digits[2] = minute / 10;
  out.digits[3] = minute % 10;
  out.x = hour < 10 ? -5 : hour < 20 ? -2 : 2;
  // The colon moves right when the minutes start with the narrow 1
  out.colonX = out.x + (minute >= 10 && minute < 20 ? 4 : 0) + 2 * kDigitSpacing * kScale;
  return out;
}

DateLayout dateLayout(int day, int month) {
  DateLayout out;
  out.digits[0] = day < 10 ? kBlank : day / 10;
  out.digits[1] = day % 10;
  out.digits[2] = month < 10 ? month : month / 10;
  out.digits[3] = month < 10 ? kBlank : month % 10;
  if (month == 1 && day == 1) {
    out.x = -2; out.separatorX = 30;
  } else if (month < 10 && day < 10) {
    out.x = 2; out.separatorX = month == 1 ? 34 : 30;
  } else if (day >= 20 && month < 10) {
    out.x = 8; out.separatorX = 36;
  } else if (month < 10) {
    out.x = 5; out.separatorX = month == 1 ? 37 : 33;
  } else if (day < 10) {
    out.x = -5; out.separatorX = 27;
  } else if (day < 20) {
    out.x = -2; out.separatorX = 30;
  } else {
    out.x = 2; out.separatorX = 34;
  }
  return out;
}

}  // namespace tetris
