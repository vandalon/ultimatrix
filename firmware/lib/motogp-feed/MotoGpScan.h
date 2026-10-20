// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// What reads the MotoGP downloads: small JSON scanners (JsonScan) that collect the calendar, the sessions, the results,
// the championship and the live order, and the helpers that turn the feed's text into the face's. No network here, so the
// preview tools can run it on recorded downloads.
#pragma once

#include <algorithm>
#include <new>
#include "F1Ticker.h"
#include "JsonScan.h"
#include "MotoGpRiders.h"

namespace {

const char kApi[] = "https://api.pulselive.motogp.com/motogp/v1";
const unsigned long kSeasonRefreshMs = 3UL * 60 * 60 * 1000;
const unsigned long kStandingsRefreshMs = 30UL * 60 * 1000;
const unsigned long kRetryMs = 60UL * 1000;
const unsigned long kSessionsIdleMs = 10UL * 60 * 1000;
const unsigned long kSessionsBusyMs = 60UL * 1000;
const unsigned long kLiveMs = 5UL * 1000;
const time_t kLiveTailSecs = 5 * 60;      // a finished session stays on the live screen this long
const uint32_t kTaskStack = 9216;
const int kMaxEvents = 8, kMaxSessions = 10;

// ---- Helpers ----

// UTC seconds of a calendar date and time
time_t utcOf(int y, int m, int d, int h, int mi, int s) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (time_t)(era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + s;
}

time_t parseDay(const char *date) {  // "2026-10-09"
  int y, m, d;
  if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  return utcOf(y, m, d, 0, 0, 0);
}

time_t parseTime(const char *date) {  // "2026-10-11T15:00:00+00:00", in UTC
  int y, m, d, h, mi;
  if (sscanf(date, "%d-%d-%dT%d:%d", &y, &m, &d, &h, &mi) != 5) return 0;
  return utcOf(y, m, d, h, mi, 0);
}

// Upper case ASCII from UTF-8: accents are dropped, anything else unknown becomes a space
void asciiUpper(const char *in, char *out, size_t size) {
  size_t n = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && n + 1 < size; p++) {
    if (*p < 0x80) { out[n++] = *p >= 'a' && *p <= 'z' ? *p - 32 : *p; continue; }
    if (*p == 0xC3 && p[1]) {
      static const char FOLD[] = "AAAAAAACEEEEIIIIDNOOOOO.OUUUUYTSaaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00C0 to U+00FF
      const uint8_t c = p[1];
      if (c >= 0x80 && c < 0xC0) { const char f = FOLD[c - 0x80]; out[n++] = f >= 'a' ? f - 32 : f; }
      p++;
      continue;
    }
    while (p[1] && (p[1] & 0xC0) == 0x80) p++;  // another multi-byte letter: skipped
  }
  out[n] = 0;
}

uint32_t hashText(const char *text) {  // FNV-1a, for the session numbers the face compares
  uint32_t h = 2166136261u;
  for (; *text; text++) h = (h ^ (uint8_t)*text) * 16777619u;
  return h ? h : 1;
}

// "01:28.852" or "1'28.852" as "1:28.852"
void lapText(const char *in, char *out, size_t size) {
  while (*in == '0' && in[1] >= '0' && in[1] <= '9') in++;
  size_t n = 0;
  for (; *in && n + 1 < size; in++) out[n++] = *in == '\'' ? ':' : *in;
  out[n] = 0;
}

// The gap to the leader as the face writes it: "+.109", "+4.26", "+12.3", "+123"
void gapText(const char *in, char *out, size_t size) {
  char *end;
  const double gap = strtod(in, &end);
  if (end == in || *end) {  // "1 Lap" and the like
    out[0] = '+';
    size_t n = 1;
    for (; *in && n + 1 < size; in++) if (*in != ' ') out[n++] = *in >= 'a' && *in <= 'z' ? *in - 32 : *in;
    out[n] = 0;
    return;
  }
  if (gap < 0.9995) snprintf(out, size, "+.%03d", (int)(gap * 1000 + 0.5));
  else if (gap < 9.995) snprintf(out, size, "+%.2f", gap);
  else if (gap < 99.95) snprintf(out, size, "+%.1f", gap);
  else snprintf(out, size, "+%d", (int)(gap + 0.5));
}

// ---- What the downloads collect ----

struct Event {
  char id[40] = "", shortName[8] = "", gp[40] = "", circuit[40] = "", place[24] = "", country[24] = "";
  time_t start = 0, end = 0;
  bool test = false, finished = false;
};
struct Sess {
  char id[40] = "", type[6] = "", status[16] = "";
  int8_t number = 0;
  time_t start = 0;
};

// On the heap, allocated when the calendar is first read: 2.3 KB less static RAM in every boot (the MotoGP build is close to the budget)
Event *events = nullptr;
uint8_t eventCount = 0;
Sess *sessions = nullptr;
uint8_t sessionCount = 0;
char seasonId[40] = "", categoryId[40] = "";
char weekendEvent[40] = "";           // the event the sessions were downloaded for
char lastId[40] = "";                 // the session whose results `last` holds
bool lastReady = false;
time_t liveFinishedSeen = 0;          // when the live feed first said the session was over

// Session names as the face writes them
void sessionName(const Sess &s, char *name, char &tag) {
  if (!strcmp(s.type, "FP")) { snprintf(name, 8, "FP%d", s.number ? s.number : 1); tag = 'P'; }
  else if (!strcmp(s.type, "PR")) { strlcpy(name, "PRAC", 8); tag = 'P'; }
  else if (!strcmp(s.type, "Q")) { snprintf(name, 8, "Q%d", s.number ? s.number : 2); tag = 'Q'; }
  else if (!strcmp(s.type, "SPR")) { strlcpy(name, "SPRINT", 8); tag = 'S'; }
  else if (!strcmp(s.type, "WUP")) { strlcpy(name, "WUP", 8); tag = 'P'; }
  else { strlcpy(name, "RACE", 8); tag = 'R'; }
}

time_t sessionLength(const Sess &s) {
  if (!strcmp(s.type, "FP")) return s.number == 2 ? 30 * 60 : 45 * 60;
  if (!strcmp(s.type, "PR")) return 60 * 60;
  if (!strcmp(s.type, "Q")) return 15 * 60;
  if (!strcmp(s.type, "SPR")) return 25 * 60;
  if (!strcmp(s.type, "WUP")) return 10 * 60;
  return 55 * 60;
}

bool finished(const Sess &s) { return !strcasecmp(s.status, "FINISHED"); }

// ---- JSON readers ----

struct SeasonScan {
  char *season, *category;
  char lastId[40] = "";
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    SeasonScan &s = *(SeasonScan *)sink;
    if (jsonPathIs(path, n, "*.id")) strlcpy(s.lastId, value, sizeof(s.lastId));
    else if (jsonPathIs(path, n, "*.current") && !text && !strcmp(value, "true") && s.season) strlcpy(s.season, s.lastId, 40);
    else if (jsonPathIs(path, n, "*.name") && s.category && !strncmp(value, "MotoGP", 6)) strlcpy(s.category, s.lastId, 40);
  }
};

struct EventScan {
  Event cur;
  int index = -1;
  time_t now;
  void finish() {
    if (index < 0 || cur.test || !cur.start || !cur.id[0]) return;
    if (cur.end + 36 * 3600 < now) return;  // over
    if (eventCount == kMaxEvents && cur.start >= events[eventCount - 1].start) return;
    int at = eventCount < kMaxEvents ? eventCount++ : kMaxEvents - 1;
    events[at] = cur;
    for (int i = at; i > 0 && events[i].start < events[i - 1].start; i--) std::swap(events[i], events[i - 1]);
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    EventScan &s = *(EventScan *)sink;
    if (n < 2) return;
    const int index = atoi(path[0]);
    if (index != s.index) { s.finish(); s.cur = Event(); s.index = index; }
    Event &e = s.cur;
    if (jsonPathIs(path, n, "*.id")) strlcpy(e.id, value, sizeof(e.id));
    else if (jsonPathIs(path, n, "*.short_name")) strlcpy(e.shortName, value, sizeof(e.shortName));
    else if (jsonPathIs(path, n, "*.name")) strlcpy(e.gp, value, sizeof(e.gp));
    else if (jsonPathIs(path, n, "*.test")) e.test = !text && !strcmp(value, "true");
    else if (jsonPathIs(path, n, "*.date_start")) e.start = parseDay(value);
    else if (jsonPathIs(path, n, "*.date_end")) e.end = parseDay(value);
    else if (jsonPathIs(path, n, "*.status")) e.finished = !strcasecmp(value, "FINISHED");
    else if (jsonPathIs(path, n, "*.country.name")) asciiUpper(value, e.country, sizeof(e.country));
    else if (jsonPathIs(path, n, "*.circuit.name")) strlcpy(e.circuit, value, sizeof(e.circuit));
    else if (jsonPathIs(path, n, "*.circuit.place")) asciiUpper(value, e.place, sizeof(e.place));
  }
};

struct SessionScan {
  Sess cur;
  Sess found[kMaxSessions];
  uint8_t count = 0;
  int index = -1;
  void finish() {
    if (index >= 0 && cur.id[0] && cur.start && count < kMaxSessions) found[count++] = cur;
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    SessionScan &s = *(SessionScan *)sink;
    if (n < 2) return;
    const int index = atoi(path[0]);
    if (index != s.index) { s.finish(); s.cur = Sess(); s.index = index; }
    if (n != 2) return;
    Sess &c = s.cur;
    if (!strcmp(path[1], "id")) strlcpy(c.id, value, sizeof(c.id));
    else if (!strcmp(path[1], "type")) strlcpy(c.type, value, sizeof(c.type));
    else if (!strcmp(path[1], "status")) strlcpy(c.status, value, sizeof(c.status));
    else if (!strcmp(path[1], "number")) c.number = text ? 0 : atoi(value);
    else if (!strcmp(path[1], "date")) c.start = parseTime(value);
  }
};

// The rider of a row, by his race number: code, name and colour
struct RiderRow {
  int number = 0;
  char fullName[28] = "", maker[12] = "";
};
void riderCode(const RiderRow &r, char *code) {
  if (const MotoGpRider *known = motoGpRider(r.number)) { strlcpy(code, known->code, 4); return; }
  char name[28];
  asciiUpper(r.fullName, name, sizeof(name));
  const char *surname = strrchr(name, ' ');
  surname = surname ? surname + 1 : name;
  strlcpy(code, surname, 4);
}
uint16_t riderColor(const RiderRow &r) {
  if (const MotoGpRider *known = motoGpRider(r.number)) return motoGpColor(known->maker);
  return motoGpColor(motoGpMaker(r.maker));
}

struct StandingScan {
  F1Ticker::Standing rows[F1Ticker::STANDINGS];
  uint8_t count = 0;
  RiderRow rider;
  int points = 0, index = -1;
  void finish() {
    if (index < 0 || count >= F1Ticker::STANDINGS || !rider.number) return;
    F1Ticker::Standing &row = rows[count++];
    riderCode(rider, row.code);
    row.color = riderColor(rider);
    row.points = points;
    if (const MotoGpRider *known = motoGpRider(rider.number)) strlcpy(row.name, known->name, sizeof(row.name));
    else {
      char name[28];
      asciiUpper(rider.fullName, name, sizeof(name));
      const char *surname = strchr(name, ' ');
      strlcpy(row.name, surname ? surname + 1 : name, sizeof(row.name));
    }
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    StandingScan &s = *(StandingScan *)sink;
    if (n < 3 || strcmp(path[0], "classification")) return;
    const int index = atoi(path[1]);
    if (index != s.index) { s.finish(); s.rider = RiderRow(); s.points = 0; s.index = index; }
    if (jsonPathIs(path, n, "classification.*.rider.number")) s.rider.number = atoi(value);
    else if (jsonPathIs(path, n, "classification.*.rider.full_name")) strlcpy(s.rider.fullName, value, sizeof(s.rider.fullName));
    else if (jsonPathIs(path, n, "classification.*.constructor.name")) strlcpy(s.rider.maker, value, sizeof(s.rider.maker));
    else if (jsonPathIs(path, n, "classification.*.points")) s.points = atoi(value);
  }
};

struct ResultScan {
  F1Ticker::Row rows[F1Ticker::ROWS];
  uint8_t count = 0;
  RiderRow rider;
  char tag = 'R';
  int index = -1, position = 0, laps = 0;
  char lap[16] = "", gap[16] = "", status[12] = "";
  void finish() {
    if (index < 0 || !rider.number) return;
    // Only riders with a place, then the ones who did not finish, in the order the feed gives them
    if (count >= F1Ticker::ROWS) return;
    F1Ticker::Row &row = rows[count];
    riderCode(rider, row.code);
    row.color = riderColor(rider);
    if (position > 0) {
      if (tag == 'P' || tag == 'Q') {
        if (position == 1) lapText(lap, row.time, sizeof(row.time));
        else gapText(gap, row.time, sizeof(row.time));
        if (!lap[0]) strlcpy(row.time, "NO TIME", sizeof(row.time));
      } else {
        if (position == 1) strlcpy(row.time, "LEAD", sizeof(row.time));
        else gapText(gap, row.time, sizeof(row.time));
      }
    } else {
      strlcpy(row.time, tag == 'R' || tag == 'S' ? "RETIRED" : "NO TIME", sizeof(row.time));
    }
    row.finished = position > 0 && (tag == 'R' || tag == 'S');
    count++;
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    ResultScan &s = *(ResultScan *)sink;
    if (n < 3 || strcmp(path[0], "classification")) return;
    const int index = atoi(path[1]);
    if (index != s.index) { s.finish(); s.rider = RiderRow(); s.position = 0; s.lap[0] = s.gap[0] = 0; s.index = index; }
    if (jsonPathIs(path, n, "classification.*.position")) s.position = text ? 0 : atoi(value);
    else if (jsonPathIs(path, n, "classification.*.rider.number")) s.rider.number = atoi(value);
    else if (jsonPathIs(path, n, "classification.*.rider.full_name")) strlcpy(s.rider.fullName, value, sizeof(s.rider.fullName));
    else if (jsonPathIs(path, n, "classification.*.constructor.name")) strlcpy(s.rider.maker, value, sizeof(s.rider.maker));
    else if (jsonPathIs(path, n, "classification.*.best_lap.time")) strlcpy(s.lap, value, sizeof(s.lap));
    else if (jsonPathIs(path, n, "classification.*.gap.first")) strlcpy(s.gap, value, sizeof(s.gap));
  }
};

struct LiveScan {
  F1Ticker::Row rows[F1Ticker::ROWS];
  char tag = 'R';                 // the kind of session, set by the caller: it decides what a row's time is
  char shortName[8] = "", status[8] = "", remaining[12] = "";
  int totalLaps = 0, leaderLap = 0, rowCount = 0;
  // The rider being read
  char key[8] = "", lap[16] = "", gap[16] = "";
  int order = 0, number = 0;
  bool pit = false;
  void finish() {
    if (order < 1 || order > F1Ticker::ROWS) return;
    F1Ticker::Row &row = rows[order - 1];
    RiderRow r;
    r.number = number;
    riderCode(r, row.code);
    row.color = motoGpRider(number) ? motoGpColor(motoGpRider(number)->maker) : 0xFFFF;
    row.pit = pit;
    if (tag == 'P' || tag == 'Q') {
      if (order == 1) lapText(lap, row.time, sizeof(row.time));
      else if (gap[0]) gapText(gap, row.time, sizeof(row.time));
      if (!lap[0]) strlcpy(row.time, "NO TIME", sizeof(row.time));
    } else {
      if (order == 1) strlcpy(row.time, "LEAD", sizeof(row.time));
      else if (gap[0]) gapText(gap, row.time, sizeof(row.time));
    }
    if (order > rowCount) rowCount = order;
  }
  static void leaf(void *sink, const char (*path)[JsonScan::KEY], int n, const char *value, bool text) {
    LiveScan &s = *(LiveScan *)sink;
    if (n == 2 && !strcmp(path[0], "head")) {
      if (!strcmp(path[1], "session_shortname")) strlcpy(s.shortName, value, sizeof(s.shortName));
      else if (!strcmp(path[1], "session_status_id")) strlcpy(s.status, value, sizeof(s.status));
      else if (!strcmp(path[1], "remaining")) strlcpy(s.remaining, value, sizeof(s.remaining));
      else if (!strcmp(path[1], "num_laps")) s.totalLaps = atoi(value);
      return;
    }
    if (n != 3 || strcmp(path[0], "rider")) return;
    if (strcmp(path[1], s.key)) {
      s.finish();
      strlcpy(s.key, path[1], sizeof(s.key));
      s.order = atoi(path[1]); s.number = 0; s.lap[0] = s.gap[0] = 0; s.pit = false;
    }
    if (!strcmp(path[2], "rider_number")) s.number = atoi(value);
    else if (!strcmp(path[2], "lap_time")) strlcpy(s.lap, value, sizeof(s.lap));
    else if (!strcmp(path[2], "gap_first")) strlcpy(s.gap, value, sizeof(s.gap));
    else if (!strcmp(path[2], "on_pit")) s.pit = !text && !strcmp(value, "true");
    else if (!strcmp(path[2], "num_lap") && s.order == 1) s.leaderLap = atoi(value);
  }
};

// The id the face compares to see that it is the same session (a hash of the feed's uuid)
uint32_t sessionNumber(const Sess &s) { return hashText(s.id); }

// The feed's short name of a session
void shortNameOf(const Sess &s, char *out, size_t size) {
  if (!strcmp(s.type, "FP")) snprintf(out, size, "FP%d", s.number ? s.number : 1);
  else if (!strcmp(s.type, "Q")) snprintf(out, size, "Q%d", s.number ? s.number : 2);
  else strlcpy(out, s.type, size);
}

unsigned long sessionsDueAt = 0;
uint32_t liveHash = 0;

}  // namespace
