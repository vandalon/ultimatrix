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
      char nbaTeams[64] = "";   // NBA: favourite teams as abbreviations, comma separated (Settings nbaTeams)
      char mlbTeams[64] = "";   // MLB: the same for baseball (Settings mlbTeams)
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
    time_t refreshNba(const Config &cfg, time_t now);
    time_t refreshMlb(const Config &cfg, time_t now);
    time_t refreshBoard(uint8_t sport, const char *url, const char *teams, time_t gameSecs, const Config &cfg, time_t now);
    void publish(uint8_t sport, const sportsplan::Events &found);
    SemaphoreHandle_t _lock = nullptr;
    sportsplan::Events _events[sportsplan::SPORTS];
    Config _cfg;
    struct Done { time_t start = 0, seen = 0; } _done[sportsplan::SPORTS][8];  // when a finished event was first seen finished: the stay after the end counts from there
    time_t finishedAt(uint8_t sport, time_t start, time_t secs, time_t now);
    sportsplan::Events _wide;  // football: the next matches of the followed leagues when the board shows none, read at most every 6 hours
    time_t _wideAt = 0;
    struct Season { time_t at = 0, tried = 0; char name[20] = ""; } _season[sportsplan::SPORTS];  // the next season's first event, looked up every 12 hours while nothing is coming
    time_t _favStart[6] = {};  // each favourite's last seen next match: ESPN moves on to the following match at kick-off, this keeps the one that is on
    volatile bool _running = false, _stop = true, _dirty = true;  // stopped until a sport is chosen
};
