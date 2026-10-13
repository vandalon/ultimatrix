// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <PanelLayout.h>
#include <ScreenTimer.h>
#include "F1Ticker.h"
#include <Locator.h>
#include <TimeService.h>
#include <stdio.h>
#include <string.h>
#include <Settings.h>
#include <Console.h>
#include <esp_heap_caps.h>
#include "F1Renderer.h"
#include "F1Screens.h"

using namespace f1view;

static F1Ticker f1Ticker;
static uint32_t snapVersion = 0;
static bool snapKnown = false;
static F1Screens screens;
static unsigned long snapSecond = 0;
static bool snapIntervals = false;
static int simScenario = 0;  // the telnet simulator's screen, 0 when off

static const char *const SIM_NAMES[] = {"off", "idle", "between sessions", "race", "safety car", "red flag", "qualifying",
                                        "practice", "full results", "qualifying Q2", "qualifying Q3", "chequered flag"};
static const int SIM_COUNT = 12;
static const char *const SIM_GRID[] = {"ANT", "RUS", "HAM", "LEC", "NOR", "PIA", "VER", "ALO", "GAS", "HAD", "STR", "COL",
                                       "ALB", "SAI", "LAW", "LIN", "HUL", "BOR", "OCO", "BEA", "PER", "BOT"};

static void simFill(F1Ticker::Session &s, const char *name, char tag, bool live) {
  s.valid = true;
  s.live = live;
  s.tag = tag;
  strlcpy(s.name, name, sizeof(s.name));
  s.count = F1Ticker::ROWS;
  static const char *const GAPS[] = {"+.088", "+.201", "+.317", "+.455", "+.620", "+.742", "+.911", "+1.030", "+1.2", "+1.5", "+1.9",
                                     "+2.3", "+2.8", "+3.1", "+3.6", "+4.0", "+4.4", "+5.1", "+5.9", "+6.5", "+7.4"};
  for (int i = 0; i < F1Ticker::ROWS; i++) {
    // every 7 seconds the order changes: a swap, then a driver dropping nine places, then back, to see the markers
    int grid = i;
    int phase = (millis() / 7000) % 3;
    if (live && phase == 1) grid = i == 3 ? 4 : i == 4 ? 3 : i;
    if (live && phase == 2) grid = i == 11 ? 2 : (i >= 2 && i < 11) ? i + 1 : i;  // the third driver drops to twelfth
    strlcpy(s.rows[i].code, SIM_GRID[grid], sizeof(s.rows[i].code));
    s.rows[i].color = F1Ticker::driverColor(SIM_GRID[grid]);
    s.rows[i].fastest = (tag == 'R' || tag == 'S') && strcmp(SIM_GRID[grid], "VER") == 0;
    s.rows[i].improved = live && tag == 'Q' && (millis() / 3000) % 2 == 0 && (i == 4 || i == 8);  // a better lap just now: the time lights up
    if (i == 0) strlcpy(s.rows[0].time, tag == 'R' || tag == 'S' ? "LEAD" : "1:29.412", sizeof(s.rows[0].time));
    else strlcpy(s.rows[i].time, GAPS[i - 1], sizeof(s.rows[i].time));
  }
}

static void simSnapshot(F1Ticker::Snapshot &s) {
  s = F1Ticker::Snapshot();
  if (simScenario == 11) return;
  const int points[] = {372, 351, 289};
  for (int i = 0; i < 22; i++) {
    strlcpy(s.standings[i].code, SIM_GRID[i], sizeof(s.standings[i].code));
    s.standings[i].color = F1Ticker::driverColor(SIM_GRID[i]);
    s.standings[i].points = i < 3 ? points[i] : 250 - i * 10;
  }
  s.standingsCount = 22;
  strlcpy(s.circuit, "Marina Bay Street Circuit Singapore", sizeof(s.circuit));
  strlcpy(s.upcoming[0].city, "MEXICO CITY", sizeof(s.upcoming[0].city));
  strlcpy(s.upcoming[0].circuit, "Marina Bay Street Circuit", sizeof(s.upcoming[0].circuit));  // the only outline there is yet
  s.upcoming[0].start = ultime::now() + 17 * SECS_PER_DAY;
  s.upcomingCount = 1;
  switch (simScenario) {
    case 2:
      s.weekend = true;
      simFill(s.last, "QUALI", 'Q', false);
      simFill(s.next, "SPRINT", 'S', false);
      s.next.start = ultime::now() + 20 * 3600;
      strlcpy(s.coming[0].name, "SPRINT", sizeof(s.coming[0].name));
      s.coming[0].start = s.next.start;
      strlcpy(s.coming[1].name, "RACE", sizeof(s.coming[1].name));
      s.coming[1].start = s.next.start + 24 * 3600;
      s.comingCount = 2;
      break;
    case 8:
      s.weekend = true;
      simFill(s.last, "SQ", 'Q', false);
      simFill(s.next, "SPRINT", 'S', false);
      s.next.start = ultime::now() + 20 * 3600;
      strlcpy(s.coming[0].name, "SPRINT", sizeof(s.coming[0].name));
      s.coming[0].start = s.next.start;
      strlcpy(s.coming[1].name, "RACE", sizeof(s.coming[1].name));
      s.coming[1].start = s.next.start + 24 * 3600;
      s.comingCount = 2;
      break;
    case 3:
    case 4:
    case 5:
      s.weekend = true;
      simFill(s.live, "RACE", 'R', true);
      s.live.period = 32;
      s.live.totalLaps = 57;  // without it the right side reads "L32/..." and the red flag label does not fit in the larger font
      s.live.flag = simScenario == 4 ? 's' : simScenario == 5 ? 'r' : 0;
      s.live.finished = (millis() / 7000) % 3 == 2;  // the chequered flag for the last seconds of each round
      break;
    case 6:
      s.weekend = true;
      simFill(s.live, "QUALI", 'Q', true);
      strlcpy(s.live.clock, "4:12", sizeof(s.live.clock));
      break;
    case 9:  // the list holds all 22: only the 16 (Q2) or 10 (Q3) still in are shown
    case 10:
      s.weekend = true;
      simFill(s.live, simScenario == 9 ? "Q2" : "Q3", 'Q', true);
      strlcpy(s.live.clock, simScenario == 9 ? "8:20" : "6:45", sizeof(s.live.clock));
      break;
    case 7:
      s.weekend = true;
      simFill(s.live, "FP2", 'P', true);
      strlcpy(s.live.clock, (millis() / 7000) % 3 == 2 ? "0:00" : "23:10", sizeof(s.live.clock));
      break;
  }
}

Clockface::Clockface(MatrixPanel_I2S_DMA* display) {
  _display = display;
  Locator::provide(display);  // the startup logo and status screens draw through the Locator
}

void Clockface::invalidate() { screens.reset(); }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  screens.reset();  // whatever was drawn over the face (notification, birthday) must be redrawn
  f1Ticker.begin(dateTime);
}

const char *Clockface::simulateNext() {
  simScenario = (simScenario + 1) % SIM_COUNT;
  screens.reset();
  return SIM_NAMES[simScenario];
}

const char *Clockface::simulate() { return simulateNext(); }

// What the face shows, to the serial log and telnet: every 15 s and whenever the screen changes, so a session can be
// followed without seeing the panel
static void logShown(int view, int page) {
  static const char *VIEWS[] = {"LIVE", "LIST", "BETWEEN", "IDLE"};
  static unsigned long at = 0;
  static int lastView = -1;
  static char lastState[160] = "";
  char state[160];
  const F1Ticker::Session &l = snap.live;
  const F1Ticker::Session &n = snap.next;
  snprintf(state, sizeof(state), "%d|%s|%c%c%d|%s|%c|%d", view, l.name, l.flag ? l.flag : '-', l.finished ? 'F' : '-', l.count,
           n.name, n.status ? n.status : '-', snap.comingCount);
  bool changed = view != lastView || strcmp(state, lastState) != 0;
  if (!changed && millis() - at < 15000) return;
  at = millis();
  lastView = (int)view;
  strlcpy(lastState, state, sizeof(lastState));
  char line[320];
  int n_ = snprintf(line, sizeof(line), "[face] %s page %d, heap %u/%u", VIEWS[view], page,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (l.valid) {
    char top[120] = "";
    for (int i = 0; i < 3 && i < l.count; i++) {
      char shown[9];
      strlcpy(shown, l.rows[i].time, sizeof(shown));
      if (i == 0 && (l.tag == 'R' || l.tag == 'S') && l.finished) strlcpy(shown, "FINISHED", sizeof(shown));
      size_t used = strlen(top);
      snprintf(top + used, sizeof(top) - used, " %s %s", l.rows[i].code, shown);
    }
    n_ += snprintf(line + n_, sizeof(line) - n_, " | live %s flag %c(shown %c) finished %d lap %d/%d clock %s rows %d:%s", l.name,
                   l.flag ? l.flag : '-', shownFlag(l) ? shownFlag(l) : '-', l.finished, l.period, l.totalLaps, l.clock, l.count, top);
  }
  if (snap.last.valid) n_ += snprintf(line + n_, sizeof(line) - n_, " | last %s", snap.last.name);
  if (snap.next.valid || snap.comingCount) {
    n_ += snprintf(line + n_, sizeof(line) - n_, " | next");
    for (int i = 0; i < snap.comingCount && n_ < (int)sizeof(line) - 24; i++) {
      const F1Ticker::Snapshot::Coming &c = snap.coming[i];
      n_ += snprintf(line + n_, sizeof(line) - n_, " %s@%ld%s", c.name, (long)(c.start - ultime::now()) / 60,
                     c.status == 'd' ? "m DELAYED" : c.status == 'n' ? "m NEWTIME" : "m");
    }
  }
  if (view == 2 && snap.comingCount) {  // the bottom row of the between-sessions screen
    int i = comingIndex();
    n_ += snprintf(line + n_, sizeof(line) - n_, " | row %d/%d %s%s", i + 1, comingCount(), snap.coming[i].name,
                   snap.coming[i].status == 'd' ? " DELAYED(orange)" : snap.coming[i].status == 'n' ? " new time(cyan)" : "");
  }
  snprintf(line + n_, sizeof(line) - n_, "\n");
  Serial.print(line);
  Console.print(line);
}

static void drawLoadingScreen(MatrixPanel_I2S_DMA *d) {
  unsigned long frame = millis() / 60;
  int height = shortPanel() ? 32 : 64;
  if (!screens.loadingChanged(frame, height)) return;
  d->fillScreen(0);
  int width = 56, flagHeight = shortPanel() ? 21 : 42;
  int left = (W - width) / 2, top = (height - flagHeight) / 2;
  int check = 7;
  float phase = (frame % 50) * (2 * PI / 50);
  for (int x = 0; x < width; x++) {
    // One gentle wave across the flag keeps the motion centred on the panel.
    int ripple = (int)roundf(sinf(x * 2 * PI / width - phase) * 2);
    for (int y = 0; y < flagHeight; y++) {
      bool edge = y == 0 || y == flagHeight - 1 || x == 0 || x == width - 1;
      bool white = ((x / check + y / check) % 2) == 0;
      d->drawPixel(left + x, top + y + ripple, edge || white ? WHITE : 0);
    }
  }
  d->flipDMABuffer();
}

static bool hasDisplayData() {
  if (snap.live.valid || (snap.weekend && snap.last.valid) || snap.standingsCount) return true;
  for (int i = 0; i < snap.upcomingCount; i++)
    if (snap.upcoming[i].city[0] && snap.upcoming[i].start > ultime::now()) return true;
  return false;
}

void Clockface::update() {
  static bool syncingTime = false;
  if (ultime::timeStatus() != timeSet) {
    if (!syncingTime) {
      Serial.println("[face] syncing time");
      Console.println("[face] syncing time");
      syncingTime = true;
    }
    drawLoadingScreen(_display);
    return;
  }
  if (syncingTime) {
    Serial.println("[face] time synced");
    Console.println("[face] time synced");
    syncingTime = false;
  }
  int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  bool intervals = (millis() / pageMs()) % 2 != 0;  // alternate at the results-page setting
  uint32_t version = f1Ticker.version();
  if (simScenario) {
    simSnapshot(snap);
    snapVersion = version - 1;  // the real data comes back when the simulator stops
    version = 0x80000000UL + simScenario + ((millis() / 7000) % 3) * 16;  // the sim order changes every 7 s
  } else if (!snapKnown || version != snapVersion || intervals != snapIntervals || (snap.live.valid && millis() / 1000 != snapSecond)) {
    f1Ticker.snapshot(snap, intervals);  // while live also every second, for the countdown
    snapIntervals = intervals;
    snapVersion = version;
    snapKnown = true;
    snapSecond = millis() / 1000;
  }
  if (!hasDisplayData()) {
    drawLoadingScreen(_display);
    return;
  }

  F1Screen view;
  int page;
  ScreenInputs in = {_dateTime, hour, minute, version, intervals, simScenario};
  bool draw = screens.advance(in, view, page);
  logShown((int)view, page);
  if (!draw) return;

  marqueeMax = 0;  // set again by a text that scrolls
  dotShift = (view == LIVE && timesFailed(snap.live)) || (view == LIST && timesFailed(snap.last)) ? 3 : 0;
  _display->fillScreen(0);
  backdropShown = false;  // until the view draws it
  tileRows = 0;
  loadingDotsShown = false;
  switch (view) {
    case LIVE: drawLive(_display, snap.live, hour, minute, page); break;
    case LIST: drawResultList(_display, snap.last, hour, minute, page); break;
    case BETWEEN: drawBetween(_display, _dateTime, hour, minute, page); break;
    case IDLE: drawIdle(_display, _dateTime, hour, minute, page); break;
  }
  _display->flipDMABuffer();
}
