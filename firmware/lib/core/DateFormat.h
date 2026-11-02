// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The numeric date in the order and with the separator the zone's country uses ("28-11", "28.11", "28/11", "11/28"). Header only.
#pragma once

#include <stdio.h>
#include <string.h>

namespace dateformat {

// Zone names listed per style; anything not listed gets day-month with a hyphen
inline char styleOf(const char *zone) {
  static const char *const kMonthDay = "|America/New_York|America/Chicago|America/Denver|America/Los_Angeles|America/Phoenix|America/Anchorage|America/Detroit|America/Boise|America/Indiana/Indianapolis|America/Toronto|America/Vancouver|America/Edmonton|America/Winnipeg|America/Halifax|Pacific/Honolulu|Asia/Tokyo|Asia/Shanghai|Asia/Seoul|Asia/Taipei|";
  static const char *const kDots = "|Europe/Berlin|Europe/Vienna|Europe/Zurich|Europe/Copenhagen|Europe/Oslo|Europe/Helsinki|Europe/Warsaw|Europe/Prague|Europe/Bratislava|Europe/Budapest|Europe/Moscow|Europe/Kiev|Europe/Kyiv|Europe/Bucharest|Europe/Sofia|Europe/Zagreb|Europe/Ljubljana|Europe/Belgrade|Europe/Tallinn|Europe/Riga|Europe/Vilnius|Europe/Istanbul|Europe/Luxembourg|";
  static const char *const kSlashes = "|Europe/London|Europe/Dublin|Europe/Paris|Europe/Madrid|Europe/Rome|Europe/Lisbon|Europe/Stockholm|Europe/Athens|Australia/Sydney|Australia/Melbourne|Australia/Brisbane|Australia/Perth|Australia/Adelaide|Pacific/Auckland|Asia/Kolkata|Asia/Singapore|Asia/Dubai|America/Mexico_City|America/Sao_Paulo|America/Argentina/Buenos_Aires|America/Bogota|Africa/Johannesburg|Africa/Cairo|";
  char needle[48];
  snprintf(needle, sizeof(needle), "|%s|", zone ? zone : "");
  if (strstr(kMonthDay, needle)) return 'm';
  if (strstr(kDots, needle)) return '.';
  if (strstr(kSlashes, needle)) return '/';
  return '-';
}

inline void format(const char *zone, int day, int month, char *out, size_t size) {
  const char style = styleOf(zone);
  if (style == 'm') snprintf(out, size, "%02d/%02d", month, day);
  else snprintf(out, size, "%02d%c%02d", day, style, month);
}

}  // namespace dateformat
