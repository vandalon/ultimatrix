// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "SportsWatch.h"

#include <EspnFeed.h>
#include <HTTPClient.h>
#include <NetworkBudget.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <strings.h>

using namespace sportsplan;

namespace {

const char kMotoGpApi[] = "https://api.pulselive.motogp.com/motogp/v1/results";
const time_t kSlow = 15 * 60, kFast = 3 * 60, kRetry = 2 * 60;

// Events are kept sorted by start, live ones first when the list is full
void addEvent(Events &out, time_t start, time_t secs, bool live, const char *label = "") {
  Event e;
  strlcpy(e.label, label, sizeof(e.label));
  e.start = start;
  e.end = start + secs;
  e.live = live;
  if (out.count < kMaxEvents) out.list[out.count++] = e;
  else if (live || start < out.list[kMaxEvents - 1].start) out.list[kMaxEvents - 1] = e;
  else return;
  for (int i = out.count - 1; i > 0 && out.list[i].start < out.list[i - 1].start; --i) std::swap(out.list[i], out.list[i - 1]);
}

// The poll interval: short when something is on or about to start
time_t nextPoll(const Events &e, time_t now) {
  for (uint8_t i = 0; i < e.count; ++i)
    if (e.list[i].live || (e.list[i].start - now < 3600 && e.list[i].end > now)) return kFast;
  return kSlow;
}

time_t parseDay(const char *day) {
  int y, m, d;
  return sscanf(day, "%d-%d-%d", &y, &m, &d) == 3 ? ultime::makeTime(0, 0, 0, d, m, y) : 0;
}

// ---- F1: ESPN's scoreboard lists the sessions of the weekend that is on or the next one ----
struct NextScan {  // the first event of an ESPN board: name and date
  char date[24] = "", name[40] = "";
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    NextScan &s = *(NextScan *)sink;
    if (jsonPathIs(path, n, "events.0.date")) strlcpy(s.date, value, sizeof(s.date));
    else if (jsonPathIs(path, n, "events.0.name")) strlcpy(s.name, value, sizeof(s.name));
  }
};

struct F1Scan {
  struct Session { char date[24] = "", abbr[12] = "", state[8] = ""; } s[8];
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    F1Scan &scan = *(F1Scan *)sink;
    if (n < 5 || strcmp(path[0], "events") || strcmp(path[1], "0")) return;
    const int k = atoi(path[3]);
    if (k < 0 || k >= 8) return;
    if (jsonPathIs(path, n, "events.0.competitions.*.date")) strlcpy(scan.s[k].date, value, sizeof(scan.s[k].date));
    else if (jsonPathIs(path, n, "events.0.competitions.*.type.abbreviation")) strlcpy(scan.s[k].abbr, value, sizeof(scan.s[k].abbr));
    else if (jsonPathIs(path, n, "events.0.competitions.*.status.type.state")) strlcpy(scan.s[k].state, value, sizeof(scan.s[k].state));
  }
};

// ---- MotoGP: seasons, categories, events, then the sessions of the coming weekend ----
struct IdScan {  // seasons (the current one, and a newer one if there is) and categories (the MotoGP class)
  char *season, *category;
  char lastId[40] = "", newestId[40] = "";
  int lastYear = 0, currentYear = 0, newestYear = 0;
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    IdScan &s = *(IdScan *)sink;
    if (jsonPathIs(path, n, "*.id")) strlcpy(s.lastId, value, sizeof(s.lastId));
    else if (jsonPathIs(path, n, "*.year")) { s.lastYear = atoi(value); if (s.lastYear > s.newestYear) { s.newestYear = s.lastYear; strlcpy(s.newestId, s.lastId, sizeof(s.newestId)); } }
    else if (jsonPathIs(path, n, "*.current") && !text && !strcmp(value, "true") && s.season) { strlcpy(s.season, s.lastId, 40); s.currentYear = s.lastYear; }
    else if (jsonPathIs(path, n, "*.name") && s.category && !strncmp(value, "MotoGP", 6)) strlcpy(s.category, s.lastId, 40);
  }
};

struct WeekendScan {  // the first event that is not over
  time_t now = 0;
  int index = -1;
  char id[40] = "";
  time_t start = 0, end = 0;
  bool test = false;
  char bestId[40] = "";
  time_t bestStart = 0, bestEnd = 0;
  void finish() {
    if (index < 0 || test || !id[0] || !start || end + 86400 < now) return;
    if (bestStart && bestStart <= start) return;
    strlcpy(bestId, id, sizeof(bestId));
    bestStart = start;
    bestEnd = end;
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    WeekendScan &s = *(WeekendScan *)sink;
    if (n != 2) return;
    const int index = atoi(path[0]);
    if (index != s.index) { s.finish(); s.index = index; s.id[0] = 0; s.start = s.end = 0; s.test = false; }
    if (!strcmp(path[1], "id")) strlcpy(s.id, value, sizeof(s.id));
    else if (!strcmp(path[1], "test")) s.test = !text && !strcmp(value, "true");
    else if (!strcmp(path[1], "date_start")) s.start = parseDay(value);
    else if (!strcmp(path[1], "date_end")) s.end = parseDay(value);
  }
};

struct SessionScan {
  struct Session { char type[8] = "", status[16] = ""; time_t start = 0; int number = 0; } s[16];
  int count = 0, index = -1;
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    SessionScan &scan = *(SessionScan *)sink;
    if (n != 2) return;
    const int index = atoi(path[0]);
    if (index < 0 || index >= 16) return;
    scan.count = std::max(scan.count, index + 1);
    Session &c = scan.s[index];
    if (!strcmp(path[1], "type")) strlcpy(c.type, value, sizeof(c.type));
    else if (!strcmp(path[1], "status")) strlcpy(c.status, value, sizeof(c.status));
    else if (!strcmp(path[1], "date")) c.start = parseEspnDate(value);
    else if (!strcmp(path[1], "number")) c.number = atoi(value);
  }
};

// ---- Football: the scoreboards of the followed leagues ----
struct BoardScan {
  struct Match { char date[24] = "", state[8] = "", name[14] = ""; bool favourite = false; } m[12];
  int count = 0;
  const char *teams;  // "449,359," ids of the favourites with a trailing comma
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    BoardScan &scan = *(BoardScan *)sink;
    if (n < 3 || strcmp(path[0], "events")) return;
    const int k = atoi(path[1]);
    if (k < 0 || k >= 12) return;
    scan.count = std::max(scan.count, k + 1);
    if (jsonPathIs(path, n, "events.*.date")) strlcpy(scan.m[k].date, value, sizeof(scan.m[k].date));
    else if (jsonPathIs(path, n, "events.*.status.type.state")) strlcpy(scan.m[k].state, value, sizeof(scan.m[k].state));
    else if (jsonPathIs(path, n, "events.*.shortName")) strlcpy(scan.m[k].name, value, sizeof(scan.m[k].name));
    else if (jsonPathIs(path, n, "events.*.competitions.0.competitors.*.team.id") && scan.teams) {
      char needle[24];
      snprintf(needle, sizeof(needle), ",%s,", value);
      if (strstr(scan.teams, needle)) scan.m[k].favourite = true;
    }
  }
};

// A team's next match: ESPN soccer/all/teams/<id>
struct TeamScan {
  char date[24] = "", state[8] = "", name[14] = "";
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool) {
    TeamScan &s = *(TeamScan *)sink;
    if (jsonPathIs(path, n, "team.nextEvent.0.date")) strlcpy(s.date, value, sizeof(s.date));
    else if (jsonPathIs(path, n, "team.nextEvent.0.competitions.0.status.type.state")) strlcpy(s.state, value, sizeof(s.state));
    else if (jsonPathIs(path, n, "team.nextEvent.0.shortName")) strlcpy(s.name, value, sizeof(s.name));
  }
};

}  // namespace

SportsWatch &SportsWatch::instance() {
  static SportsWatch *watch = nullptr;  // on the heap: only a pointer is static RAM, and only a clock that uses it pays for the rest
  if (!watch) { watch = new SportsWatch(); watch->_lock = xSemaphoreCreateMutex(); }
  return *watch;
}

void SportsWatch::configure(const Config &cfg) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _cfg = cfg;
  _dirty = true;
  xSemaphoreGive(_lock);
  bool any = false;
  for (bool on : cfg.sports) any |= on;
  _stop = !any;
  poll();
}

void SportsWatch::poll() {
  if (_running || _stop || firmwareUpdating) return;
  _running = true;
  if (xTaskCreatePinnedToCore(task, "sportswatch", 9216, this, 1, nullptr, 0) != pdPASS) _running = false;
}

void SportsWatch::events(Events (&out)[SPORTS]) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  for (int s = 0; s < SPORTS; ++s) out[s] = _events[s];
  xSemaphoreGive(_lock);
}

// The end of a finished event: when it was first seen finished, but never later than its assumed length, so a clock that starts
// after the match still counts from the assumed end. Remembered per start time (small table, oldest replaced).
time_t SportsWatch::finishedAt(uint8_t sport, time_t start, time_t secs, time_t now) {
  Done *slot = nullptr, *oldest = &_done[sport][0];
  for (Done &d : _done[sport]) {
    if (d.start == start) { slot = &d; break; }
    if (!d.start || d.seen < oldest->seen || !oldest->start) oldest = &d;
  }
  if (!slot) { slot = oldest; slot->start = start; slot->seen = now; }
  return std::min(slot->seen, start + secs);
}

void SportsWatch::publish(uint8_t sport, const Events &found) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _events[sport] = found;
  xSemaphoreGive(_lock);
}

void SportsWatch::task(void *self) {
  ((SportsWatch *)self)->loop();
  ((SportsWatch *)self)->_running = false;
  vTaskDelete(nullptr);
}

void SportsWatch::loop() {
  time_t due[SPORTS] = {};
  while (!_stop && !firmwareUpdating) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const time_t now = time(nullptr);
    if (now < 100000 || feedsHalted() || WiFi.status() != WL_CONNECTED) continue;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const Config cfg = _cfg;
    const bool dirty = _dirty;
    _dirty = false;
    xSemaphoreGive(_lock);
    if (dirty) for (int s = 0; s < SPORTS; ++s) due[s] = 0;
    for (int s = 0; s < SPORTS; ++s) {
      if (!cfg.sports[s]) {
        if (_events[s].count) publish(s, Events());
        continue;
      }
      if (now < due[s]) continue;
      due[s] = now + (s == F1 ? refreshF1(cfg, now) : s == MOTOGP ? refreshMotoGp(cfg, now) : refreshFootball(cfg, now));  // one sport per second at most
      break;
    }
  }
}

bool SportsWatch::get(const char *url, JsonScan::Leaf leaf, void *sink) {
  std::lock_guard<std::mutex> download(networkBudget());
  if (firmwareUpdating || feedsHalted()) return false;
  const bool secure = !strncmp(url, "https", 5);
  if (secure && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 45000) return false;  // a TLS handshake needs this much in one piece
  WiFiClientSecure tls;
  WiFiClient plain;
  tls.setInsecure();  // public schedules, nothing to protect
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0");
  if (!(secure ? http.begin(tls, url) : http.begin(plain, url))) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  YieldingReader reader(http.getStream(), 10000);
  const bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  return ok;
}

time_t SportsWatch::refreshF1(const Config &cfg, time_t now) {
  static const char kBoard[] = "http://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard";
  F1Scan scan;
  if (!get(kBoard, F1Scan::leaf, &scan)) return kRetry;
  Events found;
  found.checkedAt = now;
  bool ahead = false;  // a session that has not finished
  for (int pass = 0; pass < 2; ++pass) {
    for (const auto &s : scan.s) {
      const time_t start = parseEspnDate(s.date);
      if (!start) continue;
      const bool over = !strcmp(s.state, "post");
      const bool practice = !strncmp(s.abbr, "FP", 2);
      const bool qualifying = strstr(s.abbr, "Qual") || !strncmp(s.abbr, "SQ", 2);
      const bool race = !practice && !qualifying;
      if (cfg.racing == 1 ? !race : cfg.racing == 0 ? practice : false) continue;
      time_t secs = !strcmp(s.abbr, "Race") ? 2 * 3600 : 3600;
      if (over) {  // finished: kept for the stay after the end, then dropped
        secs = finishedAt(F1, start, secs, now) - start;
        if (start + secs + 2 * 3600 <= now) continue;
      } else ahead = true;
      addEvent(found, start, secs, !over && !strcmp(s.state, "in"), s.abbr);
    }
    if (pass || ahead) break;
    // The board still shows the weekend that has just ended: ask for the next four weeks
    char url[160];
    struct tm from, to;
    const time_t end = now + 28 * 86400;
    gmtime_r(&now, &from); gmtime_r(&end, &to);
    snprintf(url, sizeof(url), "%s?dates=%04d%02d%02d-%04d%02d%02d", kBoard, from.tm_year + 1900, from.tm_mon + 1, from.tm_mday, to.tm_year + 1900, to.tm_mon + 1, to.tm_mday);
    scan = F1Scan();
    if (!get(url, F1Scan::leaf, &scan)) break;
  }
  Season &next = _season[F1];
  if (ahead) next.at = 0;
  else {  // nothing is coming: when does the next season start, if it is published yet
    if (!next.tried || now - next.tried >= 12 * 3600) {
      next.tried = now; next.at = 0; next.name[0] = 0;
      char url[160];
      struct tm from, to;
      const time_t begin = now + 28 * 86400, end = now + 430 * 86400;
      gmtime_r(&begin, &from); gmtime_r(&end, &to);
      snprintf(url, sizeof(url), "%s?dates=%04d%02d%02d-%04d%02d%02d", kBoard, from.tm_year + 1900, from.tm_mon + 1, from.tm_mday, to.tm_year + 1900, to.tm_mon + 1, to.tm_mday);
      NextScan ahead_;
      if (get(url, NextScan::leaf, &ahead_)) { next.at = parseEspnDate(ahead_.date); strlcpy(next.name, ahead_.name, sizeof(next.name)); }
    }
    found.nextAt = next.at > now ? next.at : 0;
    strlcpy(found.nextName, next.name, sizeof(found.nextName));
  }
  publish(F1, found);
  const time_t wait = nextPoll(found, now);
  return wait == kSlow && found.count && found.list[0].start - now > 2 * 86400 ? 3600 : wait;  // as MotoGP: days away, read once an hour
}

time_t SportsWatch::refreshMotoGp(const Config &cfg, time_t now) {
  static char season[40], category[40], eventId[40];
  static time_t eventStart, eventEnd, eventsAt;
  char url[200];
  if (!season[0]) {
    IdScan scan = {season, nullptr};
    snprintf(url, sizeof(url), "%s/seasons", kMotoGpApi);
    if (!get(url, IdScan::leaf, &scan) || !season[0]) return kRetry;
  }
  if (!category[0]) {
    IdScan scan = {nullptr, category};
    snprintf(url, sizeof(url), "%s/categories?seasonUuid=%s", kMotoGpApi, season);
    if (!get(url, IdScan::leaf, &scan) || !category[0]) return kRetry;
  }
  if (!eventId[0] || now > eventEnd + 86400 || now - eventsAt > 6 * 3600) {
    WeekendScan weekend;
    weekend.now = now;
    snprintf(url, sizeof(url), "%s/events?seasonUuid=%s", kMotoGpApi, season);
    if (!get(url, WeekendScan::leaf, &weekend)) return kRetry;
    weekend.finish();
    eventsAt = now;
    strlcpy(eventId, weekend.bestId, sizeof(eventId));
    eventStart = weekend.bestStart;
    eventEnd = weekend.bestEnd;
    if (!eventId[0]) {  // no event left this season: look for the next season then
      Events none;
      none.checkedAt = now;
      Season &next = _season[MOTOGP];
      if (!next.tried || now - next.tried >= 12 * 3600) {
        next.tried = now; next.at = 0;
        char current[40] = "";
        IdScan scan = {current, nullptr};
        snprintf(url, sizeof(url), "%s/seasons", kMotoGpApi);
        if (get(url, IdScan::leaf, &scan) && scan.newestYear > scan.currentYear) {
          WeekendScan later;
          later.now = now;
          snprintf(url, sizeof(url), "%s/events?seasonUuid=%s", kMotoGpApi, scan.newestId);
          if (get(url, WeekendScan::leaf, &later)) { later.finish(); next.at = later.bestStart; }
        }
      }
      none.nextAt = next.at > now ? next.at : 0;
      season[0] = 0; category[0] = 0;
      publish(MOTOGP, none);
      return 6 * 3600;
    }
  }
  SessionScan scan;
  snprintf(url, sizeof(url), "%s/sessions?eventUuid=%s&categoryUuid=%s", kMotoGpApi, eventId, category);
  if (!get(url, SessionScan::leaf, &scan)) return kRetry;
  Events found;
  found.checkedAt = now;
  for (int i = 0; i < scan.count; ++i) {
    const auto &s = scan.s[i];
    if (!s.start || !strcasecmp(s.type, "WUP")) continue;
    const bool over = !strcasecmp(s.status, "FINISHED");
    const bool race = !strcmp(s.type, "RAC") || !strcmp(s.type, "SPR");
    const bool qualifying = !strcmp(s.type, "Q");
    if (cfg.racing == 1 ? !race : cfg.racing == 0 ? !(race || qualifying) : false) continue;
    char label[14];
    if (!strcmp(s.type, "RAC")) strlcpy(label, "Race", sizeof(label));
    else if (!strcmp(s.type, "SPR")) strlcpy(label, "Sprint", sizeof(label));
    else if (s.number) snprintf(label, sizeof(label), "%s%d", s.type, s.number);
    else strlcpy(label, s.type, sizeof(label));
    time_t secs = !strcmp(s.type, "RAC") ? 3600 : 2700;
    if (over) {
      secs = finishedAt(MOTOGP, s.start, secs, now) - s.start;
      if (s.start + secs + 2 * 3600 <= now) continue;
    }
    addEvent(found, s.start, secs, !over && (strcasestr(s.status, "PROGRESS") || strcasestr(s.status, "LIVE")), label);
  }
  publish(MOTOGP, found);
  const time_t wait = nextPoll(found, now);
  return wait == kSlow && eventStart - now > 2 * 86400 ? 3600 : wait;  // the weekend is days away: the plan only changes when the organisers move a session
}

time_t SportsWatch::refreshFootball(const Config &cfg, time_t now) {
  // the favourites' ids with a trailing comma, to look a team up as ",id,"
  char teams[300];
  teams[0] = ',';
  size_t at = 1;
  for (const char *p = cfg.teams; *p && at < sizeof(teams) - 12;) {
    while (*p == ',') ++p;
    while (*p >= '0' && *p <= '9') teams[at++] = *p++;
    teams[at++] = ',';
    while (*p && *p != ',') ++p;
  }
  teams[at] = 0;
  const bool anyTeam = at > 2;
  Events found;
  found.checkedAt = now;
  bool ahead = false;  // a match that has not finished
  if (!cfg.favouritesOnly || anyTeam) {
    char url[160];
    int leagues = 0;
    for (const char *p = cfg.leagues; *p && leagues < 8;) {
      char code[32];
      size_t n = 0;
      while (p[n] && p[n] != ',' && n < sizeof(code) - 1) { code[n] = p[n]; ++n; }
      code[n] = 0;
      p += n;
      while (*p == ',') ++p;
      if (!n) continue;
      ++leagues;
      BoardScan scan;
      scan.teams = teams;
      snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/%s/scoreboard", code);
      if (!get(url, BoardScan::leaf, &scan)) return kRetry;
      for (int i = 0; i < scan.count; ++i) {
        const auto &m = scan.m[i];
        const time_t start = parseEspnDate(m.date);
        if (!start || (cfg.favouritesOnly && !m.favourite)) continue;
        time_t secs = 150 * 60;
        const bool over = !strcmp(m.state, "post");
        if (over) {  // full time: kept for the stay after the end, then dropped
          secs = finishedAt(FOOTBALL, start, secs, now) - start;
          if (start + secs + 2 * 3600 <= now) continue;
        }
        addEvent(found, start, secs, !over && !strcmp(m.state, "in"), m.name);
        if (!over) ahead = true;
      }
    }
  }
  if (cfg.othersFav && anyTeam) {  // favourites playing where no followed league lists them (cups, friendlies): ask per team
    char url[96];
    int index = 0;
    for (const char *p = teams + 1; *p && index < 6; ++index) {
      char id[12];
      size_t n = 0;
      while (p[n] && p[n] != ',' && n < sizeof(id) - 1) { id[n] = p[n]; ++n; }
      id[n] = 0;
      p += n + 1;
      if (!n) { --index; continue; }
      TeamScan scan;
      snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/all/teams/%s", id);
      if (!get(url, TeamScan::leaf, &scan)) return kRetry;
      const time_t start = parseEspnDate(scan.date);
      if (start && start != _favStart[index] && _favStart[index] + 150 * 60 > now) addEvent(found, _favStart[index], 150 * 60, false, scan.name);  // ESPN moved on at kick-off
      _favStart[index] = start;
      if (start && strcmp(scan.state, "post")) { addEvent(found, start, 150 * 60, !strcmp(scan.state, "in"), scan.name); ahead = true; }
    }
  }
  if (!ahead && !cfg.favouritesOnly) {  // a break: the boards show no coming match. Ask for the next four weeks, now and then
    if (!_wideAt || now - _wideAt >= 6 * 3600) {
      _wide = Events();
      char url[200];
      struct tm from, to;
      const time_t end = now + 28 * 86400;
      gmtime_r(&now, &from); gmtime_r(&end, &to);
      int leagues = 0;
      for (const char *p = cfg.leagues; *p && leagues < 8;) {
        char code[32];
        size_t n = 0;
        while (p[n] && p[n] != ',' && n < sizeof(code) - 1) { code[n] = p[n]; ++n; }
        code[n] = 0;
        p += n;
        while (*p == ',') ++p;
        if (!n) continue;
        ++leagues;
        BoardScan scan;
        scan.teams = teams;
        snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/soccer/%s/scoreboard?dates=%04d%02d%02d-%04d%02d%02d", code, from.tm_year + 1900, from.tm_mon + 1, from.tm_mday, to.tm_year + 1900, to.tm_mon + 1, to.tm_mday);
        if (!get(url, BoardScan::leaf, &scan)) break;
        for (int i = 0; i < scan.count; ++i) {
          const time_t start = parseEspnDate(scan.m[i].date);
          if (start > now && strcmp(scan.m[i].state, "post")) addEvent(_wide, start, 150 * 60, false, scan.m[i].name);
        }
      }
      _wideAt = now;
    }
    for (uint8_t i = 0; i < _wide.count; ++i) if (_wide.list[i].start > now) addEvent(found, _wide.list[i].start, _wide.list[i].end - _wide.list[i].start, false, _wide.list[i].label);
  }
  publish(FOOTBALL, found);
  const time_t wait = nextPoll(found, now);
  return wait == kSlow && found.count && found.list[0].start - now > 2 * 86400 ? 3600 : wait;  // days away: once an hour
}
