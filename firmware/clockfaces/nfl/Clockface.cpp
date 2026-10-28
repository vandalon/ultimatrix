// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// The NFL as a round of pages, much like the NBA and MLB faces:
//  - Games on: the live games one by one (favourites first), then the line score of the first.
//  - Nothing live: the favourites' games, the coming days next to the conference seeds, and the two conference tables, with the clock in between.
//  - Without any game this week: the clock with a football flying about, then the tables.
//  A score in a live game plays a text flash (TOUCHDOWN, FIELD GOAL ...) before the page returns; the row that scored lights up.
//  Pages slide over each other.

#include "Clockface.h"
#include "NflRenderer.h"
#include "NflTicker.h"
#include <FaceKit.h>
#include <Locator.h>
#include <Settings.h>
#include <new>
#include <string.h>

using namespace nflview;

namespace {

enum Kind : uint8_t { GamePage, LinePage, UpcomingPage, AfcPage, NfcPage, ClockPage, ScorePage };  // ScorePage only in the demo
struct Item { Kind kind; uint8_t index, page, pages; uint16_t ms; uint8_t variant; };  // variant: which made-up data the demo draws the page from
constexpr int kMaxItems = 28;

constexpr uint32_t kFlashMs = 2500, kSlideMs = 380, kQuietMs = 8000;

nfl::Ticker *ticker = nullptr;  // heap: the face's data must not be reserved by an update-only boot that never loads it
nfl::Snapshot *snap = nullptr;
uint32_t seenVersion = 0;
bool haveFrame = false;
uint32_t drawnAt = 0;

Item cycle[kMaxItems];
int cycleCount = 0;
uint32_t cycleMs = 0;
char cycleFavourites[64] = "";
uint8_t cycleTables = 0xFF, cycleIdle = 0xFF;
bool cycleLiveOnly = true, cycleExtra = false;
uint16_t cycleSecs = 0, cycleKeep = 0xFFFF;  // the settings the round and the result window were made with

// What the pages before the update showed, for the scores: a game by its teams
struct Score { char away[5], home[5]; int64_t start; uint16_t awayScore, homeScore; bool live; };
Score before[nfl::kMaxGames];
int beforeCount = 0;
uint32_t flashUntil[nfl::kMaxGames][2] = {};

struct Scored { bool active = false, home = false; int points = 0; uint32_t start = 0; nfl::Game game; } scored;
uint32_t lastScoreAt = 0;

// The page on show and the one sliding out
Item shown{};
Item sliding{};
uint32_t slideStart = 0;
bool haveShown = false;

void add(Kind kind, int index, int page, int pages, uint16_t ms) {
  if (cycleCount < kMaxItems) cycle[cycleCount++] = {kind, (uint8_t)index, (uint8_t)page, (uint8_t)pages, ms, 0};
}

void build(const char *favourites, ClockTime *time, uint16_t pageMs, uint8_t tables, bool liveOnly, bool extra, uint8_t idle) {
  cycleCount = 0;
  int order[nfl::kMaxGames];
  const int count = orderGames(*snap, favourites, order);
  if (snap->live) {
    int shownGames = 0;
    for (int i = 0; i < count && shownGames < 8; i++)
      if (snap->games[order[i]].state == nfl::Live) { add(GamePage, order[i], shownGames, snap->live > 8 ? 8 : snap->live, pageMs); shownGames++; }
    for (int i = 0; i < count; i++)
      if (extra && snap->games[order[i]].state == nfl::Live) { add(LinePage, order[i], 0, 1, pageMs); break; }
  } else {
    int favouriteGames = 0;
    for (int i = 0; extra && i < count && favouriteGames < 2; i++)
      if (hasFavourite(favourites, snap->games[order[i]])) {
        add(GamePage, order[i], favouriteGames, 2, pageMs);
        if (snap->games[order[i]].state == nfl::Final) add(LinePage, order[i], 0, 1, pageMs);
        favouriteGames++;
      }
  }
  if (extra ? !(snap->live && liveOnly) : !snap->live) {
    // what to show while no game is live (Settings > NFL): games in the next days, the scrolling list with its own small clock (idle 0, 2);
    // the big clock page (idle 1, 2) and when nothing is coming
    if (snap->nextCount && idle != 1) add(UpcomingPage, 0, 0, 1, upcomingMs(*snap, time, pageMs));
    if ((!snap->nextCount || idle != 0) && !snap->live) add(ClockPage, 0, 0, 1, pageMs);
  }
  if (extra && !(snap->live && liveOnly) && (tables == 2 || (tables == 1 && !snap->live))) {
    for (int page = 0, pages = standingsPages(snap->afc); page < pages; page++) add(AfcPage, 0, page, pages, pageMs);
    for (int page = 0, pages = standingsPages(snap->nfc); page < pages; page++) add(NfcPage, 0, page, pages, pageMs);
  }
  if (!cycleCount) add(ClockPage, 0, 0, 1, pageMs);
  cycleMs = 0;
  for (int i = 0; i < cycleCount; i++) cycleMs += cycle[i].ms;
}

// The page for this moment, and how far into it
uint32_t itemElapsed = 0;  // how long the page returned by current() has been up

const Item &current(uint32_t now) {
  uint32_t t = now % cycleMs;
  for (int i = 0; i < cycleCount; i++) {
    if (t < cycle[i].ms) { itemElapsed = t; return cycle[i]; }
    t -= cycle[i].ms;
  }
  return cycle[0];
}

bool sameItem(const Item &a, const Item &b) { return a.kind == b.kind && a.index == b.index && a.variant == b.variant && (a.kind == ClockPage || a.page == b.page); }  // the clock's scenes follow each other without a slide

void fillDemo(nfl::Snapshot &s, uint8_t variant);
extern bool demo;
extern uint8_t demoFilled;

void render(MatrixPanel_I2S_DMA &d, const Item &item, ClockTime *time, uint32_t now, const char *favourites) {
  if (demo && item.variant != demoFilled) { fillDemo(*snap, item.variant); demoFilled = item.variant; }
  switch (item.kind) {
    case GamePage: {
      if (item.index >= snap->count) { drawClock(d, time, now, nullptr, Settings::getInstance()->nflHours, Settings::getInstance()->nflMinutes); break; }
      const nfl::Game &g = snap->games[item.index];
      drawGame(d, g, time, now, now < flashUntil[item.index][0], now < flashUntil[item.index][1], hasFavourite(favourites, g));
      break;
    }
    case LinePage:
      if (item.index < snap->count) drawLineScore(d, snap->games[item.index], time, now);
      break;
    case UpcomingPage: {
      const Settings &settings = *Settings::getInstance();
      const uint16_t seconds = settings.matchSecs < 3 ? 3 : settings.matchSecs > 60 ? 60 : settings.matchSecs;
      drawUpcoming(d, *snap, favourites, time, now, itemElapsed, seconds * 1000);
      break;
    }
    case AfcPage: drawStandings(d, snap->afc, true, favourites, item.page, time, now); break;
    case NfcPage: drawStandings(d, snap->nfc, false, favourites, item.page, time, now); break;
    case ScorePage: drawScore(d, snap->games[0], true, item.page, itemElapsed < kScoreMs ? itemElapsed : kScoreMs); break;
    default: drawClock(d, time, demo ? item.page * 12000u + itemElapsed : now, snap->nextCount ? &snap->next[0] : nullptr, Settings::getInstance()->nflHours, Settings::getInstance()->nflMinutes);
  }
}

// A higher score than before in a game that was live: touchdown, field goal ...
void noticeScores(uint32_t now, const char *favourites) {
  for (int i = 0; i < snap->count; i++) {
    const nfl::Game &g = snap->games[i];
    for (int j = 0; j < beforeCount; j++) {
      const Score &was = before[j];
      if (strcmp(was.away, g.away) || strcmp(was.home, g.home) || was.start != g.start || !was.live || g.state != nfl::Live) continue;
      const int away = g.awayScore - was.awayScore, home = g.homeScore - was.homeScore;
      if (away > 0) flashUntil[i][0] = now + kFlashMs;
      if (home > 0) flashUntil[i][1] = now + kFlashMs;
      const bool watched = !favourites[0] || hasFavourite(favourites, g);
      if ((away > 0 || home > 0) && watched && !scored.active && now - lastScoreAt > kQuietMs) {
        scored.active = true;
        scored.home = home > away;
        scored.points = scored.home ? home : away;
        scored.start = now;
        scored.game = g;
        lastScoreAt = now;
      }
    }
  }
}

// A finished game stays for the "keep results" setting (minutes, 0 = until midnight), counted from when it was first seen over
struct Ended { char away[5], home[5]; int64_t start, at; };
Ended ended[nfl::kMaxGames];
int endedCount = 0;

void applyResultWindow(ClockTime *clock, uint32_t keepMins) {
  const int64_t now = ::time(nullptr);
  Ended next[nfl::kMaxGames];
  int nextCount = 0, kept = 0;
  snap->live = 0;
  for (int i = 0; i < snap->count; i++) {
    const nfl::Game g = snap->games[i];
    if (g.state == nfl::Final) {
      int64_t at = 0;
      for (int j = 0; j < endedCount && !at; j++)
        if (ended[j].start == g.start && !strcmp(ended[j].away, g.away) && !strcmp(ended[j].home, g.home)) at = ended[j].at;
      if (!at) at = now < g.start + 14400 ? now : g.start + 14400;  // already over when first seen: about when it can have ended (a game lasts about 3.5 hours)
      if (nextCount < nfl::kMaxGames) { strlcpy(next[nextCount].away, g.away, 5); strlcpy(next[nextCount].home, g.home, 5); next[nextCount].start = g.start; next[nextCount++].at = at; }
      const bool keep = keepMins ? now - at < (int64_t)keepMins * 60 : clock->utcToLocal((time_t)at) / 86400 == clock->localNow() / 86400;
      if (!keep) continue;
    }
    snap->live += g.state == nfl::Live;
    snap->games[kept++] = g;
  }
  snap->count = kept;
  memcpy(ended, next, sizeof(next));
  endedCount = nextCount;
}

// A Sunday holds up to 16 games: the next-up list shows whole days, as many as fit in 32 games, never part of a day
void trimFixtures(ClockTime *clock) {
  constexpr int kMaxShown = 32;
  int end = 0;
  while (end < snap->nextCount) {
    const int day = (int)(clock->utcToLocal((time_t)snap->next[end].start) / 86400);
    int next = end;
    while (next < snap->nextCount && (int)(clock->utcToLocal((time_t)snap->next[next].start) / 86400) == day) next++;
    if (end && next > kMaxShown) break;
    end = next;
  }
  snap->nextCount = end;
}

Background backgroundOf(Kind kind) {
  switch (kind) {
    case GamePage: return BgGame;
    case LinePage: return BgLine;
    case UpcomingPage: return BgNext;
    case AfcPage: return BgAfc;
    case NfcPage: return BgNfc;
    default: return BgClock;
  }
}

void remember() {
  beforeCount = snap->count;
  for (int i = 0; i < snap->count; i++) {
    const nfl::Game &g = snap->games[i];
    strlcpy(before[i].away, g.away, sizeof(before[i].away));
    strlcpy(before[i].home, g.home, sizeof(before[i].home));
    before[i].start = g.start;
    before[i].awayScore = g.awayScore; before[i].homeScore = g.homeScore; before[i].live = g.state == nfl::Live;
  }
}


// ---- the demo: every screen in turn from made-up data (POST /api/nfl/demo?on=1), once through ----
bool demo = false, demoBuilt = false;
uint32_t demoStart = 0;
nfl::Snapshot *realSnap = nullptr;  // the live data while the demo draws from its own snapshot
uint8_t demoFilled = 0xFF;
constexpr uint8_t kDemoVariants = 3;
constexpr uint32_t kSceneMs = 12000;  // the big clock plays its three scenes of this long

nfl::Game demoGame(const char *away, const char *home, int as, int hs, nfl::State state, int quarter, const char *clock, const char *ar, const char *hr, int64_t start) {
  nfl::Game g;
  strlcpy(g.away, away, sizeof(g.away)); strlcpy(g.home, home, sizeof(g.home));
  g.awayScore = as; g.homeScore = hs; g.state = state; g.quarter = quarter;
  strlcpy(g.clock, clock, sizeof(g.clock));
  strlcpy(g.awayRecord, ar, sizeof(g.awayRecord)); strlcpy(g.homeRecord, hr, sizeof(g.homeRecord));
  g.start = start;
  return g;
}

void demoPoints(nfl::Game &g, std::initializer_list<int> away, std::initializer_list<int> home) {
  int i = 0;
  for (int p : away) g.awayPoints[i++] = p;
  g.periods = i;
  i = 0;
  for (int p : home) g.homePoints[i++] = p;
  g.periods = std::max<int>(g.periods, i);
}

// Variant 0: live games, tables and a busy week; 1 the playoffs, a game a day; 2 a few games a day
void fillDemo(nfl::Snapshot &s, uint8_t variant) {
  const int64_t t0 = ::time(nullptr) + 7200, day = 86400;
  s = nfl::Snapshot();
  nfl::Game live = demoGame("CAR", "GB", 14, 20, nfl::Live, 3, "4:12", "5-2", "5-2", t0 - 7200);
  demoPoints(live, {7, 0, 7}, {3, 10, 7});
  live.possession = 2; live.down = 3; live.distance = 7; live.fieldPos = 65; live.awayTimeouts = 2; live.homeTimeouts = 3;
  strlcpy(live.spot, "GB 35", sizeof(live.spot));
  nfl::Game second = demoGame("KC", "BUF", 24, 24, nfl::Live, 4, "0:48", "5-1", "5-1", t0 - 7200);
  demoPoints(second, {7, 10, 0, 7}, {3, 7, 14, 0});
  second.possession = 1; second.down = 2; second.distance = 4; second.fieldPos = 86; second.redZone = true; second.awayTimeouts = 1; second.homeTimeouts = 1;
  strlcpy(second.spot, "BUF 14", sizeof(second.spot));
  nfl::Game half = demoGame("DAL", "PHI", 10, 17, nfl::Live, 2, "0:00", "3-3-1", "6-1", t0 - 7200);
  half.halftime = true;
  demoPoints(half, {3, 7}, {7, 10});
  nfl::Game final = demoGame("SF", "SEA", 23, 20, nfl::Final, 5, "0:00", "4-3", "5-2", t0 - 14400);
  demoPoints(final, {7, 3, 7, 3, 3}, {3, 7, 7, 3, 0});
  s.games[0] = live; s.games[1] = second; s.games[2] = half; s.games[3] = demoGame("DEN", "LV", 0, 0, nfl::Pre, 0, "", "4-3", "2-5", t0 + 3600); s.games[4] = final;
  s.count = 5; s.live = 3;
  static const char *const afc[] = {"KC", "BUF", "BAL", "PIT", "DEN", "NE", "LAC", "HOU", "IND", "CIN", "JAX", "MIA", "CLE", "NYJ", "LV", "TEN"};
  static const char *const nfc[] = {"PHI", "DET", "GB", "SEA", "CHI", "SF", "LAR", "WSH", "MIN", "TB", "ATL", "DAL", "CAR", "ARI", "NO", "NYG"};
  static const uint8_t afcWins[] = {5, 5, 4, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 0}, nfcWins[] = {6, 5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 2, 2, 1, 1};
  for (int i = 0; i < 16; i++) {
    strlcpy(s.afc.rows[i].abbr, afc[i], 5); s.afc.rows[i].wins = afcWins[i]; s.afc.rows[i].losses = 7 - afcWins[i]; s.afc.rows[i].seed = i + 1; strlcpy(s.afc.rows[i].streak, i % 3 ? "W2" : "L1", 4);
    strlcpy(s.nfc.rows[i].abbr, nfc[i], 5); s.nfc.rows[i].wins = nfcWins[i]; s.nfc.rows[i].losses = 7 - nfcWins[i]; s.nfc.rows[i].seed = i + 1; strlcpy(s.nfc.rows[i].streak, i % 2 ? "W3" : "L2", 4);
  }
  s.afc.count = s.nfc.count = 16;
  auto fix = [&](int i, const char *away, const char *home, int64_t start) {
    strlcpy(s.next[i].away, away, 5); strlcpy(s.next[i].home, home, 5); s.next[i].start = (uint32_t)start;
  };
  static const char *const away[] = {"CAR", "BAL", "TEN", "MIA", "NYJ", "CLE", "ATL", "MIN", "TB", "LAR", "IND", "HOU", "CHI", "NO"}, *const home[] = {"GB", "BUF", "CIN", "DEN", "NE", "PIT", "KC", "DET", "SEA", "SF", "JAX", "LAC", "WSH", "NYG"};
  if (variant == 0) {
    for (int i = 0; i < 14; i++) fix(i, away[i], home[i], t0 + (i < 3 ? 0 : i < 13 ? 3 * day : 4 * day) + (i % 5) * 3600);
    s.nextCount = 14;
  } else if (variant == 1) {
    for (int i = 0; i < 4; i++) fix(i, away[i], home[i], t0 + i * day);
    s.nextCount = 4;
  } else {
    for (int i = 0; i < 5; i++) fix(i, away[i], home[i], t0 + (i < 2 ? 0 : 3 * day) + i * 600);
    s.nextCount = 5;
  }
}

void addDemo(Kind kind, int variant, int index, int page, int pages, uint32_t ms) {
  if (cycleCount < kMaxItems) cycle[cycleCount++] = {kind, (uint8_t)index, (uint8_t)page, (uint8_t)pages, (uint16_t)ms, (uint8_t)variant};
}

void buildDemo(ClockTime *time) {
  cycleCount = 0;
  constexpr uint32_t kPage = 6000;
  addDemo(GamePage, 0, 0, 0, 5, kPage);   // live, in the red zone, halftime, coming, final
  addDemo(GamePage, 0, 1, 1, 5, kPage);
  addDemo(GamePage, 0, 2, 2, 5, kPage);
  addDemo(GamePage, 0, 3, 3, 5, kPage);
  addDemo(GamePage, 0, 4, 4, 5, kPage);
  addDemo(ScorePage, 0, 0, 6, 4, kScoreMs + 600);  // a touchdown
  addDemo(ScorePage, 0, 0, 3, 4, kScoreMs + 600);  // a field goal
  addDemo(LinePage, 0, 0, 0, 1, kPage);
  for (int v = 0; v < kDemoVariants; v++) {
    fillDemo(*snap, v);
    addDemo(UpcomingPage, v, 0, 0, 1, upcomingMs(*snap, time, kPage));
  }
  demoFilled = kDemoVariants - 1;
  for (int page = 0, pages = standingsPages(snap->afc); page < pages; page++) addDemo(AfcPage, 0, 0, page, pages, kPage);
  for (int page = 0, pages = standingsPages(snap->nfc); page < pages; page++) addDemo(NfcPage, 0, 0, page, pages, kPage);
  for (int scene = 0; scene < 3; scene++) addDemo(ClockPage, 0, 0, scene, 3, kSceneMs);
  cycleMs = 0;
  for (int i = 0; i < cycleCount; i++) cycleMs += cycle[i].ms;
  demoBuilt = true;
}

void stopDemo() {
  if (!demo) return;
  demo = demoBuilt = false;
  delete snap;
  snap = realSnap;
  realSnap = nullptr;
  cycleCount = 0;
  seenVersion = 0;
  haveFrame = false;
  haveShown = false;
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA *display) {
  _display = display;
  Locator::provide(display);  // the startup logo and status screens draw through the Locator
}

void Clockface::invalidate() { haveFrame = false; }

// 1 starts the demo (every screen once, from made-up data), 0 stops it
void Clockface::steer(int dir) {
  if (dir == 0) { stopDemo(); return; }
  if (demo || !snap) return;
  nfl::Snapshot *own = new (std::nothrow) nfl::Snapshot;
  if (!own) return;
  realSnap = snap;
  snap = own;
  demo = true;
  demoBuilt = false;
  demoStart = millis();
  demoFilled = 0xFF;
  scored.active = false;
  cycleCount = 0;
  haveFrame = false;
  haveShown = false;
}

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  haveFrame = false;
  seenVersion = 0;
  haveShown = false;
  scored.active = false;
  if (!snap) snap = new nfl::Snapshot;
  if (!ticker) ticker = new nfl::Ticker;
  ticker->begin();
  ticker->resume();
}

void Clockface::update() {
  const uint32_t now = millis();
  const bool moving = haveShown && !sameItem(shown, sliding) && now - slideStart < kSlideMs;
  const uint32_t at = demo ? now - demoStart : now;  // the demo runs from its own start
  const bool listPage = cycleCount && current(at).kind == UpcomingPage;  // the lists slide: smoother
  const bool clockPage = !cycleCount || current(at).kind == ClockPage;  // the ball moves: as smooth as the lists
  const uint32_t interval = scored.active || moving || listPage || clockPage ? 40 : 90;
  if (haveFrame && now - drawnAt < interval) return;
  ticker->resume();  // after an update check the feed task is gone

  const Settings &settings = *Settings::getInstance();
  const char *favourites = settings.nflTeams.c_str();
  const uint16_t seconds = settings.matchSecs < 3 ? 3 : settings.matchSecs > 60 ? 60 : settings.matchSecs;
  const bool fresh = !demo && ticker->version() != seenVersion;
  if (demo && !demoBuilt) buildDemo(_dateTime);
  if (demo && at >= cycleMs) { stopDemo(); return; }  // once through, then the real data again
  if (!demo && (fresh || strcmp(cycleFavourites, favourites) || seconds != cycleSecs || settings.resultMins != cycleKeep || settings.nflTables != cycleTables || settings.nflIdle != cycleIdle || settings.nflLiveOnly != cycleLiveOnly || settings.nflExtra != cycleExtra)) {
    ticker->snapshot(*snap);
    seenVersion = snap->version;
    applyResultWindow(_dateTime, settings.resultMins);
    trimFixtures(_dateTime);
    if (fresh) {
      noticeScores(now, favourites);
      remember();
    }
    strlcpy(cycleFavourites, favourites, sizeof(cycleFavourites));
    cycleSecs = seconds;
    cycleKeep = settings.resultMins;
    cycleTables = settings.nflTables;
    cycleIdle = settings.nflIdle;
    cycleLiveOnly = settings.nflLiveOnly;
    cycleExtra = settings.nflExtra;
    build(favourites, _dateTime, seconds * 1000, settings.nflTables, settings.nflLiveOnly, settings.nflExtra, settings.nflIdle);
  }
  if (!demo && !cycleCount) build(favourites, _dateTime, seconds * 1000, settings.nflTables, settings.nflLiveOnly, settings.nflExtra, settings.nflIdle);

  haveFrame = true;
  drawnAt = now;
  _display->fillScreen(0);
  if (scored.active) {
    if (now - scored.start < kScoreMs) {
      drawScore(*_display, scored.game, scored.home, scored.points, now - scored.start);
      _display->flipDMABuffer();
      return;
    }
    scored.active = false;
  }
  const Item &item = current(at);
  if (!haveShown) { shown = item; sliding = item; haveShown = true; }
  if (!sameItem(item, shown)) {  // the next page slides in over the one that was on
    sliding = shown;
    shown = item;
    slideStart = now;
  } else {
    shown = item;  // the same page (the clock's next scene, say): no slide
  }
  const uint32_t slid = now - slideStart;
  if (slid < kSlideMs && !sameItem(shown, sliding)) {
    const int dx = (int)(64.0f * (1 - (1 - slid / (float)kSlideMs) * (1 - slid / (float)kSlideMs)));  // eases out
    const bool sameBackground = backgroundOf(shown.kind) == backgroundOf(sliding.kind);
    if (sameBackground) {  // what is behind both stays where it is; only the pages slide
      drawBackground(*_display, backgroundOf(shown.kind), now);
      backgroundDrawnByFace(true);
    }
    facekit::view.dx = -dx;
    render(*_display, sliding, _dateTime, now, favourites);
    facekit::view.dx = 64 - dx;
    render(*_display, shown, _dateTime, now, favourites);
    facekit::view.dx = 0;
    backgroundDrawnByFace(false);
  } else {
    render(*_display, shown, _dateTime, now, favourites);
  }
  _display->flipDMABuffer();
}
