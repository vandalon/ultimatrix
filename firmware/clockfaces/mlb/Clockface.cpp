// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// MLB as a round of pages, much like the NBA face:
//  - Games on: the live games one by one (favourites first) with the bases, count and outs, then the line score of the first and today's list.
//  - Nothing live: the favourites' games, today's list, the coming days and the two league tables, with the clock in between.
//  - Without any game today: the clock with a ball thrown into a catcher's mitt, then the coming days and the tables.
//  A run in a live game plays a short animation (the ball over the fence) before the page returns; the row that scored lights up.
//  Pages slide over each other.

#include "Clockface.h"
#include "MlbRenderer.h"
#include "MlbTicker.h"
#include <FaceKit.h>
#include <Locator.h>
#include <Settings.h>
#include <new>
#include <string.h>

using namespace mlbview;

namespace {

enum Kind : uint8_t { GamePage, LinePage, TodayPage, UpcomingPage, AmericanPage, NationalPage, ClockPage, RunPage };  // RunPage only in the demo
struct Item { Kind kind; uint8_t index, page, pages; uint16_t ms; uint8_t variant; };  // variant: which made-up data the demo draws the page from
constexpr int kMaxItems = 28;

constexpr uint32_t kFlashMs = 2500, kSlideMs = 380, kQuietMs = 8000;

mlb::Ticker *ticker = nullptr;  // heap: the face's data must not be reserved by an update-only boot that never loads it
mlb::Snapshot *snap = nullptr;
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

// What the pages before the update showed, for the runs: a game by its teams
struct Score { char away[5], home[5]; int64_t start; uint16_t awayScore, homeScore; bool live; };
Score before[mlb::kMaxGames];
int beforeCount = 0;
uint32_t flashUntil[mlb::kMaxGames][2] = {};

struct Run { bool active = false, home = false; int points = 0; uint32_t start = 0; mlb::Game game; } run;
uint32_t lastRunAt = 0;

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
  int order[mlb::kMaxGames];
  const int count = orderGames(*snap, favourites, order);
  if (snap->live) {
    int shownGames = 0;
    for (int i = 0; i < count && shownGames < 6; i++)
      if (snap->games[order[i]].state == mlb::Live) { add(GamePage, order[i], shownGames, snap->live > 6 ? 6 : snap->live, pageMs); shownGames++; }
    for (int i = 0; i < count; i++)
      if (extra && snap->games[order[i]].state == mlb::Live) { add(LinePage, order[i], 0, 1, pageMs); break; }
  } else {
    int favouriteGames = 0;
    for (int i = 0; extra && i < count && favouriteGames < 2; i++)
      if (hasFavourite(favourites, snap->games[order[i]])) {
        add(GamePage, order[i], favouriteGames, 2, pageMs);
        if (snap->games[order[i]].state == mlb::Final) add(LinePage, order[i], 0, 1, pageMs);
        favouriteGames++;
      }
  }
  if (extra ? !(snap->live && liveOnly) : !snap->live) {
    // what to show while no game is live (Settings > MLB): games in the next days, the scrolling list with its own small clock (idle 0, 2);
    // the big clock page (idle 1, 2) and when nothing is coming
    if (snap->nextCount && idle != 1) add(UpcomingPage, 0, 0, 1, upcomingMs(*snap, time, pageMs));
    if ((!snap->nextCount || idle != 0) && !snap->live) add(ClockPage, 0, 0, 1, pageMs);
  }
  if (extra && !(snap->live && liveOnly) && (tables == 2 || (tables == 1 && !snap->live))) {
    for (int page = 0, pages = standingsPages(snap->american); page < pages; page++) add(AmericanPage, 0, page, pages, pageMs);
    for (int page = 0, pages = standingsPages(snap->national); page < pages; page++) add(NationalPage, 0, page, pages, pageMs);
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

void fillDemo(mlb::Snapshot &s, uint8_t variant);
extern bool demo;
extern uint8_t demoFilled;

void render(MatrixPanel_I2S_DMA &d, const Item &item, ClockTime *time, uint32_t now, const char *favourites) {
  if (demo && item.variant != demoFilled) { fillDemo(*snap, item.variant); demoFilled = item.variant; }
  switch (item.kind) {
    case GamePage: {
      if (item.index >= snap->count) { drawClock(d, time, now, nullptr); break; }
      const mlb::Game &g = snap->games[item.index];
      drawGame(d, g, time, now, now < flashUntil[item.index][0], now < flashUntil[item.index][1], hasFavourite(favourites, g), item.page, item.pages);
      break;
    }
    case LinePage:
      if (item.index < snap->count) drawLineScore(d, snap->games[item.index], time, now);
      break;
    case TodayPage: {
      int order[mlb::kMaxGames];
      const int count = orderGames(*snap, favourites, order);
      drawToday(d, *snap, order, count, favourites, item.page, time, now);
      break;
    }
    case UpcomingPage: {
      const Settings &settings = *Settings::getInstance();
      const uint16_t seconds = settings.matchSecs < 3 ? 3 : settings.matchSecs > 60 ? 60 : settings.matchSecs;
      drawUpcoming(d, *snap, favourites, item.page, time, now, itemElapsed, seconds * 1000);
      break;
    }
    case AmericanPage: drawStandings(d, snap->american, true, favourites, item.page, time, now); break;
    case NationalPage: drawStandings(d, snap->national, false, favourites, item.page, time, now); break;
    case RunPage: drawRun(d, snap->games[0], true, 2, itemElapsed < kRunMs ? itemElapsed : kRunMs); break;
    default: drawClock(d, time, demo ? item.page * 14000u + itemElapsed : now, snap->nextCount ? &snap->next[0] : nullptr);
  }
}

// A higher score than before in a game that was live: a run
void noticeRuns(uint32_t now, const char *favourites) {
  for (int i = 0; i < snap->count; i++) {
    const mlb::Game &g = snap->games[i];
    for (int j = 0; j < beforeCount; j++) {
      const Score &was = before[j];
      if (strcmp(was.away, g.away) || strcmp(was.home, g.home) || was.start != g.start || !was.live || g.state != mlb::Live) continue;
      const int away = g.awayScore - was.awayScore, home = g.homeScore - was.homeScore;
      if (away > 0) flashUntil[i][0] = now + kFlashMs;
      if (home > 0) flashUntil[i][1] = now + kFlashMs;
      const bool watched = !favourites[0] || hasFavourite(favourites, g);
      if ((away > 0 || home > 0) && watched && !run.active && now - lastRunAt > kQuietMs) {
        run.active = true;
        run.home = home > away;
        run.points = run.home ? home : away;
        run.start = now;
        run.game = g;
        lastRunAt = now;
      }
    }
  }
}

// A finished game stays for the "keep results" setting (minutes, 0 = until midnight), counted from when it was first seen over
struct Ended { char away[5], home[5]; int64_t start, at; };
Ended ended[mlb::kMaxGames];
int endedCount = 0;

void applyResultWindow(ClockTime *clock, uint32_t keepMins) {
  const int64_t now = ::time(nullptr);
  Ended next[mlb::kMaxGames];
  int nextCount = 0, kept = 0;
  snap->live = 0;
  for (int i = 0; i < snap->count; i++) {
    const mlb::Game g = snap->games[i];
    if (g.state == mlb::Final) {
      int64_t at = 0;
      for (int j = 0; j < endedCount && !at; j++)
        if (ended[j].start == g.start && !strcmp(ended[j].away, g.away) && !strcmp(ended[j].home, g.home)) at = ended[j].at;
      if (!at) at = now < g.start + 10800 ? now : g.start + 10800;  // already over when first seen: about when it can have ended
      if (nextCount < mlb::kMaxGames) { strlcpy(next[nextCount].away, g.away, 5); strlcpy(next[nextCount].home, g.home, 5); next[nextCount].start = g.start; next[nextCount++].at = at; }
      const bool keep = keepMins ? now - at < (int64_t)keepMins * 60 : clock->utcToLocal((time_t)at) / 86400 == clock->localNow() / 86400;
      if (!keep) continue;
    }
    snap->live += g.state == mlb::Live;
    snap->games[kept++] = g;
  }
  snap->count = kept;
  memcpy(ended, next, sizeof(next));
  endedCount = nextCount;
}

// A day holds up to 15 games: the next-up list shows whole days, as many as fit in 32 games (the next day or two), never part of a day
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
    case TodayPage: return BgToday;
    case UpcomingPage: return BgNext;
    case AmericanPage: return BgAmerican;
    case NationalPage: return BgNational;
    default: return BgClock;
  }
}

void remember() {
  beforeCount = snap->count;
  for (int i = 0; i < snap->count; i++) {
    const mlb::Game &g = snap->games[i];
    strlcpy(before[i].away, g.away, sizeof(before[i].away));
    strlcpy(before[i].home, g.home, sizeof(before[i].home));
    before[i].start = g.start;
    before[i].awayScore = g.awayScore; before[i].homeScore = g.homeScore; before[i].live = g.state == mlb::Live;
  }
}


// ---- the demo: every screen in turn from made-up data (POST /api/mlb/demo?on=1), once through ----
bool demo = false, demoBuilt = false;
uint32_t demoStart = 0;
mlb::Snapshot *realSnap = nullptr;  // the live data while the demo draws from its own snapshot
uint8_t demoFilled = 0xFF;
constexpr uint8_t kDemoVariants = 6;
constexpr uint32_t kSceneMs = 14000;  // the big clock plays its seven scenes of this long

mlb::Game demoGame(const char *away, const char *home, int as, int hs, mlb::State state, int inning, bool bottom, const char *ar, const char *hr, int64_t start) {
  mlb::Game g;
  strlcpy(g.away, away, sizeof(g.away)); strlcpy(g.home, home, sizeof(g.home));
  g.awayScore = as; g.homeScore = hs; g.state = state; g.inning = inning; g.bottom = bottom;
  strlcpy(g.awayRecord, ar, sizeof(g.awayRecord)); strlcpy(g.homeRecord, hr, sizeof(g.homeRecord));
  g.start = start;
  return g;
}

void demoRuns(mlb::Game &g, std::initializer_list<int> away, std::initializer_list<int> home) {
  int i = 0;
  for (int r : away) g.awayRuns[i++] = r;
  g.awayInnings = i;
  i = 0;
  for (int r : home) g.homeRuns[i++] = r;
  g.homeInnings = i;
}

// Variant 0: live games, tables and a busy few days; 1 one game a day; 2 a few games a day; 3 several playoff series; 4 the World Series; 5 two teams and no series
void fillDemo(mlb::Snapshot &s, uint8_t variant) {
  const int64_t t0 = ::time(nullptr) + 7200, day = 86400;
  s = mlb::Snapshot();
  mlb::Game live = demoGame("MIL", "TB", 4, 2, mlb::Live, 7, false, "103-59", "98-64", t0 - 7200);
  demoRuns(live, {0, 1, 0, 2, 0, 1}, {0, 0, 1, 0, 1, 0});
  live.outs = 1; live.balls = 2; live.strikes = 1; live.bases = 5; live.awayHits = 8; live.homeHits = 5; live.homeErrors = 1;
  mlb::Game second = demoGame("LAD", "NYY", 3, 3, mlb::Live, 9, true, "100-62", "94-68", t0 - 7200);
  demoRuns(second, {0, 0, 2, 0, 0, 1, 0, 0, 0}, {1, 0, 0, 0, 0, 2, 0, 0});
  second.outs = 2; second.balls = 3; second.strikes = 2; second.awayHits = 6; second.homeHits = 7; second.homeErrors = 2;
  mlb::Game final = demoGame("BOS", "HOU", 6, 3, mlb::Final, 9, false, "87-75", "81-81", t0 - 14400);
  demoRuns(final, {0, 2, 0, 0, 1, 0, 3, 0, 0}, {1, 0, 0, 2, 0, 0, 0, 0});
  final.awayHits = 11; final.homeHits = 7; final.homeErrors = 1;
  s.games[0] = live; s.games[1] = second; s.games[2] = demoGame("SD", "SF", 0, 0, mlb::Pre, 0, false, "91-71", "65-97", t0 + 3600); s.games[3] = final;
  s.count = 4; s.live = 2;
  static const char *const al[] = {"TB", "CLE", "HOU", "NYY", "BOS", "CHW", "TEX", "BAL", "TOR", "MIN", "DET", "SEA", "KC", "ATH", "LAA"};
  static const char *const nl[] = {"MIL", "LAD", "ATL", "SD", "CHC", "PHI", "ARI", "PIT", "MIA", "STL", "WSH", "CIN", "NYM", "SF", "COL"};
  static const uint8_t alWins[] = {98, 85, 81, 93, 87, 84, 80, 79, 79, 77, 76, 76, 69, 64, 62}, nlWins[] = {103, 100, 94, 91, 89, 88, 86, 82, 80, 77, 77, 75, 74, 65, 58};
  for (int i = 0; i < 15; i++) {
    strlcpy(s.american.rows[i].abbr, al[i], 5); s.american.rows[i].wins = alWins[i]; s.american.rows[i].losses = 162 - alWins[i]; s.american.rows[i].seed = i + 1; strlcpy(s.american.rows[i].streak, i % 3 ? "W2" : "L1", 4);
    strlcpy(s.national.rows[i].abbr, nl[i], 5); s.national.rows[i].wins = nlWins[i]; s.national.rows[i].losses = 162 - nlWins[i]; s.national.rows[i].seed = i + 1; strlcpy(s.national.rows[i].streak, i % 2 ? "W3" : "L2", 4);
  }
  s.american.count = s.national.count = 15;
  auto fix = [&](int i, const char *away, const char *home, int64_t start) {
    strlcpy(s.next[i].away, away, 5); strlcpy(s.next[i].home, home, 5); s.next[i].start = (uint32_t)start;
  };
  static const char *const away[] = {"MIL", "BOS", "SD", "CHC", "TOR", "SEA", "CHW", "ARI", "ATH", "PIT", "MIN", "WSH"}, *const home[] = {"TB", "NYY", "LAD", "STL", "BAL", "TEX", "CLE", "COL", "KC", "CIN", "DET", "MIA"};
  if (variant == 0) {
    for (int i = 0; i < 12; i++) fix(i, away[i], home[i], t0 + (i < 5 ? 0 : i < 9 ? day : 2 * day) + (i % 5) * 3000);
    s.nextCount = 12;
  } else if (variant == 1) {
    for (int i = 0; i < 4; i++) fix(i, away[i], home[i], t0 + i * day);
    s.nextCount = 4;
  } else if (variant == 2) {
    for (int i = 0; i < 9; i++) fix(i, away[i], home[i], t0 + (i < 3 ? 0 : i < 5 ? day : 2 * day) + i * 600);
    s.nextCount = 9;
  } else if (variant == 3) {
    mlb::Series a, b;
    strcpy(a.round, "NLDS"); strcpy(a.first, "LAD"); strcpy(a.second, "SD"); a.firstColor = 0x39FF; a.secondColor = 0xFFE0; a.firstWins = 1; a.secondWins = 1; a.bestOf = 5; a.game = 3;
    strcpy(b.round, "ALDS"); strcpy(b.first, "NYY"); strcpy(b.second, "BOS"); b.firstColor = 0x9CF3; b.secondColor = 0xF800; b.firstWins = 2; b.secondWins = 0; b.bestOf = 5; b.game = 3;
    s.series[0] = a; s.series[1] = b; s.seriesCount = 2;
    fix(0, "SD", "LAD", t0); fix(1, "BOS", "NYY", t0 + 3000); fix(2, "LAD", "SD", t0 + day); fix(3, "NYY", "BOS", t0 + day + 3000); fix(4, "SD", "LAD", t0 + 2 * day);
    s.nextCount = 5;
  } else {
    for (int i = 0; i < (variant == 4 ? 4 : 3); i++) fix(i, i % 2 ? "LAD" : "NYY", i % 2 ? "NYY" : "LAD", t0 + i * day);
    s.nextCount = variant == 4 ? 4 : 3;
    if (variant == 4) {
      mlb::Series w;
      strcpy(w.round, "WORLD"); strcpy(w.first, "LAD"); strcpy(w.second, "NYY"); w.firstColor = 0x39FF; w.secondColor = 0x9CF3; w.firstWins = 2; w.secondWins = 1; w.bestOf = 7; w.game = 4;
      s.series[0] = w; s.seriesCount = 1;
    }
  }
}

void addDemo(Kind kind, int variant, int index, int page, int pages, uint32_t ms) {
  if (cycleCount < kMaxItems) cycle[cycleCount++] = {kind, (uint8_t)index, (uint8_t)page, (uint8_t)pages, (uint16_t)ms, (uint8_t)variant};
}

void buildDemo(ClockTime *time) {
  cycleCount = 0;
  constexpr uint32_t kPage = 6000;
  addDemo(GamePage, 0, 0, 0, 2, kPage);
  addDemo(GamePage, 0, 1, 1, 2, kPage);
  addDemo(RunPage, 0, 0, 0, 1, kRunMs + 800);
  addDemo(LinePage, 0, 0, 0, 1, kPage);
  for (int v = 0; v < kDemoVariants; v++) {
    fillDemo(*snap, v);
    addDemo(UpcomingPage, v, 0, 0, 1, upcomingMs(*snap, time, kPage));
  }
  demoFilled = kDemoVariants - 1;
  for (int page = 0, pages = standingsPages(snap->american); page < pages; page++) addDemo(AmericanPage, 0, 0, page, pages, kPage);
  for (int page = 0, pages = standingsPages(snap->national); page < pages; page++) addDemo(NationalPage, 0, 0, page, pages, kPage);
  for (int scene = 0; scene < 7; scene++) addDemo(ClockPage, 0, 0, scene, 7, kSceneMs);
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
  mlb::Snapshot *own = new (std::nothrow) mlb::Snapshot;
  if (!own) return;
  realSnap = snap;
  snap = own;
  demo = true;
  demoBuilt = false;
  demoStart = millis();
  demoFilled = 0xFF;
  run.active = false;
  cycleCount = 0;
  haveFrame = false;
  haveShown = false;
}

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  haveFrame = false;
  seenVersion = 0;
  haveShown = false;
  run.active = false;
  if (!snap) snap = new mlb::Snapshot;
  if (!ticker) ticker = new mlb::Ticker;
  ticker->begin();
  ticker->resume();
}

void Clockface::update() {
  const uint32_t now = millis();
  const bool moving = haveShown && !sameItem(shown, sliding) && now - slideStart < kSlideMs;
  const uint32_t at = demo ? now - demoStart : now;  // the demo runs from its own start
  const bool listPage = cycleCount && current(at).kind == UpcomingPage;  // the lists slide: smoother
  const bool clockPage = !cycleCount || current(at).kind == ClockPage;  // the ball moves: as smooth as the lists
  const uint32_t interval = run.active || moving || listPage || clockPage ? 40 : 90;
  if (haveFrame && now - drawnAt < interval) return;
  ticker->resume();  // after an update check the feed task is gone

  const Settings &settings = *Settings::getInstance();
  const char *favourites = settings.mlbTeams.c_str();
  const uint16_t seconds = settings.matchSecs < 3 ? 3 : settings.matchSecs > 60 ? 60 : settings.matchSecs;
  const bool fresh = !demo && ticker->version() != seenVersion;
  if (demo && !demoBuilt) buildDemo(_dateTime);
  if (demo && at >= cycleMs) { stopDemo(); return; }  // once through, then the real data again
  if (!demo && (fresh || strcmp(cycleFavourites, favourites) || seconds != cycleSecs || settings.resultMins != cycleKeep || settings.mlbTables != cycleTables || settings.mlbIdle != cycleIdle || settings.mlbLiveOnly != cycleLiveOnly || settings.mlbExtra != cycleExtra)) {
    ticker->snapshot(*snap);
    seenVersion = snap->version;
    applyResultWindow(_dateTime, settings.resultMins);
    trimFixtures(_dateTime);
    if (fresh) {
      noticeRuns(now, favourites);
      remember();
    }
    strlcpy(cycleFavourites, favourites, sizeof(cycleFavourites));
    cycleSecs = seconds;
    cycleKeep = settings.resultMins;
    cycleTables = settings.mlbTables;
    cycleIdle = settings.mlbIdle;
    cycleLiveOnly = settings.mlbLiveOnly;
    cycleExtra = settings.mlbExtra;
    build(favourites, _dateTime, seconds * 1000, settings.mlbTables, settings.mlbLiveOnly, settings.mlbExtra, settings.mlbIdle);
  }
  if (!demo && !cycleCount) build(favourites, _dateTime, seconds * 1000, settings.mlbTables, settings.mlbLiveOnly, settings.mlbExtra, settings.mlbIdle);

  haveFrame = true;
  drawnAt = now;
  _display->fillScreen(0);
  if (run.active) {
    if (now - run.start < kRunMs) {
      drawRun(*_display, run.game, run.home, run.points, now - run.start);
      _display->flipDMABuffer();
      return;
    }
    run.active = false;
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

