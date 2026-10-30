// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NbaTicker.h"

#include <Console.h>
#include <EspnFeed.h>
#include <HTTPClient.h>
#include <NetworkBudget.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

extern std::atomic<uint32_t> memoryAllocationFailures;  // main.cpp

namespace nba {
namespace {

constexpr uint32_t kStackBytes = 8192, kMinFreeBlock = 10000;
constexpr unsigned long kLiveMs = 20000, kIdleMs = 5UL * 60000, kRetryMs = 60000, kSlowMs = 30UL * 60000;
constexpr int kFixtureDays = 7;
constexpr char kScoreboard[] = "http://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard";
constexpr char kStandings[] = "http://site.api.espn.com/apis/v2/sports/basketball/nba/standings";

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

bool hexColour(const char *text, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (strlen(text) != 6) return false;
  unsigned v = 0;
  for (int i = 0; i < 6; i++) {
    char c = text[i];
    unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    if (d > 15) return false;
    v = v << 4 | d;
  }
  r = v >> 16; g = v >> 8; b = v;
  return true;
}

int luma(uint8_t r, uint8_t g, uint8_t b) { return (r * 3 + g * 6 + b) / 10; }

// What one competitor of an event carries; the team colour is only kept when it can be seen on black
struct Side {
  bool home = false;
  char abbr[5] = "", record[8] = "";
  uint16_t score = 0;
  uint8_t quarters[kPeriods] = {};
  char color[8] = "", alt[8] = "";
};

// events.N.competitions.0.competitors.M.<...>: the scoreboard of a day
struct DayScan {
  Game games[kMaxGames];
  int count = 0;
  Side sides[2];
  Game *event = nullptr;
  char statusName[28] = "";

  static int competitor(const char (*path)[JsonScan::KEY]) { return atoi(path[5]) & 1; }

  void endEvent() {
    if (!event) return;
    for (const Side &s : sides) {
      uint8_t r, g, b;
      uint16_t colour = 0xFFFF;
      if (hexColour(s.color, r, g, b) && luma(r, g, b) >= 70) colour = rgb565(r, g, b);
      else if (hexColour(s.alt, r, g, b) && luma(r, g, b) >= 70) colour = rgb565(r, g, b);
      if (s.home) {
        strlcpy(event->home, s.abbr, sizeof(event->home)); strlcpy(event->homeRecord, s.record, sizeof(event->homeRecord));
        event->homeScore = s.score; event->homeColor = colour; memcpy(event->homeQ, s.quarters, sizeof(s.quarters));
      } else {
        strlcpy(event->away, s.abbr, sizeof(event->away)); strlcpy(event->awayRecord, s.record, sizeof(event->awayRecord));
        event->awayScore = s.score; event->awayColor = colour; memcpy(event->awayQ, s.quarters, sizeof(s.quarters));
      }
    }
    event->halftime = !strcmp(statusName, "STATUS_HALFTIME");
    event->endOfPeriod = !strcmp(statusName, "STATUS_END_PERIOD");
    event = nullptr;
  }

  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    DayScan &s = *static_cast<DayScan *>(sink);
    if (n < 3 || strcmp(path[0], "events")) return;
    const int index = atoi(path[1]);
    if (index < 0 || index >= kMaxGames) return;
    if (s.event != &s.games[index]) {
      s.endEvent();
      s.event = &s.games[index];
      s.sides[0] = Side();
      s.sides[1] = Side();
      s.statusName[0] = 0;
      s.count = index + 1;
    }
    Game &g = *s.event;
    if (jsonPathIs(path, n, "events.*.date")) {
      g.start = parseEspnDate(value);
    } else if (jsonPathIs(path, n, "events.*.status.period")) {
      g.period = std::min(atoi(value), 15);
    } else if (jsonPathIs(path, n, "events.*.status.displayClock")) {
      strlcpy(g.clock, value, sizeof(g.clock));
    } else if (jsonPathIs(path, n, "events.*.status.type.state")) {
      g.state = !strcmp(value, "in") ? Live : !strcmp(value, "post") ? Final : Pre;
    } else if (jsonPathIs(path, n, "events.*.status.type.name")) {
      strlcpy(s.statusName, value, sizeof(s.statusName));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.homeAway")) {
      s.sides[competitor(path)].home = !strcmp(value, "home");
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.score")) {
      s.sides[competitor(path)].score = std::min(atoi(value), 999);
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.team.abbreviation")) {
      strlcpy(s.sides[competitor(path)].abbr, value, sizeof(s.sides[0].abbr));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.team.color")) {
      strlcpy(s.sides[competitor(path)].color, value, sizeof(s.sides[0].color));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.team.alternateColor")) {
      strlcpy(s.sides[competitor(path)].alt, value, sizeof(s.sides[0].alt));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.records.0.summary")) {
      strlcpy(s.sides[competitor(path)].record, value, sizeof(s.sides[0].record));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.linescores.*.value")) {
      const int q = atoi(path[7]);
      if (q >= 0 && q < kPeriods) s.sides[competitor(path)].quarters[q] = std::min(atoi(value), 255);
    }
  }
};

// children.C.standings.entries.E.<...>: the two conferences
struct TableScan {
  Table tables[2];
  uint8_t slot[4] = {0, 1, 0, 1};  // children index -> East (0) or West (1), from its abbreviation when it comes
  char stat[16] = "";

  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    TableScan &s = *static_cast<TableScan *>(sink);
    if (n < 3 || strcmp(path[0], "children")) return;
    const int child = atoi(path[1]);
    if (child < 0 || child > 3) return;
    if (n == 3 && !strcmp(path[2], "abbreviation")) { s.slot[child] = !strcmp(value, "West"); return; }
    if (n < 7 || strcmp(path[2], "standings") || strcmp(path[3], "entries")) return;
    const int e = atoi(path[4]);
    if (e < 0 || e >= kTeams) return;
    Table &table = s.tables[s.slot[child]];
    Standing &row = table.rows[e];
    table.count = std::max<int>(table.count, e + 1);
    if (n == 7 && !strcmp(path[5], "team") && !strcmp(path[6], "abbreviation")) {
      strlcpy(row.abbr, value, sizeof(row.abbr));
    } else if (n == 8 && !strcmp(path[5], "stats")) {
      if (!strcmp(path[7], "name")) {
        strlcpy(s.stat, value, sizeof(s.stat));
      } else if (!strcmp(path[7], "displayValue")) {
        int w, l;
        if (!strcmp(s.stat, "overall") && sscanf(value, "%d-%d", &w, &l) == 2) { row.wins = std::min(w, 82); row.losses = std::min(l, 82); }
        else if (!strcmp(s.stat, "streak")) strlcpy(row.streak, value, sizeof(row.streak));
      }
    }
  }
};

void sortTable(Table &t) {
  std::stable_sort(t.rows, t.rows + t.count, [](const Standing &a, const Standing &b) {
    const int pa = a.wins * (b.wins + b.losses), pb = b.wins * (a.wins + a.losses);  // win percentage, compared without dividing
    return pa != pb ? pa > pb : a.wins > b.wins;
  });
}

bool due(unsigned long at) { return (long)(millis() - at) >= 0; }

}  // namespace

void Ticker::begin() {
  if (_started) return;
  _started = true;
  resume();
}

void Ticker::resume() {
  if (!_started || _task.load() || feedsHalted()) return;
  if (createFeedTask(task, "nba", kStackBytes, this, 1, &_task, 0) != pdPASS) _task = nullptr;
}

void Ticker::snapshot(Snapshot &out) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _data;
}

bool Ticker::get(const char *url, JsonScan::Leaf leaf, void *sink) {
  std::lock_guard<std::mutex> download(networkBudget());
  if (firmwareUpdating) return false;
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    consoleLog("[NBA] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }
  YieldingReader reader(http.getStream(), 10000);
  bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  if (!ok) consoleLog("[NBA] %s: %s after %u bytes\n", url, reader.dry ? "the connection stopped sending" : "not valid JSON", (unsigned)reader.count);
  return ok;
}

bool Ticker::refreshToday() {
  DayScan *scan = new DayScan;
  bool ok = get(kScoreboard, DayScan::leaf, scan);
  scan->endEvent();
  if (ok) {
    std::lock_guard<std::mutex> guard(_lock);
    _data.count = 0;
    _data.live = 0;
    for (int i = 0; i < scan->count; i++) {
      const Game &g = scan->games[i];
      if (!g.home[0] || !g.away[0]) continue;  // an event without two teams
      _data.games[_data.count++] = g;
      _data.live += g.state == Live;
    }
    _data.version = ++_version;
  }
  delete scan;
  liveEventOn = ok && _data.live > 0;
  return ok;
}

// The games of the coming days, one request per day (a range of dates is not answered for this sport)
bool Ticker::refreshFixtures() {
  Fixture found[kMaxFixtures];
  int count = 0;
  const time_t now = time(nullptr);
  for (int day = 1; day <= kFixtureDays; day++) {
    struct tm date;
    const time_t eastern = now - 5 * 3600 + day * 86400L;  // ESPN's days are American ones
    gmtime_r(&eastern, &date);
    char url[160];
    snprintf(url, sizeof(url), "%s?dates=%04d%02d%02d", kScoreboard, date.tm_year + 1900, date.tm_mon + 1, date.tm_mday);
    DayScan *scan = new DayScan;
    bool ok = get(url, DayScan::leaf, scan);
    scan->endEvent();
    if (ok) {
      for (int i = 0; i < scan->count && count < kMaxFixtures; i++) {
        const Game &g = scan->games[i];
        if (g.state != Pre || !g.home[0] || !g.away[0] || g.start <= now) continue;
        strlcpy(found[count].away, g.away, sizeof(found[count].away));
        strlcpy(found[count].home, g.home, sizeof(found[count].home));
        found[count++].start = g.start;
      }
    }
    delete scan;
    if (!ok) return false;
  }
  std::lock_guard<std::mutex> guard(_lock);
  memcpy(_data.next, found, sizeof(found));
  _data.nextCount = count;
  _data.version = ++_version;
  return true;
}

bool Ticker::refreshStandings() {
  TableScan *scan = new TableScan;
  bool ok = get(kStandings, TableScan::leaf, scan);
  if (ok) {
    sortTable(scan->tables[0]);
    sortTable(scan->tables[1]);
    std::lock_guard<std::mutex> guard(_lock);
    _data.east = scan->tables[0];
    _data.west = scan->tables[1];
    _data.version = ++_version;
  }
  delete scan;
  return ok;
}

void Ticker::task(void *self) {
  waitForFeedRegistration();
  Ticker *ticker = static_cast<Ticker *>(self);
  for (;;) {
    if (feedsHalted()) {  // an update has the network and the heap to itself; resume() starts the task again
      ticker->_task = nullptr;
      stopFeedTask();
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinFreeBlock) {
      memoryAllocationFailures.fetch_add(1, std::memory_order_relaxed);  // tells main's guard to shed telnet and mDNS
      consoleLog("[NBA] heap too low, skipping a refresh\n");
      vTaskDelay(pdMS_TO_TICKS(kRetryMs));
      continue;
    }
    try {
      if (due(ticker->_todayAt)) {
        const bool ok = ticker->refreshToday();
        ticker->_ok = ok;
        ticker->_todayAt = millis() + (ok ? (ticker->_data.live ? kLiveMs : kIdleMs) : kRetryMs);
      } else if (ticker->_ok && due(ticker->_fixturesAt)) {  // one slow read per round, so the scores are never kept waiting
        ticker->_fixturesAt = millis() + (ticker->refreshFixtures() ? kSlowMs : kRetryMs);
      } else if (ticker->_ok && due(ticker->_standingsAt)) {
        ticker->_standingsAt = millis() + (ticker->refreshStandings() ? kSlowMs : kRetryMs);
      }
    } catch (const std::exception &e) {  // std::bad_alloc: keep the last data
      consoleLog("[NBA] out of memory in a refresh (%s)\n", e.what());
      ticker->_todayAt = millis() + kRetryMs;
    }
    for (int i = 0; i < 5 && !feedsHalted(); i++) vTaskDelay(pdMS_TO_TICKS(1000));  // in slices, so an update is seen within a second
  }
}

}  // namespace nba
