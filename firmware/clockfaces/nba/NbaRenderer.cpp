// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NbaRenderer.h"

#include <BoldFont.h>
#include <FaceKit.h>
#include <MiniFont.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>

using namespace facekit;

namespace nbaview {
namespace {

const uint16_t kText = rgb(225, 225, 225), kDim = rgb(100, 100, 112), kMid = rgb(160, 160, 170), kOrange = rgb(235, 110, 20),
               kLive = rgb(60, 220, 90), kFav = rgb(255, 205, 40), kCourt = rgb(40, 17, 5), kBall = rgb(235, 115, 25), kSeam = rgb(80, 30, 6),
               kRed = rgb(230, 50, 50);

// ---- small helpers ----

int textWidth(const char *s) { return miniWidth(s); }
void centered(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t colour) { miniText(d, s, (W - textWidth(s)) / 2, y, colour); }
void rightAligned(MatrixPanel_I2S_DMA &d, const char *s, int xRight, int y, uint16_t colour) { miniText(d, s, xRight - textWidth(s) + 1, y, colour); }

// 3x5 letters at twice the size
void text2x(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) {
  for (; *s; s++, x += 7) {
    const uint8_t *g = miniGlyph(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g[row] >> (2 - col) & 1) box(d, x + col * 2, y + row * 2, 2, 2, colour);
  }
}
int width2x(const char *s) { return strlen(s) ? (int)strlen(s) * 7 - 1 : 0; }

void disc(MatrixPanel_I2S_DMA &d, int cx, int cy, int r, uint16_t colour) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r / 2) put(d, cx + x, cy + y, colour);
}

// A basketball: the seams turn with `spin` (radians) so a rolling ball looks like one
void ball(MatrixPanel_I2S_DMA &d, int cx, int cy, int r, float spin) {
  disc(d, cx, cy, r, kBall);
  const int seam = (int)lroundf(sinf(spin) * r * 0.55f);
  for (int i = -r; i <= r; i++) {
    if (i * i + r * r > r * r * 2) continue;
    put(d, cx + seam, cy + i, kSeam);   // the vertical seam
    put(d, cx + i, cy, kSeam);          // the equator
  }
}

uint16_t pulse(uint16_t colour, uint32_t ms, uint32_t period, int low = 110) {
  const float s = 0.5f + 0.5f * sinf(ms * 6.2831853f / period);
  return dim(colour, low + (int)((255 - low) * s));
}

// Defined with the backgrounds below
}  // namespace
void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms);
namespace {
bool backgroundOff();
void backdrop(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { if (!backgroundOff()) drawBackground(d, background, ms); }

// ---- the court behind the pages ----

void arc(MatrixPanel_I2S_DMA &d, int cx, int cy, int r, int fromDeg, int toDeg, int clipBottom, uint16_t colour) {
  for (int a = fromDeg * 2; a <= toDeg * 2; a++) {
    const float rad = a * 0.5f * 0.0174533f;
    const int x = cx + (int)lroundf(cosf(rad) * r), y = cy - (int)lroundf(sinf(rad) * r);
    if (y < clipBottom) put(d, x, y, colour);
  }
}

// Half a court seen from above, the basket at the bottom: three point line, paint, free throw circle
void court(MatrixPanel_I2S_DMA &d, uint16_t colour = kCourt) {
  box(d, 3, 52, 1, 12, colour);   // the corner lines
  box(d, 60, 52, 1, 12, colour);
  arc(d, 32, 52, 28, 0, 180, 64, colour);
  box(d, 22, 30, 1, 34, colour);  // the paint
  box(d, 41, 30, 1, 34, colour);
  box(d, 22, 30, 20, 1, colour);  // the free throw line
  arc(d, 32, 30, 9, 0, 180, 30, colour);
  box(d, 28, 61, 9, 1, colour);   // the backboard
  arc(d, 32, 58, 2, 0, 360, 64, colour);
}

// Centre court: the halfway line and the circle
void centreCourt(MatrixPanel_I2S_DMA &d, uint16_t colour = kCourt) {
  box(d, 0, 32, W, 1, colour);
  arc(d, 32, 32, 11, 0, 360, 64, colour);
  arc(d, 32, 32, 3, 0, 360, 64, colour);
  box(d, 0, 13, 1, 51, colour);
  box(d, 63, 13, 1, 51, colour);
}

// The hardwood floor: planks in rows with staggered joints
void parquet(MatrixPanel_I2S_DMA &d, int top = 13) {
  const uint16_t plank = rgb(26, 11, 3), joint = rgb(10, 4, 1);
  for (int y = top, row = 0; y < H; y += 4, row++) {
    box(d, 0, y, W, 3, plank);
    for (int x = (row % 2) * 8 + 3; x < W; x += 16) box(d, x, y, 1, 3, joint);
  }
}

// Arena lights: two beams swaying from the ceiling, brighter near the lamp and in the middle. The panel has few steps between black and
// dim, so the gradient is cut into 13 levels it can tell apart and each pixel takes the level above or below by an ordered pattern:
// the edges between levels blur instead of showing as bands
void beams(MatrixPanel_I2S_DMA &d, uint32_t ms, int top = 13) {
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  constexpr int kLevels = 13;
  for (int b = 0; b < 2; b++) {
    const float centre = (b ? 50 : 14) + 7 * sinf(ms * 0.0007f + b * 2.1f);
    for (int y = top; y < H; y++) {
      const float spread = 3 + (y - top) * 0.55f;
      const float fade = 1.0f - (y - top) / (float)(H - top);
      for (int x = (int)(centre - spread); x <= (int)(centre + spread); x++) {
        const float edge = 1.0f - fabsf(x - centre) / (spread + 1);
        const float level = kLevels * fade * (0.35f + 0.65f * edge);
        int k = (int)level;
        if (bayer[y & 3][x & 3] < (int)((level - k) * 16)) k++;
        if (k > 0) put(d, x, y, rgb(k * 46 / kLevels, k * 56 / kLevels, k * 92 / kLevels));
      }
    }
  }
}

// The net seen from below: crossing diagonals in a conference's colour
void netting(MatrixPanel_I2S_DMA &d, uint16_t colour, int top = 13) {
  for (int y = top; y < H; y++)
    for (int x = (y - top) % 6; x < W; x += 6) { put(d, x, y, colour); put(d, W - 1 - x, y, colour); }
}

// ---- times ----

void clockText(ClockTime *time, char *out, size_t size) {
  if (time->is24hFormat()) snprintf(out, size, "%02d:%02d", time->getHour(), time->getMinute());
  else snprintf(out, size, "%d:%02d", time->getHour(), time->getMinute());
}

// The local tip-off time: "7:30P" or "19:30"
void tipOff(ClockTime *time, int64_t start, char *out, size_t size) {
  struct tm local;
  time_t t = time->utcToLocal((time_t)start);
  gmtime_r(&t, &local);
  if (time->is24hFormat()) snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
  else snprintf(out, size, "%d:%02d%c", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min, local.tm_hour < 12 ? 'A' : 'P');
}

void periodLabel(const nba::Game &g, char *out, size_t size) {
  if (g.period <= 4) snprintf(out, size, "Q%d", g.period);
  else if (g.period == 5) snprintf(out, size, "OT");
  else snprintf(out, size, "%dOT", g.period - 4);
}

void status(const nba::Game &g, ClockTime *time, char *out, size_t size, uint16_t &colour, bool compact) {
  char period[8];
  colour = kDim;
  switch (g.state) {
    case nba::Live:
      colour = kLive;
      periodLabel(g, period, sizeof(period));
      if (g.halftime) snprintf(out, size, "HALF");
      else if (g.endOfPeriod) snprintf(out, size, "END %s", period);
      else snprintf(out, size, "%s %s", period, g.clock);
      break;
    case nba::Final:
      if (g.period > 4) { periodLabel(g, period, sizeof(period)); snprintf(out, size, "FINAL %s", period); }
      else snprintf(out, size, "FINAL");
      break;
    default:
      tipOff(time, g.start, out, size);
      if (!compact && !time->is24hFormat()) strlcat(out, "M", size);
  }
}

// How far into the game the clock is, in quarters (0 to 4, more in overtime is cut to 4)
float gameProgress(const nba::Game &g) {
  if (g.state == nba::Final) return 4;
  if (g.state != nba::Live || g.period == 0) return 0;
  if (g.period > 4) return 4;
  float left = 0;
  int minutes = 0;
  float seconds = 0;
  if (strchr(g.clock, ':')) { sscanf(g.clock, "%d:%f", &minutes, &seconds); left = minutes * 60 + seconds; }
  else left = atof(g.clock);
  float done = 1 - left / 720.0f;
  if (g.endOfPeriod || g.halftime) done = 1;
  return (g.period - 1) + (done < 0 ? 0 : done > 1 ? 1 : done);
}

void header(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, bool live, const char *label = "NBA") {
  char text[8];
  clockText(time, text, sizeof(text));
  miniText(d, label, 1, 3, kOrange);
  text2x(d, text, (W - width2x(text)) / 2 + 5, 1, kText);
  if (live) box(d, 60, 4, 3, 3, pulse(kRed, ms, 1000, 60));
  box(d, 0, 12, W, 1, dim(kOrange, 150));
}

void pageDots(MatrixPanel_I2S_DMA &d, int page, int pages, int y) {
  if (pages < 2) return;
  const int width = pages * 4 - 2;
  for (int i = 0; i < pages; i++) box(d, (W - width) / 2 + i * 4, y, 2, 1, i == page ? kText : dim(kDim, 150));
}

// A short name of a conference table row's streak, etc. are drawn with the mini font only

}  // namespace

bool backgroundByFace = false;
namespace {
bool backgroundOff() { return backgroundByFace; }
}

// ---- backgrounds: drawn by the pages themselves, or once by the face while two pages that share one slide over each other ----

namespace {
void paint(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) {
  switch (background) {
    case BgGame: court(d); break;
    case BgQuarters: centreCourt(d); break;
    case BgToday: parquet(d); break;
    case BgNext: beams(d, ms); break;
    case BgEast: netting(d, rgb(10, 18, 44)); break;
    case BgWest: netting(d, rgb(40, 11, 11)); break;
    case BgClock: {  // the arena: beams, the floor along the bottom, a hoop on the right
      beams(d, ms, 0);
      box(d, 0, 57, W, 7, rgb(30, 13, 4));
      for (int x = 5; x < W; x += 16) box(d, x, 57, 1, 7, rgb(12, 5, 1));
      box(d, 0, 57, W, 1, rgb(60, 26, 6));
      box(d, 60, 29, 2, 16, kMid);
      box(d, 49, 41, 12, 2, kBall);
      for (int k = 0; k < 7; k++) {
        const int left = 50 + k / 3, right = 59 - k / 3;
        for (int x = left; x <= right; x++) if ((x + k) % 2 == 0) put(d, x, 43 + k, rgb(120, 120, 130));
      }
      return;
    }
  }
  box(d, 0, 0, W, 12, rgb(30, 12, 3));  // the band behind the time
}
}  // namespace

void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { paint(d, background, ms); }
void backgroundDrawnByFace(bool yes) { backgroundByFace = yes; }

// ---- favourites and order ----

bool isFavourite(const char *favourites, const char *abbr) {
  if (!favourites || !abbr[0]) return false;
  const size_t n = strlen(abbr);
  for (const char *p = favourites; *p;) {
    const char *end = strchr(p, ',');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !strncasecmp(p, abbr, n)) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

bool hasFavourite(const char *favourites, const nba::Game &game) { return isFavourite(favourites, game.away) || isFavourite(favourites, game.home); }

int orderGames(const nba::Snapshot &snap, const char *favourites, int *out) {
  int n = 0;
  // favourites first, then live, coming and finished: one pass per rank keeps ESPN's own order inside a rank
  for (int rank = 0; rank < 6; rank++)
    for (int i = 0; i < snap.count; i++) {
      const nba::Game &g = snap.games[i];
      const int r = (hasFavourite(favourites, g) ? 0 : 3) + (g.state == nba::Live ? 0 : g.state == nba::Pre ? 1 : 2);
      if (r == rank) out[n++] = i;
    }
  return n;
}

int gamePages(int count) { return count ? (count + kGamesPerPage - 1) / kGamesPerPage : 0; }

// ---- the upcoming days as lines: a day header, then its games ----

namespace {
struct Line { bool day; int index; };  // a date line (index = the first fixture of the day), or a fixture

int fixtureDay(ClockTime *time, uint32_t start) {
  time_t t = time->utcToLocal((time_t)start);
  return (int)(t / 86400);
}

int lines(const nba::Snapshot &snap, ClockTime *time, Line *out, int max) {
  int n = 0, lastDay = -1;
  for (int i = 0; i < snap.nextCount && n < max - 1; i++) {
    const int day = fixtureDay(time, snap.next[i].start);
    if (day != lastDay) { out[n++] = {true, i}; lastDay = day; }
    out[n++] = {false, i};
  }
  return n;
}

constexpr int kUpcomingLines = 7;

// Pages of lines; a page that starts inside a day gets that day's header again (and has one fixture less)
int pageStart(const Line *all, int n, int page, bool &again) {
  int start = 0;
  again = false;
  for (int p = 0; p < page && start < n; p++) {
    start += kUpcomingLines - (again ? 1 : 0);
    again = start < n && !all[start].day;
  }
  return start;
}
}  // namespace

int fixturePages(const nba::Snapshot &snap, ClockTime *time) {
  Line all[nba::kMaxFixtures * 2];
  const int n = lines(snap, time, all, nba::kMaxFixtures * 2);
  int pages = 0, start = 0;
  bool again = false;
  while (start < n) {
    start += kUpcomingLines - (again ? 1 : 0);
    again = start < n && !all[start].day;
    pages++;
  }
  return pages;
}

int standingsPages(const nba::Table &table) { return table.count ? (table.count + kStandingsPerPage - 1) / kStandingsPerPage : 0; }

// ---- pages ----

void drawGame(MatrixPanel_I2S_DMA &d, const nba::Game &g, ClockTime *time, uint32_t ms, bool flashAway, bool flashHome, bool favourite, int page, int pages) {
  backdrop(d, BgGame, ms);
  header(d, time, ms, g.state == nba::Live);
  if (favourite) box(d, 0, 0, 1, 12, kFav);
  const bool away = g.awayScore > g.homeScore, home = g.homeScore > g.awayScore;
  for (int side = 0; side < 2; side++) {
    const bool isHome = side == 1;
    const int y = isHome ? 35 : 15;
    const uint16_t colour = isHome ? g.homeColor : g.awayColor;
    const int score = isHome ? g.homeScore : g.awayScore;
    const bool flash = isHome ? flashHome : flashAway;
    const bool leads = isHome ? home : away;
    if (flash) box(d, 3, y, W - 3, 18, dim(colour, 60 + (int)(40 * sinf(ms * 0.02f))));
    box(d, 0, y, 3, 18, colour);
    text2x(d, isHome ? g.home : g.away, 5, y + 1, g.state == nba::Pre || leads || flash ? kText : kMid);
    miniText(d, isHome ? g.homeRecord : g.awayRecord, 5, y + 12, kDim);
    if (g.state != nba::Pre) {
      const int width = blockNumber(d, score, 0, 0, 8, 14, 0, true);
      blockNumber(d, score, W - 2 - width, y + 2, 8, 14, leads || flash || g.state == nba::Live ? (flash ? kText : colour) : kDim);
    }
  }
  char text[16];
  uint16_t colour;
  status(g, time, text, sizeof(text), colour, false);
  centered(d, text, 54, colour);
  // The quarter bar: four segments that fill as the game goes
  const float progress = gameProgress(g);
  for (int q = 0; q < 4; q++) {
    const int x = 2 + q * 15;
    const float fill = progress - q < 0 ? 0 : progress - q > 1 ? 1 : progress - q;
    box(d, x, 60, 14, 1, dim(kDim, 90));
    if (fill > 0) box(d, x, 60, (int)lroundf(fill * 14), 1, g.state == nba::Final ? kMid : kLive);
  }
  pageDots(d, page, pages, 62);
}

void drawQuarters(MatrixPanel_I2S_DMA &d, const nba::Game &g, ClockTime *time, uint32_t ms) {
  backdrop(d, BgQuarters, ms);
  header(d, time, ms, g.state == nba::Live, "QTR");
  int periods = g.period;
  if (g.state == nba::Final && periods < 4) periods = 4;
  if (periods < 1) periods = 1;
  if (periods > nba::kPeriods) periods = nba::kPeriods;
  const int shown = periods > 5 ? 5 : periods, first = periods - shown;  // the last five periods
  const int awayX = 31, homeX = 59;  // right edges of the two columns
  box(d, 0, 15, 2, 5, g.awayColor);
  miniText(d, g.away, 4, 15, g.awayColor);
  box(d, 33, 15, 2, 5, g.homeColor);
  miniText(d, g.home, 37, 15, g.homeColor);
  for (int i = 0; i < shown; i++) {
    const int period = first + i + 1, y = 22 + i * 6;
    char label[4];
    if (period <= 4) snprintf(label, sizeof(label), "Q%d", period);
    else snprintf(label, sizeof(label), period == 5 ? "OT" : "%dO", period - 4);
    const int a = g.awayQ[first + i], h = g.homeQ[first + i];
    const bool now = g.state == nba::Live && period == periods;
    miniText(d, label, 1, y, now ? kLive : kDim);
    char number[6];
    snprintf(number, sizeof(number), "%d", a);
    rightAligned(d, number, awayX - 4, y, a > h || now ? kText : kDim);
    snprintf(number, sizeof(number), "%d", h);
    rightAligned(d, number, homeX - 4, y, h > a || now ? kText : kDim);
    // the margin as a short bar between the columns, towards the winner
    const int len = (abs(a - h) > 10 ? 10 : abs(a - h)) / 2;
    if (a > h) box(d, 38 - len, y + 2, len + 1, 1, g.awayColor);
    else if (h > a) box(d, 38, y + 2, len + 1, 1, g.homeColor);
  }
  const int line = 22 + shown * 6 + 1;
  box(d, 0, line, W, 1, dim(kDim, 130));
  char total[6];
  const bool awayLeads = g.awayScore > g.homeScore, homeLeads = g.homeScore > g.awayScore;
  miniText(d, "T", 1, line + 2, kDim);
  snprintf(total, sizeof(total), "%d", g.awayScore);
  rightAligned(d, total, awayX - 4, line + 2, awayLeads || g.state == nba::Live ? kText : kDim);
  snprintf(total, sizeof(total), "%d", g.homeScore);
  rightAligned(d, total, homeX - 4, line + 2, homeLeads || g.state == nba::Live ? kText : kDim);
  char text[16];
  uint16_t colour;
  status(g, time, text, sizeof(text), colour, false);
  centered(d, text, 58, colour);
}

void drawToday(MatrixPanel_I2S_DMA &d, const nba::Snapshot &snap, const int *order, int count, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  backdrop(d, BgToday, ms);
  header(d, time, ms, snap.live > 0);
  const int first = page * kGamesPerPage;
  for (int row = 0; row < kGamesPerPage && first + row < count; row++) {
    const nba::Game &g = snap.games[order[first + row]];
    const int y = 14 + row * 12;
    const bool fav = hasFavourite(favourites, g);
    const bool final = g.state == nba::Final, pre = g.state == nba::Pre;
    const bool awayWon = final && g.awayScore > g.homeScore, homeWon = final && g.homeScore > g.awayScore;
    if (fav) box(d, 0, y, 1, 11, kFav);
    miniText(d, g.away, 2, y + 1, final && !awayWon ? dim(g.awayColor, 150) : g.awayColor);
    miniText(d, g.home, 35, y + 1, final && !homeWon ? dim(g.homeColor, 150) : g.homeColor);
    if (pre) {
      miniText(d, "-", 30, y + 1, kDim);
    } else {
      char a[6], h[6];
      snprintf(a, sizeof(a), "%d", g.awayScore);
      snprintf(h, sizeof(h), "%d", g.homeScore);
      rightAligned(d, a, 28, y + 1, final && !awayWon ? kDim : kText);
      rightAligned(d, h, 62, y + 1, final && !homeWon ? kDim : kText);
    }
    char text[16];
    uint16_t colour;
    status(g, time, text, sizeof(text), colour, true);
    miniText(d, text, 2, y + 7, colour);
  }
  pageDots(d, page, gamePages(count), 62);
}

void drawUpcoming(MatrixPanel_I2S_DMA &d, const nba::Snapshot &snap, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  backdrop(d, BgNext, ms);
  header(d, time, ms, snap.live > 0, "NEXT");
  Line all[nba::kMaxFixtures * 2];
  const int n = lines(snap, time, all, nba::kMaxFixtures * 2);
  bool again;
  int at = pageStart(all, n, page, again);
  int y = 15, row = 0;
  auto day = [&](int fixture) {
    struct tm local;
    time_t when = time->utcToLocal((time_t)snap.next[fixture].start);
    gmtime_r(&when, &local);
    static const char *const names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    char text[24];
    snprintf(text, sizeof(text), "%s %d", names[local.tm_wday], local.tm_mday % 100);
    miniText(d, text, 1, y, kOrange);
    box(d, textWidth(text) + 4, y + 2, W - textWidth(text) - 5, 1, dim(kOrange, 70));
  };
  if (again) {  // the day this page continues
    int back = at;
    while (back > 0 && !all[back].day) back--;
    day(all[back].index);
    y += 7;
    row = 1;
  }
  for (; row < kUpcomingLines && at < n; row++, at++, y += 7) {
    const Line &line = all[at];
    const nba::Fixture &f = snap.next[line.index];
    if (line.day) {
      day(line.index);
    } else {
      const bool fav = isFavourite(favourites, f.away) || isFavourite(favourites, f.home);
      if (fav) box(d, 0, y, 1, 5, kFav);
      miniText(d, f.away, 2, y, kText);
      miniText(d, "-", 18, y, kDim);
      miniText(d, f.home, 23, y, kText);
      char text[8];
      tipOff(time, f.start, text, sizeof(text));
      rightAligned(d, text, 62, y, kMid);
    }
  }
  if (!n) centered(d, "NO GAMES", 30, kDim);
}

void drawStandings(MatrixPanel_I2S_DMA &d, const nba::Table &table, bool east, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  backdrop(d, east ? BgEast : BgWest, ms);
  header(d, time, ms, false, east ? "EAST" : "WEST");
  const int first = page * kStandingsPerPage;
  for (int row = 0; row < kStandingsPerPage && first + row < table.count; row++) {
    const int rank = first + row;
    const nba::Standing &s = table.rows[rank];
    const int y = 14 + row * 6;
    const bool fav = isFavourite(favourites, s.abbr);
    const uint16_t colour = rank < 6 ? kText : rank < 10 ? kMid : kDim;  // the direct playoff places, the play-in, the rest
    if (fav) box(d, 0, y - 1, W, 7, dim(kFav, 50));
    char number[16];
    snprintf(number, sizeof(number), "%d", rank + 1);
    rightAligned(d, number, 7, y, fav ? kFav : kDim);
    miniText(d, s.abbr, 10, y, fav ? kFav : colour);
    snprintf(number, sizeof(number), "%d-%d", s.wins, s.losses);
    rightAligned(d, number, 47, y, colour);
    if (s.streak[0]) miniText(d, s.streak, 51, y, s.streak[0] == 'W' ? kLive : kRed);
    if (rank == 5 || rank == 9) box(d, 0, y + 5, W, 1, dim(kOrange, 80));
  }
  pageDots(d, page, standingsPages(table), 62);
}

void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const char *line) {
  backdrop(d, BgClock, ms);
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), time->is24hFormat() ? "%02d" : "%d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  auto number = [&](const char *text, int y) {
    const int n = strlen(text), width = n * 12 - 1;
    for (int pass = 0; pass < 2; pass++)  // an orange shadow under the white digits
      for (int i = 0; i < n; i++) blockDigit(d, text[i] - '0', 9 + i * 12 + (pass ? 0 : 1) + (n == 1 ? 6 : 0), y + (pass ? 0 : 1), 11, 21, pass ? kText : rgb(150, 55, 8));
    (void)width;
  };
  number(hours, 3);
  number(minutes, 27);
  // The ball: dribbles in from the left, shoots from mid court, drops through the net and rolls back
  constexpr uint32_t kRound = 14000, kShootAt = 8500, kShootMs = 1200, kDropMs = 1000;
  const uint32_t t = ms % kRound;
  float x, y, spin = ms * 0.004f;
  const float bounce = fabsf(sinf(ms * 3.14159f / 420.0f));
  float sway = 0;
  if (t < kShootAt) {
    x = 6 + 30 * (t / (float)kShootAt);
    y = 54 - bounce * 8;
  } else if (t < kShootAt + kShootMs) {
    const float u = (t - kShootAt) / (float)kShootMs;
    x = 36 + 18 * u;
    y = 51 + (42 - 51) * u - 4.0f * 18 * u * (1 - u);
    spin = u * 14;
  } else if (t < kShootAt + kShootMs + kDropMs) {
    const float u = (t - kShootAt - kShootMs) / (float)kDropMs;
    x = 54 + sinf(u * 9) * 0.6f;
    y = 42 + u * u * 12;
    sway = (1 - u) * 1.2f;
  } else {
    const float u = (t - kShootAt - kShootMs - kDropMs) / (float)(kRound - kShootAt - kShootMs - kDropMs);
    x = 54 - 48 * u;
    y = 54 - fabsf(sinf(u * 9.4f)) * (1 - u) * 10;
    sway = t < kShootAt + kShootMs + kDropMs + 600 ? 0.8f : 0;
  }
  (void)sway;
  box(d, (int)x - 2, 56, 5, 1, rgb(10, 4, 1));  // its shadow on the floor
  ball(d, (int)lroundf(x), (int)lroundf(y), 3, spin);
  if (line && line[0]) centered(d, line, 59, kMid);
}

// ---- the basket ----

void drawBasket(MatrixPanel_I2S_DMA &d, const nba::Game &g, bool home, int points, uint32_t ms) {
  const float t = ms >= kBasketMs ? 1.0f : ms / (float)kBasketMs;
  const uint16_t colour = home ? g.homeColor : g.awayColor;
  const char *abbr = home ? g.home : g.away;
  const int score = home ? g.homeScore : g.awayScore;
  const int otherScore = home ? g.awayScore : g.homeScore;
  beams(d, ms * 3, 0);  // the arena lights, swinging faster for the moment
  // a frame in the team's colour that fades out
  const uint16_t glow = dim(colour, (int)(200 * (1 - t)) + 30);
  box(d, 0, 0, W, 1, glow); box(d, 0, 63, W, 1, glow); box(d, 0, 0, 1, W, glow); box(d, 63, 0, 1, W, glow);
  // backboard, rim, net
  box(d, 57, 8, 2, 20, kMid);
  box(d, 40, 25, 17, 2, kBall);
  const float sway = t > 0.50f && t < 0.9f ? sinf((t - 0.5f) * 40.0f) * (1 - (t - 0.5f) / 0.4f) * 1.3f : 0;
  for (int k = 0; k < 9; k++) {
    const int left = 41 + k / 3 + (int)lroundf(sway * k / 8.0f), right = 55 - k / 3 + (int)lroundf(sway * k / 8.0f);
    for (int x = left; x <= right; x++) if ((x + k) % 2 == 0) put(d, x, 27 + k, rgb(190, 190, 200));
  }
  // the ball: a long arc into the rim (higher for a three), straight down through the net, gone
  const float flight = 0.5f;
  const int apex = points >= 3 ? 28 : 18;
  if (t < flight) {
    const float u = t / flight;
    ball(d, (int)lroundf(4 + 44 * u), (int)lroundf(38 + (24 - 38) * u - 4.0f * apex * u * (1 - u)), 4, u * 12.0f);
  } else if (t < 0.72f) {
    const float u = (t - flight) / (0.72f - flight);
    ball(d, 48, 25 + (int)lroundf(u * 13), 4, 0);
  }
  // sparkles around the rim as it goes in
  if (t > flight - 0.04f && t < 0.85f) {
    const float u = (t - (flight - 0.04f)) / 0.35f;
    for (int i = 0; i < 10; i++) {
      const float a = i * 0.6283f, r = 4 + u * 12;
      const int x = 48 + (int)lroundf(cosf(a) * r), y = 26 + (int)lroundf(sinf(a) * r * 0.8f);
      if (u < 1) put(d, x, y, dim(i % 2 ? kFav : colour, 255 - (int)(u * 200)));
    }
  }
  // +2 / +3
  if (t > 0.45f) {
    const int grow = t > 0.62f ? 0 : 1;  // the first moments a pixel larger
    const int cx = 8, cy = 18;
    box(d, cx - 4 - grow, cy - 1, 9 + 2 * grow, 3, colour);
    box(d, cx - 1, cy - 4 - grow, 3, 9 + 2 * grow, colour);
    blockNumber(d, points, 16, 10, 10, 16, (int)(ms / 120) % 2 || t > 0.7f ? colour : kText);
  }
  // the team and its new score, the other side small
  const int baseY = 44;
  text2x(d, abbr, 3, baseY, colour);
  const int width = blockNumber(d, score, 0, 0, 8, 14, 0, true);
  blockNumber(d, score, W - 3 - width, baseY - 2, 8, 14, t > 0.6f ? kText : dim(kText, 120));
  char other[16];
  snprintf(other, sizeof(other), "%s %d", home ? g.away : g.home, otherScore);
  miniText(d, other, 4, 58, kDim);
}

}  // namespace nbaview
