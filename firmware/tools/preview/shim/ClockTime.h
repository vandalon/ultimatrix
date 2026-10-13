// A clock the preview sets by hand
#pragma once
#include <Arduino.h>
#include <ezTime.h>

class ClockTime {
 public:
  int year = 2026, hour = 21, minute = 34, second = 0, day = 4, month = 10, weekday = 0;
  int utcOffset = 0;
  long ms = 0;  // milliseconds into the second, set by the preview
  int getMilliseconds() { return (int)ms; }
  String getHour(const char *fmt) { char b[8]; snprintf(b, sizeof(b), fmt[0] == '0' && fmt[1] == '0' ? "%02d" : "%d", hour); return String(b); }
  String getMinute(const char *fmt) { char b[8]; snprintf(b, sizeof(b), fmt[0] == '0' && fmt[1] == '0' ? "%02d" : fmt, minute); return String(b); }
  bool is24hFormat() { return true; }
  bool isAM() { return hour < 12; }
  int getYear() { return year; }
  int getHour() { return hour; }
  int getHour24() { return hour; }
  int getMinute() { return minute; }
  int getSecond() { return second; }
  int getDay() { return day; }
  int getMonth() { return month; }
  int getWeekday() { return weekday; }
  time_t localNow() { return ezt::now() + utcOffset; }
  time_t utcToLocal(time_t utc) { return utc + utcOffset; }
};
