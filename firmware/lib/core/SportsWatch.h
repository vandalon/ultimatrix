// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include "JsonScan.h"
#include "SportsPlan.h"

// Reads the schedules of the sports the user lets take over the panel (F1 and Football from ESPN, MotoGP from its own
// API) in one small task that only runs while at least one sport is chosen. The running face does not matter: each sport
// is looked up the same way, so a Football clock still sees when the F1 session starts. The decision itself is in
// SportsPlan.h; main.cpp asks events() and switches the face with the normal signed install.
class SportsWatch {
  public:
    struct Config {
      bool sports[sportsplan::SPORTS] = {};
      uint8_t racing = 0;       // 0 qualifying and races, 1 races only, 2 every session
      bool favouritesOnly = true;
      bool othersFav = true;    // Football: also each favourite's next match, wherever it is played
      char leagues[420] = "";   // Football: ESPN league codes, comma separated (Settings footballLeagues)
      char teams[420] = "";     // Football: "id:ABBR:n" entries, comma separated (Settings footballTeams)
    };
    static SportsWatch &instance();
    void configure(const Config &cfg);  // starts, restarts or stops the task as needed
    void poll();                        // main loop: starts the task again after an update, stops it while one runs
    void events(sportsplan::Events (&out)[sportsplan::SPORTS]);

  private:
    static void task(void *self);
    void loop();
    bool get(const char *url, JsonScan::Leaf leaf, void *sink);
    time_t refreshF1(const Config &cfg, time_t now);
    time_t refreshMotoGp(const Config &cfg, time_t now);
    time_t refreshFootball(const Config &cfg, time_t now);
    void publish(uint8_t sport, const sportsplan::Events &found);
    SemaphoreHandle_t _lock = nullptr;
    sportsplan::Events _events[sportsplan::SPORTS];
    Config _cfg;
    time_t _favStart[6] = {};  // each favourite's last seen next match: ESPN moves on to the following match at kick-off, this keeps the one that is on
    volatile bool _running = false, _stop = true, _dirty = true;  // stopped until a sport is chosen
};
