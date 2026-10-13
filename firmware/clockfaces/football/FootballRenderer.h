// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The drawing half of the football face: what is on screen as a function of the snapshot and the view state below.
// Clockface.cpp decides what is shown and when (see plan step 5, which turns this state into a Frame).

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <PanelLayout.h>
#include <TimeService.h>
#include <string.h>
#include "FootballTicker.h"

namespace fbview {

constexpr int W = layout::width;
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

static const uint16_t DATE = rgb(120, 185, 140);
static const int CLOCK_RIGHT = 41;  // where the upcoming games' panel starts: the ball disappears behind it

static const unsigned long SLIDE_MS = 400;  // rows of results slide up one at a time: a pause, then a quick slide
static const unsigned long SWAP_MS = SLIDE_MS;
static const unsigned long FLIP_MS = 640, FLIP_STAGGER = 220;
static const unsigned long SCORE_FLIP_MS = 160, SCORE_STAGGER = 60;
static const unsigned long SLOT_SLIDE_MS = 400;
static const unsigned long RANKING_TITLE_MS = 4000;
static const unsigned long RANKING_SLIDE_MS = 350;
static const int RANKING_ROW_STEP = 6, RANKING_ROWS = 6, RANKING_BOTTOM = 62;
static const int STYLE_FADE = 1, STYLE_ROLL = 2, STYLE_DISSOLVE = 3, STYLE_DRIFT = 4, STYLE_SHIMMER = 5;

// Results slide down one row at a time: which row is leaving, and how far it has moved
struct Slide {
  unsigned long step;
  int offset;
};

// One page of a table: up to six rows
struct RankingPage {
  char competition[32] = {};
  char teams[6][5] = {};
  int16_t points[6] = {};
  uint8_t count = 0;
  uint8_t firstPosition = 1;
  size_t pageCount = 0;
  bool valid = false;
};

// One match as drawn: a live match, or a result (live = false)
struct MatchRow {
  char home[4], away[4];
  TeamKit homeKit, awayKit;
  int8_t homeScore, awayScore;
  uint16_t scoreColor;  // the competition's colour
  float progress;       // playing time 0..1, drawn as a bar under the text
  bool live, favourite;
  char detail[8];       // live: "67'" or "HT"
  char competition[6];
  uint8_t markCount;
  football::Mark marks[football::kMarks];
  // "2-1"
  String score() const { return String((int)homeScore) + "-" + String((int)awayScore); }
  // Both teams have a single figure: the score can go on flip tiles
  bool singleFigures() const { return homeScore >= 0 && homeScore <= 9 && awayScore >= 0 && awayScore <= 9; }
};

// A std::vector's calls on a fixed array, for lists that must not touch the heap
template <typename T, size_t N> struct FixedList {
  T items[N];
  size_t count = 0;
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  void clear() { count = 0; }
  void push_back(const T &item) {
    if (count < N) items[count++] = item;
  }
  T &operator[](size_t i) { return items[i]; }
  const T &operator[](size_t i) const { return items[i]; }
};

struct Overview {
  FixedList<MatchRow, football::kLive> live;  // the live matches, or the result on top
};

// View state: Clockface::update() keeps these, the drawing functions read them
extern Overview overview;
extern football::Snapshot snapshot;
extern uint32_t overviewRevision;
extern bool resultsOnTop;
extern size_t shownResult;
extern bool ballLeft;
extern int ballLift;
extern int clockDigits[4], oldDigits[4];
extern float tileProgress[4];
extern bool flipAll;
extern int timeStyle;
extern int scoreDigit[2], scoreOld[2];
extern unsigned long scoreStart[2];
extern float scoreProgress[2];
extern football::FixtureRow slotOld[2];
extern unsigned long slotElapsed[2];
extern bool slideRoll;
extern RankingPage rankingPage, previousRankingPage;
extern unsigned long rankingStartedAt;
extern bool previousRankingValid;

int textWidth(const String &s, int scale = 1);
int textWidth(const char *s, int scale = 1);
uint16_t scaleColor(uint16_t c, int level);

size_t finishedCount();
size_t upcomingCount();
MatchRow liveMatch(const football::LiveRow &row);
bool readResult(size_t index, MatchRow &out);
uint16_t favouriteResults();
size_t resultsBelow();
size_t belowRows(ClockTime *dateTime);
int mainSlots(unsigned long turn, const football::FixtureRow *slots[2]);

Slide slideAt(unsigned long now, int rowHeight, unsigned long pauseMs);
int ballPass(unsigned long now, bool big);
int colonPulse(unsigned long now);

void drawStadium(MatrixPanel_I2S_DMA *d, int level = 32, bool withBoards = true, bool clockGaps = true, int fromX = 0, int toX = W,
                 int fromY = 0, int toY = 64);
void drawLive(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, size_t index, size_t pinned, size_t shift, int hour, int minute, int second,
              int pulse, const Slide &slide, int push, const MatchRow *swapOld, unsigned long swapT);
void drawMain(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int second, int pulse, unsigned long turn,
              bool showRankings, unsigned long now, int roll);
void drawBigClock(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int second, int roll);
void drawStatusDot(MatrixPanel_I2S_DMA *d, FootballTicker::Status status, int pulse);

// Card and substitution animations: frame 0 .. incidentFrames()-1, INCIDENT_FRAME_MS apart; each draws the whole screen
static const unsigned long INCIDENT_FRAME_MS = 40;  // 25 frames a second
int incidentFrames(const FootballTicker::Incident &in);
void drawIncidentFrame(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, int frame);

}  // namespace fbview
