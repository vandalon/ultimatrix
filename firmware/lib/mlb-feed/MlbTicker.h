// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <JsonScan.h>
#include "MlbTypes.h"

// ESPN's MLB feeds in one task: today's scoreboard (often), the next days' games and the standings (every half hour).
// The result is a Snapshot the face copies out under a lock.
namespace mlb {

class Ticker {
 public:
  void begin();
  // After an update check the task is gone: start it again once the update is over
  void resume();
  uint32_t version() const { return _version.load(); }
  void snapshot(Snapshot &out);
  bool ok() const { return _ok.load(); }

 private:
  static void task(void *self);
  bool refreshToday();
  bool refreshFixtures();
  bool refreshStandings();
  bool get(const char *url, JsonScan::Leaf leaf, void *sink);

  void mergeSeries();  // _data.series from the two lists below; the lock is held

  Series _seriesToday[kMaxSeries], _seriesLater[kMaxSeries];  // the series of today's games, and of the coming days' that today has none of
  uint8_t _seriesTodayCount = 0, _seriesLaterCount = 0;
  std::mutex _lock;
  Snapshot _data;
  std::atomic<uint32_t> _version{0};
  std::atomic<bool> _ok{false};
  std::atomic<TaskHandle_t> _task{nullptr};
  bool _started = false;
  unsigned long _todayAt = 0, _fixturesAt = 0, _standingsAt = 0;  // millis() of the next read; 0 = now
};

}  // namespace mlb
