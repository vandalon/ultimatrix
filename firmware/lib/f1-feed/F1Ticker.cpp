// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "F1Ticker.h"
#include <NetworkBudget.h>

#include <algorithm>
#include <memory>
#include <HTTPClient.h>
#include <Console.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TimeService.h>

#include <EspnFeed.h>
#include <F1Drivers.h>
#include "JsonScan.h"

static const unsigned long LIVE_REFRESH_MS = 15 * 1000UL;  // plus a request per driver for the times
static const unsigned long LIVE_FEED_REFRESH_MS = 90 * 1000UL;  // the live feed is up to date: it has all the live view shows
static const unsigned long WEEKEND_REFRESH_MS = 5 * 60 * 1000UL;  // between sessions
static const unsigned long START_REFRESH_MS = 30 * 1000UL;        // a session is about to start
static const unsigned long IDLE_REFRESH_MS = 30 * 60 * 1000UL;
static const unsigned long STANDINGS_REFRESH_MS = 30 * 60 * 1000UL;
static const unsigned long SEASON_REFRESH_MS = 60 * 60 * 1000UL;
static const unsigned long RETRY_MS = 60 * 1000UL;
// true = never start the F1 live-timing connection (the gaps and lap times), for finding memory problems
static const bool F1_LIVE_FEED_OFF = false;
static const uint32_t TASK_STACK = 9216;  // it uses about 5.5 KB, and every KB counts while the live feed is connected
static const time_t DELAY_GRACE_SECS = 3 * 60;     // a session this long past its start time and still not running is delayed
static const time_t MOVED_MIN_SECS = 5 * 60;  // a start this much later than first seen is a new start time
static const time_t DELAY_MAX_SECS = 6 * 60 * 60;  // and is forgotten this long after
static const time_t WEEKEND_TAIL_SECS = 3 * 60 * 60;  // without a clock, the weekend lasts this long after the race started

extern volatile bool faceBootDone;      // main.cpp: the first update check waits for the first refresh

uint16_t F1Ticker::driverColor(const char *code) { return f1DriverColor(code); }

static void f1log(const char *format, ...) {
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


void F1Ticker::begin(ClockTime *dateTime) {
  if (_started) return;
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT)
    f1log("[F1] the last restart was a crash; F1 code was at: %.15s\n", f1Phase);
  f1At("begin");
  _started = true;
  _dateTime = dateTime;
  loadCache();
  // Core 0, so downloading never stalls the display loop on core 1
  resume();
}

// After an update check the task is gone (see task()): start it again once the update is over
void F1Ticker::resume() {
  if (!_started || _task || firmwareUpdating || f1Park) return;
  if (createFeedTask(task, "f1", TASK_STACK, this, 1, &_task, 0) == pdPASS) return;
  static unsigned long loggedAt = 0;
  if (!loggedAt || millis() - loggedAt > 10000) {
    loggedAt = millis();
    memoryAllocationFailures.fetch_add(1, std::memory_order_relaxed);  // tells main's guard to shed telnet and mDNS
    f1log("[F1] task cannot restart (data RAM free %u, largest block %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
}

// The running order from the live feed replaces ESPN's while it is up to date; ESPN still says which session it is
// How many places have a time or gap
static int timedRows(const F1Ticker::Row *rows) {
  int n = 0;
  for (int i = 0; i < F1Ticker::ROWS; i++)
    if (rows[i].time[0]) n++;
  return n;
}

// A refresh that got no time for a driver (a request failed, or has not come back yet) keeps the time he had in the
// same session
static void keepTimes(F1Ticker::Session &now, const F1Ticker::Session &old) {
  if (!now.valid || !old.valid || !now.id || now.id != old.id) return;
  for (F1Ticker::Row &row : now.rows) {
    if (!row.code[0] || row.time[0]) continue;
    for (const F1Ticker::Row &before : old.rows)
      if (before.time[0] && strcmp(before.time, "DNF") && strcmp(before.time, "RETIRED") && !strcmp(before.code, row.code)) {
        strlcpy(row.time, before.time, sizeof(row.time));
        break;
      }
  }
}

void F1Ticker::snapshot(Snapshot &out, bool intervals) {
  {
    std::lock_guard<std::mutex> guard(_lock);
    out = _snap;
  }
  F1LiveState::Row rows[F1LiveState::ROWS];
  int count = 0, lap = 0, totalLaps = 0;
  char flag = 0;
  bool finished = false;
  long finishedUtc = 0;
  if (out.live.valid && _live.get(out.live.tag, rows, count, lap, totalLaps, flag, finished, finishedUtc, intervals)) {
    for (int i = 0; i < ROWS; i++) {
      strlcpy(out.live.rows[i].code, rows[i].code, sizeof(out.live.rows[i].code));
      strlcpy(out.live.rows[i].time, rows[i].time, sizeof(out.live.rows[i].time));
      out.live.rows[i].color = rows[i].color;
      out.live.rows[i].fastest = rows[i].fastest;
    }
    out.live.count = count;
    out.live.fromFeed = true;
    if (lap > 0) out.live.period = lap;
    out.live.totalLaps = totalLaps;
    out.live.flag = flag;
    out.live.finished = finished;
    out.live.finishedAt = finishedUtc;
    bool intervalView = intervals && !finished && (out.live.tag == 'R' || out.live.tag == 'S');
    if (out.live.id && !intervalView) {  // save only gaps to leader, so cached and final results keep their meaning
      std::lock_guard<std::mutex> guard(_lock);
      if (_saved.id != out.live.id || memcmp(_saved.rows, out.live.rows, sizeof(_saved.rows)) != 0 || _saved.flag != flag) {
        _saved.id = out.live.id;
        _saved.count = count;
        _saved.flag = flag;
        memcpy(_saved.rows, out.live.rows, sizeof(_saved.rows));
        _savedDirty = true;
        _savedFromFlash = false;
      }
    }
    for (int i = 0; i < ROWS; i++) {  // after the saving: these come and go within seconds
      out.live.rows[i].improved = rows[i].improved;
      out.live.rows[i].pit = rows[i].pit;
      out.live.rows[i].finished = rows[i].finished;
    }
    int part = _live.part();
    if (out.live.tag == 'Q' && part >= 1 && part <= 3) snprintf(out.live.name, sizeof(out.live.name), "%s%d", out.live.name[0] == 'S' ? "SQ" : "Q", part);
    int left = _live.clockSecs((long)ultime::now());
    if (left >= 0 && (out.live.tag == 'P' || out.live.tag == 'Q')) snprintf(out.live.clock, sizeof(out.live.clock), "%d:%02d", left / 60 % 100, left % 60);
    std::lock_guard<std::mutex> guard(_lock);
    strlcpy(_saved.name, out.live.name, sizeof(_saved.name));
  } else {
    // No fresh data from the feed: the last known order and times of this session, from before a restart or a gap
    std::lock_guard<std::mutex> guard(_lock);
    Session *sessions[] = {&out.live, &out.last};
    for (Session *session : sessions)
      if (session->valid && session->id && session->id == _saved.id && _saved.count &&
          (!session->rows[0].time[0] || (session == &out.last && timedRows(_saved.rows) > timedRows(session->rows)))) {
        memcpy(session->rows, _saved.rows, sizeof(session->rows));
        session->count = _saved.count;
        // From before the boot: shown dimmed until a download brings something new (the times, for a finished one)
        session->stale = _savedFromFlash && (session == &out.live || !_renewed.times);
        if (session == &out.live) {
          session->flag = _saved.flag;
          if (_saved.name[0]) strlcpy(session->name, _saved.name, sizeof(session->name));
        }
      }
  }
  if (intervals && out.live.valid && !out.live.finished && !out.live.fromFeed &&
      (out.live.tag == 'R' || out.live.tag == 'S')) {
    // The fallback and flash hold gaps to leader, not direct intervals. Wait for the feed's interval values.
    for (int i = 1; i < ROWS; i++) out.live.rows[i].time[0] = 0;
  }
}

uint32_t F1Ticker::version() {
  resume();
  std::lock_guard<std::mutex> guard(_lock);
  return _version + _live.updates();
}

void F1Ticker::task(void *self) {
  waitForFeedRegistration();
  F1Ticker *ticker = static_cast<F1Ticker *>(self);
  f1TaskUp = true;
  for (;;) {
    if (firmwareUpdating || f1Park) {  // the update, or the live feed's handshake, has the network and the heap to itself
      // The TLS handshake needs big free blocks: hand back the task stack.
      // The data stays in the ticker; resume() starts the task again after the update.
      ticker->_task = nullptr;
      f1TaskUp = false;
      stopFeedTask();
    }
    // in slices, so an update that starts meanwhile is seen within a second
    for (unsigned long left = ticker->refresh(); left > 0 && !firmwareUpdating && !f1Park;) {
      if (ticker->_feedPaced && !ticker->_live.fresh()) {  // the live feed went quiet: back to ESPN every 15 s
        ticker->_feedPaced = false;
        ticker->_weekendAt = millis();
        break;
      }
      unsigned long slice = std::min(left, 1000UL);
      vTaskDelay(pdMS_TO_TICKS(slice));
      left -= slice;
    }
  }
}

// Milliseconds until the millis() value at, 0 when it has passed
static unsigned long until(unsigned long at) {
  long left = (long)(at - millis());
  return left > 0 ? left : 0;
}

// The standings and the next race change slowly, so the last download is kept in flash: it shows right after
// a boot (the standings dimmed, as they are downloaded again at the first refresh) and the next race's HTTPS
// download only runs once it is older than its refresh time
void F1Ticker::loadCache() {
  _cacheLoaded = true;
  Preferences prefs;
  if (!prefs.begin("um-f1cache", true)) return;
  time_t now = ultime::now();
  bool synced = ultime::timeStatus() == timeSet;
  Standing standings[STANDINGS];
  uint8_t count = prefs.getUChar("standN", 0);
  time_t at = prefs.getUInt("standAt", 0);
  if (count > 0 && count <= STANDINGS && prefs.getBytesLength("stand") == sizeof(standings) && at && (!synced || now >= at)) {
    prefs.getBytes("stand", standings, sizeof(standings));
    std::lock_guard<std::mutex> guard(_lock);
    for (int i = 0; i < count; i++) _snap.standings[i] = standings[i];
    _snap.standingsCount = count;
    _snap.standingsStale = true;  // shown dimmed until the download at the first refresh
    _version++;
  }
  if (prefs.getBytesLength("liveRows") == sizeof(_saved)) {
    SavedRows saved;  // read before taking the lock: flash reads are slow and the display thread waits for it
    prefs.getBytes("liveRows", &saved, sizeof(saved));
    std::lock_guard<std::mutex> guard(_lock);
    _saved = saved;
    // Older firmware inferred retirement from missing times, including lapped finishers.
    // Reconfirm these labels from current status data before showing them again.
    for (Row &row : _saved.rows)
      if (!strcmp(row.time, "DNF") || !strcmp(row.time, "RETIRED")) row.time[0] = 0;
    _savedFromFlash = true;
  }
  if (prefs.getBytesLength("cars") == sizeof(_cars)) {
    prefs.getBytes("cars", _cars, sizeof(_cars));
    _carCount = prefs.getUChar("carN", 0);
    _carEvent = prefs.getUInt("carEvent", 0);
    if (_carCount > CARS) _carCount = 0;
  }
  Race race;
  at = prefs.getUInt("raceAt", 0);
  if (prefs.getBytesLength("race") == sizeof(race) && at && (!synced || now >= at)) {
    prefs.getBytes("race", &race, sizeof(race));
    unsigned long age = synced ? now - at : 0;  // seconds
    if (race.city[0] && (!synced || race.start > now)) {
      std::lock_guard<std::mutex> guard(_lock);
      _snap.upcoming[0] = race;
      _snap.upcomingCount = 1;
      _version++;
      if (synced && age < SEASON_REFRESH_MS / 1000) _seasonAt = millis() + (SEASON_REFRESH_MS / 1000 - age) * 1000UL;
    }
  }
  prefs.end();
  f1log("[F1] from flash: %d standings, next race %s, next downloads in %lu and %lu s\n", _snap.standingsCount,
        _snap.upcomingCount ? _snap.upcoming[0].city : "-", until(_standingsAt) / 1000, until(_seasonAt) / 1000);
}

// While a session runs the order is saved every 2 minutes; when it has ended, at once
void F1Ticker::persistLive() {
  SavedRows copy;
  {
    std::lock_guard<std::mutex> guard(_lock);
    bool over = !_snap.live.valid;
    if (!_savedDirty || (!over && millis() - _savedAt < 2 * 60 * 1000UL && _savedAt)) return;
    copy = _saved;
    _savedDirty = false;
    _savedAt = millis();
  }
  Preferences prefs;
  if (!prefs.begin("um-f1cache", false)) return;
  prefs.putBytes("liveRows", &copy, sizeof(copy));
  prefs.end();
  f1log("[F1] saved the order of session %lu (%d places)\n", (unsigned long)copy.id, copy.count);
}

// "[F1] renewed: weekend 17:55:02, times 17:41:10 (OpenF1), standings 16:02:11, season 09:15:40, live feed on":
// the local time each kind of data last came in whole, "-" when not since the boot
void F1Ticker::logRenewed() {
  char text[4][12];
  const time_t at[4] = {_renewed.weekend, _renewed.times, _renewed.standings, _renewed.season};
  for (int i = 0; i < 4; i++) {
    if (!at[i] || !_dateTime) { strlcpy(text[i], "-", sizeof(text[i])); continue; }
    time_t local = _dateTime->utcToLocal(at[i]);
    struct tm t;
    gmtime_r(&local, &t);
    snprintf(text[i], sizeof(text[i]), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }
  f1log("[F1] renewed: weekend %s, times %s%s%s%s, standings %s, season %s, live feed %s\n", text[0], text[1],
        _renewed.times ? " (" : "", _renewed.timesFrom, _renewed.times ? ")" : "", text[2], text[3], _live.connected() ? "on" : "off");
}

// Does whatever download is due. Returns how long to wait before looking again.
unsigned long F1Ticker::refresh() {
  if (WiFi.status() != WL_CONNECTED || ultime::timeStatus() != timeSet) return 10 * 1000UL;
  std::lock_guard<std::mutex> net(f1NetLock);  // the live feed connects between two refreshes, see F1Live.h
  if (!_cacheLoaded) loadCache();
  persistLive();
  if (until(_weekendAt) == 0) _weekendAt = millis() + fetchWeekend();
  // The two Jolpica downloads are HTTPS
  // The live feed's connection holds the TLS memory: these wait until it's closed (they are not urgent)
  bool liveOpen = liveDataActive();
  if (until(_standingsAt) == 0 && !liveOpen) {
    _standingsAt = millis() + fetchStandings();
  }
  if (until(_seasonAt) == 0 && !liveOpen) {
    _seasonAt = millis() + fetchSeason();
  }
  f1At("idle");
  f1log("[F1] refresh done (stack left %u, heap free %u, largest block %u)\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  logRenewed();
  faceBootDone = true;
  if (liveOpen && (until(_standingsAt) == 0 || until(_seasonAt) == 0)) return std::max(1000UL, std::min(until(_weekendAt), 30 * 1000UL));
  return std::max(1000UL, std::min(until(_weekendAt), std::min(until(_standingsAt), until(_seasonAt))));
}

static void copyName(char *to, size_t size, const char *from) { strlcpy(to, from, size); }

// "M. Verstappen" -> "VER" in the team's colour, unknown drivers in white
static void fillRow(F1Ticker::Row &row, const char *shortName) {
  const char *surname = strrchr(shortName, ' ');
  surname = surname ? surname + 1 : shortName;
  if (const F1Driver *known = f1DriverBySurname(surname)) {
    strlcpy(row.code, known->code, sizeof(row.code));
    row.color = known->color;
    return;
  }
  size_t n = 0;
  for (; n < 3 && surname[n]; n++) row.code[n] = toupper((unsigned char)surname[n]);
  row.code[n] = 0;
  row.color = 0xFFFF;
}

// The ESPN scoreboard, read as it streams in. A session (a competition) is complete when the next one starts or the
// text ends, and so is a driver: only the ones being read are held, never the document.
struct WeekendScan {
  time_t nowUtc = 0;
  char eventId[16] = "", eventDate[24] = "", eventEnd[24] = "", liveId[16] = "", circuit[48] = "";
  F1Ticker::Snapshot snap;
  time_t lastDate = 0, nextDate = 0;

  int comp = -1, competitor = -1;  // which session and driver are being read
  F1Ticker::Session s;
  char id[16] = "", date[24] = "", type[8] = "", state[8] = "", clock[8] = "", statusName[24] = "";
  int period = 0;
  uint32_t delayed[8] = {};  // sessions seen delayed (in and out)
  uint8_t delayedCount = 0;
  F1SeenStart seen[8];  // the first start time seen of each session still to come
  uint8_t seenCount = 0;
  bool anyDelayed = false;   // a session is delayed now
  uint32_t driverId = 0;
  int order = 0;
  char driverName[24] = "";

  void flushCompetitor() {
    if (competitor < 0) return;
    if (order >= 1 && order <= F1Ticker::ROWS) {
      fillRow(s.rows[order - 1], driverName);
      s.rows[order - 1].athleteId = driverId;
      s.count = std::max((int)s.count, order);
    }
    competitor = -1;
    driverId = 0;
    order = 0;
    driverName[0] = 0;
  }

  // The session just read is sorted into the live one, the last finished and the next to come
  void finish() {
    if (comp < 0) return;
    flushCompetitor();
    time_t when = parseEspnDate(date);
    s.valid = true;
    s.start = when;
    s.eventId = strtoul(eventId, nullptr, 10);
    s.id = strtoul(id, nullptr, 10);
    if (!strncmp(type, "FP", 2)) {
      s.tag = 'P';
      copyName(s.name, sizeof(s.name), type);
    } else if (!strcmp(type, "Qual")) {
      s.tag = 'Q';
      copyName(s.name, sizeof(s.name), "QUALI");
    } else if (!strcmp(type, "SS")) {  // sprint qualifying
      s.tag = 'Q';
      copyName(s.name, sizeof(s.name), "SQ");
    } else if (!strcmp(type, "SR")) {
      s.tag = 'S';
      copyName(s.name, sizeof(s.name), "SPRINT");
    } else {
      s.tag = 'R';
      copyName(s.name, sizeof(s.name), "RACE");
    }
    if (!strcmp(state, "pre")) {
      for (F1Ticker::Row &row : s.rows) row = F1Ticker::Row();
      s.count = 0;
      // ESPN does not flag a late start: the start time just stays in the past until a new one is set
      bool late = when + DELAY_GRACE_SECS <= nowUtc && when + DELAY_MAX_SECS > nowUtc;
      bool delayedNow = late || strstr(statusName, "DELAY") || strstr(statusName, "POSTPON");
      bool known = false;
      for (int i = 0; i < delayedCount; i++) known = known || delayed[i] == s.id;
      // A new start time announced before the old one passed: the start moved later than first seen
      int at = -1;
      for (int i = 0; i < seenCount; i++) if (seen[i].id == s.id) at = i;
      if (at < 0) {
        if (seenCount == 8) {
          memmove(seen, seen + 1, 7 * sizeof(seen[0]));
          seenCount--;
        }
        seen[seenCount].id = s.id;
        seen[seenCount].start = (uint32_t)when;
        seenCount++;
      } else if (!known && (long)when - (long)seen[at].start >= MOVED_MIN_SECS) {
        known = true;
        if (delayedCount == 8) {
          memmove(delayed, delayed + 1, 7 * sizeof(delayed[0]));
          delayedCount--;
        }
        delayed[delayedCount++] = s.id;
      }
      if (delayedNow) {
        s.status = 'd';
        anyDelayed = true;
        if (!known) {
          if (delayedCount == 8) {
            memmove(delayed, delayed + 1, 7 * sizeof(delayed[0]));
            delayedCount--;
          }
          delayed[delayedCount++] = s.id;
        }
      } else if (known && when > nowUtc) {
        s.status = 'n';
      }
      // ESPN calls a session that has just started 'pre' for a few minutes: it stays the next one, not shown as delayed yet
      bool shown = delayedNow || when > nowUtc || when + DELAY_GRACE_SECS > nowUtc;
      if (shown && (nextDate == 0 || when < nextDate)) {
        nextDate = when;
        snap.next = s;
      }
      if (shown && snap.comingCount < F1Ticker::Snapshot::COMING) {  // in order of start
        int at = snap.comingCount;
        while (at > 0 && snap.coming[at - 1].start > when) {
          snap.coming[at] = snap.coming[at - 1];
          at--;
        }
        copyName(snap.coming[at].name, sizeof(snap.coming[at].name), s.name);
        snap.coming[at].start = when;
        snap.coming[at].status = s.status;
        snap.comingCount++;
      }
    } else {
      s.period = period;
      if (strcmp(clock, "0:00") && strcmp(clock, "0.0")) copyName(s.clock, sizeof(s.clock), clock);
      if (!strcmp(state, "in")) {
        s.live = true;
        snap.live = s;
        strlcpy(liveId, id, sizeof(liveId));
      } else if (!strcmp(state, "post") && when >= lastDate) {
        lastDate = when;
        snap.last = s;
      }
    }
    comp = -1;
  }

  void start(int index) {
    finish();
    comp = index;
    s = F1Ticker::Session();
    id[0] = date[0] = type[0] = state[0] = clock[0] = statusName[0] = 0;
    period = 0;
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    WeekendScan &w = *(WeekendScan *)sink;
    if (n < 3 || strcmp(p[0], "events") || strcmp(p[1], "0")) return;  // the first event only
    if (n == 3) {
      if (!strcmp(p[2], "id")) strlcpy(w.eventId, v, sizeof(w.eventId));
      else if (!strcmp(p[2], "date")) strlcpy(w.eventDate, v, sizeof(w.eventDate));
      else if (!strcmp(p[2], "endDate")) strlcpy(w.eventEnd, v, sizeof(w.eventEnd));
      return;
    }
    if (!strcmp(p[2], "circuit")) {  // the name, then the city
      if (jsonPathIs(p, n, "events.0.circuit.fullName") || jsonPathIs(p, n, "events.0.circuit.address.city")) {
        if (w.circuit[0]) strlcat(w.circuit, " ", sizeof(w.circuit));
        strlcat(w.circuit, v, sizeof(w.circuit));
      }
      return;
    }
    if (n < 5 || strcmp(p[2], "competitions")) return;
    int c = atoi(p[3]);
    if (c != w.comp) w.start(c);
    if (jsonPathIs(p, n, "events.0.competitions.*.id")) strlcpy(w.id, v, sizeof(w.id));
    else if (jsonPathIs(p, n, "events.0.competitions.*.date")) strlcpy(w.date, v, sizeof(w.date));
    else if (jsonPathIs(p, n, "events.0.competitions.*.type.abbreviation")) strlcpy(w.type, v, sizeof(w.type));
    else if (jsonPathIs(p, n, "events.0.competitions.*.status.period")) w.period = atoi(v);
    else if (jsonPathIs(p, n, "events.0.competitions.*.status.displayClock")) strlcpy(w.clock, v, sizeof(w.clock));
    else if (jsonPathIs(p, n, "events.0.competitions.*.status.type.name")) {
      strlcpy(w.statusName, v, sizeof(w.statusName));
      for (char *c = w.statusName; *c; c++) *c = toupper((unsigned char)*c);
    } else if (jsonPathIs(p, n, "events.0.competitions.*.status.type.state")) strlcpy(w.state, v, sizeof(w.state));
    else if (n >= 7 && !strcmp(p[4], "competitors")) {
      int k = atoi(p[5]);
      if (k != w.competitor) {
        w.flushCompetitor();
        w.competitor = k;
      }
      if (jsonPathIs(p, n, "events.0.competitions.*.competitors.*.id")) w.driverId = strtoul(v, nullptr, 10);
      else if (jsonPathIs(p, n, "events.0.competitions.*.competitors.*.order")) w.order = atoi(v);
      else if (jsonPathIs(p, n, "events.0.competitions.*.competitors.*.athlete.shortName")) strlcpy(w.driverName, v, sizeof(w.driverName));
    }
  }
};

unsigned long F1Ticker::fetchWeekend() {
  f1At("fetch weekend");
  std::unique_ptr<WeekendScan> scanHeap(new WeekendScan);  // big: not on the task's stack
  WeekendScan &scan = *scanHeap;
  scan.nowUtc = ultime::now();
  memcpy(scan.delayed, _delayed, sizeof(_delayed));
  scan.delayedCount = _delayedCount;
  memcpy(scan.seen, _seen, sizeof(_seen));
  scan.seenCount = _seenCount;
  if (!getScan("http://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard", WeekendScan::leaf, &scan)) return RETRY_MS;
  scan.finish();
  memcpy(_delayed, scan.delayed, sizeof(_delayed));
  _delayedCount = scan.delayedCount;
  memcpy(_seen, scan.seen, sizeof(_seen));
  _seenCount = scan.seenCount;

  time_t nowUtc = scan.nowUtc, nextDate = scan.nextDate;
  String eventId = scan.eventId, liveId = scan.liveId;
  time_t weekendStart = parseEspnDate(scan.eventDate), weekendEnd = parseEspnDate(scan.eventEnd);
  Snapshot &snap = scan.snap;
  // The weekend is over at the end of the race day (local time); without a clock, a few hours after the race
  time_t weekendOver = weekendEnd + WEEKEND_TAIL_SECS;
  if (weekendEnd && _dateTime) {
    time_t local = _dateTime->utcToLocal(weekendEnd);
    weekendOver = weekendEnd + ((local / 86400 + 1) * 86400 - local);
  }
  snap.weekend = snap.live.valid || (weekendStart && nowUtc >= weekendStart && nowUtc <= weekendOver);

  // The flag is in the session's own status: keep safety car and virtual safety car distinct
  if (snap.live.valid && !eventId.isEmpty() && !liveId.isEmpty()) {
    char url[160];
    snprintf(url, sizeof(url),
             "http://sports.core.api.espn.com/v2/sports/racing/leagues/f1/events/%s/competitions/%s/status",
             eventId.c_str(), liveId.c_str());
    char flagText[24] = "";
    if (getScan(url, [](void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
          if (n == 1 && !strcmp(p[0], "flag")) strlcpy((char *)sink, v, 24);
        }, flagText)) {
      String name = flagText;
      name.toUpperCase();
      if (name.indexOf("RED") >= 0) snap.live.flag = 'r';
      else if (name == "VSC" || name.indexOf("VIRTUAL") >= 0) snap.live.flag = 'v';
      else if (name == "SC" || name.indexOf("SAFETY") >= 0) snap.live.flag = 's';
      else if (name.indexOf("YELLOW") >= 0 || name.indexOf("CAUTION") >= 0) snap.live.flag = 'y';
      f1log("[F1] flag: %s\n", name.c_str());
    }
  }
  f1log("[F1] circuit: %s\n", scan.circuit);
  f1log("[F1] weekend %d, live %s %s, last %s, next %s\n", snap.weekend, snap.live.valid ? snap.live.name : "-",
        snap.live.valid ? snap.live.rows[0].code : "", snap.last.valid ? snap.last.name : "-",
        snap.next.valid ? snap.next.name : "-");

  {
    std::lock_guard<std::mutex> guard(_lock);
    keepTimes(snap.live, _snap.live);
    keepTimes(snap.last, _snap.last);
    _snap.weekend = snap.weekend;
    strlcpy(_snap.circuit, scan.circuit, sizeof(_snap.circuit));
    _snap.live = snap.live;
    liveEventOn = snap.live.valid;
    _snap.last = snap.last;
    _snap.next = snap.next;
    for (int i = 0; i < Snapshot::COMING; i++) _snap.coming[i] = snap.coming[i];
    _snap.comingCount = snap.comingCount;
    _version++;
  }

  // The live feed runs from shortly before a session until a few minutes after it; a delayed one has to wait for ESPN to say it is on
  _live.setWanted(!F1_LIVE_FEED_OFF && (snap.live.valid || (snap.weekend && snap.next.valid && snap.next.status != 'd' && snap.next.start - nowUtc < 10 * 60)));

  // Times and gaps cost a request per driver: only for the drivers shown, the last session once
  if (snap.live.valid && !_live.fresh() && !_live.connected()) {  // the live feed has them otherwise
    fetchTimes(snap.live, TIMED);
    _liveFails = snap.live.timeFails ? (_liveFails < 255 ? _liveFails + 1 : 255) : 0;  // refreshes in a row without times
    snap.live.timeFails = _liveFails;
  }
  if (snap.last.valid) {
    if (_lastTimed.valid && _lastTimed.id == snap.last.id) {
      snap.last = _lastTimed;
    } else if (!snap.live.valid && !liveFeedReading) {  // leave historical results downloads until the live session ends
      // Everyone: the result lists show them all. Practice and qualifying come in a few requests from OpenF1, the rest
      // (or when it has nothing yet) one request per driver from ESPN
      if (!((snap.last.tag == 'P' || snap.last.tag == 'Q') && fetchOpenF1(snap.last))) fetchTimes(snap.last, ROWS);
      {
        // The live feed knew who had the fastest lap of a race; Jolpica only has it hours later
        std::lock_guard<std::mutex> guard(_lock);
        if (_saved.id == snap.last.id)
          for (const Row &old : _saved.rows)
            for (Row &row : snap.last.rows)
              if (!strcmp(row.code, old.code)) {
                if (old.fastest) row.fastest = true;
              }
      }
      {
        std::lock_guard<std::mutex> guard(_lock);
        keepTimes(snap.last, _snap.last);
      }
      if (snap.last.rows[0].time[0]) {
        // Downloaded again, a few times at most, while a driver has no time: one failed request must not stay
        bool complete = !snap.last.timeFails;
        if (_timedId != snap.last.id) { _timedId = snap.last.id; _timedTries = 0; }
        bool giveUp = !complete && ++_timedTries >= 3;
        f1log("[F1] times %s: %s, try %d, fails %d\n", snap.last.name, complete ? "complete" : "a download failed", _timedTries, snap.last.timeFails);
        snap.last.timeFails = complete ? 0 : giveUp ? 255 : _timedTries;  // 2 or more: the dot turns red
        if (complete || giveUp) _lastTimed = snap.last;
        // Keep the final result for the next boot: the saved order is otherwise the live feed's older one
        std::lock_guard<std::mutex> guard(_lock);
        _saved.id = snap.last.id;
        _saved.count = snap.last.count;
        memcpy(_saved.rows, snap.last.rows, sizeof(_saved.rows));
        _savedDirty = true;
      }
    }
    // A race or sprint: who had the fastest lap. Jolpica has the results some time after the finish: look again
    // every 10 minutes until they are there
    bool race = snap.last.tag == 'R' || snap.last.tag == 'S';
    bool have = false;
    for (const Row &row : snap.last.rows) have = have || row.fastest;
    if (!snap.live.valid && !liveFeedReading && race && !have && (_fastestId != snap.last.id || millis() - _fastestAt > 10 * 60 * 1000UL)) {
      _fastestId = snap.last.id;
      _fastestAt = millis();
      if (fetchFastest(snap.last) && _lastTimed.valid && _lastTimed.id == snap.last.id) _lastTimed = snap.last;
    }
  }

  // Published once, with the times in: a first publish without them made the time column blank for the
  // seconds the requests above take, on every refresh
  {
    std::lock_guard<std::mutex> guard(_lock);
    keepTimes(snap.live, _snap.live);
    keepTimes(snap.last, _snap.last);
    _snap.weekend = snap.weekend;
    _snap.live = snap.live;
    _snap.last = snap.last;
    _snap.next = snap.next;
    _version++;
  }

  _renewed.weekend = ultime::now();

  // Be there when the next session starts
  unsigned long wait = IDLE_REFRESH_MS;
  // While the live feed is up to date ESPN is only needed for the session's end and what comes next; the task
  // looks again at once when the feed goes quiet (see task())
  _feedPaced = snap.live.valid && _live.connected() && _live.fresh();
  if (_feedPaced) return LIVE_FEED_REFRESH_MS;
  if (snap.live.valid) return LIVE_REFRESH_MS;
  if (snap.weekend) wait = WEEKEND_REFRESH_MS;
  if (nextDate > nowUtc) {
    time_t left = nextDate - nowUtc;
    wait = left < 15 * 60 ? START_REFRESH_MS : std::min(wait, (unsigned long)(left - 14 * 60) * 1000UL);
  }
  if (snap.last.valid && snap.last.timeFails && !(_lastTimed.valid && _lastTimed.id == snap.last.id)) wait = std::min(wait, 30000UL);  // a driver's time is missing: try again soon
  f1log("[F1] next refresh in %lu s\n", wait / 1000);
  if (scan.anyDelayed) wait = std::min(wait, START_REFRESH_MS * 4);  // look for the new start time
  return wait;
}

// The top of the drivers' championship from Jolpica (HTTPS only; the answer is tiny)
struct StandingsScan {
  F1Ticker::Standing standings[F1Ticker::STANDINGS];
  uint8_t count = 0;
  int index = -1;
  char code[4] = "---", points[8] = "0";

  void flush() {
    if (index >= 0 && count < F1Ticker::STANDINGS) {
      strlcpy(standings[count].code, code, sizeof(standings[count].code));
      standings[count].color = F1Ticker::driverColor(code);
      standings[count].points = atoi(points);
      count++;
    }
    index = -1;
    strcpy(code, "---");
    strcpy(points, "0");
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    StandingsScan &s = *(StandingsScan *)sink;
    bool isPoints = jsonPathIs(p, n, "MRData.StandingsTable.StandingsLists.0.DriverStandings.*.points");
    if (!isPoints && !jsonPathIs(p, n, "MRData.StandingsTable.StandingsLists.0.DriverStandings.*.Driver.code")) return;
    int k = atoi(p[5]);
    if (k != s.index) {
      s.flush();
      s.index = k;
    }
    if (isPoints) strlcpy(s.points, v, sizeof(s.points));
    else strlcpy(s.code, v, sizeof(s.code));
  }
};

unsigned long F1Ticker::fetchStandings() {
  f1At("fetch standings");
  StandingsScan scan;
  if (!getScan("https://api.jolpi.ca/ergast/f1/current/driverstandings.json?limit=30", StandingsScan::leaf, &scan)) return RETRY_MS;
  scan.flush();
  Standing *standings = scan.standings;
  uint8_t count = scan.count;
  f1log("[F1] standings: %d drivers, leader %s\n", count, count ? standings[0].code : "-");
  // What was just downloaded (not the snapshot or the flash copy), to compare with the Jolpica answer
  char top[96] = "";
  for (int i = 0; i < count && i < 6; i++) {
    char entry[16];
    snprintf(entry, sizeof(entry), "%s%d.%s %d", i ? ", " : "", i + 1, standings[i].code, standings[i].points);
    strlcat(top, entry, sizeof(top));
  }
  f1log("[F1] standings downloaded: %s\n", top);
  if (count == 0) return RETRY_MS;
  _renewed.standings = ultime::now();

  {
    std::lock_guard<std::mutex> guard(_lock);
    for (int i = 0; i < count; i++) _snap.standings[i] = standings[i];
    _snap.standingsCount = count;
    _snap.standingsStale = false;
    _version++;
  }
  // Flash writes are slow: not while the display thread waits for the lock
  Preferences prefs;
  if (prefs.begin("um-f1cache", false)) {
    prefs.putBytes("stand", standings, sizeof(scan.standings));  // the whole array: standings is a pointer
    prefs.putUChar("standN", count);
    prefs.putUInt("standAt", ultime::now());
    prefs.end();
  }
  return STANDINGS_REFRESH_MS;
}

// "1:35.130" (a lap), "1:47:14.808" (a race) or "35.130" in milliseconds, -1 when it isn't a time
static long parseTimeMs(const char *text) {
  if (!*text || !isdigit((unsigned char)*text)) return -1;
  long parts[3];
  int n = 0;
  const char *p = text;
  char *end;
  while (n < 3) {
    parts[n++] = strtol(p, &end, 10);
    if (*end != ':') break;
    p = end + 1;
  }
  long ms = 0;
  if (*end == '.') {
    char fraction[4] = "000";
    int i = 0;
    for (const char *q = end + 1; isdigit((unsigned char)*q) && i < 3; q++) fraction[i++] = *q;
    ms = atoi(fraction);
  }
  long seconds = 0;
  for (int i = 0; i < n; i++) seconds = seconds * 60 + parts[i];
  return seconds * 1000 + ms;
}

// A gap as the 3x5 font shows it: "+.088", "+1.234", "+12.3", "+1:05"
static void formatGap(char *out, size_t size, long ms) {
  if (ms < 1000) snprintf(out, size, "+0.%03ld", ms);
  else if (ms < 10000) snprintf(out, size, "+%ld.%03ld", ms / 1000, ms % 1000);
  else if (ms < 60000) snprintf(out, size, "+%ld.%ld", ms / 1000, ms % 1000 / 100);
  else snprintf(out, size, "+%ld:%02ld", ms / 60000, ms / 1000 % 60);
}

// One driver's statistics: the entry called totalTime holds the time
struct TimesScan {
  int index = -1;
  char name[16] = "", value[16] = "", total[16] = "";
  bool have = false;

  void flush() {
    if (index >= 0 && !strcmp(name, "totalTime")) {
      strlcpy(total, value, sizeof(total));
      have = true;
    }
    index = -1;
    name[0] = value[0] = 0;
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    TimesScan &t = *(TimesScan *)sink;
    bool isName = jsonPathIs(p, n, "splits.categories.0.stats.*.name");
    if (!isName && !jsonPathIs(p, n, "splits.categories.0.stats.*.displayValue")) return;
    int k = atoi(p[4]);
    if (k != t.index) {
      t.flush();
      t.index = k;
    }
    if (isName) strlcpy(t.name, v, sizeof(t.name));
    else strlcpy(t.value, v, sizeof(t.value));
  }
};

// An explicit result status, with the number of laps completed. Missing totalTime alone says nothing about a DNF.
struct FinishScan {
  char status[24] = "";
  int laps = -1;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    FinishScan &f = *(FinishScan *)sink;
    if (jsonPathIs(p, n, "type.name")) strlcpy(f.status, v, sizeof(f.status));
    else if (jsonPathIs(p, n, "period")) f.laps = atoi(v);
  }
};

// Fills in the times of the first count drivers: ESPN's totalTime is the best lap in practice and
// qualifying and the race time in a race, so the first place shows it (or LEAD in a race) and the others
// their gap to it.
void F1Ticker::fetchTimes(Session &session, int count) {
  f1At("fetch times");
  if (!session.id || !session.eventId) return;
  bool race = session.tag == 'R' || session.tag == 'S';
  long ms[ROWS];
  bool failed = false;  // a request did not get through: not the same as an answer without a time
  char leader[12] = "";  // the first place's own text, "1:35.130"
  for (int i = 0; i < ROWS; i++) ms[i] = -1;
  for (int i = 0; i < count && i < ROWS; i++) {
    if (!session.rows[i].athleteId) continue;
    char url[200];
    snprintf(url, sizeof(url),
             "http://sports.core.api.espn.com/v2/sports/racing/leagues/f1/events/%lu/competitions/%lu/competitors/%lu/statistics/0",
             (unsigned long)session.eventId, (unsigned long)session.id, (unsigned long)session.rows[i].athleteId);
    TimesScan scan;
    if (!getScan(url, TimesScan::leaf, &scan)) { failed = true; continue; }
    scan.flush();
    if (scan.have) {
      ms[i] = parseTimeMs(scan.total);
      if (i == 0) strlcpy(leader, scan.total, sizeof(leader));
    }
    if (race && !session.live && (ms[i] < 0 || (ms[0] >= 0 && ms[i] < ms[0]))) {
      snprintf(url, sizeof(url),
               "http://sports.core.api.espn.com/v2/sports/racing/leagues/f1/events/%lu/competitions/%lu/competitors/%lu/status",
               (unsigned long)session.eventId, (unsigned long)session.id, (unsigned long)session.rows[i].athleteId);
      FinishScan finish;
      if (!getScan(url, FinishScan::leaf, &finish)) { failed = true; continue; }
      Row &row = session.rows[i];
      if (!strcmp(finish.status, "STATUS_RETIRED")) strlcpy(row.time, "DNF", sizeof(row.time));
      else if (!strcmp(finish.status, "STATUS_CLASSIFIED")) {
        int lapsBehind = session.period - finish.laps;
        if (finish.laps >= 0 && lapsBehind > 0) snprintf(row.time, sizeof(row.time), "+%dL", lapsBehind % 100);
      }
    }
  }

  session.timeFails = failed ? 1 : 0;
  if (!failed && ms[0] >= 0) _renewed.times = ultime::now(), _renewed.timesFrom = "ESPN";
  if (ms[0] >= 0) {
    strlcpy(session.rows[0].time, race ? (session.live ? "LEAD" : "WINNER") : leader, sizeof(session.rows[0].time));
    for (int i = 1; i < count && i < ROWS; i++) {
      Row &row = session.rows[i];
      if (ms[i] >= ms[0]) formatGap(row.time, sizeof(row.time), ms[i] - ms[0]);
    }
  }
  f1log("[F1] times %s: leader %ld ms\n", session.name, ms[0]);
}

// OpenF1's session result: per driver the car number and the lap time(s): three entries (Q1, Q2, Q3) in qualifying,
// one number otherwise. The best is the last that is not null.
struct ResultScan {
  static const int N = F1Ticker::ROWS;
  int count = 0;
  int number[N];
  long best[N];
  ResultScan() { for (int i = 0; i < N; i++) number[i] = 0, best[i] = -1; }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool text) {
    ResultScan &r = *(ResultScan *)sink;
    if (n < 2 || n > 3) return;
    int i = atoi(p[0]);
    if (i < 0 || i >= N) return;
    if (n == 2 && !strcmp(p[1], "driver_number")) {
      r.number[i] = atoi(v);
      if (i + 1 > r.count) r.count = i + 1;
    } else if (!strcmp(p[1], "duration") && !text && strcmp(v, "null")) {
      r.best[i] = lroundf(atof(v) * 1000);
    }
  }
};

struct NumbersScan {
  static const int N = 32;
  int count = 0;
  int number[N];
  char code[N][4];
  NumbersScan() { for (int i = 0; i < N; i++) number[i] = 0, code[i][0] = 0; }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    NumbersScan &r = *(NumbersScan *)sink;
    if (n != 2) return;
    int i = atoi(p[0]);
    if (i < 0 || i >= N) return;
    if (!strcmp(p[1], "driver_number")) {
      r.number[i] = atoi(v);
      if (i + 1 > r.count) r.count = i + 1;
    } else if (!strcmp(p[1], "name_acronym")) {
      strlcpy(r.code[i], v, sizeof(r.code[i]));
    }
  }
};

struct OpenSessionScan {
  uint32_t key = 0;
  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    OpenSessionScan &s = *(OpenSessionScan *)sink;
    if (!s.key && n == 2 && !strcmp(p[0], "0") && !strcmp(p[1], "session_key")) s.key = strtoul(v, nullptr, 10);
  }
};

// The car numbers come from OpenF1 and are kept in flash: asked for again each race weekend (a reserve driver can stand in)
// and for a number that is not known yet
const char *F1Ticker::carCode(int number) {
  for (int i = 0; i < _carCount; i++)
    if (_cars[i].number == number) return _cars[i].code;
  return nullptr;
}

bool F1Ticker::fetchCarNumbers(uint32_t sessionKey) {
  char url[96];
  snprintf(url, sizeof(url), "https://api.openf1.org/v1/drivers?session_key=%lu", (unsigned long)sessionKey);
  NumbersScan scan;
  if (!getScan(url, NumbersScan::leaf, &scan) || !scan.count) return false;
  for (int i = 0; i < scan.count; i++) {
    if (!scan.number[i] || !scan.code[i][0]) continue;
    CarNumber *car = nullptr;
    for (int k = 0; k < _carCount && !car; k++)
      if (_cars[k].number == scan.number[i]) car = &_cars[k];
    if (!car && _carCount < CARS) car = &_cars[_carCount++];
    if (!car) continue;
    car->number = scan.number[i];
    strlcpy(car->code, scan.code[i], sizeof(car->code));
  }
  Preferences prefs;
  if (prefs.begin("um-f1cache", false)) {
    prefs.putBytes("cars", _cars, sizeof(_cars));
    prefs.putUChar("carN", _carCount);
    prefs.putUInt("carEvent", _carEvent);
    prefs.end();
  }
  f1log("[F1] %d car numbers from OpenF1\n", _carCount);
  return true;
}

// The times of a finished practice or qualifying session from OpenF1: the session, its result and, once, the
// drivers' car numbers. False when it has nothing for this session (yet) or a request fails: ESPN then goes driver by driver.
bool F1Ticker::fetchOpenF1(Session &session) {
  f1At("fetch OpenF1");
  if (!session.id || !session.start) return false;
  if (_openId != session.id) {
    struct tm lo, hi;
    time_t from = session.start - 40 * 60, to = session.start + 40 * 60;
    gmtime_r(&from, &lo);
    gmtime_r(&to, &hi);
    char url[160];
    snprintf(url, sizeof(url), "https://api.openf1.org/v1/sessions?date_start>=%04d-%02d-%02dT%02d:%02d:%02d&date_start<=%04d-%02d-%02dT%02d:%02d:%02d",
             lo.tm_year + 1900, lo.tm_mon + 1, lo.tm_mday, lo.tm_hour, lo.tm_min, lo.tm_sec,
             hi.tm_year + 1900, hi.tm_mon + 1, hi.tm_mday, hi.tm_hour, hi.tm_min, hi.tm_sec);
    OpenSessionScan found;
    if (!getScan(url, OpenSessionScan::leaf, &found)) return false;
    if (!found.key) return false;
    _openId = session.id;
    _openKey = found.key;
  }
  char url[96];
  snprintf(url, sizeof(url), "https://api.openf1.org/v1/session_result?session_key=%lu", (unsigned long)_openKey);
  ResultScan result;
  if (!getScan(url, ResultScan::leaf, &result) || !result.count) return false;
  if (_carEvent != session.eventId) _carCount = 0;  // a new race weekend: this weekend's drivers, a reserve can stand in
  bool known = true;
  for (int i = 0; i < result.count && known; i++) known = !result.number[i] || carCode(result.number[i]);
  if (!known) {
    _carEvent = session.eventId;  // saved with the numbers
    if (!fetchCarNumbers(_openKey)) return false;
  }

  long ms[ROWS];
  for (int i = 0; i < ROWS; i++) ms[i] = -1;
  for (int r = 0; r < result.count; r++) {
    const char *code = carCode(result.number[r]);
    if (!code || result.best[r] < 0) continue;
    for (int i = 0; i < ROWS; i++)
      if (!strcmp(session.rows[i].code, code)) ms[i] = result.best[r];
  }
  if (ms[0] < 0) return false;  // ESPN's order and OpenF1's do not agree on who is first: not usable
  snprintf(session.rows[0].time, sizeof(session.rows[0].time), "%u:%02u.%03u", (unsigned)(ms[0] / 60000 % 10), (unsigned)(ms[0] / 1000 % 60), (unsigned)(ms[0] % 1000));
  for (int i = 1; i < ROWS; i++)
    if (ms[i] >= ms[0]) formatGap(session.rows[i].time, sizeof(session.rows[i].time), ms[i] - ms[0]);
  session.timeFails = 0;
  _renewed.times = ultime::now(), _renewed.timesFrom = "OpenF1";
  f1log("[F1] times %s from OpenF1: leader %ld ms\n", session.name, ms[0]);
  return true;
}

// The results of the last race (or sprint) from Jolpica: the driver whose fastest lap has rank 1
struct FastestScan {
  const char *list;  // "Results" or "SprintResults"
  char date[12] = "", code[4] = "", rank[4] = "", fastest[4] = "";
  int index = -1;

  void flush() {
    if (index >= 0 && !strcmp(rank, "1")) strlcpy(fastest, code, sizeof(fastest));
    index = -1;
    code[0] = rank[0] = 0;
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    FastestScan &s = *(FastestScan *)sink;
    if (n < 5 || strcmp(p[0], "MRData") || strcmp(p[1], "RaceTable") || strcmp(p[2], "Races") || strcmp(p[3], "0")) return;
    if (n == 5 && !strcmp(p[4], "date")) strlcpy(s.date, v, sizeof(s.date));
    if (n != 8 || strcmp(p[4], s.list)) return;
    bool isCode = !strcmp(p[6], "Driver") && !strcmp(p[7], "code");
    bool isRank = !strcmp(p[6], "FastestLap") && !strcmp(p[7], "rank");
    if (!isCode && !isRank) return;
    int k = atoi(p[5]);
    if (k != s.index) {
      s.flush();
      s.index = k;
    }
    if (isCode) strlcpy(s.code, v, sizeof(s.code));
    else strlcpy(s.rank, v, sizeof(s.rank));
  }
};

bool F1Ticker::fetchFastest(Session &session) {
  f1At("fetch fastest");
  FastestScan scan;
  scan.list = session.tag == 'S' ? "SprintResults" : "Results";
  const char *url = session.tag == 'S' ? "https://api.jolpi.ca/ergast/f1/current/last/sprint.json?limit=30"
                                       : "https://api.jolpi.ca/ergast/f1/current/last/results.json?limit=30";
  if (!getScan(url, FastestScan::leaf, &scan)) return false;
  scan.flush();
  char start[24];
  snprintf(start, sizeof(start), "%sT00:00", scan.date);
  time_t day = parseEspnDate(start);
  if (!scan.fastest[0] || !day || labs((long)(session.start - day)) > 2 * 24 * 3600L) return false;  // the results of another race
  for (Row &row : session.rows)
    if (!strcmp(row.code, scan.fastest)) {
      row.fastest = true;
      f1log("[F1] fastest lap: %s\n", row.code);
      return true;
    }
  return false;
}

// The circuit's city as the 3x5 font can show it: ASCII capitals ("Sao paulo" with an accent too)
static void cityName(char *to, size_t size, const char *city) {
  static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTSAAAAAAACEEEEIIIIDNOOOOO/OUUUUYTY";  // U+00C0..U+00FF
  size_t n = 0;
  for (const uint8_t *p = (const uint8_t *)city; *p && n + 1 < size; p++) {
    if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
      to[n++] = LATIN1[p[1] - 0x80];
      p++;
    } else if (*p < 0x80) {
      to[n++] = toupper(*p);
    }
  }
  to[n] = 0;
}

// Stream the calendar and keep the nearest weekend whose first practice has not started.
// Jolpica's "next" race can still be the current weekend until its results are published.
struct SeasonScan {
  char name[32] = "", date[16] = "", time[16] = "", circuit[48] = "", locality[32] = "";
  int index = -1;
  time_t nowUtc = 0;
  F1Ticker::Race upcoming;

  void flush() {
    char start[48];
    snprintf(start, sizeof(start), "%sT%s", date, time);
    time_t when = parseEspnDate(start);
    if (name[0] && date[0] && time[0] && when > nowUtc && (!upcoming.start || when < upcoming.start)) {
      String city = name;
      city.replace(" Grand Prix", "");
      city.replace(" City", "");
      cityName(upcoming.city, sizeof(upcoming.city), city.c_str());
      cityName(upcoming.place, sizeof(upcoming.place), locality);
      strlcpy(upcoming.circuit, circuit, sizeof(upcoming.circuit));
      upcoming.start = when;
    }
    name[0] = date[0] = time[0] = circuit[0] = locality[0] = 0;
    index = -1;
  }

  static void leaf(void *sink, const char (*p)[JsonScan::KEY], int n, const char *v, bool) {
    SeasonScan &s = *(SeasonScan *)sink;
    if (n < 5 || strcmp(p[0], "MRData") || strcmp(p[1], "RaceTable") || strcmp(p[2], "Races")) return;
    int index = atoi(p[3]);
    if (index != s.index) {
      s.flush();
      s.index = index;
    }
    if (jsonPathIs(p, n, "MRData.RaceTable.Races.*.raceName")) strlcpy(s.name, v, sizeof(s.name));
    else if (jsonPathIs(p, n, "MRData.RaceTable.Races.*.FirstPractice.date")) strlcpy(s.date, v, sizeof(s.date));
    else if (jsonPathIs(p, n, "MRData.RaceTable.Races.*.FirstPractice.time")) strlcpy(s.time, v, sizeof(s.time));
    else if (jsonPathIs(p, n, "MRData.RaceTable.Races.*.Circuit.circuitName") || jsonPathIs(p, n, "MRData.RaceTable.Races.*.Circuit.Location.locality")) {
      if (s.circuit[0]) strlcat(s.circuit, " ", sizeof(s.circuit));
      strlcat(s.circuit, v, sizeof(s.circuit));
      if (p[n - 1][0] == 'l') strlcpy(s.locality, v, sizeof(s.locality));  // "locality"
    }
  }
};

unsigned long F1Ticker::fetchSeason() {
  f1At("fetch season");
  time_t now = ultime::now();
  struct tm today;
  gmtime_r(&now, &today);
  SeasonScan scan;
  scan.nowUtc = now;
  for (int year = today.tm_year + 1900; year <= today.tm_year + 1901; year++) {
    char url[80];
    snprintf(url, sizeof(url), "https://api.jolpi.ca/ergast/f1/%d.json?limit=100", year);
    if (!getScan(url, SeasonScan::leaf, &scan)) return RETRY_MS;
    scan.flush();
    if (scan.upcoming.start) break;
  }
  Race upcoming = scan.upcoming;
  f1log("[F1] next weekend: %s, first practice UTC %ld\n", upcoming.city, (long)upcoming.start);
  if (!upcoming.city[0] || !upcoming.start) return RETRY_MS;
  _renewed.season = ultime::now();

  {
    std::lock_guard<std::mutex> guard(_lock);
    _snap.upcoming[0] = upcoming;
    _snap.upcomingCount = 1;
    _version++;
  }
  Preferences prefs;
  if (prefs.begin("um-f1cache", false)) {
    prefs.putBytes("race", &upcoming, sizeof(upcoming));
    prefs.putUInt("raceAt", ultime::now());
    prefs.end();
  }
  return SEASON_REFRESH_MS;
}

bool F1Ticker::getScan(const char *url, JsonScan::Leaf leaf, void *sink) {
  bool secure = strncmp(url, "https", 5) == 0;
  std::lock_guard<std::mutex> download(f1TlsLock);
  if (firmwareUpdating) return false;
  WiFiClient plain;
  WiFiClientSecure tls;
  if (secure) tls.setInsecure();  // public data, nothing to protect
  WiFiClient &client = secure ? (WiFiClient &)tls : plain;

  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  uint32_t failures = memoryAllocationFailures.load(std::memory_order_relaxed);
  int code = http.GET();
  if (code < 0 && memoryAllocationFailures.load(std::memory_order_relaxed) != failures) {
    f1log("[F1] allocation failed: trying again after the memory guard\n");
    http.end();
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!http.begin(client, url)) return false;
    code = http.GET();
  }
  if (code != HTTP_CODE_OK) {
    f1log("[F1] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }

  YieldingReader reader(http.getStream(), 10000);
  bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  if (!ok) f1log("[F1] %s: cut off or not JSON after %u bytes (heap free %u, largest block %u)\n", url, (unsigned)reader.count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return ok;
}
