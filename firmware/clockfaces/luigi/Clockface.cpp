// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <MiniFont.h>
#include <PlumberSprites.h>

using namespace facekit;

namespace {

const int kGroundTop = 52;
const int kFeet = 58;                   // the row Luigi's feet are on
const int kLuigiY = kFeet - 17;         // he is 18 rows tall
const int kMinX = 6, kMaxX = 46;
const int kDigitTop = 4, kChangeMs = 1300;
const int kDigitX[4] = {3, 16, 37, 50};

// ---- The digits: strokes drawn with a small round brush, so they look lit rather than blocky ----

struct Stroke { int8_t n; int8_t pts[10][2]; };
const Stroke kStrokes[10][2] = {
  /*0*/ {{5, {{1, 1}, {9, 1}, {9, 17}, {1, 17}, {1, 1}}}, {0, {}}},
  /*1*/ {{3, {{2, 4}, {5, 1}, {5, 17}}}, {2, {{2, 17}, {8, 17}}}},
  /*2*/ {{7, {{1, 3}, {3, 1}, {7, 1}, {9, 3}, {9, 6}, {1, 17}, {9, 17}}}, {0, {}}},
  /*3*/ {{4, {{1, 1}, {9, 1}, {9, 17}, {1, 17}}}, {2, {{3, 9}, {9, 9}}}},
  /*4*/ {{3, {{8, 1}, {1, 12}, {10, 12}}}, {2, {{8, 1}, {8, 17}}}},
  /*5*/ {{8, {{9, 1}, {1, 1}, {1, 8}, {8, 8}, {9, 10}, {9, 15}, {7, 17}, {1, 17}}}, {0, {}}},
  /*6*/ {{10, {{8, 1}, {3, 2}, {1, 6}, {1, 14}, {3, 17}, {7, 17}, {9, 14}, {9, 11}, {7, 9}, {2, 9}}}, {0, {}}},
  /*7*/ {{3, {{1, 1}, {9, 1}, {4, 17}}}, {0, {}}},
  /*8*/ {{5, {{2, 1}, {8, 1}, {8, 9}, {2, 9}, {2, 1}}}, {5, {{1, 9}, {9, 9}, {9, 17}, {1, 17}, {1, 9}}}},
  /*9*/ {{10, {{2, 17}, {7, 16}, {9, 12}, {9, 4}, {7, 1}, {3, 1}, {1, 4}, {1, 7}, {3, 9}, {8, 9}}}, {0, {}}},
};
uint16_t masks[10][19];  // one bit per pixel of an 11 x 19 digit
bool masksBuilt = false;

void stamp(uint16_t (&mask)[19], int cx, int cy) {
  static const int8_t PLUS[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto &p : PLUS) {
    const int x = cx + p[0], y = cy + p[1];
    if (x >= 0 && x < 11 && y >= 0 && y < 19) mask[y] |= 1 << x;
  }
  // A square brush: fill the corners too so the strokes are three pixels wide
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      const int x = cx + dx, y = cy + dy;
      if ((dx || dy) && x >= 0 && x < 11 && y >= 0 && y < 19 && !(dx && dy)) mask[y] |= 1 << x;
    }
}

void line(uint16_t (&mask)[19], int x0, int y0, int x1, int y1) {
  const int steps = max(abs(x1 - x0), abs(y1 - y0));
  for (int i = 0; i <= steps; i++) {
    const int x = steps ? x0 + (x1 - x0) * i / steps : x0, y = steps ? y0 + (y1 - y0) * i / steps : y0;
    stamp(mask, x, y);
  }
}

void buildMasks() {
  for (int digit = 0; digit < 10; digit++)
    for (const Stroke &s : kStrokes[digit])
      for (int i = 0; i + 1 < s.n; i++) line(masks[digit], s.pts[i][0], s.pts[i][1], s.pts[i + 1][0], s.pts[i + 1][1]);
  masksBuilt = true;
}

bool lit(int digit, int x, int y) {
  return digit >= 0 && x >= 0 && x < 11 && y >= 0 && y < 19 && (masks[digit][y] >> x & 1);
}

// A digit with a soft halo, at some brightness (0..255) and lifted by `rise` pixels
void drawDigit(MatrixPanel_I2S_DMA &d, int digit, int x, int y, int level) {
  if (digit < 0 || level <= 0) return;
  const uint16_t core = dim(rgb(120, 255, 150), level), halo = dim(rgb(20, 90, 50), level), rim = dim(rgb(60, 200, 100), level);
  for (int row = -1; row <= 19; row++)
    for (int col = -1; col <= 11; col++) {
      if (lit(digit, col, row)) {
        const bool edge = !lit(digit, col - 1, row) || !lit(digit, col + 1, row) || !lit(digit, col, row - 1) || !lit(digit, col, row + 1);
        put(d, x + col, y + row, edge ? rim : core);
      } else if (lit(digit, col - 1, row) || lit(digit, col + 1, row) || lit(digit, col, row - 1) || lit(digit, col, row + 1)) {
        put(d, x + col, y + row, halo);
      }
    }
}

struct Slot { int8_t digit, from; uint32_t since; };
Slot slots[4];

// ---- Scenery ----

const uint16_t kSkyTop = rgb(6, 6, 30), kSkyBottom = rgb(78, 42, 118);
const uint16_t kHouse = rgb(12, 7, 26), kHouseEdge = rgb(34, 20, 60), kWindow = rgb(255, 205, 80);

void drawSky(MatrixPanel_I2S_DMA &d, int flash) {
  for (int y = 0; y < kGroundTop; y++) {
    uint16_t c = mix(kSkyTop, kSkyBottom, y * 255 / kGroundTop);
    if (flash) c = mix(c, rgb(190, 190, 255), flash);
    box(d, 0, y, W, 1, c);
  }
}

void drawStars(MatrixPanel_I2S_DMA &d, uint32_t now, int flash) {
  if (flash) return;
  for (int i = 0; i < 14; i++) {
    const uint32_t h = hash(i * 977 + 13);
    const int x = h % W, y = (h >> 8) % 30;
    const int level = 60 + triangle((now / 12 + (h >> 16)) % 512) * 150 / 255;
    put(d, x, y, dim(rgb(255, 255, 230), level));
  }
}

void drawMoon(MatrixPanel_I2S_DMA &d) {
  const int cx = 58, cy = 33;
  for (int y = -4; y <= 4; y++)
    for (int x = -4; x <= 4; x++) {
      const int r2 = x * x + y * y;
      if (r2 > 17) continue;
      const bool crater = (x == -1 && y == 0) || (x == 1 && y == 2);
      put(d, cx + x, cy + y, crater ? rgb(180, 180, 160) : (r2 > 11 ? rgb(214, 214, 190) : rgb(238, 238, 212)));
    }
}

void drawMansion(MatrixPanel_I2S_DMA &d, uint32_t now) {
  // Body and two towers
  box(d, 13, 38, 38, 14, kHouse);
  box(d, 12, 30, 10, 22, kHouse);
  box(d, 42, 33, 9, 19, kHouse);
  for (int i = 0; i < 6; i++) {  // pointed roofs
    box(d, 12 + i, 30 - i - 1, 10 - 2 * i, 1, kHouse);
    if (i < 5) box(d, 42 + i, 33 - i - 1, 9 - 2 * i, 1, kHouse);
  }
  box(d, 17, 24, 1, 6, kHouse);                       // a lightning rod on the left tower
  for (int i = 0; i < 4; i++) box(d, 24 + i * 3, 37 - (i % 2), 3, 1, kHouse);  // a jagged roof ridge
  box(d, 13, 38, 38, 1, kHouseEdge);
  box(d, 12, 30, 1, 22, kHouseEdge);
  box(d, 42, 33, 1, 19, kHouseEdge);
  // Windows, a few of them flickering
  struct Win { int8_t x, y; uint8_t id; };
  static const Win WINDOWS[] = {{15, 33, 0}, {18, 40, 1}, {24, 42, 2}, {29, 42, 3}, {38, 42, 4}, {44, 36, 5}, {46, 43, 6}, {20, 46, 7}};
  for (const Win &w : WINDOWS) {
    const uint32_t slot = (now / 700 + w.id * 5) % 40;
    int level = 255;
    if (w.id % 3 == 1 && slot % 11 == 0) level = 70;
    else if (w.id == 5 && slot % 17 < 3) level = 0;
    else if (w.id == 6 && slot > 25) level = 0;
    const uint16_t c = level ? dim(kWindow, level) : kHouseEdge;
    box(d, w.x, w.y, 3, 4, c);
    if (level == 255) box(d, w.x + 1, w.y, 1, 4, rgb(120, 70, 20));
  }
  // The door
  box(d, 31, 46, 5, 6, rgb(70, 36, 14));
  box(d, 32, 46, 3, 1, rgb(100, 56, 20));
  put(d, 34, 49, kWindow);
}

void drawTree(MatrixPanel_I2S_DMA &d) {
  const uint16_t c = rgb(14, 8, 30);
  box(d, 4, 36, 2, 16, c);
  for (int i = 0; i < 7; i++) { put(d, 5 + i / 2, 38 - i, c); put(d, 3 - i / 2, 40 - i, c); }
  for (int i = 0; i < 5; i++) { put(d, 6 + i, 43 - i / 2, c); put(d, 3 - i, 46 - i / 2, c); }
}

void drawGround(MatrixPanel_I2S_DMA &d) {
  for (int y = kGroundTop; y < H; y++)
    for (int x = 0; x < W; x++) {
      const uint32_t h = hash(x * 31 + y * 101);
      uint16_t c = mix(rgb(10, 36, 28), rgb(6, 22, 20), (y - kGroundTop) * 255 / 12);
      if (h % 17 == 0) c = rgb(18, 62, 38);
      put(d, x, y, c);
    }
  // A stone path from the door
  for (int y = kGroundTop; y < H; y++) {
    const int half = 3 + (y - kGroundTop) / 2;
    for (int x = 33 - half; x <= 33 + half; x++)
      put(d, x, y, rgb(38, 34, 56));
  }
}

// A faint mist, a few streaks that drift at different speeds
void drawMist(MatrixPanel_I2S_DMA &d, uint32_t now) {
  for (int i = 0; i < 4; i++) {
    const int y = 46 + i * 3, len = 10 + i * 3;
    const int x = (int)((now / (110 + i * 40) + i * 29) % (W + 2 * len)) - len;
    for (int k = 0; k < len; k++)
      if ((k + i) % 2 == 0) put(d, x + k, y, rgb(46, 38, 82));
  }
}

// ---- Boo ----

const char *const kBoo[11] = {"...wwwww...", "..wwwwwww..", ".wwwwwwwww.", "wwwwwwwwwww", "wwkkwwwkkww", "wwkkwwwkkww", "wwwwwwwwwww",
                              "wkkkkkkkkkw", ".wkrrrrrkw.", "..wwwwwww..", "...wwwww..."};
const char *const kBooShy[11] = {"...wwwww...", "..wwwwwww..", ".wwwwwwwww.", "wwwwwwwwwww", "wgggwwwgggw", "wgggwwwgggw", "wwwwwwwwwww",
                                 "wwwwwwwwwww", ".wwwwwwwww.", "..wwwwwww..", "...wwwww..."};

struct Boo { float x = 50; int8_t dir = -1; bool shy = false; uint32_t shySince = 0; } boo;

// ---- Luigi ----

enum class Mode : uint8_t { Walk, Pause, Scared };
struct Luigi { float x = 20, distance = 0; int8_t dir = 1; Mode mode = Mode::Walk; uint32_t since = 0, pauseMs = 0, lastShine = 0; int32_t scaredMinute = -1; } luigi;
uint32_t lastNow = 0;

void beam(MatrixPanel_I2S_DMA &d, int fx, int fy, int dir, uint32_t now) {
  // A cone of light thrown ahead and a little upward, as a loose dither so the scene shows through
  for (int dx = 2; dx <= 26; dx++)
    for (int dy = -(int)(dx * 0.6f) - 1; dy <= (int)(dx * 0.12f); dy++)
      if ((dx + dy + ((now / 300) % 2 && dx > 20 ? 1 : 0)) % 2 == 0) {
        const int level = 200 - dx * 6;
        put(d, fx + dir * dx, fy + dy, dim(rgb(255, 244, 170), level > 30 ? level : 30));
      }
}

const char *const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// Lightning on the hour: three flashes
int flashLevel(int32_t ms) {
  static const int16_t ON[3][2] = {{500, 640}, {900, 960}, {1320, 1500}};
  for (const auto &w : ON)
    if (ms >= w[0] && ms < w[1]) return 100;
  return 0;
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  if (!masksBuilt) buildMasks();
  const int digits[4] = {dateTime->getHour() / 10, dateTime->getHour() % 10, dateTime->getMinute() / 10, dateTime->getMinute() % 10};
  for (int i = 0; i < 4; i++) slots[i] = {(int8_t)digits[i], (int8_t)digits[i], 0};
  luigi = Luigi();
  boo = Boo();
  luigi.scaredMinute = minuteOfDay(dateTime);
  lastNow = millis();
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  float dt = (now - lastNow) / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastNow = now;
  const int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  const int32_t ms = msOfMinute(_dateTime), mod = minuteOfDay(_dateTime);
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  const bool hideTens = !_dateTime->is24hFormat() && hour < 10;
  for (int i = 0; i < 4; i++)
    if (digits[i] != slots[i].digit) slots[i] = {(int8_t)digits[i], slots[i].digit, now};

  // ---- Luigi ----
  const float booCentre = boo.x + 5;
  const bool booInBeam = luigi.dir > 0 ? booCentre > luigi.x + 14 && booCentre < luigi.x + 40
                                       : booCentre < luigi.x - 1 && booCentre > luigi.x - 27;
  if (minute == 0 && luigi.scaredMinute != mod && ms >= 400) { luigi.scaredMinute = mod; luigi.mode = Mode::Scared; luigi.since = now; }
  switch (luigi.mode) {
    case Mode::Walk:
      luigi.x += luigi.dir * 6.0f * dt; luigi.distance += 6.0f * dt;
      if (luigi.x >= kMaxX) { luigi.x = kMaxX; luigi.dir = -1; luigi.mode = Mode::Pause; luigi.since = now; luigi.pauseMs = 1500 + hash(now / 5000) % 2500; }
      else if (luigi.x <= kMinX) { luigi.x = kMinX; luigi.dir = 1; luigi.mode = Mode::Pause; luigi.since = now; luigi.pauseMs = 1500 + hash(now / 4000) % 2500; }
      else if (booInBeam && now - luigi.lastShine > 9000) { luigi.mode = Mode::Pause; luigi.since = now; luigi.pauseMs = 2400; luigi.lastShine = now; }
      break;
    case Mode::Pause:
      if (now - luigi.since >= luigi.pauseMs) luigi.mode = Mode::Walk;
      break;
    case Mode::Scared:
      if (now - luigi.since >= 3400) { luigi.mode = Mode::Pause; luigi.since = now; luigi.pauseMs = 800; }
      break;
  }
  // Boo drifts along while nobody shines a light on him, and cowers when somebody does
  const bool looked = booInBeam && luigi.mode != Mode::Scared;
  if (looked) { if (!boo.shy) { boo.shy = true; boo.shySince = now; } }
  else if (boo.shy && now - boo.shySince > 1500) boo.shy = false;
  if (!boo.shy) {
    boo.x += boo.dir * 4.5f * dt;
    if (boo.x < -12) boo.dir = 1;
    if (boo.x > 66) boo.dir = -1;
  }

  // ---- Picture ----
  const int flash = minute == 0 ? flashLevel(ms) : 0;
  drawSky(d, flash);
  drawStars(d, now, flash);
  drawMoon(d);
  drawMansion(d, now);
  drawTree(d);
  drawGround(d);

  // The digits: the old one fades upward and away, the new one comes up from below
  for (int i = 0; i < 4; i++) {
    if (i == 0 && hideTens && slots[0].digit == 0) continue;
    const Slot &slot = slots[i];
    const int age = slot.since ? (int)(now - slot.since) : kChangeMs;
    if (age >= kChangeMs) { drawDigit(d, slot.digit, kDigitX[i], kDigitTop, 255); continue; }
    // The old digit drifts up and out, then the new one comes up from below
    const int p = ease(age, kChangeMs);  // 0..1000
    if (p < 500) drawDigit(d, slot.from, kDigitX[i], kDigitTop - 3 * p / 500, 255 - 255 * p / 500);
    else drawDigit(d, slot.digit, kDigitX[i], kDigitTop + 3 - 3 * (p - 500) / 500, 255 * (p - 500) / 500);
  }
  // The colon: two small green flames
  for (int i = 0; i < 2; i++) {
    const int fx = 30, fy = kDigitTop + (i ? 12 : 3);
    const int shape = (hash(now / 140 + i * 5) % 3);
    const uint16_t outer = rgb(20, 140, 70), inner = rgb(190, 255, 190);
    box(d, fx + 1, fy + 1, 2, 4, outer);
    box(d, fx, fy + 2, 4, 2, outer);
    put(d, fx + 1 + (shape == 1), fy, outer);
    box(d, fx + 1, fy + 2, 2, 2, inner);
  }

  // Boo, then the light, then Luigi in front
  {
    const int bx = (int)boo.x, by = 29 + (int)(4 * sinf(now / 2300.0f)) + 4;
    const Swatch pal[] = {{'w', boo.shy ? rgb(150, 150, 190) : rgb(236, 236, 255)}, {'k', rgb(16, 8, 24)}, {'r', rgb(230, 50, 70)}, {'g', rgb(255, 255, 255)}};
    sprite(d, boo.shy ? kBooShy : kBoo, 11, bx, by, pal, 4, boo.dir > 0);
  }
  Pose pose = Pose::Stand;
  int lift = 0, shake = 0;
  if (luigi.mode == Mode::Walk) {
    static const Pose CYCLE[4] = {Pose::Walk1, Pose::Stand, Pose::Walk2, Pose::Stand};
    pose = CYCLE[((int)(luigi.distance / 2.8f)) % 4];
  } else if (luigi.mode == Mode::Scared) {
    const uint32_t t = now - luigi.since;
    if (t < 300) { pose = Pose::Jump; lift = (int)(4 * 4 * (t / 300.0f) * (1 - t / 300.0f) + 0.5f); }
    else shake = (t / 90) % 2 ? 1 : -1;
  }
  const int lx = (int)(luigi.x + 0.5f) + shake;
  const bool left = luigi.dir < 0;
  // The vacuum on his back, the flashlight in front of him
  const int packX = left ? lx + 11 : lx - 2;
  box(d, packX, kLuigiY + 7 - lift, 4, 9, rgb(120, 140, 130));
  box(d, packX + (left ? 0 : 3), kLuigiY + 7 - lift, 1, 9, rgb(70, 86, 84));
  box(d, packX + 1, kLuigiY + 8 - lift, 2, 2, rgb(80, 220, 230));
  if (luigi.mode != Mode::Scared) beam(d, left ? lx - 1 : lx + 14, kLuigiY + 10 - lift, left ? -1 : 1, now);
  drawPlumber(d, lx, kLuigiY - lift, pose, left, LUIGI, true);
  box(d, left ? lx - 2 : lx + 13, kLuigiY + 9 - lift, 3, 2, rgb(150, 150, 160));
  put(d, left ? lx - 2 : lx + 15, kLuigiY + 9 - lift, rgb(255, 250, 200));

  drawMist(d, now);

  // The date, dim
  char text[16];
  snprintf(text, sizeof(text), "%s %d %s", kDays[_dateTime->getWeekday() % 7], _dateTime->getDay(), kMonths[(_dateTime->getMonth() + 11) % 12]);
  box(d, 0, 59, W, 5, rgb(4, 12, 14));
  miniCentered(d, text, 59, rgb(40, 110, 84));

  d.flipDMABuffer();
}
