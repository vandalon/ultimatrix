// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The timing half of the football face: which view is on, which match is on top, and where every slide, flip and
// dip is. No drawing: it hands Clockface a Frame, and the renderer (FootballRenderer.h) draws it.

#include <Arduino.h>
#include <TimeService.h>
#include "FootballRenderer.h"
#include "FootballTicker.h"

namespace fbview {

enum class View { Live, Fixtures, Clock };

// Everything that makes the picture differ from the one before. Clockface redraws when it changes, so only
// values that change a pixel belong here: animation frames rather than raw milliseconds.
struct Frame {
  uint32_t version;
  int32_t view;
  int32_t hour, minute, second;
  uint32_t index;
  uint32_t slideStep;
  int32_t slideOffset;
  int32_t pulse, roll, ballLeft, ballLift;
  int32_t flipFrame;
  uint32_t turn;
  int32_t push;
  uint32_t shift;
  int32_t slotFrame[2];
  int32_t swapFrame;
  int32_t scoreDigit[2];
  int32_t scoreFrame[2];
  int32_t status, timeStyle, shimmerFrame;
  uint32_t rankingFrame;
};

struct ScreenInputs {
  ClockTime *dateTime;
  int hour, minute, second;
  unsigned long now;
  uint32_t version;  // of the ticker's data
  bool showRankings, simRankings;
  FootballTicker::Status status;
};

class FootballScreens {
 public:
  // Step the view, flip clock, slots and live swap to now
  void advance(const ScreenInputs &in);
  // Then, after any goal or incident animation: the score tiles, and the Frame
  void finish();
  // The ranking pages start over (setting or simulation changed)
  void resetRankings();
  void resetRankingPage() { _shownRankingPage = SIZE_MAX; }

  const Frame &frame() const { return _frame; }
  // What drawing needs besides the Frame
  View view = View::Clock;
  size_t index = 0, pinned = 0;
  bool showRankings = false, swapActive = false;
  unsigned long swapT = 0, now = 0;
  Slide slide = {0, 0};
  MatchRow liveOld;  // the match leaving the top while swapActive

 private:
  Frame _frame;
  bool _swapBegan = false;
  size_t _shownRankingPage = SIZE_MAX;
  uint32_t _rankingVersion = 0;
  unsigned long _rankingPageDuration = FootballTicker::STANDINGS_STEP_MS;
  bool _digitsKnown = false;
  unsigned long _flipStart = 0, _tileStart[4] = {0, 0, 0, 0};
  int _shownStyle = -1;
  football::FixtureRow _slotShown[2];
  bool _slotKnown[2] = {false, false};
  unsigned long _slotStart[2] = {0, 0};
  MatchRow _liveShown;
  bool _liveKnown = false;
  unsigned long _swapStart = 0;
  uint32_t _liveShownVersion = 0;
};

}  // namespace fbview
