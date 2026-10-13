#pragma once
#include <time.h>
#define SECS_PER_DAY ((time_t)86400)
namespace ezt {
extern time_t fakeNow;
inline time_t now() { return fakeNow; }
inline int day(time_t t) { struct tm tmv; gmtime_r(&t, &tmv); return tmv.tm_mday; }
inline int month(time_t t) { struct tm tmv; gmtime_r(&t, &tmv); return tmv.tm_mon + 1; }
}
