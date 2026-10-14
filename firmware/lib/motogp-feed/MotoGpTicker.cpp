// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The MotoGP data service. It fills the same snapshot as the F1 one (F1Ticker.h) so that the F1 face can show it:
//  - the season's calendar, the weekend's sessions, the results of the finished ones and the riders' championship, from
//    the MotoGP results API (api.pulselive.motogp.com)
//  - the running order of a session that is on, from the same site's live timing (livetiming-lite)
// Only the premier class is followed. A background task on core 0 downloads; the display copies the snapshot.
#include "F1Ticker.h"

#include <algorithm>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Console.h>
#include <EspnFeed.h>
#include <NetworkBudget.h>
#include <TimeService.h>
#include "MotoGpScan.h"


namespace {
void say(const char *format, ...) {
  static std::mutex lock;
  char line[200];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  std::lock_guard<std::mutex> guard(lock);
  Serial.print(line);
  Console.print(line);
}
}  // namespace

// ---- The service ----

uint16_t F1Ticker::driverColor(const char *code) {
  const MotoGpRider *rider = motoGpRiderByCode(code);
  return rider ? motoGpColor(rider->maker) : 0xFFFF;
}

void F1Ticker::begin(ClockTime *dateTime) {
  if (_started) return;
  _started = true;
  _dateTime = dateTime;
  resume();  // core 0: downloading never stalls the display
}

void F1Ticker::resume() {
  if (!_started || _task || firmwareUpdating) return;
  createFeedTask(task, "motogp", kTaskStack, this, 1, &_task, 0);
}

uint32_t F1Ticker::version() {
  resume();
  std::lock_guard<std::mutex> guard(_lock);
  return _version;
}

void F1Ticker::snapshot(Snapshot &out, bool) {
  std::lock_guard<std::mutex> guard(_lock);
  out = _snap;
}

bool F1Ticker::getScan(const char *url, JsonScan::Leaf leaf, void *sink) {
  std::lock_guard<std::mutex> download(networkBudget());
  if (firmwareUpdating) return false;
  WiFiClientSecure tls;
  tls.setInsecure();  // public data, nothing to protect
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(tls, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    say("[MotoGP] %s: HTTP %d\n", url, code);
    http.end();
    return false;
  }
  YieldingReader reader(http.getStream(), 10000);
  const bool ok = JsonScan::read([](void *r) { return ((YieldingReader *)r)->read(); }, &reader, leaf, sink);
  http.end();
  if (!ok) say("[MotoGP] %s: cut off or not JSON after %u bytes (heap free %u)\n", url, (unsigned)reader.count,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  return ok;
}

// The calendar: the weekend that is on and the races to come. Returns how long to wait before the next download.
unsigned long F1Ticker::fetchSeason() {
  char url[200];
  static char season[40], category[40];
  if (!season[0]) {
    SeasonScan scan = {season, nullptr};
    snprintf(url, sizeof(url), "%s/results/seasons", kApi);
    if (!getScan(url, SeasonScan::leaf, &scan) || !season[0]) return kRetryMs;
  }
  if (!category[0]) {
    SeasonScan scan = {nullptr, category};
    snprintf(url, sizeof(url), "%s/results/categories?seasonUuid=%s", kApi, season);
    if (!getScan(url, SeasonScan::leaf, &scan) || !category[0]) return kRetryMs;
  }
  strlcpy(seasonId, season, sizeof(seasonId));
  strlcpy(categoryId, category, sizeof(categoryId));
  EventScan scan;
  scan.now = ultime::now();
  eventCount = 0;
  snprintf(url, sizeof(url), "%s/results/events?seasonUuid=%s", kApi, season);
  if (!getScan(url, EventScan::leaf, &scan)) { season[0] = 0; return kRetryMs; }
  scan.finish();

  Race races[UPCOMING];
  uint8_t raceCount = 0;
  for (int i = 0; i < eventCount && raceCount < UPCOMING; i++) {
    const Event &e = events[i];
    if (e.finished) continue;
    Race &r = races[raceCount++];
    strlcpy(r.city, e.place, sizeof(r.city));
    strlcpy(r.place, e.country, sizeof(r.place));
    snprintf(r.circuit, sizeof(r.circuit), "%.14s %.21s %.10s", e.country, e.circuit, e.place);  // the country first: the flag is found by it
    r.start = e.start + 9 * 3600;  // the first session, about nine in the morning UTC
  }
  std::lock_guard<std::mutex> guard(_lock);
  for (int i = 0; i < UPCOMING; i++) _snap.upcoming[i] = i < raceCount ? races[i] : Race();
  _snap.upcomingCount = raceCount;
  _version++;
  say("[MotoGP] calendar: %d events, next %s\n", eventCount, eventCount ? events[0].shortName : "-");
  return kSeasonRefreshMs;
}

unsigned long F1Ticker::fetchStandings() {
  if (!seasonId[0] || !categoryId[0]) return kRetryMs;
  char url[200];
  snprintf(url, sizeof(url), "%s/results/standings?seasonUuid=%s&categoryUuid=%s", kApi, seasonId, categoryId);
  StandingScan scan;
  if (!getScan(url, StandingScan::leaf, &scan)) return kRetryMs;
  scan.finish();
  std::lock_guard<std::mutex> guard(_lock);
  for (int i = 0; i < STANDINGS; i++) _snap.standings[i] = i < scan.count ? scan.rows[i] : Standing();
  _snap.standingsCount = scan.count;
  _snap.standingsStale = false;
  _version++;
  say("[MotoGP] championship: %d riders, leader %s\n", scan.count, scan.count ? scan.rows[0].code : "-");
  return kStandingsRefreshMs;
}

// The weekend: its sessions, the one that is on, the last one's results. Returns how long to wait.
unsigned long F1Ticker::fetchWeekend() {
  const time_t now = ultime::now();
  const Event *weekend = nullptr;
  for (int i = 0; i < eventCount; i++)
    if (now >= events[i].start - 24 * 3600 && now <= events[i].end + 30 * 3600) weekend = &events[i];
  if (!weekend) {
    std::lock_guard<std::mutex> guard(_lock);
    if (_snap.weekend || _snap.live.valid || _snap.last.valid || _snap.next.valid) {
      _snap.weekend = false;
      _snap.live = Session();
      _snap.last = Session();
      _snap.next = Session();
      _snap.comingCount = 0;
      _snap.circuit[0] = 0;
      _version++;
    }
    sessionCount = 0;
    liveEventOn = false;
    weekendEvent[0] = 0;
    return 5 * 60 * 1000UL;
  }
  if (strcmp(weekendEvent, weekend->id)) {
    strlcpy(weekendEvent, weekend->id, sizeof(weekendEvent));
    sessionCount = 0;
    lastReady = false;
    lastId[0] = 0;
    sessionsDueAt = 0;
  }
  char circuit[48];
  snprintf(circuit, sizeof(circuit), "%.14s %.21s %.10s", weekend->country, weekend->circuit, weekend->place);

  // The session list: now and then, and often while a session is near
  if (!sessionCount || !sessionsDueAt || (int32_t)(millis() - sessionsDueAt) >= 0) {
    char url[200];
    snprintf(url, sizeof(url), "%s/results/sessions?eventUuid=%s&categoryUuid=%s", kApi, weekend->id, categoryId);
    SessionScan scan;
    if (!getScan(url, SessionScan::leaf, &scan)) return kRetryMs;
    scan.finish();
    sessionCount = scan.count;
    for (int i = 0; i < sessionCount; i++) sessions[i] = scan.found[i];
    std::sort(sessions, sessions + sessionCount, [](const Sess &a, const Sess &b) { return a.start < b.start; });
    sessionsDueAt = millis() + kSessionsIdleMs;
    say("[MotoGP] %s: %d sessions\n", weekend->shortName, sessionCount);
  }

  // Which session is on, which was the last one, which is next
  int on = -1, lastDone = -1, nextOne = -1;
  for (int i = 0; i < sessionCount; i++) {
    const Sess &s = sessions[i];
    const bool over = finished(s) || s.start + sessionLength(s) + 30 * 60 < now;
    if (!over && now >= s.start - 120 && now <= s.start + sessionLength(s) + 20 * 60) on = i;
    if (over) lastDone = i;
    else if (nextOne < 0 && s.start > now) nextOne = i;
  }
  liveEventOn = on >= 0;
  bool near = on >= 0 || (nextOne >= 0 && sessions[nextOne].start - now < 15 * 60);
  if (near) sessionsDueAt = std::min<unsigned long>(sessionsDueAt, millis() + kSessionsBusyMs);

  // The live order, from the live timing
  Session live;
  if (on >= 0) {
    const Sess &s = sessions[on];
    LiveScan scan;
    char tag;
    char name[8];
    sessionName(s, name, tag);
    scan.tag = tag;
    char url[120];
    snprintf(url, sizeof(url), "%s/timing-gateway/livetiming-lite", kApi);
    char expected[8];
    shortNameOf(s, expected, sizeof(expected));
    if (getScan(url, LiveScan::leaf, &scan)) {
      scan.finish();
      if (!strcmp(scan.shortName, expected)) {
        const bool over = !strcmp(scan.status, "F");
        if (over && !liveFinishedSeen) liveFinishedSeen = now;
        if (!over) liveFinishedSeen = 0;
        if (!over || now - liveFinishedSeen < kLiveTailSecs) {
          live.valid = true;
          live.live = true;
          live.fromFeed = true;
          live.tag = tag;
          strlcpy(live.name, name, sizeof(live.name));
          live.finished = over;
          live.finishedAt = over ? liveFinishedSeen : 0;
          live.start = s.start;
          live.id = sessionNumber(s);
          live.period = scan.leaderLap;
          live.totalLaps = tag == 'R' || tag == 'S' ? scan.totalLaps : 0;
          live.count = scan.rowCount;
          memcpy(live.rows, scan.rows, sizeof(live.rows));
          if (tag == 'P' || tag == 'Q') {
            const int left = atoi(scan.remaining);  // seconds, "0" once it is over
            if (strchr(scan.remaining, ':')) strlcpy(live.clock, scan.remaining, sizeof(live.clock));
            else snprintf(live.clock, sizeof(live.clock), "%d:%02d", left / 60 % 100, left % 60);
          }
        }
      }
    }
  } else {
    liveFinishedSeen = 0;
  }

  // The results of the last finished session, once
  Session last;
  bool haveLast = false;
  {
    std::lock_guard<std::mutex> guard(_lock);
    last = _snap.last;
    haveLast = last.valid;
  }
  if (lastDone >= 0 && !liveDataActive()) {
    const Sess &s = sessions[lastDone];
    if (strcmp(lastId, s.id) || !lastReady) {
      ResultScan scan;
      char tag;
      char name[8];
      sessionName(s, name, tag);
      scan.tag = tag;
      char url[160];
      snprintf(url, sizeof(url), "%s/results/session/%s/classification?test=false", kApi, s.id);
      if (getScan(url, ResultScan::leaf, &scan)) {
        scan.finish();
        // Qualifying: the second part's twelve, then the others in the order of the first part
        if (tag == 'Q' && s.number == 2) {
          for (int i = lastDone - 1; i >= 0; i--)
            if (!strcmp(sessions[i].type, "Q") && sessions[i].number == 1) {
              ResultScan first;
              first.tag = 'Q';
              snprintf(url, sizeof(url), "%s/results/session/%s/classification?test=false", kApi, sessions[i].id);
              if (getScan(url, ResultScan::leaf, &first)) {
                first.finish();
                for (int k = 0; k < first.count && scan.count < ROWS; k++) {
                  bool present = false;
                  for (int j = 0; j < scan.count; j++) present |= !strcmp(scan.rows[j].code, first.rows[k].code);
                  if (!present && strcmp(first.rows[k].time, "NO TIME")) scan.rows[scan.count++] = first.rows[k];
                }
              }
              break;
            }
        }
        last = Session();
        last.valid = true;
        last.tag = tag;
        strlcpy(last.name, name, sizeof(last.name));
        last.start = s.start;
        last.id = sessionNumber(s);
        last.finished = true;
        last.count = scan.count;
        memcpy(last.rows, scan.rows, sizeof(last.rows));
        strlcpy(lastId, s.id, sizeof(lastId));
        lastReady = true;
        haveLast = true;
        if (tag == 'R' || tag == 'S') _standingsAt = millis() + 10 * 60 * 1000UL;  // the championship has changed
        say("[MotoGP] results of %s: %d riders\n", name, scan.count);
      } else {
        return kRetryMs;
      }
    }
  }

  // The coming sessions
  Session next;
  Snapshot::Coming coming[Snapshot::COMING];
  uint8_t comingCount = 0;
  for (int i = 0; i < sessionCount && comingCount < Snapshot::COMING; i++) {
    const Sess &s = sessions[i];
    if (finished(s) || s.start + sessionLength(s) < now || i == on) continue;
    char name[8];
    char tag;
    sessionName(s, name, tag);
    if (comingCount == 0) {
      next.valid = true;
      next.tag = tag;
      strlcpy(next.name, name, sizeof(next.name));
      next.start = s.start;
      next.id = sessionNumber(s);
    }
    strlcpy(coming[comingCount].name, name, sizeof(coming[comingCount].name));
    coming[comingCount].start = s.start;
    comingCount++;
  }

  {
    std::lock_guard<std::mutex> guard(_lock);
    _snap.weekend = true;
    strlcpy(_snap.circuit, circuit, sizeof(_snap.circuit));
    _snap.live = live;
    if (haveLast) _snap.last = last;
    _snap.next = next;
    for (int i = 0; i < Snapshot::COMING; i++) _snap.coming[i] = i < comingCount ? coming[i] : Snapshot::Coming();
    _snap.comingCount = comingCount;
    // A new version only when something that is drawn has changed
    uint32_t h = live.valid ? hashText(live.clock) ^ live.count ^ live.period * 31 ^ (live.finished ? 7 : 0) : 0;
    for (int i = 0; i < live.count; i++) h = h * 33 + hashText(live.rows[i].code) + hashText(live.rows[i].time);
    h ^= comingCount * 977 ^ (uint32_t)lastId[0] << 8 ^ (uint32_t)(haveLast ? last.count : 0);
    if (h != liveHash) { liveHash = h; _version++; }
  }
  const bool polling = on >= 0;
  return polling ? kLiveMs : near ? kSessionsBusyMs : kSessionsIdleMs;
}

unsigned long F1Ticker::refresh() {
  if (ultime::timeStatus() != timeSet || WiFi.status() != WL_CONNECTED) return 2000;
  unsigned long now = millis();
  // Bootstrap the calendar once; thereafter current-session reads go first.
  if (!eventCount && !liveDataActive() && (!_seasonAt || (int32_t)(now - _seasonAt) >= 0)) {
    _seasonAt = millis() + fetchSeason();
    now = millis();
  }
  if (eventCount && (!_weekendAt || (int32_t)(now - _weekendAt) >= 0)) {
    _weekendAt = millis() + fetchWeekend();
    now = millis();
  }
  if (!liveDataActive() && !firmwareUpdating) {
    if (eventCount && (!_seasonAt || (int32_t)(now - _seasonAt) >= 0)) _seasonAt = millis() + fetchSeason();
    if (!_standingsAt || (int32_t)(now - _standingsAt) >= 0) _standingsAt = millis() + fetchStandings();
    now = millis();
  }
  unsigned long wait = 5000;
  for (unsigned long due : {_seasonAt, _standingsAt, _weekendAt})
    if (due && (int32_t)(due - now) > 0) wait = std::min<unsigned long>(wait, due - now);
  return wait < 250 ? 250 : wait;
}

void F1Ticker::task(void *self) {
  waitForFeedRegistration();
  F1Ticker *ticker = static_cast<F1Ticker *>(self);
  for (;;) {
    if (firmwareUpdating) {  // the update has the network and the heap to itself: hand back the stack
      ticker->_task = nullptr;
      stopFeedTask();
    }
    for (unsigned long left = ticker->refresh(); left > 0 && !firmwareUpdating;) {
      const unsigned long slice = std::min(left, 1000UL);
      vTaskDelay(pdMS_TO_TICKS(slice));
      left -= slice;
    }
  }
}
