// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
// Small drawing helpers shared by the pixel-art faces (Mario, Luigi, Pac-Man, Time in Words): colours, clipped pixels,
// sprites drawn from rows of text, and the wall clock as milliseconds. Header only.
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ClockTime.h>

namespace facekit {

constexpr int W = 64, H = 64;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
inline uint8_t red(uint16_t c) { return (c >> 11) * 255 / 31; }
inline uint8_t green(uint16_t c) { return ((c >> 5) & 63) * 255 / 63; }
inline uint8_t blue(uint16_t c) { return (c & 31) * 255 / 31; }

// t = 0 gives a, 255 gives b
inline uint16_t mix(uint16_t a, uint16_t b, int t) {
  if (t <= 0) return a;
  if (t >= 255) return b;
  return rgb((red(a) * (255 - t) + red(b) * t) / 255, (green(a) * (255 - t) + green(b) * t) / 255,
             (blue(a) * (255 - t) + blue(b) * t) / 255);
}
// level 255 is the colour itself, 0 is black
inline uint16_t dim(uint16_t c, int level) { return mix(0, c, level); }

// Everything a face draws through put/box can be shifted sideways and cut to a column range (page slides); the default is the whole panel
struct View { int dx = 0, x0 = 0, x1 = W; };
inline View view;

inline void put(MatrixPanel_I2S_DMA &d, int x, int y, uint16_t c) {
  x += view.dx;
  if (x >= view.x0 && x < view.x1 && (unsigned)y < (unsigned)H) d.drawPixel(x, y, c);
}
inline void box(MatrixPanel_I2S_DMA &d, int x, int y, int w, int h, uint16_t c) {
  x += view.dx;
  if (x < view.x0) { w -= view.x0 - x; x = view.x0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > view.x1) w = view.x1 - x;
  if (y + h > H) h = H - y;
  if (w > 0 && h > 0) d.fillRect(x, y, w, h, c);
}

// A sprite is rows of text: '.' and ' ' are see-through, any other character picks a colour from the palette
struct Swatch { char key; uint16_t color; };

inline void sprite(MatrixPanel_I2S_DMA &d, const char *const *rows, int height, int x, int y, const Swatch *palette,
                   int swatches, bool flip = false) {
  for (int row = 0; row < height; row++) {
    const char *line = rows[row];
    int width = 0;
    while (line[width]) width++;
    for (int col = 0; col < width; col++) {
      const char key = line[flip ? width - 1 - col : col];
      if (key == '.' || key == ' ') continue;
      for (int i = 0; i < swatches; i++)
        if (palette[i].key == key) { put(d, x + col, y + row, palette[i].color); break; }
    }
  }
}

// The same sprite with one swatch's colour replaced by another colour (a recoloured hat, a flash of white)
template <int N> inline void sprite(MatrixPanel_I2S_DMA &d, const char *const (&rows)[N], int x, int y, const Swatch *palette,
                                    int swatches, bool flip = false) {
  sprite(d, rows, N, x, y, palette, swatches, flip);
}

// Position within the current minute, in milliseconds, and within the day
// The seconds and the milliseconds are two reads: if the second turned in between, the seconds are read again
inline int32_t msOfMinute(ClockTime *t) {
  int32_t ms = (int32_t)t->getMilliseconds();
  int seconds = t->getSecond();
  const int32_t after = (int32_t)t->getMilliseconds();
  if (after < ms) { seconds = t->getSecond(); ms = after; }
  return seconds * 1000 + ms;
}
inline int32_t minuteOfDay(ClockTime *t) { return t->getHour24() * 60 + t->getMinute(); }

// A repeatable pseudo-random number from a seed (twinkling stars, which minute a Goomba walks by)
inline uint32_t hash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// 0..255 -> 0..255..0 as a triangle, for pulses
inline int triangle(int phase) { phase &= 511; return phase < 256 ? phase : 511 - phase; }

// Smooth ease for the first half of a move: 0..1000 in, 0..1000 out
inline int ease(int t, int total) {
  if (t <= 0) return 0;
  if (t >= total) return 1000;
  int p = t * 1000 / total;
  return p * p * (3000 - 2 * p) / 1000000;
}

}  // namespace facekit
