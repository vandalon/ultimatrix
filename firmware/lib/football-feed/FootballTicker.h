// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <ClockTime.h>
#include <JsonScan.h>
#include <functional>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "FootballSnapshot.h"
#include "TeamKit.h"

struct FootballLeague;  // FootballCatalog.h

// Live football scores for the bottom row of a 64x32 panel: the competitions
// and favourite teams chosen on the settings page (by default Eredivisie,
// Bundesliga, Champions League, KNVB Beker, Nations League, World Cup,
// European Championship, and the Dutch and German national teams). While a Formula 1 race is on, its top 3 is one of the pages.
//
// A background task polls ESPN's scoreboard for today's matches. The clockface
// calls draw() every frame: it shows the live matches one at a time or, when
// none are live, the ones that finished today; when a score goes up,
// nextGoal() hands out a goal to celebrate full screen.
class FootballTicker {
  public:
    struct Goal {
      String home, away;
      int homeScore, awayScore;
      bool homeScored;
      TeamKit homeKit, awayKit;
      unsigned long detectedAt;
    };

    // A yellow or red card, or a substitution, to show full screen (nextIncident)
    struct Incident {
      char kind;               // 'y' yellow card, 'r' red card, 's' substitution
      String home, away;
      TeamKit homeKit, awayKit;
      bool homeTeam;           // the home team's
      uint8_t minute;
      char name[11];           // a card: the player's surname, "" when unknown
      uint8_t number;          // a card: the shirt number, a substitution: the player leaving; 0 = unknown
      uint8_t numberOn;        // a substitution: the player entering; 0 = unknown
      unsigned long detectedAt;
    };

    // A goal or card, for the timeline of faces that draw their own layout
    struct Event {
      uint8_t minute;  // 1-based playing minute, stoppage time counts on
      char kind;       // 'g' goal, 'y' yellow card, 'r' red card
      bool home;       // the home team's
      uint8_t number;  // a card: the player's shirt number, 0 = unknown
      char name[11];   // a card: the player's surname
    };

    // withRace = false leaves out the Formula 1 downloads. extras = true also
    // downloads goals and cards, the league tables and the coming matches.
    // resultWindowSecs > 0 keeps finished matches for that long instead of
    // until midnight (it then also downloads the scoreboards of the days before).
    void begin(ClockTime *dateTime, bool withRace = true, bool extras = false, uint32_t resultWindowSecs = 0);

    // Changes the result window while running; the next refresh uses it
    void setResultWindow(uint32_t secs);

    // Live, finished and coming matches and the league tables as plain fixed-size data (see FootballSnapshot.h), for the display:
    // one short lock, no files. version() changes whenever any of it does, so a face only needs to copy it then.
    void snapshot(football::Snapshot &out);
    uint32_t version();
    void resume();  // restarts the task after a firmware update; version() does it

    // How the downloads are going: nothing yet, fine, or the last attempt failed with nothing to show
    enum Status { LOADING, OK, FAILED };
    Status status();
    // True while a download runs, and a moment after it so a quick one still shows
    bool updating();

    // Takes the next goal to celebrate, if any. Goals that weren't picked up
    // within a couple of minutes (night mode, notifications) are dropped.
    bool nextGoal(Goal &goal);
    // The next card or substitution of a live match to show, false when there is none
    bool nextIncident(Incident &incident);

#ifdef UM_FOOTBALL_ONLY
    static const unsigned long STANDINGS_STEP_MS = 4000;
    static size_t standingsSteps(size_t teams) {
      return (teams + 5) / 6;
    }
#endif

#if !defined(UM_FOOTBALL_ONLY)
    // The Formula 1 race, shown as one more page of the ticker while it runs
    struct Race {
      bool live = false;
      char tag = 0;           // the session: 'P' practice, 'Q' qualifying, 'S' sprint race, 0 = the race
      char flag = 0;          // while live: 'y' = safety car or yellow flag, 'r' = red flag
      String top[3];  // 3-letter codes of the leaders
      uint16_t color[3];  // and their teams' colours
    };

#endif
  private:
    struct Match {
      String label;       // for the log
      const char *competition = "";  // short code shown by faces that draw their own layout
      uint16_t color;     // the competition's colour, for the ticker
      TeamKit homeKit, awayKit;  // the clubs' colours
      time_t kickoff;     // UTC
      char state;         // 'p'= not started, 'i' = live, 'f' = finished
      String detail;      // live: "67'" or "HT"
      String home, away;
      int homeScore, awayScore;
      time_t endedAt = 0; // UTC, finished matches only
      std::vector<Event> events;
      String league;      // ESPN code of the competition, for its table
      String id;          // ESPN's number of the match, for its summary

      String key() const { return home + "-" + away + "@" + String((uint32_t)kickoff); }
    };

    typedef std::vector<Match *> MatchList;  // pointers into _boards, valid for one refresh

    ClockTime *_dateTime = nullptr;
    bool _started = false;
    std::atomic<TaskHandle_t> _task{nullptr};  // the fetch task; none while a firmware update needs the memory
#ifndef UM_FOOTBALL_ONLY
    bool _withRace = true;
#endif
    bool _extras = false;
#ifdef UM_FOOTBALL_ONLY
    bool _showRankings = false;
    bool _cacheRestored = false;
#endif
    std::atomic<uint32_t> _resultWindowSecs{0};  // set from the clock task, read by the feed task
    // Parse buffer, allocated once at startup so a fragmented heap later on
    // can't make a download fail.

    // Only used by the background task
    std::map<String, std::pair<int, int>> _scores;  // highest score seen per match
    std::map<String, time_t> _endedAt;              // when finished matches ended

    // Last download of each competition's scoreboard. Only competitions with
    // a match on (or about to start) are downloaded on every refresh.
    struct Board {
      std::vector<Match> matches;
      uint32_t day = 0;            // local day number the matches are for
      unsigned long fetchedAt = 0;
    };
    std::map<String, Board> _boards;
    bool _haveScores = false;
    unsigned long _nationalCheckedAt = 0;
    bool _quietFetched = false;  // a quiet board was downloaded in this round: the others wait for the next one
    bool _anyLive = false;       // a match was on at the last refresh: only matches that are on or about to start are downloaded
    bool _deferredFirst = false;  // ...and one of them has no data at all yet
    bool _deferred = false;      // some download was left for the next round

    // What to follow, from the settings (only used by the background task)
    struct Favourite {
      String id, abbreviation;  // ESPN team id and the abbreviation its matches show
      bool national = false;
      String league;            // ESPN code of the competition of today's match
      uint32_t day = 0;         // local day number of that match
      String nextLeague;        // competition and kick-off of its next match, today or later
      time_t nextKickoff = 0;
    };
    std::vector<const FootballLeague *> _leagues;
    std::vector<Favourite> _favourites;
    std::atomic<uint32_t> _pageMs{8000};  // how long a match is shown, from the settings (read by the display)

#ifndef UM_FOOTBALL_ONLY
    // The Formula 1 race, shown as one more page of the ticker while it runs
    Race _race;                       // the session being run, guarded by _lock
    std::vector<Race> _results;       // the sessions finished today, guarded by _lock
    unsigned long _raceNextAt = 0;    // millis() of the next download, 0 = now
#endif
    uint32_t _day = 0;                // local day number the scores on show are from, 0 = not known yet
    std::atomic<bool> _refreshNow{false};  // asked for from the clock task; the feed task owns _footballNextAt
    std::atomic<unsigned long> _footballNextAt{0};  // millis() of the next football refresh, 0 = now

    // Only used by the background task (extras)
    // ESPN league code -> its groups (a league is one group), each the team abbreviations from the top
#ifndef UM_FOOTBALL_ONLY
    std::map<String, std::vector<std::vector<String>>> _tables;
#endif
    std::map<String, unsigned long> _tablesAt;  // millis() of each table's last download

    std::mutex _lock;
    uint8_t _cacheSlot = 0;           // which of the two cache files holds the last saved results and fixtures
    football::Snapshot _snapshot;     // published, guarded by _lock
    football::Snapshot _staging;      // only used by the background task
    bool _standingsDirty = true;      // the tables changed: _staging gets them again
    uint32_t _version = 0;            // guarded by _lock
    std::atomic<bool> _fetching{false};
    std::atomic<unsigned long> _fetchEnded{0};  // millis() of the last download's end
    Status _status = LOADING;         // guarded by _lock
    void setStatus(Status status);
    std::vector<Goal> _goals;         // guarded by _lock
    std::vector<Incident> _incidents; // guarded by _lock
    std::map<String, uint8_t> _cardCount, _subCount;  // seen so far per live match; only used by the background task
    std::map<String, unsigned long> _subChecked;      // millis() of the last summary download per match

    static void task(void *self);
    void dropOldDay();
#ifdef UM_FOOTBALL_ONLY
    bool saveBoard(const String &key, const std::vector<Match> &matches);
    void restoreBoard(const String &key);
    void releaseCachedMatches();
    bool saveMatches(const String &path, const std::vector<const Match *> &matches, bool compareOnly = false);
    bool readMatch(const String &path, size_t index, Match &out);
    void restoreOverview();
#endif
    unsigned long refresh();
#ifndef UM_FOOTBALL_ONLY
    unsigned long refreshRace();
    unsigned long fetchRace();
#endif
    bool loadConfig();
    bool followsLeague(const String &code);
    bool isFavourite(const String &abbreviation);
    bool nationalFavourite(const char *abbreviation);
    void checkFavourites(time_t today);
    bool fetchNextMatch(const char *teamId, String &league, time_t &kickoff);
    bool updateBoard(const String &key, const char *league, time_t today, const char *label, uint16_t color,
                     const char *onlyTeam);
    bool fetchScoreboard(const char *league, time_t localDay, const char *label, uint16_t color,
                         const char *onlyTeam, std::vector<Match> &out, bool withEvents = true, time_t *nextDay = nullptr);
    bool updatePastBoard(const String &key, const char *league, time_t day, const char *label, uint16_t color);
    bool updateNextBoard(const char *league, const char *label, uint16_t color);
    bool updateNextFavouriteBoard(const Favourite &fav, const String &key);
    void refreshTables(const MatchList &matches, const std::vector<const Match *> &coming);
    void tablePositions(const Match &m, uint8_t &homePos, uint8_t &awayPos);
    // Downloads url and hands the text to parse (an espn::parse... call), without keeping the text or a tree of it
    bool getScan(const char *url, const std::function<bool(JsonScan::Next, void *)> &parse);
    bool getScanNow(const char *url, const std::function<bool(JsonScan::Next, void *)> &parse);
    void trackEndTimes(MatchList &matches, time_t nowUtc);
    void detectGoals(const MatchList &matches);
    void detectCards(const MatchList &matches);
    void checkSubstitutions(const MatchList &matches);
    bool fetchSubstitutions(const Match &match, std::vector<Incident> &found, uint8_t &total);
    void addIncidents(const std::vector<Incident> &incidents);
    bool buildEntries(const MatchList &matches, time_t nowUtc);
    void fillStandings(football::Snapshot &out);
    void buildSnapshot(const std::vector<const Match *> &live, const std::vector<const Match *> &finished,
                       const std::vector<const Match *> &next);
    void buildOverview(const MatchList &matches, const std::vector<const Match *> &coming, time_t nowUtc);
};
