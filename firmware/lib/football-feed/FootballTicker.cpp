// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "FootballTicker.h"
#include <NetworkBudget.h>
#include "GoalAnimation.h"
#include "TeamColors.h"

#include <algorithm>
#include <cstdlib>
#include <esp_heap_caps.h>
#include "EspnParse.h"
#ifndef UM_FOOTBALL_ONLY
#include <Fonts/Picopixel.h>
#endif
#include <HTTPClient.h>
#include <Console.h>
#include <mutex>
#include <WiFi.h>
#include <Preferences.h>
#include <TimeService.h>
#ifdef UM_FOOTBALL_ONLY
#include <LittleFS.h>
#include <DiskCache.h>
#endif

#include <Settings.h>
#include <EspnFeed.h>
#ifndef UM_FOOTBALL_ONLY
#include <F1Drivers.h>
#endif
#include "FootballCatalog.h"

// A live match of a favourite team: the football face's reason to keep the birthday screen off
static bool favouriteLive(const football::Snapshot &s) {
  for (size_t i = 0; i < s.liveCount; i++) if (s.live[i].favourite) return true;
  return false;
}

// Which competitions and favourite teams to follow is set on the settings page (see FootballCatalog.h
// for what can be chosen). Matches are drawn in their competition's colour.
static const size_t MAX_LEAGUES = 10, MAX_FAVOURITES = 8;  // every one is a download

static const FootballLeague *findLeague(const String &code) {
  for (const auto &league : FOOTBALL_LEAGUES)
    if (code == league.code) return &league;
  return nullptr;
}

// A favourite's matches in the competition's colour; the Netherlands and Germany keep their own
static uint16_t favouriteColor(const String &abbreviation, bool national, const String &league) {
  if (national && abbreviation == "NED") return 0xFC00;  // orange
  if (national && abbreviation == "GER") return 0xFEA0;  // gold; black wouldn't show
  const FootballLeague *known = findLeague(league);
  return known ? known->color : 0xFFFF;
}

static std::vector<String> splitList(const String &list) {
  std::vector<String> out;
  int start = 0;
  while (start <= (int)list.length()) {
    int end = list.indexOf(',', start);
    if (end < 0) end = list.length();
    String item = list.substring(start, end);
    item.trim();
    if (!item.isEmpty()) out.push_back(item);
    start = end + 1;
  }
  return out;
}

// The short code of a competition for faces that name it: the league's label from the catalog.
// Matches of the national teams in any other competition are "INT".
static const char *competitionCode(const char *league, bool nationalTeam) {
  for (const auto &entry : FOOTBALL_LEAGUES)
    if (strcmp(entry.code, league) == 0) return entry.label;
  return nationalTeam ? "INT" : "";
}

// ESPN's "rrggbb" as RGB565, 0 when missing
static uint16_t hexColor(const char *hex) {
  if (!hex || strlen(hex) != 6) return 0;
  long v = strtol(hex, nullptr, 16);
  return ((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F);
}

// Bright enough to show on the LEDs
static bool visible(uint16_t c) { return (c >> 11) >= 8 || ((c >> 5) & 63) >= 16 || (c & 31) >= 8; }

// Bright enough for small two-tone details
static bool showsAsDetail(uint16_t c) { return (c >> 11) >= 12 || ((c >> 5) & 63) >= 24 || (c & 31) >= 12; }

// A team's home kit. Known teams come from TeamColors; for others ESPN's
// colours are used. ESPN's second colour is sometimes the away kit (Germany's
// turquoise) but for teams not in the table it's the best there is. Black
// doesn't show on the LEDs, so a dark colour falls back to white or none.
static TeamKit clubKit(const String &team, const char *league, const char *main, const char *alt) {
  TeamKit kit;
  if (lookupTeamKit(team, league, kit)) return kit;
  uint16_t a = hexColor(main), b = hexColor(alt);
  kit.shirt = visible(a) ? a : visible(b) ? b : 0xFFFF;
  kit.shorts = visible(b) && b != kit.shirt ? b : 0xFFFF;
  kit.second = showsAsDetail(b) && b != kit.shirt ? b : 0;
  return kit;
}

// Matches that finished today stay on the ticker for the rest of the day, but
// only while no match is live, and at most MAX_SHOWN of them.
static const size_t MAX_SHOWN = 12;
#ifndef UM_FOOTBALL_ONLY
static const unsigned long RACE_SLIDE_MS = 600;  // places swapping in the F1 row
#endif
// End of a match we only saw after it finished: kick-off + 2 halves, half
// time and stoppage time
static const time_t TYPICAL_MATCH_SECS = 115 * 60;
static const int16_t CHAR_WIDTH = 6;        // default 5x7 font + 1px spacing

static const unsigned long LIVE_REFRESH_MS = 10 * 1000UL;  // also how fast goals show up
// Competitions without a match on are downloaded less often, to keep the
// load on ESPN's (unofficial, unmetered) feed low during live matches
static const unsigned long QUIET_BOARD_MS = 60 * 60 * 1000UL;
static const unsigned long BOARD_RETRY_MS = 5 * 60 * 1000UL;  // after a failed download of a coming or past board
static const unsigned long KICKOFF_REFRESH_MS = 60 * 1000UL;
static const unsigned long TODAY_REFRESH_MS = 10 * 60 * 1000UL;
static const unsigned long IDLE_REFRESH_MS = 30 * 60 * 1000UL;
static const unsigned long RETRY_MS = 30 * 1000UL;
static const unsigned long NATIONAL_CHECK_MS = 3 * 60 * 60 * 1000UL;
static const unsigned long CONFIG_POLL_MS = 2 * 1000UL;
static const unsigned long GOAL_EXPIRY_MS = 2 * 60 * 1000UL;
static const unsigned long SUB_REFRESH_MS = 45 * 1000UL;  // how often a live match's summary is read for substitutions
// Extras (begin(..., extras = true)): the coming matchdays and the league tables change slowly
static const unsigned long NEXT_REFRESH_MS = 60 * 60 * 1000UL;
static const unsigned long TABLES_REFRESH_MS = 3 * 60 * 60 * 1000UL;
#ifdef UM_FOOTBALL_ONLY
static const int NEXT_EXTRA_DAYS = 6;  // the rest of the coming round, including midweek fixtures
#else
static const int NEXT_EXTRA_DAYS = 2;
#endif
// Downloads that aren't urgent (a quiet league, the coming matches) go one league at a time, this far apart
static const unsigned long STAGGER_MS = 15 * 1000UL;
static const unsigned long FIRST_STAGGER_MS = 2 * 1000UL;  // leagues or teams that have no data yet follow each other quickly
static const uint32_t TASK_STACK_BYTES = 14336;  // a busy evening needs more than a quiet one (the retry without events)
extern std::atomic<uint32_t> memoryAllocationFailures;  // main.cpp
static const uint32_t MIN_FREE_BLOCK = 10000;    // below this largest free block of data memory a refresh is skipped: an out of memory aborts the chip
// Data memory only: ESP.getFreeHeap() and getMaxAllocHeap() include 45 KB of instruction RAM that no data can use
static size_t dataFree() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
static size_t dataBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

#ifdef UM_FOOTBALL_ONLY
static bool nationalTeamCompetition(const char *code) {
  static const char *const national[] = {"uefa.nations", "fifa.world", "uefa.euro", "fifa.worldq.uefa", "uefa.euroq"};
  for (const char *entry : national)
    if (strcmp(code, entry) == 0) return true;
  return false;
}
#endif
#ifdef UM_FOOTBALL_ONLY
static const size_t MAX_UPCOMING = 16;
#else
static const size_t MAX_UPCOMING = 24;
#endif

// The debug messages go to the serial port and the telnet server. One lock: the ticker task and the clock loop
// both write to the telnet stream.
static void logf(const char *format, ...) {
  static std::mutex lock;
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  std::lock_guard<std::mutex> guard(lock);
  Serial.print(line);
  Console.print(line);
}

#ifdef UM_FOOTBALL_ONLY
static bool cacheReady = false;
static std::mutex cacheLock;

struct CachedMatch {
  char home[8], away[8], league[24], label[8], id[20], detail[32];
  TeamKit homeKit, awayKit;
  int64_t kickoff, endedAt;
  int16_t homeScore, awayScore;
  uint16_t color, events;
  char state;
};

struct CachedTeam {
  char name[8];
  int16_t points;
};

struct CachedOverview {
  uint32_t magic;
  uint16_t favouriteResults, comingToday;
  uint8_t finishedCount, upcomingCount, slot;
};

// The cache's tag folds in the sizes of what is stored, so a changed layout reads as no cache. Bump the base for changes that keep every size.
static const uint32_t CACHE_MAGIC = 0x46424331 ^ ((uint32_t)sizeof(CachedMatch) << 20) ^ ((uint32_t)sizeof(FootballTicker::Event) << 12) ^ ((uint32_t)sizeof(CachedTeam) << 6) ^ (uint32_t)sizeof(CachedOverview);

static String displayCachePath(const char *kind, uint8_t slot) { return String("/um-fb-") + kind + (slot ? "-b" : "-a"); }

static String cachePath(const char *kind, const String &key) {
  uint32_t hash = 2166136261UL;
  for (unsigned i = 0; i < key.length(); i++) hash = (hash ^ (uint8_t)key[i]) * 16777619UL;
  char path[32];
  snprintf(path, sizeof(path), "/um-fb-%s-%08lx", kind, (unsigned long)hash);
  return String(path);
}

template <typename T> static bool cacheRead(File &file, T &out) {
  return file.read((uint8_t *)&out, sizeof(out)) == sizeof(out);
}

template <typename T> static bool cacheWrite(File &file, const T &value) {
  return file.write((const uint8_t *)&value, sizeof(value)) == sizeof(value);
}

static bool cacheHeader(File &file, uint16_t &count) {
  uint32_t magic = 0;
  return file && cacheRead(file, magic) && magic == CACHE_MAGIC && cacheRead(file, count) && count <= 256;
}

// Writes go to a temporary file; identical downloads leave the existing cache alone.
static bool cacheCommit(const String &path) {
  String temporary = path + ".tmp";
  File next = LittleFS.open(temporary, "r"), previous = LittleFS.open(path, "r");
  bool same = next && previous && next.size() == previous.size();
  uint8_t a[64], b[64];
  while (same && next.available()) {
    size_t count = next.read(a, sizeof(a));
    same = previous.read(b, count) == count && memcmp(a, b, count) == 0;
  }
  next.close();
  previous.close();
  if (same) {
    LittleFS.remove(temporary);
    return true;
  }
  return LittleFS.rename(temporary, path);
}

bool FootballTicker::saveMatches(const String &path, const std::vector<const Match *> &matches, bool compareOnly) {
  if (!cacheReady) return false;
  std::lock_guard<std::mutex> guard(cacheLock);
  auto record = [](const Match *m) {
    CachedMatch saved;
    memset(&saved, 0, sizeof(saved));
    strlcpy(saved.home, m->home.c_str(), sizeof(saved.home));
    strlcpy(saved.away, m->away.c_str(), sizeof(saved.away));
    strlcpy(saved.league, m->league.c_str(), sizeof(saved.league));
    strlcpy(saved.label, m->label.c_str(), sizeof(saved.label));
    strlcpy(saved.id, m->id.c_str(), sizeof(saved.id));
    strlcpy(saved.detail, m->detail.c_str(), sizeof(saved.detail));
    saved.homeKit = m->homeKit;
    saved.awayKit = m->awayKit;
    saved.kickoff = m->kickoff;
    saved.endedAt = m->endedAt;
    saved.homeScore = m->homeScore;
    saved.awayScore = m->awayScore;
    saved.color = m->color;
    saved.state = m->state;
    saved.events = min(m->events.size(), size_t(128));
    return saved;
  };
  File previous = LittleFS.open(path, "r");
  uint16_t oldCount = 0;
  bool same = cacheHeader(previous, oldCount) && oldCount == matches.size();
  for (const Match *m : matches) {
    if (!same) break;
    CachedMatch saved = record(m), old;
    same = cacheRead(previous, old) && memcmp(&saved, &old, sizeof(saved)) == 0;
    for (size_t i = 0; same && i < saved.events; i++) {
      Event event;
      same = cacheRead(previous, event) && memcmp(&event, &m->events[i], sizeof(event)) == 0;
    }
  }
  previous.close();
  if (same) return true;
  if (compareOnly) return false;
  File file = LittleFS.open(path + ".tmp", "w");
  uint16_t count = matches.size();
  bool ok = file && cacheWrite(file, CACHE_MAGIC) && cacheWrite(file, count);
  for (const Match *m : matches) {
    CachedMatch saved = record(m);
    ok &= cacheWrite(file, saved);
    for (size_t i = 0; i < saved.events; i++) ok &= cacheWrite(file, m->events[i]);
  }
  file.close();
  if (!ok) {
    LittleFS.remove(path + ".tmp");
    return false;
  }
  return cacheCommit(path);
}

bool FootballTicker::saveBoard(const String &key, const std::vector<Match> &matches) {
  std::vector<const Match *> saved;
  for (const Match &m : matches)
    if (m.state != 'i') saved.push_back(&m);
  return saveMatches(cachePath("board", key), saved);
}

bool FootballTicker::readMatch(const String &path, size_t index, Match &out) {
  if (!cacheReady) return false;
  std::lock_guard<std::mutex> guard(cacheLock);
  File file = LittleFS.open(path, "r");
  uint16_t count = 0;
  if (!cacheHeader(file, count) || index >= count) return false;
  CachedMatch saved;
  for (size_t i = 0; i <= index; i++) {
    if (!cacheRead(file, saved) || saved.events > 128 || (saved.state != 'p' && saved.state != 'f')) return false;
    if (i != index) {
      if (!file.seek(file.position() + saved.events * sizeof(Event))) return false;
      continue;
    }
    if (!memchr(saved.home, 0, sizeof(saved.home)) || !memchr(saved.away, 0, sizeof(saved.away)) ||
        !memchr(saved.league, 0, sizeof(saved.league)) || !memchr(saved.label, 0, sizeof(saved.label)) ||
        !memchr(saved.id, 0, sizeof(saved.id)) || !memchr(saved.detail, 0, sizeof(saved.detail))) return false;
    out.home = saved.home;
    out.away = saved.away;
    out.league = saved.league;
    out.label = saved.label;
    out.id = saved.id;
    out.detail = saved.detail;
    out.competition = competitionCode(saved.league, nationalFavourite(saved.home) || nationalFavourite(saved.away));
    out.homeKit = saved.homeKit;
    out.awayKit = saved.awayKit;
    out.kickoff = saved.kickoff;
    out.endedAt = saved.endedAt;
    out.homeScore = saved.homeScore;
    out.awayScore = saved.awayScore;
    out.color = saved.color;
    out.state = saved.state;
    out.events.clear();
    for (size_t e = 0; e < saved.events; e++) {
      Event event;
      if (!cacheRead(file, event)) return false;
      out.events.push_back(event);
    }
  }
  return true;
}

void FootballTicker::restoreBoard(const String &key) {
  Board &board = _boards[key];
  time_t nowUtc = ultime::now();
  uint32_t today = _dateTime->localNow() / SECS_PER_DAY;
  Match m;
  for (size_t i = 0; readMatch(cachePath("board", key), i, m); i++) {
    if (m.state == 'f' && _dateTime->utcToLocal(m.kickoff) / SECS_PER_DAY != today &&
        (!_resultWindowSecs || m.endedAt + (time_t)_resultWindowSecs <= nowUtc)) continue;
    if (m.state == 'p' && m.kickoff + TYPICAL_MATCH_SECS < nowUtc) continue;
    bool known = false;
    for (const Match &o : board.matches)
      known |= o.kickoff == m.kickoff && o.home == m.home && o.away == m.away;
    if (!known) board.matches.push_back(std::move(m));
  }
}

void FootballTicker::releaseCachedMatches() {
  bool released = false;
  for (auto &entry : _boards) {
    auto &matches = entry.second.matches;
    size_t count = matches.size();
    matches.erase(std::remove_if(matches.begin(), matches.end(), [](const Match &m) { return m.state != 'i'; }), matches.end());
    released |= matches.size() != count;
    if (matches.empty()) std::vector<Match>().swap(matches);
  }
  auto liveKey = [&](const String &key) {
    for (const auto &board : _boards)
      for (const Match &m : board.second.matches) {
        char wanted[64];
        snprintf(wanted, sizeof(wanted), "%s-%s@%lu", m.home.c_str(), m.away.c_str(), (unsigned long)(uint32_t)m.kickoff);
        if (key == wanted) return true;
      }
    return false;
  };
  for (auto it = _scores.begin(); it != _scores.end();)
    it = liveKey(it->first) ? std::next(it) : _scores.erase(it);
  for (auto it = _cardCount.begin(); it != _cardCount.end();)
    it = liveKey(it->first) ? std::next(it) : _cardCount.erase(it);
  _endedAt.clear();
  if (released) logf("[Football] live data retained, data free %u, largest block %u\n", (unsigned)dataFree(), (unsigned)dataBlock());
}

#endif

void FootballTicker::setResultWindow(uint32_t secs) {
  if (secs == _resultWindowSecs) return;
  _resultWindowSecs = secs;
  _refreshNow = true;  // download again now: a longer window needs the boards of the days before
}

void FootballTicker::begin(ClockTime *dateTime, bool withRace, bool extras, uint32_t resultWindowSecs) {
  if (_started) return;
  _started = true;
#ifndef UM_FOOTBALL_ONLY
  _withRace = withRace;
#else
  (void)withRace;
#endif
  _extras = extras;
  _resultWindowSecs = resultWindowSecs;
  _dateTime = dateTime;
#ifdef UM_FOOTBALL_ONLY
  cacheReady = beginDiskCache();
  if (!cacheReady) logf("[Football] flash cache unavailable\n");
#endif
  loadConfig();
#ifdef UM_FOOTBALL_ONLY
  if (cacheReady && ultime::timeStatus() == timeSet) {
    try {
      _day = _dateTime->localNow() / SECS_PER_DAY;
      restoreOverview();
      _cacheRestored = true;
    } catch (const std::exception &e) {
      logf("[Football] Could not restore boot cache (%s)\n", e.what());
    }
  }
#endif
  feedTrimArg = this;
  feedTrimFn = [](void *self) { static_cast<FootballTicker *>(self)->trim(); };
  // Core 0, so fetching never stalls the display loop on core 1
  resume();
}

void FootballTicker::trim() {
  if (_task) return;  // only while the task is gone
  // Keep only live-match baselines, so returning from WLED cannot replay old events.
  auto liveKey = [&](const String &key) {
    for (const auto &board : _boards)
      for (const Match &m : board.second.matches) {
        if (m.state != 'i') continue;
        char live[64];
        snprintf(live, sizeof(live), "%s-%s@%lu", m.home.c_str(), m.away.c_str(), (unsigned long)(uint32_t)m.kickoff);
        if (key == live) return true;
      }
    return false;
  };
  for (auto it = _scores.begin(); it != _scores.end();)
    it = liveKey(it->first) ? std::next(it) : _scores.erase(it);
  for (auto it = _cardCount.begin(); it != _cardCount.end();)
    it = liveKey(it->first) ? std::next(it) : _cardCount.erase(it);
  for (auto it = _subCount.begin(); it != _subCount.end();)
    it = liveKey(it->first) ? std::next(it) : _subCount.erase(it);
  _subChecked.clear();
  _boards.clear();
  _tablesAt.clear();
  _staging.reset();
  _standingsDirty = true;
  _footballNextAt = 0;
  {
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<Goal>().swap(_goals);
    std::vector<Incident>().swap(_incidents);
  }
#ifndef UM_FOOTBALL_ONLY
  std::lock_guard<std::mutex> guard(_lock);
  _tables.clear();
  std::vector<Race>().swap(_results);
#endif
}

// After an update check the task is gone (see task()): start it again once the update is over
void FootballTicker::resume() {
  if (!_started || _task || feedsHalted()) return;
  // No task when the heap has not recovered yet (the stack does not fit): try again at the next frame
  if (createFeedTask(task, "football", TASK_STACK_BYTES, this, 1, &_task, 0) != pdPASS) _task = nullptr;
}


void FootballTicker::task(void *self) {
  waitForFeedRegistration();
  FootballTicker *ticker = static_cast<FootballTicker *>(self);
  for (;;) {
    if (feedsHalted()) {  // the update has the network and the heap to itself
      // The TLS handshake needs big free blocks: hand back the task stack.
      // The scores stay in the ticker; resume() starts the task again after the update.
      ticker->_fetching = false;
      ticker->_task = nullptr;
      stopFeedTask();
    }
    unsigned long wait = RETRY_MS;
#ifdef UM_FOOTBALL_ONLY
    if (!ticker->_cacheRestored && cacheReady && ultime::timeStatus() == timeSet) {
      try {
        ticker->_day = ticker->_dateTime->localNow() / SECS_PER_DAY;
        ticker->restoreOverview();
        ticker->_cacheRestored = true;
      } catch (const std::exception &e) {
        logf("[Football] Could not restore boot cache (%s)\n", e.what());
      }
    }
#endif
    if (dataBlock() < MIN_FREE_BLOCK) {
      // C++ exceptions are on: a failed allocation that nobody catches restarts the clock. Wait for the heap to recover.
      memoryAllocationFailures.fetch_add(1, std::memory_order_relaxed);  // tells main's guard to shed telnet and mDNS
      logf("[Football] heap too low, skipping a refresh (data free %u, largest block %u)\n", (unsigned)dataFree(),
           (unsigned)dataBlock());
    } else {
      try {
        ticker->dropOldDay();
        wait = ticker->refresh();
#ifdef UM_FOOTBALL_RACE
        if (ticker->_withRace) wait = std::min(wait, ticker->refreshRace());
#endif
      } catch (const std::exception &e) {  // std::bad_alloc: keep the data of the last round
        logf("[Football] out of memory in a refresh (%s), data free %u, largest block %u\n", e.what(), (unsigned)dataFree(),
             (unsigned)dataBlock());
        ticker->_fetching = false;
        ticker->_footballNextAt = millis() + RETRY_MS;
        wait = RETRY_MS;
      }
    }
    wait = std::min(wait, CONFIG_POLL_MS);  // a change on the settings page is picked up within seconds
    // in slices, so an update that starts meanwhile is seen within a second
    for (unsigned long left = wait; left > 0 && !feedsHalted();) {
      unsigned long slice = std::min(left, 1000UL);
      vTaskDelay(pdMS_TO_TICKS(slice));
      left -= slice;
    }
  }
}

// How long until the next local midnight, when the day's scores are dropped
static unsigned long untilMidnightMs(time_t localNow) {
  return (SECS_PER_DAY - localNow % SECS_PER_DAY + 2) * 1000UL;
}

// At the first moment of a new local day all scores go, finished matches and
// F1 results included, so the clock shows its date until today's matches are
// known. Wiped even if the new downloads fail, so yesterday never lingers.
void FootballTicker::dropOldDay() {
  if (ultime::timeStatus() == timeNotSet) return;
  uint32_t todayNr = _dateTime->localNow() / SECS_PER_DAY;
  if (todayNr == _day) return;
  _day = todayNr;
  _footballNextAt = 0;
  std::lock_guard<std::mutex> guard(_lock);
  if (!_resultWindowSecs) {
    _cacheSlot = 0;
    _snapshot.liveCount = _snapshot.resultCount = _snapshot.fixtureCount = 0;
    _version++;
  }
#ifndef UM_FOOTBALL_ONLY
  _results.clear();
  _race = Race();
#endif
  liveEventOn = _snapshot.liveCount > 0;
  liveFavouriteOn = favouriteLive(_snapshot);
}

// Fetches today's matches, updates the ticker and looks for goals.
// Returns how long to wait before the next refresh.
unsigned long FootballTicker::refresh() {
  if (WiFi.status() != WL_CONNECTED) setStatus(FAILED);
  if (WiFi.status() != WL_CONNECTED || ultime::timeStatus() != timeSet)
    return 10 * 1000UL;
#ifdef UM_FOOTBALL_ONLY
  struct CacheRound {
    FootballTicker *ticker;
    ~CacheRound() { ticker->releaseCachedMatches(); }
  } cacheRound{this};
#endif
  // Other leagues or favourites chosen: start over with what is followed now
  if (loadConfig()) {
    _boards.clear();
    _footballNextAt = 0;
    _nationalCheckedAt = 0;
#ifdef UM_FOOTBALL_ONLY
    _cacheRestored = false;
    _standingsDirty = true;
#endif
  }
#ifdef UM_FOOTBALL_ONLY
  if (!_cacheRestored) {
    restoreOverview();
    _cacheRestored = true;
  }
#endif
  // The task also wakes up for the F1 timer: only refresh when football is due
  unsigned long start = millis();
  if (_refreshNow.exchange(false)) _footballNextAt = 0;
  unsigned long due = _footballNextAt;
  if (due && (long)(start - due) < 0) return due - start;

  time_t today = _dateTime->localNow();
  uint32_t todayNr = today / SECS_PER_DAY;
  _quietFetched = false;
  _deferred = false;
  _deferredFirst = false;
  checkFavourites(today);

  // Favourites first: a match that is also in one of the followed leagues is then skipped as a
  // duplicate, so it keeps the favourite's colour. A club whose league is followed anyway is on
  // that league's board already.
  std::vector<String> keys;
  int boards = 0, failed = 0;  // one failing board (a bad team id, one league down) must not hold up the others
  for (Favourite &fav : _favourites) {
    if (fav.day != todayNr || (!fav.national && followsLeague(fav.league))) continue;
    String key = fav.league + "/" + fav.abbreviation;
    boards++;
    failed += !updateBoard(key, fav.league.c_str(), today, fav.abbreviation.c_str(),
                      favouriteColor(fav.abbreviation, fav.national, fav.league), fav.abbreviation.c_str());
    keys.push_back(key);
  }
  for (const FootballLeague *league : _leagues) {
    boards++;
    failed += !updateBoard(league->code, league->code, today, league->label, league->color, nullptr);
    keys.push_back(league->code);
  }
  if (boards && failed == boards) {  // nothing came in: keep showing the previous data
    setStatus(FAILED);
    _footballNextAt = millis() + RETRY_MS;
    return RETRY_MS;
  }
  _anyLive = false;
  for (const auto &board : _boards)
    for (const Match &match : board.second.matches) _anyLive |= match.state == 'i';
  liveEventOn = _anyLive;
  // With a result window, the finished matches of the days before as well. A failed download
  // only means fewer old results, so it doesn't hold up the rest.
  if (_resultWindowSecs) {
    int days = (_resultWindowSecs + SECS_PER_DAY - 1) / SECS_PER_DAY;
    for (int d = 1; d <= days; d++)
      for (const FootballLeague *league : _leagues) {
        String key = String(league->code) + "<" + d;
        if (!liveDataActive()) updatePastBoard(key, league->code, today - d * SECS_PER_DAY, league->label, league->color);
        keys.push_back(key);
      }
  }

  // Pointers into the boards: copying every match (strings, goals and cards) each round costs too much heap.
  // The boards aren't touched again until the next round.
  MatchList matches;
  for (const String &key : keys) {
    for (Match &m : _boards[key].matches) {
      bool duplicate = false;  // e.g. the Netherlands playing Germany
      for (const Match *o : matches)
        if (o->kickoff == m.kickoff && o->home == m.home && o->away == m.away) duplicate = true;
      if (!duplicate) matches.push_back(&m);
    }
  }

  time_t nowUtc = ultime::now();
  trackEndTimes(matches, nowUtc);
#ifdef UM_FOOTBALL_ONLY
  for (const auto &board : _boards)
    if (!board.second.matches.empty()) saveBoard(board.first, board.second.matches);
#endif
  detectGoals(matches);
  detectCards(matches);
  checkSubstitutions(matches);

  // Extras: the league tables and the coming matchdays (ESPN's scoreboard without a date)
  std::vector<const Match *> coming;  // pointers into the boards: copies of the matches cost too much heap
  if (_extras) {
    bool fetched = false;  // one league per round: the others follow STAGGER_MS later
    for (const FootballLeague *league : _leagues) {
      Board &next = _boards[String("next/") + league->code];
#ifdef UM_FOOTBALL_ONLY
      restoreBoard(String("next/") + league->code);
#endif
      if (!liveDataActive() && (!next.fetchedAt || millis() - next.fetchedAt >= NEXT_REFRESH_MS)) {
        if (fetched) {
          _deferred = true;
          _deferredFirst |= !next.fetchedAt;
        }
        else {
          fetched = true;
          updateNextBoard(league->code, league->label, league->color);
        }
      }
      for (const Match &m : _boards[String("next/") + league->code].matches)
        if (m.state == 'p' && m.kickoff > nowUtc) coming.push_back(&m);
    }
    // Favourites outside the followed leagues: their next match, from the day it is on
    for (const Favourite &fav : _favourites) {
      if (fav.nextLeague.isEmpty() || fav.nextKickoff <= nowUtc || (!fav.national && followsLeague(fav.nextLeague))) continue;
      String key = "next/" + fav.nextLeague + "/" + fav.abbreviation;
      Board &next = _boards[key];
#ifdef UM_FOOTBALL_ONLY
      restoreBoard(key);
#endif
      if (!liveDataActive() && (!next.fetchedAt || millis() - next.fetchedAt >= NEXT_REFRESH_MS)) {
        if (fetched) {
          _deferred = true;
          _deferredFirst |= !next.fetchedAt;
        }
        else {
          fetched = true;
          updateNextFavouriteBoard(fav, key);
        }
      }
      for (const Match &m : _boards[key].matches) {
        bool known = false;  // the same match can come with a league too
        for (const Match *o : coming) known |= o->kickoff == m.kickoff && o->home == m.home && o->away == m.away;
        if (m.state == 'p' && m.kickoff > nowUtc && !known) coming.push_back(&m);
      }
    }
    if (!liveDataActive()) refreshTables(matches, coming);
  }

  buildEntries(matches, nowUtc);
  buildOverview(matches, coming, nowUtc);

  bool live = false, kickoffSoon = false, laterToday = false;
  for (const Match *mp : matches) {
    const Match &m = *mp;
    if (m.state == 'i') live = true;
    if (m.state == 'p') {
      if (m.kickoff < nowUtc + 15 * 60) kickoffSoon = true;
      else laterToday = true;
    }
  }
  _anyLive = live;
  unsigned long wait = IDLE_REFRESH_MS;
  if (live) wait = LIVE_REFRESH_MS;
  else if (kickoffSoon) wait = KICKOFF_REFRESH_MS;
  else if (laterToday) wait = TODAY_REFRESH_MS;
  wait = std::min(wait, untilMidnightMs(today));  // yesterday's scores go at midnight
  if (_deferred) wait = std::min(wait, _deferredFirst ? FIRST_STAGGER_MS : STAGGER_MS);
  logf("[Football] refreshed, data free %u, largest block %u, stack left %u\n", (unsigned)dataFree(), (unsigned)dataBlock(),
       (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  setStatus(OK);
  if (_refreshNow) wait = std::min<unsigned long>(wait, 1000);  // asked for again while this one ran
  _footballNextAt = millis() + wait;
  return wait;
}

// Downloads the Formula 1 scoreboard when due. Returns how long until the next
// download is due.
#ifdef UM_FOOTBALL_RACE  // only the faces that show the Formula 1 top 3 (Tetris) carry the race code
unsigned long FootballTicker::refreshRace() {
  if (WiFi.status() != WL_CONNECTED || ultime::timeStatus() != timeSet) return 10 * 1000UL;
  unsigned long now = millis();
  if (_raceNextAt && (long)(now - _raceNextAt) < 0) return _raceNextAt - now;
  unsigned long wait = fetchRace();
  _raceNextAt = millis() + wait;
  return wait;
}

// The current Grand Prix weekend: the session being run, with its top three and
// flag, and the top three of every session that finished today. Returns how
// long to wait before the next download.
unsigned long FootballTicker::fetchRace() {
  espn::Weekend weekend;
  if (!getScan("http://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard",
               [&](JsonScan::Next read, void *source) { return espn::parseRace(read, source, weekend); }))
    return RETRY_MS;

  time_t nowUtc = ultime::now();
  time_t localNow = _dateTime->localNow();
  Race live;                  // the session being run
  std::vector<Race> results;  // the sessions finished today, in order
  time_t nextStart = 0;
  String eventId = weekend.eventId.c_str(), liveId;

  for (const espn::RaceSession &c : weekend.sessions) {
    String state = c.state.c_str();
    time_t date = parseEspnDate(c.date.c_str());
    if (state == "pre") {
      if (date > nowUtc && (nextStart == 0 || date < nextStart)) nextStart = date;
      continue;
    }
    bool running = state == "in";
    if (!running && (state != "post" || _dateTime->utcToLocal(date) / SECS_PER_DAY != localNow / SECS_PER_DAY))
      continue;

    Race r;
    r.live = running;
    String type = c.type.c_str();
    if (type.startsWith("FP")) r.tag = 'P';
    else if (type == "Qual" || type == "SS") r.tag = 'Q';  // SS: sprint qualifying
    else if (type == "SR") r.tag = 'S';
    for (int i = 0; i < 3; i++) r.color[i] = 0xFFFF;
    for (const espn::RaceDriver &d : c.drivers) {
      int order = d.order;
      if (order < 1 || order > 3) continue;
      // "M. Verstappen" -> "VER", in the team's colour; unknown drivers white
      String name = d.shortName.c_str();
      name = name.substring(name.lastIndexOf(' ') + 1, name.length());
      String code = name.substring(0, 3);
      code.toUpperCase();
      if (const F1Driver *known = f1DriverBySurname(name.c_str())) {
        code = known->code;
        r.color[order - 1] = known->color;
      }
      r.top[order - 1] = code;
    }
    if (running) {
      live = r;
      liveId = c.id.c_str();
    } else {
      results.push_back(r);
    }
  }

  // The flag is in the session's own status: a safety car shows as yellow, a red flag as red
  if (live.live && !eventId.isEmpty() && !liveId.isEmpty()) {
    char url[160];
    snprintf(url, sizeof(url),
             "http://sports.core.api.espn.com/v2/sports/racing/leagues/f1/events/%s/competitions/%s/status",
             eventId.c_str(), liveId.c_str());
    std::string flag;
    if (getScan(url, [&](JsonScan::Next read, void *source) { return espn::parseFlag(read, source, flag); })) {
      String name = flag.c_str();
      name.toUpperCase();
      if (name.indexOf("RED") >= 0) live.flag = 'r';
      else if (name.indexOf("YELLOW") >= 0 || name.indexOf("SAFETY") >= 0 || name.indexOf("CAUTION") >= 0 ||
               name == "SC" || name == "VSC") live.flag = 'y';
      logf("[F1] flag: %s\n", name.c_str());
    }
  }
  logf("[F1] %s %c %s %s %s, %d finished today\n", live.live ? "live" : "no session running",
                live.tag ? live.tag : 'R', live.top[0].c_str(), live.top[1].c_str(), live.top[2].c_str(),
                (int)results.size());

  {
    std::lock_guard<std::mutex> guard(_lock);
    _race = live;
    liveEventOn = _snapshot.liveCount > 0;
    liveFavouriteOn = favouriteLive(_snapshot);
#ifndef UM_FOOTBALL_ONLY
  liveEventOn = liveEventOn.load() || _race.live;
#endif
    _results.swap(results);
  }

  unsigned long wait = IDLE_REFRESH_MS;
  if (live.live) {
    wait = LIVE_REFRESH_MS;
  } else if (nextStart > nowUtc) {
    // Be there when it starts
    if (nextStart - nowUtc < 15 * 60) wait = KICKOFF_REFRESH_MS;
    else wait = std::min(IDLE_REFRESH_MS, (unsigned long)(nextStart - nowUtc - 14 * 60) * 1000UL);
  }
  return std::min(wait, untilMidnightMs(localNow));
}
#endif  // UM_FOOTBALL_RACE

// Reads the followed leagues and favourite teams from the settings. Returns true when they changed.
bool FootballTicker::loadConfig() {
  // Looked at every couple of seconds, so read into fixed buffers: Strings are only made when something changed
  static char leagues[401], favourites[401];  // the settings page keeps them to 400 characters
  static char lastLeagues[401] = "\x01", lastFavourites[401];  // the first call always finds a change
#ifdef UM_FOOTBALL_ONLY
  static int lastShowRankings = -1;
#endif
  static uint32_t seenSaves = UINT32_MAX;  // nothing saved since the last look: skip the flash read
  if (seenSaves == Settings::saveCount()) return false;
  seenSaves = Settings::saveCount();
  Settings config;
  config.load();
  strlcpy(leagues, config.footballLeagues.c_str(), 401);
  strlcpy(favourites, config.footballTeams.c_str(), 401);
#ifdef UM_FOOTBALL_ONLY
  bool showRankings = config.showRankings;
#endif
  _pageMs = constrain((unsigned)config.matchSecs, 3U, 60U) * 1000UL;
  bool leaguesChanged = strcmp(leagues, lastLeagues) != 0;
  bool favouritesChanged = strcmp(favourites, lastFavourites) != 0;
#ifdef UM_FOOTBALL_ONLY
  bool rankingsChanged = lastShowRankings < 0 || showRankings != (lastShowRankings != 0);
  lastShowRankings = showRankings;
#else
  bool rankingsChanged = false;
#endif
  if (!leaguesChanged && !favouritesChanged && !rankingsChanged) return false;
  strlcpy(lastLeagues, leagues, sizeof(lastLeagues));
  strlcpy(lastFavourites, favourites, sizeof(lastFavourites));

  if (leaguesChanged) {
    std::vector<const FootballLeague *> followed;
    for (const String &code : splitList(String(leagues))) {
      const FootballLeague *league = findLeague(code);
      if (league && followed.size() < MAX_LEAGUES) followed.push_back(league);
    }
    std::lock_guard<std::mutex> guard(_lock);
    _leagues.swap(followed);
  }
  if (favouritesChanged) {
    _favourites.clear();
    for (const String &item : splitList(String(favourites))) {
      int first = item.indexOf(':'), last = item.lastIndexOf(':');
      if (first <= 0 || last <= first || _favourites.size() >= MAX_FAVOURITES) continue;
      Favourite fav;
      fav.id = item.substring(0, first);
      fav.abbreviation = item.substring(first + 1, last);
      fav.national = item.substring(last + 1) == "n";
      _favourites.push_back(fav);
    }
  }
#ifdef UM_FOOTBALL_ONLY
  if (rankingsChanged) {
    _showRankings = showRankings;
    if (!showRankings) {
      std::lock_guard<std::mutex> guard(_lock);
      _cacheRestored = false;
    }
    _tablesAt.clear();
    if (!leaguesChanged && !favouritesChanged) _footballNextAt = 0;
  }
#endif
  logf("[Football] Following %u leagues and %u favourites\n", (unsigned)_leagues.size(), (unsigned)_favourites.size());
  return leaguesChanged || favouritesChanged;
}

bool FootballTicker::followsLeague(const String &code) {
  for (const FootballLeague *league : _leagues)
    if (code == league->code) return true;
  return false;
}

bool FootballTicker::isFavourite(const String &abbreviation) {
  for (const Favourite &fav : _favourites)
    if (fav.abbreviation == abbreviation) return true;
  return false;
}

bool FootballTicker::nationalFavourite(const char *abbreviation) {
  for (const Favourite &fav : _favourites)
    if (fav.national && fav.abbreviation == abbreviation) return true;
  return false;
}

// Every few hours, see whether the favourites play today and in which
// competition. A match day, once found, sticks until the day is over.
void FootballTicker::checkFavourites(time_t today) {
  if (liveDataActive() || firmwareUpdating) return;
  if (_nationalCheckedAt != 0 && millis() - _nationalCheckedAt < NATIONAL_CHECK_MS) return;

  bool ok = true;
  for (Favourite &fav : _favourites) {
    String league;
    time_t kickoff;
    if (!fetchNextMatch(fav.id.c_str(), league, kickoff)) {
      ok = false;
      continue;
    }
    fav.nextLeague = league;
    fav.nextKickoff = kickoff;
    bool playsToday = _dateTime->utcToLocal(kickoff) / SECS_PER_DAY == today / SECS_PER_DAY;
    if (playsToday && !league.isEmpty()) {
      fav.league = league;
      fav.day = today / SECS_PER_DAY;
    }
    logf("[Football] %s next match: %s, %s\n", fav.abbreviation.c_str(), league.c_str(),
                  playsToday ? "today" : "not today");
  }
  // A failure (a wrong team id, ESPN down) is asked again in BOARD_RETRY_MS, not every round
  _nationalCheckedAt = ok ? millis() : millis() - NATIONAL_CHECK_MS + BOARD_RETRY_MS;
}

bool FootballTicker::fetchNextMatch(const char *teamId, String &league, time_t &kickoff) {
  char url[96];
  snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/all/teams/%s", teamId);

  espn::NextMatch next;
  if (!getScan(url, [&](JsonScan::Next read, void *source) { return espn::parseNextMatch(read, source, next); })) return false;
  league = next.league.c_str();
  kickoff = parseEspnDate(next.date.c_str());
  return true;
}

// Downloads a competition's scoreboard if it has a match on or about to start,
// hasn't been downloaded today, or not for QUIET_BOARD_MS. Returns false if a
// needed download failed and there's no data for today to fall back on.
bool FootballTicker::updateBoard(const String &key, const char *league, time_t today, const char *label,
                                 uint16_t color, const char *onlyTeam) {
#ifdef UM_FOOTBALL_ONLY
  restoreBoard(key);
#endif
  Board &board = _boards[key];
  uint32_t todayNr = today / SECS_PER_DAY;
  time_t nowUtc = ultime::now();
  if (board.retryAt && (long)(millis() - board.retryAt) < 0) return board.day == todayNr;  // failed lately: wait before asking again
  board.retryAt = 0;

  bool busy = false;
  for (const Match &m : board.matches)
    if (m.state == 'i' || (m.state == 'p' && m.kickoff < nowUtc + 15 * 60)) busy = true;
  if (board.day == todayNr && !busy && millis() - board.fetchedAt < QUIET_BOARD_MS) return true;
  if (board.day == todayNr && !busy) {  // only refreshing: one such board per round, so they don't all come at once
    if (_anyLive) return true;  // nothing but the live matches while a match is on, to keep it smooth
    if (_quietFetched) {
      _deferred = true;
      return true;
    }
    _quietFetched = true;
  }

  std::vector<Match> matches;
  if (!fetchScoreboard(league, today, label, color, onlyTeam, matches)) { board.retryAt = millis() + (board.day == todayNr ? RETRY_MS : BOARD_RETRY_MS); return board.day == todayNr; }
  if (onlyTeam) {
    // ESPN files a match under its US Eastern date, which starts at 06:00 in
    // Europe: a national team playing abroad overnight (00:00-06:00 here) is
    // under yesterday's date. Look there too, and keep what is on today.
    if (!fetchScoreboard(league, today - SECS_PER_DAY, label, color, onlyTeam, matches)) { board.retryAt = millis() + (board.day == todayNr ? RETRY_MS : BOARD_RETRY_MS); return board.day == todayNr; }
  }
  matches.erase(std::remove_if(matches.begin(), matches.end(),
                               [&](const Match &m) {
                                 return m.state != 'i' && _dateTime->utcToLocal(m.kickoff) / SECS_PER_DAY != todayNr;
                               }),
                matches.end());
  logf("[Football] Downloaded %s\n", key.c_str());
#ifdef UM_FOOTBALL_ONLY
  saveBoard(key, matches);
#endif
  board.matches.swap(matches);
  board.day = todayNr;
  board.fetchedAt = millis();
  return true;
}

// A finished day's scoreboard, for the result window: downloaded once, again only while a
// match of that day was still going
bool FootballTicker::updatePastBoard(const String &key, const char *league, time_t day, const char *label,
                                     uint16_t color) {
#ifdef UM_FOOTBALL_ONLY
  restoreBoard(key);
#endif
  Board &board = _boards[key];
  uint32_t dayNr = day / SECS_PER_DAY;
  bool allFinished = std::all_of(board.matches.begin(), board.matches.end(), [](const Match &m) { return m.state == 'f'; });
  if (board.day == dayNr && (allFinished || _anyLive || millis() - board.fetchedAt < BOARD_RETRY_MS)) return true;
  std::vector<Match> matches;
  if (!fetchScoreboard(league, day, label, color, nullptr, matches)) return false;
  logf("[Football] Downloaded %s\n", key.c_str());
#ifdef UM_FOOTBALL_ONLY
  saveBoard(key, matches);
#endif
  board.matches.swap(matches);
  board.day = dayNr;
  board.fetchedAt = millis();
  return true;
}

// The undated board can still be yesterday's results: use its calendar to find the next matchday.
bool FootballTicker::updateNextBoard(const char *league, const char *label, uint16_t color) {
  Board &board = _boards[String("next/") + league];
  if (board.fetchedAt && millis() - board.fetchedAt < NEXT_REFRESH_MS) return true;
  std::vector<Match> matches;
  time_t nextDay = 0, nowUtc = ultime::now();
  if (!fetchScoreboard(league, 0, label, color, nullptr, matches, false, &nextDay)) {
    board.fetchedAt = millis() - NEXT_REFRESH_MS + BOARD_RETRY_MS;  // try again in a few minutes
    return false;
  }
  matches.erase(std::remove_if(matches.begin(), matches.end(),
                               [&](const Match &m) { return m.state != 'p' || m.kickoff <= nowUtc; }), matches.end());
  if (matches.empty() && nextDay) {
    delay(500);
    if (!fetchScoreboard(league, nextDay, label, color, nullptr, matches, false)) return false;
  }
  if (!matches.empty()) {
    time_t first = matches[0].kickoff;
    for (const Match &m : matches) first = std::min(first, m.kickoff);
    time_t day = _dateTime->utcToLocal(first) / SECS_PER_DAY * SECS_PER_DAY;
    for (int d = 1; d <= NEXT_EXTRA_DAYS; d++) {
      delay(500);  // not in a burst
      std::vector<Match> more;
      if (!fetchScoreboard(league, day + d * SECS_PER_DAY, label, color, nullptr, more, false)) break;  // fewer days will do
      for (const Match &m : more) {
        bool known = false;  // ESPN's dates are US Eastern: a match can show on two days
        for (const Match &o : matches) known |= o.kickoff == m.kickoff && o.home == m.home && o.away == m.away;
        if (!known) matches.push_back(m);
      }
    }
  }
  logf("[Football] Downloaded next/%s (%u matches)\n", league, (unsigned)matches.size());
#ifdef UM_FOOTBALL_ONLY
  saveBoard(String("next/") + league, matches);
#endif
  board.matches.swap(matches);
  board.fetchedAt = millis();
  return true;
}

// A favourite's next match: its competition's scoreboard of the day it is on, only that team's matches
bool FootballTicker::updateNextFavouriteBoard(const Favourite &fav, const String &key) {
  Board &board = _boards[key];
  uint16_t color = favouriteColor(fav.abbreviation, fav.national, fav.nextLeague);
  time_t local = _dateTime->utcToLocal(fav.nextKickoff);
  time_t day = local / SECS_PER_DAY * SECS_PER_DAY;
  std::vector<Match> matches;
  bool ok = fetchScoreboard(fav.nextLeague.c_str(), day, fav.abbreviation.c_str(), color, fav.abbreviation.c_str(), matches, false);
  if (ok && local - day < 6 * 3600) {  // ESPN files these under the day before (US Eastern)
    delay(500);
    ok = fetchScoreboard(fav.nextLeague.c_str(), day - SECS_PER_DAY, fav.abbreviation.c_str(), color, fav.abbreviation.c_str(),
                         matches, false);
  }
  if (!ok) {
    board.fetchedAt = millis() - NEXT_REFRESH_MS + BOARD_RETRY_MS;  // try again in a few minutes
    return false;
  }
  logf("[Football] Downloaded %s\n", key.c_str());
#ifdef UM_FOOTBALL_ONLY
  saveBoard(key, matches);
#endif
  board.matches.swap(matches);
  board.fetchedAt = millis();
  return true;
}

// The tables of the competitions with a match today or coming up: a league is one table, the
// Nations League, a World Cup or a qualifying round have groups. ESPN lists each in order, so a
// team's place is its position in that list (teams level on points get consecutive places).
// Competitions without a table (cups, friendlies) simply have none.
void FootballTicker::refreshTables(const MatchList &matches, const std::vector<const Match *> &coming) {
  std::vector<String> leagues;
  auto add = [&](const Match &m) {
    if (!m.league.isEmpty() && std::find(leagues.begin(), leagues.end(), m.league) == leagues.end())
      leagues.push_back(m.league);
  };
  for (const Match *m : matches) add(*m);
  for (const Match *m : coming) add(*m);
#ifdef UM_FOOTBALL_ONLY
  if (_showRankings)
    for (const FootballLeague *league : _leagues)
      if (!nationalTeamCompetition(league->code) &&
          std::find(leagues.begin(), leagues.end(), league->code) == leagues.end()) leagues.push_back(league->code);
#endif

  for (const String &league : leagues) {
    auto at = _tablesAt.find(league);
    if (at != _tablesAt.end() && millis() - at->second < TABLES_REFRESH_MS) continue;
    _tablesAt[league] = millis();  // also after a failure: a missing table only means no places
    char url[112];
    snprintf(url, sizeof(url), "http://site.api.espn.com/apis/v2/sports/soccer/%s/standings", league.c_str());
    espn::Groups table;
#ifdef UM_FOOTBALL_ONLY
    espn::GroupPoints points;
    if (!getScan(url, [&](JsonScan::Next read, void *source) {
          return espn::parseStandings(read, source, table, _showRankings ? &points : nullptr);
        })) continue;
#else
    if (!getScan(url, [&](JsonScan::Next read, void *source) { return espn::parseStandings(read, source, table); })) continue;
#endif
#ifdef UM_FOOTBALL_ONLY
    if (!cacheReady) continue;
    std::lock_guard<std::mutex> cacheGuard(cacheLock);
    String path = cachePath("table", league);
    File file = LittleFS.open(path + ".tmp", "w");
    uint16_t count = table.size();
    bool saved = file && cacheWrite(file, CACHE_MAGIC) && cacheWrite(file, count);
    for (size_t g = 0; g < table.size(); g++) {
      uint16_t rows = table[g].size();
      saved &= cacheWrite(file, rows);
      for (size_t i = 0; i < table[g].size(); i++) {
        CachedTeam team = {};
        strlcpy(team.name, table[g][i].c_str(), sizeof(team.name));
        team.points = g < points.size() && i < points[g].size() ? points[g][i] : INT16_MIN;
        saved &= cacheWrite(file, team);
      }
      logf("[Football] Table %s group %u: %u teams, %u pages\n", league.c_str(), (unsigned)g + 1,
           (unsigned)rows, (unsigned)standingsSteps(rows));
    }
    file.close();
    if (!saved || !cacheCommit(path)) {
      LittleFS.remove(path + ".tmp");
      logf("[Football] Could not cache table %s\n", league.c_str());
    }
    _standingsDirty = true;
#else
    std::vector<std::vector<String>> groups;
    for (const auto &group : table) {
      std::vector<String> teams;
      for (const std::string &abbreviation : group) teams.push_back(abbreviation.c_str());
      groups.push_back(teams);
    }
    logf("[Football] Table %s: %u groups\n", league.c_str(), (unsigned)groups.size());
    for (size_t g = 0; g < groups.size(); g++)
      logf("[Football] Table %s group %u: %u teams, %u pages\n", league.c_str(), (unsigned)g + 1,
           (unsigned)groups[g].size(), (unsigned)((groups[g].size() + 5) / 6));
    std::lock_guard<std::mutex> guard(_lock);
    _tables[league].swap(groups);
#endif
  }
}

// The places of both teams, only when they are in the same group: in a knockout round the
// teams come from different groups, and their group places mean nothing there
void FootballTicker::tablePositions(const Match &m, uint8_t &homePos, uint8_t &awayPos) {
  homePos = awayPos = 0;
#ifdef UM_FOOTBALL_ONLY
  if (!cacheReady) return;
  std::lock_guard<std::mutex> guard(cacheLock);
  File file = LittleFS.open(cachePath("table", m.league), "r");
  uint16_t groups = 0, rows = 0;
  if (!cacheHeader(file, groups)) return;
  for (size_t g = 0; g < groups; g++) {
    if (!cacheRead(file, rows) || rows > 128) return;
    uint8_t home = 0, away = 0;
    for (size_t i = 0; i < rows; i++) {
      CachedTeam team;
      if (!cacheRead(file, team) || !memchr(team.name, 0, sizeof(team.name))) return;
      if (m.home == team.name) home = i + 1;
      if (m.away == team.name) away = i + 1;
    }
    if (home && away) {
      homePos = home;
      awayPos = away;
      return;
    }
  }
#else
  auto table = _tables.find(m.league);
  if (table == _tables.end()) return;
  for (const auto &group : table->second) {
    auto home = std::find(group.begin(), group.end(), m.home), away = std::find(group.begin(), group.end(), m.away);
    if (home == group.end() || away == group.end()) continue;
    homePos = home - group.begin() + 1;
    awayPos = away - group.begin() + 1;
    return;
  }
#endif
}

// A word that is no surname: an initial ("J."), or a suffix (Jr, Sr, II, III, IV)
static bool notSurname(const char *word, int len) {
  if (len < 2) return true;
  if (len > 3) return false;
  char w[4] = {0};
  for (int i = 0; i < len; i++) w[i] = tolower((unsigned char)word[i]);
  return !strcmp(w, "jr") || !strcmp(w, "sr") || !strcmp(w, "ii") || !strcmp(w, "iii") || !strcmp(w, "iv");
}

// The last word of a name that is a surname (not an initial or Jr), plain letters only (the display
// has no accents), at most 10
static void surname(const char *fullName, char *out) {
  const char *best = fullName;  // when every word is one of those: the last one
  bool found = false;
  const char *word = fullName;
  for (const char *c = fullName;; c++) {
    if (*c == ' ' || !*c) {
      int len = 0;
      for (const char *w = word; w < c; w++) len += isalpha((unsigned char)*w) ? 1 : 0;
      if (c > word && !notSurname(word, len)) {
        best = word;
        found = true;
      } else if (c > word && !found) {
        best = word;
      }
      word = c + 1;
      if (!*c) break;
    }
  }
  int n = 0;
  for (const char *c = best; *c && *c != ' ' && n < 10; c++)
    if (isalpha((unsigned char)*c) || *c == '-' || *c == '\'') out[n++] = *c;
  out[n] = 0;
}

// localDay = 0: no date, which gets the coming matchday. withEvents (and extras) also reads
// the goals and cards.
bool FootballTicker::fetchScoreboard(const char *league, time_t localDay, const char *label, uint16_t color,
                                     const char *onlyTeam, std::vector<Match> &out, bool withEvents, time_t *nextDay) {
  char url[128];
  if (localDay) {
    tmElements_t day;
    ultime::breakTime(localDay, day);
    snprintf(url, sizeof(url),
             "http://site.api.espn.com/apis/site/v2/sports/soccer/%s/scoreboard?dates=%04d%02d%02d",
             league, day.Year + 1970, day.Month, day.Day);
  } else {
    snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/%s/scoreboard", league);
  }
  withEvents &= _extras;

  std::vector<espn::ScoreEvent> events;
  char afterDate[11] = {0};
  std::string nextDate;
  if (nextDay) {
    tmElements_t today;
    ultime::breakTime(_dateTime->localNow(), today);
    snprintf(afterDate, sizeof(afterDate), "%04u-%02u-%02u", (unsigned)(today.Year + 1970) % 10000, (unsigned)today.Month % 100, (unsigned)today.Day % 100);
    *nextDay = 0;
  }
  if (!getScan(url, [&](JsonScan::Next read, void *source) {
        return espn::parseScoreboard(read, source, events, withEvents, nextDay ? &nextDate : nullptr, afterDate);
      }))
    return false;
  if (nextDay && !nextDate.empty()) *nextDay = parseEspnDate(nextDate.c_str()) / SECS_PER_DAY * SECS_PER_DAY;

  for (const espn::ScoreEvent &e : events) {
    if (e.name == "STATUS_POSTPONED" || e.name == "STATUS_CANCELED" || e.name == "STATUS_ABANDONED") continue;

    Match m;
    m.label = label;
    m.league = league;
    m.competition = competitionCode(league, onlyTeam != nullptr && nationalFavourite(onlyTeam));
    m.color = color;
    m.kickoff = parseEspnDate(e.date.c_str());
    if (m.kickoff == 0) continue;
    m.id = e.id.c_str();

    m.state = e.state == "in" ? 'i' : e.state == "post" ? 'f' : 'p';
    m.detail = e.shortDetail.c_str();

    String homeId;
    for (const espn::Competitor &c : e.competitors) {
      bool home = c.homeAway == "home";
      (home ? m.home : m.away) = c.hasAbbreviation ? c.abbreviation.c_str() : "?";
      (home ? m.homeKit : m.awayKit) = clubKit(home ? m.home : m.away, league, c.hasColor ? c.color.c_str() : nullptr,
                                               c.hasAlternateColor ? c.alternateColor.c_str() : nullptr);
      (home ? m.homeScore : m.awayScore) = atoi(c.score.c_str());
      if (home) homeId = c.id.c_str();
    }

    // Goals and cards: the clock is in seconds, "6'" is anywhere in the 6th minute
    for (const espn::Detail &d : e.details) {
      char kind = d.redCard ? 'r' : d.yellowCard ? 'y' : d.scoringPlay ? 'g' : 0;
      if (!kind) continue;
      int minute = std::min(120, (int)(float)d.clock / 60 + 1);
      Event event = {(uint8_t)minute, kind, homeId == d.teamId.c_str(), 0, ""};
      if (kind != 'g') {
        event.number = d.hasJersey ? atoi(d.jersey.c_str()) : 0;
        surname(d.hasDisplayName ? d.displayName.c_str() : "", event.name);
      }
      m.events.push_back(event);
    }

    if (onlyTeam && m.home != onlyTeam && m.away != onlyTeam) continue;
    out.push_back(m);
  }
  return true;
}

bool FootballTicker::updating() {
  return _fetching || (_fetchEnded && millis() - _fetchEnded < 1500);
}

bool FootballTicker::getScan(const char *url, const std::function<bool(JsonScan::Next, void *)> &parse) {
  _fetching = true;
  bool ok = getScanNow(url, parse);
  _fetching = false;
  _fetchEnded = millis();
  return ok;
}

bool FootballTicker::getScanNow(const char *url, const std::function<bool(JsonScan::Next, void *)> &parse) {
  std::lock_guard<std::mutex> download(networkBudget());
  if (firmwareUpdating) return false;
  // Plain HTTP: public score data, and the ESP32's TLS client sometimes stalls
  // after the first 16KB record of these ~50KB responses
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    logf("[Football] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }

  YieldingReader reader(http.getStream(), 10000);
  bool ok = parse([](void *r) { return ((YieldingReader *)r)->read(); }, &reader);
  http.end();
  if (!ok) logf("[Football] %s: %s after %u bytes (data free %u, largest block %u)\n", url,
                reader.dry ? "the connection stopped sending" : "not valid JSON", (unsigned)reader.count, (unsigned)dataFree(),
                (unsigned)dataBlock());
  return ok;
}

// Remembers when each match ended: when we first see it finished, or, if it
// had already finished the first time we saw it, an estimate from the kick-off
void FootballTicker::trackEndTimes(MatchList &matches, time_t nowUtc) {
  std::map<String, time_t> endedAt;
  for (Match *mp : matches) {
    Match &m = *mp;
    if (m.state != 'f') continue;
    String key = m.key();
    auto known = _endedAt.find(key);
    if (known != _endedAt.end())
      m.endedAt = known->second;
    else if (_haveScores && _scores.count(key))
      m.endedAt = nowUtc;  // still going at the previous refresh
    else
      m.endedAt = std::min(nowUtc, m.kickoff + TYPICAL_MATCH_SECS);
    endedAt[key] = m.endedAt;
  }
  _endedAt.swap(endedAt);
}

// Compares scores with the highest seen so far for each match. The very first
// refresh only records them, so a reboot doesn't replay goals. Only a score
// above the highest counts: a feed that flaps (1-0, 0-0, 1-0) or a goal that
// is ruled out and given again isn't celebrated twice.
void FootballTicker::detectGoals(const MatchList &matches) {
  std::map<String, std::pair<int, int>> scores;
  std::vector<Goal> goals;

  for (const Match *mp : matches) {
    const Match &m = *mp;
    String key = m.key();
    auto previous = _scores.find(key);
    scores[key] = previous == _scores.end()
                      ? std::make_pair(m.homeScore, m.awayScore)
                      : std::make_pair(std::max(m.homeScore, previous->second.first),
                                       std::max(m.awayScore, previous->second.second));

    if (!_haveScores || previous == _scores.end()) continue;
    if (m.homeScore > previous->second.first)
      goals.push_back({m.home, m.away, m.homeScore, m.awayScore, true, m.homeKit, m.awayKit, millis()});
    if (m.awayScore > previous->second.second)
      goals.push_back({m.home, m.away, m.homeScore, m.awayScore, false, m.homeKit, m.awayKit, millis()});
  }
  _scores.swap(scores);
  _haveScores = true;

  for (const Goal &g : goals)
    logf("[Football] GOAL %s: %s %d-%d %s\n", g.homeScored ? g.home.c_str() : g.away.c_str(),
                  g.home.c_str(), g.homeScore, g.awayScore, g.away.c_str());

  if (goals.empty()) return;
  std::lock_guard<std::mutex> guard(_lock);
  _goals.insert(_goals.end(), goals.begin(), goals.end());
  if (_goals.size() > 5) _goals.erase(_goals.begin(), _goals.end() - 5);
}

// New cards of live matches. Like goals, the first time a match is seen only records how many there are.
void FootballTicker::detectCards(const MatchList &matches) {
  std::map<String, uint8_t> counts;
  std::vector<Incident> found;
  for (const Match *mp : matches) {
    const Match &m = *mp;
    if (m.state != 'i') continue;
    String key = m.key();
    uint8_t count = 0;
    for (const Event &e : m.events) count += e.kind != 'g';
    auto previous = _cardCount.find(key);
    counts[key] = previous == _cardCount.end() ? count : std::max(count, previous->second);  // a download without events isn't fewer
    if (previous == _cardCount.end()) continue;
    uint8_t seen = 0;
    for (const Event &e : m.events) {
      if (e.kind == 'g' || seen++ < previous->second) continue;
      Incident in = {e.kind, m.home, m.away, m.homeKit, m.awayKit, e.home, e.minute, "", e.number, 0, millis()};
      strcpy(in.name, e.name);
      found.push_back(in);
    }
  }
  _cardCount.swap(counts);
  addIncidents(found);
}

// Substitutions aren't on the scoreboard: the summary of a live match has them, with the shirt numbers in the line-ups.
// One summary per refresh, the one read longest ago.
void FootballTicker::checkSubstitutions(const MatchList &matches) {
  if (!_extras) return;
  const Match *due = nullptr;
  unsigned long dueSince = 0;
  for (const Match *mp : matches) {
    const Match &m = *mp;
    if (m.state != 'i' || m.id.isEmpty()) continue;
    auto checked = _subChecked.find(m.key());
    unsigned long since = checked == _subChecked.end() ? 0xFFFFFFFFUL : millis() - checked->second;
    if (since >= SUB_REFRESH_MS && since >= dueSince) {
      due = mp;
      dueSince = since;
    }
  }
  // Forget the matches that are over, or these maps grow with every match played
  for (auto it = _subChecked.begin(); it != _subChecked.end();) {
    bool live = std::any_of(matches.begin(), matches.end(), [&](const Match *m) { return m->state == 'i' && m->key() == it->first; });
    it = live ? std::next(it) : _subChecked.erase(it);
  }
  for (auto it = _subCount.begin(); it != _subCount.end();) {
    bool live = std::any_of(matches.begin(), matches.end(), [&](const Match *m) { return m->state == 'i' && m->key() == it->first; });
    it = live ? std::next(it) : _subCount.erase(it);
  }
  if (!due) return;
  String key = due->key();
  std::vector<Incident> found;
  uint8_t total = 0;
  _subChecked[key] = millis();
  if (!fetchSubstitutions(*due, found, total)) return;
  auto previous = _subCount.find(key);
  bool known = previous != _subCount.end();
  uint8_t seen = known ? previous->second : total;
  _subCount[key] = std::max(total, seen);
  if (!known) return;  // the first look only records how many there are
  if (found.size() > seen) found.erase(found.begin(), found.begin() + seen); else found.clear();
  addIncidents(found);
}

bool FootballTicker::fetchSubstitutions(const Match &match, std::vector<Incident> &found, uint8_t &total) {
  char url[128];
  snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/%s/summary?event=%s",
           match.league.c_str(), match.id.c_str());
  espn::Summary summary;
  if (!getScan(url, [&](JsonScan::Next read, void *source) { return espn::parseSummary(read, source, summary); })) return false;

  std::map<String, uint8_t> jerseys;  // athlete id -> shirt number
  String homeTeamId;
  for (const espn::Squad &squad : summary.squads) {
    if (squad.homeAway == "home") homeTeamId = squad.teamId.c_str();
    for (const espn::Player &player : squad.players) jerseys[String(player.athleteId.c_str())] = atoi(player.jersey.c_str());
  }
  total = 0;
  for (const espn::Substitution &s : summary.substitutions) {
    total++;
    // ESPN lists the player coming on first, then the one going off
    uint8_t on = jerseys[String(s.on.c_str())];
    uint8_t off = jerseys[String(s.off.c_str())];
    Incident in = {'s', match.home, match.away, match.homeKit, match.awayKit, homeTeamId == s.teamId.c_str(),
                   (uint8_t)std::min(120, (int)(float)s.clock / 60 + 1), "", off, on, millis()};
    found.push_back(in);
  }
  return true;
}

void FootballTicker::addIncidents(const std::vector<Incident> &incidents) {
  if (incidents.empty()) return;
  std::lock_guard<std::mutex> guard(_lock);
  _incidents.insert(_incidents.end(), incidents.begin(), incidents.end());
  if (_incidents.size() > 6) _incidents.erase(_incidents.begin(), _incidents.end() - 6);
}

bool FootballTicker::nextIncident(Incident &incident) {
  std::lock_guard<std::mutex> guard(_lock);
  while (!_incidents.empty()) {
    incident = _incidents.front();
    _incidents.erase(_incidents.begin());
    if (millis() - incident.detectedAt < GOAL_EXPIRY_MS) return true;
  }
  return false;
}

bool FootballTicker::nextGoal(Goal &goal) {
  std::lock_guard<std::mutex> guard(_lock);
  while (!_goals.empty()) {
    goal = _goals.front();
    _goals.erase(_goals.begin());
    if (millis() - goal.detectedAt < GOAL_EXPIRY_MS) return true;
  }
  return false;
}

// Live matches, or when none are live, the matches that finished today (the
// MAX_SHOWN most recent). Returns whether any finished match is shown.
bool FootballTicker::buildEntries(const MatchList &matches, time_t nowUtc) {
  time_t localNow = _dateTime->utcToLocal(nowUtc);
  uint32_t today = localNow / SECS_PER_DAY;
  size_t live = 0, todayMatches = 0, recentResults = 0;
  std::vector<const Match *> finished;
  for (const Match *mp : matches) {
    if (mp->state == 'i') live++;
    bool playedToday = _dateTime->utcToLocal(mp->kickoff) / SECS_PER_DAY == today;
    if (playedToday) todayMatches++;
    if (mp->state == 'f' && (playedToday || (_resultWindowSecs && mp->endedAt + (time_t)_resultWindowSecs > nowUtc))) {
      finished.push_back(mp);
      if (!playedToday) recentResults++;
    }
  }
  std::sort(finished.begin(), finished.end(), [](const Match *a, const Match *b) {
    return a->endedAt > b->endedAt;  // most recent first
  });
  finished.resize(live > 0 ? 0 : std::min(finished.size(), MAX_SHOWN));

  String log;
  for (const Match *mp : matches) {
    const Match &m = *mp;
    bool show = m.state == 'i' || std::find(finished.begin(), finished.end(), mp) != finished.end();
    if (!show) continue;

    log += String(" [") + m.label + "] " + m.detail + " " + m.home + " " + String(m.homeScore) + "-" + String(m.awayScore) + " " + m.away;
  }
  tmElements_t date;
  ultime::breakTime(localNow, date);
  logf("[Football] %u matches today (%04d-%02d-%02d), %u recent results, showing:%s\n", (unsigned)todayMatches,
       date.Year + 1970, date.Month, date.Day, (unsigned)recentResults, log.isEmpty() ? " none" : log.c_str());

  return !finished.empty();
}

template <size_t N> static void snapshotText(char (&out)[N], const String &text) { strlcpy(out, text.c_str(), N); }

static football::Side snapshotSide(const String &code, const TeamKit &kit, uint8_t position) {
  football::Side side = {};
  snapshotText(side.code, code);
  side.kit = kit;
  side.position = position;
  return side;
}

// The latest kMarks goals and cards
template <typename Row> static void snapshotMarks(Row &row, const std::vector<FootballTicker::Event> &events) {
  size_t from = events.size() > football::kMarks ? events.size() - football::kMarks : 0;
  for (size_t i = from; i < events.size(); i++) row.marks[row.markCount++] = {events[i].minute, events[i].kind, events[i].home};
}

static int8_t snapshotScore(int score) { return (int8_t)std::max(0, std::min(score, 99)); }

#ifdef UM_FOOTBALL_ONLY
// The tables of the followed competitions (cups and national teams have none), in the order the
// display pages through them. Reads the table files: only the data task calls this.
void FootballTicker::fillStandings(football::Snapshot &out) {
  out.groupCount = 0;
  out.teamCount = 0;
  if (!cacheReady) return;
  std::lock_guard<std::mutex> guard(cacheLock);
  for (const FootballLeague *league : _leagues) {
    if (nationalTeamCompetition(league->code)) continue;
    File file = LittleFS.open(cachePath("table", league->code), "r");
    uint16_t groups = 0, rows = 0;
    if (!cacheHeader(file, groups)) continue;
    for (size_t g = 0; g < groups; g++) {
      if (!cacheRead(file, rows) || rows > 128) break;
      if (out.groupCount >= football::kGroups || out.teamCount + rows > football::kTeams) {
        logf("[Football] table of %s left out of the snapshot: no room\n", league->code);
        if (!file.seek(file.position() + rows * sizeof(CachedTeam))) break;
        continue;
      }
      football::Group &group = out.groups[out.groupCount++];
      group = {};
      strlcpy(group.name, FOOTBALL_LEAGUE_NAMES[league - FOOTBALL_LEAGUES], sizeof(group.name));
      if (groups > 1) {
        size_t n = strlen(group.name);
        if (n + 2 < sizeof(group.name)) {
          group.name[n] = ' ';
          group.name[n + 1] = char('A' + g);
          group.name[n + 2] = 0;
        }
      }
      group.first = out.teamCount;
      for (size_t i = 0; i < rows; i++) {
        CachedTeam team;
        if (!cacheRead(file, team) || !memchr(team.name, 0, sizeof(team.name))) return;
        football::TableRow &row = out.teams[out.teamCount++];
        strlcpy(row.code, team.name, sizeof(row.code));
        row.points = team.points;
        group.rows++;
      }
    }
  }
}
#else
void FootballTicker::fillStandings(football::Snapshot &out) {
  out.groupCount = 0;
  out.teamCount = 0;
}
#endif

// Fills _staging from the lists buildOverview has settled on: the data task is the only one to touch it.
// buildOverview publishes it with the rest, under _lock.
void FootballTicker::buildSnapshot(const std::vector<const Match *> &live, const std::vector<const Match *> &finished,
                                   const std::vector<const Match *> &next) {
  if (!_staging) {
    _staging.reset(new football::Snapshot{});
    // Standings can survive a refresh without being fetched again.
    std::lock_guard<std::mutex> guard(_lock);
    *_staging = _snapshot;
  }
  football::Snapshot &s = *_staging;
  s.liveCount = std::min(live.size(), (size_t)football::kLive);
  if (live.size() > football::kLive)
    logf("[Football] %u live matches, showing %u\n", (unsigned)live.size(), (unsigned)football::kLive);
  for (size_t i = 0; i < s.liveCount; i++) {
    const Match &m = *live[i];
    football::LiveRow &row = s.live[i];
    row = {};
    uint8_t homePos, awayPos;
    tablePositions(m, homePos, awayPos);
    row.home = snapshotSide(m.home, m.homeKit, homePos);
    row.away = snapshotSide(m.away, m.awayKit, awayPos);
    row.homeScore = snapshotScore(m.homeScore);
    row.awayScore = snapshotScore(m.awayScore);
    // The playing time: "67'", "45'+2'" or "HT"
    row.progress = m.detail.startsWith("HT") ? 0.5f : std::min(1.0f, m.detail.toInt() / 90.0f);
    snapshotText(row.clock, m.detail);
    snapshotText(row.competition, m.competition);
    row.color = m.color;
    row.favourite = isFavourite(m.home) || isFavourite(m.away);
    snapshotMarks(row, m.events);
  }
  s.resultCount = std::min(finished.size(), (size_t)football::kResults);
  for (size_t i = 0; i < s.resultCount; i++) {
    const Match &m = *finished[i];
    football::ResultRow &row = s.results[i];
    row = {};
    uint8_t homePos, awayPos;
    tablePositions(m, homePos, awayPos);
    row.home = snapshotSide(m.home, m.homeKit, homePos);
    row.away = snapshotSide(m.away, m.awayKit, awayPos);
    row.homeScore = snapshotScore(m.homeScore);
    row.awayScore = snapshotScore(m.awayScore);
    snapshotText(row.competition, m.competition);
    row.color = m.color;
    row.favourite = isFavourite(m.home) || isFavourite(m.away);
    snapshotMarks(row, m.events);
  }
  s.fixtureCount = std::min(next.size(), (size_t)football::kFixtures);
  for (size_t i = 0; i < s.fixtureCount; i++) {
    const Match &m = *next[i];
    football::FixtureRow &row = s.fixtures[i];
    row = {};
    uint8_t homePos, awayPos;
    tablePositions(m, homePos, awayPos);
    row.home = snapshotSide(m.home, m.homeKit, homePos);
    row.away = snapshotSide(m.away, m.awayKit, awayPos);
    row.kickoff = m.kickoff;
    row.color = m.color;
  }
  if (_standingsDirty) {
    fillStandings(s);
    _standingsDirty = false;
  }
}

// For faces with their own layout: all live matches, the finished ones (today's, or those of
// the result window) and the coming ones, today's not started yet included
void FootballTicker::buildOverview(const MatchList &matches, const std::vector<const Match *> &coming,
                                  time_t nowUtc) {
  std::vector<const Match *> live, finished, next;
  uint32_t today = _dateTime->localNow() / SECS_PER_DAY;
  for (const Match *mp : matches) {
    const Match &m = *mp;
    if (m.state == 'i') live.push_back(mp);
    bool playedToday = _dateTime->utcToLocal(m.kickoff) / SECS_PER_DAY == today;
    if (m.state == 'f' && (playedToday || (_resultWindowSecs && m.endedAt + (time_t)_resultWindowSecs > nowUtc)))
      finished.push_back(mp);
    if (m.state == 'p' && m.kickoff > nowUtc) next.push_back(mp);
  }
  std::stable_partition(live.begin(), live.end(), [&](const Match *m) { return isFavourite(m->home) || isFavourite(m->away); });
  // While matches are live, the results and fixtures below them are those of the same match weekend (any league)
  auto otherWeekend = [&](const Match *m) {
    int64_t day = _dateTime->utcToLocal(m->kickoff) / SECS_PER_DAY;
    for (const Match *l : live)
      if (std::llabs(day - (int64_t)(_dateTime->utcToLocal(l->kickoff) / SECS_PER_DAY)) <= 3) return false;
    return true;
  };
  if (!live.empty()) {
    finished.erase(std::remove_if(finished.begin(), finished.end(), otherWeekend), finished.end());
    next.erase(std::remove_if(next.begin(), next.end(), otherWeekend), next.end());
  }
  std::sort(finished.begin(), finished.end(), [&](const Match *a, const Match *b) {
    bool aToday = _dateTime->utcToLocal(a->kickoff) / SECS_PER_DAY == today;
    bool bToday = _dateTime->utcToLocal(b->kickoff) / SECS_PER_DAY == today;
    if (aToday != bToday) return aToday;
    return a->endedAt > b->endedAt;
  });
  finished.resize(std::min(finished.size(), MAX_SHOWN));

  for (const Match *m : coming) {
    bool known = false;  // today's matches are on both boards
    for (const Match *n : next) known |= n->kickoff == m->kickoff && n->home == m->home && n->away == m->away;
    if (!known && (live.empty() || !otherWeekend(m))) next.push_back(m);
  }
  // The face names the weekday only, so a match a week or more away would read as an earlier day
  next.erase(std::remove_if(next.begin(), next.end(),
                            [&](const Match *m) { return _dateTime->utcToLocal(m->kickoff) / SECS_PER_DAY >= today + 7; }),
             next.end());
  // Matches that kick off together keep their order between refreshes, or a face that takes turns in them swaps early
  std::sort(next.begin(), next.end(), [&](const Match *a, const Match *b) {
#ifdef UM_FOOTBALL_ONLY
    auto leagueOrder = [&](const String &code) {
      for (size_t i = 0; i < _leagues.size(); i++)
        if (code == _leagues[i]->code) return i;
      return _leagues.size();
    };
    size_t aLeague = leagueOrder(a->league), bLeague = leagueOrder(b->league);
    if (aLeague != bLeague) return aLeague < bLeague;
    if (a->league != b->league) return a->league < b->league;
#endif
    if (a->kickoff != b->kickoff) return a->kickoff < b->kickoff;
    if (a->home != b->home) return a->home < b->home;
    return a->away < b->away;
  });
  next.resize(std::min(next.size(), MAX_UPCOMING));
  // What the display shows is in the snapshot; the cache files only bring it back after a restart
  uint8_t slot = _cacheSlot;
  bool saved = saveMatches(displayCachePath("results", slot), finished, true) &&
               saveMatches(displayCachePath("next", slot), next, true);
  if (!saved) {
    slot ^= 1;
    saved = saveMatches(displayCachePath("results", slot), finished) &&
            saveMatches(displayCachePath("next", slot), next);
  }
  if (saved) {
    std::lock_guard<std::mutex> cacheGuard(cacheLock);
    CachedOverview metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.magic = CACHE_MAGIC;
    metadata.finishedCount = finished.size();
    metadata.upcomingCount = next.size();
    metadata.slot = slot;
    CachedOverview old;
    File previous = LittleFS.open("/um-fb-display", "r");
    bool same = previous && cacheRead(previous, old) && memcmp(&old, &metadata, sizeof(old)) == 0;
    previous.close();
    if (!same) {
      File file = LittleFS.open("/um-fb-display.tmp", "w");
      bool written = file && cacheWrite(file, metadata);
      file.close();
      saved = written && cacheCommit("/um-fb-display");
    }
  }
  if (saved) _cacheSlot = slot;
  else logf("[Football] Could not save the display cache\n");

  buildSnapshot(live, finished, next);

  std::lock_guard<std::mutex> guard(_lock);
  _snapshot = *_staging;
  liveEventOn = _snapshot.liveCount > 0;
  liveFavouriteOn = favouriteLive(_snapshot);
#ifndef UM_FOOTBALL_ONLY
  liveEventOn = liveEventOn.load() || _race.live;
#endif
  _version++;
}

#ifdef UM_FOOTBALL_ONLY
void FootballTicker::restoreOverview() {
  if (!cacheReady) return;
  uint8_t slot;
  {
    std::lock_guard<std::mutex> guard(cacheLock);
    File file = LittleFS.open("/um-fb-display", "r");
    CachedOverview metadata;
    if (!file || !cacheRead(file, metadata) || metadata.magic != CACHE_MAGIC || metadata.slot > 1 ||
        metadata.finishedCount > MAX_SHOWN || metadata.upcomingCount > MAX_UPCOMING) return;
    slot = metadata.slot;
  }
  _cacheSlot = slot;
  std::vector<Match> results, fixtures;
  results.reserve(MAX_SHOWN);
  fixtures.reserve(MAX_UPCOMING);
  time_t nowUtc = ultime::now();
  uint32_t today = _dateTime->localNow() / SECS_PER_DAY;
  auto followed = [&](const Match &m) { return followsLeague(m.league) || isFavourite(m.home) || isFavourite(m.away); };
  Match m;
  for (size_t i = 0; i < MAX_SHOWN && readMatch(displayCachePath("results", slot), i, m); i++)
    if (followed(m) && m.state == 'f' && (_dateTime->utcToLocal(m.kickoff) / SECS_PER_DAY == today ||
        (_resultWindowSecs && m.endedAt + (time_t)_resultWindowSecs > nowUtc))) results.push_back(std::move(m));
  for (size_t i = 0; i < MAX_UPCOMING && readMatch(displayCachePath("next", slot), i, m); i++)
    if (followed(m) && m.state == 'p' && m.kickoff > nowUtc) fixtures.push_back(std::move(m));
  MatchList matches;
  std::vector<const Match *> coming;
  for (Match &result : results) matches.push_back(&result);
  for (const Match &fixture : fixtures) coming.push_back(&fixture);
  buildOverview(matches, coming, nowUtc);
  logf("[Football] Restored %u results and %u upcoming games from flash\n", (unsigned)results.size(), (unsigned)fixtures.size());
}
#endif

void FootballTicker::snapshot(football::Snapshot &out) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _snapshot;
  out.version = _version;
}

void FootballTicker::setStatus(Status status) {
  std::lock_guard<std::mutex> guard(_lock);
  _status = status;
}

FootballTicker::Status FootballTicker::status() {
  std::lock_guard<std::mutex> guard(_lock);
  return _status;
}

uint32_t FootballTicker::version() {
  resume();
  std::lock_guard<std::mutex> guard(_lock);
  return _version;
}

#if !defined(UM_FOOTBALL_ONLY)
// The F1 logo in red and the first three of the race in white, each with a bar in
// their team's colour underneath, in Picopixel (its cursor is the baseline).
// A safety car turns the row's background yellow, a red flag red.
static void drawRace(Adafruit_GFX *display, int16_t y, const FootballTicker::Race &race, bool slide) {
  uint16_t background = race.flag == 'r' ? 0xA000 : race.flag == 'y' ? 0xC600 : 0;
  uint16_t text = race.flag == 'y' ? 0 : 0xFFFF;  // black on yellow
  display->fillRect(0, y - 1, 64, display->height() - (y - 1), background);  // to the very bottom
  display->setFont(&Picopixel);
  int16_t baseline = y + 5;
  // P = practice, Q = qualifying, S = sprint race; the Grand Prix itself has no letter
  if (race.tag) {
    display->setTextColor(race.flag ? text : 0x8410);
    display->setCursor(15, baseline);
    display->print(race.tag);
  }
  // The F1 logo, 13x7: a slanted F and 1 that lean the same way
  static const char *const LOGO[7] = {"..#####...##.", "..##....####.", ".###.....##..", ".#####...##..",
                                      ".##......##..", "##......##...", "##......##..."};
  uint16_t logo = race.flag == 'r' ? 0xFFFF : race.flag == 'y' ? 0 : 0xF800;
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 13; col++)
      if (LOGO[row][col] == '#') display->drawPixel(col, y + row, logo);
  // Finished: a chequered flag of 5x4 pixels right of the logo, its last row
  // below the logo's bottom
  if (!race.live)
    for (int row = 0; row < 4; row++)
      for (int col = 0; col < 5; col++)
        if ((row + col) % 2) display->drawPixel(12 + col, y + 3 + row, 0xFFFF);
  // When places change, the codes slide to their new slots. Not when the row
  // wasn't showing a moment ago (another page was up), then it just updates.
  static String shown[3];
  static int from[3] = {0, 1, 2};
  static unsigned long changedAt = 0, lastDrawn = 0;
  unsigned long now = millis();
  bool changed = false;
  for (int i = 0; i < 3; i++) changed |= shown[i] != race.top[i];
  if (changed) {
    bool animate = slide && now - lastDrawn < 1000;
    for (int i = 0; i < 3; i++) {
      from[i] = i;
      for (int j = 0; j < 3; j++)
        if (animate && shown[j] == race.top[i]) from[i] = j;
    }
    for (int i = 0; i < 3; i++) shown[i] = race.top[i];
    changedAt = now;
  }
  lastDrawn = now;
  float t = std::min(1.0f, (now - changedAt) / (float)RACE_SLIDE_MS);
  t = t * t * (3 - 2 * t);  // eases in and out

  for (int i = 0; i < 3; i++) {
    int16_t x = 20 + (int16_t)roundf((from[i] + (i - from[i]) * t) * 15);
    display->setTextColor(text);
    display->setCursor(x, baseline);
    display->print(race.top[i]);
    display->drawFastHLine(x, y + 7, 14, race.color[i]);
  }
  display->setFont(NULL);
}

#endif
