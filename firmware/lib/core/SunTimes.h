// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Sunrise and sunset for a place and moment (NOAA's low-precision formulas, good to about two minutes), as minutes of the
// local day. Header only; the local offset comes from the C library's timezone, so call it after the clock's zone is set.
#pragma once

#include <math.h>
#include <time.h>

namespace sun {

enum class Result { Normal, AlwaysNight, AlwaysDay };

inline Result times(float latitude, float longitude, time_t moment, int &riseMinute, int &setMinute) {
  struct tm local, utc;
  localtime_r(&moment, &local);
  gmtime_r(&moment, &utc);
  int days = local.tm_yday - utc.tm_yday;
  if (days > 1) days = -1; else if (days < -1) days = 1;  // across new year
  const int offset = days * 1440 + (local.tm_hour * 60 + local.tm_min) - (utc.tm_hour * 60 + utc.tm_min);

  const float rad = 0.017453292f;
  const float g = 2.0f * 3.14159265f / 365.0f * (utc.tm_yday + 0.5f);
  const float equation = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g) - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
  const float declination = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g) -
                            0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
  const float lat = latitude * rad;
  const float c = cosf(90.833f * rad) / (cosf(lat) * cosf(declination)) - tanf(lat) * tanf(declination);
  if (c > 1.0f) return Result::AlwaysNight;
  if (c < -1.0f) return Result::AlwaysDay;
  const float hourAngle = acosf(c) / rad;
  const float rise = 720.0f - 4.0f * (longitude + hourAngle) - equation + offset;
  const float set = 720.0f - 4.0f * (longitude - hourAngle) - equation + offset;
  riseMinute = ((int)lroundf(rise) % 1440 + 1440) % 1440;
  setMinute = ((int)lroundf(set) % 1440 + 1440) % 1440;
  return Result::Normal;
}

// Whether the sun is down at this minute of the local day
inline bool isNight(float latitude, float longitude, time_t moment, int localMinute) {
  int rise = 0, set = 0;
  const Result r = times(latitude, longitude, moment, rise, set);
  if (r == Result::AlwaysNight) return true;
  if (r == Result::AlwaysDay) return false;
  const bool day = rise < set ? localMinute >= rise && localMinute < set : localMinute >= rise || localMinute < set;
  return !day;
}

}  // namespace sun
