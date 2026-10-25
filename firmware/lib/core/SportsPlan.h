// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stdint.h>
#include <string.h>
#include <time.h>

// Which sports face should be on the panel right now. Pure logic (no hardware), fed by SportsWatch with the events
// found in the sports feeds and by the settings the user chose.
namespace sportsplan {

enum Sport : uint8_t { F1, MOTOGP, FOOTBALL, SPORTS };
static const char *const kModes[SPORTS] = {"f1", "motogp", "football"};  // the firmware mode that shows each sport

struct Event {
  time_t start = 0, end = 0;  // UTC; end is when the event is expected to be over
  bool live = false;          // the feed said it is on right now
  char label[14] = "";        // what it is: "Race", "NFO @ IPS"
};
static const int kMaxEvents = 12;
struct Events {
  Event list[kMaxEvents];
  uint8_t count = 0;
  time_t checkedAt = 0;  // when the feed was last read; 0: never
};

struct Config {
  uint8_t order[SPORTS];  // enabled sports, best first
  uint8_t orderCount = 0;
  char home[16] = "";  // mode to go back to when nothing is on; empty: stay
  uint16_t leadSecs = 600;
  uint16_t holdSecs = 900;  // stay this long after the last event ended
  uint8_t maxPerDay = 4;
  uint16_t minGapSecs = 1800;
  uint16_t bridgeSecs = 3600;  // stay between two events of the same sport that start within this long of each other (0 = off)
};

struct Log {
  uint32_t day = 0;   // days since 1970 (UTC) of `count`
  uint8_t count = 0;  // automatic switches into a sport on that day
  time_t last = 0;    // the last automatic switch of any kind
  char target[16] = "";  // the mode that switch went to: only a face we switched to is left again by itself
  char seen[16] = "";    // the last mode noticed as chosen by hand (not by us)
  time_t pausedUntil = 0;  // a face chosen by hand during an event: no automatic switch before this
};

static const time_t kStale = 20 * 60;  // a feed read older than this no longer vouches for "live"

// "f1,football" -> order. Unknown names are skipped, repeats too
inline void parseOrder(const char *csv, Config &cfg) {
  cfg.orderCount = 0;
  while (csv && *csv) {
    const char *end = strchr(csv, ',');
    size_t n = end ? (size_t)(end - csv) : strlen(csv);
    for (uint8_t s = 0; s < SPORTS; ++s) {
      if (strlen(kModes[s]) != n || strncmp(csv, kModes[s], n)) continue;
      bool seen = false;
      for (uint8_t i = 0; i < cfg.orderCount; ++i) seen |= cfg.order[i] == s;
      if (!seen) cfg.order[cfg.orderCount++] = s;
    }
    csv = end ? end + 1 : nullptr;
  }
}

inline int sportOfMode(const char *mode) {
  for (int s = 0; s < SPORTS; ++s) if (!strcmp(mode, kModes[s])) return s;
  return -1;
}

// Is something on in this sport now (or about to start, or just over when `holding`)?
inline bool wanted(const Events &e, time_t now, const Config &cfg, bool holding) {
  for (uint8_t i = 0; i < e.count; ++i) {
    const Event &v = e.list[i];
    if (v.live && e.checkedAt && now - e.checkedAt <= kStale) return true;
    if (now >= v.start - cfg.leadSecs && now < v.end + (holding ? cfg.holdSecs : 0)) return true;
    if (holding && cfg.bridgeSecs && now >= v.end)
      for (uint8_t j = 0; j < e.count; ++j)
        if (e.list[j].start >= v.end && e.list[j].start - v.end <= cfg.bridgeSecs && now < e.list[j].start) return true;
  }
  return false;
}

// The mode to switch to, or nullptr to stay. `current` is the running mode (UM_MODE_NAME).
inline const char *decide(const Events (&events)[SPORTS], time_t now, const Config &cfg, const Log &log, const char *current) {
  if (!cfg.orderCount || now < 100000) return nullptr;  // off, or the clock has no time yet
  if (now < log.pausedUntil) return nullptr;  // chosen by hand while an event is on: left alone until it is over
  const int here = sportOfMode(current);
  const bool gapOk = !log.last || now - log.last >= cfg.minGapSecs || now < log.last;
  for (uint8_t i = 0; i < cfg.orderCount; ++i) {
    const uint8_t s = cfg.order[i];
    if (!wanted(events[s], now, cfg, here == s)) continue;
    if (here == s) return nullptr;  // already on the best sport that is on
    const uint32_t day = (uint32_t)(now / 86400);
    const uint8_t used = log.day == day ? log.count : 0;
    return gapOk && used < cfg.maxPerDay ? kModes[s] : nullptr;
  }
  // nothing is on: leave a sport face we are on for the home face
  if (here >= 0 && cfg.home[0] && strcmp(cfg.home, current) && !strcmp(log.target, current) && (!log.last || now - log.last >= 300 || now < log.last)) return cfg.home;
  return nullptr;
}

// A face that is neither the one we switched to nor one already noticed was chosen by hand: no automatic switch until the
// events that are on now are over (hold included). True when the log changed. A switch under way (the old image still runs
// for a minute) is not mistaken for it.
inline bool noticeManual(Log &log, const Events (&events)[SPORTS], time_t now, const Config &cfg, const char *current) {
  if (!log.target[0] || !strcmp(current, log.target) || !strcmp(current, log.seen) || (log.last && now >= log.last && now - log.last < 600)) return false;
  strlcpy(log.seen, current, sizeof(log.seen));
  log.pausedUntil = 0;
  // Chosen by hand, but it is the very sport the clock would pick now: nothing to override, and it goes home after the event like any other switch
  const int here = sportOfMode(current);
  for (uint8_t i = 0; i < cfg.orderCount; ++i) {
    if (!wanted(events[cfg.order[i]], now, cfg, false)) continue;
    if (cfg.order[i] == here) strlcpy(log.target, current, sizeof(log.target));
    break;
  }
  if (!strcmp(log.target, current)) return true;
  for (uint8_t i = 0; i < cfg.orderCount; ++i) {
    const Events &e = events[cfg.order[i]];
    for (uint8_t k = 0; k < e.count; ++k) {
      const Event &v = e.list[k];
      const bool on = (v.live && e.checkedAt && now - e.checkedAt <= kStale) || (now >= v.start - cfg.leadSecs && now < v.end);
      if (on && v.end + cfg.holdSecs > log.pausedUntil) log.pausedUntil = v.end + cfg.holdSecs;
    }
  }
  return true;
}

// The log after an automatic switch to `mode`
inline void noteSwitch(Log &log, time_t now, const char *mode) {
  const uint32_t day = (uint32_t)(now / 86400);
  if (log.day != day) { log.day = day; log.count = 0; }
  if (sportOfMode(mode) >= 0 && log.count < 255) ++log.count;
  log.last = now;
  strlcpy(log.target, mode, sizeof(log.target));
  log.seen[0] = 0;
  log.pausedUntil = 0;
}

}  // namespace sportsplan
