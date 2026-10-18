// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include "FootballRenderer.h"
#include "TeamColors.h"
#include "FlipDigits.h"
#include "NumericFont.h"
#include <Console.h>
#include <math.h>
#include <functional>
#include <new>

namespace fbview {

Overview overview;  // the live matches of the snapshot, or the result on top
static football::Snapshot *displaySnapshot = nullptr;
bool prepareSnapshot() {
  if (!displaySnapshot) displaySnapshot = new (std::nothrow) football::Snapshot{};
  return displaySnapshot != nullptr;
}
void releaseSnapshot() { delete displaySnapshot; displaySnapshot = nullptr; }
football::Snapshot &snap() {
  if (!displaySnapshot) displaySnapshot = new football::Snapshot{};
  return *displaySnapshot;
}

uint32_t overviewRevision = 0;
RankingPage rankingPage, previousRankingPage;
unsigned long rankingStartedAt = 0;
bool previousRankingValid = false;

// Rows of results slide up one at a time: a pause, then a quick slide
static const int STRIP_H = 9;



static const uint16_t INK = rgb(255, 255, 255);
static const uint16_t TEAM_NAME = rgb(232, 232, 232);
static const uint16_t CLOCK = rgb(238, 246, 240);
static const uint16_t MINUTES = rgb(190, 235, 150);
static const uint16_t DIMMED = rgb(110, 116, 126);
static const uint16_t DASH = rgb(200, 200, 208);
static const uint16_t HALF_TIME = rgb(175, 180, 190);  // HT and the front of the timeline at half time
static const uint16_t YELLOW_CARD = rgb(255, 214, 0);
static const uint16_t RED_CARD = rgb(225, 30, 30);
static const uint16_t STRIP = rgb(22, 118, 48);
static const uint16_t STRIP_LINE = rgb(90, 170, 110);
static const uint16_t TRACK = rgb(30, 32, 38);
static const uint16_t PLAYED = rgb(50, 200, 90);
static const uint16_t DIVIDER = rgb(40, 44, 52);
static const uint16_t MAIN_DIVIDER = rgb(24, 20, 24);  // both dividers in the upcoming games and rankings views; green kept low, the panel shows it brighter than red and blue
static const uint16_t PANEL = rgb(20, 22, 28);  // behind the upcoming games
static const uint16_t SECONDS_TRACK = rgb(28, 30, 38);
static const uint16_t POSITION = rgb(170, 172, 180);  // places in the table, the same for every competition

// Rows outside clipTop..clipBottom-1 aren't drawn, for lists that slide
static int clipTop = 0, clipBottom = 64;

uint16_t scaleColor(uint16_t c, int level);
static uint16_t mixColor(uint16_t c0, uint16_t c1, int k);
static uint16_t backgroundAt(int x, int y, int stripe, bool withBoards);

// What is drawn comes out at blockDim/32 of its brightness (32 = as it is), for the dips when a live match or
// a coming match changes
static int blockDim = 32;

static void fill(MatrixPanel_I2S_DMA *d, int x, int y, int w, int h, uint16_t color) {
  if (blockDim < 32) color = scaleColor(color, blockDim);
  if (y < clipTop) {
    h -= clipTop - y;
    y = clipTop;
  }
  if (y + h > clipBottom) h = clipBottom - y;
  if (w > 0 && h > 0) d->fillRect(x, y, w, h, color);
}

// 3x5 font, one row per byte, bit 2 = left pixel
static const uint8_t *glyph3x5(char c) {
  static const struct { char c; uint8_t rows[5]; } FONT[] = {
    {'0', {7,5,5,5,7}}, {'1', {2,6,2,2,7}}, {'2', {7,1,7,4,7}}, {'3', {7,1,7,1,7}}, {'4', {5,5,7,1,1}},
    {'5', {7,4,7,1,7}}, {'6', {7,4,7,5,7}}, {'7', {7,1,1,1,1}}, {'8', {7,5,7,5,7}}, {'9', {7,5,7,1,7}},
    {'A', {2,5,7,5,5}}, {'B', {6,5,6,5,6}}, {'C', {3,4,4,4,3}}, {'D', {6,5,5,5,6}}, {'E', {7,4,6,4,7}},
    {'F', {7,4,6,4,4}}, {'G', {3,4,5,5,3}}, {'H', {5,5,7,5,5}}, {'I', {7,2,2,2,7}}, {'J', {1,1,1,5,2}},
    {'K', {5,5,6,5,5}}, {'L', {4,4,4,4,7}}, {'M', {5,7,7,5,5}}, {'N', {6,5,5,5,5}}, {'O', {7,5,5,5,7}},
    {'P', {6,5,6,4,4}}, {'Q', {2,5,5,7,3}}, {'R', {6,5,6,5,5}}, {'S', {3,4,2,1,6}}, {'T', {7,2,2,2,2}},
    {'U', {5,5,5,5,7}}, {'V', {5,5,5,5,2}}, {'W', {5,5,5,7,5}}, {'X', {5,5,2,5,5}}, {'Y', {5,5,2,2,2}},
    {'Z', {7,1,2,4,7}}, {'-', {0,0,7,0,0}}, {'\'', {2,2,0,0,0}}, {'+', {0,2,7,2,0}}, {':', {0,2,0,2,0}},
    {'/', {1,1,2,4,4}}, {'.', {0,0,0,0,2}},
  };
  for (const auto &g : FONT)
    if (g.c == c) return g.rows;
  return nullptr;  // space and anything unknown
}

// 5x7 font for the time, bit 4 = left pixel
static const uint8_t DIGITS[10][7] = {
  {14,17,17,17,17,17,14}, {4,12,4,4,4,4,14},  {14,17,1,2,4,8,31},    {30,1,1,14,1,1,30},  {2,6,10,18,31,2,2},
  {31,16,30,1,1,17,14},   {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},    {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
};

static const char *const DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

int textWidth(const String &s, int scale) { return s.length() ? (s.length() * 4 - 1) * scale : 0; }
int textWidth(const char *s, int scale) { return *s ? (strlen(s) * 4 - 1) * scale : 0; }

static void drawText(MatrixPanel_I2S_DMA *d, const String &s, int x, int y, uint16_t color, int scale = 1) {
  for (unsigned i = 0; i < s.length(); i++, x += 4 * scale) {
    const uint8_t *rows = glyph3x5(s[i]);
    if (!rows) continue;
    for (int j = 0; j < 5; j++)
      for (int k = 0; k < 3; k++)
        if (rows[j] >> (2 - k) & 1) fill(d, x + k * scale, y + j * scale, scale, scale, color);
  }
}

static void drawCentered(MatrixPanel_I2S_DMA *d, const String &s, int y, uint16_t color, int scale = 1) {
  drawText(d, s, (W - textWidth(s, scale)) / 2, y, color, scale);
}

// A digit of the 5x7 font, each pixel scale x scale
static void drawDigit(MatrixPanel_I2S_DMA *d, int digit, int x, int y, uint16_t color, int scale) {
  for (int j = 0; j < 7; j++)
    for (int k = 0; k < 5; k++)
      if (numericfont::enabled() ? numericfont::smallPixel(digit, k, j) : (DIGITS[digit][j] >> (4 - k) & 1)) fill(d, x + k * scale, y + j * scale, scale, scale, color);
}

// Where drawNumber() starts a number centred on cx
static int numberX(const String &digits, int cx, int scale) { return cx - (digits.length() * 6 * scale - scale) / 2; }

// A number in the 5x7 font, centred on cx
static void drawNumber(MatrixPanel_I2S_DMA *d, const String &digits, int cx, int y, uint16_t color, int scale) {
  int x = numberX(digits, cx, scale);
  for (unsigned i = 0; i < digits.length(); i++, x += 6 * scale)
    if (isdigit(digits[i])) drawDigit(d, digits[i] - '0', x, y, color, scale);
}

// The outermost lit column of a 1x number drawn centred on cx: its right edge, or its left edge
static int inkEdge(const String &digits, int cx, bool right) {
  int x0 = numberX(digits, cx, 1), edge = right ? -1 : W;
  for (unsigned i = 0; i < digits.length(); i++) {
    if (!isdigit(digits[i])) continue;
    for (int k = 0; k < 5; k++)
      for (int j = 0; j < 7; j++)
        if (numericfont::enabled() ? numericfont::smallPixel(digits[i] - '0', k, j) : (DIGITS[digits[i] - '0'][j] >> (4 - k) & 1)) edge = right ? max(edge, x0 + (int)i * 6 + k) : min(edge, x0 + (int)i * 6 + k);
  }
  return edge;
}

static String twoDigits(int n) { return String(n / 10) + String(n % 10); }

// Squared distance between two RGB565 colours, in 8-bit RGB
static int colorDistance(uint16_t a, uint16_t b) {
  int dr = ((a >> 11) & 31) * 8 - ((b >> 11) & 31) * 8;
  int dg = ((a >> 5) & 63) * 4 - ((b >> 5) & 63) * 4;
  int db = (a & 31) * 8 - (b & 31) * 8;
  return dr * dr + dg * dg + db * db;
}

// The shirt colours of the two teams: each team's primary colour. When they are too alike to tell
// apart (say two red teams), the away team changes: its second colour, its shorts, or white, the first
// that is clear of the home colour (else the one furthest from it). Dark shorts don't show on the LEDs.
static const int TOO_ALIKE = 100 * 100;  // Moldova (#0046AE) and Slovakia (#0C2FFF) are 85 apart and still look the same
static const uint16_t AWAY_WHITE = 0xFFFF;
static const uint16_t AWAY_DARK = 0x3186;  // dark grey-blue, for a home team in white
static void kitColors(const TeamKit &home, const TeamKit &away, uint16_t &homeColor, uint16_t &awayColor) {
  homeColor = home.shirt;
  awayColor = away.shirt;
  if (colorDistance(homeColor, awayColor) >= TOO_ALIKE) return;
  const uint16_t candidates[] = {away.second, away.shorts, AWAY_WHITE, AWAY_DARK};
  int best = colorDistance(homeColor, awayColor);
  for (uint16_t c : candidates) {
    if (!c) continue;
    int distance = colorDistance(homeColor, c);
    if (distance > best) {
      best = distance;
      awayColor = c;
    }
    if (distance >= TOO_ALIKE) break;
  }
}

// Collar and cuffs: the club's second colour, else white on a dark shirt and dark on a light one.
// A shirt that isn't the club's own (the away team changed) gets its own colour as the trim.
static uint16_t trimColor(const TeamKit &kit, uint16_t shirt) {
  if (shirt != kit.shirt) return kit.shirt;
  if (kit.second && kit.second != shirt) return kit.second;
  int brightness = ((shirt >> 11) & 31) * 8 * 3 + ((shirt >> 5) & 63) * 4 * 6 + (shirt & 31) * 8;
  return brightness > 1400 ? rgb(60, 60, 70) : rgb(255, 255, 255);
}

// A 14x12 shirt
static void drawShirt(MatrixPanel_I2S_DMA *d, int x, int y, uint16_t shirt, uint16_t trim) {
  fill(d, x + 1, y + 1, 12, 1, shirt);   // shoulders
  fill(d, x, y + 2, 14, 3, shirt);       // sleeves and chest
  fill(d, x + 3, y + 5, 8, 7, shirt);    // body
  fill(d, x + 5, y + 1, 4, 1, trim);     // collar
  fill(d, x + 6, y + 2, 2, 1, trim);
  fill(d, x, y + 2, 1, 3, trim);         // cuffs
  fill(d, x + 13, y + 2, 1, 3, trim);
}

// A 4x4 white ball: a goal on the timeline
static void drawSmallBall(MatrixPanel_I2S_DMA *d, int x, int y) {
  fill(d, x + 1, y, 2, 1, INK);
  fill(d, x, y + 1, 4, 2, INK);
  fill(d, x + 1, y + 3, 2, 1, INK);
}

// A colour at level/32 of its brightness, for things that pulse
static uint16_t dimmed(uint8_t r, uint8_t g, uint8_t b, int level) { return rgb(r * level / 32, g * level / 32, b * level / 32); }


// A 7x7 ball at x, y, at level/32 of its brightness. The four dark patches turn by angle (radians,
// ultimatrix); the shading to the lower right stays where it is, like a light on a ball rolling by.
// ALPHA: how much of each pixel the round ball covers (0..32), so the corners are soft: they fade into the band behind
// the ball (backgroundAt; stripe = the width of the grass stripes there).
static void drawBall(MatrixPanel_I2S_DMA *d, int x, int y, float angle, int level = 32, int clipRight = CLOCK_RIGHT,
                     int stripe = 6, bool withBoards = true, int backgroundLevel = 32) {
  static const uint8_t ALPHA[7][7] = {{0, 8, 32, 32, 32, 8, 0}, {8, 32, 32, 32, 32, 32, 8}, {32, 32, 32, 32, 32, 32, 32},
                                      {32, 32, 32, 32, 32, 32, 32}, {32, 32, 32, 32, 32, 32, 32}, {8, 32, 32, 32, 32, 32, 8},
                                      {0, 8, 32, 32, 32, 8, 0}};
  static const char *const SHADE[7] = {".......", ".......", ".......", "......g", "....ggG", "....GG.", "..GGG.."};
  static const int PATCHES[4][2] = {{0, -2}, {-2, 0}, {2, 0}, {0, 2}};  // around the middle
  float c = cosf(angle), s = sinf(angle);
  bool patch[7][7] = {};
  for (const auto &p : PATCHES) {
    int px = (int)roundf(3 + p[0] * c - p[1] * s), py = (int)roundf(3 + p[0] * s + p[1] * c);
    patch[py][px] = true;
  }
  for (int j = 0; j < 7; j++)
    for (int i = 0; i < 7; i++) {
      if (!ALPHA[j][i] || x + i < 0 || x + i >= clipRight || y + j < 0 || y + j >= 64) continue;  // outside the ball, or rolling in or out
      char shade = SHADE[j][i];
      uint16_t color = patch[j][i] ? 0 : shade == 'g' ? dimmed(190, 190, 196, level) : shade == 'G' ? dimmed(125, 125, 134, level) : dimmed(246, 246, 246, level);
      d->drawPixel(x + i, y + j, ALPHA[j][i] == 32 ? color
                                                    : mixColor(scaleColor(backgroundAt(x + i, y + j, stripe, withBoards), backgroundLevel), color, ALPHA[j][i]));
    }
}

// "2-1" into its two scores; false when it isn't that shape
static bool splitScore(const String &score, String &home, String &away) {
  int dash = score.indexOf('-');
  if (dash <= 0 || dash >= (int)score.length() - 1) return false;
  home = score.substring(0, dash);
  away = score.substring(dash + 1);
  return true;
}

static bool halfTime(const MatchRow &m) { return m.live && strncmp(m.detail, "HT", 2) == 0; }

// The game time for the green bar: "67'", "45+2" for stoppage time, "45'" at half time
static String gameTime(const MatchRow &m) {
  if (halfTime(m)) return "45'";
  if (!m.live) return "90'";  // a finished match shown on top (see showResultsLive)
  String time = m.detail;
  if (time.indexOf('+') > 0) time.replace("'", "");  // keep only the extra minutes supplied by ESPN
  return time;
}

// With no live match, results take turns in the main view. Favourite results
// take turns on top instead, with the other results and today's kick-offs below.
bool resultsOnTop = false;
size_t shownResult = SIZE_MAX;
size_t finishedCount() { return snap().resultCount; }
size_t upcomingCount() { return snap().fixtureCount; }

// The snapshot's rows as the MatchRows the drawing code takes
static void matchFrom(const football::Side &home, const football::Side &away, int homeScore, int awayScore,
                      const char *competition, uint16_t color, bool favourite, MatchRow &m) {
  strlcpy(m.home, home.code, sizeof(m.home));
  strlcpy(m.away, away.code, sizeof(m.away));
  m.homeScore = homeScore;
  m.awayScore = awayScore;
  m.homeKit = home.kit;
  m.awayKit = away.kit;
  m.scoreColor = color;
  strlcpy(m.competition, competition, sizeof(m.competition));
  m.favourite = favourite;
}

template <typename Row> static void addMarks(const Row &row, MatchRow &m) {
  m.markCount = row.markCount;
  memcpy(m.marks, row.marks, sizeof(m.marks));
}

MatchRow liveMatch(const football::LiveRow &row) {
  MatchRow m = {};
  matchFrom(row.home, row.away, row.homeScore, row.awayScore, row.competition, row.color, row.favourite, m);
  m.progress = row.progress;
  m.live = true;
  strlcpy(m.detail, row.clock, sizeof(m.detail));
  addMarks(row, m);
  return m;
}

bool readResult(size_t index, MatchRow &out) {
  if (index >= snap().resultCount) return false;
  const football::ResultRow &row = snap().results[index];
  out = MatchRow();
  matchFrom(row.home, row.away, row.homeScore, row.awayScore, row.competition, row.color, row.favourite, out);
  out.progress = 1;
  addMarks(row, out);
  return true;
}

uint16_t favouriteResults() {
  uint16_t mask = 0;
  for (size_t i = 0; i < snap().resultCount; i++)
    if (snap().results[i].favourite) mask |= 1U << i;
  return mask;
}

static bool comingOnDay(ClockTime *dateTime, size_t i, uint32_t today) {
  return dateTime->utcToLocal(snap().fixtures[i].kickoff) / 86400 == today;
}

static size_t resultBelowIndex(size_t row) {
  uint16_t favourites = resultsOnTop ? favouriteResults() : 0;
  for (size_t i = 0; i < finishedCount(); i++) {
    if (resultsOnTop && (favourites ? (favourites & (1U << i)) != 0 : i == shownResult)) continue;
    if (row-- == 0) return i;
  }
  return SIZE_MAX;
}

size_t resultsBelow() {
  size_t count = 0;
  while (resultBelowIndex(count) != SIZE_MAX) count++;
  return count;
}

// Today's coming matches, for the rows below the results: the n-th one, null when there is none
static const football::FixtureRow *comingToday(ClockTime *dateTime, size_t n) {
  uint32_t today = dateTime->localNow() / 86400;
  for (size_t i = 0; i < upcomingCount(); i++) {
    if (comingOnDay(dateTime, i, today) && n-- == 0) return &snap().fixtures[i];
  }
  return nullptr;
}

// How many rows there are below the top match: the other results, then today's coming matches (also next to a live match)
size_t belowRows(ClockTime *dateTime) {
  size_t n = resultsBelow();
  uint32_t today = dateTime->localNow() / 86400;
  for (size_t i = 0; i < upcomingCount(); i++)
    if (comingOnDay(dateTime, i, today)) n++;
  return n;
}

// Results slide down one row at a time: which row is leaving, and how far it has moved
Slide slideAt(unsigned long now, int rowHeight, unsigned long pauseMs) {
  unsigned long cycle = pauseMs + SLIDE_MS, phase = now % cycle;
  return {now / cycle, phase < pauseMs ? 0 : (int)min((unsigned long)rowHeight, (phase - pauseMs) * rowHeight / SLIDE_MS)};
}

// ---- Live ----

static int textWidth5(const String &s);
static void drawText5(MatrixPanel_I2S_DMA *d, const String &s, int x, int y, uint16_t color);

// The green bar: 7px text with a pixel above and below. With a single match
// the clock sits at the bottom instead.
static void drawStrip(MatrixPanel_I2S_DMA *d, const MatchRow &m, int hour, int minute, bool withClock) {
  d->fillRect(0, 0, W, STRIP_H, STRIP);
  String status = String(m.competition) + " " + gameTime(m);
  if (!withClock) {
    drawText5(d, status, (W - textWidth5(status)) / 2, 1, INK);
    return;
  }
  if (textWidth5(status) > 30) status = gameTime(m);
  drawText5(d, status, 1, 1, INK);
  d->fillRect(32, 0, 1, STRIP_H, STRIP_LINE);
  if (numericfont::enabled()) {
    drawText5(d, twoDigits(hour), 38, 1, INK);
    fill(d, 50, 3, 1, 1, INK);
    fill(d, 50, 5, 1, 1, INK);
    drawText5(d, String(minute / 10), minute / 10 == 1 ? 52 : 53, 1, INK);
    drawText5(d, String(minute % 10), 59, 1, INK);
  } else {
    drawText5(d, twoDigits(hour) + ":" + twoDigits(minute), 34, 1, INK);
  }
}

// The score between the shirts: double size, single size when a team has reached double figures
static void drawScore(MatrixPanel_I2S_DMA *d, const String &score, int dy) {
  String home, away;
  if (!splitScore(score, home, away)) {
    drawCentered(d, score, 11 + dy, INK, 2);
  } else if (home.length() == 1 && away.length() == 1) {
    drawNumber(d, home, 23, 9 + dy, INK, 2);
    drawNumber(d, away, 41, 9 + dy, INK, 2);
    fill(d, W / 2 - 2, 15 + dy, 4, 2, DASH);
  } else {
    drawNumber(d, home, 23, 12 + dy, INK, 1);
    drawNumber(d, away, 41, 12 + dy, INK, 1);
    int left = inkEdge(home, 23, true) + 1, right = inkEdge(away, 41, false);  // the free columns between them
    fill(d, left + (right - left - 4) / 2, 15 + dy, 4, 1, DASH);
  }
}

// The playing time as a bar with a 2x2 leading edge (grey at half time). Goals as balls and cards
// as stripes at their minute: the home team's above the bar, the away team's below.
static void drawTimeline(MatrixPanel_I2S_DMA *d, const MatchRow &m, int y) {
  int played = (int)roundf((W - 4) * constrain(m.progress, 0.0f, 1.0f));
  fill(d, 2, y, W - 4, 2, TRACK);
  fill(d, 2, y, played, 2, PLAYED);
  for (size_t i = 0; i < m.markCount; i++) {
    const football::Mark &e = m.marks[i];
    int x = constrain(2 + (W - 4) * min((int)e.minute, 90) / 90, 2, W - 3);
    if (e.kind == 'g') drawSmallBall(d, x - 1, e.home ? y - 5 : y + 3);
    else fill(d, x, e.home ? y - 4 : y + 3, 1, 3, e.kind == 'r' ? RED_CARD : YELLOW_CARD);
  }
  fill(d, min(W - 4, 2 + played), y, 2, 2, halfTime(m) ? HALF_TIME : INK);
}

static void drawScoreTiles(MatrixPanel_I2S_DMA *d, int y);
static void drawScoreDigits(MatrixPanel_I2S_DMA *d, int home, int away, int y);
static bool scoreOnTiles();

// The score between the shirts: on flip tiles, or the plain digits when a team has reached double figures
static void drawScoreArea(MatrixPanel_I2S_DMA *d, const MatchRow &m, int shirtY, bool settled = false) {
  if (settled && m.singleFigures())
    drawScoreDigits(d, m.homeScore, m.awayScore, shirtY);
  else if (!settled && scoreOnTiles()) drawScoreTiles(d, shirtY);
  else drawScore(d, m.score(), shirtY - 9);
}

static void drawMatch(MatrixPanel_I2S_DMA *d, const MatchRow &m, bool roomy, int dy = 0, bool settled = false) {
  // With the list below it is one match high, so the top part spreads out; the single match view
  // keeps the clock under the timeline
  int shirtY = (roomy ? 12 : 11) + dy, nameY = (roomy ? 28 : 26) + dy, timelineY = (roomy ? 44 : 39) + dy;
  uint16_t homeColor, awayColor;
  kitColors(m.homeKit, m.awayKit, homeColor, awayColor);
  drawShirt(d, 1, shirtY, homeColor, trimColor(m.homeKit, homeColor));
  drawShirt(d, 49, shirtY, awayColor, trimColor(m.awayKit, awayColor));
  // The names in the 5x7 font, 17 wide, each under its shirt
  String home = m.home, away = m.away;
  drawText5(d, home, 8 - textWidth5(home) / 2, nameY, TEAM_NAME);
  drawText5(d, away, 55 - textWidth5(away) / 2, nameY, TEAM_NAME);
  if (halfTime(m)) drawCentered(d, "HT", nameY + 2, HALF_TIME);
  else if (!m.live) drawCentered(d, "FT", nameY + 2, HALF_TIME);
  drawScoreArea(d, m, shirtY, settled);
  drawTimeline(d, m, timelineY);
}

// The next match enters from above; the previous match moves down into the lower row.

static void drawMatchSwap(MatrixPanel_I2S_DMA *d, const MatchRow &old, const MatchRow &cur, unsigned long t) {
  int offset = (int)(min(t, SWAP_MS) * (54 - STRIP_H) / SWAP_MS);
  clipTop = STRIP_H;
  clipBottom = 54;
  drawMatch(d, old, true, offset, true);
  drawMatch(d, cur, true, offset - (54 - STRIP_H), true);
  clipTop = 0;
  clipBottom = 64;
}

// The winner's number green and the loser's grey, a draw white
static uint16_t resultColor(int own, int other) { return own == other ? INK : own > other ? MINUTES : DIMMED; }

// HH:MM on flip tiles at the bottom, for a single match (drawn with the flip clock below)
static void drawLiveClock(MatrixPanel_I2S_DMA *d, int pulse);

static void drawResult(MatrixPanel_I2S_DMA *d, const MatchRow &m, int y);
static void drawComing(MatrixPanel_I2S_DMA *d, const football::FixtureRow &u, time_t local, int y);

// A row below the top match: a result, or when those are through a coming match
static void drawRow(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, size_t row, int y) {
  size_t count = resultsBelow();
  if (row < count) {
    MatchRow result;
    if (readResult(resultBelowIndex(row), result)) drawResult(d, result, y);
  } else if (const auto *u = comingToday(dateTime, row - count)) {
    drawComing(d, *u, dateTime->utcToLocal(u->kickoff), y);
  }
}

// The live view. Below the timeline: the finished matches (sliding when more than two), else the
// next live matches in the order they come up, else (a single match) the time.
void drawLive(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, size_t index, size_t pinned, size_t shift, int hour, int minute, int second,
                     int pulse, const Slide &slide, int push, const MatchRow *swapOld, unsigned long swapT) {
  const auto &live = overview.live;
  const auto &m = live[index];
  size_t rows = belowRows(dateTime);
  bool single = live.size() == 1 && rows == 0;
  bool swapping = swapOld && !single;
  drawStrip(d, m, hour, minute, !single);
  if (swapping) drawMatchSwap(d, *swapOld, m, swapT);
  else drawMatch(d, m, !single);
  if (single) {
    drawLiveClock(d, pulse);
    return;
  }
  d->fillRect(0, 54, W, 1, DIVIDER);
  clipTop = 55;
  clipBottom = 63;  // one row shows: the one waiting above or below must not peek in
  if (!pinned && live.size() > 1 && !(resultsOnTop && m.favourite)) {
    // The previous main match enters the lower row from above.
    int offset = swapping ? (int)(min(swapT, SWAP_MS) * 9 / SWAP_MS) : 9;
    for (size_t k = 0; k < 2; k++) drawResult(d, live[(index + live.size() + k - 2) % live.size()], 56 - k * 9 + offset);
  } else if (pinned) {
    // Favourites are on top and stay there; the other live matches take turns in the row below, which
    // moves down when it is the next one's turn (the next one enters from above)
    size_t others = live.size() - pinned;
    if (others == 1) {
      drawResult(d, live[pinned], 56);
    } else {
      for (size_t k = 0; k < 2; k++) drawResult(d, live[pinned + (shift + k + others - 1) % others], 56 - k * 9 + push);
    }
  } else if (rows > 0) {
    if (rows == 1) {
      drawRow(d, dateTime, 0, 56);
    } else {
      for (size_t k = 0; k < 2; k++) drawRow(d, dateTime, (slide.step + k) % rows, 56 - k * 9 + slide.offset);
    }
  }
  clipTop = 0;
  clipBottom = 64;
}

// ---- Rows below the live match ----

// The classic 5x7 font (the one the Tetris ticker uses) for those rows. The glyphs come from a
// small canvas so they are clipped like all other text.
static int textWidth5(const String &s) { return s.length() ? s.length() * 6 - 1 : 0; }
static void drawText5(MatrixPanel_I2S_DMA *d, const String &s, int x, int y, uint16_t color) {
  static GFXcanvas1 glyph(6, 8);
  for (unsigned i = 0; i < s.length(); i++, x += 6) {
    glyph.fillScreen(0);
    if (numericfont::enabled() && isdigit(s[i])) {
      for (int gy = 0; gy < 7; gy++)
        for (int gx = 0; gx < 4; gx++)
          if (numericfont::smallPixel(s[i] - '0', gx, gy)) fill(d, x + gx, y + gy, 1, 1, color);
      continue;
    }
    glyph.drawChar(0, 0, s[i], 1, 0, 1);
    for (int gy = 0; gy < 7; gy++)
      for (int gx = 0; gx < 5; gx++)
        if (glyph.getPixel(gx, gy)) fill(d, x + gx, y + gy, 1, 1, color);
  }
}

// A result: kit bars and names around the score, the winner's number green and the loser's grey
// (a draw white). Double figures get the small font so the score fits between the names.
// A live match has no winner yet: its numbers and dash are orange, so it stands out from the results
static const uint16_t LIVE_ROW = rgb(255, 140, 40);
static uint16_t scoreColor(const MatchRow &m, int own, int other) {
  return m.live ? LIVE_ROW : resultColor(own, other);
}

static void drawResult(MatrixPanel_I2S_DMA *d, const MatchRow &m, int y) {
  uint16_t homeColor, awayColor;
  kitColors(m.homeKit, m.awayKit, homeColor, awayColor);
  String score = m.score(), home, away;
  bool split = splitScore(score, home, away);
  bool big = split && home.length() + away.length() <= 2;  // "2-1" in the 5x7 font; "12-1" doesn't fit
  int scoreWidth = big ? 17 : textWidth(score);
  fill(d, 0, y, 2, 7, homeColor);
  drawText5(d, String(m.home), 3, y, TEAM_NAME);
  int sx = (W - scoreWidth) / 2;
  if (big) {
    drawText5(d, home, sx, y, scoreColor(m, home.toInt(), away.toInt()));
    int dashWidth = numericfont::enabled() && away != "1" ? 4 : 5;
    fill(d, sx + 6, y + 3, dashWidth, 1, m.live ? LIVE_ROW : DASH);
    drawText5(d, away, sx + 12, y, scoreColor(m, away.toInt(), home.toInt()));
  } else if (split) {
    drawText(d, home, sx, y + 1, scoreColor(m, home.toInt(), away.toInt()));
    drawText(d, "-", sx + textWidth(home) + 1, y + 1, m.live ? LIVE_ROW : DASH);
    drawText(d, away, sx + textWidth(home) + 5, y + 1, scoreColor(m, away.toInt(), home.toInt()));
  } else {
    drawText(d, score, sx, y + 1, m.live ? LIVE_ROW : INK);
  }
  drawText5(d, String(m.away), W - 20, y, TEAM_NAME);
  fill(d, W - 2, y, 2, 7, awayColor);
}

// How bright the pulsing colons are now, 0..32: one full cycle every 3 seconds, a bit
// calmer than the Tetris face
static const unsigned long PULSE_MS = 3000;
int colonPulse(unsigned long now) {
  float level = (1 - cosf(2 * PI * (now % PULSE_MS) / (float)PULSE_MS)) / 2;
  return (int)roundf(level * level * 32);  // gamma, so the dark end fades evenly
}

// The ball is passed to and fro along the grass by two players standing out of sight: it is kicked in from one edge,
// travels across and leaves at the other; a moment later it is kicked back the other way, and so on. roll counts the
// pixels travelled, so going right x = roll - 7 (7 = ball width) and going left x = edge - roll; ballLeft says which
// way it goes and ballLift how many rows above the ground it is.
// Each pass is of a kind picked by a hash of its number and lasts as long as it needs, so every screen keeps its own
// clock (ballPass). Both clocks have all of them (see ultimatrix-mockups/football-extra-passes.py); the upcoming-games
// view now uses the full panel width too:
//   roll   kicked in at BIG_ROLL_SPEED, slowing down a little
//   lob    kicked into the air, the same speed all the way, a curve LOB_HEIGHT high
//   header rises steeply, kinks at the top as if headed, drops and rolls on
//   chip   one big arc over the minutes tiles
//   shot   low and fast, 2 rows of lift in the middle
//   high   leaves the frame at the top, drops back in further along, bounces twice and rolls out
static const int ROLL_STEPS = CLOCK_RIGHT + 7;
static const float ROLL_SPEED = 40;         // px a second when it is kicked ...
static const float ROLL_SLOWING = 15;       // ... slowing down by this much a second
static const float ROLL_PASS_SECS = 1.551f; // the time that takes to cross the 44 pixels (17 px a second at the other side)
static const float ROLL_BETWEEN_SECS = 0.6f; // before the other player kicks it back
static const int BIG_ROLL_STEPS = W + 7;
static const float BIG_ROLL_SPEED = 55, BIG_ROLL_SLOWING = 18, BIG_ROLL_PASS_SECS = 1.852f;  // crosses the 71 pixels
static const float LOB_SECS = 1.4f;
static const int LOB_HEIGHT = 10;
static const float HEADER_RISE_SECS = 0.9f, HEADER_DROP_SECS = 0.6f, HEADER_KINK = 31, HEADER_LAND = 51, HEADER_HEIGHT = 18;
static const float HEADER_ROLL_SPEED = 40;  // px a second after it has landed
static const float CHIP_SECS = 1.5f;
static const int CHIP_HEIGHT = 22;
static const float SHOT_SPEED = 140;  // px a second, the same all the way
static const int SHOT_HEIGHT = 2;
// the high kick: [seconds, pixels travelled at its end, rows high]: up and out of the frame, two bounces, then it rolls out
static const float HIGH_AIR_SECS = 2.0f, HIGH_LAND = 45, HIGH_APEX = 100;
static const float HIGH_BOUNCE[2][3] = {{0.6f, 14, 14}, {0.35f, 6, 5}};
static const float HIGH_ROLL_SPEED = 22;  // px a second after the last bounce
bool ballLeft = false;
int ballLift = 0;
enum PassKind { PASS_ROLL, PASS_LOB, PASS_HEADER, PASS_CHIP, PASS_SHOT, PASS_HIGH };

static PassKind passKind(uint32_t pass) {
  uint32_t h = (pass * 2654435761u) ^ (pass >> 3);
  h = (h ^ (h >> 15)) * 2246822519u;
  int r = (h >> 8) % 100;
  return r < 50 ? PASS_ROLL : r < 70 ? PASS_LOB : r < 78 ? PASS_HEADER : r < 84 ? PASS_CHIP : r < 94 ? PASS_SHOT : PASS_HIGH;
}

// How much narrower than the full width the earlier upcoming-games pass is
static float sc(int steps) { return steps / (float)BIG_ROLL_STEPS; }

static float passSecs(PassKind kind, int steps) {
  switch (kind) {
    case PASS_HIGH: return HIGH_AIR_SECS + 0.6f + 0.35f + (steps - 65 * sc(steps)) / HIGH_ROLL_SPEED;
    case PASS_HEADER: return HEADER_RISE_SECS + HEADER_DROP_SECS + (steps - HEADER_LAND * sc(steps)) / HEADER_ROLL_SPEED;
    case PASS_CHIP: return CHIP_SECS;
    case PASS_LOB: return LOB_SECS;
    case PASS_SHOT: return steps / SHOT_SPEED;
    default: return steps == ROLL_STEPS ? ROLL_PASS_SECS : BIG_ROLL_PASS_SECS;
  }
}

// An arc rows high that lasts the whole pass and covers all steps
static int arcPass(float t, float secs, int steps, int rows) {
  float u = min(1.0f, t / secs);
  ballLift = (int)roundf(rows * 4 * u * (1 - u));
  return (int)roundf(steps * u);
}

// Returns the pixels travelled and sets ballLeft and ballLift. big uses the full width; false keeps the earlier short pass.
int ballPass(unsigned long now, bool big) {
  static unsigned long start[2] = {0, 0};
  static uint32_t pass[2] = {0, 0};
  unsigned long &s = start[big];
  uint32_t &n = pass[big];
  const int steps = big ? BIG_ROLL_STEPS : ROLL_STEPS;
  if (now - s > 60000) s = now;  // away for a while
  PassKind kind = passKind(n);
  while (now - s >= (passSecs(kind, steps) + ROLL_BETWEEN_SECS) * 1000) {
    s += (unsigned long)((passSecs(kind, steps) + ROLL_BETWEEN_SECS) * 1000);
    kind = passKind(++n);
  }
  float t = (now - s) / 1000.0f;
  const float k = sc(steps);
  ballLeft = n % 2;
  ballLift = 0;
  switch (kind) {
    case PASS_LOB: return arcPass(t, LOB_SECS, steps, LOB_HEIGHT);
    case PASS_CHIP: return arcPass(t, CHIP_SECS, steps, CHIP_HEIGHT);
    case PASS_SHOT: return arcPass(t, passSecs(kind, steps), steps, SHOT_HEIGHT);
    case PASS_HEADER: {
      if (t < HEADER_RISE_SECS) {
        float u = t / HEADER_RISE_SECS;
        ballLift = (int)roundf(HEADER_HEIGHT * (1 - (1 - u) * (1 - u)));
        return (int)roundf(HEADER_KINK * k * u);
      }
      t -= HEADER_RISE_SECS;
      if (t < HEADER_DROP_SECS) {
        float u = t / HEADER_DROP_SECS;
        ballLift = (int)roundf(HEADER_HEIGHT * (1 - u * u));
        return (int)roundf((HEADER_KINK + (HEADER_LAND - HEADER_KINK) * u) * k);
      }
      t -= HEADER_DROP_SECS;
      return min(steps, (int)roundf(HEADER_LAND * k + HEADER_ROLL_SPEED * t));
    }
    case PASS_HIGH: {
      if (t < HIGH_AIR_SECS) {
        float u = t / HIGH_AIR_SECS;
        ballLift = (int)roundf(HIGH_APEX * 4 * u * (1 - u));
        return (int)roundf(HIGH_LAND * k * u);
      }
      float travelled = HIGH_LAND * k;
      t -= HIGH_AIR_SECS;
      for (const auto &b : HIGH_BOUNCE) {
        if (t < b[0]) {
          float u = t / b[0];
          ballLift = (int)roundf(b[2] * 4 * u * (1 - u));
          return (int)roundf(travelled + b[1] * k * u);
        }
        t -= b[0];
        travelled += b[1] * k;
      }
      return min(steps, (int)roundf(travelled + HIGH_ROLL_SPEED * t));
    }
    default:
      return big ? min(steps, (int)roundf(BIG_ROLL_SPEED * t - BIG_ROLL_SLOWING / 2 * t * t))
                 : min(steps, (int)roundf(ROLL_SPEED * t - ROLL_SLOWING / 2 * t * t));
  }
}

// A coming match in the idle list: kit bars and names in the 5x7 font, the kick-off time between
static void drawComing(MatrixPanel_I2S_DMA *d, const football::FixtureRow &u, time_t local, int y) {
  uint16_t homeColor, awayColor;
  kitColors(u.home.kit, u.away.kit, homeColor, awayColor);
  fill(d, 0, y, 2, 7, homeColor);
  drawText5(d, String(u.home.code), 3, y, TEAM_NAME);
  drawText(d, twoDigits(local % 86400 / 3600), 23, y + 1, DATE);
  fill(d, 31, y + 2, 1, 1, DATE);
  fill(d, 31, y + 4, 1, 1, DATE);
  drawText(d, twoDigits(local % 3600 / 60), 33, y + 1, DATE);
  drawText5(d, String(u.away.code), W - 20, y, TEAM_NAME);
  fill(d, W - 2, y, 2, 7, awayColor);
}

// ---- Flip clock ----

// The digits of the flip clock: bitmaps (see FlipDigits.h), one size for the clock without matches and one for the
// clock next to the kick-offs. Every pixel is 0..32: how much of it the digit covers. Pixels covered less
// than SoftEdge.lo stay tile, more than SoftEdge.hi get the full colour, in between they get a part of it:
// the soft edge of curves and diagonals (the bitmaps have straight edges snapped to all or nothing, so only
// those are soft). A narrower range is a crisper edge.
struct GlyphSet {
  int w, h;
  const uint8_t *pixels;  // [digit][y][x]
};
static const GlyphSet MAIN_GLYPHS = {12, 18, &FLIP_MAIN[0][0]};
static const GlyphSet BLOCK_CLOCK_GLYPHS = {14, 21, nullptr};
static const GlyphSet LIVE_GLYPHS = {10, 12, &FLIP_LIVE[0][0]};

static const uint16_t TILE = rgb(34, 36, 42);

struct SoftEdge { float lo, hi; };
static const SoftEdge WHITE_SOFT = {0.2f, 0.8f};  // the hours
static const SoftEdge GREEN_SOFT = {0.25f, 0.75f};  // the minutes: the brighter colour looks blurrier

// The four digits on the tiles (hours, minutes) and the ones before them. When a digit changes its tile
// flips: the old top half folds down onto the split, then the new bottom half folds out below it.
// tileProgress runs 0..1 during the flip, 1 when it is over, -1 while it waits for the tiles before it (when several
// digits change they flip one after the other, the last one first, FLIP_STAGGER ms apart); update() keeps these.
int clockDigits[4] = {0, 0, 0, 0}, oldDigits[4] = {0, 0, 0, 0};
float tileProgress[4] = {1, 1, 1, 1};
// A changed timeStyle setting flips the last minute tile to itself once, so the new style can be seen straight away:
// flipAll while it lasts, flipSame for the tile being drawn
bool flipAll = false;
static bool flipSame = false;

// A colour at level/32 of its brightness
uint16_t scaleColor(uint16_t c, int level) {
  return rgb((c >> 11 << 3) * level / 32, (c >> 5 & 63) * 4 * level / 32, (c & 31) * 8 * level / 32);
}

// c1 at k/32 of the way from c0
static uint16_t mixColor(uint16_t c0, uint16_t c1, int k) {
  int r0 = c0 >> 11 << 3, g0 = c0 >> 5 & 63, b0 = c0 & 31, r1 = c1 >> 11 << 3, g1 = c1 >> 5 & 63, b1 = c1 & 31;
  return rgb(r0 + (r1 - r0) * k / 32, (g0 + (g1 - g0) * k / 32) << 2, (b0 + (b1 - b0) * k / 32) << 3);
}

static int glyphPixel(const GlyphSet &set, int digit, int x, int y) {
  if (!set.pixels) {
    if (numericfont::enabled()) return numericfont::pixel(digit, x, y, set.w, set.h);
    int gx = x * MAIN_GLYPHS.w / set.w, gy = y * MAIN_GLYPHS.h / set.h;
    return MAIN_GLYPHS.pixels[(digit * MAIN_GLYPHS.h + gy) * MAIN_GLYPHS.w + gx];
  }
  return numericfont::enabled() ? numericfont::pixel(digit, x, y, set.w, set.h)
                                : set.pixels[(digit * set.h + y) * set.w + x];
}

// One row of a tile, drawn at screen row y: the card with its rounded corners, the digit's pixels on it,
// and the split as a black row. level dims it for the folding halves.
static void drawTileRow(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int digit, int x, int y, int w, int h, int row,
                        uint16_t color, const SoftEdge &soft, int level) {
  if (row == h / 2) {
    fill(d, x, y, w, 1, 0);
    return;
  }
  bool edge = row == 0 || row == h - 1;
  uint16_t tile = scaleColor(TILE, level);
  fill(d, edge ? x + 1 : x, y, edge ? w - 2 : w, 1, tile);
  int gy = row - (h - set.h) / 2, gx = x + (w - set.w) / 2;
  if (gy < 0 || gy >= set.h) return;
  float lo = soft.lo * 32, scale = 32 / ((soft.hi - soft.lo) * 32);
  auto share = [&](int i) {
    int pixel = glyphPixel(set, digit, i, gy);
    return (int)constrain((pixel - lo) * scale, 0.0f, 32.0f);
  };  // 0..32 of the colour
  for (int i = 0; i < set.w;) {
    int a = share(i), run = 1;
    if (a == 0) { i++; continue; }
    while (i + run < set.w && share(i + run) == a) run++;
    fill(d, gx + i, y, run, 1, scaleColor(mixColor(TILE, color, a), level));
    i += run;
  }
}

// How the digits change (the timeStyle setting): 0 the flip cards above, 1 fade, 2 roll up, 3 dissolve, 4 drift, 5 fade
// with a shimmer. The styles other than the cards draw only the lit pixels of the digit, on whatever is behind it.
// update() keeps this.
int timeStyle = 2;

// A pseudo-random 0..1 for a pixel of a digit, to dissolve in a different order each time
static float scatter(int i, int j, int digit) {
  uint32_t h = i * 73856093u ^ j * 19349663u ^ digit * 83492791u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return (h % 1000) / 1000.0f;
}

// One digit without a card, in the middle of the w x h box at x, y: level is its brightness 0..32, dy moves it
// down (the rows outside the box are not drawn), dissolve says how far it has dissolved in (+) or out (-) 0..1,
// 0 = not at all.
static void drawPlainGlyph(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int digit, int x, int y, int w, int h, uint16_t color,
                           const SoftEdge &soft, int level, int dy, float dissolve) {
  int gx = x + (w - set.w) / 2, gy = y + (h - set.h) / 2 + dy;
  float lo = soft.lo * 32, scale = 32 / ((soft.hi - soft.lo) * 32);
  float sweep = timeStyle == STYLE_SHIMMER ? fmodf(millis() / 40.0f, 150) - 35 : 0;  // 25 px a second, and a pause
  for (int j = 0; j < set.h; j++) {
    int py = gy + j;
    if (py < y || py >= y + h) continue;
    for (int i = 0; i < set.w; i++) {
      int pixel = glyphPixel(set, digit, i, j);
      int a = (int)constrain((pixel - lo) * scale, 0.0f, 32.0f);
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
      fill(d, gx + i, py, 1, 1, scaleColor(color, (int)(a * lv / 32)));
    }
  }
}

// A tile in the plain styles. progress < 1 changes it from oldDigit to digit.
static void drawPlainTile(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int digit, int oldDigit, float progress, int x, int y, int w,
                          int h, uint16_t color, const SoftEdge &soft) {
  if (progress >= 1 || (oldDigit == digit && !flipSame)) {
    drawPlainGlyph(d, set, digit, x, y, w, h, color, soft, 32, 0, 0);
    return;
  }
  float p = progress;
  if (timeStyle == STYLE_ROLL) {
    int run = h + 2, off = (int)roundf(p * p * (3 - 2 * p) * run);
    drawPlainGlyph(d, set, oldDigit, x, y, w, h, color, soft, 32, -off, 0);
    drawPlainGlyph(d, set, digit, x, y, w, h, color, soft, 32, run - off, 0);
  } else if (timeStyle == STYLE_DISSOLVE) {
    drawPlainGlyph(d, set, oldDigit, x, y, w, h, color, soft, 32, 0, -max(p, 0.001f));
    drawPlainGlyph(d, set, digit, x, y, w, h, color, soft, 32, 0, max(p, 0.001f));
  } else {  // fade, drift and shimmer: the old one out, then the new one in
    int drift = timeStyle == STYLE_DRIFT ? max(2, h / 8) : 0;
    if (p < 0.5f) drawPlainGlyph(d, set, oldDigit, x, y, w, h, color, soft, (int)(32 * (1 - 2 * p)), -(int)roundf(drift * 2 * p), 0);
    else drawPlainGlyph(d, set, digit, x, y, w, h, color, soft, (int)(32 * (2 * p - 1)), (int)roundf(drift * (2 - 2 * p)), 0);
  }
}

// One flip clock tile. progress < 1 flips it from oldDigit to digit.
static void drawFlipTile(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int digit, int oldDigit, float progress, int x, int y,
                         int w, int h, uint16_t color, const SoftEdge &soft) {
  if (timeStyle) {
    drawPlainTile(d, set, digit, oldDigit, progress, x, y, w, h, color, soft);
    return;
  }
  int split = h / 2, below = h - 1 - split;
  bool flipping = progress < 1 && (oldDigit != digit || flipSame);
  for (int row = 0; row < h; row++)  // the new top half over the old bottom half
    drawTileRow(d, set, flipping && row > split ? oldDigit : digit, x, y + row, w, h, row, color, soft, 32);
  if (!flipping) return;
  if (progress < 0.5f) {
    int k = (int)roundf(split * (1 - 2 * progress));
    for (int r = 0; r < k; r++)
      drawTileRow(d, set, oldDigit, x, y + split - k + r, w, h, r * split / k, color, soft, 18 + 14 * k / split);
  } else {
    int k = (int)roundf(below * (2 * progress - 1));
    for (int r = 0; r < k; r++)
      drawTileRow(d, set, digit, x, y + split + 1 + r, w, h, split + 1 + r * below / k, color, soft, 18 + 14 * k / below);
  }
}

// Two tiles side by side: first is the index into clockDigits (0 = hours, 2 = minutes), x the left tile's
// edge, one pixel between them
static void drawFlipPair(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int first, int x, int y, int w, int h, uint16_t color,
                         const SoftEdge &soft) {
  for (int i = 0; i < 2; i++) {
    flipSame = flipAll && first + i == 3;
    int k = first + i;
    bool waiting = tileProgress[k] < 0;
    flipSame = flipAll && k == 3;
    drawFlipTile(d, set, waiting ? oldDigits[k] : clockDigits[k], oldDigits[k], waiting ? 1 : tileProgress[k], x + i * (w + 1), y, w, h,
                 color, soft);
  }
  flipSame = false;
}

// HH:MM on four tiles, no colon between the pairs; x = the left tile's edge, gap = the pixels between the pairs
static void drawTileClock(MatrixPanel_I2S_DMA *d, const GlyphSet &set, int x, int y, int w, int h, int gap, int pulse) {
  drawFlipPair(d, set, 0, x, y, w, h, CLOCK, WHITE_SOFT);
  drawFlipPair(d, set, 2, x + 2 * w + 1 + gap, y, w, h, MINUTES, GREEN_SOFT);
}


// Live with a single match: 12x16 tiles at the bottom
static void drawLiveClock(MatrixPanel_I2S_DMA *d, int pulse) { drawTileClock(d, LIVE_GLYPHS, 5, 47, 12, 16, 4, pulse); }

// The live score on two flip tiles: the digits on them and the ones before, the ms each one's flip starts
// (later than now: waiting for the dip of a new match) and how far it is. -1 = no tiles, the score is
// double figures. update() keeps these.
int scoreDigit[2] = {-1, -1}, scoreOld[2] = {-1, -1};
unsigned long scoreStart[2] = {0, 0};
float scoreProgress[2] = {1, 1};
static const GlyphSet SCORE_GLYPHS = {8, 11, &FLIP_SCORE[0][0]};
static const GlyphSet BLOCK_SCORE_GLYPHS = {8, 12, nullptr};
static const int SCORE_TILE_W = 12, SCORE_TILE_H = 15;

static bool scoreOnTiles() { return scoreDigit[0] >= 0 && scoreDigit[1] >= 0; }

static void drawScoreDigits(MatrixPanel_I2S_DMA *d, int home, int away, int y) {
  const GlyphSet &set = numericfont::enabled() ? BLOCK_SCORE_GLYPHS : SCORE_GLYPHS;
  int x2 = 17 + SCORE_TILE_W + 1 + 4 + 1;
  drawFlipTile(d, set, home, home, 1, 17, y, SCORE_TILE_W, SCORE_TILE_H, INK, WHITE_SOFT);
  drawFlipTile(d, set, away, away, 1, x2, y, SCORE_TILE_W, SCORE_TILE_H, INK, WHITE_SOFT);
  fill(d, 17 + SCORE_TILE_W + 1, y + SCORE_TILE_H / 2 - 1, 4, 2, DASH);
}

// The tiles from x 17, the dash between them, y = the top
static void drawScoreTiles(MatrixPanel_I2S_DMA *d, int y) {
  const GlyphSet &set = numericfont::enabled() ? BLOCK_SCORE_GLYPHS : SCORE_GLYPHS;
  int x2 = 17 + SCORE_TILE_W + 1 + 4 + 1;
  drawFlipTile(d, set, scoreDigit[0], scoreOld[0], scoreProgress[0], 17, y, SCORE_TILE_W, SCORE_TILE_H, INK, WHITE_SOFT);
  drawFlipTile(d, set, scoreDigit[1], scoreOld[1], scoreProgress[1], x2, y, SCORE_TILE_W, SCORE_TILE_H, INK, WHITE_SOFT);
  fill(d, 17 + SCORE_TILE_W + 1, y + SCORE_TILE_H / 2 - 1, 4, 2, DASH);
}

// The stadium colours, shared by the backgrounds of the two ball screens
static const uint16_t BG_CROWD[5] = {rgb(120, 30, 30), rgb(30, 60, 130), rgb(130, 120, 40), rgb(110, 110, 118), rgb(30, 100, 60)};
static const uint16_t BG_FRONT = rgb(22, 25, 33), BG_LINE = rgb(60, 130, 70);

static uint16_t crowdDot(int x, int y, int level, int chance = 72) {  // 0 where this pixel has no dot
  uint32_t h = (x * 73856093u) ^ (y * 19349663u);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (h >> 8) % 100 < chance ? scaleColor(BG_CROWD[(h >> 16) % 5], level) : 0;
}

// The hoardings are ad boards in blocks of 15 pixels, the last column of each a darker seam; the stands have an
// aisle (two columns without crowd) between the blocks above them
static const uint16_t BG_ADS[4] = {rgb(60, 72, 120), rgb(120, 56, 62), rgb(84, 88, 100), rgb(50, 110, 104)};
static uint16_t adAt(int x) { return x % 15 == 14 ? scaleColor(BG_ADS[x / 15 % 4], 16) : BG_ADS[x / 15 % 4]; }
static bool aisleAt(int x) { return x >= 14 && (x + 1) % 15 < 2; }

static uint16_t grassAt(int x, int stripe = 6) { return (x / stripe) % 2 == 0 ? rgb(14, 70, 28) : rgb(10, 54, 22); }

// ---- Main ----

static const int MAIN_BALL_Y = 56;

// Two games at a time; an odd last page advances one game to keep both slots filled.
int mainSlots(unsigned long turn, const football::FixtureRow *slots[2]) {
  size_t count = min(upcomingCount(), size_t(16));
  if (!count) return 0;
  size_t pages = (count + 1) / 2;
  size_t first = count > 2 ? min((size_t)(turn % pages) * 2, count - 2) : 0;
  slots[0] = &snap().fixtures[first];
  if (count > 1) slots[1] = &snap().fixtures[first + 1];
  return count > 1 ? 2 : 1;
}

// When a slot takes another match the old one slides up out of the slot and the new one follows from below;
// when the second takes the first's place and a new one comes in below, they roll up one row. update() keeps the old matches and the ms since
// the slide began (SLOT_SLIDE_MS = done)
static const int SLOT_Y[2] = {3, 35};  // where the slots start
static const int SLOT_PITCH = 32;
football::FixtureRow slotOld[2];
unsigned long slotElapsed[2] = {SLOT_SLIDE_MS, SLOT_SLIDE_MS};
bool slideRoll = false;

// One slot: the day and time centred over the kit bars and names, x 43..62
static void drawSlot(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, const football::FixtureRow &u, int y) {
  uint32_t today = dateTime->localNow() / 86400;
  time_t local = dateTime->utcToLocal(u.kickoff);
  uint32_t day = local / 86400;
  String dayName = day == today ? String("TODAY") : String(DAYS[(day + 4) % 7]);  // 1 Jan 1970 was a Thursday
  String kickoff = twoDigits(local % 86400 / 3600) + ":" + twoDigits(local % 3600 / 60);
  drawText(d, dayName, 43 + (20 - textWidth(dayName)) / 2, y, DATE);
  drawText(d, kickoff, 43 + (20 - textWidth(kickoff)) / 2, y + 6, MINUTES);
  uint16_t homeColor, awayColor;
  kitColors(u.home.kit, u.away.kit, homeColor, awayColor);
  fill(d, 43, y + 12, 2, 7, homeColor);
  drawText5(d, String(u.home.code), 46, y + 12, TEAM_NAME);
  fill(d, 43, y + 20, 2, 7, awayColor);
  drawText5(d, String(u.away.code), 46, y + 20, TEAM_NAME);
}


static int rankingStartY(const RankingPage &page) {
  size_t rows = min((size_t)page.count, (size_t)RANKING_ROWS);
  return RANKING_BOTTOM - 5 - (rows ? (rows - 1) * RANKING_ROW_STEP : 0);
}

static void drawRankingClock(MatrixPanel_I2S_DMA *d) {
  const int digitWidth = 8, digitHeight = 14, pitch = 10;
  const GlyphSet glyphs = {digitWidth, digitHeight, nullptr};
  int x = (CLOCK_RIGHT - (4 * digitWidth + 3 * (pitch - digitWidth))) / 2;
  for (int i = 0; i < 4; i++, x += pitch) {
    bool waiting = tileProgress[i] < 0;
    flipSame = flipAll && i == 3;
    drawFlipTile(d, glyphs, waiting ? oldDigits[i] : clockDigits[i], oldDigits[i], waiting ? 1 : tileProgress[i],
                 x + (i >= 2 ? 1 : 0), 3, digitWidth, digitHeight, i < 2 ? CLOCK : MINUTES,
                 i < 2 ? WHITE_SOFT : GREEN_SOFT);
  }
  flipSame = false;
}

static void drawRankingRows(MatrixPanel_I2S_DMA *d, const RankingPage &page, int y) {
  for (size_t i = 0; page.valid && i < page.count; i++, y += RANKING_ROW_STEP) {
    String place = String(page.firstPosition + i);
    drawText(d, place, 1 + (7 - textWidth(place)) / 2, y, DATE);
    drawText(d, String(page.teams[i]), 10, y, TEAM_NAME);
    if (page.points[i] != INT16_MIN) {
      String points = String(page.points[i]);
      drawText(d, points, 40 - textWidth(points), y, MINUTES);
    }
  }
}

static void drawRankingTitle(MatrixPanel_I2S_DMA *d, const char *name, int y, unsigned long phase, int shift = 0) {
  const int titleWidth = CLOCK_RIGHT - 2;
  int overflow = max(0, textWidth(name) - titleWidth);
  int offset = phase < 500 ? 0 : min(overflow, (int)((phase - 500) * overflow / 2800));
  for (unsigned i = 0; i < strlen(name); i++) {
    const uint8_t *rows = glyph3x5(toupper((unsigned char)name[i]));
    if (!rows) continue;
    int x = 1 + (int)i * 4 - offset + shift;
    for (int row = 0; row < 5; row++)
      for (int px = 0; px < 3; px++)
        if (x + px >= 1 && x + px < CLOCK_RIGHT - 1 && (rows[row] >> (2 - px) & 1))
          fill(d, x + px, y + row, 1, 1, DATE);
  }
}

static void drawRankings(MatrixPanel_I2S_DMA *d, unsigned long now) {
  if (!rankingPage.valid) return;
  now -= rankingStartedAt;
  int currentY = rankingStartY(rankingPage), titleY = currentY - 8;
  clipTop = titleY;
  clipBottom = titleY + 5;
  if (previousRankingValid && strcmp(rankingPage.competition, previousRankingPage.competition) != 0 && now < 200) {
    float p = now / 200.0f;
    int distance = CLOCK_RIGHT - 2;
    int offset = (int)roundf(distance * p * p * (3 - 2 * p));
    drawRankingTitle(d, previousRankingPage.competition, titleY, RANKING_TITLE_MS - 1, -offset);
    drawRankingTitle(d, rankingPage.competition, titleY, now, distance - offset);
  } else {
    drawRankingTitle(d, rankingPage.competition, titleY, min(now, RANKING_TITLE_MS - 1));
  }
  clipTop = 0;
  clipBottom = 64;
  fill(d, 0, currentY - 2, CLOCK_RIGHT, 1, MAIN_DIVIDER);
  clipTop = currentY;
  clipBottom = RANKING_BOTTOM;
  unsigned long slidePhase = now;
  if (previousRankingValid && slidePhase < RANKING_SLIDE_MS) {
    int rows = RANKING_ROWS;
    if (strcmp(rankingPage.competition, previousRankingPage.competition) == 0 &&
        rankingPage.firstPosition > previousRankingPage.firstPosition)
      rows = min(RANKING_ROWS, rankingPage.firstPosition - previousRankingPage.firstPosition);
    int distance = rows * RANKING_ROW_STEP;
    float p = (float)slidePhase / RANKING_SLIDE_MS;
    int offset = (int)roundf(distance * p * p * (3 - 2 * p));
    clipBottom = currentY + distance - offset;
    drawRankingRows(d, previousRankingPage, rankingStartY(previousRankingPage) - offset);
    clipBottom = RANKING_BOTTOM;
    drawRankingRows(d, rankingPage, currentY + distance - offset);
  } else {
    drawRankingRows(d, rankingPage, currentY);
  }
  clipTop = 0;
  clipBottom = 64;
}

// Hours over minutes, or standings below a small clock, with the next fixtures on the right.
void drawMain(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int hour, int minute, int second, int pulse,
                     unsigned long turn, bool showRankings, unsigned long now, int roll) {
  showRankings = showRankings && rankingPage.valid;
  drawStadium(d, 16, !showRankings, false, CLOCK_RIGHT);
  if (showRankings) {
    drawStadium(d, 32, false, false, 0, CLOCK_RIGHT, 0, 8);
    drawStadium(d, 16, false, false, 0, CLOCK_RIGHT, 8, 64);
  } else {
    drawStadium(d, 32, true, false, 0, CLOCK_RIGHT);
  }
  if (!showRankings) {
    if (roll < 0) drawBall(d, W / 3 - 3, MAIN_BALL_Y, 0, 16, W, 12, true);
    else if (ballLeft) drawBall(d, W - roll, MAIN_BALL_Y - ballLift, -roll / 3.5f, 16, W, 12, true);
    else drawBall(d, roll - 7, MAIN_BALL_Y - ballLift, roll / 3.5f, 16, W, 12, true);
  }

  if (showRankings) {
    drawRankingClock(d);
    drawRankings(d, now);
    if (rankingPage.count) {
      size_t last = rankingPage.count - 1;
      const int ballX = 24;  // after the four-letter team column
      int pointsLeft = 40;
      if (rankingPage.points[last] != INT16_MIN)
        pointsLeft -= textWidth(String(rankingPage.points[last]));
      if (pointsLeft >= ballX + 7) {
        drawBall(d, ballX, RANKING_BOTTOM - 7, 0, 16, CLOCK_RIGHT, 12, false, 16);
        // Keep the two bottom edge pixels the same colour as the surrounding grass.
        for (int x : {ballX + 1, ballX + 5})
          d->drawPixel(x, RANKING_BOTTOM - 1, scaleColor(grassAt(x, 12), 16));
      }
    }
  } else {
    const GlyphSet &clockGlyphs = numericfont::enabled() ? BLOCK_CLOCK_GLYPHS : MAIN_GLYPHS;
    const int clockX = (CLOCK_RIGHT - 33) / 2;  // two 16 px tiles and the pixel between them
    drawFlipPair(d, clockGlyphs, 0, clockX, 1, 16, 25, CLOCK, WHITE_SOFT);
    drawFlipPair(d, clockGlyphs, 2, clockX, 28, 16, 25, MINUTES, GREEN_SOFT);
  }

  const football::FixtureRow *slots[2];
  int n = mainSlots(turn, slots);
  bool sliding[2] = {slotElapsed[0] < SLOT_SLIDE_MS, slotElapsed[1] < SLOT_SLIDE_MS};
  if (n == 2 && sliding[0] && sliding[1]) {
    int distance = slideRoll ? SLOT_PITCH : SLOT_PITCH * 2;
    int off = slotElapsed[0] * distance / SLOT_SLIDE_MS;
    clipTop = SLOT_Y[0];
    clipBottom = SLOT_Y[1] + 27;
    for (int slot = 0; slot < 2; slot++) {
      drawSlot(d, dateTime, slotOld[slot], SLOT_Y[slot] - off);
      drawSlot(d, dateTime, *slots[slot], SLOT_Y[slot] + distance - off);
    }
    clipTop = 0;
    clipBottom = 64;
  } else {
    for (int slot = 0; slot < n; slot++) {
      int y = SLOT_Y[slot];
      if (sliding[slot]) {
        int off = slotElapsed[slot] * SLOT_PITCH / SLOT_SLIDE_MS;
        clipTop = y;
        clipBottom = slot == 0 ? SLOT_Y[1] : 64;
        drawSlot(d, dateTime, slotOld[slot], y - off);
        drawSlot(d, dateTime, *slots[slot], y + SLOT_PITCH - off);
        clipTop = 0;
        clipBottom = 64;
      } else {
        drawSlot(d, dateTime, *slots[slot], y);
      }
    }
  }
  fill(d, CLOCK_RIGHT, 0, 1, 64, MAIN_DIVIDER);
}

// A single LED in the very top right corner, in every view: how the downloads are going. Amber pulsing until the
// first ones are in (later ones, which come all the time during matches, stay quiet), red when the last ones
// failed, nothing when all is well.
void drawStatusDot(MatrixPanel_I2S_DMA *d, FootballTicker::Status status, int pulse) {
  if (status == FootballTicker::OK) return;
  d->drawPixel(W - 1, 0, status == FootballTicker::LOADING ? dimmed(240, 170, 30, pulse) : RED_CARD);
}

// No matches at all: HH:MM on flip clock tiles over a stadium, the crowd getting sparser and dimmer upwards from the
// band at the bottom (see ultimatrix-mockups/football-stadium-clock.py, "C"),
// the date in the top left corner and the seconds in the top right. The ball is passed to and fro along the
// grass of the band like on the kick-off screen (ballPass), over the full width: kicked a bit harder to get across.
static const int CLOCK_Y = 19, CROWD_TOP = 2, BAND_Y = 50;  // the tiles' top, the highest crowd row, the first crowd row of the band
static const int BALL_Y = 56;  // its bottom row, 62, is in the grass

// What the band (see drawStadium) shows at x, y, for the soft edge of the ball;
// above the band: black
static uint16_t backgroundAt(int x, int y, int stripe, bool withBoards = true) {
  int row = y - BAND_Y;
  if (row < 0 || row == 7) return 0;
  if (row < 7) return row % 2 || x % 2 != (y / 2) % 2 || aisleAt(x) ? 0 : crowdDot(x, y, 12 + 3 * row / 2);
  if (!withBoards) return row < 11 ? 0 : grassAt(x, stripe);
  return row == 8 ? BG_FRONT : row < 11 ? adAt(x) : row == 11 ? BG_LINE : grassAt(x, stripe);
}

void drawStadium(MatrixPanel_I2S_DMA *d, int level, bool withBoards, bool clockGaps, int fromX, int toX, int fromY, int toY) {
  for (int y = CROWD_TOP; y < BAND_Y; y += 2) {  // the stands behind the clock: thinner and dimmer the higher
    if (y < fromY || y >= toY) continue;
    int k = 100 * (BAND_Y - y) / (BAND_Y - CROWD_TOP);  // 0 at the band .. 100 at the top
    int firstX = (y / 2) % 2;
    while (firstX < fromX) firstX += 2;
    for (int x = firstX; x < toX; x += 2) {
      if (aisleAt(x) || (clockGaps && y < 8 && (x < 22 || x >= 53))) continue;  // the date and the seconds stand clear of it
      if (clockGaps && (x == 15 || x == 48) && y >= CLOCK_Y && y < CLOCK_Y + 26) continue;  // and the seams between the digits
      uint16_t c = crowdDot(x, y, 14 - 8 * k / 100, 80 - 30 * k / 100);  // still there at the top, just thinner and dimmer
      if (c) d->drawPixel(x, y, scaleColor(c, level));
    }
  }
  for (int y = BAND_Y; y <= BAND_Y + 6; y += 2) {  // the band: four rows of crowd, a little brighter lower down
    if (y < fromY || y >= toY) continue;
    int firstX = (y / 2) % 2;
    while (firstX < fromX) firstX += 2;
    for (int x = firstX; x < toX; x += 2) {
      if (aisleAt(x)) continue;
      uint16_t c = crowdDot(x, y, 12 + 3 * (y - BAND_Y) / 2);
      if (c) d->drawPixel(x, y, scaleColor(c, level));
    }
  }
  if (withBoards) {
    if (BAND_Y + 8 >= fromY && BAND_Y + 8 < toY) d->fillRect(fromX, BAND_Y + 8, toX - fromX, 1, scaleColor(BG_FRONT, level));
    for (int x = fromX; x < toX; x++)
      if (BAND_Y + 9 >= fromY && BAND_Y + 10 < toY) d->fillRect(x, BAND_Y + 9, 1, 2, scaleColor(adAt(x), level));
    if (BAND_Y + 11 >= fromY && BAND_Y + 11 < toY) d->fillRect(fromX, BAND_Y + 11, toX - fromX, 1, scaleColor(BG_LINE, level));
  }
  int grassY = BAND_Y + (withBoards ? 12 : 11), grassH = withBoards ? 2 : 3;
  if (grassY < fromY) {
    grassH -= fromY - grassY;
    grassY = fromY;
  }
  if (grassY + grassH > toY) grassH = toY - grassY;
  for (int x = fromX; grassH > 0 && x < toX; x++)
    d->fillRect(x, grassY, 1, grassH, scaleColor(grassAt(x, 12), level));
}

void drawBigClock(MatrixPanel_I2S_DMA *d, ClockTime *dateTime, int second, int roll) {
  drawStadium(d);
  String date = String(dateTime->getDay()) + " " + MONTHS[(dateTime->getMonth() - 1 + 12) % 12];  // "6 OCT", top left
  drawText(d, date, 2, 2, DATE);
  drawText(d, twoDigits(second), W - 2 - textWidth("00"), 2, MINUTES);  // the seconds, top right
  const GlyphSet &clockGlyphs = numericfont::enabled() ? BLOCK_CLOCK_GLYPHS : MAIN_GLYPHS;
  const int clockOffset = numericfont::enabled() ? -1 : 0;
  drawFlipPair(d, clockGlyphs, 0, 1 + clockOffset, CLOCK_Y, 14, 26, CLOCK, WHITE_SOFT);
  drawFlipPair(d, clockGlyphs, 2, 34 + clockOffset, CLOCK_Y, 14, 26, MINUTES, GREEN_SOFT);
  if (roll < 0) drawBall(d, W / 3 - 3, BALL_Y, 0, 32, W, 12);  // resting a third of the grass from the left
  else if (ballLeft) drawBall(d, W - roll, BALL_Y - ballLift, -roll / 3.5f, 32, W, 12);
  else drawBall(d, roll - 7, BALL_Y - ballLift, roll / 3.5f, 32, W, 12);
}
// ---- Cards and substitutions: a few seconds full screen, like a goal ----

static const uint16_t PITCH_LIGHT = rgb(14, 34, 18), PITCH_DARK = rgb(10, 18, 12);
static const uint16_t OFF_RED = rgb(235, 70, 60), ON_GREEN = rgb(60, 220, 90), CARD_INK = rgb(18, 18, 20);

static bool lightColor(uint16_t c) {
  return ((c >> 11) & 31) * 8 * 3 + ((c >> 5) & 63) * 4 * 6 + (c & 31) * 8 > 1200;
}

// The stripes of the pitch behind it, and a bar with the team and the minute
static void drawIncidentBackground(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, bool withHeader) {
  for (int y = 0; y < 64; y += 4) fill(d, 0, y, W, 4, y % 8 ? PITCH_DARK : PITCH_LIGHT);
  if (!withHeader) return;
  uint16_t homeColor, awayColor;
  kitColors(in.homeKit, in.awayKit, homeColor, awayColor);
  String minute = String(in.minute) + "'";
  fill(d, 0, 0, W, 9, rgb(24, 30, 26));
  fill(d, 2, 2, 2, 5, in.homeTeam ? homeColor : awayColor);
  drawText(d, in.homeTeam ? in.home : in.away, 6, 2, TEAM_NAME);
  drawText(d, minute, W - 2 - textWidth(minute), 2, MINUTES);
}

// A card 26 x 34 with the shirt number on it
static void drawCard(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, int y, bool withNumber) {
  uint16_t color = in.kind == 'y' ? YELLOW_CARD : RED_CARD;
  fill(d, 20, y + 1, 26, 34, rgb(4, 8, 5));  // shadow
  fill(d, 19, y, 26, 34, color);
  if (withNumber && in.number) drawNumber(d, String(min((int)in.number, 99)), 32, y + 11, in.kind == 'y' ? CARD_INK : INK, 2);
}

static const int CARD_RISE = 500, CARD_FLASH = 200, CARD_STAY = 2900, CARD_LEAVE = 500;
static const int SUB_HOLD = 600, SUB_SLIDE = 600, SUB_SHOW = 4000;

static void drawCardFrame(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, int t) {
  const int RISE = CARD_RISE, FLASH = CARD_FLASH, STAY = CARD_STAY, LEAVE = CARD_LEAVE, LANDED = 15;
  uint16_t color = in.kind == 'y' ? YELLOW_CARD : RED_CARD;
  {
    int y = LANDED;
    if (t < RISE) {
      float u = 1 - t / (float)RISE;
      y = LANDED + (int)roundf((64 - LANDED) * u * u);  // quick, then settling
    } else if (t >= STAY) {
      float u = (t - STAY) / (float)LEAVE;
      y = LANDED - (int)roundf((LANDED + 36) * u * u);
    }
    d->fillScreen(0);
    drawIncidentBackground(d, in, t >= 250);
    drawCard(d, in, y, t >= RISE);
    if (in.name[0] && t >= RISE + FLASH) drawCentered(d, in.name, 52, INK);
    // The flash: a red card twice
    int sinceLanding = t - RISE;
    bool flash = sinceLanding >= 0 && (sinceLanding < FLASH || (in.kind == 'r' && sinceLanding >= 2 * FLASH && sinceLanding < 3 * FLASH));
    if (flash) {
      fill(d, 0, 0, 2, 64, color);
      fill(d, 62, 0, 2, 64, color);
      fill(d, 0, 0, W, 2, color);
      fill(d, 0, 62, W, 2, color);
    }
  }
}

// A shirt twice the size of drawShirt's, with its number on the chest (0 = none)
static void drawBigShirt(MatrixPanel_I2S_DMA *d, int x, int y, uint16_t shirt, uint16_t trim, uint8_t number) {
  fill(d, x + 2, y + 2, 24, 2, shirt);
  fill(d, x, y + 4, 28, 6, shirt);
  fill(d, x + 6, y + 10, 16, 14, shirt);
  fill(d, x + 10, y + 2, 8, 2, trim);
  fill(d, x + 12, y + 4, 4, 2, trim);
  fill(d, x, y + 4, 2, 6, trim);
  fill(d, x + 26, y + 4, 2, 6, trim);
  if (number) drawNumber(d, String(min((int)number, 99)), x + 14, y + 12, lightColor(shirt) ? CARD_INK : INK, 1);
}

static void drawSubstitutionFrame(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, int t) {
  const int HOLD = SUB_HOLD, SLIDE = SUB_SLIDE, SHIRT_Y = 22;
  uint16_t homeColor, awayColor;
  kitColors(in.homeKit, in.awayKit, homeColor, awayColor);
  const TeamKit &kit = in.homeTeam ? in.homeKit : in.awayKit;
  uint16_t shirt = in.homeTeam ? homeColor : awayColor, trim = trimColor(kit, shirt);
  {
    float u = constrain((t - HOLD) / (float)SLIDE, 0.0f, 1.0f);
    u = 1 - (1 - u) * (1 - u);  // eases out
    d->fillScreen(0);
    drawIncidentBackground(d, in, true);
    drawCentered(d, "SUBSTITUTION", 12, DATE);
    blockDim = 32 - (int)roundf(12 * u);  // the player going off dims
    drawBigShirt(d, 18 - (int)roundf(15 * u), SHIRT_Y, shirt, trim, in.number);
    blockDim = 32;
    if (t >= HOLD) drawBigShirt(d, 64 - (int)roundf(31 * u), SHIRT_Y, shirt, trim, in.numberOn);
    if (u >= 1) {
      drawText(d, "OFF", 12, 50, OFF_RED);  // centred under the shirts, at x 17 and 47
      fill(d, 15, 57, 5, 1, OFF_RED);  // arrow down
      fill(d, 16, 58, 3, 1, OFF_RED);
      fill(d, 17, 59, 1, 1, OFF_RED);
      drawText(d, "ON", 44, 50, ON_GREEN);
      fill(d, 47, 57, 1, 1, ON_GREEN);  // arrow up
      fill(d, 46, 58, 3, 1, ON_GREEN);
      fill(d, 45, 59, 5, 1, ON_GREEN);
    }
  }
}

int incidentFrames(const FootballTicker::Incident &in) {
  return ((in.kind == 's' ? SUB_SHOW : CARD_STAY + CARD_LEAVE) + INCIDENT_FRAME_MS - 1) / INCIDENT_FRAME_MS;
}

void drawIncidentFrame(MatrixPanel_I2S_DMA *d, const FootballTicker::Incident &in, int frame) {
  if (in.kind == 's') drawSubstitutionFrame(d, in, frame * INCIDENT_FRAME_MS);
  else drawCardFrame(d, in, frame * INCIDENT_FRAME_MS);
}

}  // namespace fbview
