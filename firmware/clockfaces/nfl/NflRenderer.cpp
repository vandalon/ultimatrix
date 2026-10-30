// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "NflRenderer.h"

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

namespace nflview {
namespace {

const uint16_t kText = rgb(225, 225, 225), kDim = rgb(100, 100, 112), kMid = rgb(160, 160, 170), kBlue = rgb(140, 190, 245), kGold = rgb(255, 200, 40),
               kLive = rgb(60, 220, 90), kFav = rgb(255, 205, 40), kPastel = rgb(150, 220, 160), kLine = rgb(30, 30, 36), kShadow = rgb(24, 52, 118);

// ---- small helpers ----

int textWidth(const char *s) { return miniWidth(s); }

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

// Letters 5 wide and 6 high (the titles) and 4 wide and 6 high (the names of the line score): one string a glyph, row after row
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
const char *const kName46[26] = {
    ".XX.X..XXXXXX..XX..XX..X",
    "XXX.X..XXXX.X..XX..XXXX.",
    ".XXXX...X...X...X....XXX",
    "XXX.X..XX..XX..XX..XXXX.",
    "XXXXX...XXX.X...X...XXXX",
    "XXXXX...XXX.X...X...X...",
    ".XXXX...X.XXX..XX..X.XXX",
    "X..XX..XXXXXX..XX..XX..X",
    "XXX..X...X...X...X..XXX.",
    "..XX...X...X...XX..X.XX.",
    "X..XX.X.XX..X.X.X..XX..X",
    "X...X...X...X...X...XXXX",
    "X..XXXXXXXXXX..XX..XX..X",
    "X..XXX.XXXXXX.XXX..XX..X",
    ".XX.X..XX..XX..XX..X.XX.",
    "XXX.X..XXXX.X...X...X...",
    ".XX.X..XX..XX.XXX..X.XXX",
    "XXX.X..XXXX.XX..X.X.X..X",
    ".XXXX....XX....X...XXXX.",
    "XXX..X...X...X...X...X..",
    "X..XX..XX..XX..XX..X.XX.",
    "X..XX..XX..XX..X.XX..XX.",
    "X..XX..XX..XXXXXXXXXX..X",
    "X..XX..X.XX..XX.X..XX..X",
    "X..XX..X.XX..X...X...X..",
    "XXXX...X..X..X..X...XXXX",
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
void titleText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour, int maxX = W, int minX = 0) { fontText(d, s, x, y, colour, 0, maxX, minX, kTitle56, 5, 6); }
void nameText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour) { fontText(d, s, x, y, colour, 0, W, 0, kName46, 4, 6); }

// Text and bars cut off above `minY` and from column `maxX` on: the table rows slide under their title and stay clear of the clock
void clipText(MatrixPanel_I2S_DMA &d, const char *s, int x, int y, uint16_t colour, int minY, int maxX, int minX = 0) {
  for (; *s; s++, x += 4) {
    const uint8_t *g = miniGlyph(*s);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if ((g[row] >> (2 - col) & 1) && y + row >= minY && y + row < H && x + col >= minX && x + col < maxX) put(d, x + col, y + row, colour);
  }
}

// The 4x7 digits of the football face's alternative number font; the colon is two dots. Returns the width
int time7Width(const char *s) {
  int w = 0;
  for (; *s; s++) w += *s == ':' ? 2 : 5;
  return w ? w - 1 : 0;
}
void time7(MatrixPanel_I2S_DMA &d, const char *s, int xRight, int y, uint16_t colour) {
  int x = xRight - time7Width(s) + 1;
  for (; *s; s++) {
    if (*s == ':') {
      put(d, x, y + 2, colour);
      put(d, x, y + 4, colour);
      x += 2;
    } else if (*s >= '0' && *s <= '9') {
      for (int row = 0; row < 7; row++)
        for (int col = 0; col < 4; col++)
          if (numericfont::smallPixel(*s - '0', col, row)) put(d, x + col, y + row, colour);
      x += 5;
    }
  }
}

// ---- footballs ----

uint16_t mix565(uint16_t a, uint16_t b, int k) {  // k 0..16 of b
  const int r = ((a >> 11 << 3) * (16 - k) + (b >> 11 << 3) * k) / 16, g = (((a >> 5) & 63) * 4 * (16 - k) + ((b >> 5) & 63) * 4 * k) / 16,
            bl = ((a & 31) * 8 * (16 - k) + (b & 31) * 8 * k) / 16;
  return rgb(r, g, bl);
}

// The small football (9x5) at a position between pixels, turned by `angle` (radians): a rounded oval, every pixel gets the share of it the oval
// covers (4x4 samples), a lighter top and darker belly, the white stripes near the ends and the laces in the middle
void football(MatrixPanel_I2S_DMA &d, float x, float y, float angle, uint8_t fade = 255) {
  const float a = 4.5f, b = 2.4f, ca = cosf(angle), sa = sinf(angle);
  const uint16_t brown = rgb(150, 80, 28), light = rgb(196, 110, 44), dark = rgb(100, 50, 18), white = rgb(238, 238, 238);
  const int x0 = (int)floorf(x), y0 = (int)floorf(y);
  const float fx = x - x0, fy = y - y0;
  for (int py = -7; py <= 7; py++)
    for (int px = -7; px <= 7; px++) {
      if (px * px + py * py > 7.5f * 7.5f) continue;
      int covered = 0, red = 0, green = 0, blue = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const float X = px + (sx + 0.5f) / 4 - 0.5f - fx, Y = py + (sy + 0.5f) / 4 - 0.5f - fy;
          const float u = X * ca + Y * sa, v = -X * sa + Y * ca;
          if (powf(fabsf(u) / a, 1.6f) + powf(fabsf(v) / b, 1.6f) > 1.0f) continue;
          covered++;
          uint16_t c = v < -b * 0.35f ? light : v > b * 0.35f ? dark : brown;
          const float au = fabsf(u), av = fabsf(v);
          if (au >= 0.6f * a && au <= 0.6f * a + 0.9f) c = mix565(c, white, 14);
          if (au <= a * 0.38f && av <= 0.45f) c = white;
          else if (au <= a * 0.38f && av <= b * 0.8f && (int)((u + a) * 1.4f) % 2 == 0) c = mix565(c, white, 11);
          red += (c >> 11) << 3; green += ((c >> 5) & 63) << 2; blue += (c & 31) << 3;
        }
      if (!covered) continue;
      const uint16_t face = rgb(red / covered, green / covered, blue / covered);
      if (covered < 3) continue;  // mostly covered: the full colour (blending towards black would draw a dark edge)
      put(d, x0 + px, y0 + py, covered >= 7 ? (fade >= 255 ? face : dim(face, fade)) : dim(face, 150 * fade / 255));
    }
}

// The big football (15x9) of the touchdown text flash, turned and scaled; centre at x, y
void footballBig(MatrixPanel_I2S_DMA &d, int x, int y, float angle) {
  static const char *const sprite[9] = {".....HHHHH.....", "...HHHHHHHHH...", ".BBBWBBLBBWBBB.", "BBBBWBLBLBWBBBB", "BBBBWLLLLLWBBBB", "BBBBWBLBLBWBBBB", ".BDDWDDDDDWDDB.", "...DDDDDDDDD...", ".....DDDDD....."};
  const float ca = cosf(angle), sa = sinf(angle);
  for (int dy = -9; dy <= 9; dy++)
    for (int dx = -9; dx <= 9; dx++) {
      const int i = (int)lroundf(dx * ca + dy * sa + 7), j = (int)lroundf(-dx * sa + dy * ca + 4);
      if (i < 0 || i >= 15 || j < 0 || j >= 9) continue;
      const char c = sprite[j][i];
      if (c == '.') continue;
      put(d, x + dx, y + dy, c == 'B' ? rgb(150, 80, 28) : c == 'H' ? rgb(196, 110, 44) : c == 'D' ? rgb(100, 50, 18) : rgb(238, 238, 238));
    }
}

// ---- times ----

// The local kick-off time: "20:15" or "8:15P"
void kickoff(ClockTime *time, int64_t start, char *out, size_t size) {
  struct tm local;
  time_t t = time->utcToLocal((time_t)start);
  gmtime_r(&t, &local);
  if (time->is24hFormat()) snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
  else snprintf(out, size, "%d:%02d%c", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min, local.tm_hour < 12 ? 'A' : 'P');
}

const char *dayNameOf(ClockTime *time, int64_t start) {
  static const char *const names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  struct tm local;
  time_t when = time->utcToLocal((time_t)start);
  gmtime_r(&when, &local);
  return names[local.tm_wday];
}

// "Q3", "HALF", "OT", "FINAL", "FINAL OT", "DELAY", or the day and kick-off time
void statusLabel(const nfl::Game &g, ClockTime *time, char *out, size_t size, uint16_t &colour) {
  colour = kDim;
  switch (g.state) {
    case nfl::Live:
      colour = kGold;
      if (g.delayed) snprintf(out, size, "DELAY");
      else if (g.halftime) snprintf(out, size, "HALF");
      else if (g.quarter > 4) snprintf(out, size, "OT");
      else snprintf(out, size, "Q%d", g.quarter);
      break;
    case nfl::Final:
      colour = kMid;
      snprintf(out, size, g.quarter > 4 ? "FINAL OT" : "FINAL");
      break;
    default: {
      char when[8];
      kickoff(time, g.start, when, sizeof(when));
      snprintf(out, size, "%s %s", dayNameOf(time, g.start), when);
    }
  }
}

// The down as the line shows it: "3RD&7", "4TH&1"
void downText(const nfl::Game &g, char *out, size_t size) {
  static const char *const ordinal[5] = {"", "1ST", "2ND", "3RD", "4TH"};
  if (g.down >= 1 && g.down <= 4) snprintf(out, size, "%s&%d", ordinal[g.down], g.distance);
  else out[0] = 0;
}

}  // namespace

bool backgroundByFace = false;
namespace {
bool backgroundOff() { return backgroundByFace; }

// ---- backgrounds ----

// The crowd of the stands: multi-coloured dots (every second pixel of every second row, thin and dim, thinner towards the top); boost lights it up
void crowd(MatrixPanel_I2S_DMA &d, int top, int bottom, int boost = 0) {
  static const uint16_t colours[4] = {rgb(230, 60, 60), rgb(70, 140, 235), rgb(255, 200, 40), rgb(200, 200, 200)};
  for (int y = top; y < bottom; y += 2)
    for (int x = (y / 2) % 2; x < W; x += 2) {
      uint32_t h = (x * 73856093u) ^ (y * 19349663u);
      h = (h ^ (h >> 13)) * 1274126177u;
      if ((h >> 8) % 100 >= 22) continue;
      put(d, x, y, dim(colours[(h >> 16) % 4], 70 + boost));
    }
}

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

// The stadium of the football face, dot for dot: the stands (thinner and dimmer the higher, an aisle every 15 columns), a band of four crowd rows,
// the dark front of the sideline boards, the advertising boards (15 px blocks), a green line and two rows of grass in stripes 12 px wide.
// gaps: the date and the weekday stand clear of the crowd
constexpr int kBandY = 50;  // the next-up page: the boards at the bottom, two rows of grass
uint16_t crowdDot(int x, int y, int level, int chance = 72) {  // black where this pixel has no dot
  static const uint16_t colours[5] = {rgb(120, 30, 30), rgb(30, 60, 130), rgb(130, 120, 40), rgb(110, 110, 118), rgb(30, 100, 60)};
  uint32_t h = (x * 73856093u) ^ (y * 19349663u);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (h >> 8) % 100 < (unsigned)chance ? dim(colours[(h >> 16) % 5], level * 255 / 32) : 0;
}
bool aisleAt(int x) { return x >= 14 && (x + 1) % 15 < 2; }

void stadium(MatrixPanel_I2S_DMA &d, bool gaps, int bandY = kBandY, int grassDim = 32, bool ribbon = false) {
  static const uint16_t ads[4] = {rgb(60, 72, 120), rgb(120, 56, 62), rgb(84, 88, 100), rgb(50, 110, 104)};
  for (int y = 2; y < bandY; y += 2) {
    const int k = 100 * (bandY - y) / (bandY - 2);  // 0 at the band .. 100 at the top
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (aisleAt(x) || inkNear(x, y)) continue;
      const uint16_t c = crowdDot(x, y, 14 - 8 * k / 100, 80 - 30 * k / 100);
      if (c) put(d, x, y, c);
    }
  }
  for (int y = bandY; y <= bandY + 6; y += 2)
    for (int x = (y / 2) % 2; x < W; x += 2) {
      if (aisleAt(x)) continue;
      const uint16_t c = crowdDot(x, y, 12 + 3 * (y - bandY) / 2);
      if (c) put(d, x, y, c);
    }
  box(d, 0, bandY + 8, W, 1, rgb(22, 25, 33));
  if (ribbon) {  // an LED ribbon board instead of the ad boards: black, the next game is written on it; then the green line and 8 rows of grass
    box(d, 0, bandY + 9, W, 7, 0);
    box(d, 0, bandY + 16, W, 1, rgb(60, 130, 70));
    for (int x = 0; x < W; x++) box(d, x, bandY + 17, 1, H - bandY - 17, dim((x / 12) % 2 == 0 ? rgb(14, 70, 28) : rgb(10, 54, 22), grassDim * 255 / 32));
    return;
  }
  for (int x = 0; x < W; x++) box(d, x, bandY + 9, 1, 2, x % 15 == 14 ? dim(ads[x / 15 % 4], 16 * 255 / 32) : ads[x / 15 % 4]);
  box(d, 0, bandY + 11, W, 1, rgb(60, 130, 70));
  for (int x = 0; x < W; x++) box(d, x, bandY + 12, 1, H - bandY - 12, dim((x / 12) % 2 == 0 ? rgb(14, 70, 28) : rgb(10, 54, 22), grassDim * 255 / 32));
}

void paint(MatrixPanel_I2S_DMA &d, Background background, uint32_t) {
  switch (background) {
    case BgClock: stadium(d, true, 39, 14, true); return;
    case BgNext: stadium(d, false); return;
    case BgGame: crowd(d, 2, 47); return;
    default: crowd(d, 2, 63); return;
  }
}
}  // namespace

void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { paint(d, background, ms); }
void backgroundDrawnByFace(bool yes) { backgroundByFace = yes; }

namespace {
void backdrop(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms) { if (!backgroundOff()) drawBackground(d, background, ms); }

// ---- favourites and order ----

// A team's colour for the bar before its name, by ESPN's abbreviation (picked to be readable on black); `fallback` for a team not in the list
uint16_t teamColour(const char *abbr, uint16_t fallback = rgb(120, 120, 120)) {
  struct Team { const char *abbr; uint8_t r, g, b; };
  static const Team teams[] = {
    {"ARI", 225, 60, 80}, {"ATL", 215, 50, 60}, {"BAL", 130, 70, 200}, {"BUF", 60, 120, 235}, {"CAR", 0, 150, 225}, {"CHI", 240, 110, 20},
    {"CIN", 250, 100, 20}, {"CLE", 200, 90, 40}, {"DAL", 100, 140, 200}, {"DEN", 250, 120, 30}, {"DET", 60, 160, 220}, {"GB", 30, 160, 80},
    {"HOU", 200, 40, 60}, {"IND", 60, 120, 230}, {"JAX", 0, 150, 150}, {"KC", 230, 40, 60}, {"LV", 190, 190, 200}, {"LAC", 60, 160, 230},
    {"LAR", 60, 120, 235}, {"MIA", 0, 190, 190}, {"MIN", 140, 80, 210}, {"NE", 70, 120, 225}, {"NO", 210, 170, 90}, {"NYG", 70, 110, 235},
    {"NYJ", 40, 160, 95}, {"PHI", 0, 150, 140}, {"PIT", 250, 190, 0}, {"SF", 220, 60, 50}, {"SEA", 100, 200, 60}, {"TB", 215, 55, 50},
    {"TEN", 80, 160, 235}, {"WSH", 190, 60, 80}, {"WAS", 190, 60, 80}};
  for (const Team &t : teams)
    if (!strcmp(t.abbr, abbr)) return rgb(t.r, t.g, t.b);
  return fallback;
}
}  // namespace

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

bool hasFavourite(const char *favourites, const nfl::Game &game) { return isFavourite(favourites, game.away) || isFavourite(favourites, game.home); }

int orderGames(const nfl::Snapshot &snap, const char *favourites, int *out) {
  int n = 0;
  // favourites first, then live, coming and finished: one pass per rank keeps ESPN's own order inside a rank
  for (int rank = 0; rank < 6; rank++)
    for (int i = 0; i < snap.count; i++) {
      const nfl::Game &g = snap.games[i];
      const int r = (hasFavourite(favourites, g) ? 0 : 3) + (g.state == nfl::Live ? 0 : g.state == nfl::Pre ? 1 : 2);
      if (r == rank) out[n++] = i;
    }
  return n;
}

namespace {
constexpr int kDividerX = 42, kColumnX = 43;  // the column of coming games is a third of the panel
constexpr int kFixtureCap = 32, kGameH = 19, kColumnGames = 3, kColumnTitleY = 1, kColumnGamesTop = 9;  // a game (time, away, home) takes 19 px; the day is the title
constexpr int kRankRows = 6, kRankStep = 6, kRankTop = 21, kSlideMs = 500;  // a table page: the title at kRankTop, a line, then six rows that slide up
constexpr int kBigGameH = 21, kBigGamesTop = 9;  // a game with the big names (19 px: time, two 6 px names) and 2 px clear of the next one

int fixtureCount(const nfl::Snapshot &snap) { return std::min<int>(snap.nextCount, kFixtureCap); }
int dayOf(ClockTime *time, const nfl::Fixture &f) { return (int)(time->utcToLocal((time_t)f.start) / 86400); }

// The coming games as pages of the right column: the day is the title and a page holds games of one day only, three when there are that many;
// the last page of a day is filled up with the games before it
struct ColumnPage { uint8_t first, count; int day; };
constexpr int kMaxColumnPages = 16;

// Quiet days (the playoffs): when no day has more than one game the column shows two days a page, each its day and its game
bool quietDays(const nfl::Snapshot &snap, ClockTime *time) {
  const int n = fixtureCount(snap);
  for (int i = 1; i < n; i++)
    if (dayOf(time, snap.next[i]) == dayOf(time, snap.next[i - 1])) return false;
  return n > 0;
}

int layoutColumn(const nfl::Snapshot &snap, ClockTime *time, ColumnPage *pages) {
  const int n = fixtureCount(snap);
  int count = 0, i = 0;
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

int rankPagesOf(const nfl::Table &table) { return table.count ? (table.count + kRankRows - 1) / kRankRows : 0; }
int rankPagesTotal(const nfl::Snapshot &snap) { return rankPagesOf(snap.afc) + rankPagesOf(snap.nfc); }
uint32_t columnMs(uint32_t groupMs) { return std::min<uint32_t>(groupMs, 5000); }  // the coming games move on faster than the table: every 5 seconds

// The page lasts until both lists have been through once (the longer of the two), at most 65 seconds
uint32_t pageLength(const nfl::Snapshot &snap, ClockTime *time, uint32_t groupMs) {
  ColumnPage pages[kMaxColumnPages];
  const uint32_t column = layoutColumn(snap, time, pages) * columnMs(groupMs);
  return std::min<uint32_t>(65000, std::max<uint32_t>(std::max<uint32_t>(column, rankPagesTotal(snap) * groupMs), groupMs));
}
}  // namespace

uint16_t upcomingMs(const nfl::Snapshot &snap, ClockTime *time, uint16_t pageMs) { return (uint16_t)pageLength(snap, time, pageMs); }

int standingsPages(const nfl::Table &table) { return table.count ? (table.count + kStandingsPerPage - 1) / kStandingsPerPage : 0; }

// ---- pages ----

namespace {
// The timeouts left as 2x2 dots in the team's colour (a used one is dim)
void timeouts(MatrixPanel_I2S_DMA &d, int x, int y, int left, uint16_t colour) {
  for (int k = 0; k < 3; k++) box(d, x + k * 3, y, 2, 2, k < left ? colour : dim(kDim, 90));
}

// The field strip: end zones in the team colours (dim), yard lines, the red zone, the line of scrimmage, the first down line and the ball
// The empty field behind the pages where the ball is not in play (halftime, delay, final, kick-off): end zones, faint yard lines, down to the bottom edge
void plainField(MatrixPanel_I2S_DMA &d, const nfl::Game &g) {
  constexpr int kTop = 47, kHeight = H - kTop;
  box(d, 0, kTop, W, kHeight, rgb(8, 40, 14));
  box(d, 0, kTop, 6, kHeight, dim(teamColour(g.away, g.awayColor), 70));
  box(d, 58, kTop, 6, kHeight, dim(teamColour(g.home, g.homeColor), 70));
  for (int x = 6; x < 58; x += 5) box(d, x, kTop, 1, kHeight, rgb(14, 64, 24));
}

void fieldStrip(MatrixPanel_I2S_DMA &d, const nfl::Game &g, uint32_t ms) {
  constexpr int kTop = 47, kHeight = 11;
  box(d, 0, kTop, W, kHeight, rgb(8, 40, 14));
  box(d, 0, kTop, 6, kHeight, dim(teamColour(g.away, g.awayColor), 70));
  box(d, 58, kTop, 6, kHeight, dim(teamColour(g.home, g.homeColor), 70));
  for (int x = 6; x < 58; x += 5) box(d, x, kTop, 1, kHeight, rgb(14, 64, 24));
  if (g.redZone && g.possession) {
    if (g.possession == 1) box(d, 46, kTop, 12, kHeight, rgb(60, 30, 10));
    else box(d, 6, kTop, 12, kHeight, rgb(60, 30, 10));
  }
  if (!g.possession) return;
  const int los = 6 + g.fieldPos * 52 / 100;
  const int dir = g.possession == 1 ? 1 : -1;
  const int first = los + dir * ((g.distance * 52 + 50) / 100);
  if (g.down && first > 5 && first < 58) box(d, first, kTop, 1, kHeight, rgb(80, 180, 255));
  box(d, los, kTop, 1, kHeight, kGold);
  football(d, los + dir * 1.0f, kTop + 5.0f, 0.0f, 255);
  (void)ms;
}
}  // namespace

void drawGame(MatrixPanel_I2S_DMA &d, const nfl::Game &g, ClockTime *time, uint32_t ms, bool flashAway, bool flashHome, bool favourite) {
  backdrop(d, BgGame, ms);
  char label[16];
  uint16_t colour;
  statusLabel(g, time, label, sizeof(label), colour);
  if (g.state == nfl::Live && !g.halftime && !g.delayed) {
    text3(d, label, 2, 1, kGold);
    time7(d, g.clock, W - 2, 1, kText);
  } else {
    text3(d, label, 2, 2, colour == kDim ? kMid : colour);
  }
  if (favourite) box(d, 0, 0, 1, 8, kFav);
  box(d, 0, 8, W, 1, kLine);
  const bool away = g.awayScore > g.homeScore, home = g.homeScore > g.awayScore;
  for (int side = 0; side < 2; side++) {
    const bool isHome = side == 1;
    const int y = 11 + side * 17;
    const uint16_t team = isHome ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
    const int score = isHome ? g.homeScore : g.awayScore;
    const bool flash = isHome ? flashHome : flashAway;
    const bool leads = isHome ? home : away;
    if (flash) box(d, 3, y, W - 3, 14, dim(team, 60 + (int)(40 * sinf(ms * 0.02f))));
    box(d, 1, y, 2, 14, team);
    titleText(d, isHome ? g.home : g.away, 5, y + 4, g.state == nfl::Pre || leads || flash ? kText : kMid);
    if (g.state == nfl::Pre) {
      text3Right(d, isHome ? g.homeRecord : g.awayRecord, W - 2, y + 5, kDim);
    } else {
      const int width = blockNumber(d, score, 0, 0, 8, 12, 0, true);
      blockNumber(d, score, W - 2 - width, y + 1, 8, 12, flash ? kText : leads || g.state == nfl::Live ? kText : kDim);
      if (g.state == nfl::Live && g.possession == (isHome ? 2 : 1)) football(d, 26.0f, y + 8.0f, 0.0f);
    }
  }
  if (g.state == nfl::Live && !g.delayed && !g.halftime) {
    fieldStrip(d, g, ms);
    char down[12];
    downText(g, down, sizeof(down));
    text3(d, down, 2, 59, kText);
    timeouts(d, 36, 60, g.awayTimeouts, teamColour(g.away, g.awayColor));
    timeouts(d, 50, 60, g.homeTimeouts, teamColour(g.home, g.homeColor));
  } else {
    // not in play: the empty field, and what the game is on it
    plainField(d, g);
    box(d, 0, 47, W, 1, kLine);
    char text[16];
    if (g.state == nfl::Final) snprintf(text, sizeof(text), g.quarter > 4 ? "FINAL OT" : "FINAL");
    else if (g.state == nfl::Live) snprintf(text, sizeof(text), g.halftime ? "HALFTIME" : "DELAY");
    else kickoff(time, g.start, text, sizeof(text));
    const int width = g.state == nfl::Pre ? time7Width(text) : miniWidth(text);
    if (g.state == nfl::Pre) time7(d, text, (W + width) / 2 - 1, 51, kMid);
    else text3(d, text, (W - width) / 2, 52, g.state == nfl::Final ? kMid : kGold);
  }
}

void drawLineScore(MatrixPanel_I2S_DMA &d, const nfl::Game &g, ClockTime *time, uint32_t ms) {
  backdrop(d, BgLine, ms);
  char label[16];
  uint16_t colour;
  statusLabel(g, time, label, sizeof(label), colour);
  text3(d, label, 2, 2, colour == kDim ? kMid : colour);
  if (g.state == nfl::Live && !g.halftime && !g.delayed) text3Right(d, g.clock, W - 2, 2, kText);
  box(d, 0, 8, W, 1, kLine);
  static const int rightEdges[4] = {25, 35, 45, 54};
  const int periods = std::max<int>(g.periods, g.state == nfl::Live ? g.quarter : 0);
  const int first = std::max(0, periods - 4);  // the latest four periods (an overtime pushes the first quarter out)
  for (int i = 0; i < 4; i++) {
    const int period = first + i + 1;
    char head[3];
    if (period > 4) snprintf(head, sizeof(head), "O");
    else snprintf(head, sizeof(head), "%d", period);
    text3Right(d, head, rightEdges[i] - 2, 12, g.state == nfl::Live && period == g.quarter ? kGold : kDim);
  }
  text3(d, "T", 60, 12, kDim);
  for (int side = 0; side < 2; side++) {
    const bool isHome = side == 1;
    const int y = 21 + side * 12;
    const uint16_t team = isHome ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
    const uint8_t *points = isHome ? g.homePoints : g.awayPoints;
    const int score = isHome ? g.homeScore : g.awayScore;
    box(d, 1, y, 1, 7, team);
    nameText(d, isHome ? g.home : g.away, 4, y, kText);
    for (int i = 0; i < 4; i++) {
      const int n = first + i;
      if (n >= g.periods && !(g.state == nfl::Live && n + 1 == g.quarter)) continue;
      char one[6];
      snprintf(one, sizeof(one), "%d", n < g.periods ? points[n] : 0);
      text3Right(d, one, rightEdges[i], y + 1, n < g.periods && points[n] ? kText : kDim);
    }
    char total[6];
    snprintf(total, sizeof(total), "%d", score);
    text3Right(d, total, W - 1, y + 1, kGold);
  }
  box(d, 0, 47, W, 1, kLine);
  if (g.state == nfl::Live && !g.halftime && !g.delayed) {
    char down[12], line[24];
    downText(g, down, sizeof(down));
    snprintf(line, sizeof(line), "%s %s", down, g.spot);
    text3(d, line, 2, 50, kMid);
    text3(d, "TIMEOUTS", 2, 58, kDim);
    timeouts(d, 36, 59, g.awayTimeouts, teamColour(g.away, g.awayColor));
    timeouts(d, 50, 59, g.homeTimeouts, teamColour(g.home, g.homeColor));
  } else if (g.state == nfl::Pre) {
    char text[8];
    kickoff(time, g.start, text, sizeof(text));
    time7(d, text, (W + time7Width(text)) / 2 - 1, 52, kMid);
  }
}

namespace {
// The first team of a table page: the last page is filled up with the teams before it, so it always has six rows
int rankFirst(const nfl::Snapshot &snap, int index) {
  const int afcPages = rankPagesOf(snap.afc);
  const bool afc = index < afcPages;
  const int count = afc ? snap.afc.count : snap.nfc.count;
  return std::min((afc ? index : index - afcPages) * kRankRows, std::max(0, count - kRankRows));
}
bool rankAfc(const nfl::Snapshot &snap, int index) { return index < rankPagesOf(snap.afc); }

// ".857" is too wide with the record, so the table of the next-up page shows the winning percentage: digits only, the dot is added when it is drawn
void percentage(const nfl::Standing &row, char *out, size_t size) {
  const int games = row.wins + row.losses + row.ties;
  const int per = games ? ((row.wins * 2 + row.ties) * 1000 + games) / (games * 2) : 0;
  snprintf(out, size, "%03d", per > 999 ? 999 : per);
}

// The six rows of one page of a conference table, the first at y0; what is above `minY` is not drawn
void rankRows(MatrixPanel_I2S_DMA &d, const nfl::Snapshot &snap, int index, int y0, const char *favourites, int minY) {
  const nfl::Table &table = rankAfc(snap, index) ? snap.afc : snap.nfc;
  const int first = rankFirst(snap, index);
  for (int r = 0; r < kRankRows && first + r < table.count; r++) {
    const int y = y0 + r * kRankStep;
    if (y + 5 <= minY || y >= H) continue;
    const nfl::Standing &row = table.rows[first + r];
    char text[12];
    snprintf(text, sizeof(text), "%d", first + r + 1);
    clipText(d, text, (7 - textWidth(text)) / 2, y, kBlue, minY, kDividerX);
    const int shift = first + r + 1 >= 10 ? 1 : 0;  // from number 10 the rank is two digits wide: the bar and the name start a pixel further right
    for (int k = 0; k < 5; k++) if (y + k >= minY && y + k < H) put(d, 7 + shift, y + k, teamColour(row.abbr));
    char pct[8];
    percentage(row, pct, sizeof(pct));
    const int left = kDividerX - 2 - textWidth(pct) + 1;
    clipText(d, row.abbr, 10 + shift, y, isFavourite(favourites, row.abbr) ? kFav : kText, minY, kDividerX);
    clipText(d, pct, left, y, kMid, minY, kDividerX);
    if (y + 4 >= minY && y + 4 < H) put(d, left - 2, y + 4, kMid);
  }
}

// One coming game at y: the time, then the away team, then the home team; what is above `minY` is not drawn (the games slide up under the day)
void columnTime(MatrixPanel_I2S_DMA &d, const nfl::Fixture &f, ClockTime *time, int y, int minY) {
  char text[8];
  kickoff(time, f.start, text, sizeof(text));
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
}

void columnGame(MatrixPanel_I2S_DMA &d, const nfl::Fixture &f, ClockTime *time, int y, const char *favourites, int minY, bool big) {
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
    if (y + 6 + r >= minY && y + 6 + r < H) put(d, kColumnX + 1, y + 6 + r, teamColour(f.away));
  for (int r = 0; r < 5; r++)
    if (y + 12 + r >= minY && y + 12 + r < H) put(d, kColumnX + 1, y + 12 + r, teamColour(f.home));
  clipText(d, f.away, kColumnX + 4, y + 6, isFavourite(favourites, f.away) ? kFav : kText, minY, W);
  clipText(d, f.home, kColumnX + 4, y + 12, isFavourite(favourites, f.home) ? kFav : kText, minY, W);
}

// The four big digits of the time at the top left of the next-up page
void rankClock(MatrixPanel_I2S_DMA &d, ClockTime *time) {
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), time->is24hFormat() ? "%02d" : "%d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  const char digits[4] = {hours[1] ? hours[0] : ' ', hours[1] ? hours[1] : hours[0], minutes[0], minutes[1]};
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < 4; i++) {
      if (digits[i] == ' ') continue;
      blockDigit(d, digits[i] - '0', 1 + i * 9 + (i >= 2 ? 4 : 0) + (pass ? 0 : 1), 1 + (pass ? 0 : 1), 8, 14, pass ? kText : kShadow);
    }
}
}  // namespace

void drawUpcoming(MatrixPanel_I2S_DMA &d, const nfl::Snapshot &snap, const char *favourites, ClockTime *time, uint32_t ms, uint32_t elapsed, uint32_t groupMs) {
  backdrop(d, BgNext, ms);
  rankClock(d, time);
  box(d, kDividerX, 0, 1, H, rgb(36, 36, 40));
  // The coming games move on every 5 seconds, the table by a page every groupMs; both slide up and the first come round again after the last
  const uint32_t columnStep = columnMs(groupMs);
  const int kc = elapsed / columnStep, kt = elapsed / groupMs;
  const uint32_t inColumn = elapsed - kc * columnStep, inTable = elapsed - kt * groupMs;
  const float uc = inColumn < (uint32_t)kSlideMs ? inColumn / (float)kSlideMs : 1.0f, easeC = uc * uc * (3 - 2 * uc);
  const float ut = inTable < (uint32_t)kSlideMs ? inTable / (float)kSlideMs : 1.0f, easeT = ut * ut * (3 - 2 * ut);
  ColumnPage column[kMaxColumnPages];
  const int columnPages = layoutColumn(snap, time, column);
  if (!columnPages) {
    miniText(d, "NO", kColumnX + 2, 26, kDim);
    miniText(d, "GAMES", kColumnX + 2, 33, kDim);
  } else if (quietDays(snap, time)) {  // two days a page, the whole page slides up
    const int now = kc % columnPages, was = (kc + columnPages - 1) % columnPages;
    const bool sliding = inColumn < (uint32_t)kSlideMs && columnPages > 1;
    const int distance = 52, shift = sliding ? (int)lroundf(distance * easeC) : 0;
    auto block = [&](const ColumnPage &p, int y0) {
      for (int i = 0; i < p.count; i++) {
        const nfl::Fixture &f = snap.next[p.first + i];
        clipText(d, dayNameOf(time, f.start), kColumnX + 1, y0 + i * 26 + 1, kPastel, 0, W, kColumnX + 1);
        columnGame(d, f, time, y0 + i * 26 + 8, favourites, 0, false);
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
    const bool bigNow = cur.count <= 2;  // few games on the page: the big names
    const int gameH = bigNow ? kBigGameH : kGameH, top = bigNow ? kBigGamesTop : kColumnGamesTop, distance = added * gameH;
    if (dayChanges && inColumn < 200) {
      const float p = inColumn / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf(21 * e);
      titleText(d, dayNameOf(time, snap.next[old.first].start), kColumnX + 1 - offset, kColumnTitleY, kPastel, W, kColumnX + 1);
      titleText(d, dayNameOf(time, snap.next[cur.first].start), kColumnX + 1 + 21 - offset, kColumnTitleY, kPastel, W, kColumnX + 1);
    } else {
      titleText(d, dayNameOf(time, snap.next[cur.first].start), kColumnX + 1, kColumnTitleY, kPastel, W, kColumnX + 1);
    }
    if (slidingGames) {
      const int shift = (int)lroundf(distance * easeC);
      for (int g = 0; g < old.count; g++) columnGame(d, snap.next[old.first + g], time, top + g * gameH - shift, favourites, top, old.count <= 2);
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, top + distance - shift + g * gameH, favourites, top, bigNow);
    } else {
      for (int g = 0; g < cur.count; g++) columnGame(d, snap.next[cur.first + g], time, top + g * gameH, favourites, top, bigNow);
    }
  }
  // the conference table: its title (AFC, NFC) stays where it is while the rows slide up under it; the title only slides sideways when it changes
  const int pages = rankPagesTotal(snap);
  if (pages) {
    const int now = kt % pages, was = (kt + pages - 1) % pages;
    const int rowsTop = kRankTop + 7;
    const bool slidingRows = inTable < (uint32_t)kSlideMs && pages > 1;
    const bool changes = slidingRows && rankAfc(snap, now) != rankAfc(snap, was);
    const int added = !changes && rankFirst(snap, now) > rankFirst(snap, was) ? std::min(kRankRows, rankFirst(snap, now) - rankFirst(snap, was)) : kRankRows;
    const int distance = added * kRankStep;
    if (changes && inTable < 200) {
      const float p = inTable / 200.0f, e = p * p * (3 - 2 * p);
      const int offset = (int)lroundf((kDividerX - 2) * e);
      titleText(d, rankAfc(snap, was) ? "AFC" : "NFC", 1 - offset, kRankTop - 2, kPastel, kDividerX);
      titleText(d, rankAfc(snap, now) ? "AFC" : "NFC", 1 + (kDividerX - 2) - offset, kRankTop - 2, kPastel, kDividerX);
    } else {
      titleText(d, rankAfc(snap, now) ? "AFC" : "NFC", 1, kRankTop - 2, kPastel, kDividerX);
    }
    box(d, 0, kRankTop + 5, kDividerX, 1, rgb(24, 20, 24));
    if (slidingRows) {
      const int shift = (int)lroundf(distance * easeT);
      rankRows(d, snap, was, rowsTop - shift, favourites, rowsTop);
      rankRows(d, snap, now, rowsTop + distance - shift, favourites, rowsTop);
    } else {
      rankRows(d, snap, now, rowsTop, favourites, rowsTop);
    }
  }
}

void drawStandings(MatrixPanel_I2S_DMA &d, const nfl::Table &table, bool afc, const char *favourites, int page, ClockTime *time, uint32_t ms) {
  (void)time;
  backdrop(d, afc ? BgAfc : BgNfc, ms);
  titleText(d, afc ? "AFC" : "NFC", 1, 1, kPastel);
  box(d, 0, 8, W, 1, rgb(24, 20, 24));
  const int first = std::min(page * kStandingsPerPage, std::max(0, (int)table.count - kStandingsPerPage));
  for (int row = 0; row < kStandingsPerPage && first + row < table.count; row++) {
    const int rank = first + row;
    const nfl::Standing &s = table.rows[rank];
    const int y = 11 + row * 8;
    const bool fav = isFavourite(favourites, s.abbr);
    const uint16_t colour = rank < 7 ? kText : kMid;  // the seven playoff places, the rest
    char number[16];
    snprintf(number, sizeof(number), "%d", rank + 1);
    text3Right(d, number, 6, y, fav ? kFav : kBlue);
    const int shift = rank + 1 >= 10 ? 1 : 0;
    box(d, 8 + shift, y, 1, 5, teamColour(s.abbr));
    text3(d, s.abbr, 11 + shift, y, fav ? kFav : colour);
    if (s.ties) snprintf(number, sizeof(number), "%d-%d-%d", s.wins, s.losses, s.ties);
    else snprintf(number, sizeof(number), "%d-%d", s.wins, s.losses);
    text3(d, number, 29, y, kMid);
    char pct[8];
    percentage(s, pct, sizeof(pct));
    text3(d, ".", 47, y, kMid);
    text3(d, pct, 49, y, kMid);
  }
  box(d, 0, 57, W, 1, kLine);
  text3(d, "PLAYOFF 1-7", 2, 59, kDim);
}

// ---- the clock ----

namespace {
// A ball flying on a parabola: the angle follows the direction it moves in
void flight(MatrixPanel_I2S_DMA &d, float x0, float y0, float x1, float y1, float apex, float u, float spin) {
  const float x = x0 + (x1 - x0) * u, y = y0 + (y1 - y0) * u - 4.0f * apex * u * (1 - u);
  const float dx = x1 - x0, dy = (y1 - y0) - 4.0f * apex * (1 - 2 * u);
  football(d, x, y, atan2f(dy, dx) + spin);
}

enum Scene : uint8_t { Pass, Bounce, Punt, Scenes };

void ballScene(MatrixPanel_I2S_DMA &d, Scene scene, uint32_t t) {
  constexpr uint32_t kRoundMs = 12000, kStart = 2500;
  switch (scene) {
    case Pass: {
      if (t < kStart) break;
      const float u = (t - kStart) / 3800.0f;
      if (u < 1.0f) flight(d, -6, 52, 72, 54, 22, u, sinf(u * 60.0f) * 0.25f);
      break;
    }
    case Bounce: {
      // dropped from the top left, bounces along the grass in smaller and smaller jumps, tumbling, and rolls out
      if (t < 1000) break;
      const float s = (t - 1000) / 1000.0f;
      float x = 6 + s * 7.0f, y = 60;
      float h = 40, period = 1.1f, at = s;
      while (at > period && h > 2) { at -= period; h *= 0.55f; period *= 0.78f; }
      const float k = at / period;
      y = 60 - h * 4.0f * k * (1 - k);
      if (x < 70) football(d, x, y, s * 3.1f + sinf(s * 5.0f));
      break;
    }
    case Punt: {  // kicked from outside the frame: it flies in from the left, high, and comes down on the right
      if (t < 1500) break;
      const float u = (t - 1500) / 3600.0f;
      if (u < 1.0f) flight(d, -10, 60, 66, 60, 56, u, 0.0f);
      break;
    }
    default: break;
  }
  (void)kRoundMs;
}

// The next game on one line under the clock, centred: both teams in their colours, the day and the kick-off time ("CAR-GB THU 20:15")
void drawNextGame(MatrixPanel_I2S_DMA &d, ClockTime *time, const nfl::Fixture &f) {
  // on the LED ribbon board, amber, centred: "CAR-GB FRI 01:15" (the day is left out for a game today)
  char when[8], text[24];
  kickoff(time, f.start, when, sizeof(when));
  const bool today = dayOf(time, f) == (int)(time->localNow() / 86400);
  if (today) snprintf(text, sizeof(text), "%s-%s %s", f.away, f.home, when);
  else snprintf(text, sizeof(text), "%s-%s %s %s", f.away, f.home, dayNameOf(time, f.start), when);
  text3(d, text, (W - textWidth(text)) / 2, 49, rgb(255, 170, 30));
}

}  // namespace

void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const nfl::Fixture *next, uint8_t hoursColour, uint8_t minutesColour) {
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
  const uint16_t hoursInk = hoursColour ? pacmancolors::sprite(hoursColour) : kText, minutesInk = minutesColour ? pacmancolors::sprite(minutesColour) : kBlue;
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
  char hours[4], minutes[4];
  snprintf(hours, sizeof(hours), "%02d", time->getHour());
  snprintf(minutes, sizeof(minutes), "%02d", time->getMinute());
  const char digits[4] = {hours[0], hours[1], minutes[0], minutes[1]};
  static const int xs[4] = {1, 16, 34, 49};
  for (int i = 0; i < 4; i++) blockDigit(d, digits[i] - '0', xs[i], 10, 14, 21, i < 2 ? hoursInk : minutesInk);
  if (next) drawNextGame(d, time, *next);  // over the field: the ball passes in front of it
  constexpr uint32_t kRound = 12000;
  ballScene(d, (Scene)((ms / kRound) % Scenes), ms % kRound);
}

// ---- a score ----

void drawScore(MatrixPanel_I2S_DMA &d, const nfl::Game &g, bool home, int points, uint32_t ms) {
  const float t = ms >= kScoreMs ? 1.0f : ms / (float)kScoreMs;
  const uint16_t team = home ? teamColour(g.home, g.homeColor) : teamColour(g.away, g.awayColor);
  const char *abbr = home ? g.home : g.away;
  const int score = home ? g.homeScore : g.awayScore;
  const int beat = ms / 250;
  const uint16_t flash = beat % 2 ? kText : team;
  crowd(d, 2, 60, beat % 2 ? 40 : 110);
  box(d, 0, 0, W, 2, flash);
  box(d, 0, 62, W, 2, flash);
  const char *top = "SCORE", *bottom = nullptr;
  if (points >= 6) { top = "TOUCH"; bottom = "DOWN"; }
  else if (points == 3) { top = "FIELD"; bottom = "GOAL"; }
  else if (points == 2) top = "SAFETY";
  else if (points == 1) { top = "EXTRA"; bottom = "POINT"; }
  auto centre = [](const char *s) { return (W - ((int)strlen(s) * 6 - 1)) / 2; };
  titleText(d, top, centre(top), bottom ? 3 : 7, flash);
  if (bottom) titleText(d, bottom, centre(bottom), 11, flash);
  // the team in double size (the 3x5 letters at 2x) and the new score centred under it
  const int nameWidth = (int)strlen(abbr) * 8 - 2;
  for (int i = 0; abbr[i]; i++) {
    const uint8_t *glyph = miniGlyph(abbr[i]);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (glyph[row] >> (2 - col) & 1) box(d, (W - nameWidth) / 2 + i * 8 + col * 2, 20 + row * 2, 2, 2, team);
  }
  char digits[8];
  snprintf(digits, sizeof(digits), "%d", score);
  const int width = blockNumber(d, score, 0, 0, 10, 16, 0, true);
  blockNumber(d, score, (W - width) / 2, 33, 10, 16, kText);
  // the ball rolls and tumbles along the bottom
  footballBig(d, (int)(8 + 48 * t), 55, -0.5f + t * 9.0f);
}

}  // namespace nflview
