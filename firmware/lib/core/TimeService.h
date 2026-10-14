#pragma once
#include <Arduino.h>
#include <time.h>

// Time as the firmware uses it: the clock is UTC from the core's SNTP client, local time comes from a
// POSIX timezone rule (setenv TZ). Replaces the ezTime library, which is no longer maintained.

enum timeStatus_t { timeNotSet, timeNeedsSync, timeSet };

// Broken-down time, same layout as the one ezTime had: Year counts from 1970
typedef struct {
  uint8_t Second, Minute, Hour, Wday;  // Wday: 1 = Sunday
  uint8_t Day, Month, Year;
} tmElements_t;

#define SECS_PER_DAY ((time_t)86400)

namespace ultime {

// Starts the SNTP client against the server and sets the timezone rule (POSIX string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3")
void begin(const char *ntpServer, const char *posix);

// The POSIX rule for a timezone name like "Europe/Amsterdam", nullptr when the name is not in the table
const char *posixForZone(const char *name);
// Asks a geo-IP service for the timezone of the network's public address; true and the name in zone when it is one in the table
bool detectZone(String &zone, float *latitude = nullptr, float *longitude = nullptr);
size_t zoneCount();
const char *zoneName(size_t index);

time_t now();                 // UTC
timeStatus_t timeStatus();    // timeSet once the clock has been set by SNTP
void events();                // nothing to do: SNTP runs in the background. Kept so the main loop does not change.

// UTC fields <-> time_t (no timezone involved)
time_t makeTime(int hour, int minute, int second, int day, int month, int year);
void breakTime(time_t t, tmElements_t &tm);
int day(time_t t);    // 1-31, of the UTC time_t you give (pass a local time_t for local fields)
int month(time_t t);  // 1-12

}  // namespace ultime
