#include "TimeService.h"
#include "TzDb.h"
#include <HTTPClient.h>
#include <esp_sntp.h>
#include <atomic>

namespace ultime {

static const time_t PLAUSIBLE = 1700000000;  // late 2023: anything before it is the clock before the first sync
static std::atomic<bool> synced{false};

static void timeSynced(struct timeval *) { synced = true; }

void begin(const char *ntpServer, const char *posix) {
  // Sets TZ and starts (or restarts) the SNTP client; the core resyncs about once an hour by itself
  // The time stays valid when only the zone or format changed: only a new server (or the first start) waits for a sync
  static uint32_t lastServer = 0;  // a hash, not the name: static RAM is tight
  uint32_t hash = 2166136261u;
  for (const char *c = ntpServer ? ntpServer : ""; *c; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
  if (hash != lastServer) { synced = false; lastServer = hash; }
  esp_sntp_set_time_sync_notification_cb(timeSynced);
  configTzTime(posix, ntpServer, "time.google.com", "pool.ntp.org");
}

const char *posixForZone(const char *name) {
  unsigned lo = 0, hi = UM_TZ_DB_SIZE;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    int c = strcmp(name, UM_TZ_DB[mid].name);
    if (c == 0) return UM_TZ_DB[mid].posix;
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  return nullptr;
}

bool placeForZone(const char *name, float &latitude, float &longitude) {
  unsigned lo = 0, hi = UM_TZ_DB_SIZE;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    int c = strcmp(name, UM_TZ_DB[mid].name);
    if (c == 0) {
      if (UM_TZ_DB[mid].lat == UM_TZ_NO_PLACE) return false;
      latitude = UM_TZ_DB[mid].lat / 100.0f; longitude = UM_TZ_DB[mid].lon / 100.0f;
      return true;
    }
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  return false;
}

bool countryForZone(const char *name, char (&country)[3]) {
  unsigned lo = 0, hi = UM_TZ_DB_SIZE;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    int c = strcmp(name, UM_TZ_DB[mid].name);
    if (c == 0) {
      memcpy(country, UM_TZ_DB[mid].country, sizeof(country));
      return country[0] != 0;
    }
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  return false;
}

bool detectZone(String &zone, float *latitude, float *longitude) {
  HTTPClient http;
  http.setConnectTimeout(3000); http.setTimeout(3000);
  if (!http.begin("http://ip-api.com/line/?fields=timezone,lat,lon")) return false;
  int code = http.GET();
  String body = code == 200 ? http.getString() : String();
  http.end();
  // One value per line, in the service's own order: the name has a slash, the two numbers are latitude then longitude
  String name;
  float numbers[2] = {0, 0};
  int found = 0;
  for (int start = 0; start < (int)body.length();) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    String line = body.substring(start, end);
    line.trim();
    if (line.indexOf('/') >= 0 || line == "UTC") name = line;
    else if (!line.isEmpty() && found < 2) numbers[found++] = line.toFloat();
    start = end + 1;
  }
  if (name.isEmpty() || !posixForZone(name.c_str())) return false;
  zone = name;
  if (latitude && longitude && found == 2) { *latitude = numbers[0]; *longitude = numbers[1]; }
  return true;
}

size_t zoneCount() { return UM_TZ_DB_SIZE; }
const char *zoneName(size_t index) { return index < UM_TZ_DB_SIZE ? UM_TZ_DB[index].name : nullptr; }

time_t now() { return time(nullptr); }

timeStatus_t timeStatus() { return synced && now() >= PLAUSIBLE ? timeSet : timeNotSet; }

void events() {}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm), valid for the whole time_t range we use
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

time_t makeTime(int hour, int minute, int second, int day, int month, int year) {
  return (time_t)daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

void breakTime(time_t t, tmElements_t &el) {
  struct tm tm;
  gmtime_r(&t, &tm);
  el.Second = tm.tm_sec;
  el.Minute = tm.tm_min;
  el.Hour = tm.tm_hour;
  el.Wday = tm.tm_wday + 1;
  el.Day = tm.tm_mday;
  el.Month = tm.tm_mon + 1;
  el.Year = tm.tm_year - 70;
}

int day(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  return tm.tm_mday;
}

int month(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  return tm.tm_mon + 1;
}

}  // namespace ultime
