// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include <NumericFont.h>
#include <math.h>
#include <stdlib.h>
#include <new>
#include <string.h>

// Effects for a 64x64 matrix. Every effect is a function of (x, y, time) written straight into the display's back
// buffer; only Game of Life keeps state (1 KB, allocated while it runs). Indexes and names follow WLED's effect list,
// the slots between them report "RSVD" like WLED does for reserved ones.
namespace wled {
namespace {

struct Rgb { uint8_t r, g, b; };

struct Ctx {
  MatrixPanel_I2S_DMA *d;
  uint32_t t;         // animation time in ms, scaled by the speed slider
  uint32_t real;      // wall time in ms
  uint32_t frame;
  uint8_t sx, ix, c1, c2, c3, pal;  // pal: the palette in use (the effect's own when the state says Default)
  uint16_t scale;     // master brightness 0..256
  Rgb col[3];
};

uint8_t sinTable[256];
bool tablesReady = false;
void initTables() {
  if (tablesReady) return;
  for (int i = 0; i < 256; ++i) sinTable[i] = (uint8_t)(128.0f + 127.0f * sinf(i * 6.2831853f / 256.0f));
  tablesReady = true;
}
inline uint8_t sin8(uint8_t a) { return sinTable[a]; }
inline uint8_t cos8(uint8_t a) { return sinTable[(uint8_t)(a + 64)]; }

inline uint32_t hash32(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
  uint32_t h = a * 374761393u + b * 668265263u + c * 2246822519u + 0x9E3779B9u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

uint8_t noise8(uint32_t x, uint32_t y) {  // value noise, coordinates in 1/256 steps
  const int xi = x >> 8, yi = y >> 8;
  const uint32_t fx = x & 255, fy = y & 255;
  const uint32_t u = fx * fx * (768 - 2 * fx) >> 16, v = fy * fy * (768 - 2 * fy) >> 16;
  const int a = hash32(xi, yi) & 255, b = hash32(xi + 1, yi) & 255, c = hash32(xi, yi + 1) & 255, d = hash32(xi + 1, yi + 1) & 255;
  const int top = a + (((b - a) * (int)u) >> 8), bottom = c + (((d - c) * (int)u) >> 8);
  return (uint8_t)(top + (((bottom - top) * (int)v) >> 8));
}

Rgb hsv(uint8_t h, uint8_t s, uint8_t v) {
  const uint8_t region = h / 43, rem = (h - region * 43) * 6;
  const uint8_t p = (v * (255 - s)) >> 8, q = (v * (255 - ((s * rem) >> 8))) >> 8, t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

inline Rgb mix(Rgb a, Rgb b, uint8_t f) {  // f = 0 -> a, 255 -> b
  return {(uint8_t)(a.r + (((int)b.r - a.r) * f >> 8)), (uint8_t)(a.g + (((int)b.g - a.g) * f >> 8)), (uint8_t)(a.b + (((int)b.b - a.b) * f >> 8))};
}
inline Rgb dim(Rgb a, uint8_t v) { return {(uint8_t)(a.r * v >> 8), (uint8_t)(a.g * v >> 8), (uint8_t)(a.b * v >> 8)}; }
inline Rgb hex(uint32_t v) { return {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }

// ---- Palettes: WLED's order for the first ones; the colour palettes are even steps, blended round the ring ----
struct Palette { const char *name; const uint32_t *stops; uint8_t count; };
const uint32_t kParty[] = {0x5500AB, 0x84007C, 0xB5004B, 0xE5001B, 0xE81700, 0xB84700, 0xAB7700, 0xABAB00, 0xAB5500, 0xDD2200, 0xF2000E, 0xC2003E, 0x8F0071, 0x5F00A1, 0x2F00D0, 0x0007F9};
const uint32_t kCloud[] = {0x0000FF, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x0000FF, 0x00008B, 0x87CEEB, 0x87CEEB, 0xADD8E6, 0xFFFFFF, 0xADD8E6, 0x87CEEB};
const uint32_t kLava[] = {0x000000, 0x800000, 0x000000, 0x800000, 0x8B0000, 0x800000, 0x8B0000, 0x8B0000, 0x8B0000, 0xFF0000, 0xFFA500, 0xFFFFFF, 0xFFA500, 0xFF0000, 0x8B0000, 0x000000};
const uint32_t kOcean[] = {0x191970, 0x00008B, 0x191970, 0x000080, 0x00008B, 0x0000CD, 0x2E8B57, 0x008080, 0x5F9EA0, 0x0000FF, 0x008B8B, 0x6495ED, 0x7FFFD4, 0x2E8B57, 0x00FFFF, 0x87CEFA};
const uint32_t kForest[] = {0x006400, 0x006400, 0x556B2F, 0x006400, 0x008000, 0x228B22, 0x6B8E23, 0x008000, 0x2E8B57, 0x66CDAA, 0x32CD32, 0x9ACD32, 0x90EE90, 0x7CFC00, 0x66CDAA, 0x228B22};
const uint32_t kSunset[] = {0x780000, 0xB31600, 0xFF6800, 0xA71612, 0x640067, 0x100082, 0x0000A0};
const uint32_t kRivendell[] = {0x010E05, 0x0F3820, 0x3A5C24, 0x8FA364, 0xE0EAB9};
const uint32_t kBreeze[] = {0x000F21, 0x0A2D59, 0x1B6F8A, 0x57C1C9, 0xE6FFFF};
const uint32_t kRedBlue[] = {0xFF0000, 0xFF0000, 0x000000, 0x000000, 0x0000FF, 0x0000FF, 0x000000, 0x000000};
const uint32_t kYellowout[] = {0xFFDD00, 0xAA6600, 0x552200, 0x000000};
const uint32_t kAnalogous[] = {0x3F00FF, 0x7F00FF, 0xBF00FF, 0xFF00BF, 0xFF007F, 0xFF003F, 0xFF0000};
const uint32_t kHeat[] = {0x000000, 0x200000, 0x500000, 0x800000, 0xB00000, 0xE01000, 0xFF3000, 0xFF6000, 0xFF9000, 0xFFC000, 0xFFE020, 0xFFF060, 0xFFFFA0, 0xFFFFD0, 0xFFFFFF, 0xFFFFFF};
#define PAL(list) {#list, list, (uint8_t)(sizeof(list) / sizeof(list[0]))}
// Index = WLED palette id; 0..5, 11 and 12 are computed
const Palette kPalettes[] = {
  {"Default", nullptr, 0}, {"* Random Cycle", nullptr, 0}, {"* Color 1", nullptr, 0}, {"* Colors 1&2", nullptr, 0}, {"* Color Gradient", nullptr, 0},
  {"* Colors Only", nullptr, 0}, {"Party", kParty, 16}, {"Cloud", kCloud, 16}, {"Lava", kLava, 16}, {"Ocean", kOcean, 16}, {"Forest", kForest, 16},
  {"Rainbow", nullptr, 0}, {"Rainbow Bands", nullptr, 0}, {"Sunset", kSunset, 7}, {"Rivendell", kRivendell, 5}, {"Breeze", kBreeze, 5},
  {"Red & Blue", kRedBlue, 8}, {"Yellowout", kYellowout, 4}, {"Analogous", kAnalogous, 7}};
constexpr int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);
constexpr uint8_t kHeatPalette = 100, kMatrixPalette = 101;  // internal defaults, not in the list

Rgb palette(const Ctx &c, uint8_t idx) {
  switch (c.pal) {
    case 1: return hsv((uint8_t)((idx >> 1) + (c.t >> 6)), 240, 255);
    case 2: return c.col[0];
    case 3: return idx < 128 ? c.col[0] : c.col[1];
    case 4: return idx < 85 ? mix(c.col[0], c.col[1], idx * 3) : idx < 170 ? mix(c.col[1], c.col[2], (idx - 85) * 3) : mix(c.col[2], c.col[0], (idx - 170) * 3);
    case 5: return idx < 85 ? c.col[0] : idx < 170 ? c.col[1] : c.col[2];
    case 11: return hsv(idx, 255, 255);
    case 12: return (idx >> 4) & 1 ? Rgb{0, 0, 0} : hsv(idx, 255, 255);
    case kMatrixPalette: return idx > 235 ? Rgb{200, 255, 200} : Rgb{0, idx, (uint8_t)(idx >> 4)};
    default: break;
  }
  const Palette *p = c.pal == kHeatPalette ? nullptr : (c.pal < kPaletteCount ? &kPalettes[c.pal] : nullptr);
  const uint32_t *stops = p ? p->stops : kHeat;
  const uint8_t n = p ? p->count : 16;
  if (!stops) return c.col[0];
  const bool ring = c.pal != kHeatPalette;  // the heat ramp ends in white, it must not blend back to black
  const uint32_t position = (uint32_t)idx * (ring ? n : n - 1);
  const uint8_t i = position >> 8, f = position & 255;
  return mix(hex(stops[i]), hex(stops[ring ? (i + 1) % n : i + 1]), f);
}

// ---- Output ----
inline void put(const Ctx &c, int x, int y, Rgb v) {
  c.d->drawPixelRGB888(x, y, (v.r * c.scale) >> 8, (v.g * c.scale) >> 8, (v.b * c.scale) >> 8);
}
inline void fill(const Ctx &c, Rgb v) { c.d->fillScreenRGB888((v.r * c.scale) >> 8, (v.g * c.scale) >> 8, (v.b * c.scale) >> 8); }
inline int pingpong(uint32_t t, int span) { int p = (int)(t % (uint32_t)(2 * span)); return p < span ? p : 2 * span - p; }  // 0..span

// ---- Effects ----
void fxSolid(Ctx &c) { fill(c, c.col[0]); }

void fxBlink(Ctx &c) {
  const uint32_t phase = c.t % 1000;
  fill(c, phase < (uint32_t)(60 + c.ix * 3) ? palette(c, 0) : c.col[1]);
}

void fxBreathe(Ctx &c) { fill(c, mix(c.col[1], palette(c, 0), sin8((uint8_t)(c.t / 8)))); }

void fxColorloop(Ctx &c) { fill(c, palette(c, (uint8_t)(c.t / 24))); }

void fxRainbow(Ctx &c) {
  const int stretch = 2 + c.ix / 32;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) put(c, x, y, palette(c, (uint8_t)((x + y) * stretch + (c.t >> 4))));
}

void fxTwinkle(Ctx &c) {
  const uint32_t life = 1400, threshold = 6 + c.ix / 5;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const uint32_t base = hash32(x, y), local = c.t + (base >> 8 & 1023), cycle = local / life, phase = local % life;
      const uint32_t h = hash32(x, y, cycle);
      Rgb v = c.col[1];
      if ((h & 255) < threshold) {
        const uint8_t level = phase < life / 2 ? phase * 510 / life : (life - phase) * 510 / life;
        v = mix(c.col[1], palette(c, (uint8_t)(h >> 8)), level);
      }
      put(c, x, y, v);
    }
}

void fxSparkle(Ctx &c) {
  fill(c, c.col[1]);
  const int count = 2 + c.ix / 6;
  for (int i = 0; i < count; ++i) {
    const uint32_t h = hash32(i, c.frame, 7);
    put(c, h & 63, (h >> 6) & 63, palette(c, (uint8_t)(h >> 12)));
  }
}

void fxFireworks(Ctx &c) {
  constexpr int kBursts = 3, kParticles = 32;
  constexpr uint32_t kPeriod = 2800, kRise = 900, kBurst = 1900;
  fill(c, c.col[1]);
  for (int b = 0; b < kBursts; ++b) {
    const uint32_t tb = c.t + b * (kPeriod / kBursts), cycle = tb / kPeriod, phase = tb % kPeriod;
    const uint32_t seed = hash32(cycle, b + 1);
    const int cx = 10 + seed % 44, cy = 8 + (seed >> 8) % 26;
    const uint8_t hue = seed >> 16;
    if (phase < kRise) {
      for (int k = 0; k < 4; ++k) {  // the rocket and its fading tail
        const int past = phase > (uint32_t)k * 40 ? phase - k * 40 : 0;
        put(c, cx, 63 - (63 - cy) * (int)past / (int)kRise, dim({255, 230, 180}, 255 - k * 60));
      }
      continue;
    }
    const uint32_t age = phase - kRise;
    for (int p = 0; p < kParticles; ++p) {
      const uint32_t hp = hash32(p, seed);
      const uint8_t angle = p * 256 / kParticles + (seed & 255);
      const int speed = 12 + (hp & 7) * 2;
      for (int k = 0; k < 3; ++k) {  // the particle and two older positions as a tail
        const uint32_t a = age > (uint32_t)k * 70 ? age - k * 70 : 0;
        const int x = cx + ((int)cos8(angle) - 128) * speed * (int)a / 128000;
        const int y = cy + ((int)sin8(angle) - 128) * speed * (int)a / 128000 + (int)(a * a / 300000);
        if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) continue;
        const int left = (int)(kBurst - age), level = (left > 1000 ? 255 : left * 255 / 1000) >> k;
        put(c, x, y, dim(c.pal == 2 ? c.col[0] : palette(c, hue + p * 3), level));
      }
    }
  }
}

// Falling streaks per column: Rain (short, sparse) and Matrix (long, dense, green)
void fxStreaks(Ctx &c, int density, int minLength, int spread, bool head) {
  for (int x = 0; x < kWidth; ++x) {
    const uint32_t h = hash32(x, 77);
    const bool active = (int)(h & 255) < density;
    const int length = minLength + (int)((h >> 8) & 255) * spread / 255, speed = 18 + (h >> 16 & 31);  // px/s
    const int span = kHeight + length;
    const int headY = (int)(((uint64_t)c.t * speed / 1000 + (h >> 20)) % span);
    for (int y = 0; y < kHeight; ++y) {
      const int d = headY - y;
      Rgb v = c.col[1];
      if (active && d >= 0 && d < length) {
        const uint8_t level = 255 - d * 255 / length;
        v = (head && d == 0) ? palette(c, 255) : mix(c.col[1], palette(c, level), level);
      }
      put(c, x, y, v);
    }
  }
}
void fxRain(Ctx &c) { fxStreaks(c, 50 + c.ix / 2, 5, 6, true); }
void fxMatrix(Ctx &c) { fxStreaks(c, 140 + c.ix / 3, 8 + c.ix / 20, 22, true); }

void fxFire(Ctx &c) {
  const uint32_t xs = 30 + c.ix / 5, ys = xs / 3;  // flames are tall: the noise changes slowly along y
  for (int y = 0; y < kHeight; ++y) {
    const uint32_t base = (uint32_t)y * 255 / (kHeight - 1), weight = (base * base * 3 >> 10) + 24;
    for (int x = 0; x < kWidth; ++x) {
      const int n1 = noise8(x * xs, y * ys + (c.t >> 1)), n2 = noise8(x * xs * 2 + 5000, y * ys * 2 + (c.t * 3 >> 2));
      int n = (n1 * 3 + n2) / 4;
      n = (n - 70) * 2;
      if (n < 0) n = 0;
      const uint32_t heat = n * weight >> 7;
      put(c, x, y, palette(c, heat > 255 ? 255 : heat));
    }
  }
}

void fxColorwaves(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int v = (sin8((uint8_t)(x * 6 + c.t / 5)) + sin8((uint8_t)(y * 5 - c.t / 7)) + sin8((uint8_t)((x + y) * 3 + c.t / 9))) / 3;
      put(c, x, y, palette(c, (uint8_t)(v + (c.t >> 5))));
    }
}

void fxNoise(Ctx &c) {
  const uint32_t scale = 14 + c.ix / 5;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) put(c, x, y, palette(c, noise8(x * scale + (c.t >> 2), y * scale - (c.t >> 3))));
}

void fxMeteor(Ctx &c) {
  const int density = 70 + c.ix / 2, length = 14;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int lane = (x - y) & 63;
      const uint32_t h = hash32(lane, 5);
      Rgb v = c.col[1];
      if ((int)(h & 255) < density) {
        const int speed = 14 + (h >> 8 & 15), span = kWidth + length;
        const int head = (int)(((uint64_t)c.t * speed / 1000 + (h >> 16)) % span), d = head - x;
        if (d >= 0 && d < length) {
          const uint8_t level = 255 - d * 255 / length;
          v = mix(c.col[1], d == 0 ? Rgb{255, 255, 255} : palette(c, (uint8_t)(lane * 4)), level);
        }
      }
      put(c, x, y, v);
    }
}

void fxPlasma(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int dx = x - 32, dy = y - 32;
      const int v = (sin8((uint8_t)(x * 5 + c.t / 16)) + sin8((uint8_t)(y * 5 + c.t / 20)) + sin8((uint8_t)((x + y) * 3 + c.t / 12)) +
                     sin8((uint8_t)((dx * dx + dy * dy) / 12 + c.t / 10))) / 4;
      put(c, x, y, palette(c, (uint8_t)v));
    }
}

void fxRotozoom(Ctx &c) {
  const int ca = (int)cos8((uint8_t)(c.t / 24)) - 128, sa = (int)sin8((uint8_t)(c.t / 24)) - 128, zoom = 5 + (sin8((uint8_t)(c.t / 30)) >> 5);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int u = ((x - 32) * ca - (y - 32) * sa) * zoom / 16, v = ((x - 32) * sa + (y - 32) * ca) * zoom / 16;
      put(c, x, y, palette(c, (uint8_t)((sin8((uint8_t)u) + sin8((uint8_t)v)) / 2 + (c.t >> 6))));
    }
}

void fxBlobs(Ctx &c) {
  int bx[4], by[4];
  for (int k = 0; k < 4; ++k) {
    bx[k] = 32 + ((int)sin8((uint8_t)(c.t / (17 + 4 * k) + k * 60)) - 128) * 22 / 128;
    by[k] = 32 + ((int)cos8((uint8_t)(c.t / (23 + 3 * k) + k * 90)) - 128) * 22 / 128;
  }
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      int field = 0;
      for (int k = 0; k < 4; ++k) {
        const int dx = x - bx[k], dy = y - by[k];
        field += 14000 / (dx * dx + dy * dy + 6);
      }
      put(c, x, y, palette(c, field > 255 ? 255 : field));
    }
}

void fxWaves(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t a = sin8((uint8_t)(y * 6 + c.t / 5)) / 3, b = sin8((uint8_t)(x * 5 - c.t / 6)) / 3;
      put(c, x, y, palette(c, sin8((uint8_t)(x * 4 + a)) / 2 + sin8((uint8_t)(y * 4 + b)) / 2 + (c.t >> 6)));
    }
}

void fxHiphotic(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      put(c, x, y, palette(c, sin8((uint8_t)(cos8((uint8_t)(x * 5 + c.t / 6)) + sin8((uint8_t)(y * 5 + c.t / 8)) + c.t / 3))));
}

void fxSwirl(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const float dx = x - 31.5f, dy = y - 31.5f;
      const float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx) * 40.74f;
      const uint8_t idx = (uint8_t)((int)a + (int)(r * 5) - (int)(c.t >> 3));
      put(c, x, y, dim(palette(c, idx), r < 6 ? 120 + r * 22 : 255));
    }
}

void fxDna(Ctx &c) {
  fill(c, c.col[1]);
  for (int y = 0; y < kHeight; ++y) {
    const uint8_t phase = (uint8_t)(y * 6 + c.t / 8);
    const int x1 = 32 + ((int)sin8(phase) - 128) * 22 / 128, x2 = 32 + ((int)sin8(phase + 128) - 128) * 22 / 128;
    if (y % 5 == 0)
      for (int x = x1 < x2 ? x1 : x2; x <= (x1 < x2 ? x2 : x1); ++x) put(c, x, y, dim(c.col[0], 90));
    put(c, x1, y, palette(c, (uint8_t)(y * 3)));
    put(c, x1 + 1, y, palette(c, (uint8_t)(y * 3)));
    put(c, x2, y, palette(c, (uint8_t)(y * 3 + 120)));
    put(c, x2 + 1, y, palette(c, (uint8_t)(y * 3 + 120)));
  }
}

void fxPolarLights(Ctx &c) {
  for (int x = 0; x < kWidth; ++x) {
    const uint8_t curtain = noise8(x * 18 + (c.t >> 3), c.t >> 4);
    for (int y = 0; y < kHeight; ++y) {
      const uint8_t shimmer = noise8(x * 40 + (c.t >> 2), y * 22 + (c.t >> 1));
      const int level = (curtain * 3 / 2 - 90) * (y > 40 ? 255 - (y - 40) * 8 : 255) / 255;
      const uint8_t v = level < 0 ? 0 : level > 255 ? 255 : level;
      put(c, x, y, dim(palette(c, (uint8_t)(shimmer / 2 + 80 + (c.t >> 6))), v));
    }
  }
}

void fxPride(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t bri = 90 + sin8((uint8_t)(x * 4 + y * 3 + c.t / 8)) * 165 / 255;
      put(c, x, y, dim(palette(c, (uint8_t)((c.t >> 5) + x * 3 + (sin8((uint8_t)(y * 6 + c.t / 10)) >> 2))), bri));
    }
}

void fxPacifica(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int v = (sin8((uint8_t)(x * 3 + c.t / 9)) + sin8((uint8_t)(y * 4 - c.t / 11)) + sin8((uint8_t)((x + y) * 2 + c.t / 7))) / 3;
      Rgb p = palette(c, (uint8_t)(v / 2 + 20));
      if (v > 215) p = mix(p, {255, 255, 255}, (v - 215) * 6);
      put(c, x, y, p);
    }
}

// Conway's Game of Life on a torus: a row is one 64-bit word
struct Life {
  uint64_t rows[kHeight];
  uint32_t stepAt, history[8], stuck;
  uint8_t slot;
};
Life *life = nullptr;

void lifeSeed(Life &l, uint8_t density, uint32_t seed) {
  for (int y = 0; y < kHeight; ++y) {
    l.rows[y] = 0;
    for (int x = 0; x < kWidth; ++x) if ((hash32(x, y, seed) & 255) < density) l.rows[y] |= 1ULL << x;
  }
  memset(l.history, 0, sizeof(l.history));
  l.stuck = 0;
}

void fxLife(Ctx &c) {
  if (!life) {
    life = new (std::nothrow) Life();
    if (!life) { fill(c, c.col[1]); return; }
    life->stepAt = c.real;
    lifeSeed(*life, 70 + c.ix / 4, c.real);
  }
  Life &l = *life;
  const uint32_t interval = 60 + (255 - c.sx) * 2;
  if (c.real - l.stepAt >= interval) {
    l.stepAt = c.real;
    uint64_t next[kHeight];
    uint32_t sum = 0;
    int population = 0;
    for (int y = 0; y < kHeight; ++y) {
      const uint64_t above = l.rows[(y + kHeight - 1) % kHeight], here = l.rows[y], below = l.rows[(y + 1) % kHeight];
      uint64_t out = 0;
      for (int x = 0; x < kWidth; ++x) {
        const int xl = (x + kWidth - 1) % kWidth, xr = (x + 1) % kWidth;
        const int n = ((above >> xl) & 1) + ((above >> x) & 1) + ((above >> xr) & 1) + ((here >> xl) & 1) + ((here >> xr) & 1) +
                      ((below >> xl) & 1) + ((below >> x) & 1) + ((below >> xr) & 1);
        if (n == 3 || (n == 2 && ((here >> x) & 1))) out |= 1ULL << x;
      }
      next[y] = out;
      sum = sum * 31 + (uint32_t)out + (uint32_t)(out >> 32) * 7;
      population += __builtin_popcountll(out);
    }
    memcpy(l.rows, next, sizeof(next));
    bool repeat = population == 0;
    for (uint32_t h : l.history) if (h == sum) repeat = true;
    l.history[l.slot++ & 7] = sum;
    l.stuck = repeat ? l.stuck + 1 : 0;
    if (l.stuck > 8) lifeSeed(l, 70 + c.ix / 4, c.real);  // a dead or looping world starts over
  }
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      put(c, x, y, (l.rows[y] >> x) & 1 ? palette(c, (uint8_t)(hash32(x, y) + (c.t >> 5))) : c.col[1]);
}

// ---- Text, clocks and images ----
inline uint16_t color565(Rgb v, uint16_t scale) { return ((v.r * scale >> 8) & 0xF8) << 8 | ((v.g * scale >> 8) & 0xFC) << 3 | (v.b * scale >> 8) >> 3; }

// The segment name is the text (WLED uses it the same way): speed scrolls, intensity sets the height, custom 3 the size
void fxText(Ctx &c) {
  fill(c, c.col[1]);
  const State &s = state();
  const char *text = s.name[0] ? s.name : "ULTIMATRIX";
  const int size = 1 + (c.c3 >= 192 ? 3 : c.c3 / 64), cell = 6 * size, length = (int)strlen(text), span = length * cell + kWidth;
  const int x = kWidth - (int)((uint64_t)c.t * 32 / 1000 % span), y = c.ix * (kHeight - 7 * size) / 255;
  c.d->setTextSize(size);
  c.d->setTextWrap(false);
  for (int i = 0; i < length; ++i) {
    const int cx = x + i * cell;
    if (cx <= -cell || cx >= kWidth) continue;
    c.d->setTextColor(color565(c.pal == 2 ? c.col[0] : palette(c, (uint8_t)(i * 24 + (c.t >> 5))), c.scale));
    c.d->setCursor(cx, y);
    c.d->write((uint8_t)text[i]);
  }
}

void fxAnalog(Ctx &c) {
  fill(c, c.col[1]);
  const ClockNow n = clockNow();
  const float cx = 31.5f, cy = 31.5f;
  auto hand = [&](float turns, float length, Rgb v) {
    const float a = turns * 6.2831853f;
    c.d->drawLine((int)(cx + 0.5f), (int)(cy + 0.5f), (int)(cx + sinf(a) * length + 0.5f), (int)(cy - cosf(a) * length + 0.5f), color565(v, c.scale));
  };
  for (int k = 0; k < 12; ++k) {
    const float a = k * 0.5235988f, r2 = k % 3 == 0 ? 22.0f : 26.0f;
    c.d->drawLine((int)(cx + sinf(a) * 29 + 0.5f), (int)(cy - cosf(a) * 29 + 0.5f), (int)(cx + sinf(a) * r2 + 0.5f), (int)(cy - cosf(a) * r2 + 0.5f), color565(dim(palette(c, (uint8_t)(k * 21)), 150), c.scale));
  }
  hand((n.hour % 12 + n.minute / 60.0f) / 12.0f, 15, palette(c, 0));
  hand((n.minute + n.second / 60.0f) / 60.0f, 24, palette(c, 0));
  hand(n.second / 60.0f, 27, c.pal == 2 ? Rgb{255, 40, 40} : palette(c, 128));
}

void fxDigital(Ctx &c) {
  fill(c, c.col[1]);
  const ClockNow n = clockNow();
  const int w = 11, h = 24, gap = 2, colon = 8, stroke = (w + 2) / 4, total = 4 * w + 2 * gap + colon;
  const int left = (kWidth - total) / 2, top = (kHeight - h) / 2;
  const Rgb v = palette(c, 0);
  auto digit = [&](int value, int x0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) if (numericfont::pixel(value, x, y, w, h)) put(c, x0 + x, top + y, v);
  };
  if (n.h24 || n.hour > 9) digit(n.hour / 10, left);
  digit(n.hour % 10, left + w + gap);
  {
    const int colonX = left + 2 * w + gap + (colon - stroke) / 2;
    for (int dy = 0; dy < stroke; ++dy)
      for (int dx = 0; dx < stroke; ++dx) { put(c, colonX + dx, top + h / 3 - stroke / 2 + dy, v); put(c, colonX + dx, top + 2 * h / 3 - stroke / 2 + dy, v); }
  }
  const int minutesX = left + 2 * w + gap + colon;
  digit(n.minute / 10, minutesX);
  digit(n.minute % 10, minutesX + w + gap);
}

// The drawing buffer (editor, "i" array, saved drawings); RGB565 as stored
void fxPaint(Ctx &c) {
  canvas::tick(c.real);
  const uint8_t *indexes = nullptr, *palette = nullptr;
  if (canvas::indexedFrame(indexes, palette)) {
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) {
        const uint8_t *rgb = palette + indexes[y * kWidth + x] * 3;
        put(c, x, y, {rgb[0], rgb[1], rgb[2]});
      }
    return;
  }
  const uint16_t *b = canvas::buffer(true);
  if (!b) { fill(c, {0, 0, 0}); return; }
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const uint16_t v = b[y * kWidth + x];
      const uint8_t r = (v >> 8) & 0xF8, g = (v >> 3) & 0xFC, bl = (v << 3) & 0xF8;
      put(c, x, y, {(uint8_t)(r | r >> 5), (uint8_t)(g | g >> 6), (uint8_t)(bl | bl >> 5)});
    }
}

// Wake-up: dark red to warm white over 1 to 17 minutes (speed), then it holds
uint32_t effectStart = 0;
void fxSunrise(Ctx &c) {
  const uint32_t duration = (1 + c.sx / 16) * 60000UL, elapsed = c.real - effectStart;
  uint8_t ramp[3];
  sunriseRamp(elapsed >= duration ? 255 : (uint8_t)((uint64_t)elapsed * 255 / duration), ramp);
  fill(c, {ramp[0], ramp[1], ramp[2]});
}

// ---- More 2D effects ----
void fxWavingCell(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int v = (sin8((uint8_t)(x * 8 + sin8((uint8_t)(y * 6 + c.t / 7)) / 2)) + sin8((uint8_t)(y * 8 + sin8((uint8_t)(x * 6 - c.t / 9)) / 2))) / 2;
      put(c, x, y, palette(c, (uint8_t)(v + c.t / 50)));
    }
}

void fxSquaredSwirl(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int dx = abs(x * 2 - 63), dy = abs(y * 2 - 63), r = (dx > dy ? dx : dy) / 2;
      put(c, x, y, palette(c, (uint8_t)(r * 6 - c.t / 8 + sin8((uint8_t)(c.t / 12 + r * 4)) / 3)));
    }
}

void fxSindots(Ctx &c) {
  fill(c, c.col[1]);
  for (int k = 0; k < 5; ++k)
    for (int m = 0; m < 10; ++m) {
      const uint32_t tt = c.t > (uint32_t)m * 80 ? c.t - m * 80 : 0;
      const int x = 32 + ((int)sin8((uint8_t)(tt / (9 + 3 * k) + k * 40)) - 128) * 28 / 128, y = 32 + ((int)sin8((uint8_t)(tt / (11 + 2 * k) + k * 70 + 64)) - 128) * 28 / 128;
      put(c, x, y, dim(palette(c, (uint8_t)(k * 50 + m * 4)), 255 - m * 24));
    }
}

void fxBursts(Ctx &c) {
  constexpr uint32_t kPeriod = 2400;
  fill(c, c.col[1]);
  for (int b = 0; b < 3; ++b) {
    const uint32_t tb = c.t + b * (kPeriod / 3), cycle = tb / kPeriod, age = tb % kPeriod, seed = hash32(cycle, b + 11);
    const int cx = 8 + seed % 48, cy = 8 + (seed >> 8) % 48, r = (int)(age * 36 / 1000), r2 = r * r, band = r * 2 + 3;
    const uint8_t fade = 255 - age * 255 / kPeriod, hue = seed >> 16;
    for (int y = (cy - r - 3 < 0 ? 0 : cy - r - 3); y < kHeight && y <= cy + r + 3; ++y)
      for (int x = (cx - r - 3 < 0 ? 0 : cx - r - 3); x < kWidth && x <= cx + r + 3; ++x) {
        const int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy), diff = abs(d2 - r2);
        if (diff < band) put(c, x, y, dim(palette(c, hue + (uint8_t)(diff * 4)), (uint8_t)((band - diff) * fade / band)));
      }
  }
}

void fxLissajous(Ctx &c) {
  fill(c, c.col[1]);
  for (int m = 0; m < 80; ++m) {
    const uint32_t tt = c.t > (uint32_t)m * 20 ? c.t - m * 20 : 0;
    put(c, 3 + sin8((uint8_t)(tt * 3 / 16)) * 58 / 255, 3 + sin8((uint8_t)(tt * 2 / 16 + 64)) * 58 / 255, dim(palette(c, (uint8_t)(m * 3 + c.t / 20)), 255 - m * 3));
  }
}

void fxTartan(Ctx &c) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const int a = sin8((uint8_t)(x * 8 + c.t / 20)), b = sin8((uint8_t)(y * 8 - c.t / 25));
      put(c, x, y, dim(palette(c, (uint8_t)(a / 2 + b / 2 + (c.t >> 6))), 80 + (a * b >> 9)));
    }
}

// ---- The list ----
struct Effect { uint8_t index; const char *name; void (*run)(Ctx &); uint8_t defaultPalette; const char *data; };
const Effect kEffects[] = {
  {0, "Solid", fxSolid, 2, ";!;"},
  {1, "Blink", fxBlink, 2, "!,Duty cycle;!,!;!;1"},
  {2, "Breathe", fxBreathe, 2, "!;!,!;!;01"},
  {8, "Colorloop", fxColorloop, 11, "!,Saturation;;!;01"},
  {9, "Rainbow", fxRainbow, 11, "!,Size;;!;2"},
  {17, "Twinkle", fxTwinkle, 2, "!,Twinkle rate;!,!;!;2"},
  {20, "Sparkle", fxSparkle, 2, "!,Sparkles;!,!;!;2"},
  {42, "Fireworks", fxFireworks, 2, "!,Frequency;!,!;!;2"},
  {43, "Rain", fxRain, 2, "!,Intensity;!,!;!;2"},
  {63, "Pride 2015", fxPride, 11, "!;;;2"},
  {66, "Fire 2012", fxFire, kHeatPalette, "!,Scale;;!;2"},
  {67, "Colorwaves", fxColorwaves, 6, "!;;!;2"},
  {70, "Noise 1", fxNoise, 6, "!,Scale;;!;2"},
  {76, "Meteor", fxMeteor, 2, "!,Trail;!,!;!;2"},
  {97, "Plasma", fxPlasma, 6, "!;;!;2"},
  {101, "Pacifica", fxPacifica, 9, "!;;!;2"},
  {104, "Sunrise", fxSunrise, 2, "Time (min);;;01"},
  {114, "Rotozoom", fxRotozoom, 6, "!;;!;2"},
  {121, "Blobs", fxBlobs, 8, "!;;!;2"},
  {122, "Scrolling Text", fxText, 2, "!,Y Offset,,,Size;!,!;!;2"},
  {124, "Distortion Waves", fxWaves, 6, "!;;!;2"},
  {127, "Waving Cell", fxWavingCell, 6, "!;;!;2"},
  {150, "Squared Swirl", fxSquaredSwirl, 6, "!;;!;2"},
  {152, "DNA", fxDna, 11, "!;!,!;!;2"},
  {153, "Matrix", fxMatrix, kMatrixPalette, "!,Length;!,!;;2"},
  {167, "Colored Bursts", fxBursts, 6, "!;!;!;2"},
  {172, "Game Of Life", fxLife, 2, "!,Fill;!,!;!;2"},
  {173, "Tartan", fxTartan, 6, "!;;!;2"},
  {174, "Polar Lights", fxPolarLights, 10, "!;;!;2"},
  {175, "Swirl", fxSwirl, 6, "!;;!;2"},
  {176, "Lissajous", fxLissajous, 6, "!;!;!;2"},
  {180, "Hiphotic", fxHiphotic, 6, "!;;!;2"},
  {181, "Sindots", fxSindots, 6, "!;!;!;2"},
  {220, "Analog Clock", fxAnalog, 2, ";!,!,!;!;2"},
  {221, "Digital Clock", fxDigital, 2, ";!,!;!;2"},
  {222, "Paint", fxPaint, 2, ";;;2"},
};
constexpr int kEffectCount = sizeof(kEffects) / sizeof(kEffects[0]);

const Effect *find(int index) {
  for (const Effect &e : kEffects) if (e.index == index) return &e;
  return nullptr;
}

uint32_t animation = 0, lastRender = 0, frames = 0;
int lastEffect = -1;
}  // namespace

int effectSlots() { return kEffects[kEffectCount - 1].index + 1; }
bool effectKnown(int index) { return find(index) != nullptr; }
const char *effectName(int index) { const Effect *e = find(index); return e ? e->name : "RSVD"; }
const char *effectData(int index) { const Effect *e = find(index); return e ? e->data : ""; }
int paletteSlots() { return kPaletteCount; }
const char *paletteName(int index) { return index >= 0 && index < kPaletteCount ? kPalettes[index].name : "RSVD"; }

void restartEffect() {
  delete life;
  life = nullptr;
  lastEffect = -1;  // the Paint buffer is freed once another effect has run
}

void render(MatrixPanel_I2S_DMA &display, uint32_t now, uint16_t scale) {
  initTables();
  State &s = state();
  const Effect *effect = find(s.fx);
  if (!effect) effect = &kEffects[0];
  if (effect->index != lastEffect) {
    delete life;
    life = nullptr;
      if (effect->index != kPaintEffect) canvas::release();
    lastEffect = effect->index;
    effectStart = now;
  }
  const uint32_t dt = lastRender ? (now - lastRender > 250 ? 250 : now - lastRender) : 0;
  lastRender = now;
  animation += dt * (24 + s.sx) / 128;
  Ctx c;
  c.d = &display;
  c.t = animation;
  c.real = now;
  c.frame = frames++;
  c.sx = s.sx; c.ix = s.ix; c.c1 = s.c1; c.c2 = s.c2; c.c3 = s.c3;
  c.pal = s.pal && s.pal < kPaletteCount ? s.pal : effect->defaultPalette;
  c.scale = scale;
  for (int i = 0; i < 3; ++i) { const uint8_t *k = displayColor(i); c.col[i] = {k[0], k[1], k[2]}; }
  effect->run(c);
}

}  // namespace wled
