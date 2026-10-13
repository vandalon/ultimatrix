#pragma once
#include <Arduino.h>

#include "TimeService.h"
#include <WiFi.h>

class ClockTime
{
private:
  struct tm localTm() const;  // the local time right now
  bool use24hFormat = true;

public:
  void begin(const char *timeZone, bool use24format, const char *ntpServer, const char *posixTZ);
  String getFormattedTime();
  String getFormattedTime(const char* format);  // strftime format, local time

  char *getHour(const char *format);
  char *getMinute(const char *format);
  int getHour();
  int getHour24();  // 0-23 whatever the display format is
  int getMinute();
  int getSecond();
  long getMilliseconds();

  int getDay();
  int getMonth();
  int getWeekday();
  int getYear();

  // Local wall-clock time as time_t, and conversion of a UTC time_t to it
  time_t localNow();
  time_t utcToLocal(time_t utc);

  bool isAM();
  bool is24hFormat();
};
