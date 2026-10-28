// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "MlbRenderer.h"

#include <BoldFont.h>
#include <FaceKit.h>
#include <MiniFont.h>
#include <PacmanColors.h>
#include <algorithm>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>

using namespace facekit;

namespace mlbview {
namespace {

const uint16_t kText = rgb(225, 225, 225), kDim = rgb(100, 100, 112), kMid = rgb(160, 160, 170), kBlue = rgb(70, 140, 235), kOrange = rgb(235, 110, 20),
               kLive = rgb(60, 220, 90), kFav = rgb(255, 205, 40), kBall = rgb(238, 238, 230), kSeam = rgb(205, 40, 40),
               kRed = rgb(230, 50, 50), kPastel = rgb(150, 225, 165);

// ---- small helpers ----

int textWidth(const char *s) { return miniWidth(s); }
void centered(MatrixPanel_I2S_DMA &d, const char *s, int y, uint16_t colour) { miniText(d, s, (W - textWidth(s)) / 2, y, colour); }
void rightAligned(MatrixPanel_I2S_DMA &d, const char *s, int xRight, int y, uint16_t colour) { miniText(d, s, xRight - textWidth(s) + 1, y, colour); }

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

// A baseball at a position between pixels: every pixel gets the share of it the disc covers (4x4 samples) and the two seams are soft
// red lines that swing in and out as it turns, so a slowly moving ball glides instead of jumping a whole pixel
uint16_t mix565(uint16_t a, uint16_t b, int k) {  // k 0..16 of b
  const int r = ((a >> 11 << 3) * (16 - k) + (b >> 11 << 3) * k) / 16, g = (((a >> 5) & 63) * 4 * (16 - k) + ((b >> 5) & 63) * 4 * k) / 16,
            bl = ((a & 31) * 8 * (16 - k) + (b & 31) * 8 * k) / 16;
  return rgb(r, g, bl);
}
uint8_t gBallFade = 255;  // 0-255: lets a ball that lies still fade out instead of vanishing

void ballSmooth(MatrixPanel_I2S_DMA &d, float x, float y, float spin, float scale = 1.0f) {
  const float r = 2.8f * scale, cx = x + 0.5f, cy = y + 0.5f, apart = 0.2f + fabsf(cosf(spin)) * r * 0.55f;
  const int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
  const int span = (int)ceilf(2 * r) + 2;
  for (int py = y0; py <= y0 + span; py++)
    for (int px = x0; px <= x0 + span; px++) {
      int covered = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const float dx = px + (sx + 0.5f) / 4 - cx, dy = py + (sy + 0.5f) / 4 - cy;
          if (dx * dx + dy * dy <= r * r) covered++;
        }
      if (!covered) continue;
      const float dy = py + 0.5f - cy, bend = 0.18f * dy * dy;  // the seams curve like ) (
      const float left = fmaxf(0.0f, 1 - fabsf(px + 0.5f - (cx - apart - bend))), right = fmaxf(0.0f, 1 - fabsf(px + 0.5f - (cx + apart + bend)));
      const uint16_t face = mix565(kBall, kSeam, (int)(16 * fmaxf(left, right) * 0.85f));
      const int alpha = covered * 255 / 16 * gBallFade / 255;
      put(d, px, py, alpha >= 255 ? face : dim(face, alpha));
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

// ---- times ----

void clockText(ClockTime *time, char *out, size_t size) {
  if (time->is24hFormat()) snprintf(out, size, "%02d:%02d", time->getHour(), time->getMinute());
  else snprintf(out, size, "%d:%02d", time->getHour(), time->getMinute());
}

// The local first pitch time: "7:05P" or "19:05"
void firstPitch(ClockTime *time, int64_t start, char *out, size_t size) {
  struct tm local;
  time_t t = time->utcToLocal((time_t)start);
  gmtime_r(&t, &local);
  if (time->is24hFormat()) snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
  else snprintf(out, size, "%d:%02d%c", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min, local.tm_hour < 12 ? 'A' : 'P');
}

// "TOP 7", "BOT 7", "MID 7" (between the halves, the top is over), "END 7", "FINAL", "FINAL 10", or the first pitch
void status(const mlb::Game &g, ClockTime *time, char *out, size_t size, uint16_t &colour, bool compact) {
  colour = kDim;
  switch (g.state) {
    case mlb::Live:
      colour = kLive;
      if (g.delayed) snprintf(out, size, "DELAY");
      else snprintf(out, size, "%s %d", g.between ? (g.bottom ? "END" : "MID") : g.bottom ? "BOT" : "TOP", g.inning);
      break;
    case mlb::Final:
      if (g.inning > 9) snprintf(out, size, "FINAL %d", g.inning);
      else snprintf(out, size, "FINAL");
      break;
    default:
      firstPitch(time, g.start, out, size);
      if (!compact && !time->is24hFormat()) strlcat(out, "M", size);
  }
}

// How far into the game it is, in innings (0 to 9; extra innings are cut to 9)
float gameProgress(const mlb::Game &g) {
  if (g.state == mlb::Final) return 9;
  if (g.state != mlb::Live || g.inning == 0) return 0;
  if (g.inning > 9) return 9;
  const float half = g.between ? (g.bottom ? 1.0f : 0.5f) : (g.bottom ? 0.5f : 0.0f);
  return (g.inning - 1) + half;
}

void header(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, bool live, const char *label = "MLB") {
  char text[8];
  clockText(time, text, sizeof(text));
  textThin(d, label, 2, 2, kBlue);  // the thin 5x7 capitals, one pixel clear of the favourite bar on the left edge
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
  box(d, 0, 12, W, 1, dim(kBlue, 150));
}

void pageDots(MatrixPanel_I2S_DMA &d, int page, int pages, int y) {
  if (pages < 2) return;
  const int width = pages * 4 - 2;
  for (int i = 0; i < pages; i++) box(d, (W - width) / 2 + i * 4, y, 2, 1, i == page ? kText : dim(kDim, 150));
}

// A little diamond of three 3x3 bases (second on top, third left, first right) at x, y (11 wide, 7 tall): a runner makes the base yellow
void miniBases(MatrixPanel_I2S_DMA &d, uint8_t bases, int x, int y) {
  const int at[3][2] = {{x + 8, y + 4}, {x + 4, y}, {x, y + 4}};
  for (int b = 0; b < 3; b++) {
    if (bases >> b & 1) {
      box(d, at[b][0], at[b][1], 3, 3, kFav);
    } else {
      const uint16_t c = dim(kMid, 120);
      box(d, at[b][0], at[b][1], 3, 1, c);
      box(d, at[b][0], at[b][1] + 2, 3, 1, c);
      put(d, at[b][0], at[b][1] + 1, c);
      put(d, at[b][0] + 2, at[b][1] + 1, c);
    }
  }
}

}  // namespace

bool backgroundByFace = false;
namespace {
bool backgroundOff() { return backgroundByFace; }
}

// ---- backgrounds: drawn by the pages themselves, or once by the face while two pages that share one slide over each other ----

namespace {
// The backdrop of most pages, from the football and NBA faces: a crowd of multi-coloured dots (every second pixel of every second row, thinner
// and dimmer towards the top, an aisle every 15 columns), the outfield fence with its ad boards and the mown grass in front of it
void stands(MatrixPanel_I2S_DMA &d, int boost = 0) {  // boost: extra brightness for the crowd, for the flashing of a run
  static const uint16_t crowd[5] = {rgb(120, 30, 30), rgb(30, 60, 130), rgb(130, 120, 40), rgb(110, 110, 118), rgb(30, 100, 60)};
  static const uint16_t ads[4] = {rgb(60, 72, 120), rgb(120, 56, 62), rgb(84, 88, 100), rgb(50, 110, 104)};
  const int bandY = 55;
  for (int y = 2; y < bandY; y += 2) {
    const int k = 100 * (bandY - y) / (bandY - 2);  // 0 at the fence .. 100 at the top
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (x >= 14 && (x + 1) % 15 < 2) continue;
      uint32_t h = (x * 73856093u) ^ (y * 19349663u);
      h = (h ^ (h >> 13)) * 1274126177u;
      if ((h >> 8) % 100 >= (unsigned)(78 - 36 * k / 100)) continue;
      put(d, x, y, dim(crowd[(h >> 16) % 5], 62 - 32 * k / 100 + boost));
    }
  }
  box(d, 0, bandY, W, 1, rgb(60, 50, 6));  // the yellow line along the top of the fence
  for (int x = 0; x < W; x++) box(d, x, bandY + 1, 1, 2, dim(ads[x / 15 % 4], x % 15 == 14 ? 8 : 40));
  box(d, 0, bandY + 3, W, 1, rgb(6, 22, 9));
  for (int x = 0; x < W; x++) box(d, x, 60, 1, 4, (x / 8) % 2 ? rgb(7, 36, 12) : rgb(5, 27, 9));  // mown stripes
}

// Mown grass in horizontal stripes (the games of the day)
void grass(MatrixPanel_I2S_DMA &d, int top = 13) {
  for (int y = top; y < H; y++) box(d, 0, y, W, 1, ((y - top) / 5) % 2 ? rgb(7, 30, 11) : rgb(5, 22, 8));
}

// The backstop net seen from behind home plate: crossing diagonals in a league's colour
void netting(MatrixPanel_I2S_DMA &d, uint16_t colour, int top = 13) {
  for (int y = top; y < H; y++)
    for (int x = (y - top) % 6; x < W; x += 6) { put(d, x, y, colour); put(d, W - 1 - x, y, colour); }
}

// Behind the line score: a faint scoreboard grid (a dot every third pixel) that glows in the away team's colour at the top left
// and the home team's at the top right, fading downwards
void scoreboard(MatrixPanel_I2S_DMA &d, uint16_t away, uint16_t home) {
  auto channel = [](uint16_t c, int i) { return i == 0 ? c >> 11 << 3 : i == 1 ? ((c >> 5) & 63) * 4 : (c & 31) * 8; };
  for (int y = 13; y < H; y++) {
    if (y % 3 != 1) continue;
    for (int x = 1; x < W; x += 3) {
      const float a = fmaxf(0.0f, 1 - hypotf(x - 8.0f, y - 25.0f) / 24) * 0.35f, h = fmaxf(0.0f, 1 - hypotf(x - 56.0f, y - 25.0f) / 24) * 0.35f;
      int c[3];
      for (int i = 0; i < 3; i++) c[i] = std::min(255, (i == 2 ? 20 : 16) + (int)(channel(away, i) * a + channel(home, i) * h));
      put(d, x, y, rgb(c[0], c[1], c[2]));
    }
  }
}

// The 3x5 font of the football face (its '7' and 'W' differ a little from the mini font): one row per byte, bit 2 = left pixel
const uint8_t *glyphFootball(char c) {
  static const struct { char c; uint8_t rows[5]; } FONT[] = {
    {'0', {7,5,5,5,7}}, {'1', {2,6,2,2,7}}, {'2', {7,1,7,4,7}}, {'3', {7,1,7,1,7}}, {'4', {5,5,7,1,1}},
    {'5', {7,4,7,1,7}}, {'6', {7,4,7,5,7}}, {'7', {7,1,1,1,1}}, {'8', {7,5,7,5,7}}, {'9', {7,5,7,1,7}},
    {'A', {2,5,7,5,5}}, {'B', {6,5,6,5,6}}, {'C', {3,4,4,4,3}}, {'D', {6,5,5,5,6}}, {'E', {7,4,6,4,7}},
    {'F', {7,4,6,4,4}}, {'G', {3,4,5,5,3}}, {'H', {5,5,7,5,5}}, {'I', {7,2,2,2,7}}, {'J', {1,1,1,5,2}},
    {'K', {5,5,6,5,5}}, {'L', {4,4,4,4,7}}, {'M', {5,7,7,5,5}}, {'N', {6,5,5,5,5}}, {'O', {7,5,5,5,7}},
    {'P', {6,5,6,4,4}}, {'Q', {2,5,5,7,3}}, {'R', {6,5,6,5,5}}, {'S', {3,4,2,1,6}}, {'T', {7,2,2,2,2}},
    {'U', {5,5,5,5,7}}, {'V', {5,5,5,5,2}}, {'W', {5,5,5,7,5}}, {'X', {5,5,2,5,5}}, {'Y', {5,5,2,2,2}},
    {'Z', {7,1,2,4,7}}, {'-', {0,0,7,0,0}}, {'+', {0,2,7,2,0}}, {':', {0,2,0,2,0}}, {'/', {1,1,2,4,4}}, {'.', {0,0,0,0,2}},
    {'&', {2,5,2,5,3}},
  };
  static const uint8_t blank[5] = {0, 0, 0, 0, 0};
  for (const auto &g : FONT)
    if (g.c == c) return g.rows;
  return blank;
}

void text3(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) {
  for (; *s; s++, x += 4) {
    const uint8_t *g = glyphFootball(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g[row] >> (2 - col) & 1) put(d, x + col, y + row, colour);
  }
}
void text3Right(MatrixPanel_I2S_DMA &d, const char *s, int xRight, int y, uint16_t colour) { text3(d, s, xRight - textWidth(s) + 1, y, colour); }

// The crowd keeps one pixel clear of the text and digits of the clock page: the pixels they cover, marked before the stands are drawn
uint64_t gInk[H] = {};
bool gInkValid = false;
void inkClear() { for (int y = 0; y < H; y++) gInk[y] = 0; gInkValid = true; }
void inkPut(int x, int y) { if (x >= 0 && x < W && y >= 0 && y < H) gInk[y] |= 1ull << x; }
bool inkNear(int x, int y) {
  if (!gInkValid) return false;
  for (int dy = -1; dy <= 1; dy++) {
    const int yy = y + dy;
    if (yy < 0 || yy >= H) continue;
    const uint64_t bits = gInk[yy] >> (x > 0 ? x - 1 : 0);
    if (x == 0 ? (bits & 3) : (bits & 7)) return true;
  }
  return false;
}
void inkText(const char *s, int x, int y) {
  for (; *s; s++, x += 4) {
    const uint8_t *g = glyphFootball(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g[row] >> (2 - col) & 1) inkPut(x + col, y + row);
  }
}
void inkDigit(int value, int x, int y) {
  for (int row = 0; row < 21; row++)
    for (int col = 0; col < 14; col++)
      if (numericfont::pixel(value, col, row, 14, 21) || (value == 1 && col == 0 && row >= 21 - 4)) inkPut(x + col, y + row);
}

// The stadium of the football face, dot for dot (the clock page): the stands (thinner and dimmer the higher, an aisle every 15 columns), a band of four
// crowd rows, the dark front of the sideline boards, the advertising boards (15 px blocks), a green line and the grass in 12 px stripes down to the bottom
// edge, dimmed so the next game can be read on it. gaps: the date and the seconds stand clear of the crowd
uint16_t crowdDot(int x, int y, int level, int chance = 72) {  // black where this pixel has no dot
  static const uint16_t colours[5] = {rgb(120, 30, 30), rgb(30, 60, 130), rgb(130, 120, 40), rgb(110, 110, 118), rgb(30, 100, 60)};
  uint32_t h = (x * 73856093u) ^ (y * 19349663u);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (h >> 8) % 100 < (unsigned)chance ? dim(colours[(h >> 16) % 5], level * 255 / 32) : 0;
}
bool aisleAt(int x) { return x >= 14 && (x + 1) % 15 < 2; }

void stadium(MatrixPanel_I2S_DMA &d, bool gaps, int bandY, int grassDim, bool ribbon = false) {
  static const uint16_t ads[4] = {rgb(60, 72, 120), rgb(120, 56, 62), rgb(84, 88, 100), rgb(50, 110, 104)};
  for (int y = 2; y < bandY; y += 2) {
    const int k = 100 * (bandY - y) / (bandY - 2);  // 0 at the band .. 100 at the top
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (aisleAt(x) || inkNear(x, y)) continue;
      const uint16_t c = crowdDot(x, y, 14 - 8 * k / 100, (80 - 30 * k / 100) / (ribbon ? 2 : 1));
      if (c) put(d, x, y, c);
    }
  }
  for (int y = bandY; y <= bandY + 6; y += 2)
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (aisleAt(x)) continue;
      const uint16_t c = crowdDot(x, y, 12 + 3 * (y - bandY) / 2, ribbon ? 36 : 72);
      if (c) put(d, x, y, c);
    }
  box(d, 0, bandY + 8, W, 1, rgb(22, 25, 33));
  if (ribbon) {  // an LED ribbon board instead of the ad boards: black, the next game is written on it; then the green line and 8 rows of grass
    box(d, 0, bandY + 9, W, 7, rgb(26, 20, 10));  // a warm dark board with a thin brass edge
    box(d, 0, bandY + 9, W, 1, rgb(90, 70, 30));
    box(d, 0, bandY + 15, W, 1, rgb(90, 70, 30));
    box(d, 0, bandY + 16, W, 1, rgb(60, 130, 70));
    for (int x = 0; x < W; x++) box(d, x, bandY + 17, 1, H - bandY - 17, dim((x / 12) % 2 == 0 ? rgb(14, 70, 28) : rgb(10, 54, 22), grassDim * 255 / 32));
    return;
  }
  for (int x = 0; x < W; x++) box(d, x, bandY + 9, 1, 2, x % 15 == 14 ? dim(ads[x / 15 % 4], 16 * 255 / 32) : ads[x / 15 % 4]);
  box(d, 0, bandY + 11, W, 1, rgb(60, 130, 70));
  for (int x = 0; x < W; x++) box(d, x, bandY + 12, 1, H - bandY - 12, dim((x / 12) % 2 == 0 ? rgb(14, 70, 28) : rgb(10, 54, 22), grassDim * 255 / 32));
}

void paint(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) {
  switch (background) {
    case BgGame: stands(d); return;  // the same stands as the other pages, no band behind the time
    case BgLine: scoreboard(d, rgb(120, 120, 130), rgb(120, 120, 130)); break;
    case BgToday: grass(d); break;
    case BgNext: stands(d); return;  // no band behind the time here
    case BgAmerican: netting(d, rgb(44, 11, 11)); break;
    case BgNational: netting(d, rgb(10, 18, 44)); break;
    case BgClock: stadium(d, true, 39, 14, true); return;  // the stadium of the football face
  }
  box(d, 0, 0, W, 12, rgb(8, 14, 30));  // the band behind the time
}
}  // namespace

void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { paint(d, background, ms); }
void backgroundDrawnByFace(bool yes) { backgroundByFace = yes; }

// ---- favourites and order ----

// A team's colour for the bar before its name, by ESPN's abbreviation (picked to be readable on black); `fallback` for a team not in the list
uint16_t teamColour(const char *abbr, uint16_t fallback = rgb(120, 120, 120)) {
  struct Team { const char *abbr; uint8_t r, g, b; };
  static const Team teams[] = {
    {"ARI", 225, 60, 80}, {"AZ", 225, 60, 80}, {"ATH", 40, 165, 95}, {"OAK", 40, 165, 95}, {"ATL", 215, 50, 70}, {"BAL", 255, 105, 25},
    {"BOS", 220, 50, 60}, {"CHC", 60, 120, 230}, {"CHW", 200, 200, 205}, {"CWS", 200, 200, 205}, {"CIN", 220, 40, 50}, {"CLE", 230, 50, 60},
    {"COL", 150, 110, 230}, {"DET", 80, 140, 230}, {"HOU", 240, 135, 40}, {"KC", 80, 150, 230}, {"LAA", 235, 55, 65}, {"LAD", 60, 140, 240},
    {"MIA", 40, 190, 220}, {"MIL", 255, 200, 50}, {"MIN", 220, 50, 70}, {"NYM", 250, 110, 40}, {"NYY", 150, 170, 205}, {"PHI", 230, 50, 70},
    {"PIT", 255, 205, 40}, {"SD", 205, 165, 110}, {"SEA", 40, 170, 170}, {"SF", 250, 110, 40}, {"STL", 220, 50, 60}, {"TB", 110, 170, 240},
    {"TEX", 60, 110, 230}, {"TOR", 60, 140, 240}, {"WSH", 230, 50, 70}, {"WAS", 230, 50, 70}};
  for (const Team &t : teams)
    if (!strcmp(t.abbr, abbr)) return rgb(t.r, t.g, t.b);
  return fallback;
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

bool hasFavourite(const char *favourites, const mlb::Game &game) { return isFavourite(favourites, game.away) || isFavourite(favourites, game.home); }

int orderGames(const mlb::Snapshot &snap, const char *favourites, int *out) {
  int n = 0;
  // favourites first, then live, coming and finished: one pass per rank keeps ESPN's own order inside a rank
  for (int rank = 0; rank < 6; rank++)
    for (int i = 0; i < snap.count; i++) {
      const mlb::Game &g = snap.games[i];
      const int r = (hasFavourite(favourites, g) ? 0 : 3) + (g.state == mlb::Live ? 0 : g.state == mlb::Pre ? 1 : 2);
      if (r == rank) out[n++] = i;
    }
  return n;
}

int gamePages(int count) { return count ? (count + kGamesPerPage - 1) / kGamesPerPage : 0; }

namespace {
constexpr int kDividerX = 42, kColumnX = 43;  // the column of coming games is a third of the panel
constexpr int kFixtureCap = 32, kGameH = 19, kColumnGames = 3, kColumnTitleY = 1, kColumnGamesTop = 9;  // a game (time, away, home) takes 19 px; the day is the title (6 px high), 2 px clear, then the games; three games end on the last row
constexpr int kRankRows = 6, kRankStep = 6, kRankTop = 21, kSlideMs = 500;  // a table page: the title at kRankTop, a line, then six rows that slide up
}  // namespace

int fixturePages(const mlb::Snapshot &snap, ClockTime *) { return snap.nextCount ? 1 : 0; }  // one page that moves on by itself

int fixtureCount(const mlb::Snapshot &snap) { return std::min<int>(snap.nextCount, kFixtureCap); }
int dayOf(ClockTime *time, const mlb::Fixture &f) { return (int)(time->utcToLocal((time_t)f.start) / 86400); }

// The coming games as pages of the right column, like the table on the left: the day is the title and a page holds games of one day only, three when there are
// that many; the last page of a day is filled up with the games before it
struct ColumnPage { uint8_t first, count; int day; };
constexpr int kMaxColumnPages = 16;

// Quiet days (the finals, a day off): when no day has more than one game the column shows two days a page, each its day and its game
bool quietDays(const mlb::Snapshot &snap, ClockTime *time) {
  const int n = fixtureCount(snap);
  for (int i = 1; i < n; i++)
    if (dayOf(time, snap.next[i]) == dayOf(time, snap.next[i - 1])) return false;
  return n > 0;
}

// Two teams playing each other and nobody else (the finals): one page with the teams and the days they play, not the same game once a day
bool sameMatchup(const mlb::Snapshot &snap) {
  const int n = fixtureCount(snap);
  if (n < 2) return false;
  const mlb::Fixture &a = snap.next[0];
  for (int i = 1; i < n; i++) {
    const mlb::Fixture &f = snap.next[i];
    const bool same = !strcmp(f.away, a.away) && !strcmp(f.home, a.home), flipped = !strcmp(f.away, a.home) && !strcmp(f.home, a.away);
    if (!same && !flipped) return false;
  }
  return true;
}

int layoutColumn(const mlb::Snapshot &snap, ClockTime *time, ColumnPage *pages) {
  const int n = fixtureCount(snap);
  int count = 0, i = 0;
  if (sameMatchup(snap)) { pages[0] = {0, (uint8_t)n, dayOf(time, snap.next[0])}; return 1; }
  if (quietDays(snap, time)) {
    for (; i < n && count < kMaxColumnPages; i += 2) pages[count++] = {(uint8_t)i, (uint8_t)std::min(2, n - i), dayOf(time, snap.next[i])};
    return count;
  }
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

int rankPagesTotal(const mlb::Snapshot &snap) {
  if (snap.seriesCount) return snap.seriesCount;  // a playoff series is a page of its own
  const int american = snap.american.count ? (snap.american.count + kRankRows - 1) / kRankRows : 0, national = snap.national.count ? (snap.national.count + kRankRows - 1) / kRankRows : 0;
  return american + national;
}
uint32_t columnMs(uint32_t groupMs) { return std::min<uint32_t>(groupMs, 5000); }  // the coming games move on faster than the table: every 5 seconds

// The page lasts until both lists have been through once (the longer of the two), at most 65 seconds
uint32_t pageLength(const mlb::Snapshot &snap, ClockTime *time, uint32_t groupMs) {
  ColumnPage pages[kMaxColumnPages];
  const uint32_t column = layoutColumn(snap, time, pages) * columnMs(groupMs);
  return std::min<uint32_t>(65000, std::max<uint32_t>(std::max<uint32_t>(column, rankPagesTotal(snap) * groupMs), groupMs));
}

uint16_t upcomingMs(const mlb::Snapshot &snap, ClockTime *time, uint16_t pageMs) { return (uint16_t)pageLength(snap, time, pageMs); }

int standingsPages(const mlb::Table &table) { return table.count ? (table.count + kStandingsPerPage - 1) / kStandingsPerPage : 0; }

// ---- pages ----

void drawGame(MatrixPanel_I2S_DMA &d, const mlb::Game &g, ClockTime *time, uint32_t ms, bool flashAway, bool flashHome, bool favourite, int page, int pages) {
  backdrop(d, BgGame, ms);
  header(d, time, ms, g.state == mlb::Live);
  if (favourite) box(d, 0, 0, 1, 12, kFav);
  const bool away = g.awayScore > g.homeScore, home = g.homeScore > g.awayScore;
  for (int side = 0; side < 2; side++) {
    const bool isHome = side == 1;
    const int y = isHome ? 32 : 14;
    const uint16_t colour = isHome ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
    const int score = isHome ? g.homeScore : g.awayScore;
    const bool flash = isHome ? flashHome : flashAway;
    const bool leads = isHome ? home : away;
    if (flash) box(d, 3, y, W - 3, 17, dim(colour, 60 + (int)(40 * sinf(ms * 0.02f))));
    box(d, 0, y, 3, 17, colour);
    textThin(d, isHome ? g.home : g.away, 5, y + 1, g.state == mlb::Pre || leads || flash ? kText : kMid);
    if (g.state == mlb::Pre) {
      miniText(d, isHome ? g.homeRecord : g.awayRecord, 5, y + 11, kDim);
    } else {
      const int width = blockNumber(d, score, 0, 0, 8, 14, 0, true);
      blockNumber(d, score, W - 2 - width, y + 2, 8, 14, leads || flash || g.state == mlb::Live ? (flash ? kText : colour) : kDim);
      // the runs of each inning under the name: the latest ones that fit left of the score, so they never touch it
      const int played = isHome ? g.homeInnings : g.awayInnings;
      const uint8_t *runs = isHome ? g.homeRuns : g.awayRuns;
      const int room = W - 2 - width - 3 - 5 + 1;
      int first = played, used = 0;
      while (first > 0) {
        char one[6];
        snprintf(one, sizeof(one), "%d", runs[first - 1]);
        const int need = textWidth(one) + (used ? 3 : 0);
        if (used + need > room) break;
        used += need;
        first--;
      }
      for (int p = first, px = 5; p < played; p++) {
        char one[6];
        snprintf(one, sizeof(one), "%d", runs[p]);
        miniText(d, one, px, y + 11, g.state == mlb::Live && p == played - 1 ? kMid : kDim);
        px += textWidth(one) + 3;
      }
    }
  }
  if (g.state == mlb::Live && !g.between && !g.delayed) {
    // the bases, the inning with an arrow for the half, the count (balls-strikes) and the outs as three dots
    miniBases(d, g.bases, 2, 50);
    for (int row = 0; row < 3; row++) {  // an arrow up for the top of the inning, down for the bottom
      const int half = g.bottom ? 2 - row : row;
      box(d, 17 + 2 - half, 52 + row, 1 + 2 * half, 1, kLive);
    }
    char text[8];
    snprintf(text, sizeof(text), "%d", g.inning);
    miniText(d, text, 25, 51, kText);
    snprintf(text, sizeof(text), "%d-%d", g.balls, g.strikes);
    miniText(d, text, 36, 51, kText);
    for (int o = 0; o < 3; o++) box(d, 50 + o * 4, 52, 3, 3, o < g.outs ? kRed : dim(kDim, 110));
  } else {
    char text[16];
    uint16_t colour;
    status(g, time, text, sizeof(text), colour, false);
    centered(d, text, 53, colour);
  }
  // The inning bar: nine segments that fill as the game goes
  const float progress = gameProgress(g);
  for (int i = 0; i < 9; i++) {
    const int x = 1 + i * 7;
    const float fill = progress - i < 0 ? 0 : progress - i > 1 ? 1 : progress - i;
    box(d, x, 60, 6, 1, dim(kDim, 90));
    if (fill > 0) box(d, x, 60, (int)lroundf(fill * 6), 1, g.state == mlb::Final ? kMid : kLive);
  }
  pageDots(d, page, pages, 63);
}

// A diamond of four 7x7 bases at the middle of (cx, cy): the base paths dotted, a runner makes his base yellow
void diamond(MatrixPanel_I2S_DMA &d, uint8_t bases, int cx, int cy) {
  const int at[4][2] = {{cx + 8, cy}, {cx, cy - 8}, {cx - 8, cy}, {cx, cy + 8}};  // first, second, third, home
  for (int i = 0; i < 4; i++) {  // the paths
    const int a = i, b = (i + 1) % 4;
    for (int k = 2; k <= 5; k++) put(d, at[a][0] + (at[b][0] - at[a][0]) * k / 8, at[a][1] + (at[b][1] - at[a][1]) * k / 8, dim(kLive, 70));
  }
  for (int i = 0; i < 4; i++) {
    const bool runner = i < 3 && (bases >> i & 1);
    const uint16_t c = runner ? kFav : dim(kMid, i == 3 ? 160 : 110);
    for (int dy = -3; dy <= 3; dy++)
      for (int dx = -3; dx <= 3; dx++) {
        const int r = abs(dx) + abs(dy);
        if (runner ? r <= 3 : r == 3) put(d, at[i][0] + dx, at[i][1] + dy, c);
      }
  }
}

// A row of `lit` out of `count` round dots from x, y: for balls, strikes and outs
void countDots(MatrixPanel_I2S_DMA &d, int x, int y, int count, int lit, uint16_t colour) {
  for (int i = 0; i < count; i++) {
    box(d, x + i * 5, y, 3, 3, i < lit ? colour : dim(kDim, 110));
    if (i < lit) continue;
  }
}

// The runs of every inning with the totals (runs, hits, errors), who bats and the bases, count and outs under it
void drawLineScore(MatrixPanel_I2S_DMA &d, const mlb::Game &g, ClockTime *time, uint32_t ms) {
  header(d, time, ms, g.state == mlb::Live, "INN");
  if (!backgroundOff()) scoreboard(d, teamColour(g.away, g.awayColor), teamColour(g.home, g.homeColor));  // under the text
  const int columns = 7;
  int innings = std::max<int>(std::max<int>(g.awayInnings, g.homeInnings), g.state == mlb::Live ? g.inning : 0);
  if (innings > mlb::kInnings) innings = mlb::kInnings;
  const int first = std::max(0, innings - columns);  // the latest seven innings
  const int colX = 15, rX = 49, hX = 56, eX = 62;
  for (int i = 0; i < columns; i++) {
    const int inning = first + i + 1;
    char number[4];
    snprintf(number, sizeof(number), "%d", inning % 10);
    miniText(d, number, colX + i * 4, 15, g.state == mlb::Live && inning == g.inning ? kLive : kDim);
  }
  rightAligned(d, "R", rX, 15, kMid);
  rightAligned(d, "H", hX, 15, kMid);
  rightAligned(d, "E", eX, 15, kMid);
  box(d, 0, 21, W, 1, dim(kDim, 130));
  for (int side = 0; side < 2; side++) {
    const bool isHome = side == 1;
    const int y = isHome ? 31 : 23;
    const uint16_t colour = isHome ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
    const uint8_t *runs = isHome ? g.homeRuns : g.awayRuns;
    const int played = isHome ? g.homeInnings : g.awayInnings, score = isHome ? g.homeScore : g.awayScore;
    const bool batting = g.state == mlb::Live && !g.between && (g.bottom == isHome);
    box(d, 0, y - 1, 1, 7, colour);
    miniText(d, isHome ? g.home : g.away, 2, y, batting ? colour : dim(colour, 190));
    for (int i = 0; i < columns; i++) {
      const int n = first + i;
      char one[4];
      if (n < played) {
        snprintf(one, sizeof(one), "%d", runs[n] > 9 ? 9 : runs[n]);
        miniText(d, one, colX + i * 4, y, runs[n] ? kText : kDim);
      } else if (g.state == mlb::Final && isHome && n == played && played < innings) {
        miniText(d, "X", colX + i * 4, y, kDim);  // the home team did not need to bat
      }
    }
    char total[6];
    snprintf(total, sizeof(total), "%d", score);
    rightAligned(d, total, rX, y, kText);
    snprintf(total, sizeof(total), "%d", std::min<int>(isHome ? g.homeHits : g.awayHits, 99));
    rightAligned(d, total, hX, y, kMid);
    snprintf(total, sizeof(total), "%d", std::min<int>(isHome ? g.homeErrors : g.awayErrors, 9));
    rightAligned(d, total, eX, y, kMid);
  }
  box(d, 0, 38, W, 1, dim(kDim, 130));
  // the lower half: the bases on the left, on the right the state of the game
  const bool inPlay = g.state == mlb::Live && !g.between && !g.delayed;
  diamond(d, inPlay ? g.bases : 0, 13, 50);
  char text[16];
  uint16_t colour;
  status(g, time, text, sizeof(text), colour, false);
  if (g.state == mlb::Live) {
    miniText(d, text, 31, 41, colour);
    if (inPlay) {
      miniText(d, g.bottom ? g.home : g.away, 52, 41, g.bottom ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor));
      miniText(d, "B", 31, 47, kMid);
      countDots(d, 38, 47, 4, g.balls, kLive);
      miniText(d, "S", 31, 52, kMid);
      countDots(d, 38, 52, 3, g.strikes, kRed);
      miniText(d, "O", 31, 57, kMid);
      countDots(d, 38, 57, 3, g.outs, kOrange);
    }
  } else {
    textThin(d, g.state == mlb::Final ? "FINAL" : "SOON", 33, 44, colour == kDim ? kMid : colour);
    if (g.state == mlb::Final && g.inning > 9) {
      snprintf(text, sizeof(text), "%d INN", g.inning);
      miniText(d, text, 33, 55, kDim);
    } else if (g.state == mlb::Pre) {
      firstPitch(time, g.start, text, sizeof(text));
      miniText(d, text, 33, 55, kDim);
    }
  }
}

void drawToday(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, const int *order, int count, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  backdrop(d, BgToday, ms);
  header(d, time, ms, snap.live > 0);
  const int first = page * kGamesPerPage;
  for (int row = 0; row < kGamesPerPage && first + row < count; row++) {
    const mlb::Game &g = snap.games[order[first + row]];
    const int y = 14 + row * 12;
    const bool fav = hasFavourite(favourites, g);
    const bool final = g.state == mlb::Final, pre = g.state == mlb::Pre;
    const bool awayWon = final && g.awayScore > g.homeScore, homeWon = final && g.homeScore > g.awayScore;
    const uint16_t awayColour = teamColour(g.away, g.awayColor), homeColour = teamColour(g.home, g.homeColor);
    if (fav) box(d, 0, y, 1, 11, kFav);
    miniText(d, g.away, 2, y + 1, final && !awayWon ? dim(awayColour, 150) : awayColour);
    miniText(d, g.home, 35, y + 1, final && !homeWon ? dim(homeColour, 150) : homeColour);
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
  pageDots(d, page, gamePages(count), 63);
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
      blockDigit(d, digits[i] - '0', 1 + i * 9 + (i >= 2 ? 4 : 0) + (pass ? 0 : 1), 1 + (pass ? 0 : 1), 8, 14, pass ? kText : rgb(24, 52, 118));
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

// Letter font for the page titles (5 wide, 6 high): one string a glyph, row after row
const char *const kTitle56[26] = {
    ".XXX.X...XX...XXXXXXX...XX...X",
    "XXXX.X...XXXXX.X...XX...XXXXX.",
    ".XXXXX....X....X....X.....XXXX",
    "XXXX.X...XX...XX...XX...XXXXX.",
    "XXXXXX....XXXX.X....X....XXXXX",
    "XXXXXX....XXXX.X....X....X....",
    ".XXXXX....X..XXX...XX...X.XXX.",
    "X...XX...XXXXXXX...XX...XX...X",
    "XXXXX..X....X....X....X..XXXXX",
    "..XXX...X....X....X.X..X..XX..",
    "X...XX..X.XXX..X..X.X...XX...X",
    "X....X....X....X....X....XXXXX",
    "X...XXX.XXX.X.XX...XX...XX...X",
    "X...XXX..XX.X.XX..XXX...XX...X",
    ".XXX.X...XX...XX...XX...X.XXX.",
    "XXXX.X...XXXXX.X....X....X....",
    ".XXX.X...XX...XX.X.XX..X..XX.X",
    "XXXX.X...XXXXX.X.X..X..X.X...X",
    ".XXXXX.....XXX.....X....XXXXX.",
    "XXXXX..X....X....X....X....X..",
    "X...XX...XX...XX...XX...X.XXX.",
    "X...XX...XX...XX...X.X.X...X..",
    "X...XX...XX.X.XX.X.XXX.XXX...X",
    "X...X.X.X...X....X...X.X.X...X",
    "X...X.X.X...X....X....X....X..",
    "XXXXX....X...X...X...X...XXXXX",
};

void fontText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour, int minY, int maxX, int minX, const char *const *font, int w, int h) {
  for (; *s; s++) {
    if (*s < 'A' || *s > 'Z') continue;
    const char *g = font[*s - 'A'];
    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++)
        if (g[row * w + col] == 'X' && y + row >= minY && y + row < H && x + col >= minX && x + col < maxX) put(d, x + col, y + row, colour);
    x += w + 1;
  }
}
void titleText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour, int maxX, int minX = 0) { fontText(d, s, x, y, colour, 0, maxX, minX, kTitle56, 5, 6); }
constexpr int kBigGameH = 21, kBigGamesTop = 9;  // a game with the big names (19 px: time, two 6 px names) and 2 px clear of the next one; 2 px under the big title

// The first team of a table page: the last page is filled up with the teams before it, so it always has six rows
int rankFirst(const mlb::Snapshot &snap, int index) {
  const int americanPages = snap.american.count ? (snap.american.count + kRankRows - 1) / kRankRows : 0;
  const bool american = index < americanPages;
  const int count = american ? snap.american.count : snap.national.count;
  return std::min((american ? index : index - americanPages) * kRankRows, std::max(0, count - kRankRows));
}

// "58-104" is too wide for the table of the next-up page (three digits of wins), so it shows the winning percentage: ".583" (digits only,
// the dot is added when it is drawn)
void percentage(const mlb::Standing &row, char *out, size_t size) {
  const int games = row.wins + row.losses;
  const int per = games ? (row.wins * 1000 + games / 2) / games : 0;
  snprintf(out, size, "%03d", per > 999 ? 999 : per);
}

// The six rows of one page of a league table, the first at y0; what is above `minY` is not drawn
void rankRows(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, int index, int y0, const char *favourites, int minY) {
  const int americanPages = snap.american.count ? (snap.american.count + kRankRows - 1) / kRankRows : 0;
  const bool american = index < americanPages;
  const mlb::Table &table = american ? snap.american : snap.national;
  const int first = rankFirst(snap, index);
  for (int r = 0; r < kRankRows && first + r < table.count; r++) {
    const int y = y0 + r * kRankStep;
    if (y + 5 <= minY || y >= H) continue;
    const mlb::Standing &row = table.rows[first + r];
    char text[12];
    snprintf(text, sizeof(text), "%d", first + r + 1);
    clipText(d, text, (7 - textWidth(text)) / 2, y, kBlue, minY, kDividerX);  // no margin on the left (the title keeps its one pixel)
    const int shift = first + r + 1 >= 10 ? 1 : 0;  // from number 10 the rank is two digits wide: the bar and the name start a pixel further right
    for (int k = 0; k < 5; k++) if (y + k >= minY && y + k < H) put(d, 7 + shift, y + k, teamColour(row.abbr));  // the team's colour, one pixel wide
    char pct[8];
    percentage(row, pct, sizeof(pct));
    const int left = kDividerX - 2 - textWidth(pct) + 1;  // one pixel clear of the divider
    clipText(d, row.abbr, 10 + shift, y, isFavourite(favourites, row.abbr) ? kFav : kText, minY, kDividerX);
    clipText(d, pct, left, y, kMid, minY, kDividerX);
    if (y + 4 >= minY && y + 4 < H) put(d, left - 2, y + 4, kMid);  // the dot right against the digits (one pixel between)
  }
}

bool rankAmerican(const mlb::Snapshot &snap, int index) { return index < (snap.american.count ? (snap.american.count + kRankRows - 1) / kRankRows : 0); }

// One coming game at y: the time, then the away team, then the home team; what is above `minY` is not drawn (the games slide up under the day)
void columnTime(MatrixPanel_I2S_DMA &d, const mlb::Fixture &f, ClockTime *time, int y, int minY, int left = -1) {  // left: where it starts, centred when not given
  char text[8];
  firstPitch(time, f.start, text, sizeof(text));
  // the tip-off time with the colon one pixel from the digits on both sides (the 3x5 colon has two empty columns each side)
  int total = 0;
  for (const char *c = text; *c; c++) total += *c == ':' ? 2 : 4;
  int x = left >= 0 ? left : kColumnX + (21 - (total - (text[strlen(text) - 1] == ':' ? 0 : 1))) / 2 - 1;
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
}

void columnGame(MatrixPanel_I2S_DMA &d, const mlb::Fixture &f, ClockTime *time, int y, const char *favourites, int minY, bool big = false) {
  if (y + 16 < minY || y >= H) return;
  columnTime(d, f, time, y, minY);
  if (big) {  // room to spare: the teams in the 5x6 capitals of the titles
    for (int t = 0; t < 2; t++) {
      const char *name = t ? f.home : f.away;
      const int top = y + 6 + t * 7;
      for (int r = 0; r < 6; r++)
        if (top + r >= minY && top + r < H) put(d, kColumnX + 1, top + r, teamColour(name));
      fontText(d, name, kColumnX + 4, top, isFavourite(favourites, name) ? kFav : kText, minY, W, 0, kTitle56, 5, 6);
    }
    return;
  }
  for (int r = 0; r < 5; r++)
    if (y + 6 + r >= minY && y + 6 + r < H) put(d, kColumnX + 1, y + 6 + r, teamColour(f.away));  // the teams' colours, a pixel wide like in the table
  for (int r = 0; r < 5; r++)
    if (y + 12 + r >= minY && y + 12 + r < H) put(d, kColumnX + 1, y + 12 + r, teamColour(f.home));
  clipText(d, f.away, kColumnX + 4, y + 6, isFavourite(favourites, f.away) ? kFav : kText, minY, W);
  clipText(d, f.home, kColumnX + 4, y + 12, isFavourite(favourites, f.home) ? kFav : kText, minY, W);
}

const char *dayName(ClockTime *time, const mlb::Fixture &f) {
  static const char *const names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  struct tm local;
  time_t when = time->utcToLocal((time_t)f.start);
  gmtime_r(&when, &local);
  return names[local.tm_wday];
}

// In a playoff series the series card on the left names the teams: here only the days and times, a bar in front in the home team's colour
void drawSeriesDates(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, ClockTime *time) {
  const int n = std::min(fixtureCount(snap), 4);
  for (int i = 0; i < n; i++) {
    const int y = 3 + i * 14;
    for (int r = 0; r < 11; r++) put(d, kColumnX + 1, y + r, teamColour(snap.next[i].home));
    clipText(d, dayName(time, snap.next[i]), kColumnX + 4, y, kPastel, 0, W, kColumnX + 4);
    columnTime(d, snap.next[i], time, y + 6, 0, kColumnX + 4);
  }
}

void drawMatchup(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, ClockTime *time, const char *favourites) {
  const mlb::Fixture &first = snap.next[0];
  const char *const names[2] = {first.away, first.home};
  for (int t = 0; t < 2; t++) {  // the teams like in a game: colour bar and name in the small font
    const int y = 1 + t * 6;
    for (int r = 0; r < 5; r++) put(d, kColumnX + 1, y + r, teamColour(names[t]));
    clipText(d, names[t], kColumnX + 4, y, isFavourite(favourites, names[t]) ? kFav : kText, 0, W);
  }
  box(d, kColumnX, 13, W - kColumnX, 1, rgb(24, 20, 24));
  const int n = std::min(fixtureCount(snap), 3);  // three games fit with room between them
  int y = 17;
  for (int i = 0; i < n; i++) {  // a day once, the times of its games under it (two games on a day share the heading)
    if (i == 0 || dayOf(time, snap.next[i]) != dayOf(time, snap.next[i - 1])) {
      if (i) y += 2;
      clipText(d, dayName(time, snap.next[i]), kColumnX + 1, y, kPastel, 0, W, kColumnX + 1);
      y += 6;
    }
    columnTime(d, snap.next[i], time, y, 0);
    y += 6;
  }
}

// The playoff series for the left column of the next-up page (in place of the league table): the round, both teams with the games they have
// won, a box for each game of the series and which game is next
void drawSeriesCard(MatrixPanel_I2S_DMA &d, const mlb::Series &series) {
  titleText(d, series.round, 1, kRankTop - 2, kPastel, kDividerX);
  box(d, 0, kRankTop + 5, kDividerX, 1, rgb(24, 20, 24));
  const uint16_t colours[2] = {teamColour(series.first, series.firstColor), teamColour(series.second, series.secondColor)};
  for (int i = 0; i < 2; i++) {
    const int y = 29 + i * 13, wins = i ? series.secondWins : series.firstWins, other = i ? series.firstWins : series.secondWins;
    box(d, 0, y, 1, 10, colours[i]);
    textThin(d, i ? series.second : series.first, 3, y, wins >= other ? kText : kMid);
    blockNumber(d, wins, kDividerX - 9, y, 6, 10, wins >= other ? colours[i] : dim(colours[i], 150));
  }
  for (int g = 0; g < series.bestOf; g++) {
    const uint16_t c = g < series.firstWins ? colours[0] : g < series.firstWins + series.secondWins ? colours[1] : dim(kDim, 110);
    box(d, 3 + g * 5, 54, 4, 4, c);
  }
  char text[12];
  if (series.game) snprintf(text, sizeof(text), "GAME %d", series.game);
  else snprintf(text, sizeof(text), "FINAL");
  miniText(d, text, 3, 59, kDim);
}

void drawUpcoming(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, const char *favourites, int page, ClockTime *time, uint32_t ms, uint32_t elapsed, uint32_t groupMs) {
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
  } else if (sameMatchup(snap)) {
    if (snap.seriesCount) drawSeriesDates(d, snap, time);
    else drawMatchup(d, snap, time, favourites);
  } else if (quietDays(snap, time)) {  // two days a page, the whole page slides up
    const int now = kc % columnPages, was = (kc + columnPages - 1) % columnPages;
    const bool sliding = inColumn < (uint32_t)kSlideMs && columnPages > 1;
    const int distance = 52, shift = sliding ? (int)lroundf(distance * easeC) : 0;
    auto block = [&](const ColumnPage &p, int y0) {
      for (int i = 0; i < p.count; i++) {
        const mlb::Fixture &f = snap.next[p.first + i];
        clipText(d, dayName(time, f), kColumnX + 1, y0 + i * 26 + 1, kPastel, 0, W, kColumnX + 1);
        columnGame(d, f, time, y0 + i * 26 + 8, favourites, 0);
      }
    };
    if (sliding) { block(column[was], -shift); block(column[now], distance - shift); }
    else block(column[now], 0);
  } else {
    const int now = kc % columnPages, was = (kc + columnPages - 1) % columnPages;
    const ColumnPage &cur = column[now], &old = column[was];
    const bool slidingGames = inColumn < (uint32_t)kSlideMs && columnPages > 1;
    const bool dayChanges = slidingGames && cur.day != old.day;
    const int added = !dayChanges && cur.first > old.first ? std::min<int>(kColumnGames, cur.first - old.first) : kColumnGames;  // as many as the new page adds
    const bool bigNow = cur.count <= 2;  // few games on the page: the big names, and the day in the big title letters
    const int gameH = bigNow ? kBigGameH : kGameH, top = bigNow ? kBigGamesTop : kColumnGamesTop, distance = added * gameH;
    if (dayChanges && inColumn < 200) {
      const float p = inColumn / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf(21 * e);
      titleText(d, dayName(time, snap.next[old.first]), kColumnX + 1 - offset, kColumnTitleY, kPastel, W, kColumnX + 1);
      titleText(d, dayName(time, snap.next[cur.first]), kColumnX + 1 + 21 - offset, kColumnTitleY, kPastel, W, kColumnX + 1);
    } else {
      titleText(d, dayName(time, snap.next[cur.first]), kColumnX + 1, kColumnTitleY, kPastel, W, kColumnX + 1);
    }
    if (slidingGames) {
      const int shift = (int)lroundf(distance * easeC);
      for (int g = 0; g < old.count; g++) columnGame(d, snap.next[old.first + g], time, top + g * gameH - shift, favourites, top, old.count <= 2);
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, top + distance - shift + g * gameH, favourites, top, bigNow);
    } else {
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, top + g * gameH, favourites, top, bigNow);
    }
  }
  // the league table: its title (AL, NL) stays where it is while the rows slide up under it; the title only slides sideways when it changes
  const int pages = rankPagesTotal(snap);
  if (snap.seriesCount) {  // the playoffs: the series instead of the league table
    drawSeriesCard(d, snap.series[kt % snap.seriesCount]);
  } else if (pages) {
    const int now = kt % pages, was = (kt + pages - 1) % pages;
    const int rowsTop = kRankTop + 7;
    const bool slidingRows = inTable < (uint32_t)kSlideMs && pages > 1;
    const bool changes = slidingRows && rankAmerican(snap, now) != rankAmerican(snap, was);
    // the rows move up by as many as the new page adds, no further (a whole page when the other conference comes)
    const int added = !changes && rankFirst(snap, now) > rankFirst(snap, was) ? std::min(kRankRows, rankFirst(snap, now) - rankFirst(snap, was)) : kRankRows;
    const int distance = added * kRankStep;
    if (changes && inTable < 200) {
      const float p = inTable / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf((kDividerX - 2) * e);
      titleText(d, rankAmerican(snap, was) ? "AL" : "NL", 1 - offset, kRankTop - 2, kPastel, kDividerX);
      titleText(d, rankAmerican(snap, now) ? "AL" : "NL", 1 + (kDividerX - 2) - offset, kRankTop - 2, kPastel, kDividerX);
    } else {
      titleText(d, rankAmerican(snap, now) ? "AL" : "NL", 1, kRankTop - 2, kPastel, kDividerX);
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

void drawStandings(MatrixPanel_I2S_DMA &d, const mlb::Table &table, bool american, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  backdrop(d, american ? BgAmerican : BgNational, ms);
  header(d, time, ms, false, american ? "AL" : "NL");
  const int first = page * kStandingsPerPage;
  for (int row = 0; row < kStandingsPerPage && first + row < table.count; row++) {
    const int rank = first + row;
    const mlb::Standing &s = table.rows[rank];
    const int y = 14 + row * 6;
    const bool fav = isFavourite(favourites, s.abbr);
    const uint16_t colour = rank < 6 ? kText : kMid;  // the playoff places (three division winners and three wild cards), the rest
    if (fav) box(d, 0, y - 1, W, 7, dim(kFav, 50));
    char number[16];
    snprintf(number, sizeof(number), "%d", rank + 1);
    rightAligned(d, number, 7, y, fav ? kFav : kDim);
    miniText(d, s.abbr, 10, y, fav ? kFav : colour);
    snprintf(number, sizeof(number), "%d-%d", s.wins, s.losses);
    rightAligned(d, number, 47, y, colour);
    if (s.streak[0]) miniText(d, s.streak, 51, y, s.streak[0] == 'W' ? kLive : kRed);
    if (rank == 5) box(d, 0, y + 5, W, 1, dim(kBlue, 90));
  }
  pageDots(d, page, standingsPages(table), 63);
}

// ---- the players of the clock page's extra scenes ----

// A player of 7x12 pixels, feet on row 11: poses stand, run (two steps), slide (12x4, lying back), catch (glove arm up)
enum Pose : uint8_t { Stand, RunA, RunB, Slide, Catch };
void figure(MatrixPanel_I2S_DMA &d, int x, int y, Pose pose, uint16_t cap) {
  static const char *const stand[12] = {"..CCC..", "..HHH..", ".JJJJJ.", ".JJJJJ.", ".JJJJJ.", "..JJJ..", "..PPP..", "..PPP..", "..P.P..", "..P.P..", "..P.P..", ".SS.SS."};
  static const char *const runA[12] = {"..CCC..", "..HHH..", ".JJJJJ.", "JJJJJJ.", ".JJJJJJ", "..JJJ..", "..PPP..", ".PPPPP.", "PP...PP", "P.....P", "S.....S", "SS...SS"};
  static const char *const runB[12] = {"..CCC..", "..HHH..", ".JJJJJ.", ".JJJJJJ", "JJJJJJ.", "..JJJ..", "..PPP..", "..PPP..", ".PP.PP.", ".P...P.", ".P...P.", "SS...SS"};
  static const char *const slide[4] = {"..CHH.........", ".JJJJJ........", "JJJJJJPPPPPPSS", "......PPPPSS.."};
  static const char *const grab[12] = {"..CCC.G", "..HHH.G", ".JJJJJJ", ".JJJJJ.", ".JJJJJ.", "..JJJ..", "..PPP..", "..PPP..", "..P.P..", "..P.P..", "..P.P..", ".SS.SS."};
  const char *const *rows = pose == Stand ? stand : pose == RunA ? runA : pose == RunB ? runB : pose == Slide ? slide : grab;
  static const uint8_t tall[16] = {0, 1, 2, 3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11};  // the standing and running poses stretched from 12 to 16 rows
  const int height = pose == Slide ? 4 : 16, width = pose == Slide ? 14 : 7;
  for (int r = 0; r < height; r++)
    for (int c = 0; c < width; c++) {
      const char k = pose == Slide ? rows[r][c] : rows[tall[r]][c];
      if (k == '.') continue;
      const uint16_t colour = k == 'C' ? cap : k == 'H' ? rgb(220, 170, 120) : k == 'J' ? rgb(225, 225, 225) : k == 'P' ? rgb(170, 170, 180) : k == 'G' ? rgb(125, 68, 28) : rgb(60, 50, 40);
      put(d, x + c, pose == Slide ? y + 12 + r : y + r, colour);
    }
}

// A dust puff of a few pixels
void dust(MatrixPanel_I2S_DMA &d, int x, int y, float u) {
  for (int i = 0; i < 6; i++) put(d, x + (int)lroundf((i - 2.5f) * (1 + 3 * u)), y - (int)lroundf(2 * u * (1 + i % 3)), dim(rgb(150, 120, 80), 255 - (int)(200 * u)));
}

// ---- scenes with only the ball ----

// Height over the grass s ms after the first touch: every bounce is lower and quicker
float bounceHeight(float s, float h, float period) {
  while (s > period && period > 40) { s -= period; h *= 0.55f; period *= 0.74f; }
  if (s > period) return 0.0f;
  const float u = s / period;
  return h * 4 * u * (1 - u);
}

// The ball's shadow on the grass: dithered, narrower the higher the ball is, gone when it is high
void ballShadow(MatrixPanel_I2S_DMA &d, float x, float y) {
  const int h = (int)(60 - y), half = h < 3 ? 2 : h < 8 ? 1 : h < 14 ? 0 : -1, cx = (int)lroundf(x);
  for (int sx = -half; sx <= half; sx++)
    if (((cx + sx) & 1) == 0) put(d, cx + sx, 62, rgb(2, 12, 4));
}

enum Scene : uint8_t { Stands, Steal, Ground, Fast, PopFly, Curve, Bounce, Scenes };

// The cheering crowd round the spot where the ball landed, drawn before the clock text so the letters stay on top
void crowdFlash(MatrixPanel_I2S_DMA &d, uint32_t t) {
  for (int hit = 0; hit < 2; hit++) {
    const uint32_t at = 2200 + hit * 6200, flight = 1500;
    const float landX = hit ? 40.0f : 54.0f, landY = hit ? 30.0f : 33.0f;
    if (t >= at + flight && t < at + flight + 1600) {
      const float k = (t - at - flight) / 1600.0f;
      for (int y = ((int)landY - 9) & ~1; y <= landY + 9; y += 2)
        for (int x = (int)landX - 9 + (y / 2) % 2; x <= landX + 9; x += 2) {
          const float dist = hypotf(x - landX, y - landY);
          if (dist > 9 || x < 0 || x >= W || y < 2 || y > 53) continue;
          uint32_t h = (x * 73856093u) ^ (y * 19349663u);
          h = (h ^ (h >> 13)) * 1274126177u;
          static const uint16_t fans[4] = {rgb(255, 200, 40), rgb(230, 60, 60), rgb(70, 140, 235), rgb(230, 230, 230)};
          const int jump = (h & 1) && ((t / 130 + h) % 2) ? 1 : 0;
          put(d, x, y - jump, dim(fans[(h >> 8) % 4], (int)((1 - k) * (240 - dist * 14))));
        }
    }
  }
}


void ballScene(MatrixPanel_I2S_DMA &d, Scene scene, uint32_t t, uint32_t seed) {
  constexpr float kFloor = 60;
  if (scene == Stands) {  // hit high from the bottom left into the crowd on the right, which lights up and jumps
    for (int hit = 0; hit < 2; hit++) {
      const uint32_t at = 2200 + hit * 6200, flight = 1500;
      const float landX = hit ? 40.0f : 54.0f, landY = hit ? 30.0f : 33.0f;
      if (t >= at && t < at + flight) {
        const float u = (t - at) / (float)flight;
        ballSmooth(d, 4 + (landX - 4) * u, 56 + (landY - 56) * u - 4.0f * 20 * u * (1 - u), u * 22, 1.5f - 1.32f * u);  // it gets smaller as it flies away into the stands
      }
    }
  } else if (scene == Ground) {  // in from the left with big hops that get lower, rolling on out to the right
    if (t >= 2000 && t < 12000) {
      const float s = (float)(t - 2000), x = -5 + 75 * (s / 10000.0f), y = kFloor - bounceHeight(s, 17, 900);
      ballShadow(d, x, y);
      ballSmooth(d, x, y, x * 0.6f);
    }
  } else if (scene == PopFly) {  // straight up out of the top of the frame, back down, bounces and rolls off
    constexpr uint32_t up = 3000, air = 3200;
    float x = 30, y = kFloor, spin = t * 0.01f;
    bool visible = t >= up;
    if (t >= up && t < up + air) {
      const float u = (t - up) / (float)air;
      y = kFloor - 4.0f * 112 * u * (1 - u);
      x = 30 + 4 * u;
    } else if (t >= up + air) {
      const float s = (float)(t - up - air);
      x = 34 + s * 0.012f;
      y = kFloor - bounceHeight(s, 16, 700);
      if (x > 70) visible = false;
    }
    if (visible) { ballShadow(d, x, y); ballSmooth(d, x, y, spin); }
  } else if (scene == Fast) {  // a fast ball with a streak behind it, 90 to 103 mph, different every time
    for (int pitch = 0; pitch < 2; pitch++) {
      uint32_t h = (seed * 2654435761u) ^ ((pitch + 1) * 40503u);
      h = (h ^ (h >> 15)) * 2246822519u;
      const int mph = 90 + (int)((h >> 8) % 14);
      const uint32_t at = 3200 + pitch * 6000, flight = 44100 / mph;  // 98 mph is 450 ms
      if (t >= at && t < at + flight) {
        const float x = -5 + 76 * (t - at) / (float)flight;
        for (int k = 3; k < 22; k++) {
          const int sx = (int)x - k;
          if (sx >= 0 && sx < W) for (int dy = -1; dy <= 1; dy++) if (dy == 0 || k < 12) put(d, sx, 48 + dy, dim(kText, 150 - k * 6));
        }
        ballSmooth(d, x, 48, x * 1.3f);
      }
    }
  } else if (scene == Curve) {  // flat and then dropping into the grass
    for (int pitch = 0; pitch < 2; pitch++) {
      const uint32_t at = 2600 + pitch * 6200, flight = 1900;
      if (t >= at && t < at + flight + 1800) {
        float x, y;
        if (t < at + flight) {
          const float u = (t - at) / (float)flight;
          x = -5 + 65 * u;
          y = 40 - 8 * u + 26 * u * u * u;
          for (int k = 1; k <= 5; k++) {  // a faint trail of dots
            const float uu = u - k * 0.04f;
            if (uu > 0) put(d, (int)(-5 + 65 * uu), (int)lroundf(40 - 8 * uu + 26 * uu * uu * uu), dim(kText, 110 - k * 18));
          }
        } else {
          const float s = (float)(t - at - flight);
          x = 60 + s * 0.012f;
          y = kFloor - bounceHeight(s, 9, 520);
        }
        if (x < 70) { if (y > 50) ballShadow(d, x, y); ballSmooth(d, x, y, x * 0.5f); }
      }
    }
  } else {  // Bounce: falls from the top and bounces, lower every time, until it lies still
    const uint32_t fall = 1000, at = 1500;
    float x = 46, y = -6;
    bool visible = t >= at;
    if (t >= at && t < at + fall) {
      const float u = (t - at) / (float)fall;
      y = -6 + (kFloor + 6) * u * u;
    } else if (t >= at + fall) {
      y = kFloor - bounceHeight((float)(t - at - fall), 34, 850);
    }
    gBallFade = t > 13200 ? (uint8_t)(255 * (14000 - fminf(t, 14000)) / 800) : 255;  // fades away at the end of the round
    if (visible) { ballShadow(d, x, y); ballSmooth(d, x, y, 0.4f); }
    gBallFade = 255;
  }
}

// The scenes of the clock page; t is ms into the 14 s round. The ball scenes have no people, the people scenes no ball
void extraScene(MatrixPanel_I2S_DMA &d, Scene scene, uint32_t t, uint32_t seed) {
  const uint16_t cap = rgb(50, 100, 215);
  constexpr int kPlayerY = 47;  // the top of a standing player: the feet are on the grass
  if (scene != Steal) {
    ballScene(d, scene, t, seed);
  } else if (scene == Steal) {
    constexpr uint32_t kGo = 3000, kRun = 1500, kSlide = 2000;
    float x = -20;
    Pose pose = Stand;
    if (t >= kGo && t < kGo + kRun) {
      x = -8 + 44.0f * (t - kGo) / kRun;
      pose = ((t - kGo) / 140) % 2 ? RunA : RunB;
    } else if (t >= kGo + kRun && t < kGo + kRun + kSlide) {  // slides on, slowing a little, and out of the frame on the right
      const float u = (t - kGo - kRun) / (float)kSlide;
      x = 36 + 42 * (1 - (1 - u) * (1 - u));
      pose = Slide;
      dust(d, (int)x + 14, 57, fmodf(u * 4, 1.0f));
    }
    if (x > -14 && x < 64) figure(d, (int)lroundf(x), kPlayerY, pose, cap);
  }
}

// The next game on one line over the field, centred: both teams in their colours, the day and the first pitch time ("NYY-LAD FRI 20:05")
void drawNextGame(MatrixPanel_I2S_DMA &d, ClockTime *time, const mlb::Fixture &f) {
  // on the ribbon board, centred: both teams in their colours, cream day and time: "MIL-TB TUE 00:40" (the day is left out for a game today)
  char when[8], text[24];
  firstPitch(time, (int64_t)f.start, when, sizeof(when));
  const bool today = dayOf(time, f) == (int)(time->localNow() / 86400);
  if (today) snprintf(text, sizeof(text), "%s-%s %s", f.away, f.home, when);
  else snprintf(text, sizeof(text), "%s-%s %s %s", f.away, f.home, dayName(time, f), when);
  const int x = (W - textWidth(text)) / 2, cream = rgb(255, 240, 205);
  text3(d, text, x, 49, cream);
  text3(d, f.away, x, 49, teamColour(f.away));
  text3(d, f.home, x + 4 * (strlen(f.away) + 1), 49, teamColour(f.home));
}

void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const mlb::Fixture *next, uint8_t hoursColour, uint8_t minutesColour) {
  {  // mark what the clock draws, so the crowd stays one pixel clear of it
    static const char *const monthNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    struct tm local;
    time_t now = (time_t)time->localNow();
    gmtime_r(&now, &local);
    char date[12], seconds[4], hh[4], mm[4];
    snprintf(date, sizeof(date), "%d %s", local.tm_mday, monthNames[local.tm_mon % 12]);
    snprintf(seconds, sizeof(seconds), "%02d", time->getSecond());
    snprintf(hh, sizeof(hh), "%02d", time->getHour());
    snprintf(mm, sizeof(mm), "%02d", time->getMinute());
    inkClear();
    inkText(date, 2, 2);
    inkText(seconds, W - 2 - textWidth(seconds) + 1, 2);
    static const int inkX[4] = {1, 16, 34, 49};
    const char inkDigits[4] = {hh[0], hh[1], mm[0], mm[1]};
    for (int i = 0; i < 4; i++) inkDigit(inkDigits[i] - '0', inkX[i], 10);
  }
  backdrop(d, BgClock, ms);
  gInkValid = false;  // for the pages that draw their own background
  constexpr uint32_t kRoundMs = 14000;
  const uint16_t hoursInk = hoursColour ? pacmancolors::sprite(hoursColour) : kText, minutesInk = minutesColour ? pacmancolors::sprite(minutesColour) : rgb(140, 190, 245);
  {  // the date top left and the seconds top right, in the 3x5 font like the football face
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    struct tm local;
    time_t now = (time_t)time->localNow();
    gmtime_r(&now, &local);
    char date[12], seconds[4];
    snprintf(date, sizeof(date), "%d %s", local.tm_mday, months[local.tm_mon % 12]);
    snprintf(seconds, sizeof(seconds), "%02d", time->getSecond());
    text3(d, date, 2, 2, kPastel);
    text3Right(d, seconds, W - 2, 2, minutesInk);
  }
  if ((Scene)((ms / kRoundMs) % Scenes) == Stands) crowdFlash(d, ms % kRoundMs);
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), "%02d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  const char digits[4] = {hours[0], hours[1], minutes[0], minutes[1]};
  static const int xs[4] = {1, 16, 34, 49};
  for (int i = 0; i < 4; i++) blockDigit(d, digits[i] - '0', xs[i], 10, 14, 21, i < 2 ? hoursInk : minutesInk);
  if (next) drawNextGame(d, time, *next);  // over the field: the ball passes in front of it
  // Seven scenes in turn, 14 seconds each: six with only the ball, one with a runner stealing a base
  extraScene(d, (Scene)((ms / kRoundMs) % Scenes), ms % kRoundMs, ms / kRoundMs);
}

// ---- the run ----

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

void drawRun(MatrixPanel_I2S_DMA &d, const mlb::Game &g, bool home, int runs, uint32_t ms) {
  const float t = ms >= kRunMs ? 1.0f : ms / (float)kRunMs;
  const float flight = 0.5f, rolled = 0.62f;  // when the ball is over the fence, and when the new score has rolled in
  const uint16_t colour = home ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
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
  // the ball: hit from home plate, a long arc (higher for more runs) over the fence at the bottom, into the stands and out of the frame
  const int apex = runs >= 2 ? 34 : 26;
  if (t < flight) {
    const float u = t / flight;
    ballSmooth(d, 4 + 62 * u, 50 - 22 * u - 4.0f * apex * u * (1 - u), u * 14.0f);
  }
  // sparkles over the fence as it clears it
  if (t > flight - 0.12f && t < 0.85f) {
    const float u = (t - (flight - 0.12f)) / 0.45f;
    for (int i = 0; i < 10; i++) {
      const float a = i * 0.6283f, r = 4 + u * 12;
      const int x = 54 + (int)lroundf(cosf(a) * r), y = 22 + (int)lroundf(sinf(a) * r * 0.8f);
      if (u < 1) put(d, x, y, dim(i % 2 ? kFav : colour, 255 - (int)(u * 200)));
    }
  }
  // +1 / +2 ...
  if (t > 0.45f) {
    const int grow = t > 0.62f ? 0 : 1;  // the first moments a pixel larger
    const int cx = 8, cy = 18;
    box(d, cx - 4 - grow, cy - 1, 9 + 2 * grow, 3, colour);
    box(d, cx - 1, cy - 4 - grow, 3, 9 + 2 * grow, colour);
    blockNumber(d, runs, 16, 10, 10, 16, (int)(ms / 120) % 2 || t > 0.7f ? colour : kText);
  }
  // the team and its new score, the other side small
  const int baseY = 44;
  textThin(d, abbr, 3, baseY + 2, colour);  // the thin 5x9 capitals, centred on the score
  // the score: the old one until the ball is over, then it rolls up into the new one
  if (t < flight || runs <= 0) {
    scoreRows(d, score - runs, W - 3, baseY - 2, 0, dim(kText, 120));
  } else if (t < rolled) {
    const int shift = (int)lroundf((t - flight) / (rolled - flight) * 16);
    scoreRows(d, score - runs, W - 3, baseY - 2, shift, dim(kText, 120));
    scoreRows(d, score, W - 3, baseY - 2, shift - 16, colour);
  } else {
    scoreRows(d, score, W - 3, baseY - 2, 0, t > 0.7f ? kText : colour);
  }
  char other[16];
  snprintf(other, sizeof(other), "%s %d", home ? g.away : g.home, otherScore);
  miniText(d, other, 4, 58, kDim);
}

}  // namespace mlbview
