// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

// The drawing half of the F1 face: what is on screen as a function of the snapshot and the view state below.
// Clockface.cpp decides what is shown and when, and calls these.

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ClockTime.h>
#include <PanelLayout.h>
#include "F1Ticker.h"

namespace f1view {

constexpr int W = layout::width;
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
static const uint16_t WHITE = rgb(255, 255, 255);
static const unsigned long LOADING_DOT_MS = 300;
static const unsigned long MARQUEE_STEP_MS = 160;

// A page of a result list: the rows of the page before slide up and out while the next ones come in
struct PageSlide { int cur = -1, prev = 0; unsigned long at = 0; };

extern F1Ticker::Snapshot snap;  // what is drawn: the ticker's snapshot, as the face adjusts it
extern bool showIntervals;
extern bool backdropShown;
extern unsigned long clockZeroAt;
extern int dotShift;
extern int glideFromN, glideToN;
extern bool loadingDotsShown;
extern int marqueeMax;
extern unsigned long qFinishedAt;
extern PageSlide slideList, slideBetween;
extern PageSlide slideNext;
extern uint64_t tileRows;

// Settings that drive the timing
bool clockMoving();
int comingCount();
int comingIndex();
void drawBetween(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int page);
void drawIdle(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int page);
void drawLive(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s, int hour, int minute, int page);
void drawResultList(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s, int hour, int minute, int page);
int filledRows(const F1Ticker::Session &s);
bool glideActive();
uint32_t improvedMask(const F1Ticker::Session &s);
void liveRight(const F1Ticker::Session &s, char *right, size_t size, int hour, int minute);
uint32_t moveMask(const F1Ticker::Session &s);
unsigned long pageMs();
int pulseLevel();
int q3Scroll(const F1Ticker::Session &s);
int q3Pos(int extra);
int qPart(const F1Ticker::Session &s);
bool qualiList(const F1Ticker::Session &s);
uint32_t resultWindowSecs();
int scrollPages(const F1Ticker::Session &s);
bool scrollsAfterQ(const F1Ticker::Session &s);
bool shortPanel();
char shownFlag(const F1Ticker::Session &s);
bool timeOfDay(const F1Ticker::Session &s);
bool timesFailed(const F1Ticker::Session &s);
void trackMoves(const F1Ticker::Session &s);
void updateClockDigits(int hour, int minute);
bool slideActive(const PageSlide &s);

}  // namespace f1view
