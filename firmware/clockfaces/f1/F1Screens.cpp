// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "F1Screens.h"
#include <ScreenTimer.h>
#include <Settings.h>
#include <TimeService.h>
#include <string.h>

namespace f1view {

bool F1Screens::loadingChanged(unsigned long frame, int height) {
  Frame next;
  memset(&next, 0, sizeof(next));
  next.view = -1;  // not one of the screens
  next.page = (int32_t)frame;
  next.rowPage = height;
  if (_haveFrame && memcmp(&next, &_frame, sizeof(next)) == 0) return false;
  _frame = next;
  _haveFrame = true;
  return true;
}

// Which screen is on and what changed on it. Returns whether the picture has to be drawn again.
bool F1Screens::advance(const ScreenInputs &in, F1Screen &viewOut, int &pageOut) {
  int hour = in.hour, minute = in.minute;
  // Practice and the last part of qualifying are over when the live feed says so, or 3 minutes after the clock hit 0:00,
  // however long ESPN takes to say it. Races allow 5 minutes after the leader finishes for the rest of the field.
  static uint32_t timeUpId = 0, closedId = 0;
  static unsigned long timeUpAt = 0;
  if (snap.live.valid && snap.live.id) {
    bool race = snap.live.tag == 'R' || snap.live.tag == 'S';
    bool last = snap.live.tag == 'P' || (snap.live.tag == 'Q' && qPart(snap.live) == 3);
    bool timeUp = race ? snap.live.finished && snap.live.finishedAt > 0 : last && (snap.live.finished || !strcmp(snap.live.clock, "0:00"));
    unsigned long timeout = (race ? 5 : 3) * 60 * 1000UL;
    bool close = snap.live.id == closedId || (last && snap.live.fromFeed && snap.live.finished);  // the feed says it is over
    if (!close && timeUp) {
      if (timeUpId != snap.live.id || !timeUpAt) {
        timeUpId = snap.live.id;
        timeUpAt = millis();
        if (race) Serial.printf("[face] %s: leader finished %ld s ago, results 5 minutes after the finish\n", snap.live.name, (long)(ultime::now() - snap.live.finishedAt));
        else Serial.printf("[face] %s: clock at 0:00, results in 3 minutes unless the feed says it is over\n", snap.live.name);
      }
      if (race ? ultime::now() >= snap.live.finishedAt + 5 * 60 : millis() - timeUpAt >= timeout) { closedId = snap.live.id; close = true; }
    } else if (!timeUp) {
      timeUpAt = 0;
    }
    if (close) {
      static uint32_t loggedId = 0;
      if (loggedId != snap.live.id) {
        loggedId = snap.live.id;
        Serial.printf("[face] %s is over (%s), showing the results\n", snap.live.name, race ? "5 minutes after the leader finished" : snap.live.finished ? "the feed says so" : "3 minutes after 0:00");
      }
      closedId = snap.live.id;
      snap.last = snap.live;
      snap.last.live = false;
      if (snap.last.tag == 'Q') strlcpy(snap.last.name, snap.last.name[0] == 'S' ? "SQ" : "QUALI", sizeof(snap.last.name));
      snap.live = F1Ticker::Session();
    }
  }
  // How long ago the last session ended: seen going from live to finished, or else guessed from its start
  static bool wasLive = false;
  static uint32_t endId = 0;
  static time_t endUtc = 0;
  time_t nowUtc = ultime::now();
  if (snap.live.valid) {
    wasLive = true;
  } else if (snap.last.valid && snap.last.id != endId) {
    endId = snap.last.id;
    int minutes = snap.last.tag == 'R' ? 120 : snap.last.tag == 'S' ? 40 : snap.last.name[0] == 'S' ? 45 : 60;
    endUtc = wasLive ? nowUtc : snap.last.start + minutes * 60;
    wasLive = false;
  }
  bool resultsOn = false;
  if (!snap.live.valid && snap.weekend && snap.last.valid && !shortPanel()) {
    uint32_t window = resultWindowSecs();
    resultsOn = window ? (long)(nowUtc - endUtc) < (long)window
                       : in.dateTime->utcToLocal(nowUtc) / SECS_PER_DAY == in.dateTime->utcToLocal(endUtc) / SECS_PER_DAY;
    if (in.simScenario == 8) resultsOn = true;
    else if (in.simScenario == 2) resultsOn = false;
  }
  F1Screen view = snap.live.valid ? LIVE : resultsOn ? LIST : snap.weekend && snap.last.valid ? BETWEEN : IDLE;
  static ScreenTimer scene;
  scene.enter((int)view, millis());
  // The info line of the short panel rotates, and so do the places after the first on the result screens
  int page = view != LIVE && (shortPanel() || view == BETWEEN || view == LIST) ? scene.page(millis(), pageMs()) : 0;
  if (view == BETWEEN && !shortPanel()) {
    int others = filledRows(snap.last) - 1;
    page = others > 4 ? page % ((others + 3) / 4) : 0;  // four places at a time; the key only changes with the page
  }
  if (view == LIST && qualiList(snap.last)) page = (scene.page(millis(), pageMs())) % scrollPages(snap.last);
  else if (view == LIST) {
    int n = filledRows(snap.last);
    page = n > 9 ? page % ((n + 6) / 8) : 0;  // eight places at a time under the winner
  }
  if (view == LIVE && snap.live.finished) { if (!qFinishedAt) qFinishedAt = millis(); } else qFinishedAt = 0;
  if (view == LIVE && scrollsAfterQ(snap.live)) page = (millis() / pageMs()) % scrollPages(snap.live);
  else if (view == LIVE && q3Scroll(snap.live) > 0) page = q3Pos(q3Scroll(snap.live));
  if (view == LIVE && strcmp(snap.live.clock, "0:00") == 0) {
    if (!clockZeroAt) clockZeroAt = millis();  // a countdown stuck at 0:00 turns into the time of day after a while
  } else {
    clockZeroAt = 0;
  }
  showIntervals = view == LIVE && !snap.live.finished && (snap.live.tag == 'R' || snap.live.tag == 'S') && in.intervals;
  updateClockDigits(hour, minute);
  // Where the soft clock is: on the idle and result screens, and on the one of a delayed start. Its digits change on
  // their own, so the minute is not part of the key there
  bool tallClock = !shortPanel() && (view == IDLE || view == BETWEEN || (view == LIVE && !snap.live.rows[0].code[0]));
  static int lastView = -1;
  if ((int)view != lastView) {  // nothing glides from another screen
    lastView = (int)view;
    slideList.cur = slideBetween.cur = slideNext.cur = -1;
    glideToN = glideFromN = 0;
  }
  bool gliding = (view == LIVE && (glideActive() || ((scrollsAfterQ(snap.live) || q3Scroll(snap.live) > 0) && slideActive(slideList)))) || (view == LIST && slideActive(slideList)) || (view == BETWEEN && (slideActive(slideBetween) || slideActive(slideNext)));
  uint32_t marks = 0;
  if (view == LIVE) {
    trackMoves(snap.live);
    marks = moveMask(snap.live) ^ (improvedMask(snap.live) * 2654435761UL);
  }

  viewOut = view;
  pageOut = page;
  // Only draw when something changed
  int rowPage = view == BETWEEN ? comingIndex() : 0;  // which of the coming sessions the bottom row shows
  int pulse = view != LIVE ? pulseLevel() : 0;  // the colon of the time breathes: drawn again at each step
  if (view == LIVE && timeOfDay(snap.live)) pulse = pulseLevel();  // the time of day's colon breathes
  if (view == LIVE || view == LIST) marqueeMax = 0;
  if (marqueeMax) pulse = pulse * 1000 + (millis() / MARQUEE_STEP_MS) % 1000;
  Frame frame;
  memset(&frame, 0, sizeof(frame));  // padding too: frames are compared as bytes
  frame.version = in.version;
  frame.view = (int32_t)view;
  frame.hour = tallClock ? 0 : hour;
  frame.minute = tallClock ? 0 : minute;
  frame.page = page;
  frame.marks = marks;
  frame.rowPage = rowPage;
  frame.pulse = pulse;
  frame.loadingDots = loadingDotsShown ? (millis() / LOADING_DOT_MS) % 3 + 1 : 0;
  frame.showIntervals = showIntervals;
  char right[12] = "";
  if (view == LIVE) liveRight(snap.live, right, sizeof(right), hour, minute);
  // Two buffers are drawn in turn and the one drawn on is the frame from two flips ago, so a change is never a patch on
  // top of it: the whole screen is drawn again, also for a moving clock digit or the countdown of a live session
  static bool wasMoving = false;
  bool moving = tallClock && clockMoving();
  bool countdown = view == LIVE && strcmp(right, _lastRight) != 0 && snap.live.rows[0].code[0];
  if (_haveFrame && memcmp(&frame, &_frame, sizeof(frame)) == 0) {
    if (!countdown && !moving && !gliding && !(tallClock && wasMoving)) return false;
  }
  wasMoving = moving;
  _frame = frame;
  _haveFrame = true;
  strcpy(_lastRight, right);

  return true;
}

}  // namespace f1view
