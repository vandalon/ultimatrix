// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

// The panel's colour quality levels, best first. A level is a colour depth, an I2S clock and a minimum refresh rate:
// the better the level, the more heap its DMA buffers take. When memory trouble persists after telnet and mDNS were
// shed, the clock restarts one level lower, down to the last. The level is kept in NVS with the reason it was lowered:
//  - Busy: a live match or session was running. That load is gone tomorrow, so the clock tries the best level again
//    at kRetryHour (local time) once kMinRetryS have passed and nothing live is running. The restart is planned for then,
//    not for whenever the time is up.
//  - Quiet: nothing live was running, so trying again would only fail again. It stays until it is reset on the
//    settings page.
// The settings page can also lock one level (testing, or a clock that should never change).
namespace paneltier {

struct Tier {
  uint8_t bits;         // colour depth per channel
  uint8_t mhz;          // I2S clock
  uint8_t minRefresh;   // minimum refresh in Hz: the driver merges the lowest bit planes until it is reached
  uint8_t iconPercent;  // boot screens: peak brightness of the icons, higher where the levels are coarser
  uint8_t uFloor;       // boot screens: input level at the bottom of the lit U
  uint8_t gradient;     // boot screens: how much of the icons' light-to-deep colour gradient is kept (percent); a coarse level shows less of it as banding
  const char *name;
};

constexpr Tier kTiers[] = {
    {8, 20, 72, 75, 65, 100, "8 bit, 20 MHz, 72 Hz"},
    {8, 20, 100, 75, 65, 100, "8 bit, 20 MHz"},
    {8, 8, 100, 85, 120, 50, "8 bit, 8 MHz"},
    {7, 8, 100, 100, 140, 25, "7 bit, 8 MHz"},
};
constexpr int kCount = sizeof(kTiers) / sizeof(*kTiers);
constexpr uint32_t kMinRetryS = 8 * 3600;  // at least this long after it was lowered
constexpr int kRetryHour = 11;             // and not before this hour of the local day
constexpr time_t kValidEpoch = 1700000000;

enum Reason : uint8_t { None = 0, Busy = 1, Quiet = 2 };

struct State {
  uint8_t level = 0;   // 0 = best
  uint8_t reason = None;
  uint8_t lock = 0;    // 0 automatic, else the level (1..kCount) to stay on
  uint32_t at = 0;     // epoch seconds when the level was lowered (0 = the time was not known)
};

inline State load() {
  State state;
  Preferences prefs;
  if (prefs.begin("ultimatrix", true)) {
    state.level = prefs.getUChar("ptier", 0);
    state.reason = prefs.getUChar("preason", None);
    state.lock = prefs.getUChar("plock", 0);
    state.at = prefs.getUInt("pat", 0);
    prefs.end();
  }
  if (state.level >= kCount) state.level = kCount - 1;
  if (state.lock > kCount) state.lock = 0;
  return state;
}

inline void save(const State &state) {
  Preferences prefs;
  if (!prefs.begin("ultimatrix", false)) return;
  prefs.putUChar("ptier", state.level);
  prefs.putUChar("preason", state.reason);
  prefs.putUChar("plock", state.lock);
  prefs.putUInt("pat", state.at);
  prefs.end();
}

// The level in use: the locked one, else the automatic one
inline int active(const State &state) { return state.lock ? state.lock - 1 : state.level; }

inline bool canLower(const State &state) { return !state.lock && state.level < kCount - 1; }

inline uint32_t nowEpoch() {
  time_t now = time(nullptr);
  return now >= kValidEpoch ? (uint32_t)now : 0;
}

// One level down; busy says whether live data was running
inline void lower(State &state, bool busy) {
  if (!canLower(state)) return;
  state.level++;
  state.reason = busy ? Busy : Quiet;
  state.at = nowEpoch();
  save(state);
}

// Back to the best level
inline void reset(State &state) {
  state.level = 0;
  state.reason = None;
  state.at = 0;
  save(state);
}

// A busy-time demotion that is old enough to try the best level again
inline bool retryDue(State &state) {
  if (state.lock || state.level == 0 || state.reason != Busy) return false;
  uint32_t epoch = nowEpoch();
  if (!epoch) return false;
  if (!state.at) { state.at = epoch; save(state); return false; }  // the time was unknown when it was lowered: count from now
  if (epoch < state.at || epoch - state.at < kMinRetryS) return false;
  time_t now = (time_t)epoch;
  struct tm local;
  localtime_r(&now, &local);
  return local.tm_hour >= kRetryHour;
}

}  // namespace paneltier
