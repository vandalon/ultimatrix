// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Binary clock. Six columns (hour tens/ones, minute tens/ones, second tens/ones) of rounded 7 px dots, rows 8-4-2-1; a digit that has fewer
// bits (hour tens 2, minute and second tens 3) keeps its dots at the bottom. A bit that turns on lights up slowly and one that turns off dims slowly. The day (5 bits, violet, left) and the month (4 bits, teal, right) sit on the top row, smallest bit on the left (1, 2, 4, 8, 16).
#include "Clockface.h"
#include <FaceKit.h>
#include <Settings.h>
#include <PacmanColors.h>
#include <math.h>

using namespace facekit;

namespace {

// Two panel sizes (the last three numbers are the rows layout): the 64 x 64 panel (7 px dots, date row on top) and the 32 x 64 panel (4 px dots, date row on top, everything tighter)
struct Layout { int dot, pitchX, pitchY, gap, top, dateDot, datePitch, dateY, monthX, rowPitchX, rowPitchY, rowTop; };
const Layout TALL = {7, 9, 11, 4, 15, 3, 5, 2, 43, 10, 17, 12};
const Layout SHORT = {4, 6, 6, 4, 6, 3, 5, 1, 43, 8, 8, 6};

const uint32_t FADE_MS = 550;   // a bit that changes lights up or dims over FADE_MS
const uint8_t BITS[6] = {2, 4, 3, 4, 3, 4};
const uint8_t UNLIT[6] = {0, 24, 38, 51, 70, 96};        // brightness of unlit dots: setting 0 (off) to 5

const uint16_t kHours = rgb(40, 170, 255), kMinutes = rgb(60, 230, 110), kSeconds = rgb(255, 150, 40);
const uint16_t kDay = rgb(190, 120, 255), kMonth = rgb(60, 200, 210);

// A setting of 0 keeps the face's own colour
uint16_t choose(uint8_t setting, uint16_t own) { return setting ? pacmancolors::sprite(setting) : own; }

// One cell of a column, at lit amount 0 (unlit) to 255 (lit), so a change can fade. shape: 0 dots, 1 rounded tiles, 2 squares, 3 discs, 4 diamonds, 5 crosses.
// Dots and tiles are one colour that fades between the unlit and the lit one. Squares and discs differ in form: the edge (outline, ring) stays
// visible when unlit, the inside goes dark, and both fill in as the cell lights up.
void cell(MatrixPanel_I2S_DMA &d, int x, int y, int size, int shape, uint16_t on, uint16_t offBody, uint16_t offEdge, int amount) {
  // squares keep all their corners, tiles lose one pixel at each, dots and discs are rounder
  const int cut = shape == 2 || shape >= 4 ? 0 : shape == 1 ? (size >= 5 ? 1 : 0) : (size >= 7 ? 2 : (size >= 4 ? 1 : 0));
  const uint16_t body = mix(offBody, on, amount), edge = mix(offEdge, on, amount), inner = mix(0, on, amount);
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) {
      const int corner = min(i, size - 1 - i) + min(j, size - 1 - j);
      if (corner < cut) continue;
      const int dx = abs(2 * i - (size - 1)), dy = abs(2 * j - (size - 1));
      if (shape == 4 && dx + dy > size - 1) continue;                      // diamond
      if (shape == 5 && min(dx, dy) > (size >= 7 ? 2 : 1)) continue;      // cross (plus sign)
      const bool border = i == 0 || j == 0 || i == size - 1 || j == size - 1;
      uint16_t colour = body;
      if (shape == 2) colour = border ? edge : inner;
      else if (shape == 3) colour = (border || corner == cut) ? edge : inner;
      put(d, x + i, y + j, colour);
    }
}

float ease(float u) { return u * u * (3.0f - 2.0f * u); }   // smoothstep: slow start and end

}  // namespace

void Clockface::setup(ClockTime *time) {
  _time = time;
  _display->setRotation(0);
  _ready = false;
}

// Digits of what the clock shows; a column that changed keeps its old digit for the fade
void Clockface::refresh(uint32_t now) {
  const int h = _time->getHour(), m = _time->getMinute(), s = _time->getSecond();
  const uint8_t digits[6] = {(uint8_t)(h / 10), (uint8_t)(h % 10), (uint8_t)(m / 10), (uint8_t)(m % 10), (uint8_t)(s / 10), (uint8_t)(s % 10)};
  for (int i = 0; i < 6; ++i) {
    if (!_ready) { _cur[i] = _prev[i] = digits[i]; _changedAt[i] = now - FADE_MS; continue; }
    if (digits[i] != _cur[i]) { _prev[i] = _cur[i]; _cur[i] = digits[i]; _changedAt[i] = now; }
  }
  _ready = true;
}

void Clockface::update() {
  const uint32_t now = millis();
  const Settings::BinaryOptions &o = Settings::getInstance()->binary;
  const Layout &L = _display->height() >= 64 ? TALL : SHORT;
  refresh(now);
  _display->fillScreen(0);

  const uint16_t groupOwn[3] = {choose(o.hours, kHours), choose(o.minutes, kMinutes), choose(o.seconds, kSeconds)};
  const uint8_t unlit = UNLIT[o.unlit];
  const int shapes[3] = {o.shape, o.shapeMinutes, o.shapeSeconds};
  auto lit = [&](uint16_t c) { return c; };
  auto unlitOf = [&](uint16_t c) { return dim(c, unlit); };
  auto edgeOf = [&](uint16_t c) { const int k = min(255, unlit * 2); return dim(c, k); };   // the outline of an unlit square or ring
  auto drawBit = [&](int x, int y, int group, bool was, bool is, uint32_t changedAt) {
    const uint32_t since = now - changedAt;
    const float e = since >= FADE_MS ? 1.0f : ease(since / (float)FADE_MS);
    const int amount = was == is ? (is ? 255 : 0) : (is ? (int)(e * 255) : 255 - (int)(e * 255));   // lights up or dims slowly
    cell(*_display, x, y, L.dot, shapes[group], lit(groupOwn[group]), unlitOf(groupOwn[group]), edgeOf(groupOwn[group]), amount);
  };

  if (o.rows) {
    // Rows: hours (5 bits), minutes (6) and seconds (6) as the columns turned clockwise: smallest bit (1) left, then 2, 4, 8..., left-aligned; a value's bits fade together
    const int count = o.showSeconds ? 3 : 2;
    const int top = L.rowTop + (3 - count) * L.rowPitchY / 2;
    const int x0 = (_display->width() - (6 * L.rowPitchX - (L.rowPitchX - L.dot))) / 2;
    for (int r = 0; r < count; ++r) {
      const int n = r == 0 ? 5 : 6;
      // a digit's "previous" value is only meaningful while it is fading; a digit that did not just change is the same as before
      auto before = [&](int d) { return now - _changedAt[d] < FADE_MS ? _prev[d] : _cur[d]; };
      const int nowV = _cur[2 * r] * 10 + _cur[2 * r + 1], oldV = before(2 * r) * 10 + before(2 * r + 1);
      const uint32_t at = max(_changedAt[2 * r], _changedAt[2 * r + 1]);
      for (int k = 0; k < n; ++k) {
        drawBit(x0 + k * L.rowPitchX, top + r * L.rowPitchY, r, (oldV >> k) & 1, (nowV >> k) & 1, at);
      }
    }
  } else {
    const int columns = o.showSeconds ? 6 : 4;
    const int totalWidth = columns * L.pitchX + (columns / 2 - 1) * L.gap - (L.pitchX - L.dot);
    int x = (_display->width() - totalWidth) / 2;
    for (int col = 0; col < columns; ++col) {
      const int n = BITS[col];
      for (int row = 4 - n; row < 4; ++row) {
        const int b2 = n - 1 - (row - (4 - n));   // value of this row: 8, 4, 2, 1 (top row is the highest bit of the digit)
        drawBit(x, L.top + row * L.pitchY, col / 2, (_prev[col] >> b2) & 1, (_cur[col] >> b2) & 1, _changedAt[col]);
      }
      x += L.pitchX + ((col & 1) && col < columns - 1 ? L.gap : 0);
    }
  }

  // Day left (5 bits), month right (4 bits); smallest bit first (left), so each group counts 1, 2, 4, 8, 16 from the left
  if (o.showDate) {
    const int day = _time->getDay(), month = _time->getMonth();
    const uint16_t dayOwn = choose(o.day, kDay), monOwn = choose(o.month, kMonth);
    for (int i = 0; i < 5; ++i) cell(*_display, 2 + i * L.datePitch, L.dateY, L.dateDot, 0, (day >> i) & 1 ? lit(dayOwn) : unlitOf(dayOwn), 0, 0, 255);
    for (int i = 0; i < 4; ++i) cell(*_display, L.monthX + i * L.datePitch, L.dateY, L.dateDot, 0, (month >> i) & 1 ? lit(monOwn) : unlitOf(monOwn), 0, 0, 255);
  }

  _display->flipDMABuffer();
}
