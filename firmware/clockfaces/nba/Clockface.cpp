// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// NBA as a round of pages, much like Football:
//  - Games on: the live games one by one (favourites first) on a half court, then the quarter scores of the first and today's list.
//  - Nothing live: the favourites' games, today's list, the coming days and the two conference tables, with the clock in between.
//  - Without any game today: the clock with a ball dribbling along the floor, then the coming days and the tables.
//  A basket of a live game plays a short animation (the ball into the net) before the page returns; the row that scored lights up.
//  Pages slide over each other.

#include "Clockface.h"
#include "NbaRenderer.h"
#include "NbaTicker.h"
#include <FaceKit.h>
#include <Locator.h>
#include <Settings.h>
#include <string.h>

using namespace nbaview;

namespace {

enum Kind : uint8_t { GamePage, QuartersPage, TodayPage, UpcomingPage, EastPage, WestPage, ClockPage };
struct Item { Kind kind; uint8_t index, page, pages; uint16_t ms; };
constexpr int kMaxItems = 24;

constexpr uint32_t kFlashMs = 2500, kSlideMs = 380, kQuietMs = 8000;

nba::Ticker *ticker = nullptr;  // heap: the face's data must not be reserved by an update-only boot that never loads it
nba::Snapshot *snap = nullptr;
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

// What the pages before the update showed, for the baskets: a game by its teams
struct Score { char away[5], home[5]; uint16_t awayScore, homeScore; bool live; };
Score before[nba::kMaxGames];
int beforeCount = 0;
uint32_t flashUntil[nba::kMaxGames][2] = {};

struct Basket { bool active = false, home = false; int points = 0; uint32_t start = 0; nba::Game game; } basket;
uint32_t lastBasketAt = 0;

// The page on show and the one sliding out
Item shown{};
Item sliding{};
uint32_t slideStart = 0;
bool haveShown = false;

void add(Kind kind, int index, int page, int pages, uint16_t ms) {
  if (cycleCount < kMaxItems) cycle[cycleCount++] = {kind, (uint8_t)index, (uint8_t)page, (uint8_t)pages, ms};
}

void build(const char *favourites, ClockTime *time, uint16_t pageMs, uint8_t tables, bool liveOnly, bool extra, uint8_t idle) {
  cycleCount = 0;
  int order[nba::kMaxGames];
  const int count = orderGames(*snap, favourites, order);
  if (snap->live) {
    int shownGames = 0;
    for (int i = 0; i < count && shownGames < 6; i++)
      if (snap->games[order[i]].state == nba::Live) { add(GamePage, order[i], shownGames, snap->live > 6 ? 6 : snap->live, pageMs); shownGames++; }
    for (int i = 0; i < count; i++)
      if (extra && snap->games[order[i]].state == nba::Live) { add(QuartersPage, order[i], 0, 1, pageMs); break; }
    if (extra && count > 1 && !liveOnly) for (int page = 0, pages = gamePages(count); page < pages && page < 2; page++) add(TodayPage, 0, page, pages, pageMs);
  } else {
    int favouriteGames = 0;
    for (int i = 0; extra && i < count && favouriteGames < 2; i++)
      if (hasFavourite(favourites, snap->games[order[i]])) {
        add(GamePage, order[i], favouriteGames, 2, pageMs);
        if (snap->games[order[i]].state == nba::Final) add(QuartersPage, order[i], 0, 1, pageMs);
        favouriteGames++;
      }
    for (int page = 0, pages = gamePages(count); extra && page < pages && page < 3; page++) add(TodayPage, 0, page, pages, pageMs);
  }
  if (extra ? !(snap->live && liveOnly) : !snap->live) {
    // what to show while no game is live (Settings > NBA): games within 7 days, the scrolling list with its own small clock (idle 0, 2);
    // the big clock page (idle 1, 2) and when nothing is coming
    if (snap->nextCount && idle != 1) add(UpcomingPage, 0, 0, 1, upcomingMs(*snap, time, pageMs));
    if ((!snap->nextCount || idle != 0) && !snap->live) add(ClockPage, 0, 0, 1, pageMs);
  }
  if (extra && !(snap->live && liveOnly) && (tables == 2 || (tables == 1 && !snap->live))) {
    for (int page = 0, pages = standingsPages(snap->east); page < pages; page++) add(EastPage, 0, page, pages, pageMs);
    for (int page = 0, pages = standingsPages(snap->west); page < pages; page++) add(WestPage, 0, page, pages, pageMs);
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

bool sameItem(const Item &a, const Item &b) { return a.kind == b.kind && a.index == b.index && a.page == b.page; }

void render(MatrixPanel_I2S_DMA &d, const Item &item, ClockTime *time, uint32_t now, const char *favourites) {
  switch (item.kind) {
    case GamePage: {
      if (item.index >= snap->count) { drawClock(d, time, now, nullptr); break; }
      const nba::Game &g = snap->games[item.index];
      drawGame(d, g, time, now, now < flashUntil[item.index][0], now < flashUntil[item.index][1], hasFavourite(favourites, g), item.page, item.pages);
      break;
    }
    case QuartersPage:
      if (item.index < snap->count) drawQuarters(d, snap->games[item.index], time, now);
      break;
    case TodayPage: {
      int order[nba::kMaxGames];
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
    case EastPage: drawStandings(d, snap->east, true, favourites, item.page, time, now); break;
    case WestPage: drawStandings(d, snap->west, false, favourites, item.page, time, now); break;
    default: {
      char line[24] = "";
      if (snap->nextCount) nextGameLine(time, snap->next[0], line, sizeof(line));
      drawClock(d, time, now, line);
    }
  }
}

// A higher score than before in a game that was live: a basket
void noticeBaskets(uint32_t now, const char *favourites) {
  for (int i = 0; i < snap->count; i++) {
    const nba::Game &g = snap->games[i];
    for (int j = 0; j < beforeCount; j++) {
      const Score &was = before[j];
      if (strcmp(was.away, g.away) || strcmp(was.home, g.home) || !was.live || g.state != nba::Live) continue;
      const int away = g.awayScore - was.awayScore, home = g.homeScore - was.homeScore;
      if (away > 0) flashUntil[i][0] = now + kFlashMs;
      if (home > 0) flashUntil[i][1] = now + kFlashMs;
      const bool watched = !favourites[0] || hasFavourite(favourites, g);
      if ((away > 0 || home > 0) && watched && !basket.active && now - lastBasketAt > kQuietMs) {
        basket.active = true;
        basket.home = home > away;
        basket.points = basket.home ? home : away;
        basket.start = now;
        basket.game = g;
        lastBasketAt = now;
      }
    }
  }
}

// A finished game stays for the "keep results" setting (minutes, 0 = until midnight), counted from when it was first seen over
struct Ended { char away[5], home[5]; int64_t start, at; };
Ended ended[nba::kMaxGames];
int endedCount = 0;

void applyResultWindow(ClockTime *clock, uint32_t keepMins) {
  const int64_t now = ::time(nullptr);
  Ended next[nba::kMaxGames];
  int nextCount = 0, kept = 0;
  snap->live = 0;
  for (int i = 0; i < snap->count; i++) {
    const nba::Game g = snap->games[i];
    if (g.state == nba::Final) {
      int64_t at = 0;
      for (int j = 0; j < endedCount && !at; j++)
        if (ended[j].start == g.start && !strcmp(ended[j].away, g.away) && !strcmp(ended[j].home, g.home)) at = ended[j].at;
      if (!at) at = now < g.start + 9000 ? now : g.start + 9000;  // already over when first seen: about when it can have ended
      if (nextCount < nba::kMaxGames) { strlcpy(next[nextCount].away, g.away, 5); strlcpy(next[nextCount].home, g.home, 5); next[nextCount].start = g.start; next[nextCount++].at = at; }
      const bool keep = keepMins ? now - at < (int64_t)keepMins * 60 : clock->utcToLocal((time_t)at) / 86400 == clock->localNow() / 86400;
      if (!keep) continue;
    }
    snap->live += g.state == nba::Live;
    snap->games[kept++] = g;
  }
  snap->count = kept;
  memcpy(ended, next, sizeof(next));
  endedCount = nextCount;
}

Background backgroundOf(Kind kind) {
  switch (kind) {
    case GamePage: return BgGame;
    case QuartersPage: return BgQuarters;
    case TodayPage: return BgToday;
    case UpcomingPage: return BgNext;
    case EastPage: return BgEast;
    case WestPage: return BgWest;
    default: return BgClock;
  }
}

void remember() {
  beforeCount = snap->count;
  for (int i = 0; i < snap->count; i++) {
    const nba::Game &g = snap->games[i];
    strlcpy(before[i].away, g.away, sizeof(before[i].away));
    strlcpy(before[i].home, g.home, sizeof(before[i].home));
    before[i].awayScore = g.awayScore; before[i].homeScore = g.homeScore; before[i].live = g.state == nba::Live;
  }
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA *display) {
  _display = display;
  Locator::provide(display);  // the startup logo and status screens draw through the Locator
}

void Clockface::invalidate() { haveFrame = false; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  haveFrame = false;
  seenVersion = 0;
  haveShown = false;
  basket.active = false;
  if (!snap) snap = new nba::Snapshot;
  if (!ticker) ticker = new nba::Ticker;
  ticker->begin();
  ticker->resume();
}

void Clockface::update() {
  const uint32_t now = millis();
  const bool moving = haveShown && !sameItem(shown, sliding) && now - slideStart < kSlideMs;
  const bool listPage = cycleCount && current(now).kind == UpcomingPage;  // the lists slide: smoother
  const bool clockPage = !cycleCount || current(now).kind == ClockPage;  // the ball moves: as smooth as the lists
  const uint32_t interval = basket.active || moving || listPage || clockPage ? 40 : 90;
  if (haveFrame && now - drawnAt < interval) return;
  ticker->resume();  // after an update check the feed task is gone

  const Settings &settings = *Settings::getInstance();
  const char *favourites = settings.nbaTeams.c_str();
  const uint16_t seconds = settings.matchSecs < 3 ? 3 : settings.matchSecs > 60 ? 60 : settings.matchSecs;
  const bool fresh = ticker->version() != seenVersion;
  if (fresh || strcmp(cycleFavourites, favourites) || seconds != cycleSecs || settings.resultMins != cycleKeep || settings.nbaTables != cycleTables || settings.nbaIdle != cycleIdle || settings.nbaLiveOnly != cycleLiveOnly || settings.nbaExtra != cycleExtra) {
    ticker->snapshot(*snap);
    seenVersion = snap->version;
    applyResultWindow(_dateTime, settings.resultMins);
    if (fresh) {
      noticeBaskets(now, favourites);
      remember();
    }
    strlcpy(cycleFavourites, favourites, sizeof(cycleFavourites));
    cycleSecs = seconds;
    cycleKeep = settings.resultMins;
    cycleTables = settings.nbaTables;
    cycleIdle = settings.nbaIdle;
    cycleLiveOnly = settings.nbaLiveOnly;
    cycleExtra = settings.nbaExtra;
    build(favourites, _dateTime, seconds * 1000, settings.nbaTables, settings.nbaLiveOnly, settings.nbaExtra, settings.nbaIdle);
  }
  if (!cycleCount) build(favourites, _dateTime, seconds * 1000, settings.nbaTables, settings.nbaLiveOnly, settings.nbaExtra, settings.nbaIdle);

  haveFrame = true;
  drawnAt = now;
  _display->fillScreen(0);
  if (basket.active) {
    if (now - basket.start < kBasketMs) {
      drawBasket(*_display, basket.game, basket.home, basket.points, now - basket.start);
      _display->flipDMABuffer();
      return;
    }
    basket.active = false;
  }
  const Item &item = current(now);
  if (!haveShown) { shown = item; sliding = item; haveShown = true; }
  if (!sameItem(item, shown)) {  // the next page slides in over the one that was on
    sliding = shown;
    shown = item;
    slideStart = now;
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

