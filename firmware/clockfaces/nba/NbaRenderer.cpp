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
               kRed = rgb(230, 50, 50), kPastel = rgb(150, 225, 165);

// ---- small helpers ----

int textWidth(const char *s) { return miniWidth(s); }
void centered(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t colour) { miniText(d, s, (W - textWidth(s)) / 2, y, colour); }
void rightAligned(MatrixPanel_I2S_DMA &d, const char *s, int xRight, int y, uint16_t colour) { miniText(d, s, xRight - textWidth(s) + 1, y, colour); }

// 3x5 letters at twice the size
void text2x(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) {
  for (; *s; s++, x += 7) {
    const uint8_t *g = miniGlyph(*s == 'Q' ? 'O' : *s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g[row] >> (2 - col) & 1) box(d, x + col * 2, y + row * 2, 2, 2, colour);
    if (*s == 'Q') box(d, x + 5, y + 8, 2, 3, colour);  // the 3x5 Q looks like a 4: draw an O with a tail sticking out of the bottom right corner
  }
}
// Text in thin capitals, 5 wide and 9 tall (a 5x7 letter with two rows repeated), one pixel strokes, pitch 6
void textThin(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) {
  static const char *const letters[26][7] = {
    {".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"},
    {"XXXX.", "X...X", "X...X", "XXXX.", "X...X", "X...X", "XXXX."},
    {".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."},
    {"XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX."},
    {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX"},
    {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "X...."},
    {".XXX.", "X...X", "X....", "X.XXX", "X...X", "X...X", ".XXXX"},
    {"X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"},
    {".XXX.", "..X..", "..X..", "..X..", "..X..", "..X..", ".XXX."},
    {"..XXX", "...X.", "...X.", "...X.", "...X.", "X..X.", ".XX.."},
    {"X...X", "X..X.", "X.X..", "XX...", "X.X..", "X..X.", "X...X"},
    {"X....", "X....", "X....", "X....", "X....", "X....", "XXXXX"},
    {"X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X"},
    {"X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X", "X...X"},
    {".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."},
    {"XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...."},
    {".XXX.", "X...X", "X...X", "X...X", "X.X.X", "X..X.", ".XX.X"},
    {"XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"},
    {".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX."},
    {"XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."},
    {"X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."},
    {"X...X", "X...X", "X...X", "X...X", "X...X", ".X.X.", "..X.."},
    {"X...X", "X...X", "X...X", "X.X.X", "X.X.X", "XX.XX", "X...X"},
    {"X...X", "X...X", ".X.X.", "..X..", ".X.X.", "X...X", "X...X"},
    {"X...X", "X...X", ".X.X.", "..X..", "..X..", "..X..", "..X.."},
    {"XXXXX", "....X", "...X.", "..X..", ".X...", "X....", "XXXXX"}};
  static const char *const digits[10][7] = {
    {".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX."},
    {"..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX."},
    {".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX"},
    {"XXXX.", "....X", "....X", ".XXX.", "....X", "....X", "XXXX."},
    {"...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."},
    {"XXXXX", "X....", "XXXX.", "....X", "....X", "X...X", ".XXX."},
    {".XXX.", "X....", "X....", "XXXX.", "X...X", "X...X", ".XXX."},
    {"XXXXX", "....X", "...X.", "..X..", ".X...", ".X...", ".X..."},
    {".XXX.", "X...X", "X...X", ".XXX.", "X...X", "X...X", ".XXX."},
    {".XXX.", "X...X", "X...X", ".XXXX", "....X", "....X", ".XXX."}};
  for (; *s; s++, x += 6) {
    const bool digit = *s >= '0' && *s <= '9';
    if (!digit && (*s < 'A' || *s > 'Z')) continue;
    static const uint8_t source[9] = {0, 1, 1, 2, 3, 4, 5, 5, 6};  // which row of the 5x7 letter each of the nine rows shows
    const char *const *rows = digit ? digits[*s - '0'] : letters[*s - 'A'];
    for (int row = 0; row < 9; row++)
      for (int col = 0; col < 5; col++)
        if (rows[source[row]][col] == 'X') put(d, x + col, y + row, colour);
  }
}
int widthThin(const char *s) { return strlen(s) ? (int)strlen(s) * 6 - 1 : 0; }
// Small text where a Q is four wide with a diagonal tail (the 3x5 Q looks like a 9 or an O): for the period labels
int qWidth(const char *s) {
  int w = 0;
  for (; *s; s++) w += *s == 'Q' ? 5 : 4;
  return w ? w - 1 : 0;
}
void qText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) {
  static const char *const q[5] = {".XX.", "X..X", "X..X", "X.X.", ".X.X"};
  for (; *s; s++) {
    if (*s == 'Q') {
      for (int row = 0; row < 5; row++)
        for (int col = 0; col < 4; col++)
          if (q[row][col] == 'X') put(d, x + col, y + row, colour);
      x += 5;
    } else {
      const char one[2] = {*s, 0};
      miniText(d, one, x, y, colour);
      x += 4;
    }
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

// The same ball at a position between pixels: every pixel gets the share of it the disc covers (4x4 samples) and the seams are
// soft lines, so a slowly moving ball glides instead of jumping a whole pixel
uint16_t mix565(uint16_t a, uint16_t b, int k) {  // k 0..16 of b
  const int r = ((a >> 11 << 3) * (16 - k) + (b >> 11 << 3) * k) / 16, g = (((a >> 5) & 63) * 4 * (16 - k) + ((b >> 5) & 63) * 4 * k) / 16,
            bl = ((a & 31) * 8 * (16 - k) + (b & 31) * 8 * k) / 16;
  return rgb(r, g, bl);
}
void ballSmooth(MatrixPanel_I2S_DMA &d, float x, float y, float spin) {
  const float r = 3.2f, cx = x + 0.5f, cy = y + 0.5f, seam = cx + sinf(spin) * 3 * 0.55f;
  const int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
  for (int py = y0; py <= y0 + 8; py++)
    for (int px = x0; px <= x0 + 8; px++) {
      int covered = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const float dx = px + (sx + 0.5f) / 4 - cx, dy = py + (sy + 0.5f) / 4 - cy;
          if (dx * dx + dy * dy <= r * r) covered++;
        }
      if (!covered) continue;
      const float lineX = fmaxf(0.0f, 1 - fabsf(px + 0.5f - seam)), lineY = fmaxf(0.0f, 1 - fabsf(py + 0.5f - cy));
      const uint16_t face = mix565(kBall, kSeam, (int)(16 * fmaxf(lineX, lineY) * 0.9f));
      put(d, px, py, covered == 16 ? face : dim(face, covered * 255 / 16));
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
  textThin(d, label, 2, 2, kOrange);  // the thin 5x7 capitals, one pixel clear of the favourite bar on the left edge
  // the label against the left edge and the time against the right (1 px margin), in the alternative number font at 6x10 (the colon two
  // dots, with two empty columns on each side)
  int width = 0;
  for (const char *c = text; *c; c++) width += *c == ':' ? 5 : 7;
  int x = W - width;  // the last digit's trailing gap is the margin
  if (live) {  // the red dot in the middle of the space between the label and the time
    const int labelEnd = 2 + widthThin(label);
    box(d, labelEnd + (x - labelEnd - 3) / 2, 4, 3, 3, pulse(kRed, ms, 2000, 60));
  }
  for (const char *c = text; *c; c++) {
    if (*c == ':') {
      box(d, x + 1, 3, 2, 2, kText);
      box(d, x + 1, 7, 2, 2, kText);
      x += 5;
    } else {
      blockDigit(d, *c - '0', x, 1, 6, 10, kText);
      x += 7;
    }
  }
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
// The rim and the net of the clock page's hoop. Drawn again over the ball when it drops in, so the ball is inside the net, not in front of it
constexpr int kHoopRise = 6;  // how many pixels above its first position the clock page's hoop hangs
void hoopFront(MatrixPanel_I2S_DMA &d) {
  box(d, 49, 41 - kHoopRise, 12, 2, kBall);
  for (int k = 0; k < 7; k++) {
    const int left = 50 + k / 3, right = 59 - k / 3;
    for (int x = left; x <= right; x++) if ((x + k) % 2 == 0) put(d, x, 43 - kHoopRise + k, rgb(120, 120, 130));
  }
}

// The next-up page's backdrop, from the football face: a crowd of multi-coloured dots (every second pixel of every
// second row, thinner and dimmer towards the top, an aisle every 15 columns), ad boards and a strip of wood
void stands(MatrixPanel_I2S_DMA &d, int boost = 0) {  // boost: extra brightness for the crowd, for the flashing of a basket
  static const uint16_t crowd[5] = {rgb(120, 30, 30), rgb(30, 60, 130), rgb(130, 120, 40), rgb(110, 110, 118), rgb(30, 100, 60)};
  static const uint16_t ads[4] = {rgb(60, 72, 120), rgb(120, 56, 62), rgb(84, 88, 100), rgb(50, 110, 104)};
  const int bandY = 55;  // the boards end at row 58, right against the floor
  for (int y = 2; y < bandY; y += 2) {
    const int k = 100 * (bandY - y) / (bandY - 2);  // 0 at the boards .. 100 at the top
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (x >= 14 && (x + 1) % 15 < 2) continue;
      uint32_t h = (x * 73856093u) ^ (y * 19349663u);
      h = (h ^ (h >> 13)) * 1274126177u;
      if ((h >> 8) % 100 >= (unsigned)(78 - 36 * k / 100)) continue;
      put(d, x, y, dim(crowd[(h >> 16) % 5], 62 - 32 * k / 100 + boost));
    }
  }
  box(d, 0, bandY, W, 1, rgb(5, 6, 8));
  for (int x = 0; x < W; x++) box(d, x, bandY + 1, 1, 2, dim(ads[x / 15 % 4], x % 15 == 14 ? 8 : 40));
  box(d, 0, bandY + 3, W, 1, rgb(8, 22, 10));
  box(d, 0, 59, W, 1, rgb(46, 22, 7));
  box(d, 0, 60, W, 4, rgb(24, 10, 3));
}

void paint(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) {
  switch (background) {
    case BgGame: stands(d); return;  // the same stands as the other pages, no band behind the time
    case BgQuarters: centreCourt(d); break;
    case BgToday: parquet(d); break;
    case BgNext: stands(d); return;  // no band behind the time here
    case BgEast: netting(d, rgb(10, 18, 44)); break;
    case BgWest: netting(d, rgb(40, 11, 11)); break;
    case BgClock: {  // the arena: beams, the floor along the bottom, a hoop on the right
      stands(d);
      box(d, 60, 29 - kHoopRise, 2, 16, kMid);
      hoopFront(d);
      return;
    }
  }
  box(d, 0, 0, W, 12, rgb(30, 12, 3));  // the band behind the time
}
}  // namespace

void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { paint(d, background, ms); }
void backgroundDrawnByFace(bool yes) { backgroundByFace = yes; }

// ---- favourites and order ----

// A team's colour for the bar before its name, by ESPN's abbreviation (picked to be readable on black)
uint16_t teamColour(const char *abbr) {
  struct Team { const char *abbr; uint8_t r, g, b; };
  static const Team teams[] = {
    {"ATL", 225, 68, 52}, {"BOS", 30, 170, 80}, {"BKN", 200, 200, 200}, {"CHA", 0, 160, 180}, {"CHI", 206, 17, 65}, {"CLE", 190, 40, 90},
    {"DAL", 40, 120, 220}, {"DEN", 255, 200, 40}, {"DET", 220, 40, 60}, {"GS", 255, 200, 40}, {"HOU", 206, 17, 65}, {"IND", 253, 187, 48},
    {"LAC", 220, 40, 60}, {"LAL", 150, 90, 220}, {"MEM", 110, 140, 200}, {"MIA", 230, 50, 70}, {"MIL", 60, 160, 90}, {"MIN", 50, 130, 190},
    {"NO", 200, 170, 100}, {"NY", 40, 120, 230}, {"OKC", 40, 150, 235}, {"ORL", 30, 140, 220}, {"PHI", 40, 110, 230}, {"PHX", 240, 110, 40},
    {"POR", 224, 58, 62}, {"SAC", 140, 80, 200}, {"SA", 190, 195, 200}, {"TOR", 206, 17, 65}, {"UTAH", 150, 100, 230}, {"WSH", 230, 40, 60}};
  for (const Team &t : teams)
    if (!strcmp(t.abbr, abbr)) return rgb(t.r, t.g, t.b);
  return rgb(120, 120, 120);
}

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

// ---- the next-up page, laid out like the football face: the clock top left, a league table under it, the coming games in a column of a third on the right ----

namespace {
constexpr int kDividerX = 42, kColumnX = 43;  // the column of coming games is a third of the panel
constexpr int kFixtureCap = 24, kGameH = 19, kColumnGames = 3, kColumnTitleY = 1, kColumnGamesTop = 8;  // a game (time, away, home) takes 19 px; the day is the title, a line under it, then the games
constexpr int kRankRows = 6, kRankStep = 6, kRankTop = 21, kSlideMs = 500;  // a table page: the title at kRankTop, a line, then six rows that slide up
}  // namespace

int fixturePages(const nba::Snapshot &snap, ClockTime *) { return snap.nextCount ? 1 : 0; }  // one page that moves on by itself

int fixtureCount(const nba::Snapshot &snap) { return std::min<int>(snap.nextCount, kFixtureCap); }
int dayOf(ClockTime *time, const nba::Fixture &f) { return (int)(time->utcToLocal((time_t)f.start) / 86400); }

// The coming games as pages of the right column, like the table on the left: the day is the title and a page holds games of one day only, three when there are
// that many; the last page of a day is filled up with the games before it
struct ColumnPage { uint8_t first, count; int day; };
constexpr int kMaxColumnPages = 12;

int layoutColumn(const nba::Snapshot &snap, ClockTime *time, ColumnPage *pages) {
  const int n = fixtureCount(snap);
  int count = 0, i = 0;
  while (i < n && count < kMaxColumnPages) {
    const int day = dayOf(time, snap.next[i]);
    int j = i;
    while (j < n && dayOf(time, snap.next[j]) == day) j++;
    const int games = j - i, perDay = (games + kColumnGames - 1) / kColumnGames;
    for (int p = 0; p < perDay && count < kMaxColumnPages; p++) pages[count++] = {(uint8_t)(i + std::min(p * kColumnGames, std::max(0, games - kColumnGames))), (uint8_t)std::min(games, kColumnGames), day};
    i = j;
  }
  return count;
}

int rankPagesTotal(const nba::Snapshot &snap) {
  const int east = snap.east.count ? (snap.east.count + kRankRows - 1) / kRankRows : 0, west = snap.west.count ? (snap.west.count + kRankRows - 1) / kRankRows : 0;
  return east + west;
}
uint32_t columnMs(uint32_t groupMs) { return std::min<uint32_t>(groupMs, 5000); }  // the coming games move on faster than the table: every 5 seconds

// The page lasts until both lists have been through once (the longer of the two), at most 65 seconds
uint32_t pageLength(const nba::Snapshot &snap, ClockTime *time, uint32_t groupMs) {
  ColumnPage pages[kMaxColumnPages];
  const uint32_t column = layoutColumn(snap, time, pages) * columnMs(groupMs);
  return std::min<uint32_t>(65000, std::max<uint32_t>(std::max<uint32_t>(column, rankPagesTotal(snap) * groupMs), groupMs));
}

uint16_t upcomingMs(const nba::Snapshot &snap, ClockTime *time, uint16_t pageMs) { return (uint16_t)pageLength(snap, time, pageMs); }

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
    textThin(d, isHome ? g.home : g.away, 5, y + 1, g.state == nba::Pre || leads || flash ? kText : kMid);
    if (g.state == nba::Pre) {
      miniText(d, isHome ? g.homeRecord : g.awayRecord, 5, y + 11, kDim);
    } else {
      const int width = blockNumber(d, score, 0, 0, 8, 14, 0, true);
      blockNumber(d, score, W - 2 - width, y + 2, 8, 14, leads || flash || g.state == nba::Live ? (flash ? kText : colour) : kDim);
      // the points of each period under the name: the latest ones that fit left of the score, so they never touch it
      int periods = g.period < 1 ? 1 : g.period > nba::kPeriods ? nba::kPeriods : g.period;
      if (g.state == nba::Final && periods < 4) periods = 4;
      const int room = W - 2 - width - 3 - 5 + 1;
      int first = periods, used = 0;
      while (first > 0) {
        char one[6];
        snprintf(one, sizeof(one), "%d", isHome ? g.homeQ[first - 1] : g.awayQ[first - 1]);
        const int need = textWidth(one) + (used ? 3 : 0);
        if (used + need > room) break;
        used += need;
        first--;
      }
      for (int p = first, px = 5; p < periods; p++) {
        char one[6];
        snprintf(one, sizeof(one), "%d", isHome ? g.homeQ[p] : g.awayQ[p]);
        miniText(d, one, px, y + 11, g.state == nba::Live && p == periods - 1 ? kMid : kDim);
        px += textWidth(one) + 3;
      }
    }
  }
  char text[16];
  uint16_t colour;
  status(g, time, text, sizeof(text), colour, false);
  qText(d, text, (W - qWidth(text)) / 2, 54, colour);
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

// Behind the quarters table: a faint scoreboard grid (a dot every third pixel) that glows in the away team's colour at the top left
// and the home team's at the top right, fading downwards
void scoreboard(MatrixPanel_I2S_DMA &d, uint16_t away, uint16_t home) {
  auto channel = [](uint16_t c, int i) { return i == 0 ? c >> 11 << 3 : i == 1 ? ((c >> 5) & 63) * 4 : (c & 31) * 8; };
  for (int y = 13; y < H; y++) {
    if (y % 3 != 1) continue;
    for (int x = 1; x < W; x += 3) {
      const float a = fmaxf(0.0f, 1 - hypotf(x - 8.0f, y - 17.0f) / 22) * 0.35f, h = fmaxf(0.0f, 1 - hypotf(x - 56.0f, y - 17.0f) / 22) * 0.35f;
      int c[3];
      for (int i = 0; i < 3; i++) c[i] = std::min(255, (i == 2 ? 20 : 16) + (int)(channel(away, i) * a + channel(home, i) * h));
      put(d, x, y, rgb(c[0], c[1], c[2]));
    }
  }
}

void drawQuarters(MatrixPanel_I2S_DMA &d, const nba::Game &g, ClockTime *time, uint32_t ms) {
  header(d, time, ms, g.state == nba::Live, "QTR");
  int periods = g.period;
  if (g.state == nba::Final && periods < 4) periods = 4;
  if (periods < 1) periods = 1;
  if (periods > nba::kPeriods) periods = nba::kPeriods;
  const int shown = periods > 5 ? 5 : periods, first = periods - shown;  // the last five periods
  const int awayX = 31, homeX = 59;  // right edges of the two columns
  if (!backgroundOff()) scoreboard(d, g.awayColor, g.homeColor);  // under the text
  // with up to four periods the team names are in the thin 5x9 capitals and the rows start under them; with overtime there is no room: small names
  const bool roomy = shown <= 4;
  const int top = roomy ? 24 : 22;
  if (roomy) {
    box(d, 0, 14, 2, 9, g.awayColor);
    textThin(d, g.away, 4, 14, g.awayColor);
    box(d, 33, 14, 2, 9, g.homeColor);
    textThin(d, g.home, 37, 14, g.homeColor);
  } else {
    box(d, 0, 15, 2, 5, g.awayColor);
    miniText(d, g.away, 4, 15, g.awayColor);
    box(d, 33, 15, 2, 5, g.homeColor);
    miniText(d, g.home, 37, 15, g.homeColor);
  }
  for (int i = 0; i < shown; i++) {
    const int period = first + i + 1, y = top + i * 6;
    char label[4];
    if (period <= 4) snprintf(label, sizeof(label), "Q%d", period);
    else snprintf(label, sizeof(label), period == 5 ? "OT" : "%dO", period - 4);
    const int a = g.awayQ[first + i], h = g.homeQ[first + i];
    const bool now = g.state == nba::Live && period == periods;
    qText(d, label, 1, y, now ? kLive : kDim);
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
  const int line = top + shown * 6 + 1;
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
  if (const char *space = strchr(text, ' ')) {  // the period on the left, the clock (or OT) on the right
    char period[8];
    strlcpy(period, text, std::min<size_t>(sizeof(period), space - text + 1));
    qText(d, period, 2, 58, colour);
    rightAligned(d, space + 1, 62, 58, colour);
  } else {
    centered(d, text, 58, colour);
  }
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
    qText(d, text, 2, y + 7, colour);
  }
  pageDots(d, page, gamePages(count), 62);
}

// The clock top left as four digits of 8x14 with an orange shadow, like the football face's
void rankClock(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), time->is24hFormat() ? "%02d" : "%d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  const char digits[4] = {hours[1] ? hours[0] : ' ', hours[1] ? hours[1] : hours[0], minutes[0], minutes[1]};
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < 4; i++) {
      if (digits[i] == ' ') continue;
      blockDigit(d, digits[i] - '0', 1 + i * 9 + (i >= 2 ? 4 : 0) + (pass ? 0 : 1), 1 + (pass ? 0 : 1), 8, 14, pass ? kText : rgb(150, 55, 8));
    }
}

// Text and bars cut off above `minY` and from column `maxX` on: the table rows slide under their title and stay clear of the clock
void clipText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour, int minY, int maxX, int minX = 0) {
  for (; *s; s++, x += 4) {
    const uint8_t *g = miniGlyph(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if ((g[row] >> (2 - col) & 1) && y + row >= minY && y + row < H && x + col >= minX && x + col < maxX) put(d, x + col, y + row, colour);
  }
}

// The first team of a table page: the last page is filled up with the teams before it, so it always has six rows
int rankFirst(const nba::Snapshot &snap, int index) {
  const int eastPages = snap.east.count ? (snap.east.count + kRankRows - 1) / kRankRows : 0;
  const bool east = index < eastPages;
  const int count = east ? snap.east.count : snap.west.count;
  return std::min((east ? index : index - eastPages) * kRankRows, std::max(0, count - kRankRows));
}

// The six rows of one page of a conference table, the first at y0; what is above `minY` is not drawn
void rankRows(MatrixPanel_I2S_DMA &d, const nba::Snapshot &snap, int index, int y0, const char *favourites, int minY) {
  const int eastPages = snap.east.count ? (snap.east.count + kRankRows - 1) / kRankRows : 0;
  const bool east = index < eastPages;
  const nba::Table &table = east ? snap.east : snap.west;
  const int first = rankFirst(snap, index);
  for (int r = 0; r < kRankRows && first + r < table.count; r++) {
    const int y = y0 + r * kRankStep;
    if (y + 5 <= minY || y >= H) continue;
    const nba::Standing &row = table.rows[first + r];
    char text[12];
    snprintf(text, sizeof(text), "%d", first + r + 1);
    clipText(d, text, (7 - textWidth(text)) / 2, y, kOrange, minY, kDividerX);  // no margin on the left (the title keeps its one pixel)
    const int shift = first + r + 1 >= 10 ? 1 : 0;  // from number 10 the rank is two digits wide: the bar and the name start a pixel further right
    for (int k = 0; k < 5; k++) if (y + k >= minY && y + k < H) put(d, 7 + shift, y + k, teamColour(row.abbr));  // the team's colour, one pixel wide
    // wins and losses with a single dot between them (a dash would be two pixels wider): "60" "22" is 17 px, ending at the divider
    char wins[6], losses[6];
    snprintf(wins, sizeof(wins), "%d", row.wins);
    snprintf(losses, sizeof(losses), "%d", row.losses);
    const int width = textWidth(wins) + 3 + textWidth(losses), left = kDividerX - 1 - width;
    // the name: UTAH is shown as UTA only when it would run into the record
    char name[5];
    strlcpy(name, row.abbr, sizeof(name));
    if (10 + shift + textWidth(name) >= left - 1 && strlen(name) > 3) name[3] = 0;
    clipText(d, name, 10 + shift, y, isFavourite(favourites, row.abbr) ? kFav : kText, minY, kDividerX);
    clipText(d, wins, left, y, kMid, minY, kDividerX);
    if (y + 2 >= minY && y + 2 < H) put(d, left + textWidth(wins) + 1, y + 2, kMid);
    clipText(d, losses, left + textWidth(wins) + 3, y, kMid, minY, kDividerX);
  }
}

bool rankEast(const nba::Snapshot &snap, int index) { return index < (snap.east.count ? (snap.east.count + kRankRows - 1) / kRankRows : 0); }

// One coming game at y: the time, then the away team, then the home team; what is above `minY` is not drawn (the games slide up under the day)
void columnGame(MatrixPanel_I2S_DMA &d, const nba::Fixture &f, ClockTime *time, int y, const char *favourites, int minY) {
  if (y + 16 < minY || y >= H) return;
  char text[8];
  tipOff(time, f.start, text, sizeof(text));
  // the tip-off time with the colon one pixel from the digits on both sides (the 3x5 colon has two empty columns each side)
  int total = 0;
  for (const char *c = text; *c; c++) total += *c == ':' ? 2 : 4;
  int x = kColumnX + (21 - (total - (text[strlen(text) - 1] == ':' ? 0 : 1))) / 2 - 1;
  for (const char *c = text; *c; c++) {
    if (*c == ':') {
      for (int r = 1; r <= 3; r += 2) if (y + r >= minY && y + r < H) put(d, x, y + r, kMid);
      x += 2;
    } else {
      const char one[2] = {*c, 0};
      clipText(d, one, x, y, kMid, minY, W);
      x += 4;
    }
  }
  for (int r = 0; r < 5; r++)
    if (y + 6 + r >= minY && y + 6 + r < H) put(d, kColumnX + 1, y + 6 + r, teamColour(f.away));  // the teams' colours, a pixel wide like in the table
  for (int r = 0; r < 5; r++)
    if (y + 12 + r >= minY && y + 12 + r < H) put(d, kColumnX + 1, y + 12 + r, teamColour(f.home));
  clipText(d, f.away, kColumnX + 4, y + 6, isFavourite(favourites, f.away) ? kFav : kText, minY, W);
  clipText(d, f.home, kColumnX + 4, y + 12, isFavourite(favourites, f.home) ? kFav : kText, minY, W);
}

const char *dayName(ClockTime *time, const nba::Fixture &f) {
  static const char *const names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  struct tm local;
  time_t when = time->utcToLocal((time_t)f.start);
  gmtime_r(&when, &local);
  return names[local.tm_wday];
}

void drawUpcoming(MatrixPanel_I2S_DMA &d, const nba::Snapshot &snap, const char *favourites, int page, ClockTime *time, uint32_t ms, uint32_t elapsed, uint32_t groupMs) {
  (void)page;
  backdrop(d, BgNext, ms);
  rankClock(d, time);
  box(d, kDividerX, 0, 1, H, rgb(36, 36, 40));
  // The coming games move on by two every 5 seconds, the table by a page every groupMs; both slide up and the first come round again after the last
  const uint32_t columnStep = columnMs(groupMs);
  const int kc = elapsed / columnStep, kt = elapsed / groupMs;
  const uint32_t inColumn = elapsed - kc * columnStep, inTable = elapsed - kt * groupMs;
  const float uc = inColumn < (uint32_t)kSlideMs ? inColumn / (float)kSlideMs : 1.0f, easeC = uc * uc * (3 - 2 * uc);
  const float ut = inTable < (uint32_t)kSlideMs ? inTable / (float)kSlideMs : 1.0f, easeT = ut * ut * (3 - 2 * ut);
  // the coming games, like the table: the day (title) stays where it is while the games slide up under it; the title slides sideways when the day changes
  ColumnPage column[kMaxColumnPages];
  const int columnPages = layoutColumn(snap, time, column);
  if (!columnPages) {
    miniText(d, "NO", kColumnX + 2, 26, kDim);
    miniText(d, "GAMES", kColumnX + 2, 33, kDim);
  } else {
    const int now = kc % columnPages, was = (kc + columnPages - 1) % columnPages;
    const ColumnPage &cur = column[now], &old = column[was];
    const bool slidingGames = inColumn < (uint32_t)kSlideMs && columnPages > 1;
    const bool dayChanges = slidingGames && cur.day != old.day;
    const int added = !dayChanges && cur.first > old.first ? std::min<int>(kColumnGames, cur.first - old.first) : kColumnGames;  // as many as the new page adds
    const int distance = added * kGameH;
    if (dayChanges && inColumn < 200) {
      const float p = inColumn / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf(21 * e);
      clipText(d, dayName(time, snap.next[old.first]), kColumnX + 1 - offset, kColumnTitleY, kPastel, 0, W, kColumnX + 1);
      clipText(d, dayName(time, snap.next[cur.first]), kColumnX + 1 + 21 - offset, kColumnTitleY, kPastel, 0, W, kColumnX + 1);
    } else {
      clipText(d, dayName(time, snap.next[cur.first]), kColumnX + 1, kColumnTitleY, kPastel, 0, W, kColumnX + 1);
    }
    if (slidingGames) {
      const int shift = (int)lroundf(distance * easeC);
      for (int g = 0; g < old.count; g++) columnGame(d, snap.next[old.first + g], time, kColumnGamesTop + g * kGameH - shift, favourites, kColumnGamesTop);
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, kColumnGamesTop + distance - shift + g * kGameH, favourites, kColumnGamesTop);
    } else {
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, kColumnGamesTop + g * kGameH, favourites, kColumnGamesTop);
    }
  }
  // the league table: its title (EAST, WEST) stays where it is while the rows slide up under it; the title only slides sideways when it changes
  const int pages = rankPagesTotal(snap);
  if (pages) {
    const int now = kt % pages, was = (kt + pages - 1) % pages;
    const int rowsTop = kRankTop + 7;
    const bool slidingRows = inTable < (uint32_t)kSlideMs && pages > 1;
    const bool changes = slidingRows && rankEast(snap, now) != rankEast(snap, was);
    // the rows move up by as many as the new page adds, no further (a whole page when the other conference comes)
    const int added = !changes && rankFirst(snap, now) > rankFirst(snap, was) ? std::min(kRankRows, rankFirst(snap, now) - rankFirst(snap, was)) : kRankRows;
    const int distance = added * kRankStep;
    if (changes && inTable < 200) {
      const float p = inTable / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf((kDividerX - 2) * e);
      clipText(d, rankEast(snap, was) ? "EAST" : "WEST", 1 - offset, kRankTop - 1, kPastel, 0, kDividerX);
      clipText(d, rankEast(snap, now) ? "EAST" : "WEST", 1 + (kDividerX - 2) - offset, kRankTop - 1, kPastel, 0, kDividerX);
    } else {
      clipText(d, rankEast(snap, now) ? "EAST" : "WEST", 1, kRankTop - 1, kPastel, 0, kDividerX);
    }
    box(d, 0, kRankTop + 5, kDividerX, 1, rgb(24, 20, 24));  // a line between the title and the rows
    if (slidingRows) {
      const int shift = (int)lroundf(distance * easeT);
      rankRows(d, snap, was, rowsTop - shift, favourites, rowsTop);
      rankRows(d, snap, now, rowsTop + distance - shift, favourites, rowsTop);
    } else {
      rankRows(d, snap, now, rowsTop, favourites, rowsTop);
    }
  }
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

void nextGameLine(ClockTime *time, const nba::Fixture &fixture, char *out, size_t size) {
  char when[8];
  tipOff(time, (int64_t)fixture.start, when, sizeof(when));
  snprintf(out, size, "%s-%s %s", fixture.away, fixture.home, when);
}

void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const char *line) {
  backdrop(d, BgClock, ms);
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), "%02d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  auto number = [&](const char *text, int y) {
    const int n = strlen(text), width = n * 12 - 1;
    for (int pass = 0; pass < 2; pass++)  // an orange shadow under the white digits
      for (int i = 0; i < n; i++) {
        const int px = 3 + i * 13 + (pass ? 0 : 1) + (n == 1 ? 6 : 0), py = y + (pass ? 0 : 1);
        const uint16_t colour = pass ? kText : rgb(150, 55, 8);
        blockDigit(d, text[i] - '0', px, py, 11, 21, colour);
        if (text[i] == '1')  // the 1's nose a column longer: three dots to the left of its edge, a pixel lower
          for (int row = 4; row <= 6; row++) put(d, px, py + row, colour);
      }
    (void)width;
  };
  number(hours, 2);  // the top of the digits level with the top of the weekday
  number(minutes, 26);
  {  // the date in the free space over the hoop: the weekday over day and month
    struct tm local;
    time_t now = (time_t)time->localNow();
    gmtime_r(&now, &local);
    static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    // thin 5x9 capitals and digits, both lines against the right edge (1 px margin)
    const char *weekday = days[local.tm_wday % 7], *month = months[local.tm_mon % 12];
    textThin(d, weekday, W - 1 - widthThin(weekday), 2, kOrange);
    char day[4];
    snprintf(day, sizeof(day), "%d", local.tm_mday);
    const int dayWidth = widthThin(day), dateWidth = dayWidth + 3 + widthThin(month), left = W - 1 - dateWidth;
    textThin(d, day, left, 13, kMid);
    textThin(d, month, left + dayWidth + 3, 13, kMid);
  }
  // The ball plays thirteen rounds in turn. Most dribble in from the left (steady, crossing over, slowly, rolling or in small hops), shoot
  // from a different spot and arc each time, and either drop through the net or hit the rim; the bounces get lower and quicker, and it
  // rolls out to the left. The special rounds: a long throw from outside the frame, a lob that leaves the top of the frame, a ball that
  // circles the rim before it drops in, and a bank shot off the backboard.
  enum Kind : uint8_t { Normal, LongThrow, TopLob, RimRoll, Bank };
  struct Play { uint8_t style, kind; float fromX, arc; bool scores; };
  static const Play plays[13] = {{0, Normal, 36, 18, true}, {3, Normal, 28, 20, true}, {1, Normal, 34, 14, false}, {0, LongThrow, 0, 48, true},
                                 {2, Normal, 22, 28, true}, {4, Normal, 32, 16, false}, {0, RimRoll, 32, 20, true}, {0, Normal, 30, 12, false},
                                 {1, TopLob, 36, 80, true}, {1, Normal, 40, 16, true}, {3, Normal, 38, 18, false}, {1, Bank, 38, 16, true},
                                 {0, Normal, 24, 26, true}};  // styles: 0 steady, 1 crossover, 2 slow, 3 rolling along the floor, 4 small hops
  constexpr uint32_t kRound = 14000, kDropMs = 1000, kRimMs = 1600;
  constexpr float kFloor = 55, kOut = -5;  // the ball comes in from and rolls out to this x, just outside the frame
  const Play &play = plays[(ms / kRound) % 13];
  const uint32_t t = ms % kRound;
  const uint32_t kShootAt = play.kind == LongThrow ? 3500 : 8500;  // a long throw starts when nothing has been on the court for a while
  const uint32_t kShootMs = play.kind == LongThrow ? 1900 : play.kind == TopLob ? 2000 : play.kind == Bank ? 1400 : 1200;
  auto bounceHeight = [](float s, float h, float period) {  // height over the floor s ms after the first touch: each bounce lower and quicker
    while (s > period && period > 40) { s -= period; h *= 0.55f; period *= 0.74f; }
    if (s > period) return 0.0f;
    const float u = s / period;
    return h * 4 * u * (1 - u);
  };
  auto easeOut = [](float w) { return 1 - (1 - w) * (1 - w); };
  const float period = play.style == 2 ? 560.0f : play.style == 1 ? 360.0f : play.style == 4 ? 300.0f : 420.0f;
  const float dribble = play.style == 2 ? 10.0f : play.style == 1 ? 7.0f : play.style == 3 ? 0.0f : play.style == 4 ? 3.0f : 8.0f;  // how high it bounces; 0 is rolling
  const bool rim = !play.scores;
  const float endX = play.scores && play.kind != RimRoll ? 54.0f : 49.0f, endY = (play.scores && play.kind != RimRoll ? 42.0f : 40.0f) - kHoopRise;
  const float handsPhase = fmodf(kShootAt / period, 1.0f), handsY = kFloor - dribble * 4 * handsPhase * (1 - handsPhase);
  const float startX = play.kind == LongThrow ? kOut : play.fromX, startY = play.kind == LongThrow ? 36.0f : handsY;
  float x = kOut, y = kFloor, spin = ms * 0.004f;
  bool visible = true;
  if (t < kShootAt) {
    if (play.kind == LongThrow) {
      visible = false;
    } else {
      const float progress = t / (float)kShootAt, phase = fmodf(t / period, 1.0f);
      x = kOut + (play.fromX - kOut) * progress;
      if (play.style == 1) x += 5 * sinf(progress * 18.85f) * sinf(progress * 3.1416f);  // crossing over from side to side
      y = kFloor - dribble * 4 * phase * (1 - phase);
      if (play.style == 3) spin = x * 0.5f;  // rolling: it turns as far as it travels
    }
  } else if (t < kShootAt + kShootMs) {
    const float u = (t - kShootAt) / (float)kShootMs;
    if (play.kind == Bank) {  // up to the backboard, then a short drop into the net
      if (u < 0.65f) {
        const float v = u / 0.65f;
        x = startX + (58 - startX) * v;
        y = startY + (endY - 4 - startY) * v - 4.0f * play.arc * v * (1 - v);
      } else {
        const float v = (u - 0.65f) / 0.35f;
        x = 58 - 4 * v;
        y = endY - 4 + 4 * v * v;
      }
    } else {
      x = startX + (endX - startX) * u;
      y = startY + (endY - startY) * u - 4.0f * play.arc * u * (1 - u);
    }
    spin = u * 14;
  } else if (play.kind == RimRoll) {
    const uint32_t from = kShootAt + kShootMs, rolling = 1700;
    if (t < from + rolling) {  // round the rim, front to back and front again, then in
      const float u = (t - from) / (float)rolling;
      x = 54 - 5 * cosf(2.5f * 3.1416f * u);
      y = endY - 2.0f * fabsf(sinf(2.5f * 3.1416f * u));
      spin = u * 20;
    } else if (t < from + rolling + kDropMs) {
      const float u = (t - from - rolling) / (float)kDropMs;
      x = 54 + sinf(u * 9) * 0.6f;
      y = endY + u * u * (kFloor - endY);
    } else {
      const float s = (float)(t - from - rolling - kDropMs), w = s / (float)(kRound - from - rolling - kDropMs);
      x = 54 - (54 - kOut) * easeOut(w);
      y = kFloor - bounceHeight(s, 8, 480);
      spin = -(54 - x) * 0.5f;
    }
  } else if (play.scores) {
    const uint32_t from = kShootAt + kShootMs;
    if (t < from + kDropMs) {
      const float u = (t - from) / (float)kDropMs;
      x = 54 + sinf(u * 9) * 0.6f;
      y = endY + u * u * (kFloor - endY);
    } else {
      const float s = (float)(t - from - kDropMs), w = s / (float)(kRound - from - kDropMs);
      x = 54 - (54 - kOut) * easeOut(w);
      y = kFloor - bounceHeight(s, 8, 480);
      spin = -(54 - x) * 0.5f;
    }
  } else {
    const uint32_t from = kShootAt + kShootMs;
    if (t < from + kRimMs) {  // off the rim, up and back
      const float u = (t - from) / (float)kRimMs;
      x = 49 - 16 * u;
      y = endY - 14 * u + (kFloor - endY + 14) * u * u;
      spin = 14 - u * 8;
    } else {
      const float s = (float)(t - from - kRimMs), w = s / (float)(kRound - from - kRimMs);
      x = 33 - (33 - kOut) * easeOut(w);
      y = kFloor - bounceHeight(s, 7, 450);
      spin = -(33 - x) * 0.5f;
    }
  }
  (void)rim;
  if (visible) {  // its shadow on the floor: dithered, narrower the higher the ball is, gone when it is high
    const int h = (int)(kFloor - y), half = h < 3 ? 2 : h < 8 ? 1 : h < 14 ? 0 : -1, cx = (int)lroundf(x);
    for (int sx = -half; sx <= half; sx++)
      if (((cx + sx) & 1) == 0) put(d, cx + sx, 59, rgb(10, 4, 1));
  }
  if (visible) ballSmooth(d, x, y, spin);
  if (x >= 46 && x <= 62 && y >= 36 - kHoopRise && y <= 58) hoopFront(d);  // in the net: the rim and the mesh cover the ball
  if (line && line[0]) centered(d, line, 59, kMid);
}

// ---- the basket ----

// A score of 8x14 block digits (right edge xRight) with its rows shifted up by `shift` (rows that leave the box are not drawn), for the
// old score rolling out and the new one rolling in
void scoreRows(MatrixPanel_I2S_DMA &d, int value, int xRight, int y, int shift, uint16_t colour) {
  char text[8];
  snprintf(text, sizeof(text), "%d", value);
  const int n = strlen(text), stroke = (8 + 2) / 4;
  int x = xRight - (n * 9 - 1) + 1;
  for (int i = 0; i < n; i++, x += 9)
    for (int row = 0; row < 14; row++) {
      const int source = row + shift;
      if (source < 0 || source >= 14) continue;
      for (int col = 0; col < 8; col++)
        if (numericfont::pixel(text[i] - '0', col, source, 8, 14) || (text[i] == '1' && col == 0 && source >= 14 - stroke)) put(d, x + col, y + row, colour);
    }
}

void drawBasket(MatrixPanel_I2S_DMA &d, const nba::Game &g, bool home, int points, uint32_t ms) {
  const float t = ms >= kBasketMs ? 1.0f : ms / (float)kBasketMs;
  const float flight = 0.5f, rolled = 0.62f;  // when the ball reaches the rim, and when the new score has rolled in
  const uint16_t colour = home ? g.homeColor : g.awayColor;
  const char *abbr = home ? g.home : g.away;
  const int score = home ? g.homeScore : g.awayScore;
  const int otherScore = home ? g.awayScore : g.homeScore;
  // the stands flash: the crowd lights up on the beat, and a strip of lights along the top alternates in the team's colour and white
  const uint32_t beat = ms / 150;
  stands(d, beat % 2 ? 110 : 20);
  for (int x = 0; x < W; x += 8) box(d, x, 0, 8, 2, (x / 8 + beat) % 2 ? dim(colour, 230) : dim(kText, 200));
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
  textThin(d, abbr, 3, baseY + 2, colour);  // the thin 5x9 capitals, centred on the score
  // the score: the old one until the ball goes in, then it rolls up into the new one
  if (t < flight || points <= 0) {
    scoreRows(d, score - points, W - 3, baseY - 2, 0, dim(kText, 120));
  } else if (t < rolled) {
    const int shift = (int)lroundf((t - flight) / (rolled - flight) * 16);
    scoreRows(d, score - points, W - 3, baseY - 2, shift, dim(kText, 120));
    scoreRows(d, score, W - 3, baseY - 2, shift - 16, colour);
  } else {
    scoreRows(d, score, W - 3, baseY - 2, 0, t > 0.7f ? kText : colour);
  }
  char other[16];
  snprintf(other, sizeof(other), "%s %d", home ? g.away : g.home, otherScore);
  miniText(d, other, 4, 58, kDim);
}

}  // namespace nbaview
