// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Football as two clocks, depending on what is on:
//  - Live: a green bar with the competition and game time left and the time right, both teams as
//    shirts either side of the score, and a timeline of the playing time with the goals (balls) and
//    cards (stripes) of the home team above it and of the away team below. Under it the matches that
//    finished, or the other live matches. With several live matches the main view takes them one by
//    one; with a single match and nothing else the time moves to the bottom, with seconds. Without a
//    live match the same view shows the results: a favourite's, else the latest, on top (FT) and the
//    other results and today's coming matches in the rows below. A card or substitution of a live match
//    plays a full screen animation first (playIncident).
//  - Main (no matches at all): flip clock tiles, hours over minutes, with a ball rolling (or resting) under
//    them on a stadium band at the bottom, and the next kick-offs on the right. Without those either: HH:MM on flip clock tiles over a stadium
//    band, the ball passed to and fro along its grass.

#include "Clockface.h"
#include "FootballRenderer.h"
#include "FootballScreens.h"
#include "FootballTicker.h"
#include "GoalAnimation.h"
#include "TeamColors.h"
#include <Settings.h>
#include <Console.h>
#include <TimeService.h>
#include <Locator.h>
#ifdef ESP_PLATFORM
#include <NetworkBudget.h>
#include <esp_heap_caps.h>
#endif
#include <math.h>
#include <functional>

using namespace fbview;

// On the heap, created by the face: its 10 KB must not be reserved by an update-only boot that never loads the face
static FootballTicker *footballTicker = nullptr;
static uint32_t overviewVersion = 0;

// How long finished matches stay on (the setting is in minutes), 0 = until midnight
static uint32_t resultWindowSecs() { return Settings::getInstance()->resultMins * 60UL; }

static FootballScreens screens;

// A goal or an incident plays one frame per update() while it lasts, so the clock's background work keeps running
static struct {
  enum Kind { None, Goal, Incident } kind = None;
  unsigned long start = 0;
  int shown = -1, total = 0;
  GoalAnimation goal;
  FootballTicker::Incident incident;
} animation;
static Frame lastFrame;  // what was drawn last, see update()
static bool haveFrame = false;
static bool snapshotReleased = false;

static void showResultsLive(Overview &o, unsigned long now) {
  if (!o.live.empty() && o.live[0].live) {
    resultsOnTop = false;
    shownResult = SIZE_MAX;
    return;
  }
  size_t count = finishedCount();
  if (!count) {
    o.live.clear();
    resultsOnTop = false;
    shownResult = SIZE_MAX;
    return;
  }
  uint16_t favourites = favouriteResults();
  size_t choices = 0;
  for (size_t i = 0; i < count; i++) choices += !favourites || (favourites & (1U << i));
  size_t turn = now / (max(3, (int)Settings::getInstance()->matchSecs) * 1000UL) % choices;
  size_t selected = 0;
  for (; selected < count; selected++)
    if ((!favourites || (favourites & (1U << selected))) && turn-- == 0) break;
  static uint32_t resultVersion = 0;
  if (selected != shownResult || resultVersion != overviewRevision || o.live.empty()) {
    MatchRow result;
    if (!readResult(selected, result)) return;
    o.live.clear();
    o.live.push_back(result);
    shownResult = selected;
    resultVersion = overviewRevision;
  }
  resultsOnTop = true;
}

Clockface::Clockface(MatrixPanel_I2S_DMA* display) {
  _display = display;
  Locator::provide(display);  // the startup logo and status screens draw through the Locator
}

void Clockface::invalidate() { haveFrame = false; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  haveFrame = false;  // whatever was drawn over the face (notification, birthday) must be redrawn
  if (!footballTicker) { footballTicker = new FootballTicker; }
  (void)snap();  // allocated right behind the ticker, so the two sit together and the free space after them stays in one piece
  footballTicker->begin(dateTime, false, true, resultWindowSecs());  // goals, cards, tables, kick-offs

#ifdef ESP_PLATFORM
  feedTrimFn = [](void *self) {
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    static_cast<FootballTicker *>(self)->trim();
    releaseSnapshot();
    snapshotReleased = true;
    animation.kind = animation.None;
    haveFrame = false;
    consoleLog("[Football] WLED trim: free %u -> %u, largest %u -> %u, snapshot %u bytes each\n",
        (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)block, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)sizeof(football::Snapshot));
  };
#endif
}

bool Clockface::animating() const { return animation.kind != animation.None; }

// Draws and shows the frame that is due, if any; true while the animation owns the screen
static bool stepAnimation(MatrixPanel_I2S_DMA *display, unsigned long now) {
  if (animation.kind == animation.None) return false;
  int frame = min((int)((now - animation.start) / GoalAnimation::FRAME_MS), animation.total - 1);
  if (frame > animation.shown) {
    if (animation.kind == animation.Goal) {
      for (int f = animation.shown + 1; f < frame; f++) animation.goal.drawFrame(f, false);  // behind: catch up its state
      animation.goal.drawFrame(frame, true);
    } else {
      drawIncidentFrame(display, animation.incident, frame);
    }
    display->flipDMABuffer();
    animation.shown = frame;
  }
  if (animation.shown >= animation.total - 1) {
    animation.goal.end();
    animation.kind = animation.None;
    haveFrame = false;
  }
  return true;
}

void Clockface::update() {
  if (snapshotReleased) {
    if (!prepareSnapshot()) return;  // retry once WLED has returned enough heap
    footballTicker->snapshot(snap());
    snapshotReleased = false;
    // Force the live rows and ranking page to be rebuilt even without a new feed version.
    overviewVersion = snap().version - 1;
    screens.resetRankingPage();
  }
  unsigned long now = millis();
  if (stepAnimation(_display, now)) return;
  int hour = _dateTime->getHour(), minute = _dateTime->getMinute(), second = _dateTime->getSecond();
  static int shownFont = -1;
  int numberFont = Settings::getInstance()->numberFont;
  if (numberFont != shownFont) {
    shownFont = numberFont;
    haveFrame = false;
  }
  static int shownRankings = -1;
  bool showRankings = Settings::getInstance()->showRankings;
  if (showRankings != (shownRankings != 0)) {
    shownRankings = showRankings;
    screens.resetRankingPage();
    haveFrame = false;
  }

  footballTicker->setResultWindow(resultWindowSecs());
  uint32_t version = footballTicker->version();
  if (version != overviewVersion) {
    footballTicker->snapshot(snap());
    overview.live.clear();
    for (size_t i = 0; i < snap().liveCount; i++) overview.live.push_back(liveMatch(snap().live[i]));
    overviewVersion = version;
    overviewRevision++;
    resultsOnTop = false;
  }
  showResultsLive(overview, now);

  FootballTicker::Status status = footballTicker->status();
  screens.advance({_dateTime, hour, minute, second, now, version, showRankings, status});

  // A goal: the net cam animation with GOAL! and the new score, then the clock again
  FootballTicker::Goal goal;
  if (footballTicker->nextGoal(goal)) {
    if (animation.goal.begin(_display, goal.home.c_str(), goal.away.c_str(), goal.homeScore, goal.awayScore, goal.homeScored,
                             goal.homeKit, goal.awayKit)) {
      animation.kind = animation.Goal;
      animation.total = animation.goal.frames();
      animation.start = now;
      animation.shown = -1;
      stepAnimation(_display, now);
    }
    haveFrame = false;
    return;
  }

  // A card or a substitution: the full screen animation, then the clock again
  FootballTicker::Incident incident;
  if (footballTicker->nextIncident(incident)) {
    animation.incident = incident;
    animation.kind = animation.Incident;
    animation.total = incidentFrames(incident);
    animation.start = now;
    animation.shown = -1;
    stepAnimation(_display, now);
    haveFrame = false;
    return;
  }

  screens.finish();

  // Only draw when something changed
  const Frame &frame = screens.frame();
  if (haveFrame && memcmp(&frame, &lastFrame, sizeof(Frame)) == 0) return;
  lastFrame = frame;
  haveFrame = true;

  _display->fillScreen(0);
  switch (screens.view) {
    case View::Live:
      drawStadium(_display, 16, false);
      drawLive(_display, _dateTime, screens.index, screens.pinned, frame.shift, hour, minute, second, frame.pulse, screens.slide, frame.push,
               screens.swapActive ? &screens.liveOld : nullptr, screens.swapT);
      break;
    case View::Fixtures:
      drawMain(_display, _dateTime, hour, minute, second, frame.pulse, frame.turn, showRankings, now, frame.roll);
      break;
    case View::Clock: drawBigClock(_display, _dateTime, second, frame.roll); break;
  }
  drawStatusDot(_display, status, frame.pulse);
  _display->flipDMABuffer();
}
