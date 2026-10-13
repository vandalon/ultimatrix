// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Clockface.h"
#include <MiniFont.h>
#include <PlumberSprites.h>

using namespace facekit;

namespace {

// ---- The two worlds: overground by day, World 1-2 underground at night ----
struct Theme {
  uint16_t sky, brick, brickLight, mortar, goomba, goombaFoot, text;
  bool overground;
};
const Theme kDay = {rgb(92, 148, 252), rgb(200, 76, 12), rgb(252, 152, 56), rgb(40, 8, 0), rgb(184, 100, 20), rgb(252, 188, 120), rgb(255, 255, 255), true};
const Theme kCave = {rgb(0, 0, 0), rgb(0, 84, 176), rgb(60, 140, 232), rgb(0, 12, 44), rgb(72, 120, 200), rgb(176, 200, 240), rgb(120, 160, 230), false};

const int kGround = 52;             // first row of the ground
const int kPlumberY = kGround - 16;
const int kBlockX[4] = {1, 15, 36, 50};
const int kBlockTop = 10, kBlockW = 13, kBlockH = 17;
const int kMinX = 18, kMaxX = 48;    // where Mario strolls
const int kHitX = 50;                // under the last block
const int kHitJumpMs = 520, kHitHeight = 9;

enum class Mode : uint8_t { Walk, Pause, Approach, Wait, Hit, Stomp, Hop };

struct Plumber {
  float x = 30, fromX = 0, vx = 0, distance = 0;
  int8_t dir = 1;
  Mode mode = Mode::Walk;
  uint32_t since = 0;
  uint32_t pauseMs = 0;
  int32_t approachedMinute = -1;
} mario;

struct Goomba {
  float x = 0;
  uint8_t state = 0;  // 0 none, 1 walking, 2 squashed
  uint32_t since = 0;
  int32_t spawnedMinute = -1;
  int16_t score = 0;
} goomba;

struct Block { int8_t shown, target; uint32_t bumpAt; };
Block blocks[4];
uint32_t lastNow = 0;
bool night = false;

// ---- Drawing ----

void drawBricks(MatrixPanel_I2S_DMA &d, int top, int bottom, const Theme &t, bool lit) {
  for (int y = top; y <= bottom; y++) {
    const int row = (y - top) / 6, inRow = (y - top) % 6;
    for (int x = 0; x < W; x++) {
      uint16_t c = t.brick;
      if (inRow == 5) c = t.mortar;
      else if ((x + (row % 2 ? 4 : 0)) % 8 == 7) c = t.mortar;
      else if (lit && y == top) c = t.brickLight;
      put(d, x, y, c);
    }
  }
}

void drawCloud(MatrixPanel_I2S_DMA &d, int x, int y) {
  static const char *const CLOUD[5] = {"....wwww........", "..wwwwwwww.wwww.", ".wwwwwwwwwwwwwww", "wwwwwwwwwwwwwwww", ".cccccccccccccc."};
  static const Swatch PAL[] = {{'w', rgb(255, 255, 255)}, {'c', rgb(196, 222, 255)}};
  sprite(d, CLOUD, 5, x, y, PAL, 2);
}

void drawHill(MatrixPanel_I2S_DMA &d, int left, int base) {
  const int width = 30, height = 15;
  for (int x = 0; x < width; x++) {
    const float u = (x - (width - 1) / 2.0f) / (width / 2.0f);
    const float shape = 1.0f - u * u;
    const int top = base - (int)(height * (shape > 0 ? sqrtf(shape) * 0.95f + 0.05f * shape : 0));
    for (int y = top; y <= base; y++) put(d, left + x, y, y == top || x == 0 || x == width - 1 ? rgb(0, 80, 0) : rgb(24, 172, 32));
  }
  // Two eyes, as hills have
  box(d, left + 12, base - 9, 1, 3, rgb(0, 40, 0));
  box(d, left + 17, base - 9, 1, 3, rgb(0, 40, 0));
  box(d, left + 7, base - 4, 2, 1, rgb(0, 112, 0));
  box(d, left + 21, base - 4, 2, 1, rgb(0, 112, 0));
}

void drawPipe(MatrixPanel_I2S_DMA &d, int x, int y) {
  const uint16_t body = rgb(0, 168, 0), light = rgb(128, 224, 40), dark = rgb(0, 72, 0), line = rgb(0, 32, 0);
  // The lip, 14 wide, then the body, 12 wide
  box(d, x, y, 14, 4, body); box(d, x + 1, y, 2, 4, light); box(d, x + 11, y, 2, 4, dark);
  box(d, x, y, 14, 1, line); box(d, x, y + 3, 14, 1, line); box(d, x, y, 1, 4, line); box(d, x + 13, y, 1, 4, line);
  box(d, x + 1, y + 4, 12, 8, body); box(d, x + 2, y + 4, 2, 8, light); box(d, x + 10, y + 4, 2, 8, dark);
  box(d, x + 1, y + 4, 1, 8, line); box(d, x + 12, y + 4, 1, 8, line);
}

void drawPlant(MatrixPanel_I2S_DMA &d, int cx, int headTop, bool open) {
  static const char *const CLOSED[8] = {"..rrrrr..", ".rrwrwrr.", "rrwwrwwrr", "rrrrrrrrr", "rrrrrrrrr", ".rrrrrrr.", "..wwwww..", "........."};
  static const char *const OPEN[8] = {"..rrrrr..", ".rrwrwrr.", "rrwwrwwrr", "rrrrrrrrr", "wkkkkkkkw", "wkkkkkkkw", "rrrrrrrrr", ".rrrrrrr."};
  static const Swatch PAL[] = {{'r', rgb(232, 40, 24)}, {'w', rgb(255, 255, 255)}, {'k', rgb(40, 0, 16)}};
  box(d, cx - 1, headTop + 7, 2, 44 - headTop - 7 + 4, rgb(0, 150, 0));  // the stalk, down into the pipe
  sprite(d, open ? OPEN : CLOSED, 8, cx - 4, headTop, PAL, 3);
}

// The plant rises, bites for a while and sinks again every twelve seconds
void drawPlantAndPipe(MatrixPanel_I2S_DMA &d, uint32_t now) {
  const int t = now % 12000;
  int rise = 0;
  if (t >= 3000 && t < 4200) rise = 13 * ease(t - 3000, 1200) / 1000;
  else if (t >= 4200 && t < 7500) rise = 13;
  else if (t >= 7500 && t < 8700) rise = 13 - 13 * ease(t - 7500, 1200) / 1000;
  if (rise > 0) drawPlant(d, 7, 42 - rise, (now / 380) % 2 == 0);
  drawPipe(d, 0, 40);
}

void drawGoomba(MatrixPanel_I2S_DMA &d, int x, int y, bool squashed, bool step, const Theme &t) {
  static const char *const WALK[12] = {"....bbbb....", "..bbbbbbbb..", ".bbbbbbbbbb.", "bbwwbbbbwwbb", "bwwwwbbwwwwb", "bwwkwbbwkwwb",
                                       "bbwwwbbwwwbb", "bbbbbbbbbbbb", "..bbssssbb..", "...ssssss...", "dddd....dddd", ".dddd..dddd."};
  static const char *const SQUASH[5] = {"..bbbbbbbb..", ".bbwwbbwwbb.", "bbbkkbbkkbbb", "..ssssssss..", "dddddddddddd"};
  const Swatch pal[] = {{'b', t.goomba}, {'w', rgb(255, 255, 255)}, {'k', rgb(16, 8, 8)}, {'s', t.goombaFoot}, {'d', rgb(80, 40, 8)}};
  if (squashed) { sprite(d, SQUASH, 5, x, y + 7, pal, 5); return; }
  static const char *rows[12];
  for (int i = 0; i < 12; i++) rows[i] = WALK[i];
  if (step) { rows[10] = ".dddd..dddd."; rows[11] = "dddd....dddd"; }
  sprite(d, rows, 12, x, y, pal, 5);
}

void drawCoin(MatrixPanel_I2S_DMA &d, int cx, int y, int width) {
  const uint16_t gold = rgb(255, 200, 32), light = rgb(255, 250, 170), edge = rgb(200, 112, 0);
  const int x = cx - width / 2;
  box(d, x, y, width, 6, gold);
  if (width >= 3) { box(d, x, y, 1, 6, light); box(d, x + width - 1, y, 1, 6, edge); }
  if (width == 4) { box(d, x + 1, y + 1, 1, 4, light); }
  box(d, x, y, width, 1, edge); box(d, x, y + 5, width, 1, edge);
}

// width of a spinning coin over time: 4, 3, 2, 1, 2, 3
int coinWidth(uint32_t phase) {
  static const uint8_t W_[6] = {4, 3, 2, 1, 2, 3};
  return W_[(phase / 170) % 6];
}

void drawDigit(MatrixPanel_I2S_DMA &d, int digit, int x, int y, uint16_t color) {
  // Seven segments with two-pixel strokes, 7 wide and 11 tall
  static const char *const GLYPHS[10][11] = {
    {"#######", "#######", "##...##", "##...##", "##...##", "##...##", "##...##", "##...##", "##...##", "#######", "#######"},
    {"....###", "....###", ".....##", ".....##", ".....##", ".....##", ".....##", ".....##", ".....##", ".....##", ".....##"},
    {"#######", "#######", ".....##", ".....##", "#######", "#######", "##.....", "##.....", "##.....", "#######", "#######"},
    {"#######", "#######", ".....##", ".....##", ".######", ".######", ".....##", ".....##", ".....##", "#######", "#######"},
    {"##...##", "##...##", "##...##", "##...##", "#######", "#######", ".....##", ".....##", ".....##", ".....##", ".....##"},
    {"#######", "#######", "##.....", "##.....", "#######", "#######", ".....##", ".....##", ".....##", "#######", "#######"},
    {"#######", "#######", "##.....", "##.....", "#######", "#######", "##...##", "##...##", "##...##", "#######", "#######"},
    {"#######", "#######", ".....##", ".....##", ".....##", "....##.", "....##.", "...##..", "...##..", "...##..", "...##.."},
    {"#######", "#######", "##...##", "##...##", "#######", "#######", "##...##", "##...##", "##...##", "#######", "#######"},
    {"#######", "#######", "##...##", "##...##", "#######", "#######", ".....##", ".....##", ".....##", "#######", "#######"}};
  for (int row = 0; row < 11; row++)
    for (int col = 0; col < 7; col++)
      if (GLYPHS[digit % 10][row][col] == '#') put(d, x + col, y + row, color);
}

void drawBlock(MatrixPanel_I2S_DMA &d, int x, int y, int digit, bool cave) {
  const uint16_t outline = rgb(96, 36, 0), body = cave ? rgb(210, 140, 30) : rgb(248, 172, 40), light = rgb(255, 224, 120), dark = rgb(184, 96, 0);
  box(d, x, y, kBlockW, kBlockH, outline);
  box(d, x + 1, y + 1, kBlockW - 2, kBlockH - 2, body);
  box(d, x + 1, y + 1, kBlockW - 2, 1, light);
  box(d, x + 1, y + 1, 1, kBlockH - 2, light);
  box(d, x + 1, y + kBlockH - 2, kBlockW - 2, 1, dark);
  box(d, x + kBlockW - 2, y + 1, 1, kBlockH - 2, dark);
  put(d, x + 2, y + 2, outline); put(d, x + kBlockW - 3, y + 2, outline);
  put(d, x + 2, y + kBlockH - 3, outline); put(d, x + kBlockW - 3, y + kBlockH - 3, outline);
  if (digit < 0) return;
  drawDigit(d, digit, x + 4, y + 4, rgb(150, 70, 0));  // the shadow of the digit
  drawDigit(d, digit, x + 3, y + 3, rgb(255, 250, 220));
}

// How far a block is lifted: it jumps up three pixels and settles again
int bumpOffset(const Block &b, uint32_t now) {
  const int32_t t = (int32_t)(now - b.bumpAt);
  if (t < 0 || t > 380) return 0;
  return -(int)(3.2f * sinf(3.14159f * t / 380.0f) + 0.5f);
}

// ---- The stroller ----

bool free0(const Plumber &m) { return m.mode == Mode::Walk || m.mode == Mode::Pause; }

float jumpHeight(uint32_t t, uint32_t total, int height) {
  const float p = (float)t / total;
  return p >= 1 ? 0 : height * 4 * p * (1 - p);
}

void step(ClockTime *clock, uint32_t now, float dt) {
  const int32_t ms = msOfMinute(clock);
  const int32_t minute = minuteOfDay(clock);
  Plumber &m = mario;

  // A Goomba comes by every third minute, in the second half of it
  if (goomba.state == 0 && minute % 3 == 1 && ms >= 16000 && ms < 36000 && goomba.spawnedMinute != minute && mario.x < 30 && free0(mario)) {
    goomba = {66, 1, now, minute, 0};
  }
  if (goomba.state == 1) {
    goomba.x -= 8 * dt;
    if (goomba.x < -14) goomba.state = 0;
  } else if (goomba.state == 2 && now - goomba.since > 900) {
    goomba.state = 0;
  }

  const bool free = m.mode == Mode::Walk || m.mode == Mode::Pause;
  if (free && goomba.state == 1 && goomba.x > m.x + 13 && goomba.x < 62) {
    m.dir = 1;
    const float gap = goomba.x - (m.x + 13);
    if (gap <= 14) {
      // Land on it: it walks on while Mario is in the air
      const float seconds = 0.56f;
      const float landing = goomba.x - 8 * seconds;
      m.fromX = m.x; m.vx = (landing - 2 - m.x) / seconds;
      m.mode = Mode::Stomp; m.since = now;
    }
  }
  if (free && m.mode != Mode::Stomp && ms >= 52000 && m.approachedMinute != minute && goomba.state == 0) {
    m.approachedMinute = minute;
    m.mode = Mode::Approach;
  }

  switch (m.mode) {
    case Mode::Walk:
      m.x += m.dir * 7.0f * dt; m.distance += 7.0f * dt;
      if (goomba.state == 1) {
        // Waiting for it, facing it
        if (m.x >= 30) { m.x = 30; m.mode = Mode::Pause; m.since = now; m.pauseMs = 30000; }
        break;
      }
      if (m.x >= kMaxX) { m.x = kMaxX; m.dir = -1; m.mode = Mode::Pause; m.since = now; m.pauseMs = 1200 + hash(minute + (int)now / 7000) % 1500; }
      else if (m.x <= kMinX) { m.x = kMinX; m.dir = 1; m.mode = Mode::Pause; m.since = now; m.pauseMs = 900 + hash(minute * 3 + (int)now / 9000) % 1800; }
      break;
    case Mode::Pause:
      if (now - m.since >= m.pauseMs) m.mode = Mode::Walk;
      break;
    case Mode::Approach: {
      if (ms < 40000) { m.mode = Mode::Walk; break; }
      const float speed = 9.5f;
      m.dir = kHitX > m.x ? 1 : -1;
      m.x += m.dir * speed * dt; m.distance += speed * dt;
      if ((m.dir > 0 && m.x >= kHitX) || (m.dir < 0 && m.x <= kHitX)) { m.x = kHitX; m.dir = 1; m.mode = Mode::Wait; }
      break;
    }
    case Mode::Wait:
      if (ms < 40000) { m.mode = Mode::Pause; m.dir = -1; m.since = now; m.pauseMs = 300; break; }  // the minute turned before he got there
      // Start the jump so that his head meets the block as the minute turns
      if (ms >= 60000 - kHitJumpMs / 2 && ms < 60000) { m.mode = Mode::Hit; m.since = now; }
      break;
    case Mode::Hit:
      if (now - m.since >= (uint32_t)kHitJumpMs) { m.mode = Mode::Pause; m.dir = -1; m.since = now; m.pauseMs = 500; }
      break;
    case Mode::Stomp: {
      const uint32_t t = now - m.since;
      m.x = m.fromX + m.vx * t / 1000.0f;
      if (t >= 560) {
        goomba.state = 2; goomba.since = now; goomba.x = m.x + 2; goomba.score = 100;
        m.mode = Mode::Hop; m.since = now;
      }
      break;
    }
    case Mode::Hop:
      m.x += 6 * dt;
      if (now - m.since >= 320) { m.mode = Mode::Walk; m.dir = 1; }
      break;
  }
  if (m.x < 0) m.x = 0;
  if (m.x > 51) m.x = 51;
}

}  // namespace

Clockface::Clockface(MatrixPanel_I2S_DMA* display) { _display = display; }

void Clockface::setNight(bool value) { night = value; }

void Clockface::setup(ClockTime *dateTime) {
  _dateTime = dateTime;
  const int digits[4] = {dateTime->getHour() / 10, dateTime->getHour() % 10, dateTime->getMinute() / 10, dateTime->getMinute() % 10};
  for (int i = 0; i < 4; i++) blocks[i] = {(int8_t)digits[i], (int8_t)digits[i], 0};
  mario = Plumber();
  goomba = Goomba();
  lastNow = millis();
}

void Clockface::update() {
  MatrixPanel_I2S_DMA &d = *_display;
  const uint32_t now = millis();
  float dt = (now - lastNow) / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastNow = now;
  const Theme &theme = night ? kCave : kDay;
  const int hour = _dateTime->getHour(), minute = _dateTime->getMinute();
  const int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
  const bool hideTens = !_dateTime->is24hFormat() && hour < 10;

  // The blocks follow the clock: the last one is hit, the others shake in turn
  for (int i = 0; i < 4; i++) {
    if (blocks[i].target != digits[i]) {
      blocks[i].target = digits[i];
      blocks[i].bumpAt = now + (3 - i) * 140;
    }
    if (blocks[i].shown != blocks[i].target && now >= blocks[i].bumpAt + 150) blocks[i].shown = blocks[i].target;
  }
  step(_dateTime, now, dt);

  d.fillScreen(theme.sky);

  // Scenery
  if (theme.overground) {
    drawCloud(d, 62 - (int)((now / 2600) % 80), 2);
    drawCloud(d, 20 - (int)((now / 3900) % 90) + 60, 29);
    drawHill(d, 22, kGround - 1);
  } else {
    drawBricks(d, 0, 5, theme, false);
    box(d, 0, 6, W, 1, theme.mortar);
  }
  drawPlantAndPipe(d, now);
  drawBricks(d, kGround, H - 1, theme, true);

  // The question blocks and, between them, two spinning coins
  for (int i = 0; i < 4; i++) {
    const int lift = bumpOffset(blocks[i], now);
    const int digit = i == 0 && hideTens ? -1 : blocks[i].shown;
    drawBlock(d, kBlockX[i], kBlockTop + lift, digit, !theme.overground);
    const int32_t since = (int32_t)(now - blocks[i].bumpAt);
    if (since >= 0 && since < 760) {
      // The coin that jumps out
      const int rise = 10 * ease(since < 700 ? since : 700, 700) / 1000;
      drawCoin(d, kBlockX[i] + 6, kBlockTop - 4 - rise + (since > 500 ? (since - 500) / 40 : 0), coinWidth(since * 2));
    }
  }
  drawCoin(d, 31, 11, coinWidth(now));
  drawCoin(d, 31, 19, coinWidth(now + 340));

  // The Goomba, and its score once it is flat
  if (goomba.state == 1) drawGoomba(d, (int)goomba.x, kGround - 12, false, (now / 220) % 2, theme);
  else if (goomba.state == 2) {
    drawGoomba(d, (int)goomba.x, kGround - 12, true, false, theme);
    const int rise = (now - goomba.since) / 110;
    miniText(d, "100", (int)goomba.x - 2, kGround - 16 - rise, rgb(255, 255, 255));
  }

  // Mario
  Pose pose = Pose::Stand;
  int lift = 0;
  switch (mario.mode) {
    case Mode::Walk: case Mode::Approach: {
      static const Pose CYCLE[4] = {Pose::Walk1, Pose::Stand, Pose::Walk2, Pose::Stand};
      pose = CYCLE[((int)(mario.distance / 2.6f)) % 4];
      break;
    }
    case Mode::Hit: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, kHitJumpMs, kHitHeight) + 0.5f); break;
    case Mode::Stomp: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 560, 9) + 0.5f); break;
    case Mode::Hop: pose = Pose::Jump; lift = (int)(jumpHeight(now - mario.since, 320, 5) + 0.5f); break;
    default: break;
  }
  drawPlumber(d, (int)(mario.x + 0.5f), kPlumberY - lift, pose, mario.dir < 0, MARIO);

  // The weekday, small, in the corner of the sky

  d.flipDMABuffer();
}
