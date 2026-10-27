// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "MlbTicker.h"

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

namespace mlb {
namespace {

constexpr uint32_t kStackBytes = 8192, kMinFreeBlock = 10000;
constexpr unsigned long kLiveMs = 20000, kIdleMs = 5UL * 60000, kRetryMs = 60000, kSlowMs = 30UL * 60000;
constexpr int kFixtureDays = 4;  // today and the next three days: a day holds up to 15 games
constexpr int kSeriesDays = 8;   // while a playoff series is on: far enough to see the rest of a best-of-seven
constexpr char kScoreboard[] = "http://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard";
constexpr char kStandings[] = "http://site.api.espn.com/apis/v2/sports/baseball/mlb/standings";

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
  uint8_t runs[kInnings] = {}, innings = 0, hits = 0, errors = 0;
  char color[8] = "", alt[8] = "";
};

// events.N.competitions.0.competitors.M.<...>: the scoreboard of a day
struct DayScan {
  Game games[kMaxGames];
  int count = 0;
  Side sides[2];
  Game *event = nullptr;
  char statusName[28] = "", detail[20] = "";  // STATUS_IN_PROGRESS, "Top 7th"
  uint8_t outs = 0, balls = 0, strikes = 0, bases = 0;
  struct Raw { bool playoff = false, done = false; char headline[48] = ""; uint8_t bestOf = 0, wins[2] = {}; char id[2][8] = {"", ""}; } raw;  // the series of the event
  Series series[kMaxGames];
  bool hasSeries[kMaxGames] = {};

  static int competitor(const char (*path)[JsonScan::KEY]) { return atoi(path[5]) & 1; }

  // The playoff series this event belongs to, its two teams in alphabetical order, from the wins ESPN gives per team id
  void makeSeries() {
    const int index = event - games;
    if (!raw.playoff || !raw.bestOf || !sides[0].abbr[0] || !sides[1].abbr[0]) return;
    Series &s = series[index];
    s = Series();
    const int low = strcmp(sides[0].abbr, sides[1].abbr) > 0 ? 1 : 0;
    const Side *two[2] = {&sides[low], &sides[1 - low]};
    uint8_t wins[2];
    uint16_t colour[2];
    for (int k = 0; k < 2; k++) {
      wins[k] = 0;
      for (int r = 0; r < 2; r++) if (two[k]->id[0] && !strcmp(two[k]->id, raw.id[r])) wins[k] = raw.wins[r];
      uint8_t r8, g8, b8;
      colour[k] = 0xFFFF;
      if (hexColour(two[k]->color, r8, g8, b8) && luma(r8, g8, b8) >= 70) colour[k] = rgb565(r8, g8, b8);
      else if (hexColour(two[k]->alt, r8, g8, b8) && luma(r8, g8, b8) >= 70) colour[k] = rgb565(r8, g8, b8);
    }
    strlcpy(s.first, two[0]->abbr, sizeof(s.first)); strlcpy(s.second, two[1]->abbr, sizeof(s.second));
    s.firstColor = colour[0]; s.secondColor = colour[1]; s.firstWins = wins[0]; s.secondWins = wins[1]; s.bestOf = raw.bestOf;
    const char *h = raw.headline;
    const bool american = strstr(h, "American") || strstr(h, "AL "), national = strstr(h, "National") || strstr(h, "NL ");
    if (strstr(h, "World Series")) strcpy(s.round, "WORLD");
    else if (strstr(h, "Wild Card")) strcpy(s.round, "WILD");
    else if (strstr(h, "ALCS")) strcpy(s.round, "ALCS");
    else if (strstr(h, "NLCS")) strcpy(s.round, "NLCS");
    else if (strstr(h, "ALDS")) strcpy(s.round, "ALDS");
    else if (strstr(h, "NLDS")) strcpy(s.round, "NLDS");
    else if (strstr(h, "Championship")) strcpy(s.round, american ? "ALCS" : national ? "NLCS" : "LCS");
    else if (strstr(h, "Division")) strcpy(s.round, american ? "ALDS" : national ? "NLDS" : "DS");
    else strcpy(s.round, "PLAY");
    const char *game = strstr(h, "Game ");
    s.game = raw.done ? 0 : game ? atoi(game + 5) : wins[0] + wins[1] + 1;
    hasSeries[index] = true;
  }

  void endEvent() {
    if (!event) return;
    for (const Side &s : sides) {
      uint8_t r, g, b;
      uint16_t colour = 0xFFFF;
      if (hexColour(s.color, r, g, b) && luma(r, g, b) >= 70) colour = rgb565(r, g, b);
      else if (hexColour(s.alt, r, g, b) && luma(r, g, b) >= 70) colour = rgb565(r, g, b);
      if (s.home) {
        strlcpy(event->home, s.abbr, sizeof(event->home)); strlcpy(event->homeRecord, s.record, sizeof(event->homeRecord));
        event->homeScore = s.score; event->homeColor = colour; memcpy(event->homeRuns, s.runs, sizeof(s.runs));
        event->homeInnings = s.innings; event->homeHits = s.hits; event->homeErrors = s.errors;
      } else {
        strlcpy(event->away, s.abbr, sizeof(event->away)); strlcpy(event->awayRecord, s.record, sizeof(event->awayRecord));
        event->awayScore = s.score; event->awayColor = colour; memcpy(event->awayRuns, s.runs, sizeof(s.runs));
        event->awayInnings = s.innings; event->awayHits = s.hits; event->awayErrors = s.errors;
      }
    }
    // "Top 7th", "Bot 7th": the half on; "Mid 7th" (the top is over) and "End 7th" (the bottom is over): between the halves
    event->bottom = !strncmp(detail, "Bot", 3) || !strncmp(detail, "End", 3);
    event->between = !strncmp(detail, "Mid", 3) || !strncmp(detail, "End", 3);
    makeSeries();
    event->delayed = strstr(statusName, "DELAY") || strstr(statusName, "SUSPEND");
    if (event->state == Live) { event->outs = std::min<int>(outs, 3); event->balls = std::min<int>(balls, 3); event->strikes = std::min<int>(strikes, 2); event->bases = bases; }
    if (strstr(statusName, "POSTPONED") || strstr(statusName, "CANCEL")) event->away[0] = 0;  // not played: dropped with the games that have no two teams
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
      s.detail[0] = 0;
      s.raw = Raw();
      s.hasSeries[index] = false;
      s.outs = s.balls = s.strikes = s.bases = 0;
      s.count = index + 1;
    }
    Game &g = *s.event;
    if (jsonPathIs(path, n, "events.*.date")) {
      g.start = parseEspnDate(value);
    } else if (jsonPathIs(path, n, "events.*.status.period")) {
      g.inning = std::min(atoi(value), 30);
    } else if (jsonPathIs(path, n, "events.*.status.type.state")) {
      g.state = !strcmp(value, "in") ? Live : !strcmp(value, "post") ? Final : Pre;
    } else if (jsonPathIs(path, n, "events.*.status.type.name")) {
      strlcpy(s.statusName, value, sizeof(s.statusName));
    } else if (jsonPathIs(path, n, "events.*.status.type.shortDetail")) {
      strlcpy(s.detail, value, sizeof(s.detail));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.id")) {
      strlcpy(s.sides[competitor(path)].id, value, sizeof(s.sides[0].id));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.series.type")) {
      s.raw.playoff = !strcmp(value, "playoff");
    } else if (jsonPathIs(path, n, "events.*.competitions.0.series.completed")) {
      s.raw.done = !strcmp(value, "true");
    } else if (jsonPathIs(path, n, "events.*.competitions.0.series.totalCompetitions")) {
      s.raw.bestOf = std::min(atoi(value), 9);
    } else if (jsonPathIs(path, n, "events.*.competitions.0.series.competitors.*.id")) {
      strlcpy(s.raw.id[atoi(path[6]) & 1], value, sizeof(s.raw.id[0]));
    } else if (jsonPathIs(path, n, "events.*.competitions.0.series.competitors.*.wins")) {
      s.raw.wins[atoi(path[6]) & 1] = std::min(atoi(value), 9);
    } else if (jsonPathIs(path, n, "events.*.competitions.0.notes.0.headline")) {
      strlcpy(s.raw.headline, value, sizeof(s.raw.headline));
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
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.hits")) {
      s.sides[competitor(path)].hits = std::min(atoi(value), 255);
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.errors")) {
      s.sides[competitor(path)].errors = std::min(atoi(value), 255);
    } else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.linescores.*.value")) {
      const int q = atoi(path[7]);
      if (q >= 0 && q < kInnings) { Side &side = s.sides[competitor(path)]; side.runs[q] = std::min(atoi(value), 255); side.innings = std::max<int>(side.innings, q + 1); }
    } else if (n >= 6 && !strcmp(path[2], "competitions") && !strcmp(path[4], "situation")) {
      // the count is plain numbers; a runner is true/false, or an object (with his player) when the feed gives more
      const char *key = path[5];
      const bool on = n == 6 ? !strcmp(value, "true") : true;
      if (n == 6 && !strcmp(key, "balls")) s.balls = atoi(value);
      else if (n == 6 && !strcmp(key, "strikes")) s.strikes = atoi(value);
      else if (n == 6 && !strcmp(key, "outs")) s.outs = atoi(value);
      else if (!strcmp(key, "onFirst") && on) s.bases |= 1;
      else if (!strcmp(key, "onSecond") && on) s.bases |= 2;
      else if (!strcmp(key, "onThird") && on) s.bases |= 4;
    }
  }
};

// children.C.standings.entries.E.<...>: the two leagues
struct TableScan {
  Table tables[2];
  uint8_t slot[4] = {0, 1, 0, 1};  // children index -> American (0) or National (1) League, from its abbreviation when it comes
  char stat[16] = "";

  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    TableScan &s = *static_cast<TableScan *>(sink);
    if (n < 3 || strcmp(path[0], "children")) return;
    const int child = atoi(path[1]);
    if (child < 0 || child > 3) return;
    if (n == 3 && !strcmp(path[2], "abbreviation")) { s.slot[child] = !strcmp(value, "NL"); return; }
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
        if (!strcmp(s.stat, "overall") && sscanf(value, "%d-%d", &w, &l) == 2) { row.wins = std::min(w, 162); row.losses = std::min(l, 162); }
        else if (!strcmp(s.stat, "streak")) strlcpy(row.streak, value, sizeof(row.streak));
        else if (!strcmp(s.stat, "playoffSeed")) row.seed = std::min(atoi(value), 99);
      }
    }
  }
};

void sortTable(Table &t) {
  std::stable_sort(t.rows, t.rows + t.count, [](const Standing &a, const Standing &b) {
    if (a.seed && b.seed && a.seed != b.seed) return a.seed < b.seed;  // ESPN's own order: the division winners and wild cards first
    const int pa = a.wins * (b.wins + b.losses), pb = b.wins * (a.wins + a.losses);  // win percentage, compared without dividing
    return pa != pb ? pa > pb : a.wins > b.wins;
  });
}

// Adds a series unless the same two teams are in the list already
void addSeries(Series *list, uint8_t &count, const Series &add) {
  for (int i = 0; i < count; i++) if (!strcmp(list[i].first, add.first) && !strcmp(list[i].second, add.second)) return;
  if (count < kMaxSeries) list[count++] = add;
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
  if (createFeedTask(task, "mlb", kStackBytes, this, 1, &_task, 0) != pdPASS) _task = nullptr;
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
    consoleLog("[MLB] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }
  YieldingReader reader(http.getStream(), 10000);
  bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  if (!ok) consoleLog("[MLB] %s: %s after %u bytes\n", url, reader.dry ? "the connection stopped sending" : "not valid JSON", (unsigned)reader.count);
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
    _seriesTodayCount = 0;
    for (int i = 0; i < scan->count; i++) if (scan->hasSeries[i]) addSeries(_seriesToday, _seriesTodayCount, scan->series[i]);
    mergeSeries();
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
  Series later[kMaxSeries];
  uint8_t laterCount = 0;
  const time_t now = time(nullptr);
  for (int day = 0; day < kSeriesDays; day++) {  // today too: its games that have not begun are coming
    if (day >= kFixtureDays && !laterCount && !_seriesTodayCount) break;  // the longer look only in the playoffs
    struct tm date;
    const time_t eastern = now - 4 * 3600 + day * 86400L;  // ESPN's days are American ones (the season is in summer time)
    gmtime_r(&eastern, &date);
    char url[160];
    snprintf(url, sizeof(url), "%s?dates=%04d%02d%02d", kScoreboard, date.tm_year + 1900, date.tm_mon + 1, date.tm_mday);
    DayScan *scan = new DayScan;
    bool ok = get(url, DayScan::leaf, scan);
    scan->endEvent();
    if (ok) {
      for (int i = 0; i < scan->count; i++) if (scan->hasSeries[i]) addSeries(later, laterCount, scan->series[i]);
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
  memcpy(_seriesLater, later, sizeof(later));
  _seriesLaterCount = laterCount;
  mergeSeries();
  _data.version = ++_version;
  return true;
}

void Ticker::mergeSeries() {
  _data.seriesCount = 0;
  for (int i = 0; i < _seriesTodayCount; i++) _data.series[_data.seriesCount++] = _seriesToday[i];
  for (int i = 0; i < _seriesLaterCount && _data.seriesCount < kMaxSeries; i++) {
    bool have = false;
    for (int j = 0; j < _data.seriesCount; j++) have |= !strcmp(_data.series[j].first, _seriesLater[i].first) && !strcmp(_data.series[j].second, _seriesLater[i].second);
    if (!have) _data.series[_data.seriesCount++] = _seriesLater[i];
  }
}

bool Ticker::refreshStandings() {
  TableScan *scan = new TableScan;
  bool ok = get(kStandings, TableScan::leaf, scan);
  if (ok) {
    sortTable(scan->tables[0]);
    sortTable(scan->tables[1]);
    std::lock_guard<std::mutex> guard(_lock);
    _data.american = scan->tables[0];
    _data.national = scan->tables[1];
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
      consoleLog("[MLB] heap too low, skipping a refresh\n");
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
      consoleLog("[MLB] out of memory in a refresh (%s)\n", e.what());
      ticker->_todayAt = millis() + kRetryMs;
    }
    for (int i = 0; i < 5 && !feedsHalted(); i++) vTaskDelay(pdMS_TO_TICKS(1000));  // in slices, so an update is seen within a second
  }
}

}  // namespace mlb
