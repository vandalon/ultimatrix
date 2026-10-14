// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <ClockTime.h>
#include <mutex>

#include "F1Live.h"
#include "JsonScan.h"

// Formula 1 data for a 64x64 panel, downloaded by a background task:
//  - the current Grand Prix weekend from ESPN's scoreboard: the session being run (order, lap or clock,
//    flag), the last session that finished and the next one
//  - the coming Grand Prix, from ESPN's season calendar
//  - the top of the drivers' championship, from Jolpica (the Ergast successor)
// ESPN gives the running order but no gaps or lap times.
struct F1SeenStart { uint32_t id = 0, start = 0; };

class F1Ticker {
  public:
    static const int ROWS = 22;     // positions kept per session: the whole grid
    static const int TIMED = 9;     // of those, the first ones whose times ESPN is asked for (a request each)
    static const int STANDINGS = 22;  // the whole championship, so a favourite driver can be shown too
    static const int UPCOMING = 4;

    struct Row {
      char code[4] = "";  // "VER"
      uint16_t color = 0xFFFF;  // the team's colour
      bool finished = false;   // live race: this driver has taken the chequered flag
      uint32_t athleteId = 0;   // ESPN's, for the times
      char time[9] = "";        // first place: the best lap or "LEAD"; others: the gap, "+.088" or "+12.3"
      bool fastest = false;     // a race: the fastest lap of all
      bool pit = false;         // in the pit lane now
      bool improved = false;    // qualifying: his best lap just got better, shown for a few seconds
    };

    struct Session {
      bool valid = false;
      bool live = false;
      char tag = 'R';         // 'P' practice, 'Q' qualifying, 'S' sprint race, 'R' race
      char name[8] = "";      // FP1, QUALI, SQ, SPRINT, RACE
      char flag = 0;          // while live: 'y' yellow, 's' safety car, 'v' virtual safety car, 'r' red
      bool fromFeed = false;  // while live: the rows are from the live feed, so a row without a time has none yet
      bool finished = false;  // while live: the feed says the session is over (a race: the leader took the chequered flag)
      int period = 0;         // the lap, for races
      int totalLaps = 0;      // of the race, from the live feed
      char clock[8] = "";     // time left, for practice and qualifying
      time_t finishedAt = 0;  // UTC of the leader taking the flag, from the live feed
      time_t start = 0;       // UTC
      char status = 0;        // a session still to come: 'd' delayed (its start has passed), 'n' it has a new start time
      uint32_t eventId = 0, id = 0;  // ESPN's, for the times
      Row rows[ROWS];
      uint8_t count = 0;
      uint8_t timeFails = 0;  // refreshes in a row that got no times
      bool stale = false;     // the rows are the ones kept in flash from before the boot: nothing new downloaded yet
    };

    struct Standing {
      char code[4] = "";
      char name[16] = "";  // the surname as the championship shows it; empty: the face knows the driver by his code
      uint16_t color = 0xFFFF;
      uint16_t points = 0;
    };

    struct Race {
      char city[20] = "";  // upper case, ASCII
      char place[20] = "";  // the circuit's locality, the same way: shown when the city does not fit
      char circuit[48] = "";  // the circuit's name and place, to find its outline
      time_t start = 0;    // UTC, the first practice session
    };

    struct Snapshot {
      bool weekend = false;  // a Grand Prix weekend is on, whether or not a session is running
      char circuit[48] = "";  // ESPN's name of the circuit and its city, to find its outline
      Session live, last, next;
      // The sessions still to come this weekend, soonest first (the first is `next`)
      struct Coming { char name[8] = ""; time_t start = 0; char status = 0; };
      static const int COMING = 6;
      Coming coming[COMING];
      uint8_t comingCount = 0;
      Standing standings[STANDINGS];
      uint8_t standingsCount = 0;
      bool standingsStale = false;  // the standings are the ones kept in flash: not downloaded since the boot
      Race upcoming[UPCOMING];  // soonest first
      uint8_t upcomingCount = 0;
    };

    void begin(ClockTime *dateTime);
    void snapshot(Snapshot &out, bool intervals = false);
    // Changes whenever a download changed anything
    uint32_t version();
    void resume();  // restarts the task after a firmware update; version() does it

    // The team colour of a driver by his 3-letter code, white when unknown
    static uint16_t driverColor(const char *code);

  private:
    static void task(void *self);
    unsigned long refresh();
    unsigned long fetchWeekend();
    unsigned long fetchStandings();
    unsigned long fetchSeason();
    void fetchTimes(Session &session, int count);
    bool fetchOpenF1(Session &session);  // a finished practice or qualifying session's times in a few requests, from OpenF1
    bool fetchCarNumbers(uint32_t sessionKey);  // car number to driver code, kept in flash
    const char *carCode(int number);
    bool fetchFastest(Session &session);  // marks the driver with the fastest lap of a finished race or sprint
    bool getScan(const char *url, JsonScan::Leaf leaf, void *sink);  // streams the JSON of a download through the scanner
    void persistLive();  // the latest order and times of a session to flash
    void loadCache();  // the standings and the next race of the last download, from flash

    ClockTime *_dateTime = nullptr;
    std::mutex _lock;
    Snapshot _snap;
    F1Live _live;
    uint32_t _fastestId = 0;  // the session the fastest lap was last looked up for, and when
    unsigned long _fastestAt = 0;
    uint32_t _timedId = 0;  // the session whose times were downloaded, and how often
    uint8_t _timedTries = 0;
    uint8_t _liveFails = 0;
    struct CarNumber { uint8_t number = 0; char code[4] = ""; };  // numbers stay the same all season
    static const int CARS = 32;
    CarNumber _cars[CARS];
    uint8_t _carCount = 0;
    uint32_t _carEvent = 0;  // the race weekend (ESPN's event) the numbers are from
    bool _feedPaced = false;  // ESPN is polled slowly because the live feed is up to date
    uint32_t _openId = 0, _openKey = 0;  // the session (ESPN's id) and its number at OpenF1
    // When each kind of data last came in whole (UTC, 0: not yet since the boot), for the "renewed" log line
    struct Renewed { time_t weekend = 0, times = 0, standings = 0, season = 0; const char *timesFrom = ""; } _renewed;
    void logRenewed();
    Session _lastTimed;  // the last session once its times are in, so they are downloaded once
    // The latest live order with its times, kept in flash too: shown while no new data is there
    struct SavedRows {
      uint32_t id = 0;  // the session
      char name[8] = "";
      char flag = 0;
      uint8_t count = 0;
      Row rows[ROWS];
    };
    SavedRows _saved;
    bool _savedFromFlash = false;  // _saved still holds what was in flash at the boot
    bool _savedDirty = false;
    unsigned long _savedAt = 0;
    uint32_t _delayed[8] = {};  // sessions seen delayed, so a later start time is shown as a new one
    uint8_t _delayedCount = 0;
    F1SeenStart _seen[8];  // the first start time seen of each session to come, to notice a start that moved
    uint8_t _seenCount = 0;
    uint32_t _version = 0;
    bool _started = false;
    bool _cacheLoaded = false;
    std::atomic<TaskHandle_t> _task{nullptr};  // the download task; none while a firmware update needs the memory
    unsigned long _weekendAt = 0, _standingsAt = 0, _seasonAt = 0;  // millis() of the next download
};
