#pragma once
#include <ezTime.h>

enum timeStatus_t { timeNotSet, timeNeedsSync, timeSet };
namespace ultime {
inline time_t now() { return ezt::now(); }
inline timeStatus_t timeStatus() { return now() >= 1700000000 ? timeSet : timeNotSet; }
inline int day(time_t t) { return ezt::day(t); }
inline int month(time_t t) { return ezt::month(t); }
}
