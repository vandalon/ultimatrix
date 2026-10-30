// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NflTicker.h"

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

namespace nfl {
namespace {

constexpr uint32_t kStackBytes = 8192, kMinFreeBlock = 10000;
constexpr unsigned long kLiveMs = 15000, kIdleMs = 5UL * 60000, kRetryMs = 60000, kSlowMs = 30UL * 60000;
constexpr char kScoreboard[] = "http://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard";  // the whole week in one answer (about 250 KB: streamed)
constexpr char kStandings[] = "http://site.api.espn.com/apis/v2/sports/football/nfl/standings";

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
  char abbr[5] = "", record[8] = "", id[8] = "";
  uint16_t score = 0;
  uint8_t points[kPeriods] = {}, periods = 0;
  char color[8] = "", alt[8] = "";
};

// events.N.competitions.0.competitors.M.<...>: the scoreboard of a week
struct WeekScan {
  Game games[kMaxGames];
  int count = 0;
  Side sides[2];
  Game *event = nullptr;
  char statusName[28] = "", possessionId[8] = "";
  uint8_t down = 0, distance = 0, awayTimeouts = 3, homeTimeouts = 3;
  bool redZone = false;
  char spot[8] = "";
  int weekNumber = 0, seasonType = 0;

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
        event->homeScore = s.score; event->homeColor = colour; memcpy(event->homePoints, s.points, sizeof(s.points));
      } else {
        strlcpy(event->away, s.abbr, sizeof(event->away)); strlcpy(event->awayRecord, s.record, sizeof(event->awayRecord));
        event->awayScore = s.score; event->awayColor = colour; memcpy(event->awayPoints, s.points, sizeof(s.points));
      }
      event->periods = std::max(event->periods, s.periods);
    }
    event->halftime = strstr(statusName, "HALFTIME") != nullptr;
    event->delayed = strstr(statusName, "DELAY") || strstr(statusName, "SUSPEND");
    if (event->state == Live) {
      event->down = std::min<int>(down, 4); event->distance = std::min<int>(distance, 99); event->redZone = redZone;
      event->awayTimeouts = std::min<int>(awayTimeouts, 3); event->homeTimeouts = std::min<int>(homeTimeouts, 3);
      strlcpy(event->spot, spot, sizeof(event->spot));
      for (const Side &s : sides)
        if (possessionId[0] && s.id[0] && !strcmp(s.id, possessionId)) event->possession = s.home ? 2 : 1;
      // "GB 25": on the named side's own half, that many yards from its own goal line
      char team[5] = "";
      int yard = 0;
      if (sscanf(spot, "%4s %d", team, &yard) == 2 && yard >= 0 && yard <= 50) {
        if (!strcmp(team, event->away)) event->fieldPos = yard;
        else if (!strcmp(team, event->home)) event->fieldPos = 100 - yard;
      } else if (!strncmp(spot, "50", 2)) {
        event->fieldPos = 50;
      }
    }
    if (strstr(statusName, "POSTPONED") || strstr(statusName, "CANCEL")) event->away[0] = 0;  // not played: dropped with the games that have no two teams
    event = nullptr;
  }

  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    WeekScan &s = *static_cast<WeekScan *>(sink);
    if (n == 2 && !strcmp(path[0], "week") && !strcmp(path[1], "number")) s.weekNumber = atoi(value);
    if (n == 2 && !strcmp(path[0], "season") && !strcmp(path[1], "type")) s.seasonType = atoi(value);
    if (n < 3 || strcmp(path[0], "events")) return;
    const int index = atoi(path[1]);
    if (index < 0 || index >= kMaxGames) return;
    if (s.event != &s.games[index]) {
      s.endEvent();
      s.event = &s.games[index];
      s.sides[0] = Side();
      s.sides[1] = Side();
      s.statusName[0] = 0;
      s.possessionId[0] = 0;
      s.spot[0] = 0;
      s.down = s.distance = 0;
      s.redZone = false;
      s.awayTimeouts = s.homeTimeouts = 3;
      s.count = index + 1;
    }
    Game &g = *s.event;
    if (jsonPathIs(path, n, "events.*.date")) {
      g.start = parseEspnDate(value);
    } else if (jsonPathIs(path, n, "events.*.status.period")) {
      g.quarter = std::min(atoi(value), 5);
    } else if (jsonPathIs(path, n, "events.*.status.displayClock")) {
      strlcpy(g.clock, value, sizeof(g.clock));
    } else if (jsonPathIs(path, n, "events.*.status.type.state")) {
      g.state = !strcmp(value, "in") ? Live : !strcmp(value, "post") ? Final : Pre;
    } else if (jsonPathIs(path, n, "events.*.status.type.name")) {
      strlcpy(s.statusName, value, sizeof(s.statusName));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.id")) {
      strlcpy(s.sides[competitor(path)].id, value, sizeof(s.sides[0].id));
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
      if (q >= 0 && q < kPeriods) { Side &side = s.sides[competitor(path)]; side.points[q] = std::min(atoi(value), 255); side.periods = std::max<int>(side.periods, q + 1); }
    } else if (n >= 6 && !strcmp(path[2], "competitions") && !strcmp(path[4], "situation")) {
      const char *key = path[5];
      if (n == 6 && !strcmp(key, "down")) s.down = atoi(value);
      else if (n == 6 && !strcmp(key, "distance")) s.distance = atoi(value);
      else if (n == 6 && !strcmp(key, "isRedZone")) s.redZone = !strcmp(value, "true");
      else if (n == 6 && !strcmp(key, "homeTimeouts")) s.homeTimeouts = atoi(value);
      else if (n == 6 && !strcmp(key, "awayTimeouts")) s.awayTimeouts = atoi(value);
      else if (n == 6 && !strcmp(key, "possessionText")) strlcpy(s.spot, value, sizeof(s.spot));
      else if (!strcmp(key, "possession")) {  // the team id (a number), or an object whose reference ends in it
        const char *slash = strrchr(value, '/');
        strlcpy(s.possessionId, slash ? slash + 1 : value, sizeof(s.possessionId));
        char *query = strchr(s.possessionId, '?');
        if (query) *query = 0;
      }
    }
  }
};

// children.C.standings.entries.E.<...>: the two conferences
struct TableScan {
  Table tables[2];
  uint8_t slot[4] = {0, 1, 0, 1};  // children index -> AFC (0) or NFC (1), from its abbreviation when it comes
  char stat[16] = "";

  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    TableScan &s = *static_cast<TableScan *>(sink);
    if (n < 3 || strcmp(path[0], "children")) return;
    const int child = atoi(path[1]);
    if (child < 0 || child > 3) return;
    if (n == 3 && !strcmp(path[2], "abbreviation")) { s.slot[child] = !strcmp(value, "NFC"); return; }
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
        int w, l, t = 0;
        if (!strcmp(s.stat, "overall") && sscanf(value, "%d-%d-%d", &w, &l, &t) >= 2) { row.wins = std::min(w, 25); row.losses = std::min(l, 25); row.ties = std::min(t, 9); }
        else if (!strcmp(s.stat, "streak")) strlcpy(row.streak, value, sizeof(row.streak));
        else if (!strcmp(s.stat, "playoffSeed")) row.seed = std::min(atoi(value), 99);
      }
    }
  }
};

void sortTable(Table &t) {
  std::stable_sort(t.rows, t.rows + t.count, [](const Standing &a, const Standing &b) {
    if (a.seed && b.seed && a.seed != b.seed) return a.seed < b.seed;  // ESPN's own order: the division winners and wild cards first
    const int ga = a.wins + a.losses + a.ties, gb = b.wins + b.losses + b.ties;
    const int pa = (a.wins * 2 + a.ties) * gb, pb = (b.wins * 2 + b.ties) * ga;  // win percentage (a tie is half a win), compared without dividing
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
  if (createFeedTask(task, "nfl", kStackBytes, this, 1, &_task, 0) != pdPASS) _task = nullptr;
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
    consoleLog("[NFL] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }
  YieldingReader reader(http.getStream(), 10000);
  bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  if (!ok) consoleLog("[NFL] %s: %s after %u bytes\n", url, reader.dry ? "the connection stopped sending" : "not valid JSON", (unsigned)reader.count);
  return ok;
}

// The week: its games (live, finished, coming) and, of those that have not begun, the coming ones as fixtures
bool Ticker::refreshWeek() {
  WeekScan *scan = new WeekScan;
  bool ok = get(kScoreboard, WeekScan::leaf, scan);
  scan->endEvent();
  // The week is over (a Tuesday): ESPN keeps showing it until Wednesday, so the coming games are those of the next week
  WeekScan *later = nullptr;
  if (ok) {
    const time_t now = time(nullptr);
    bool coming = false;
    for (int i = 0; i < scan->count; i++) coming |= scan->games[i].state == Pre && scan->games[i].start > now;
    if (!coming && scan->seasonType == 2 && scan->weekNumber > 0 && scan->weekNumber < 18) {
      char url[120];
      snprintf(url, sizeof(url), "%s?week=%d&seasontype=2", kScoreboard, scan->weekNumber + 1);
      later = new WeekScan;
      if (get(url, WeekScan::leaf, later)) later->endEvent();
      else { delete later; later = nullptr; }
    }
  }
  if (ok) {
    const time_t now = time(nullptr);
    std::lock_guard<std::mutex> guard(_lock);
    _data.count = 0;
    _data.live = 0;
    _data.nextCount = 0;
    for (int i = 0; i < scan->count; i++) {
      const Game &g = scan->games[i];
      if (!g.home[0] || !g.away[0]) continue;  // an event without two teams
      _data.games[_data.count++] = g;
      _data.live += g.state == Live;
    }
    // the coming games in the order of the kick-off
    auto addFixtures = [&](const Game *games, int count) {
      for (int i = 0; i < count; i++) {
        const Game &g = games[i];
        if (g.state != Pre || g.start <= now || !g.home[0] || !g.away[0] || _data.nextCount >= kMaxFixtures) continue;
        Fixture &f = _data.next[_data.nextCount++];
        strlcpy(f.away, g.away, sizeof(f.away));
        strlcpy(f.home, g.home, sizeof(f.home));
        f.start = (uint32_t)g.start;
      }
    };
    addFixtures(_data.games, _data.count);
    if (later) addFixtures(later->games, later->count);
    std::stable_sort(_data.next, _data.next + _data.nextCount, [](const Fixture &a, const Fixture &b) { return a.start < b.start; });
    _data.version = ++_version;
  }
  delete later;
  delete scan;
  liveEventOn = ok && _data.live > 0;
  return ok;
}

bool Ticker::refreshStandings() {
  TableScan *scan = new TableScan;
  bool ok = get(kStandings, TableScan::leaf, scan);
  if (ok) {
    sortTable(scan->tables[0]);
    sortTable(scan->tables[1]);
    std::lock_guard<std::mutex> guard(_lock);
    _data.afc = scan->tables[0];
    _data.nfc = scan->tables[1];
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
      consoleLog("[NFL] heap too low, skipping a refresh\n");
      vTaskDelay(pdMS_TO_TICKS(kRetryMs));
      continue;
    }
    try {
      if (due(ticker->_weekAt)) {
        const bool ok = ticker->refreshWeek();
        ticker->_ok = ok;
        ticker->_weekAt = millis() + (ok ? (ticker->_data.live ? kLiveMs : kIdleMs) : kRetryMs);
      } else if (ticker->_ok && due(ticker->_standingsAt)) {  // one slow read per round, so the scores are never kept waiting
        ticker->_standingsAt = millis() + (ticker->refreshStandings() ? kSlowMs : kRetryMs);
      }
    } catch (const std::exception &e) {  // std::bad_alloc: keep the last data
      consoleLog("[NFL] out of memory in a refresh (%s)\n", e.what());
      ticker->_weekAt = millis() + kRetryMs;
    }
    for (int i = 0; i < 5 && !feedsHalted(); i++) vTaskDelay(pdMS_TO_TICKS(1000));  // in slices, so an update is seen within a second
  }
}

}  // namespace nfl
