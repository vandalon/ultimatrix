#include "ClockTime.h"
#include "Console.h"
#include <sys/time.h>

void ClockTime::begin(const char *timeZone, bool use24format, const char *ntpServer = "time.cloudflare.com", const char *posixTZ = "")
{
  consoleLog("[Time] NTP Server: %s, Timezone: %s\n", ntpServer, timeZone);

  const char *rule = posixTZ;
  if (strlen(rule) <= 1) {
    // No POSIX rule given: look the timezone name up in the built-in table
    rule = ultime::posixForZone(timeZone);
    if (!rule) {
      consoleLog("[Time] Unknown timezone '%s', using UTC. Set a POSIX rule instead\n", timeZone);
      rule = "UTC0";
    }
  }
  consoleLog("[Time] Rule: %s\n", rule);

  this->use24hFormat = use24format;
  ultime::begin(ntpServer, rule);
}

// The fields are read one call at a time, so the time is kept for a few milliseconds: a face that reads hour, minute and second in
// one frame always sees the same instant, also across a minute rollover (it would otherwise draw 12:00 for 13:00 for a frame).
struct tm ClockTime::localTm() const
{
  static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  static struct tm cached;
  static uint32_t cachedAt = 0;
  static bool have = false;
  const uint32_t now = millis();
  struct tm tm;
  portENTER_CRITICAL(&lock);
  const bool fresh = have && (uint32_t)(now - cachedAt) < 30;
  if (fresh) tm = cached;
  portEXIT_CRITICAL(&lock);
  if (fresh) return tm;
  time_t t = ultime::now();
  localtime_r(&t, &tm);
  portENTER_CRITICAL(&lock);
  cached = tm; cachedAt = now; have = true;
  portEXIT_CRITICAL(&lock);
  return tm;
}

String ClockTime::getFormattedTime()
{
  return getFormattedTime("%a, %d-%b-%Y %H:%M:%S");
}

String ClockTime::getFormattedTime(const char *format)
{
  struct tm tm = localTm();
  char buffer[64];
  strftime(buffer, sizeof(buffer), format, &tm);
  return String(buffer);
}

char *ClockTime::getHour(const char *format)
{
  static char buffer[3] = {'\0'};
  snprintf(buffer, sizeof(buffer), "%02d", getHour());
  return buffer;
}

char *ClockTime::getMinute(const char *format)
{
  static char buffer[3] = {'\0'};
  snprintf(buffer, sizeof(buffer), "%02d", getMinute());
  return buffer;
}

int ClockTime::getHour()
{
  int h = localTm().tm_hour;
  if (use24hFormat) return h;
  return h % 12 == 0 ? 12 : h % 12;
}

int ClockTime::getHour24()
{
  return localTm().tm_hour;
}

int ClockTime::getMinute()
{
  return localTm().tm_min;
}

int ClockTime::getSecond()
{
  return localTm().tm_sec;
}

int ClockTime::getDay()
{
  return localTm().tm_mday;
}

int ClockTime::getMonth()
{
  return localTm().tm_mon + 1;
}

int ClockTime::getYear()
{
  return localTm().tm_year + 1900;
}

int ClockTime::getWeekday()
{
  return localTm().tm_wday;  // 0 = Sunday
}

long ClockTime::getMilliseconds()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_usec / 1000;
}

time_t ClockTime::localNow()
{
  return utcToLocal(ultime::now());
}

time_t ClockTime::utcToLocal(time_t utc)
{
  struct tm tm;
  localtime_r(&utc, &tm);
  return ultime::makeTime(tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
}

bool ClockTime::isAM()
{
  return localTm().tm_hour < 12;
}

bool ClockTime::is24hFormat()
{
  return this->use24hFormat;
}
