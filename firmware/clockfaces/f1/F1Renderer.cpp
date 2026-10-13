// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.

#include "F1Renderer.h"
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

namespace f1view {


// Formula 1 as three screens:
//  - A session is live: a header with the session and its lap or time left, then the running order
//    (position, team colour, driver) of the first places. A driver who gains or loses a place is marked for a
//    few seconds. A safety car turns the header yellow, a red flag red.
//  - A race weekend, nothing live: the clock and the result of the session that just finished, the winner fixed
//    and the other places scrolling, and the start of the next session along the bottom.
//  - Otherwise: the clock, the top three of the drivers' championship and the coming Grand Prix along the bottom.
// On a panel of 32 rows there is room for the clock and one line, so the standings, the results and the next
// session take turns in that line, a few seconds each.

F1Ticker::Snapshot snap;
bool showIntervals = false;  // the live race shows the gaps between cars rather than to the leader
static bool loadingDotVisible(const char *at, const char *dots) {
  if (!dots || at < dots || at >= dots + 3) return true;
  return (unsigned long)(at - dots) <= (millis() / LOADING_DOT_MS) % 3;
}

// How long a page of the results (or, on 32 rows, a line of the rotating information) stays: the football setting
unsigned long pageMs() { return max(3, (int)Settings::getInstance()->matchSecs) * 1000UL; }
// How long the whole classification stays after a session, in seconds; 0 = until midnight (the football setting)
uint32_t resultWindowSecs() { return Settings::getInstance()->resultMins * 60UL; }
bool shortPanel() { return Settings::getInstance()->displayHeight == 32; }

// A neutral RGB565 colour at the lowest visible level in the driver's CIE table for this colour depth.
#if defined(UM_PIXEL_DEPTH_BITS) && UM_PIXEL_DEPTH_BITS <= 5
static const uint8_t MIN_LIT = 56;
#elif defined(UM_PIXEL_DEPTH_BITS) && UM_PIXEL_DEPTH_BITS == 7
static const uint8_t MIN_LIT = 16;
#elif defined(UM_PIXEL_DEPTH_BITS) && UM_PIXEL_DEPTH_BITS >= 8
static const uint8_t MIN_LIT = 8;
#else
static const uint8_t MIN_LIT = 40;
#endif

static const uint16_t CLOCK = rgb(238, 246, 240);
static const uint16_t MINUTES = rgb(190, 235, 150);
static const uint16_t GREY = rgb(150, 150, 160);
static const uint16_t TIME_GREY = rgb(178, 178, 188);  // the times of the places after the first, brighter than the rest of the grey
static const uint16_t SESSION_NAME = rgb(78, 82, 92);
static const uint16_t DIMMED = rgb(110, 116, 126);
static const uint16_t DELAYED = rgb(255, 150, 0);   // a session that has not started on time
static const uint16_t NEW_TIME = rgb(80, 205, 255);  // its new start time
static const uint16_t GOLD = rgb(255, 214, 0);
static const uint16_t F1_RED = rgb(225, 6, 0);
static const uint16_t LIVE_GREEN = rgb(60, 230, 90);
static const uint16_t HEADER_NAVY = rgb(14, 22, MIN_LIT > 52 ? MIN_LIT : 52);  // the header bar: dark, with a small coloured tab on its left edge
static const uint16_t TAB_QUIET = rgb(90, 110, 150);  // the tab when it is not a race or qualifying
static const uint16_t RED_FLAG_BAR = rgb(150, 8, 6);
static const uint16_t LINE = rgb(MIN_LIT, MIN_LIT + 4, MIN_LIT + 12);
static const uint16_t RULE = rgb(80, 85, 96);  // the two lines of the tall idle screen: lighter, to stand out from the squares
static const uint16_t BLACK = 0;
static const uint16_t FLAG_TILE = rgb(MIN_LIT, MIN_LIT, MIN_LIT);  // the squares of the chequered flag, dim: level 1 on all three
                                                                  // colours (one of them a level lower turns the grey cyan)
static const uint16_t TRACK_LINE = rgb(48, 78, 150);

#include <LapFonts.h>

static const char *const DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const DAYS2[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};  // the short days of the Unicode locale data

static int textWidth(const char *s, int scale = 1) { return *s ? (strlen(s) * 4 - 1) * scale : 0; }

// Text is only drawn between these rows: a line sliding in or out of a strip is cut at the strip's edges
static int clipTop = 0, clipBottom = 64, clipLeft = 0, clipRight = 64;

// The text font: single pixel strokes, 7 rows high and 5 wide (the dot and colon 1), with the lower case letters of the days
static const struct { char c; const char *rows[7]; } TALL_FONT[] = {
  {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}, {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
  {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}}, {'D', {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."}},
  {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}}, {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
  {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}}, {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
  {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}}, {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
  {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}}, {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
  {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}}, {'N', {"#...#", "##..#", "##..#", "#.#.#", "#..##", "#..##", "#...#"}},
  {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}, {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
  {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}}, {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
  {'S', {".###.", "#...#", "#....", ".###.", "....#", "#...#", ".###."}}, {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
  {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}, {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
  {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}}, {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
  {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}}, {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
  {'0', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}, {'1', {"..#..", ".##..", "#.#..", "..#..", "..#..", "..#..", "#####"}},
  {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}}, {'3', {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
  {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}}, {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
  {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}}, {'7', {"#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."}},
  {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}}, {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
  {'a', {".....", ".....", ".###.", "....#", ".####", "#...#", ".####"}}, {'e', {".....", ".....", ".###.", "#...#", "#####", "#....", ".####"}},
  {'h', {"#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"}}, {'o', {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."}},
  {'r', {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."}}, {'u', {".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####"}},
  {':', {".", "#", ".", ".", ".", "#", "."}}, {'.', {".", ".", ".", ".", ".", ".", "#"}},
  {'/', {"....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."}}, {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
  {'+', {".....", ".....", "..#..", ".###.", "..#..", ".....", "....."}},
};
static const int TALL_SPACE = 3;
static const char *const *tallGlyph(char c) {
  for (const auto &g : TALL_FONT)
    if (g.c == c) return g.rows;
  return nullptr;
}
// The session names of the bottom bar in 4 pixels wide letters, so SPRINT fits beside the time; other characters
// come from the 5 wide font
static const struct { char c; const char *rows[7]; } NARROW_FONT[] = {
  {'A', {".##.", "#..#", "#..#", "####", "#..#", "#..#", "#..#"}}, {'C', {".###", "#...", "#...", "#...", "#...", "#...", ".###"}},
  {'E', {"####", "#...", "#...", "###.", "#...", "#...", "####"}}, {'F', {"####", "#...", "#...", "###.", "#...", "#...", "#..."}},
  {'I', {"###", ".#.", ".#.", ".#.", ".#.", ".#.", "###"}}, {'L', {"#...", "#...", "#...", "#...", "#...", "#...", "####"}},
  {'N', {"#..#", "##.#", "##.#", "#.##", "#.##", "#..#", "#..#"}}, {'P', {"###.", "#..#", "#..#", "###.", "#...", "#...", "#..."}},
  {'Q', {".##.", "#..#", "#..#", "#..#", "#.##", ".##.", "..##"}}, {'R', {"###.", "#..#", "#..#", "###.", "#.#.", "#..#", "#..#"}},
  {'S', {".###", "#...", "#...", ".##.", "...#", "...#", "###."}}, {'T', {"###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."}},
  {'U', {"#..#", "#..#", "#..#", "#..#", "#..#", "#..#", ".##."}}, {'1', {".#.", "##.", ".#.", ".#.", ".#.", ".#.", "###"}},
  {'2', {".##.", "#..#", "...#", "..#.", ".#..", "#...", "####"}}, {'3', {"###.", "...#", "...#", ".##.", "...#", "...#", "###."}},
  {'B', {"###.", "#..#", "#..#", "###.", "#..#", "#..#", "###."}}, {'D', {"###.", "#..#", "#..#", "#..#", "#..#", "#..#", "###."}},
  {'G', {".###", "#...", "#...", "#.##", "#..#", "#..#", ".###"}}, {'H', {"#..#", "#..#", "#..#", "####", "#..#", "#..#", "#..#"}},
  {'J', {"..##", "...#", "...#", "...#", "...#", "#..#", ".##."}}, {'K', {"#..#", "#.#.", "##..", "##..", "#.#.", "#..#", "#..#"}},
  {'M', {"#..#", "####", "####", "#..#", "#..#", "#..#", "#..#"}}, {'O', {".##.", "#..#", "#..#", "#..#", "#..#", "#..#", ".##."}},
  {'V', {"#..#", "#..#", "#..#", "#..#", "#..#", ".##.", ".##."}}, {'W', {"#..#", "#..#", "#..#", "#..#", "####", "####", "#..#"}},
  {'X', {"#..#", "#..#", ".##.", ".##.", ".##.", "#..#", "#..#"}}, {'Y', {"#..#", "#..#", ".##.", ".##.", "..#.", "..#.", "..#."}},
  {'Z', {"####", "...#", "..#.", "..#.", ".#..", "#...", "####"}}, {'0', {".##.", "#..#", "#..#", "#..#", "#..#", "#..#", ".##."}},
  {'4', {"..#.", ".##.", "#.#.", "#.#.", "####", "..#.", "..#."}}, {'5', {"####", "#...", "###.", "...#", "...#", "#..#", ".##."}},
  {'6', {".##.", "#...", "#...", "###.", "#..#", "#..#", ".##."}}, {'7', {"####", "...#", "..#.", "..#.", ".#..", ".#..", ".#.."}},
  {'8', {".##.", "#..#", "#..#", ".##.", "#..#", "#..#", ".##."}}, {'9', {".##.", "#..#", "#..#", ".###", "...#", "...#", ".##."}},
  {'/', {"..#", "..#", ".#.", ".#.", ".#.", "#..", "#.."}},
};
static const char *const *narrowGlyph(char c) {
  for (const auto &g : NARROW_FONT)
    if (g.c == c) return g.rows;
  return tallGlyph(c);
}
static void drawNarrowText(MatrixPanel_I2S_DMA *d, const char *s, int x, int y, uint16_t color) {
  for (; *s; s++) {
    const char *const *rows = narrowGlyph(*s);
    if (!rows) { x += TALL_SPACE; continue; }
    int width = (int)strlen(rows[0]);
    for (int j = 0; j < 7; j++)
      for (int k = 0; k < width; k++)
        if (rows[j][k] == '#' && y + j >= clipTop && y + j < clipBottom) d->drawPixel(x + k, y + j, color);
    x += width + 1;
  }
}

static int tallWidth(const char *s) {
  int w = 0;
  for (; *s; s++) {
    const char *const *rows = tallGlyph(*s);
    w += rows ? (int)strlen(rows[0]) + 1 : TALL_SPACE;
  }
  return w ? w - 1 : 0;
}

// tall: the bottom bar's 4x7 letters instead of the 3x5 ones
static void drawText(MatrixPanel_I2S_DMA *d, const char *s, int x, int y, uint16_t color, bool tall = false) {
  if (tall) {
    for (; *s; s++) {
      const char *const *rows = tallGlyph(*s);
      if (!rows) { x += TALL_SPACE; continue; }
      int width = (int)strlen(rows[0]);
      for (int j = 0; j < 7; j++) {
        if (y + j < clipTop || y + j >= clipBottom) continue;
        for (int k = 0; k < width; k++)
          if (rows[j][k] == '#' && x + k >= clipLeft && x + k < clipRight) d->drawPixel(x + k, y + j, color);
      }
      x += width + 1;
    }
    return;
  }
  for (; *s; s++, x += 4) {
    const uint8_t *rows = glyph3x5(*s);
    if (!rows) continue;
    for (int j = 0; j < 5; j++) {
      if (y + j < clipTop || y + j >= clipBottom) continue;
      uint8_t bits = rows[j];
      for (int k = 0; k < 3; k++)
        if (bits >> (2 - k) & 1 && x + k >= clipLeft && x + k < clipRight) d->drawPixel(x + k, y + j, color);
    }
  }
}

// The header's text, 6 rows high (y is the top row), same width as the small font
static void drawHeaderText(MatrixPanel_I2S_DMA *d, const char *s, int x, int y, uint16_t color) {
  const char *dots = strstr(s, "...");
  for (; *s; x += *s == ' ' ? 2 : 4, s++) {  // a space is 2: with the letter's own gap, 3 blank columns (RED FLAG)
    if (!loadingDotVisible(s, dots)) continue;
    const uint8_t *rows = glyph3x6(*s);
    if (!rows) continue;
    for (int j = 0; j < 6; j++)
      for (int k = 0; k < 3; k++)
        if (rows[j] >> (2 - k) & 1) d->drawPixel(x + k, y + j, color);
  }
}

static void drawTextRight(MatrixPanel_I2S_DMA *d, const char *s, int right, int y, uint16_t color, bool tall = false) {
  drawText(d, s, right - (tall ? tallWidth(s) : textWidth(s)), y, color, tall);
}

// A time or gap ("1:29.412", "+0.123"): the font's dot and colon sit in the middle of a cell, which leaves two empty
// columns either side; here they take one column and a gap of one, so one column less on each side
static int timeAdvance(char c) { return c == ' ' ? 1 : c == '.' || c == ':' ? 2 : 4; }  // a space (the day and the time) is 1: with the letter's own gap, 2 blank columns
static int timeWidth(const char *s) {
  int w = 0;
  for (; *s; s++) w += timeAdvance(*s);
  return w ? w - 1 : 0;
}
static void drawTimeRight(MatrixPanel_I2S_DMA *d, const char *s, int right, int y, uint16_t color) {
  int x = right - timeWidth(s);
  const char *dots = strstr(s, "...");
  for (; *s; x += timeAdvance(*s), s++) {
    if (!loadingDotVisible(s, dots)) continue;
    char one[2] = {*s, 0};
    drawText(d, one, *s == '.' || *s == ':' ? x - 1 : x, y, color);  // the dot is in the middle column of its cell
  }
}

// The colon of the current time breathes: a fade between a third and full brightness, in 32 steps (a frame is 50 ms)
static const unsigned long PULSE_MS = 2000;
int pulseLevel() { return (int)roundf((0.5f - 0.5f * cosf((millis() % PULSE_MS) * 6.2831853f / PULSE_MS)) * 31); }
// Blend toward the background at the dim end: headers and backdrop tiles keep their colour beneath the pulse
static uint16_t pulsed(uint16_t color, uint16_t bg = 0) {
  int f = 85 + pulseLevel() * 170 / 31;  // out of 255; keep the dim end visible to avoid a blink
  auto mix = [f](int c, int b) { return b + (c - b) * f / 255; };
  int r = mix(color >> 11 & 31, bg >> 11 & 31), g = mix(color >> 5 & 63, bg >> 5 & 63), b = mix(color & 31, bg & 31);
  return r << 11 | g << 5 | b;
}

// The time in the banner, 3x6 digits with the colon tucked in: one pixel either side, where the font's own colon
// leaves two. Flush against the right edge.
// The time in the results header: the list's 3x5 digits stretched to 4 x 7, a 1 wide colon between them with a pixel either side
static void drawStretchedDigits(MatrixPanel_I2S_DMA *d, const char *s, int x, int y, uint16_t color) {
  static const uint8_t ROW_OF[7] = {0, 1, 1, 2, 3, 3, 4}, COL_OF[4] = {0, 1, 1, 2};
  static const char *const ONE[7] = {"..#.", ".##.", "..#.", "..#.", "..#.", "..#.", ".###"};  // the stretched 1 would have a double stem
  for (; *s; s++, x += 5) {
    const uint8_t *rows = glyph3x5(*s);
    if (!rows) continue;
    for (int j = 0; j < 7; j++)
      for (int k = 0; k < 4; k++)
        if (*s == '1' ? ONE[j][k] == '#' : rows[ROW_OF[j]] >> (2 - COL_OF[k]) & 1) d->drawPixel(x + k, y + j, color);
  }
}
static void drawHeaderTime(MatrixPanel_I2S_DMA *d, int hour, int minute, int right, int y, uint16_t color, uint16_t bg) {
  char h[3], m[3];
  snprintf(h, sizeof(h), "%d", hour);
  snprintf(m, sizeof(m), "%02d", minute);
  int hw = (int)strlen(h) * 5 - 1;
  int x = right - (hw + 3 + 9);
  drawStretchedDigits(d, h, x, y, color);
  d->drawPixel(x + hw + 1, y + 1, pulsed(color, bg));
  d->drawPixel(x + hw + 1, y + 5, pulsed(color, bg));
  drawStretchedDigits(d, m, x + hw + 3, y, color);
}

// A text that is wider than its window waits, scrolls a pixel at a time until its end is in view, waits again and
// scrolls back. marqueeMax is how far it scrolls (0 when nothing does); the offset is a function of the time, so the
// screen is drawn again whenever it changes
static const unsigned long MARQUEE_PAUSE_MS = 2000;
int marqueeMax = 0;
static int marqueeOffset(int most) {
  unsigned long scroll = most * MARQUEE_STEP_MS;
  unsigned long t = millis() % (2 * (MARQUEE_PAUSE_MS + scroll));
  if (t < MARQUEE_PAUSE_MS) return 0;
  t -= MARQUEE_PAUSE_MS;
  if (t < scroll) return (int)(t / MARQUEE_STEP_MS);
  t -= scroll;
  if (t < MARQUEE_PAUSE_MS) return most;
  t -= MARQUEE_PAUSE_MS;
  return most - (int)(t / MARQUEE_STEP_MS);
}
static void drawMarquee(MatrixPanel_I2S_DMA *d, const char *text, int x, int y, int right, uint16_t color, bool tall = false) {
  int width = tall ? tallWidth(text) : textWidth(text);
  if (width <= right - x) {
    drawText(d, text, x, y, color, tall);
    return;
  }
  int overflow = width - (right - x);
  marqueeMax = max(marqueeMax, overflow);
  clipLeft = x;
  clipRight = right;
  drawText(d, text, x - marqueeOffset(overflow), y, color, tall);
  clipLeft = 0;
  clipRight = 64;
}

// A two digit number in the 5x7 font, each pixel 2x2, centred on cx. The convex corners are rounded, like the
// football clock: where a pixel has no neighbour on either side of a corner, that corner of its block is left out;
// where the only neighbour is diagonal, the two blocks are joined instead
static void drawBigDigit(MatrixPanel_I2S_DMA *d, int digit, int x, int y, uint16_t color, int sy) {
  const int sx = 2;  // each pixel is 2 wide and sy high
  auto lit = [digit](int k, int j) { return k >= 0 && k < 5 && j >= 0 && j < 7 && (DIGITS[digit][j] >> (4 - k) & 1); };
  for (int j = 0; j < 7; j++)
    for (int k = 0; k < 5; k++) {
      if (!lit(k, j)) continue;
      int bx = x + k * sx, by = y + j * sy;
      d->fillRect(bx, by, sx, sy, color);
      for (int dy = -1; dy <= 1; dy += 2)
        for (int dx = -1; dx <= 1; dx += 2)
          if (!lit(k + dx, j) && !lit(k, j + dy)) {
            int cx = dx < 0 ? bx : bx + sx - 1, cy = dy < 0 ? by : by + sy - 1;
            if (!lit(k + dx, j + dy)) {
              d->drawPixel(cx, cy, 0);
            } else {
              d->drawPixel(cx + dx, cy, color);
              d->drawPixel(cx, cy + dy, color);
            }
          }
    }
}

static void drawBigNumber(MatrixPanel_I2S_DMA *d, int value, int cx, int y, uint16_t color, int sy) {
  drawBigDigit(d, value / 10, cx - 11, y, color, sy);
  drawBigDigit(d, value % 10, cx + 1, y, color, sy);
}

// The letters of the banner are in the text font when they fit; a countdown or lap count stays in the small digits.
// The countdown is redrawn alone from x 33, so a text in the text font stays left of that when there is one.
// The banner's text: the 4 wide letters, with the digits of the time in the stretched 4 x 7 ones
static const int HEADER_SPACE = 2;  // a space in the header: with the letter's own gap, 3 blank columns (RED FLAG)
static int headerTextWidth(const char *s) {
  int w = 0;
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') { w += 5; continue; }
    const char *const *rows = narrowGlyph(*s);
    w += rows ? (int)strlen(rows[0]) + 1 : HEADER_SPACE;
  }
  return w ? w - 1 : 0;
}
// pulseColon: the text is the time of day, whose colon breathes like the clock's, down to bg (the header's fill)
static void drawHeaderMixed(MatrixPanel_I2S_DMA *d, const char *s, int x, int y, uint16_t color, bool pulseColon = false, uint16_t bg = 0) {
  const char *dots = strstr(s, "...");
  for (; *s; s++) {
    char one[2] = {*s, 0};
    if (*s >= '0' && *s <= '9') {
      drawStretchedDigits(d, one, x, y, color);
      x += 5;
    } else {
      if (loadingDotVisible(s, dots)) drawNarrowText(d, one, x, y, pulseColon && *s == ':' ? pulsed(color, bg) : color);
      x += headerTextWidth(one) + 1;
    }
  }
}
// While the times are still to come, a dot sits at the far right of the header and the text moves left to make room
int dotShift = 0;

static void drawHeader(MatrixPanel_I2S_DMA *d, const char *left, const char *right, uint16_t fill, uint16_t ink, uint16_t tab = 0, uint16_t rightInk = 0, bool rightTime = false, uint16_t leftInk = 0) {
  if (!rightInk) rightInk = ink;
  if (!leftInk) leftInk = ink;
  int h = shortPanel() ? 8 : 9;
  d->fillRect(0, 0, W, h, fill);
  if (tab) d->fillRect(0, 0, 2, h, tab);
  int x = tab ? 3 : 1;  // one pixel after the tab, or from the edge
  int edge = W - 1 - (dotShift ? dotShift - 1 : 0);  // one pixel from the right edge, or from the red stripe of the failed times
  if (x + headerTextWidth(left) + 3 + headerTextWidth(right) + (W - edge) <= W) {
    drawHeaderMixed(d, left, x, 1, leftInk);
    drawHeaderMixed(d, right, edge - headerTextWidth(right), 1, rightInk, rightTime, fill);
  } else if (x + headerTextWidth(left) + 3 + textWidth(right) + (W - edge) <= W) {  // the left in the larger font, only the right small
    drawHeaderMixed(d, left, x, 1, leftInk);
    drawHeaderText(d, right, edge - textWidth(right), 1, rightInk);
  } else {
    drawHeaderText(d, left, x, 1, leftInk);
    drawHeaderText(d, right, edge - textWidth(right), 1, rightInk);
  }
}

// Downloads for the times failed more than once: a red stripe at the far right of the header, like the tab on the left
bool timesFailed(const F1Ticker::Session &s) { return s.valid && s.timeFails >= 2; }

static void drawWaitDot(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s) {
  if (timesFailed(s)) d->fillRect(W - 2, 0, 2, shortPanel() ? 8 : 9, F1_RED);
}

// ---- The clock on the tall panel: the lap-time digits blown up ----

// Each pixel of a digit is 0..32: how much of it the letter covers. Less than SoftEdge.lo is nothing, more than
// SoftEdge.hi all of the colour, in between a part of it: the soft edge of the curves.
// The clock's digits are the 3x5 lap-time ones built at 10 x 18 pixels (about the same shape, strokes and bars 3 thick:
// each pixel of the 3x5 digit a bar or stem of that thickness, neighbouring ones joined), but for the 1: that is drawn by
// hand. The same at 13 x 23 (strokes 4 thick) for the main screen outside a race weekend, which has room for it.
// Kept as constants so they stay in flash, not in RAM.
struct GlyphSet { int w, h; const char *const *rows; };
static const char *const CLOCK_ROWS[10 * 18] = {
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX....XXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 0
  "....XXX...", "...XXXX...", "..XXXXX...", ".XX.XXX...", ".X..XXX...", "....XXX...",
  "....XXX...", "....XXX...", "....XXX...", "....XXX...", "....XXX...", "....XXX...",
  "....XXX...", "....XXX...", "....XXX...", ".XXXXXXXXX", ".XXXXXXXXX", ".XXXXXXXXX",  // 1
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX", ".......XXX",
  ".......XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX.......", "XXX.......",
  "XXX.......", "XXX.......", "XXX.......", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 2
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX", ".......XXX",
  ".......XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 3
  "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX",  // 4
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX.......", "XXX.......", "XXX.......",
  "XXX.......", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 5
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX.......", "XXX.......", "XXX.......",
  "XXX.......", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 6
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX", ".......XXX",  // 7
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX....XXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXX....XXX", "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 8
  "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXX....XXX", "XXX....XXX", "XXX....XXX",
  "XXX....XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX", ".......XXX", ".......XXX",
  ".......XXX", ".......XXX", ".......XXX", "XXXXXXXXXX", "XXXXXXXXXX", "XXXXXXXXXX",  // 9
};
static const char *const BIG_ROWS[10 * 23] = {
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 0
  ".....XXXX....", "....XXXXX....", "...XXXXXX....", "..XXXXXXX....", ".XXX.XXXX....", ".XX..XXXX....",
  ".X...XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....",
  ".....XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....", ".....XXXX....",
  ".....XXXX....", ".XXXXXXXXXXXX", ".XXXXXXXXXXXX", ".XXXXXXXXXXXX", ".XXXXXXXXXXXX",  // 1
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", "XXXX.........", "XXXX.........", "XXXX.........", "XXXX.........", "XXXX.........",
  "XXXX.........", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 2
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 3
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",  // 4
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXX.........", "XXXX.........",
  "XXXX.........", "XXXX.........", "XXXX.........", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 5
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXX.........", "XXXX.........",
  "XXXX.........", "XXXX.........", "XXXX.........", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 6
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",  // 7
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 8
  "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXX.....XXXX",
  "XXXX.....XXXX", "XXXX.....XXXX", "XXXX.....XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX", ".........XXXX",
  ".........XXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",  // 9
};
static const GlyphSet CLOCK_GLYPHS = {10, 18, CLOCK_ROWS};
static const GlyphSet BIG_GLYPHS = {13, 23, BIG_ROWS};
static bool bigClock = false;  // which of the two sizes is drawn
struct SoftEdge { float lo, hi; };
static const SoftEdge WHITE_SOFT = {0.2f, 0.8f};    // the hours
static const SoftEdge GREEN_SOFT = {0.25f, 0.75f};  // the minutes: the brighter colour looks blurrier

// How the digits change (the timeStyle setting, as on the football clock, without its flip cards):
// 1 fade, 2 roll up, 3 dissolve, 4 drift, 5 fade with a shimmer
static int timeStyle = 2;
static const int STYLE_FADE = 1, STYLE_ROLL = 2, STYLE_DISSOLVE = 3, STYLE_DRIFT = 4, STYLE_SHIMMER = 5;
static const unsigned long CHANGE_MS = layout::clockTransitionMs, CHANGE_STAGGER = layout::clockDigitStaggerMs;  // a digit takes this long; the digits go one after the other
static const int TILE_W = 10, TILE_H = 18;
static int tileW() { return bigClock ? 13 : TILE_W; }
static int tileH() { return bigClock ? 23 : TILE_H; }

static int clockDigits[4] = {0, 0, 0, 0}, oldDigits[4] = {0, 0, 0, 0};
static unsigned long changeAt[4] = {0, 0, 0, 0};
static bool clockStateKnown = false;
static bool tileSettled[4] = {true, true, true, true};  // drawn in its final form: no need to draw it again

// A colour at level/32 of its brightness
static uint16_t scaleColor(uint16_t c, int level) {
  return rgb((c >> 11 << 3) * level / 32, (c >> 5 & 63) * 4 * level / 32, (c & 31) * 8 * level / 32);
}

// A pseudo-random 0..1 for a pixel of a digit, to dissolve in a different order each time
static float scatter(int i, int j, int digit) {
  uint32_t h = i * 73856093u ^ j * 19349663u ^ digit * 83492791u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return (h % 1000) / 1000.0f;
}

// One digit in the middle of the w x h box at x, y: level is its brightness 0..32, dy moves it down (rows outside the
// box are not drawn), dissolve says how far it has dissolved in (+) or out (-) 0..1, 0 = not at all.
static void drawGlyph(MatrixPanel_I2S_DMA *d, int digit, int x, int y, uint16_t color, const SoftEdge &soft, int level, int dy, float dissolve) {
  const GlyphSet &set = bigClock ? BIG_GLYPHS : CLOCK_GLYPHS;
  int gx = x + (tileW() - set.w) / 2, gy = y + (tileH() - set.h) / 2 + dy;
  float lo = soft.lo * 32, scale = 32 / ((soft.hi - soft.lo) * 32);
  float sweep = timeStyle == STYLE_SHIMMER ? fmodf(millis() / 40.0f, 150) - 35 : 0;  // 25 px a second, and a pause
  for (int j = 0; j < set.h; j++) {
    int py = gy + j;
    if (py < y || py >= y + tileH()) continue;
    for (int i = 0; i < set.w; i++) {
      int a = (int)constrain(((set.rows[digit * set.h + j][i] == 'X' ? 32 : 0) - lo) * scale, 0.0f, 32.0f);
      if (!a) continue;
      float lv = level;
      if (dissolve != 0) {
        float v = constrain((fabsf(dissolve) - scatter(i, j, digit) * 0.6f) / 0.4f, 0.0f, 1.0f);
        lv *= dissolve > 0 ? v : 1 - v;
      }
      if (timeStyle == STYLE_SHIMMER) {  // a bright band slanting across the digits
        float k = fabsf((gx + i) + py * 0.6f - sweep) / 12;
        lv *= 0.8f + (k < 1 ? 0.2f * (1 - k) * (1 - k) : 0);
      }
      d->drawPixel(gx + i, py, scaleColor(color, (int)(a * lv / 32)));
    }
  }
}

// A changed timeStyle setting changes the last minute digit to itself once, so the new style is seen straight away
static bool sampling = false;
static int shownStyle = -1;

// How far the digit k has got in changing: 0..1, 1 when it is over, -1 while it waits for the digits after it
static float changeProgress(int k) {
  long t = (long)(millis() - changeAt[k]);
  return t < 0 ? -1 : t >= (long)CHANGE_MS ? 1 : t / (float)CHANGE_MS;
}

// One digit tile: the digit drawn, changing from oldDigits[k] to clockDigits[k], over whatever is behind it
static void drawTile(MatrixPanel_I2S_DMA *d, int k, int x, int y, uint16_t color, const SoftEdge &soft) {
  float p = changeProgress(k);  // (the screen is cleared and the background drawn before: nothing to clear here)
  if (p < 0) {
    drawGlyph(d, oldDigits[k], x, y, color, soft, 32, 0, 0);
  } else if (p >= 1 || (oldDigits[k] == clockDigits[k] && !(sampling && k == 3))) {
    drawGlyph(d, clockDigits[k], x, y, color, soft, 32, 0, 0);
  } else if (timeStyle == STYLE_ROLL) {
    int run = tileH() + 2, off = (int)roundf(p * p * (3 - 2 * p) * run);
    drawGlyph(d, oldDigits[k], x, y, color, soft, 32, -off, 0);
    drawGlyph(d, clockDigits[k], x, y, color, soft, 32, run - off, 0);
  } else if (timeStyle == STYLE_DISSOLVE) {
    drawGlyph(d, oldDigits[k], x, y, color, soft, 32, 0, -max(p, 0.001f));
    drawGlyph(d, clockDigits[k], x, y, color, soft, 32, 0, max(p, 0.001f));
  } else {  // fade, drift and shimmer: the old one out, then the new one in
    int drift = timeStyle == STYLE_DRIFT ? max(2, tileH() / 8) : 0;
    if (p < 0.5f) drawGlyph(d, oldDigits[k], x, y, color, soft, (int)(32 * (1 - 2 * p)), -(int)roundf(drift * 2 * p), 0);
    else drawGlyph(d, clockDigits[k], x, y, color, soft, (int)(32 * (2 * p - 1)), (int)roundf(drift * (2 - 2 * p)), 0);
  }
}

static const int TILE_OFFSET[4] = {0, 11, 28, 39};  // the hours and the minutes: 2 pixels either side of the colon
static const int BIG_OFFSET[4] = {0, 14, 33, 47};
static const int CLOCK_WIDTH = 49, BIG_WIDTH = 60;
static int clockWidth() { return bigClock ? BIG_WIDTH : CLOCK_WIDTH; }
static int clockLeft = (64 - CLOCK_WIDTH) / 2;  // where the clock starts: in the middle, or at the right on the results screen

// The four digits for the time: starts a change on the ones that differ
void updateClockDigits(int hour, int minute) {
  int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  timeStyle = Settings::getInstance()->timeStyle;
  if (timeStyle < 1 || timeStyle > 5) timeStyle = STYLE_ROLL;  // there are no flip cards on this clock
  if (sampling && changeProgress(3) >= 1) sampling = false;
  if (shownStyle >= 0 && timeStyle != shownStyle && clockStateKnown) {
    sampling = true;
    changeAt[3] = millis();
    tileSettled[3] = false;
  }
  shownStyle = timeStyle;
  if (!clockStateKnown) {
    for (int i = 0; i < 4; i++) clockDigits[i] = oldDigits[i] = digits[i];
    clockStateKnown = true;
    return;
  }
  int order = 0;
  for (int k = 3; k >= 0; k--)
    if (digits[k] != clockDigits[k]) {
      oldDigits[k] = clockDigits[k];
      clockDigits[k] = digits[k];
      changeAt[k] = millis() + order++ * CHANGE_STAGGER;
      tileSettled[k] = false;
    }
}

// Is a digit changing (or is it the shimmer, which moves all the time)?
bool clockMoving() {
  if (timeStyle == STYLE_SHIMMER) return true;
  for (int k = 0; k < 4; k++)
    if (changeProgress(k) < 1) return true;
  return false;
}

// What the backdrop shows at a pixel (black where there is none), so a colon can fade into it; see drawBackdrop()
static uint16_t backdropAt(int x, int y);
// A block of a clock's colon at the pulse's level, each pixel fading into what is behind it
static void colonBlock(MatrixPanel_I2S_DMA *d, int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) d->drawPixel(i, j, pulsed(WHITE, backdropAt(i, j)));
}

// The whole clock, or only the digits that move
static void drawTallClock(MatrixPanel_I2S_DMA *d, int y, bool onlyMoving) {
  for (int k = 0; k < 4; k++) {
    if (onlyMoving && timeStyle != STYLE_SHIMMER && tileSettled[k]) continue;
    drawTile(d, k, clockLeft + (bigClock ? BIG_OFFSET : TILE_OFFSET)[k], y, k < 2 ? CLOCK : MINUTES, k < 2 ? WHITE_SOFT : GREEN_SOFT);
    if (changeProgress(k) >= 1) tileSettled[k] = true;
  }
  if (bigClock) {
    colonBlock(d, clockLeft + 28, y + 6, 4, 4);
    colonBlock(d, clockLeft + 28, y + 13, 4, 4);
    return;
  }
  colonBlock(d, clockLeft + 23, y + 4, 3, 3);
  colonBlock(d, clockLeft + 23, y + 11, 3, 3);
}

// The clock: the small panel has the 5x7 digits two rows tall; the tall panel the soft ones above
static void drawClock(MatrixPanel_I2S_DMA *d, int hour, int minute, int y, bool tall = false) {
  if (tall) {
    drawTallClock(d, y, false);
    return;
  }
  drawBigNumber(d, hour, 17, y, CLOCK, 2);
  drawBigNumber(d, minute, 47, y, MINUTES, 2);
  colonBlock(d, 32, y + 4, 1, 1);
  colonBlock(d, 32, y + 9, 1, 1);
}


static const uint16_t FASTEST = rgb(190, 80, 255);  // purple, as on the timing screens
static const uint16_t KNOCKED_OUT = rgb(255, 90, 80);  // red-orange: the drivers who dropped out of a qualifying part
static const uint16_t IN_PIT = rgb(80, 200, 255);  // cyan: the driver is in the pit lane
static const uint16_t FAV_IN_PIT = rgb(70, 120, 255);  // a favourite in the pit lane: a deeper blue than the others
static bool tintKnockedOut = false;
static bool loadingTimes = false;  // show an ellipsis in an empty time column while data is still pending
bool loadingDotsShown = false;
static const char *loadingDots() {
  loadingDotsShown = true;
  return "...";  // measure all three dots; the text renderers animate visibility without changing width
}
static bool dimPlaces = false;  // the places shown are the ones kept in flash from before the boot: dimmed until new data is in
static uint16_t placeInk(uint16_t c) { return dimPlaces ? scaleColor(c, 12) : c; }
static bool showPit = true;  // off while the list of a finished part is drawn: everybody is in the pit lane  // set while the list after a qualifying part is drawn

// One position: place, team colour bar, driver code and, when known, the time or gap on the right.
// The first place's time is green for a race (LEAD) and purple for a best lap. Nothing for an empty place.
static void drawPlace(MatrixPanel_I2S_DMA *d, int place, const F1Ticker::Row &row, int y, bool race = false, int moved = 0, bool fav = false) {
  if (!row.code[0]) return;
  if (moved) d->fillRect(0, y - 1, W, 7, moved > 0 ? rgb(0, 80, 20) : rgb(50, 0, 4));  // gained or lost a place
  char number[3];
  snprintf(number, sizeof(number), "%d", place);
  drawText(d, number, place < 10 ? 4 : 2, y, placeInk(place == 1 ? GOLD : GREY));
  d->fillRect(10, y, 2, 5, placeInk(row.color));
  drawText(d, row.code, 14, y, placeInk(row.fastest ? FASTEST : fav ? (row.pit && showPit ? FAV_IN_PIT : GOLD) : row.pit && showPit ? IN_PIT : tintKnockedOut ? KNOCKED_OUT : WHITE));  // a favourite driver; the fastest lap in purple
  if (moved) {  // a little arrow between the code and the time
    uint16_t color = moved > 0 ? rgb(60, 230, 90) : rgb(255, 70, 60);
    int top = moved > 0 ? y : y + 4;
    int dy = moved > 0 ? 1 : -1;
    d->drawPixel(28, top, color);
    d->drawFastHLine(27, top + dy, 3, color);
    d->drawFastHLine(26, top + 2 * dy, 5, color);
  }
  if (!row.time[0] && loadingTimes) drawTimeRight(d, loadingDots(), W - 2, y, TIME_GREY);
  else drawTimeRight(d, race && !strcmp(row.time, "RETIRED") ? "DNF" : row.time, W - 2, y, placeInk(race && row.finished ? MINUTES : row.improved ? rgb(60, 230, 90) : place > 1 || !strcmp(row.time, "NO TIME") ? TIME_GREY : race ? MINUTES : FASTEST));  // green: a better lap just now
}

// Who moved up or down at the last change of the order, and until when it is shown
static const unsigned long MOVE_MS = 4000;
static struct Move { char code[4]; int8_t dir; unsigned long until; } moves[F1Ticker::ROWS];
static char lastOrder[F1Ticker::ROWS][4];
static uint32_t lastOrderSession = 0;

static int movedDir(const char *code) {
  for (const Move &m : moves)
    if (m.dir && (long)(m.until - millis()) > 0 && strcmp(m.code, code) == 0) return m.dir;
  return 0;
}

// Bit per row that has a marker now: when it changes, the list is drawn again
uint32_t moveMask(const F1Ticker::Session &s) {
  uint32_t mask = 0;
  for (int i = 0; i < F1Ticker::ROWS; i++)
    if (s.rows[i].code[0] && movedDir(s.rows[i].code)) mask |= 1UL << i;
  return mask;
}

// Bit per row whose best lap just got better: when it changes, the list is drawn again
uint32_t improvedMask(const F1Ticker::Session &s) {
  uint32_t mask = 0;
  for (int i = 0; i < F1Ticker::ROWS; i++)
    if (s.rows[i].code[0] && s.rows[i].improved) mask |= 1UL << i;
  return mask;
}

// Compares the order with the one before it. A big reshuffle is a new session or another source, not overtaking.
void trackMoves(const F1Ticker::Session &s) {
  int changed = 0, dirs[F1Ticker::ROWS] = {0};
  bool known = lastOrderSession == s.id && lastOrder[0][0];
  for (int i = 0; i < F1Ticker::ROWS; i++) {
    if (!s.rows[i].code[0] || !known) continue;
    for (int j = 0; j < F1Ticker::ROWS; j++)
      if (strcmp(lastOrder[j], s.rows[i].code) == 0) {
        if (j != i) { dirs[i] = j > i ? 1 : -1; changed++; }
        break;
      }
  }
  if (known && changed > 0) {
    unsigned long now = millis();
    for (int i = 0; i < F1Ticker::ROWS; i++) {
      if (!dirs[i]) continue;
      Move *slot = nullptr;
      for (Move &m : moves)
        if (strcmp(m.code, s.rows[i].code) == 0) { slot = &m; break; }
      if (!slot)
        for (Move &m : moves)
          if (!m.dir || (long)(m.until - now) <= 0) { slot = &m; break; }
      if (!slot) continue;
      strlcpy(slot->code, s.rows[i].code, sizeof(slot->code));
      slot->dir = dirs[i];
      slot->until = now + MOVE_MS;
    }
  }
  for (int i = 0; i < F1Ticker::ROWS; i++) strlcpy(lastOrder[i], s.rows[i].code, sizeof(lastOrder[i]));
  lastOrderSession = s.id;
}

// Local time parts of a UTC time
struct LocalTime { int weekday, hour, minute; };
static LocalTime localParts(ClockTime *dateTime, time_t utc) {
  time_t local = dateTime->utcToLocal(utc);
  return {(int)((local / SECS_PER_DAY + 4) % 7), (int)(local % SECS_PER_DAY / 3600), (int)(local % 3600 / 60)};
}

// The header's right side: laps in a race, the time left in practice and qualifying. Once that has sat at 0:00
// for 5 minutes, the time of day instead.
static const unsigned long OVER_MS = 5 * 60 * 1000UL;
unsigned long clockZeroAt = 0;  // since when the session clock reads 0:00
// The header shows the time of day instead of the countdown
bool timeOfDay(const F1Ticker::Session &s) { return s.tag != 'R' && s.tag != 'S' && clockZeroAt && millis() - clockZeroAt > OVER_MS; }

void liveRight(const F1Ticker::Session &s, char *right, size_t size, int hour, int minute) {
  right[0] = 0;
  if (s.tag == 'R' || s.tag == 'S') {
    if (s.period > 0 && s.flag == 'r') snprintf(right, size, "L%d", s.period);  // red flag: the lap only, so RED FLAG and the lap both fit in the larger font
    else if (s.period > 0 && s.totalLaps > 0) snprintf(right, size, "L%d/%d", s.period, s.totalLaps);
    else if (s.period > 0) snprintf(right, size, "L%d/%s", s.period, loadingDots());
    else strlcpy(right, loadingDots(), size);
  } else if (clockZeroAt && millis() - clockZeroAt > OVER_MS) {
    snprintf(right, size, "%d:%02d", hour, minute);
  } else {
    strlcpy(right, s.clock[0] ? s.clock : loadingDots(), size);
  }
}

// The chequered flag behind a list: squares of 8 pixels, seven rows of eight under the 8 pixel header
static void flagFill(MatrixPanel_I2S_DMA *d, int x, int y, int w, int h) {
  d->fillRect(x, y, w, h, 0);
  int top = max(y, 8), bottom = y + h;
  for (int row = (top - 8) / 8; 8 + row * 8 < bottom; row++)
    for (int col = 0; col < 8; col++) {
      if (!((row + col) & 1)) continue;
      int x0 = max(x, col * 8), x1 = min(x + w, col * 8 + 8), y0 = max(top, 8 + row * 8), y1 = min(bottom, 16 + row * 8);
      if (x1 > x0 && y1 > y0) d->fillRect(x0, y0, x1 - x0, y1 - y0, FLAG_TILE);
    }
}


// The favourite drivers of the settings, in the simulator too
static bool isFavourite(const char *code) {
  const String &favs = Settings::getInstance()->f1Drivers;
  return code[0] && favs.indexOf(code) >= 0;
}

// ---- The dim background of the main screens: the flag of the host country of the Grand Prix, drawn from a few stripes
// and shapes ----
static int bdTop = 0, bdBottom = 64;  // only these rows are drawn
bool backdropShown = false;  // the frame being drawn has the backdrop (set by drawBackdrop, cleared for each new frame)
uint64_t tileRows = 0;  // the rows chequer() filled in this frame, one bit each
// Probing: the flag is "drawn" without a display, only to learn the colour that would end up at one pixel
static int bdProbeX = -1, bdProbeY = -1;
static uint16_t bdProbeColor = 0;
// The flag's colours at the lowest level the display shows: a channel is on (at level 1) or off, so the strong
// channels of a colour keep its hue (brown and maroon turn red, which is all that level 1 allows)
static uint16_t dimRgb(int r, int g, int b) { return rgb(r >= 128 ? MIN_LIT : 0, g >= 128 ? MIN_LIT : 0, b >= 128 ? MIN_LIT : 0); }
static void bdPixel(MatrixPanel_I2S_DMA *d, int x, int y, uint16_t c) {
  if (bdProbeX >= 0) {
    if (x == bdProbeX && y == bdProbeY && y >= bdTop && y < bdBottom) bdProbeColor = c;
    return;
  }
  if (y >= bdTop && y < bdBottom) d->drawPixel(x, y, c);
}
static void bdRect(MatrixPanel_I2S_DMA *d, int x, int y, int w, int h, uint16_t c) {
  int y0 = max(y, bdTop), y1 = min(y + h, bdBottom);
  if (bdProbeX >= 0) {
    if (bdProbeX >= x && bdProbeX < x + w && bdProbeY >= y0 && bdProbeY < y1) bdProbeColor = c;
    return;
  }
  if (y1 > y0 && w > 0) d->fillRect(x, y0, w, y1 - y0, c);
}
static void bdDisc(MatrixPanel_I2S_DMA *d, int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r / 2) bdPixel(d, cx + x, cy + y, c);
}

// A filled polygon of points relative to (cx, cy), by scan lines
static void bdPolygon(MatrixPanel_I2S_DMA *d, const int8_t (*pts)[2], int n, int cx, int cy, uint16_t c) {
  int ymin = 99, ymax = -99;
  for (int i = 0; i < n; i++) { ymin = min(ymin, (int)pts[i][1]); ymax = max(ymax, (int)pts[i][1]); }
  for (int y = ymin; y <= ymax; y++) {
    int xs[16], m = 0;
    for (int i = 0; i < n && m < 16; i++) {
      int x0 = pts[i][0], y0 = pts[i][1], x1 = pts[(i + 1) % n][0], y1 = pts[(i + 1) % n][1];
      if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) xs[m++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
    }
    for (int i = 1; i < m; i++)
      for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) { int t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
    for (int i = 0; i + 1 < m; i += 2) bdRect(d, cx + xs[i], cy + y, xs[i + 1] - xs[i] + 1, 1, c);
  }
}

// The maple leaf of the Canadian flag, about 24 x 26
static const int8_t MAPLE[][2] = {{0, -13}, {2, -8}, {5, -9}, {4, -4}, {9, -6}, {8, -2}, {12, -1}, {9, 2}, {10, 5}, {5, 4}, {1, 6},
                                   {1, 12}, {-1, 12}, {-1, 6}, {-5, 4}, {-10, 5}, {-9, 2}, {-12, -1}, {-8, -2}, {-9, -6}, {-4, -4},
                                   {-5, -9}, {-2, -8}};

enum Country { C_NONE, C_AU, C_CN, C_JP, C_US, C_CA, C_MC, C_ES, C_AT, C_GB, C_BE, C_HU, C_NL, C_IT, C_AZ, C_SG, C_MX, C_BR, C_QA, C_AE, C_BH, C_SA, C_DE, C_FR, C_TR, C_RU, C_PT, C_TH, C_ID, C_MY, C_CZ, C_SM, C_AR };
// Words in ESPN's circuit name and city, lower case, then the country
static const struct { const char *words; Country country; } COUNTRIES[] = {
  {"albert park|melbourne", C_AU}, {"shanghai", C_CN}, {"suzuka", C_JP}, {"miami|americas|austin|las vegas", C_US},
  {"villeneuve|montreal", C_CA}, {"monaco|monte carlo", C_MC}, {"catalunya|barcelona|madring|madrid", C_ES},
  {"red bull ring|spielberg", C_AT}, {"silverstone", C_GB}, {"francorchamps", C_BE}, {"hungaroring|budapest", C_HU},
  {"zandvoort", C_NL}, {"monza|imola|mugello", C_IT}, {"baku", C_AZ}, {"marina bay|singapore", C_SG},
  {"hermanos|mexico", C_MX}, {"interlagos|paulo", C_BR}, {"lusail|losail", C_QA}, {"yas |abu dhabi", C_AE},
  {"sakhir|bahrain", C_BH}, {"jeddah", C_SA}, {"rburgring|hockenheim", C_DE}, {"ricard", C_FR}, {"istanbul", C_TR},
  {"sochi", C_RU}, {"algarve|portim", C_PT},
  // The MotoGP calendar: the circuit and its place are followed by the country
  {"thailand|buriram", C_TH}, {"indonesia|mandalika|lombok", C_ID}, {"malaysia|sepang", C_MY}, {"czech|brno", C_CZ},
  {"san marino|misano", C_SM}, {"argentina|termas", C_AR}, {"japan|motegi", C_JP}, {"australia|phillip island", C_AU},
  {"united sta|cota", C_US}, {"spain|jerez|aragon|alca|valencia|tormo", C_ES}, {"portugal", C_PT}, {"france|le mans", C_FR},
  {"germany|sachsenring", C_DE}, {"netherlands|assen", C_NL}, {"hungary|balaton", C_HU}, {"austria", C_AT},
  {"united kin|great britain", C_GB}, {"qatar", C_QA}, {"brazil|goiania", C_BR}, {"italy", C_IT},
};
static Country countryFor(const char *text) {
  char lower[80];
  size_t n = 0;
  for (; text[n] && n < sizeof(lower) - 1; n++) lower[n] = text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n];
  lower[n] = 0;
  for (const auto &c : COUNTRIES) {
    const char *w = c.words;
    while (*w) {
      const char *end = strchr(w, '|');
      size_t len = end ? end - w : strlen(w);
      char word[24];
      memcpy(word, w, len);
      word[len] = 0;
      if (strstr(lower, word)) return c.country;
      w += len + (end ? 1 : 0);
    }
  }
  return C_NONE;
}

// The flag, edge to edge (64 x 40) from y Y; each flag as a few stripes and one or two small shapes
static void drawFlag(MatrixPanel_I2S_DMA *d, Country country, int Y) {
  const int X = 0, W2 = 64, H = 40;
  const uint16_t WH = dimRgb(255, 255, 255), RD = dimRgb(225, 30, 40), BL = dimRgb(30, 70, 200), GR = dimRgb(20, 150, 70),
                 YE = dimRgb(255, 205, 0), BK = dimRgb(0, 0, 0);
  auto h3 = [&](uint16_t a, uint16_t b, uint16_t c) { bdRect(d, X, Y, W2, H / 3, a); bdRect(d, X, Y + H / 3, W2, H / 3, b); bdRect(d, X, Y + 2 * (H / 3), W2, H - 2 * (H / 3), c); };
  auto v3 = [&](uint16_t a, uint16_t b, uint16_t c) { bdRect(d, X, Y, W2 / 3, H, a); bdRect(d, X + W2 / 3, Y, W2 / 3, H, b); bdRect(d, X + 2 * (W2 / 3), Y, W2 - 2 * (W2 / 3), H, c); };
  auto h2 = [&](uint16_t a, uint16_t b) { bdRect(d, X, Y, W2, H / 2, a); bdRect(d, X, Y + H / 2, W2, H - H / 2, b); };
  auto union_jack = [&](int x, int y, int w, int h) {
    bdRect(d, x, y, w, h, BL);
    for (int i = 0; i < w; i++) {  // the diagonals
      int j = i * h / w;
      for (int t = -1; t <= 1; t++) { bdPixel(d, x + i, y + j + t, WH); bdPixel(d, x + i, y + h - 1 - j + t, WH); }
    }
    bdRect(d, x, y + h / 2 - 3, w, 6, WH);
    bdRect(d, x + w / 2 - 3, y, 6, h, WH);
    bdRect(d, x, y + h / 2 - 1, w, 2, RD);
    bdRect(d, x + w / 2 - 1, y, 2, h, RD);
  };
  auto crescent = [&](int cx, int cy, int r, uint16_t on, uint16_t off) { bdDisc(d, cx, cy, r, on); bdDisc(d, cx + r / 2 + 1, cy, r - 1, off); };
  switch (country) {
    case C_AU: bdRect(d, X, Y, W2, H, BL); union_jack(X, Y, W2 / 2, H / 2);
      bdRect(d, X + 12, Y + 24, 3, 3, WH);
      for (int k = 0; k < 4; k++) bdRect(d, X + 42 + (k % 2) * 12, Y + 6 + (k / 2) * 16 + (k == 1 ? 6 : 0), 2, 2, WH);
      bdRect(d, X + 52, Y + 22, 2, 2, WH);
      break;
    case C_CN: bdRect(d, X, Y, W2, H, RD); bdDisc(d, X + 9, Y + 8, 4, YE); break;
    case C_JP: bdRect(d, X, Y, W2, H, WH); bdDisc(d, X + W2 / 2, Y + H / 2, 10, RD); break;
    case C_US:
      for (int k = 0; k < 13; k++) bdRect(d, X, Y + k * H / 13, W2, (k + 1) * H / 13 - k * H / 13, k % 2 ? WH : RD);
      bdRect(d, X, Y, W2 * 2 / 5, H * 7 / 13, BL);
      for (int r = 0; r < 5; r++) for (int c = 0; c < 6; c++) bdRect(d, X + 3 + c * 4, Y + 3 + r * 4, 1, 1, WH);
      break;
    case C_CA: bdRect(d, X + 16, Y, W2 - 32, H, WH); bdRect(d, X, Y, 16, H, RD); bdRect(d, X + W2 - 16, Y, 16, H, RD);
      bdPolygon(d, MAPLE, sizeof(MAPLE) / sizeof(MAPLE[0]), X + W2 / 2, Y + H / 2, RD);
      break;
    case C_MC: h2(RD, WH); break;
    case C_ES: bdRect(d, X, Y, W2, H, RD); bdRect(d, X, Y + H / 4, W2, H / 2, YE); break;
    case C_AT: h3(RD, WH, RD); break;
    case C_GB: union_jack(X, Y, W2, H); break;
    case C_BE: v3(BK, YE, RD); break;
    case C_HU: h3(RD, WH, GR); break;
    case C_NL: h3(RD, WH, BL); break;
    case C_IT: v3(GR, WH, RD); break;
    case C_AZ: h3(dimRgb(0, 150, 200), RD, GR); crescent(X + 27, Y + H / 2, 6, WH, RD); bdRect(d, X + 37, Y + H / 2 - 1, 3, 3, WH); break;
    case C_SG: h2(RD, WH);
      crescent(X + 12, Y + 9, 7, WH, RD);  // the crescent, with the five stars in a circle in its opening
      for (int k = 0; k < 5; k++) {
        float angle = (-90 + k * 72) * 3.14159f / 180;
        int sx = X + 16 + (int)roundf(4.2f * cosf(angle)), sy = Y + 9 + (int)roundf(4.2f * sinf(angle));
        bdPixel(d, sx, sy, WH); bdPixel(d, sx - 1, sy, WH); bdPixel(d, sx + 1, sy, WH); bdPixel(d, sx, sy - 1, WH); bdPixel(d, sx, sy + 1, WH);
      }
      break;
    case C_MX: v3(GR, WH, RD); bdDisc(d, X + W2 / 2, Y + H / 2, 8, dimRgb(150, 100, 40)); break;
    case C_BR: bdRect(d, X, Y, W2, H, GR);
      for (int j = -17; j <= 17; j++) bdRect(d, X + W2 / 2 - (28 - abs(j) * 28 / 18), Y + H / 2 + j, 2 * (28 - abs(j) * 28 / 18), 1, YE);
      bdDisc(d, X + W2 / 2, Y + H / 2, 9, BL);
      break;
    case C_QA: bdRect(d, X, Y, W2, H, dimRgb(140, 20, 60)); bdRect(d, X, Y, 17, H, WH);
      for (int k = 0; k < 10; k++) bdRect(d, X + 17, Y + k * 4, 4 - (k % 2) * 3, 4, WH);
      break;
    case C_AE: bdRect(d, X, Y, 14, H, RD); bdRect(d, X + 14, Y, W2 - 14, 13, GR); bdRect(d, X + 14, Y + 13, W2 - 14, 13, WH); bdRect(d, X + 14, Y + 26, W2 - 14, 13, BK); break;
    case C_BH: bdRect(d, X, Y, W2, H, RD); bdRect(d, X, Y, 17, H, WH);
      for (int k = 0; k < 10; k++) bdRect(d, X + 17, Y + k * 4, 4 - (k % 2) * 3, 4, WH);
      break;
    case C_SA: bdRect(d, X, Y, W2, H, GR); bdRect(d, X + 14, Y + 12, 36, 2, WH); bdRect(d, X + 14, Y + 26, 36, 2, WH); break;
    case C_DE: h3(BK, RD, YE); break;
    case C_FR: v3(BL, WH, RD); break;
    case C_TR: bdRect(d, X, Y, W2, H, RD); crescent(X + 24, Y + H / 2, 8, WH, RD); bdRect(d, X + 36, Y + H / 2 - 1, 3, 3, WH); break;
    case C_RU: h3(WH, BL, RD); break;
    case C_TH: bdRect(d, X, Y, W2, H, RD); bdRect(d, X, Y + 7, W2, 26, WH); bdRect(d, X, Y + 13, W2, 14, BL); break;
    case C_ID: h2(RD, WH); break;
    case C_MY:
      for (int k = 0; k < 14; k++) bdRect(d, X, Y + k * H / 14, W2, (k + 1) * H / 14 - k * H / 14, k % 2 ? WH : RD);
      bdRect(d, X, Y, 32, 22, BL); crescent(X + 11, Y + 11, 7, YE, BL); bdRect(d, X + 21, Y + 10, 3, 3, YE);
      break;
    case C_CZ: h2(WH, RD);
      for (int x = 0; x < 28; x++) { int half = (28 - x) * H / 2 / 28; bdRect(d, X + x, Y + H / 2 - half, 1, 2 * half, BL); }
      break;
    case C_SM: h2(WH, dimRgb(90, 190, 240)); break;
    case C_AR: h3(dimRgb(110, 190, 240), WH, dimRgb(110, 190, 240)); bdDisc(d, X + W2 / 2, Y + H / 2, 4, YE); break;
    case C_PT: bdRect(d, X, Y, 25, H, GR); bdRect(d, X + 25, Y, W2 - 25, H, RD); bdDisc(d, X + 25, Y + H / 2, 7, YE); break;
    default: break;
  }
}

// The flag of the host country of this weekend's Grand Prix, or else of the next one; none when the circuit is not known
static Country backdropCountry() {
  if (shortPanel() || !Settings::getInstance()->f1Flags) return C_NONE;  // an option, off unless chosen
  const char *circuit = snap.weekend && snap.circuit[0] ? snap.circuit : snap.upcomingCount ? snap.upcoming[0].circuit : "";
  return countryFor(circuit);
}

// Where the flag starts: 40 rows from here, so its bottom is the row above the bottom bar (y 56)
static const int FLAG_TOP = 16;

// The chequered flag of the results over the rows y0 to y1 (not included): squares of 8, the same everywhere
static void chequer(MatrixPanel_I2S_DMA *d, int y0, int y1) {
  for (int y = max(y0, 0); y < min(y1, 64); y++) tileRows |= 1ULL << y;  // so a colon over them fades into the squares
  for (int block = y0 / 8; block * 8 < y1; block++)
    for (int col = 0; col < W / 8; col++) {
      if ((block + col) & 1) continue;
      int from = max(y0, block * 8), to = min(y1, block * 8 + 8);
      if (to > from) d->fillRect(col * 8, from, 8, to - from, FLAG_TILE);
    }
}

// The background, in the rows from top to bottom: the chequered flag behind the clock, then the country's flag (or more chequers)
static void drawBackdrop(MatrixPanel_I2S_DMA *d, int top, int bottom) {
  if (shortPanel()) return;
  backdropShown = true;
  bdTop = top;
  bdBottom = bottom;
  chequer(d, max(top, 0), min(bottom, FLAG_TOP));
  Country country = backdropCountry();
  if (country != C_NONE) {
    drawFlag(d, country, FLAG_TOP);
  } else {
    chequer(d, max(top, FLAG_TOP), min(bottom, 56));
  }
}

static uint16_t backdropAt(int x, int y) {
  if (y < 0 || y >= 64) return 0;
  if (tileRows >> y & 1) return ((y / 8 + x / 8) & 1) ? 0 : FLAG_TILE;  // the squares, as chequer() draws them
  if (!backdropShown || shortPanel() || y < bdTop || y >= bdBottom) return 0;
  Country country = backdropCountry();
  if (country == C_NONE || y < FLAG_TOP) return 0;
  bdProbeX = x, bdProbeY = y, bdProbeColor = 0;
  drawFlag(nullptr, country, FLAG_TOP);  // only the probe: nothing is drawn
  bdProbeX = -1;
  return bdProbeColor;
}

// The bottom bar: one row of the same squares, from the divider's row down
static void drawBottomChequers(MatrixPanel_I2S_DMA *d) {
  d->fillRect(0, 56, W, 8, 0);
  chequer(d, 56, 64);
}

// The dim background of a live list, in the rows from top to bottom (under the header): the chequered flag, as behind
// the results
static void drawLiveBackdrop(MatrixPanel_I2S_DMA *d, int top, int bottom) {
  if (shortPanel()) return;
  chequer(d, max(top, 9), bottom);  // the tall panel's header occupies rows 0 through 8
}

// Is the flag out? A race: as soon as the leader has finished; practice and qualifying: when the clock reads 0:00
static bool flagOn(const F1Ticker::Session &s) {
  if (shortPanel()) return false;
  if (s.tag == 'R' || s.tag == 'S') return s.finished;
  return strcmp(s.clock, "0:00") == 0;
}

// Rows that change place, and the pages of a result list, glide to where they go instead of jumping
static const unsigned long GLIDE_MS = 350;
static float glideEase(unsigned long since) {
  if (since >= GLIDE_MS) return 1;
  float p = (float)since / GLIDE_MS;
  return p * p * (3 - 2 * p);
}

// The live list: where each driver was (from) and is going (to), by code, and when the move started
struct GlideSlot { char code[4]; int y; };
static GlideSlot glideFrom[F1Ticker::ROWS], glideTo[F1Ticker::ROWS];
int glideFromN = 0, glideToN = 0;
static unsigned long glideAt = 0;

static int glideEnterY = 0;  // where a driver who was not in the list comes in from: just below it

static int glideStart(const char *code, int fallback) {
  for (int i = 0; i < glideFromN; i++)
    if (strcmp(glideFrom[i].code, code) == 0) return glideFrom[i].y;
  return glideFromN ? glideEnterY : fallback;
}

// Where a driver is drawn now, on the way from glideFrom to glideTo
static int glideNow(const char *code, int target) {
  return glideStart(code, target) + (int)roundf((target - glideStart(code, target)) * glideEase(millis() - glideAt));
}

// A new layout for the live list: the drivers glide from where they are drawn now
static void glideTo_(const GlideSlot *layout, int count) {
  bool same = count == glideToN;
  for (int i = 0; same && i < count; i++) same = layout[i].y == glideTo[i].y && strcmp(layout[i].code, glideTo[i].code) == 0;
  if (same) return;
  GlideSlot shown[F1Ticker::ROWS];
  for (int i = 0; i < glideToN; i++) {
    strcpy(shown[i].code, glideTo[i].code);
    shown[i].y = glideNow(glideTo[i].code, glideTo[i].y);
  }
  memcpy(glideFrom, shown, sizeof(glideFrom));
  glideFromN = glideToN;
  memcpy(glideTo, layout, sizeof(GlideSlot) * count);
  glideToN = count;
  glideAt = millis();
}

// A page of a result list: the rows of the page before slide up and out while the next ones come in
PageSlide slideList, slideBetween;
static void slideSeen(PageSlide &s, int first) {
  if (s.cur < 0) { s.cur = first; s.at = 0; }
  else if (first != s.cur) { s.prev = s.cur; s.cur = first; s.at = millis(); }
}
bool slideActive(const PageSlide &s) { return s.at && millis() - s.at < GLIDE_MS + 60; }
static int slideShift(const PageSlide &s, int total) { return slideActive(s) ? (int)roundf(total * glideEase(millis() - s.at)) : 0; }

bool glideActive() { return glideToN && millis() - glideAt < GLIDE_MS + 60; }

// The flag shown: a yellow one that is still out when the session is over (the leader has finished) is not shown
char shownFlag(const F1Ticker::Session &s) { return s.finished && (s.flag == 'y' || s.flag == 's' || s.flag == 'v') ? 0 : s.flag; }

static const char *liveLabel(const F1Ticker::Session &s) {
  char flag = shownFlag(s);
  return flag == 'r' ? "RED FLAG" : flag == 's' ? "SC" : flag == 'v' ? "VSC" : s.name;
}

static void liveColors(const F1Ticker::Session &s, uint16_t &fill, uint16_t &ink, uint16_t &tab) {
  tab = 0;
  char flag = shownFlag(s);
  if (flag == 'y' || flag == 's' || flag == 'v') { fill = GOLD; ink = BLACK; }
  else if (flag == 'r') { fill = RED_FLAG_BAR; ink = WHITE; }
  else { fill = HEADER_NAVY; ink = WHITE; tab = s.tag == 'P' ? TAB_QUIET : F1_RED; }
}

// How many places have a driver
int filledRows(const F1Ticker::Session &s) {
  int n = 0;
  for (int i = 0; i < F1Ticker::ROWS; i++)
    if (s.rows[i].code[0]) n = i + 1;
  return n;
}

// Qualifying: which part it is (Q1 or SQ1 give 1), 0 for anything else
int qPart(const F1Ticker::Session &s) {
  const char *digit = s.name[0] == 'Q' ? s.name + 1 : s.name[0] == 'S' && s.name[1] == 'Q' ? s.name + 2 : "";
  return atoi(digit);
}

// The places of the list that is shown: in the second part of qualifying only the 16 who got through, in the third the 10
static int shownRows(const F1Ticker::Session &s) {
  int n = filledRows(s), part = s.tag == 'Q' ? qPart(s) : 0;
  return part == 2 ? min(n, 16) : part == 3 ? min(n, 10) : n;
}

// Which places to show on `slots` lines: the top three, every favourite, then the places around the best placed
// favourite (or just on from the top when there is none, or when that favourite is in the top three anyway).
// A favourite outside the top nine is kept with its immediate neighbours, as a complete group.
// Returns how many, in order, in `out`.
static int pickPlaces(const F1Ticker::Session &s, int slots, int *out) {
  bool chosen[F1Ticker::ROWS] = {false};
  int n = shownRows(s), count = 0, best = -1;
  auto take = [&](int i) {
    if (i < 0 || i >= n || chosen[i] || count >= slots) return;
    int first = i, last = i;
    if (i >= 9 && isFavourite(s.rows[i].code)) {
      first = i - 1;
      last = min(i + 1, n - 1);
      // Adjacent favourites need their neighbours too; extend the group rather than cutting one off.
      while (first >= 9 && isFavourite(s.rows[first].code)) first--;
      while (last < n - 1 && isFavourite(s.rows[last].code)) last++;
    }
    int needed = 0;
    for (int j = first; j <= last; j++) if (!chosen[j]) needed++;
    if (count + needed > slots) return;
    for (int j = first; j <= last; j++)
      if (!chosen[j]) { chosen[j] = true; count++; }
  };
  for (int i = 0; i < 3; i++) take(i);
  for (int i = 0; i < n; i++)
    if (isFavourite(s.rows[i].code)) {
      if (best < 0) best = i;
      take(i);
    }
  if (best >= 3) {
    for (int dist = 1; count < slots && dist < n; dist++) {
      take(best - dist);
      take(best + dist);
    }
  }
  for (int i = 0; count < slots && i < n; i++) take(i);
  int m = 0;
  for (int i = 0; i < n; i++)
    if (chosen[i]) out[m++] = i;
  return m;
}

// A row of the live list as it is drawn: practice and qualifying say so when a driver has no time yet
static F1Ticker::Row liveRow(const F1Ticker::Session &s, int i) {
  F1Ticker::Row row = s.rows[i];
  row.finished = s.fromFeed && row.finished;  // cached rows cannot confirm an individual finish
  if (i == 0 && s.live && !s.finished && (s.tag == 'R' || s.tag == 'S'))
    strlcpy(row.time, showIntervals ? "INT" : "GAP", sizeof(row.time));
  bool answered = s.fromFeed || (!s.timeFails && s.rows[0].time[0]);  // the feed, or ESPN's download that went through
  if (answered && (s.tag == 'P' || s.tag == 'Q') && row.code[0] && !row.time[0]) strlcpy(row.time, "NO TIME", sizeof(row.time));
  return row;
}

// A qualifying part that is over: the top three stay, the places after them scroll a page at a time
// The timer at 0 is not the end: the drivers on a lap finish it. The list scrolls once every car is back in the pit lane
// (or after three minutes, for a car that stopped on the track).
unsigned long qFinishedAt = 0;  // when the part was first seen over, 0 = not over
bool scrollsAfterQ(const F1Ticker::Session &s) {
  if (shortPanel() || s.tag != 'Q' || !s.finished || !qFinishedAt || shownRows(s) <= 9) return false;
  if (millis() - qFinishedAt > 180000) return true;
  for (int i = 0; i < F1Ticker::ROWS; i++)
    if (s.rows[i].code[0] && !s.rows[i].pit) return false;
  return true;
}
int scrollPages(const F1Ticker::Session &s) { return max(1, (shownRows(s) - 3 + 5) / 6); }

// Q3 while it runs: ten places do not fit under the header, so the top three stay and places 4 to 10 scroll up and down
// a row at a time. Returns how many rows the list can move.
int q3Scroll(const F1Ticker::Session &s) {
  if (shortPanel() || s.tag != 'Q' || qPart(s) != 3 || s.finished) return 0;
  return max(0, shownRows(s) - 9);
}
// Where the scrolling list is: 0 to q3Scroll, a row every page time, there and back
int q3Pos(int extra) {
  int period = 2 * extra, t = (millis() / pageMs()) % period;
  return t <= extra ? t : period - t;
}

static void drawLiveScroll(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s, const char *right, uint16_t fill, uint16_t ink, uint16_t tab, int page, uint16_t rightInk = 0) {
  dimPlaces = s.stale;
  loadingTimes = s.live || (!s.fromFeed && (!s.rows[0].time[0] || s.timeFails));
  const int SLOTS = 6, TOP = 10;
  int n = shownRows(s);
  bool step = q3Scroll(s) > 0;  // a row at a time, the page is the row
  int first = step ? 3 + page : min(3 + page * SLOTS, n - SLOTS);
  slideSeen(slideList, first);
  bool flag = flagOn(s);
  if (flag) flagFill(d, 0, 8, W, 56);
  else drawLiveBackdrop(d, 8, 64);
  bool sliding = slideActive(slideList);
  if (sliding && first > slideList.prev && first - slideList.prev < SLOTS) step = true;  // the last page moves only as far as it is short
  int shift = slideShift(slideList, SLOTS * 6);
  // The last six are the ones who dropped out of the part (Q1: the 17th to 22nd, Q2: the 11th to 16th, the list then holds 16); nobody drops out of Q3
  int part = qPart(s);
  int outFrom = part == 1 || part == 2 ? n - 6 : n, outTo = n;
  auto knocked = [&](int i) { return i >= outFrom && i < outTo; };
  if (step) {  // rows i from the old first to the new one, each where it is on the way
    int lo = sliding ? min(slideList.prev, first) : first, hi = sliding ? max(slideList.prev, first) : first;
    int offset = sliding ? (int)roundf((first - slideList.prev) * 6 * (1 - glideEase(millis() - slideList.at))) : 0;
    for (int i = lo; i < hi + SLOTS && i < n; i++) {
      tintKnockedOut = knocked(i);
      drawPlace(d, i + 1, liveRow(s, i), TOP + 3 * 6 + (i - first) * 6 + offset, false, 0, isFavourite(s.rows[i].code));
    }
  }
  for (int slot = 0; !step && slot < SLOTS; slot++) {  // the scrolling places go first: the header and the top three are drawn over what slides out
    int i = first + slot, y = TOP + (3 + slot) * 6;
    if (sliding) {
      int before = slideList.prev + slot;
      if (before < n) { tintKnockedOut = knocked(before); drawPlace(d, before + 1, liveRow(s, before), y - shift, false, 0, isFavourite(s.rows[before].code)); }
      if (i < n) { tintKnockedOut = knocked(i); drawPlace(d, i + 1, liveRow(s, i), y + SLOTS * 6 - shift, false, 0, isFavourite(s.rows[i].code)); }
    } else if (i < n) {
      tintKnockedOut = knocked(i);
      drawPlace(d, i + 1, liveRow(s, i), y, false, 0, isFavourite(s.rows[i].code));
    }
  }
  tintKnockedOut = false;
  if (sliding) {
    d->fillRect(0, 0, W, TOP + 3 * 6 - 1, 0);
    if (flag) flagFill(d, 0, 8, W, TOP + 3 * 6 - 9);
    else drawLiveBackdrop(d, 8, TOP + 3 * 6 - 1);
  }
  drawHeader(d, liveLabel(s), right, fill, ink, tab, rightInk, rightInk != 0, s.live && !shownFlag(s) ? LIVE_GREEN : 0);  // a right side in its own colour is the time of day
  drawWaitDot(d, s);
  for (int k = 0; k < 3; k++) drawPlace(d, k + 1, liveRow(s, k), TOP + k * 6, false, 0, isFavourite(s.rows[k].code));
  showPit = true;
}

void drawLive(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s, int hour, int minute, int page) {
  dimPlaces = s.stale;
  loadingTimes = s.live || (!s.fromFeed && (!s.rows[0].time[0] || s.timeFails));
  // Marked live, but nothing is happening yet (a delayed start): say so, with the time to look at meanwhile
  if (s.rows[0].code[0] == 0) {
    drawHeader(d, s.name, "LOADING", HEADER_NAVY, WHITE, TAB_QUIET, 0, false, s.live ? LIVE_GREEN : 0);
    clockLeft = (W - clockWidth()) / 2;
    drawClock(d, hour, minute, shortPanel() ? 14 : 24, !shortPanel());
    return;
  }
  char right[12];
  liveRight(s, right, sizeof(right), hour, minute);
  uint16_t fill, ink, tab;
  liveColors(s, fill, ink, tab);
  if (scrollsAfterQ(s) || q3Scroll(s) > 0) {
    drawLiveScroll(d, s, right, fill, ink, tab, page, timeOfDay(s) ? MINUTES : 0);
    return;
  }
  drawHeader(d, liveLabel(s), right, fill, ink, tab, 0, timeOfDay(s), s.live && !shownFlag(s) ? LIVE_GREEN : 0);
  drawWaitDot(d, s);
  bool flag = flagOn(s);
  if (flag) flagFill(d, 0, shortPanel() ? 8 : 9, W, shortPanel() ? 56 : 55);  // below the header
  else drawLiveBackdrop(d, 8, 64);
  bool race = s.tag == 'R' || s.tag == 'S';
  int shown = shortPanel() ? 4 : 9;  // 32 rows: from y 9, the last one ends on the bottom row
  int top = shortPanel() ? 9 : 10;
  int places[F1Ticker::ROWS];
  int count = pickPlaces(s, shown, places);
  GlideSlot layout[F1Ticker::ROWS];
  for (int k = 0; k < count; k++) {
    strcpy(layout[k].code, s.rows[places[k]].code);
    layout[k].y = top + k * 6;
  }
  glideEnterY = top + shown * 6;
  glideTo_(layout, count);
  // What is drawn: the places of the list, and while it glides also the drivers who just dropped out of it (they
  // slide off the bottom). Whoever travels furthest is drawn last, so he passes in front of the others.
  struct Item { int row, y, moved, key; };
  Item items[F1Ticker::ROWS * 2];
  int n = 0;
  bool moving = glideActive();
  for (int k = 0; k < count; k++) {
    int i = places[k], y = glideNow(layout[k].code, layout[k].y), moved = movedDir(s.rows[i].code);
    items[n++] = Item{i, y, moved, abs(layout[k].y - glideStart(layout[k].code, layout[k].y)) * 2 + (moved > 0 ? 1 : 0)};
  }
  if (moving)
    for (int f = 0; f < glideFromN; f++) {
      bool kept = false;
      for (int k = 0; k < count && !kept; k++) kept = strcmp(layout[k].code, glideFrom[f].code) == 0;
      if (kept) continue;
      for (int i = 0; i < F1Ticker::ROWS; i++)
        if (strcmp(s.rows[i].code, glideFrom[f].code) == 0) {
          int y = glideFrom[f].y + (int)roundf((glideEnterY - glideFrom[f].y) * glideEase(millis() - glideAt));
          items[n++] = Item{i, y, movedDir(s.rows[i].code), abs(glideEnterY - glideFrom[f].y) * 2};
          break;
        }
    }
  for (int a = 1; a < n; a++)  // insertion sort, few items
    for (int b = a; b > 0 && items[b].key < items[b - 1].key; b--) std::swap(items[b], items[b - 1]);
  for (int a = 0; a < n; a++) {
    const Item &it = items[a];
    if (moving) {  // opaque, so what it passes is covered
      if (flag) {
        flagFill(d, 0, it.y - 1, W, 7);
      } else {
        d->fillRect(0, it.y - 1, W, 7, 0);
        drawLiveBackdrop(d, max(it.y - 1, 8), min(it.y + 6, 64));
      }
    }
    F1Ticker::Row shown = liveRow(s, it.row);
    if (race && s.finished && it.row == 0) strlcpy(shown.time, "FINISHED", sizeof(shown.time));  // the leader has taken the flag
    drawPlace(d, it.row + 1, shown, it.y, race, it.moved, isFavourite(shown.code));
  }
}


static void whenText(ClockTime *dateTime, time_t utc, char *out, size_t size);

// The championship places to show: the top three and, when a favourite driver is further down, the best placed one
static int standingsShown(int *out, int rows = 3) {
  int n = 0;
  for (int i = 0; i < snap.standingsCount && n < 3; i++) out[n++] = i;
  int favourite = -1;
  for (int i = 3; i < snap.standingsCount && favourite < 0; i++)
    if (isFavourite(snap.standings[i].code)) favourite = i;
  int fill = favourite >= rows ? rows - 1 : rows;  // the places after the top 3 up to the rows, the last one for a favourite further down
  for (int i = 3; i < snap.standingsCount && n < fill; i++) out[n++] = i;
  if (favourite >= rows) out[n++] = favourite;
  return n;
}

struct ChampionshipLayout {
  int margin, numberWidth;
};

static const char *championshipName(const char *code) {
  static const struct { const char *code, *name; } NAMES[] = {
    {"VER", "VERSTAPPEN"}, {"HAD", "HADJAR"}, {"RUS", "RUSSELL"}, {"ANT", "ANTONELLI"},
    {"LEC", "LECLERC"}, {"HAM", "HAMILTON"}, {"NOR", "NORRIS"}, {"PIA", "PIASTRI"},
    {"ALO", "ALONSO"}, {"STR", "STROLL"}, {"GAS", "GASLY"}, {"COL", "COLAPINTO"},
    {"ALB", "ALBON"}, {"SAI", "SAINZ"}, {"LAW", "LAWSON"}, {"LIN", "LINDBLAD"},
    {"HUL", "HULKENBERG"}, {"BOR", "BORTOLETO"}, {"OCO", "OCON"}, {"BEA", "BEARMAN"},
    {"PER", "PEREZ"}, {"BOT", "BOTTAS"}
  };
  for (const auto &driver : NAMES)
    if (!strcmp(driver.code, code)) return driver.name;
  return code;
}

// The name of a rider or driver in the championship: the one the feed gave, or else the one the face knows
static const char *standingName(const F1Ticker::Standing &s) { return s.name[0] ? s.name : championshipName(s.code); }

static ChampionshipLayout championshipLayout(const int *shown, int count) {
  ChampionshipLayout layout{2, 3};
  for (int i = 0; i < count; i++) {
    char number[12];
    snprintf(number, sizeof(number), "%d", shown[i] + 1);
    layout.numberWidth = max(layout.numberWidth, textWidth(number));
  }
  for (; layout.margin > 0; layout.margin--) {
    bool fits = true;
    for (int i = 0; i < count; i++) {
      const auto &driver = snap.standings[shown[i]];
      char points[8];
      snprintf(points, sizeof(points), "%d", driver.points);
      int room = W - 2 * layout.margin - layout.numberWidth - 7 - textWidth(points);
      if (textWidth(standingName(driver)) > room) fits = false;
    }
    if (fits) break;
  }
  return layout;
}

static void drawChampionshipRow(MatrixPanel_I2S_DMA *d, int index, int y, const ChampionshipLayout &layout) {
  const F1Ticker::Standing &driver = snap.standings[index];
  char number[12], points[8];
  snprintf(number, sizeof(number), "%d", index + 1);
  snprintf(points, sizeof(points), "%d", driver.points);
  int bar = layout.margin + layout.numberWidth + 2;
  drawTextRight(d, number, bar - 2, y, placeInk(index == 0 ? GOLD : GREY));
  d->fillRect(bar, y, 2, 5, placeInk(driver.color));
  int right = W - layout.margin - textWidth(points) - 2;
  drawMarquee(d, standingName(driver), bar + 3, y, right, placeInk(isFavourite(driver.code) ? GOLD : WHITE));
  drawTextRight(d, points, W - layout.margin, y, placeInk(index == 0 ? MINUTES : GREY));
}

static void drawLoadingLabel(MatrixPanel_I2S_DMA *d, int y) {
  loadingDotsShown = true;
  int x = (W - textWidth("LOADING...")) / 2;
  drawText(d, "LOADING", x, y, GREY);
  int dots = (millis() / LOADING_DOT_MS) % 3 + 1;
  for (int i = 0; i < dots; i++) d->drawPixel(x + 29 + i * 4, y + 4, GREY);
}

// Both clock views use the same upcoming weekend and local date/time.
static void drawRaceWeekend(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int x, int y, int right) {
  time_t nowUtc = ultime::now();
  for (int i = 0; i < snap.upcomingCount; i++) {
    if (!snap.upcoming[i].city[0] || (ultime::timeStatus() == timeSet && snap.upcoming[i].start <= nowUtc)) continue;
    char when[12];
    whenText(dateTime, snap.upcoming[i].start, when, sizeof(when));
    int room = right - timeWidth(when) - 2;
    const F1Ticker::Race &race = snap.upcoming[i];
    // The locality (the circuit's own place) when the Grand Prix's name does not fit but it does
    const char *name = tallWidth(race.city) > room - x && race.place[0] && tallWidth(race.place) <= room - x ? race.place : race.city;
    drawMarquee(d, name, x, y - 1, room, WHITE, true);
    drawTimeRight(d, when, right, y, MINUTES);  // one pixel either side of the colon
    return;
  }
  drawLoadingLabel(d, y);
}

// 32 rows: the line under the clock. Idle: the top three of the championship, then the next weekend.
// Between sessions: the top three of the one that just finished, then the next session or weekend.
static void drawInfoRow(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, bool idle, int page, int y) {
  dimPlaces = idle ? snap.standingsStale : snap.last.stale;
  loadingTimes = !idle && !snap.last.fromFeed && (!snap.last.rows[0].time[0] || snap.last.timeFails);
  int shown[4];
  int places = idle ? standingsShown(shown) : min(3, filledRows(snap.last));
  bool nextSession = snap.next.valid && snap.next.name[0] &&
                     (snap.comingCount || snap.next.status == 'd' || snap.next.start > ultime::now());
  bool next = idle ? snap.upcomingCount > 0 : true;
  int count = places + (next ? 1 : 0);
  if (count == 0) {
    drawLoadingLabel(d, y);
    return;
  }
  int item = page % count;
  if (item < places) {
    if (idle) {
      int k = shown[item];
      drawChampionshipRow(d, k, y, championshipLayout(shown + item, 1));
    } else {
      drawPlace(d, item + 1, snap.last.rows[item], y, snap.last.tag == 'R' || snap.last.tag == 'S');
    }
  } else if (idle || !nextSession) {
    drawRaceWeekend(d, dateTime, 2, y, W - 2);
  } else {
    LocalTime t = localParts(dateTime, snap.next.start);
    char when[12];
    if (snap.next.status == 'd') strcpy(when, "DELAYED");
    else snprintf(when, sizeof(when), "%s %02d:%02d", DAYS[t.weekday], t.hour, t.minute);
    drawText(d, snap.next.name, 2, y, WHITE);
    drawTimeRight(d, when, W - 2, y, snap.next.status == 'd' ? DELAYED : snap.next.status == 'n' ? NEW_TIME : MINUTES);
  }
}

// The bottom row: the next session, with a pixel of margin above and below its text
// All the sessions still to come this weekend, one after the other, sliding up like the pages of the lists
PageSlide slideNext;

// A delayed session is shown alone: no scrolling through the others
static int delayedIndex() {
  for (int i = 0; i < snap.comingCount; i++) if (snap.coming[i].status == 'd') return i;
  return -1;
}
int comingCount() {
  if (delayedIndex() >= 0) return 1;
  if (snap.comingCount) return snap.comingCount;
  return snap.next.valid && snap.next.name[0] && (snap.next.status == 'd' || snap.next.start > ultime::now()) ? 1 : 0;
}
int comingIndex() {
  int delayed = delayedIndex();
  if (delayed >= 0) return delayed;
  int n = comingCount();
  return n ? (int)(millis() / pageMs() % n) : 0;
}

// A session as the bottom row shows it: P1, SQ, SR, Q, R
static const char *shortSession(const char *name) {
  static char buffer[4];
  if (!strcmp(name, "QUALI")) return "Q";
  if (!strcmp(name, "SPRINT")) return "SR";
  if (!strcmp(name, "RACE")) return "R";
  if (!strncmp(name, "FP", 2)) { strlcpy(buffer, name + 1, sizeof(buffer)); return buffer; }  // FP1 is P1 here
  return name;
}

// The coming session along the bottom: its name at the left, the day and the time at the right (the time only today)
static void drawComing(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int index, int y) {
  const char *name = snap.comingCount ? snap.coming[index].name : snap.next.name;
  time_t start = snap.comingCount ? snap.coming[index].start : snap.next.start;
  char status = snap.comingCount ? snap.coming[index].status : snap.next.status;
  LocalTime t = localParts(dateTime, start);
  char when[12];
  bool today = dateTime->utcToLocal(start) / SECS_PER_DAY == dateTime->localNow() / SECS_PER_DAY;
  if (status == 'd') strcpy(when, "DELAYED");
  else if (today) snprintf(when, sizeof(when), "%02d:%02d", t.hour, t.minute);
  else snprintf(when, sizeof(when), "%s %02d:%02d", DAYS[t.weekday], t.hour, t.minute);
  drawNarrowText(d, name, 1, y, WHITE);
  drawTimeRight(d, when, W - 1, y + 1, status == 'd' ? DELAYED : status == 'n' ? NEW_TIME : MINUTES);  // 5 rows in the middle of the name's 7
}

static void drawNextRow(MatrixPanel_I2S_DMA *d, ClockTime *dateTime) {
  drawBottomChequers(d);
  int n = comingCount();
  if (!n) {
    drawRaceWeekend(d, dateTime, 1, 57, W - 1);
    return;
  }
  int index = comingIndex();
  slideSeen(slideNext, index);
  clipTop = 56;
  if (slideActive(slideNext)) {
    int shift = slideShift(slideNext, 8);
    drawComing(d, dateTime, slideNext.prev % n, 56 - shift);
    drawComing(d, dateTime, index, 56 + 8 - shift);
  } else {
    drawComing(d, dateTime, index, 56);
  }
  clipTop = 0;
}

// The first place shown under the winner on a page; the last page is moved back so it ends on the last place, full
static int firstShown(int n, int page, int slots) {
  if (n <= slots + 1) return 1;
  return min(1 + page * slots, n - slots);
}

// After a session, for the time set in the settings: the whole classification in the live list's look, the winner
// fixed and the other places scrolling a page at a time. The header has the session and the time.
bool qualiList(const F1Ticker::Session &s) { return !shortPanel() && s.tag == 'Q' && filledRows(s) > 9; }

void drawResultList(MatrixPanel_I2S_DMA *d, const F1Ticker::Session &s, int hour, int minute, int page) {
  dimPlaces = s.stale;
  loadingTimes = s.live || (!s.fromFeed && (!s.rows[0].time[0] || s.timeFails));
  if (qualiList(s)) {  // qualifying: the look of its live list, the top three fixed and the other places scrolling
    F1Ticker::Session shown = s;
    shown.flag = 0;
    shown.finished = false;
    char right[8];
    snprintf(right, sizeof(right), "%d:%02d", hour, minute);
    drawLiveScroll(d, shown, right, HEADER_NAVY, WHITE, F1_RED, page, MINUTES);
    return;
  }
  bool race = s.tag == 'R' || s.tag == 'S';
  int n = filledRows(s);
  int first = firstShown(n, page, 8);
  slideSeen(slideList, first);
  flagFill(d, 0, 8, W, 56);  // the results of a finished session: the chequered flag behind them
  // The scrolling places go first, so the header and the winner can be drawn over the part that slides out
  {
    bool sliding = slideActive(slideList);
    int span = first > slideList.prev && first - slideList.prev < 8 ? first - slideList.prev : 8;  // the last page moves only as far as it is short
    int shift = slideShift(slideList, span * 6);
    for (int slot = 1; slot < 9; slot++) {
      int i = first + slot - 1, y = 10 + slot * 6;
      if (sliding) {
        int before = slideList.prev + slot - 1;
        if (before < n) drawPlace(d, before + 1, s.rows[before], y - shift, race, 0, isFavourite(s.rows[before].code));
        if (i < n) drawPlace(d, i + 1, s.rows[i], y + span * 6 - shift, race, 0, isFavourite(s.rows[i].code));
      } else if (i < n) {
        drawPlace(d, i + 1, s.rows[i], y, race, 0, isFavourite(s.rows[i].code));
      }
    }
    if (sliding) {
      d->fillRect(0, 0, W, 8, 0);
      flagFill(d, 0, 8, W, 8);
    }
  }
  const char *name = !strcmp(s.name, "QUALI") ? "Q" : !strcmp(s.name, "SPRINT") ? "SR" : !strcmp(s.name, "RACE") ? "RACE" : !strncmp(s.name, "FP", 2) ? s.name + 1 : s.name;  // FP1 is P1 here
  drawHeader(d, "", "", HEADER_NAVY, WHITE);
  drawWaitDot(d, s);
  drawText(d, name, strcmp(name, "RACE") ? 0 : 2, 1, FASTEST, true);  // all the way to the left (RACE with 2 pixels), in the best-time purple,
  drawHeaderTime(d, hour, minute, W - 2 - dotShift, 1, MINUTES, HEADER_NAVY);  // and the time 2 pixels from the right edge and 1 from the top, in the clock's green
  drawPlace(d, 1, s.rows[0], 10, race, 0, isFavourite(s.rows[0].code));
}

void drawBetween(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int page) {
  dimPlaces = snap.last.stale;
  loadingTimes = !snap.last.fromFeed && (!snap.last.rows[0].time[0] || snap.last.timeFails);
  if (shortPanel()) {
    drawClock(d, hour, minute, 1);
    d->drawFastHLine(2, 16, 60, LINE);
    drawInfoRow(d, dateTime, false, page, 19);
    return;
  }
  // The winner stays; the places after it follow three at a time, so the whole classification gets its turn.
  // Those places go first: the part that slides out over the clock or the next session is wiped before those are drawn
  bool race = snap.last.tag == 'R' || snap.last.tag == 'S';
  int n = filledRows(snap.last);
  int first = firstShown(n, page, 4);
  slideSeen(slideBetween, first);
  chequer(d, 0, 64);
  bool sliding = slideActive(slideBetween);
  int shift = slideShift(slideBetween, 24);
  for (int slot = 0; slot < 4; slot++) {
    int i = first + slot, y = 29 + slot * 6;
    if (sliding) {
      int before = slideBetween.prev + slot;
      if (before < n) drawPlace(d, before + 1, snap.last.rows[before], y - shift, race, 0, isFavourite(snap.last.rows[before].code));
      if (i < n) drawPlace(d, i + 1, snap.last.rows[i], y + 24 - shift, race, 0, isFavourite(snap.last.rows[i].code));
    } else if (i < n) {
      drawPlace(d, i + 1, snap.last.rows[i], y, race, 0, isFavourite(snap.last.rows[i].code));
    }
  }
  if (sliding) {
    d->fillRect(0, 0, W, 29, 0);
    chequer(d, 0, 29);
    d->fillRect(0, 54, W, 10, 0);
    chequer(d, 54, 64);
  }
  d->drawFastHLine(0, 20, W, RULE);  // between the clock and the winner, and under the places
  d->drawFastHLine(0, 54, W, RULE);
  clockLeft = W - 1 - clockWidth();  // at the right, a pixel from the edge, the session name on the left
  drawClock(d, hour, minute, 1, true);
  // The session the results are of, left of the clock: a light grey, to read well on the squares
  // One letter (Q, R) sits right above the place number below it, flush with its right edge (columns 4 to 6); two (SR, P1) at the left
  if (snap.last.name[0]) {
    const char *label = shortSession(snap.last.name);
    drawText(d, label, strlen(label) == 1 ? 7 - tallWidth(label) : 1, 12, rgb(205, 210, 220), true);
  }
  drawPlace(d, 1, snap.last.rows[0], 23, race, 0, isFavourite(snap.last.rows[0].code));
  drawNextRow(d, dateTime);
}

// The weekend's local start: TODAY or a weekday within six days, followed by the time, or only the date after that.
static void whenText(ClockTime *dateTime, time_t utc, char *out, size_t size) {
  time_t local = dateTime->utcToLocal(utc);
  long days = local / SECS_PER_DAY - dateTime->localNow() / SECS_PER_DAY;
  LocalTime t = localParts(dateTime, utc);
  if (days <= 0) snprintf(out, size, "TODAY %02d:%02d", t.hour, t.minute);
  else if (days <= 6) snprintf(out, size, "%s %02d:%02d", DAYS[t.weekday], t.hour, t.minute);
  else snprintf(out, size, "%02d/%02d", ultime::day(local), ultime::month(local));
}

// No header: the clock on top, then the championship, and the coming race along the bottom
void drawIdle(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int page) {
  dimPlaces = snap.standingsStale;
  loadingTimes = false;
  if (shortPanel()) {
    drawClock(d, hour, minute, 1);
    d->drawFastHLine(2, 16, 60, LINE);
    drawInfoRow(d, dateTime, true, page, 19);
    return;
  }
  drawBackdrop(d, 0, 64);
  bigClock = true;
  clockLeft = (W - clockWidth()) / 2;
  drawClock(d, hour, minute, 2, true);
  bigClock = false;
  int shown[4];
  int places = standingsShown(shown, 4);
  ChampionshipLayout layout = championshipLayout(shown, places);
  d->drawFastHLine(0, 26, W, RULE);
  for (int i = 0; i < places; i++)
    drawChampionshipRow(d, shown[i], 29 + i * 6, layout);
  d->drawFastHLine(0, 54, W, RULE);  // a second line, between the list and the bottom bar
  drawBottomChequers(d);
  drawRaceWeekend(d, dateTime, 1, 57, W - 1);
}

// Made-up screens for the telnet simulator, so every look can be checked outside a race weekend

}  // namespace f1view
