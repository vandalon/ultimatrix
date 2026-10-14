// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "FootballScreens.h"
#include <Settings.h>
#include <math.h>

namespace fbview {

// Each page advances six rows; the last page reaches the bottom before the next table.
static bool standingsPage(const football::Snapshot &snap, size_t page, RankingPage &out) {
  out.pageCount = 0;
  out.count = 0;
  for (size_t g = 0; g < snap.groupCount; g++) out.pageCount += FootballTicker::standingsSteps(snap.groups[g].rows);
  if (!out.pageCount) return false;
  page %= out.pageCount;
  for (size_t g = 0; g < snap.groupCount; g++) {
    const football::Group &group = snap.groups[g];
    size_t steps = FootballTicker::standingsSteps(group.rows);
    if (page >= steps) {
      page -= steps;
      continue;
    }
    size_t first = min(page * 6, group.rows > 6 ? size_t(group.rows - 6) : size_t(0));
    strlcpy(out.competition, group.name, sizeof(out.competition));
    out.firstPosition = first + 1;
    for (size_t i = first; i < min(first + 6, size_t(group.rows)); i++) {
      strlcpy(out.teams[out.count], snap.teams[group.first + i].code, sizeof(out.teams[0]));
      out.points[out.count++] = snap.teams[group.first + i].points;
    }
    return true;
  }
  return false;
}

void FootballScreens::advance(const ScreenInputs &in) {
  now = in.now;
  showRankings = in.showRankings;
  memset(&_frame, 0, sizeof(_frame));  // the padding too: Frames are compared byte for byte

  // Which view, which live match is on top, and where the sliding lists are
  view = !overview.live.empty() ? View::Live : upcomingCount() ? View::Fixtures : View::Clock;
  if (view == View::Fixtures && showRankings) {
    bool firstPage = _shownRankingPage == SIZE_MAX;
    bool advance = !firstPage && rankingPage.valid && now - rankingStartedAt >= _rankingPageDuration;
    size_t page = firstPage ? 0 : _shownRankingPage + (advance ? 1 : 0);
    if (page != _shownRankingPage || _rankingVersion != in.version) {
      bool hadPage = rankingPage.valid;
      previousRankingValid = hadPage && advance;
      if (previousRankingValid) previousRankingPage = rankingPage;
      rankingPage.valid = standingsPage(snap(), page, rankingPage);
      if (previousRankingValid && strcmp(rankingPage.competition, previousRankingPage.competition) == 0 &&
          rankingPage.firstPosition == previousRankingPage.firstPosition) previousRankingValid = false;
      if (firstPage || advance || (!hadPage && rankingPage.valid)) {
        rankingStartedAt = now;
        _rankingPageDuration = FootballTicker::STANDINGS_STEP_MS + (previousRankingValid ? RANKING_SLIDE_MS : 0);
      }
      _shownRankingPage = page;
      _rankingVersion = in.version;
    }
  } else _shownRankingPage = SIZE_MAX;
  unsigned long matchMs = max(3, (int)Settings::getInstance()->matchSecs) * 1000UL;
  // Live favourites come first in the list and are pinned to the top: with just one of them the top never
  // changes, with several they take turns
  pinned = 0;
  while (pinned < overview.live.size() && overview.live[pinned].favourite) pinned++;
  if (pinned == overview.live.size()) pinned = 0;  // all of them are: the usual turns
  size_t shift = now / matchMs;
  index = view == View::Live ? shift % (pinned ? pinned : overview.live.size()) : 0;
  size_t rows = belowRows(in.dateTime);
  bool resultFavourites = resultsOnTop && !overview.live.empty() && overview.live[0].favourite;
  bool slides = view == View::Live && rows > 1 && (overview.live.size() == 1 || resultFavourites);
  slide = slides ? slideAt(now, 9, matchMs - SLIDE_MS) : Slide{0, 0};
  bool singleLive = view == View::Live && overview.live.size() == 1 && rows == 0;
  bool loading = in.status == FootballTicker::LOADING;
  bool rolling = (view == View::Clock || (view == View::Fixtures && !(showRankings && rankingPage.valid)))
                 && Settings::getInstance()->ballRoll;
  // The flip clock tiles flip when a digit changes
  int digits[4] = {in.hour / 10, in.hour % 10, in.minute / 10, in.minute % 10};
  if (!_digitsKnown) {
    for (int i = 0; i < 4; i++) clockDigits[i] = oldDigits[i] = digits[i];
    _digitsKnown = true;
  } else if (memcmp(digits, clockDigits, sizeof(digits)) != 0) {
    int rank = 0;
    for (int i = 3; i >= 0; i--)  // the last digit first, like an odometer
      if (digits[i] != clockDigits[i]) _tileStart[i] = now + rank++ * FLIP_STAGGER;
    memcpy(oldDigits, clockDigits, sizeof(digits));
    memcpy(clockDigits, digits, sizeof(digits));
    _flipStart = now;
  }
  timeStyle = Settings::getInstance()->timeStyle;
  if (_shownStyle >= 0 && timeStyle != _shownStyle) {
    flipAll = true;
    _tileStart[3] = _flipStart = now;
  }
  _shownStyle = timeStyle;
  bool flipping = false;
  for (int i = 0; i < 4; i++) {
    long t = (long)(now - _tileStart[i]);
    bool changing = oldDigits[i] != clockDigits[i] || (flipAll && i == 3);
    tileProgress[i] = !changing || t >= (long)FLIP_MS ? 1 : t < 0 ? -1 : t / (float)FLIP_MS;
    if (tileProgress[i] < 1) flipping = true;
  }
  if (!flipping) flipAll = false;
  int flipFrame = flipping ? (now - _flipStart) / 40 + 1 : 0;  // 25 frames a second while it lasts
  // the shimmer keeps moving while a clock is on show: 12 frames a second
  int shimmerFrame = timeStyle == STYLE_SHIMMER && (view != View::Live || singleLive) ? now / 80 : 0;

  int roll = view != View::Fixtures && view != View::Clock ? 0 : !rolling ? -1  // -1 = the ball rests instead
             : ballPass(now, view != View::Live);
  int pulse = singleLive || loading ? colonPulse(now) : 0;  // only redraw for it when shown
  // The live list below moves down a row as the next match takes the top
  unsigned long sinceTurn = now % matchMs;
  bool listTurns = !pinned || overview.live.size() - pinned > 1;  // pinned: only more than one other takes turns
  int push = view == View::Live && (rows == 0 || pinned || overview.live.size() > 1) && listTurns && sinceTurn < SLIDE_MS ? sinceTurn * 9 / SLIDE_MS : 9;
  unsigned long turn = view == View::Fixtures ? now / matchMs : 0;

  // A slot or the live match that takes another match flips or dips: keep the old one while it lasts
  int slotFrame[2] = {0, 0};
  const football::FixtureRow *slots[2];
  int slotCount = view == View::Fixtures ? mainSlots(turn, slots) : 0;
  bool slotChanged[2] = {false, false};
  for (int i = 0; i < 2; i++) {
    if (i < slotCount) {
      const auto &u = *slots[i];
      if (_slotKnown[i] && (strcmp(_slotShown[i].home.code, u.home.code) != 0 || strcmp(_slotShown[i].away.code, u.away.code) != 0 || _slotShown[i].kickoff != u.kickoff)) {
        slotOld[i] = _slotShown[i];
        _slotStart[i] = now;
        slotElapsed[i] = 0;
        slotChanged[i] = true;
      }
      _slotShown[i] = u;
      _slotKnown[i] = true;
      if (slotElapsed[i] < SLOT_SLIDE_MS) {
        slotElapsed[i] = now - _slotStart[i];
        if (slotElapsed[i] >= SLOT_SLIDE_MS) slotElapsed[i] = SLOT_SLIDE_MS;
        else slotFrame[i] = slotElapsed[i] / 30 + 1;  // 30 frames a second while it lasts
      }
    } else {
      _slotKnown[i] = false;
      slotElapsed[i] = SLOT_SLIDE_MS;
    }
  }
  if (slotChanged[0] || slotChanged[1])
    slideRoll = slotChanged[0] && slotChanged[1] && slotCount == 2 && strcmp(slotOld[1].home.code, slots[0]->home.code) == 0 &&
                strcmp(slotOld[1].away.code, slots[0]->away.code) == 0 && slotOld[1].kickoff == slots[0]->kickoff;
  _swapBegan = false;
  if (view == View::Live) {
    const auto &m = overview.live[index];
    bool changed = _liveKnown && (strcmp(_liveShown.home, m.home) != 0 || strcmp(_liveShown.away, m.away) != 0);
    if (changed) {
      liveOld = _liveShown;
      _swapStart = now;
      swapActive = true;
      _swapBegan = true;
    }
    if (!_liveKnown || changed || in.version != _liveShownVersion) {  // copying every frame would be a waste
      _liveShown = m;
      _liveShownVersion = in.version;
    }
    _liveKnown = true;
  } else {
    _liveKnown = swapActive = false;
  }
  swapT = swapActive ? now - _swapStart : 0;
  if (swapActive && swapT >= SWAP_MS) swapActive = false;
  int swapFrame = swapActive ? swapT / 30 + 1 : 0;

  _frame.version = in.version;
  _frame.view = (int)view;
  _frame.hour = in.hour;
  _frame.minute = in.minute;
  _frame.second = in.second;
  _frame.index = index;
  _frame.slideStep = slide.step;
  _frame.slideOffset = slide.offset;
  _frame.pulse = pulse;
  _frame.roll = roll;
  _frame.ballLeft = ballLeft;
  _frame.ballLift = ballLift;
  _frame.flipFrame = flipFrame;
  _frame.turn = turn;
  _frame.push = push;
  _frame.shift = shift;
  _frame.slotFrame[0] = slotFrame[0];
  _frame.slotFrame[1] = slotFrame[1];
  _frame.swapFrame = swapFrame;
  _frame.status = (int)in.status;
  _frame.timeStyle = timeStyle;
  _frame.shimmerFrame = shimmerFrame;
}

void FootballScreens::finish() {
  // Changed scores flip their digits; a different match scrolls in with its own settled score.
  unsigned long tnow = millis();
  int target[2] = {-1, -1};
  if (view == View::Live) {
    const MatchRow &m = overview.live[index];  // single figures on both sides
    if (m.singleFigures()) {
      target[0] = m.homeScore;
      target[1] = m.awayScore;
    }
  }
  int scoreFrame[2] = {0, 0};
  for (int i = 0; i < 2; i++) {
    if (target[0] < 0 || target[1] < 0) {
      scoreDigit[i] = scoreOld[i] = -1;
      scoreProgress[i] = 1;
      continue;
    }
    if (scoreDigit[i] < 0 || _swapBegan) {
      scoreDigit[i] = scoreOld[i] = target[i];
    } else if (target[i] != scoreDigit[i]) {
      scoreOld[i] = scoreDigit[i];
      scoreDigit[i] = target[i];
      scoreStart[i] = tnow + i * SCORE_STAGGER;
    }
    scoreProgress[i] = 1;
    if (scoreOld[i] != scoreDigit[i]) {
      long t = (long)(tnow - scoreStart[i]);
      if (t >= (long)SCORE_FLIP_MS) {
        scoreOld[i] = scoreDigit[i];
      } else {
        scoreProgress[i] = t <= 0 ? 0 : t / (float)SCORE_FLIP_MS;
        scoreFrame[i] = max(t, 0L) / 30 + 1;  // 30 frames a second while it lasts
      }
    }
  }

  unsigned rankingFrame = 0;
  if (view == View::Fixtures && showRankings && rankingPage.valid) {
    unsigned long elapsed = now - rankingStartedAt;
    if (previousRankingValid && elapsed < RANKING_SLIDE_MS) rankingFrame = elapsed / 40 + 1;
    else {
      int overflow = max(0, textWidth(rankingPage.competition) - (CLOCK_RIGHT - 2));
      unsigned long phase = min(elapsed, RANKING_TITLE_MS - 1);
      rankingFrame = 10 + (phase < 500 ? 0 : min(overflow, (int)((phase - 500) * overflow / 2800)));
    }
  }
  _frame.scoreDigit[0] = scoreDigit[0];
  _frame.scoreDigit[1] = scoreDigit[1];
  _frame.scoreFrame[0] = scoreFrame[0];
  _frame.scoreFrame[1] = scoreFrame[1];
  _frame.rankingFrame = rankingFrame;
}

}  // namespace fbview
